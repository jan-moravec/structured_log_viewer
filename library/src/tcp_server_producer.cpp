#include "loglib/tcp_server_producer.hpp"

#include "loglib/internal/line_bytes_queue.hpp"

#include <asio.hpp>
#ifdef LOGLIB_HAS_TLS
#include <asio/ssl.hpp>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace loglib::internal
{

// Bytes-queue and line-aware back-pressure are shared with
// `UdpServerProducer` via `LineBytesQueue`
// (`loglib/internal/line_bytes_queue.hpp`); sessions push complete-line
// payloads through `mImpl->mReadyBuffer.Append(...)`.

class TcpServerProducerImpl;

/// Type-erased session handle so the impl can store both plain
/// (`asio::ip::tcp::socket`) and TLS (`asio::ssl::stream<>`) sessions
/// in a single `mActiveSessions` map and close them uniformly during
/// `Stop()` without needing the templated session type at the call
/// site. The vtable is tiny (one virtual: `Close`) so the indirection
/// cost is negligible vs the network I/O each call wraps.
// NOLINTNEXTLINE(misc-use-internal-linkage): `loglib::internal` TU-local implementation detail for
// `TcpServerProducerImpl`.
class SessionBase
{
public:
    SessionBase() = default;
    virtual ~SessionBase() = default;

    SessionBase(const SessionBase &) = delete;
    SessionBase &operator=(const SessionBase &) = delete;
    SessionBase(SessionBase &&) = delete;
    SessionBase &operator=(SessionBase &&) = delete;

    /// Cancel any in-flight read and close the underlying socket. The
    /// async handler observes `operation_aborted` and finalises the
    /// session via `Finalize`. Idempotent.
    virtual void Close() noexcept = 0;
};

/// Per-connection state, ref-counted via `shared_ptr` so the in-flight
/// Asio async chain keeps the session alive until the read completes
/// and we explicitly drop it.
// NOLINTNEXTLINE(misc-use-internal-linkage): TU-local async session type for `TcpServerProducerImpl`.
template <class Stream> class Session : public SessionBase, public std::enable_shared_from_this<Session<Stream>>
{
public:
    Session(Stream stream, TcpServerProducerImpl *impl, size_t id, size_t readChunkBytes);

    /// Begin processing this session. For TLS sessions this issues
    /// the handshake first; for plain sessions it goes straight to
    /// the read loop.
    void Start();

    void Close() noexcept override;

private:
    void DoRead();
    void OnRead(const asio::error_code &ec, std::size_t bytes);

    /// Flush the session's per-client carry buffer (synthetic newline
    /// appended if absent) and remove this session from the impl.
    /// Idempotent via `mFinalized`.
    void Finalize();

    Stream mStream;
    TcpServerProducerImpl *mImpl;
    size_t mId;

    std::vector<char> mReadBuffer;
    std::string mCarry;
    bool mFinalized = false;
};

class TcpServerProducerImpl
{
public:
    explicit TcpServerProducerImpl(TcpServerProducer::Options options);
    ~TcpServerProducerImpl();

    TcpServerProducerImpl(const TcpServerProducerImpl &) = delete;
    TcpServerProducerImpl &operator=(const TcpServerProducerImpl &) = delete;
    TcpServerProducerImpl(TcpServerProducerImpl &&) = delete;
    TcpServerProducerImpl &operator=(TcpServerProducerImpl &&) = delete;

    size_t Read(std::span<char> buffer);
    void WaitForBytes(std::chrono::milliseconds timeout);
    void Stop() noexcept;
    [[nodiscard]] bool IsClosed() const noexcept;
    [[nodiscard]] std::string DisplayName() const;
    void SetStatusCallback(const std::function<void(SourceStatus)> &callback);
    [[nodiscard]] uint16_t BoundPort() const noexcept;
    [[nodiscard]] size_t ActiveClientCount() const noexcept;
    [[nodiscard]] size_t TotalClientsAccepted() const noexcept;
    [[nodiscard]] size_t TotalClientsRejected() const noexcept;
    [[nodiscard]] size_t DroppedByteCount() const noexcept;

    /// Called from a session's read handler. Pushes the bytes in
    /// @p completeLines (always ending on a `\n` boundary) into the
    /// shared queue under the mutex, applying back-pressure.
    void OnSessionLines(std::string_view completeLines);

    /// Called from a session's `Finalize`. Drops the session from
    /// `mActiveSessions`, decrements the counter, and notifies the cv
    /// so callers waiting in `WaitForBytes` wake up to observe a
    /// possibly-empty queue + closed state.
    void OnSessionEnded(size_t sessionId);

    /// Advance status to `Running` once we've actually received bytes.
    void MarkRunning();

    [[nodiscard]] size_t ReadChunkBytes() const noexcept
    {
        return mOptions.readChunkBytes;
    }

private:
    void StartAccept();

#ifdef LOGLIB_HAS_TLS
    void OnAcceptTls(const asio::error_code &ec, asio::ip::tcp::socket socket);
#endif
    void OnAcceptPlain(const asio::error_code &ec, asio::ip::tcp::socket socket);

    /// Common post-accept dispatch: if the cap is reached, immediately
    /// close the new socket and bump the rejection counter. Otherwise
    /// wrap into a Session and call `Start()`.
    bool AdmitOrReject(asio::ip::tcp::socket &socket);

    TcpServerProducer::Options mOptions;
    std::string mDisplayName;

    asio::io_context mIoContext;
    asio::ip::tcp::acceptor mAcceptor;

#ifdef LOGLIB_HAS_TLS
    /// Built once in the ctor when TLS is configured; nullopt for
    /// plaintext.
    std::optional<asio::ssl::context> mSslContext;
#endif

    mutable std::mutex mMutex;
    std::condition_variable mCv;

    /// Byte queue. Backed by `LineBytesQueue` (deque<char> with
    /// line-aware back-pressure) so the per-batch front drop is
    /// O(K-bytes-dropped) instead of O(remaining buffer). The parse
    /// loop's `Read` / `WaitForBytes` pattern stays uniform across
    /// all producers.
    LineBytesQueue mReadyBuffer;

    /// Active sessions, keyed by id. Lives on the mutex so `Stop` can
    /// snapshot and close them without racing the accept handler.
    std::unordered_map<size_t, std::shared_ptr<SessionBase>> mActiveSessions;
    size_t mNextSessionId = 1;

    std::atomic<bool> mStopRequested{false};
    std::atomic<bool> mWorkerExited{false};
    std::atomic<size_t> mTotalAccepted{0};
    std::atomic<size_t> mTotalRejected{0};
    std::atomic<size_t> mDroppedByteCount{0};

    uint16_t mBoundPort = 0;

    mutable std::mutex mCallbackMutex;
    std::function<void(SourceStatus)> mStatusCallback;
    SourceStatus mLastReportedStatus = SourceStatus::Waiting;

    std::thread mWorker;
};

template <class Stream>
Session<Stream>::Session(Stream stream, TcpServerProducerImpl *impl, size_t id, size_t readChunkBytes)
    : mStream(std::move(stream)), mImpl(impl), mId(id), mReadBuffer(readChunkBytes)
{
}

template <class Stream> void Session<Stream>::Start()
{
    auto self = this->shared_from_this();
#ifdef LOGLIB_HAS_TLS
    if constexpr (std::is_same_v<Stream, asio::ssl::stream<asio::ip::tcp::socket>>)
    {
        mStream.async_handshake(asio::ssl::stream_base::server, [self](const asio::error_code &ec) {
            if (ec)
            {
                self->Finalize();
                return;
            }
            self->DoRead();
        });
    }
    else
    {
        DoRead();
    }
#else
    DoRead();
#endif
}

// NOLINTNEXTLINE(misc-no-recursion): async continuation chain; not stack recursion.
template <class Stream> void Session<Stream>::DoRead()
{
    auto self = this->shared_from_this();
    // NOLINTNEXTLINE(misc-no-recursion): Asio completion handler re-arms reads asynchronously.
    mStream.async_read_some(asio::buffer(mReadBuffer), [self](const asio::error_code &ec, std::size_t n) {
        self->OnRead(ec, n);
    });
}

// NOLINTNEXTLINE(misc-no-recursion): async continuation chain; not stack recursion.
template <class Stream> void Session<Stream>::OnRead(const asio::error_code &ec, std::size_t bytes)
{
    if (ec)
    {
        // EOF, peer reset, or our own close from `Stop()`. All paths
        // converge on a clean session shutdown.
        Finalize();
        return;
    }
    if (bytes == 0)
    {
        // NOLINTNEXTLINE(misc-no-recursion): async re-arm; not synchronous stack recursion.
        DoRead();
        return;
    }

    // Append into the per-session carry, then split out only complete
    // lines (terminated by '\n') and forward those to the shared
    // queue. The trailing partial stays in the carry until the next
    // chunk -- this is what guarantees that lines from concurrent
    // peers don't tear in the shared queue.
    mCarry.append(mReadBuffer.data(), bytes);

    if (const auto lastNewline = mCarry.rfind('\n'); lastNewline != std::string::npos)
    {
        const std::string_view completeLines(mCarry.data(), lastNewline + 1);
        mImpl->OnSessionLines(completeLines);
        mCarry.erase(0, lastNewline + 1);
    }

    // NOLINTNEXTLINE(misc-no-recursion): async re-arm; not synchronous stack recursion.
    DoRead();
}

template <class Stream> void Session<Stream>::Close() noexcept
{
    asio::error_code ec;
    if constexpr (std::is_same_v<Stream, asio::ip::tcp::socket>)
    {
        mStream.close(ec);
    }
#ifdef LOGLIB_HAS_TLS
    else if constexpr (std::is_same_v<Stream, asio::ssl::stream<asio::ip::tcp::socket>>)
    {
        mStream.lowest_layer().close(ec);
    }
#endif
}

template <class Stream> void Session<Stream>::Finalize()
{
    if (mFinalized)
    {
        return;
    }
    mFinalized = true;

    // Flush the partial-line carry as a synthetic last line so a peer
    // disconnect mid-record does not silently lose its trailing line.
    if (!mCarry.empty())
    {
        if (mCarry.back() != '\n')
        {
            mCarry.push_back('\n');
        }
        mImpl->OnSessionLines(std::string_view(mCarry));
        mCarry.clear();
    }

    Close();
    mImpl->OnSessionEnded(mId);
}

#ifdef LOGLIB_HAS_TLS
namespace
{

/// Build the server-side TLS context from the user-supplied options.
/// Throws `std::runtime_error` on missing or malformed cert / key
/// files so the producer constructor surfaces the failure synchronously.
asio::ssl::context BuildSslContext(const TcpServerProducer::TlsOptions &tls)
{
    asio::ssl::context ctx(asio::ssl::context::tls_server);
    // Disable old, broken protocols. tls1.0 / tls1.1 are deprecated;
    // only tls1.2 and tls1.3 remain enabled with this combo.
    ctx.set_options(
        asio::ssl::context::default_workarounds | asio::ssl::context::no_sslv2 | asio::ssl::context::no_sslv3 |
        asio::ssl::context::no_tlsv1 | asio::ssl::context::no_tlsv1_1 | asio::ssl::context::single_dh_use
    );

    asio::error_code ec;
    ctx.use_certificate_chain_file(tls.certificateChain.string(), ec);
    if (ec)
    {
        throw std::runtime_error(
            "TcpServerProducer: failed to load certificate chain '" + tls.certificateChain.string() +
            "': " + ec.message()
        );
    }
    ctx.use_private_key_file(tls.privateKey.string(), asio::ssl::context::pem, ec);
    if (ec)
    {
        throw std::runtime_error(
            "TcpServerProducer: failed to load private key '" + tls.privateKey.string() + "': " + ec.message()
        );
    }

    if (!tls.caBundle.empty())
    {
        ctx.load_verify_file(tls.caBundle.string(), ec);
        if (ec)
        {
            throw std::runtime_error(
                "TcpServerProducer: failed to load CA bundle '" + tls.caBundle.string() + "': " + ec.message()
            );
        }
        int verifyMode = asio::ssl::verify_peer;
        if (tls.requireClientCertificate)
        {
            verifyMode |= asio::ssl::verify_fail_if_no_peer_cert;
        }
        ctx.set_verify_mode(verifyMode);
    }
    else
    {
        ctx.set_verify_mode(asio::ssl::verify_none);
    }
    return ctx;
}

} // namespace
#endif // LOGLIB_HAS_TLS

TcpServerProducerImpl::TcpServerProducerImpl(TcpServerProducer::Options options)
    : mOptions(std::move(options)), mAcceptor(mIoContext)
{
    if (mOptions.tls.has_value())
    {
#ifdef LOGLIB_HAS_TLS
        mSslContext.emplace(BuildSslContext(*mOptions.tls));
#else
        throw std::runtime_error("TcpServerProducer: TLS support is not built in (rebuild with LOGLIB_NETWORK_TLS=ON)");
#endif
    }

    asio::error_code ec;
    const asio::ip::address bindAddr = asio::ip::make_address(mOptions.bindAddress, ec);
    if (ec)
    {
        throw std::system_error(ec, "TcpServerProducer: invalid bind address '" + mOptions.bindAddress + "'");
    }

    const asio::ip::tcp::endpoint endpoint(bindAddr, mOptions.port);
    mAcceptor.open(endpoint.protocol(), ec);
    if (ec)
    {
        throw std::system_error(ec, "TcpServerProducer: acceptor open failed");
    }
    mAcceptor.set_option(asio::socket_base::reuse_address(true), ec);
    mAcceptor.bind(endpoint, ec);
    if (ec)
    {
        throw std::system_error(
            ec, "TcpServerProducer: bind failed for " + mOptions.bindAddress + ":" + std::to_string(mOptions.port)
        );
    }
    mAcceptor.listen(asio::socket_base::max_listen_connections, ec);
    if (ec)
    {
        throw std::system_error(ec, "TcpServerProducer: listen failed");
    }
    mBoundPort = mAcceptor.local_endpoint().port();

    const std::string scheme = mOptions.tls.has_value() ? "tcp+tls://" : "tcp://";
    mDisplayName = scheme + mOptions.bindAddress + ":" + std::to_string(mBoundPort);

    StartAccept();
    mWorker = std::thread([this] {
        try
        {
            mIoContext.run();
        }
        catch (...)
        {
            // Asio handlers in this TU never throw; defensive only.
            static_cast<void>(0);
        }
        mWorkerExited.store(true, std::memory_order_release);
        mCv.notify_all();
    });
}

TcpServerProducerImpl::~TcpServerProducerImpl()
{
    Stop();
    if (mWorker.joinable())
    {
        mWorker.join();
    }
}

size_t TcpServerProducerImpl::Read(std::span<char> buffer)
{
    if (buffer.empty())
    {
        return 0;
    }
    const std::scoped_lock lock(mMutex);
    return mReadyBuffer.Read(buffer);
}

void TcpServerProducerImpl::WaitForBytes(std::chrono::milliseconds timeout)
{
    if (timeout.count() <= 0)
    {
        return;
    }
    std::unique_lock<std::mutex> lock(mMutex);
    mCv.wait_for(lock, timeout, [&] {
        return !mReadyBuffer.Empty() || mStopRequested.load(std::memory_order_acquire) ||
               mWorkerExited.load(std::memory_order_acquire);
    });
}

// NOLINTNEXTLINE(bugprone-exception-escape): `asio::io_context::stop()` may throw in rare error paths; `noexcept`
// matches producer contract; catch block is the safety net.
void TcpServerProducerImpl::Stop() noexcept
{
    if (mStopRequested.exchange(true, std::memory_order_acq_rel))
    {
        return;
    }
    // Post the close work onto the io_context so the acceptor and
    // session sockets are touched only on the worker thread (the same
    // thread that owns the in-flight async operations). This avoids a
    // hard `mIoContext.stop()` that would abandon queued handlers --
    // here every in-flight `async_accept` / `async_read_some` instead
    // completes with `operation_aborted` and the session's `Finalize`
    // path runs cleanly, flushing the per-client carry buffer as a
    // synthetic last line. Once every session has finalised and the
    // acceptor is closed, the io_context's outstanding work count
    // drops to zero and `run()` returns naturally.
    try
    {
        asio::post(mIoContext, [this] {
            asio::error_code ec;
            mAcceptor.close(ec);

            std::vector<std::shared_ptr<SessionBase>> snapshot;
            {
                const std::scoped_lock lock(mMutex);
                snapshot.reserve(mActiveSessions.size());
                for (auto &kv : mActiveSessions)
                {
                    snapshot.push_back(kv.second);
                }
            }
            for (auto &session : snapshot)
            {
                if (session)
                {
                    session->Close();
                }
            }
        });
    }
    catch (...)
    {
        // `asio::post` can theoretically throw on allocator failure; in
        // that case fall back to a hard stop so the producer still
        // terminates rather than wedging the worker thread.
        mIoContext.stop();
    }
    mCv.notify_all();
}

bool TcpServerProducerImpl::IsClosed() const noexcept
{
    if (!mWorkerExited.load(std::memory_order_acquire))
    {
        return false;
    }
    const std::scoped_lock lock(mMutex);
    return mReadyBuffer.Empty();
}

std::string TcpServerProducerImpl::DisplayName() const
{
    return mDisplayName;
}

void TcpServerProducerImpl::SetStatusCallback(const std::function<void(SourceStatus)> &callback)
{
    std::function<void(SourceStatus)> snapshot;
    SourceStatus current{};
    {
        const std::scoped_lock lock(mCallbackMutex);
        mStatusCallback = callback;
        snapshot = mStatusCallback;
        current = mLastReportedStatus;
    }
    if (snapshot)
    {
        snapshot(current);
    }
}

uint16_t TcpServerProducerImpl::BoundPort() const noexcept
{
    return mBoundPort;
}

size_t TcpServerProducerImpl::ActiveClientCount() const noexcept
{
    const std::scoped_lock lock(mMutex);
    return mActiveSessions.size();
}

size_t TcpServerProducerImpl::TotalClientsAccepted() const noexcept
{
    return mTotalAccepted.load(std::memory_order_acquire);
}

size_t TcpServerProducerImpl::TotalClientsRejected() const noexcept
{
    return mTotalRejected.load(std::memory_order_acquire);
}

size_t TcpServerProducerImpl::DroppedByteCount() const noexcept
{
    return mDroppedByteCount.load(std::memory_order_acquire);
}

void TcpServerProducerImpl::OnSessionLines(std::string_view completeLines)
{
    if (completeLines.empty())
    {
        return;
    }
    {
        const std::scoped_lock lock(mMutex);
        mReadyBuffer.Append(completeLines, mOptions.maxQueueBytes, mDroppedByteCount);
    }
    MarkRunning();
    mCv.notify_all();
}

void TcpServerProducerImpl::OnSessionEnded(size_t sessionId)
{
    {
        const std::scoped_lock lock(mMutex);
        mActiveSessions.erase(sessionId);
    }
    mCv.notify_all();
}

void TcpServerProducerImpl::MarkRunning()
{
    std::function<void(SourceStatus)> cb;
    {
        const std::scoped_lock lock(mCallbackMutex);
        if (mLastReportedStatus == SourceStatus::Running)
        {
            return;
        }
        mLastReportedStatus = SourceStatus::Running;
        cb = mStatusCallback;
    }
    if (cb)
    {
        cb(SourceStatus::Running);
    }
}

void TcpServerProducerImpl::StartAccept()
{
    if (mStopRequested.load(std::memory_order_acquire))
    {
        return;
    }
#ifdef LOGLIB_HAS_TLS
    if (mSslContext.has_value())
    {
        mAcceptor.async_accept([this](const asio::error_code &ec, asio::ip::tcp::socket socket) {
            OnAcceptTls(ec, std::move(socket));
        });
        return;
    }
#endif
    mAcceptor.async_accept([this](const asio::error_code &ec, asio::ip::tcp::socket socket) {
        OnAcceptPlain(ec, std::move(socket));
    });
}

bool TcpServerProducerImpl::AdmitOrReject(asio::ip::tcp::socket &socket)
{
    const std::scoped_lock lock(mMutex);
    if (mActiveSessions.size() >= mOptions.maxConcurrentClients)
    {
        asio::error_code closeEc;
        socket.close(closeEc);
        mTotalRejected.fetch_add(1, std::memory_order_acq_rel);
        return false;
    }
    return true;
}

void TcpServerProducerImpl::OnAcceptPlain(const asio::error_code &ec, asio::ip::tcp::socket socket)
{
    if (mStopRequested.load(std::memory_order_acquire))
    {
        return;
    }
    if (ec)
    {
        // Acceptor closed during shutdown is benign; other errors are
        // also ignored -- we keep the listener alive across them so a
        // single bad connection cannot wedge the producer.
        if (ec != asio::error::operation_aborted)
        {
            StartAccept();
        }
        return;
    }
    if (AdmitOrReject(socket))
    {
        size_t id = 0;
        std::shared_ptr<Session<asio::ip::tcp::socket>> session;
        {
            const std::scoped_lock lock(mMutex);
            id = mNextSessionId++;
            session =
                std::make_shared<Session<asio::ip::tcp::socket>>(std::move(socket), this, id, mOptions.readChunkBytes);
            mActiveSessions[id] = session;
        }
        mTotalAccepted.fetch_add(1, std::memory_order_acq_rel);
        session->Start();
    }
    StartAccept();
}

#ifdef LOGLIB_HAS_TLS
void TcpServerProducerImpl::OnAcceptTls(const asio::error_code &ec, asio::ip::tcp::socket socket)
{
    if (mStopRequested.load(std::memory_order_acquire))
    {
        return;
    }
    if (ec)
    {
        if (ec != asio::error::operation_aborted)
        {
            StartAccept();
        }
        return;
    }
    if (AdmitOrReject(socket))
    {
        using TlsStream = asio::ssl::stream<asio::ip::tcp::socket>;
        size_t id = 0;
        std::shared_ptr<Session<TlsStream>> session;
        {
            const std::scoped_lock lock(mMutex);
            id = mNextSessionId++;
            TlsStream stream(std::move(socket), *mSslContext);
            session = std::make_shared<Session<TlsStream>>(std::move(stream), this, id, mOptions.readChunkBytes);
            mActiveSessions[id] = session;
        }
        mTotalAccepted.fetch_add(1, std::memory_order_acq_rel);
        session->Start();
    }
    StartAccept();
}
#endif // LOGLIB_HAS_TLS

} // namespace loglib::internal

