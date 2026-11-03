/**
 * @brief Byte producer that reads standard input.
 *
 * StdinBytesProducer feeds a piped log into the same streaming path as a tailed file. Reading happens on a worker
 * thread so the caller never blocks, and end of input is final: there is nothing to rotate or recover. It is used when
 * the program is started with a pipe.
 *
 * This is part of the ingest module.
 */
#pragma once

#include "loglib/core/bytes_producer.hpp"

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
} // namespace internal

/**
 * @brief A BytesProducer that reads the process's standard input.
 *
 * It serves piped input such as a command line tool started with a pipe. Bytes flow one way and the end of input is
 * final, so unlike TailingBytesProducer there is nothing to rotate or recover.
 *
 * - A worker thread reads from a duplicate of the standard input handle, so the original handle stays untouched. Stop
 *   closes the duplicate, which wakes a read that is blocked.
 * - Each read takes up to readChunkBytes. The bytes go into a LineBytesQueue, the same queue the network producers use,
 *   and Read takes them from there.
 * - WaitForBytes sleeps until the worker adds bytes or reaches the end of input.
 * - Stop closes the duplicate, joins the worker and marks the producer closed. It is safe to call twice and from any
 *   thread.
 * - Format detection happens before the producer is used. The caller peeks with StdinPeek and hands those bytes to the
 *   parser through ParserOptions::initialCarry, so the producer only delivers the bytes that come after the peek.
 */
class StdinBytesProducer final : public BytesProducer
{
public:
    /**
     * @brief The default size of one read, 64 KiB. TailingBytesProducer uses the same size, so their throughput can be
     * compared.
     */
    static constexpr std::size_t DEFAULT_READ_CHUNK_BYTES = 64UZ * 1024UZ;

    /** @brief The default soft limit for the byte queue, 64 MiB. TcpServerProducer uses the same limit. */
    static constexpr std::size_t DEFAULT_QUEUE_CAP_BYTES = 64UZ * 1024UZ * 1024UZ;

    /** @brief Settings, mostly for tests. */
    struct Options
    {
        /** @brief How many bytes one read takes. */
        std::size_t readChunkBytes = DEFAULT_READ_CHUNK_BYTES;

        /**
         * @brief Soft limit for the byte queue. Zero turns it off.
         *
         * When the parser cannot keep up, the oldest bytes are dropped up to the next newline, as in
         * TcpServerProducer.
         */
        std::size_t queueCapBytes = DEFAULT_QUEUE_CAP_BYTES;

        /** @brief The name DisplayName returns. Tests change it to tell several producers apart. */
        std::string displayName = "<stdin>";
    };

    /**
     * @brief Starts the worker thread.
     *
     * The handle of standard input is duplicated first, so reads in the worker cannot race with other code that still
     * uses standard input. Throws std::runtime_error, with the operating system's message, if duplicating fails.
     */
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

    /**
     * @brief How many bytes were dropped because the queue was full. Tests use it to check that memory stays bounded.
     */
    [[nodiscard]] std::size_t DroppedByteCount() const noexcept;

private:
    friend struct internal::StdinBytesProducerTestAccess;

    /**
     * @brief Constructor for tests. It reads from the given handle, a HANDLE on Windows or a file descriptor cast to a
     * pointer elsewhere. The producer owns the handle and closes it on Stop.
     */
    struct FromRawTag
    {
    };
    StdinBytesProducer(FromRawTag, void *opaqueHandle, Options options);

    std::unique_ptr<internal::StdinBytesProducerImpl> mImpl;
};

namespace internal
{

/**
 * @brief Lets tests build a StdinBytesProducer on a pipe of their own. It is not part of the public API.
 *
 * Tests create a pipe and pass its read end to Create, and the producer owns and closes that end. On Windows the handle
 * is a HANDLE cast to a pointer. Elsewhere it is a file descriptor converted to an integer and then to a pointer.
 * Production code uses the public constructors.
 */
struct StdinBytesProducerTestAccess
{
    [[nodiscard]] static std::unique_ptr<StdinBytesProducer> Create(
        void *opaqueHandle, StdinBytesProducer::Options options
    );
};

} // namespace internal

} // namespace loglib
