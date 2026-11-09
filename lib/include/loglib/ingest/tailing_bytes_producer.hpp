/**
 * @brief Byte producer that follows a file as it grows.
 *
 * A TailingBytesProducer first loads the last lines already in the file, then delivers new lines as they are appended.
 * It keeps following through the usual rotation styles: rename and create, copy and truncate, truncate in place, and
 * delete and recreate. Used for live-tail sessions.
 *
 * This is part of the ingest module.
 */
#pragma once

#include "loglib/core/bytes_producer.hpp"

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string>

namespace loglib
{

namespace internal
{
class TailingBytesProducerImpl; // pimpl forward decl
}

/**
 * @brief A BytesProducer that follows a file while it is being written.
 *
 * It first delivers the last complete lines already in the file, then the lines that are appended. It keeps following
 * through the usual rotation styles: rename and create, copy and truncate, truncate in place, and delete and recreate.
 *
 * - Reads use plain read calls and not a memory mapping, because a rotation would invalidate a mapping or block the
 *   writer.
 * - A worker thread checks the file size every pollInterval and also wakes when the operating system reports a change,
 *   so the delay is usually the watcher's and not the poll's.
 * - A rotation is detected when the file identity changes, the path is missing, or the size shrank. Rotations within
 *   rotationDebounce count as one.
 * - Read returns complete lines only. A partial last line waits in a separate buffer. It is dropped on truncation and
 *   delivered as a last line on Stop or when a rename rotation retires the file.
 * - After a rename rotation the old file is read to its end before switching, so nothing written before the rename is
 *   lost.
 */
class TailingBytesProducer final : public BytesProducer
{
public:
    /** @brief The default for Options::maxQueuedBytes, 64 MiB. */
    static constexpr size_t DEFAULT_MAX_QUEUED_BYTES = size_t{64} * 1024 * 1024;

    /** @brief Settings. Tests on slow machines shorten the intervals and turn the native watcher off. */
    // NOLINTBEGIN(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)
    struct Options
    {
        /** @brief How often the worker checks the file when the operating system reports nothing. */
        std::chrono::milliseconds pollInterval = std::chrono::milliseconds(250);

        /** @brief Rotations that happen within this time count as one event. */
        std::chrono::milliseconds rotationDebounce = std::chrono::milliseconds(1000);

        /** @brief How many bytes one read takes. 64 KiB keeps the partial-line buffer small without many calls. */
        size_t readChunkBytes = 64 * 1024;

        /**
         * @brief How many bytes one backward read takes while looking for the last lines. Separate so tests can shrink
         * it.
         */
        size_t prefillChunkBytes = 64 * 1024;

        /**
         * @brief The most bytes the backward search for the last lines may scan.
         *
         * It protects against files without newlines and against a huge retentionLines on a file of many gigabytes. If
         * the limit is reached, no earlier lines are delivered and tailing starts at the end of the file.
         */
        size_t prefillMaxScanBytes = 16 * 1024 * 1024;

        /**
         * @brief Does not use the native file watcher and relies on polling only. Tests set it where file events are
         * unreliable.
         */
        bool disableNativeWatcher = false;

        /**
         * @brief The longest line that is held while waiting for its newline. A longer line is dropped up to its
         * newline and counted in DroppedByteCount.
         */
        size_t maxLineBytes = DEFAULT_MAX_LINE_BYTES;

        /**
         * @brief Reading pauses while this many unread bytes are queued, and resumes when the consumer catches up.
         * Nothing is dropped.
         */
        size_t maxQueuedBytes = DEFAULT_MAX_QUEUED_BYTES;
    };
    // NOLINTEND(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)

    /**
     * @brief Starts the worker thread.
     *
     * The last retentionLines complete lines of the file are queued first, and tailing continues from where they ended.
     * It throws if the file cannot be opened at the start. A path that goes missing later, during a rotation, is
     * handled inside and never thrown.
     */
    TailingBytesProducer(std::filesystem::path path, size_t retentionLines);
    TailingBytesProducer(std::filesystem::path path, size_t retentionLines, Options options);

    ~TailingBytesProducer() override;

    TailingBytesProducer(const TailingBytesProducer &) = delete;
    TailingBytesProducer &operator=(const TailingBytesProducer &) = delete;
    TailingBytesProducer(TailingBytesProducer &&) = delete;
    TailingBytesProducer &operator=(TailingBytesProducer &&) = delete;

    size_t Read(std::span<char> buffer) override;

    void WaitForBytes(std::chrono::milliseconds timeout) override;

    /**
     * @brief Wakes any blocked Read or WaitForBytes, delivers the partial last line as a line of its own, unless a
     * rotation is in progress, and tells the worker to exit.
     *
     * Safe from any thread and safe to call twice.
     */
    void Stop() noexcept override;

    [[nodiscard]] bool IsClosed() const noexcept override;

    [[nodiscard]] std::string DisplayName() const override;

    void SetRotationCallback(const std::function<void()> &callback) override;

    void SetStatusCallback(const std::function<void(SourceStatus)> &callback) override;

    /**
     * @brief How many rotations the worker has seen. The rotation callback fires less often when rotations are merged,
     * which tests check with this count.
     */
    [[nodiscard]] size_t RotationCount() const noexcept;

    /**
     * @brief How many passes the worker has finished. A pass checks for rotation and reads what is new. A caller that
     * changed the file and then saw the count rise by two knows the worker has looked at the file since the change.
     * Tests use this to wait for the worker and to wait out a known number of passes.
     */
    [[nodiscard]] size_t PollCount() const noexcept;

    /** @brief How many bytes were dropped from lines longer than Options::maxLineBytes. */
    [[nodiscard]] size_t DroppedByteCount() const noexcept override;

private:
    std::unique_ptr<internal::TailingBytesProducerImpl> mImpl;
};

} // namespace loglib