namespace loglib
{

TcpServerProducer::TcpServerProducer(Options options)
    // NOLINTNEXTLINE(clang-analyzer-cplusplus.NewDelete): Asio `win_thread` false positive under MSVC headers.
    : mImpl(std::make_unique<internal::TcpServerProducerImpl>(std::move(options)))
{
}

TcpServerProducer::~TcpServerProducer() = default;

size_t TcpServerProducer::Read(std::span<char> buffer)
{
    return mImpl->Read(buffer);
}

void TcpServerProducer::WaitForBytes(std::chrono::milliseconds timeout)
{
    mImpl->WaitForBytes(timeout);
}

void TcpServerProducer::Stop() noexcept
{
    mImpl->Stop();
}

bool TcpServerProducer::IsClosed() const noexcept
{
    return mImpl->IsClosed();
}

std::string TcpServerProducer::DisplayName() const
{
    return mImpl->DisplayName();
}

void TcpServerProducer::SetStatusCallback(const std::function<void(SourceStatus)> &callback)
{
    mImpl->SetStatusCallback(callback);
}

uint16_t TcpServerProducer::BoundPort() const noexcept
{
    return mImpl->BoundPort();
}

size_t TcpServerProducer::ActiveClientCount() const noexcept
{
    return mImpl->ActiveClientCount();
}

size_t TcpServerProducer::TotalClientsAccepted() const noexcept
{
    return mImpl->TotalClientsAccepted();
}

size_t TcpServerProducer::TotalClientsRejected() const noexcept
{
    return mImpl->TotalClientsRejected();
}

size_t TcpServerProducer::DroppedByteCount() const noexcept
{
    return mImpl->DroppedByteCount();
}

} // namespace loglib
