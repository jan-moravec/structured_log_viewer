#pragma once

#include "bytes_producer.hpp"

#include <chrono>
#include <cstddef>
#include <memory>
#include <span>
#include <string>

namespace loglib
{

namespace internal
{
class StdinBytesProducerImpl; // pimpl forward decl
struct StdinBytesProducerTestAccess;
}

/// `BytesProducer` over the process's standard input. Used by the
/// stdin CLI path (`slv -` / `slv --stdin`) and any future feature
/// that wants to consume a pipe. Bytes flow one direction, EOF is
/// terminal: unlike `TailingBytesProducer` there is nothing to
/// rotate or recover.
///
/// Design summary:
///   - Blocking `read(2)` / `ReadFile` on a duplicate of the process's
///     stdin FD / HANDLE. The dup is closed on `Stop()` so a
///     blocked `read` returns immediately; the process's original
///     stdin is left untouched.
///   - One worker thread per producer, drains `readChunkBytes` per
///     syscall (default 64 KiB, matching `TailingBytesProducer`).
///   - `Read()` drains a byte queue backed by
///     `internal::LineBytesQueue`, shared with the network stream
///     producers.
///   - `WaitForBytes` parks on a condition variable that the
///     worker signals on every new chunk or on EOF.
///   - `Stop()` closes the dup, joins the worker, marks the
///     producer terminally closed. Idempotent and safe from any
///     thread (GUI teardown, session switch, `NewSession`).
///   - Format detection is handled *outside* this class via the
///     synchronous `internal::StdinPeek` on the GUI thread; the
///     peeked bytes are fed to the parser via
///     `ParserOptions::initialCarry` and the producer only yields
///     the bytes remaining after the peek.
class StdinBytesProducer final : public BytesProducer
{
public:
    /// Tuning knobs, mostly for tests.
    struct Options
    {
        /// Bytes per stdin read syscall. 64 KiB amortises syscall
        /// cost across a typical console-piped writer's chunk
        /// boundary. Sized to match `TailingBytesProducer` so
        /// throughput profiles compare like-for-like.
        std::size_t readChunkBytes = 64 * 1024;

        /// Soft cap on the internal byte queue. 0 disables the
        /// cap. When the parser can't keep up, oldest bytes are
        /// dropped to the next newline boundary (identical
        /// semantics to `TcpServerProducer`).
        std::size_t queueCapBytes = 64 * 1024 * 1024;

        /// Display name shown in the status bar / window title.
        /// The default `<stdin>` matches shell convention; tests
        /// override it to distinguish multiple producers.
        std::string displayName = "<stdin>";
    };

    /// Construct and spawn the worker. Duplicates the process's
    /// stdin FD / HANDLE up front so the worker's `read` calls
    /// don't race with any other code that might still be using
    /// stdin (in practice only in tests). Throws
    /// `std::runtime_error` if the duplicate fails; the OS
    /// error is included in the message.
    StdinBytesProducer();
    explicit StdinBytesProducer(Options options);

    ~StdinBytesProducer() override;

    StdinBytesProducer(const StdinBytesProducer &) = delete;
    StdinBytesProducer &operator=(const StdinBytesProducer &) = delete;
    StdinBytesProducer(StdinBytesProducer &&) = delete;
    StdinBytesProducer &operator=(StdinBytesProducer &&) = delete;

    std::size_t Read(std::span<char> buffer) override;

    void WaitForBytes(std::chrono::milliseconds timeout) override;

    void Stop() noexcept override;

    [[nodiscard]] bool IsClosed() const noexcept override;

    [[nodiscard]] std::string DisplayName() const override;

    /// Byte count dropped because of queue-cap back-pressure.
    /// Mirrors the accessor on `TcpServerProducer`; tests use it
    /// to assert bounded memory under sustained overload.
    [[nodiscard]] std::size_t DroppedByteCount() const noexcept;

private:
    friend struct internal::StdinBytesProducerTestAccess;

    /// Test-only ctor. `opaqueHandle` is a `HANDLE` on Windows or a
    /// POSIX fd cast to `void*` on other platforms; the producer
    /// takes ownership and closes it on `Stop()`. Reachable only
    /// through `internal::StdinBytesProducerTestAccess`.
    struct FromRawTag
    {
    };
    StdinBytesProducer(FromRawTag, void *opaqueHandle, Options options);

    std::unique_ptr<internal::StdinBytesProducerImpl> mImpl;
};

namespace internal
{

/// Test-support hook. Not part of the public API; production code
/// should always construct `StdinBytesProducer` with its default
/// / `Options` ctor. Tests build a `pipe(2)` / `CreatePipe` and
/// hand the read-end to `Create` -- the producer then owns and
/// closes that handle on `Stop()`. `opaqueHandle` conventions:
/// - Windows: `HANDLE` cast to `void *` (i.e. the read-end).
/// - POSIX: a file descriptor packed into `void *` via
///   `reinterpret_cast<void *>(static_cast<intptr_t>(fd))`.
struct StdinBytesProducerTestAccess
{
    [[nodiscard]] static std::unique_ptr<StdinBytesProducer> Create(
        void *opaqueHandle, StdinBytesProducer::Options options
    );
};

} // namespace internal

} // namespace loglib
