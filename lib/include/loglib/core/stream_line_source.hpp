/**
 * @brief Line source backed by a live byte producer.
 *
 * A StreamLineSource reads bytes from a BytesProducer and keeps each line, and the strings that cannot be views, in
 * memory. It can drop old lines, which lets a live tail run for days with bounded memory. The parser must store owned
 * strings for it, because there is no stable file to point into.
 *
 * The line id of a row is a one-based number that AppendLine gives to each record, and it grows by one per record. A
 * record that spans several lines gets one id. An id counts the records appended so far, not physical lines, and
 * eviction does not renumber the records that remain.
 *
 * This is part of the core module.
 */
#pragma once

#include "loglib/core/line_source.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <shared_mutex>
#include <string>
#include <string_view>

namespace loglib
{

class BytesProducer;

/**
 * @brief LineSource for a live stream, fed by a BytesProducer.
 *
 * It keeps the text of every line, and the decoded strings of that line, in memory. Eviction drops the entries of the
 * oldest lines, which is how a streaming session stays within its retention limit.
 *
 * - Its bytes are not stable, so parsers store decoded values as OwnedString cells. A MmapSlice cell of a stream row is
 * a slice of the raw line of the row, which the source keeps.
 * - It supports eviction.
 * - A reader-writer lock guards all state, so a parser thread can append while other threads read or evict. A view from
 *   ResolveOwnedBytes or ResolveMmapBytes is invalid once its line is evicted.
 * - Each read takes the lock on its own. A loop over many rows takes it once with a ReadBatch instead.
 */
class StreamLineSource final : public LineSource
{
public:
    /**
     * @param displayName Name to show for the source, usually a file path.
     * @param producer Where the bytes come from. May be null for a source that is only filled through AppendLine.
     */
    StreamLineSource(std::filesystem::path displayName, std::unique_ptr<BytesProducer> producer);

    ~StreamLineSource() override;

    StreamLineSource(const StreamLineSource &) = delete;
    StreamLineSource &operator=(const StreamLineSource &) = delete;

    StreamLineSource(StreamLineSource &&) = delete;
    StreamLineSource &operator=(StreamLineSource &&) = delete;

    [[nodiscard]] const std::filesystem::path &Path() const noexcept override;
    [[nodiscard]] std::string RawLine(size_t lineId) const override;

    void AppendRawLine(size_t lineId, std::string &out) const override;

    [[nodiscard]] std::string_view ResolveMmapBytes(
        uint64_t offset, uint32_t length, size_t lineId
    ) const noexcept override;

    [[nodiscard]] std::string_view ResolveOwnedBytes(
        uint64_t offset, uint32_t length, size_t lineId
    ) const noexcept override;

    [[nodiscard]] std::span<const char> StableBytes() const noexcept override;

    uint64_t AppendOwnedBytes(size_t lineId, std::string_view bytes) override;

    [[nodiscard]] bool SupportsEviction() const noexcept override;

    void EvictBefore(size_t firstSurvivingLineId) override;
    [[nodiscard]] size_t FirstAvailableLineId() const noexcept override;

    /** @brief The producer, which stays owned by the source. Null if the source was built without one. */
    [[nodiscard]] BytesProducer *Producer() noexcept;
    [[nodiscard]] const BytesProducer *Producer() const noexcept;

    /** @brief Bytes the producer dropped so far because the reader fell behind. Zero if there is no producer. */
    [[nodiscard]] size_t DroppedByteCount() const noexcept;

    /** @brief Adds a line with its decoded strings, which may be empty, and returns the new line id. */
    size_t AppendLine(std::string rawLine, std::string ownedBytes);

    /**
     * @brief Puts bytes in front of the bytes the source has not handed to a parser yet.
     *
     * A caller that read from the producer to detect the format gives those bytes back here, so the parser processes
     * them first and none are lost. A second call puts its bytes in front of the first call's. Call it before the parse
     * starts.
     */
    void UnreadBytes(std::string bytes);

    /** @brief Returns the bytes given to UnreadBytes, in order, and clears them. The streaming parse calls it once. */
    [[nodiscard]] std::string TakeUnreadBytes();

    /** @brief Number of lines held now, not counting evicted ones. */
    [[nodiscard]] size_t Size() const noexcept;

    /** @brief Heap bytes held by line text and decoded strings, counting capacity. For the memory benchmark. */
    [[nodiscard]] size_t OwnedMemoryBytes() const noexcept;

    /**
     * @brief Holds the read lock of a source on the calling thread, so the reads in between take no lock of their own.
     *
     * A filter or an exporter makes one for each chunk of rows. Appends and evictions wait until it is destroyed, so
     * keep it to one chunk. It does nothing for a null source, and a second one for the same source on the same thread
     * does nothing either. The thread must not append to or evict from the source while it holds one.
     */
    class ReadBatch
    {
    public:
        explicit ReadBatch(const StreamLineSource *source);
        ~ReadBatch();

        ReadBatch(const ReadBatch &) = delete;
        ReadBatch &operator=(const ReadBatch &) = delete;
        ReadBatch(ReadBatch &&) = delete;
        ReadBatch &operator=(ReadBatch &&) = delete;

    private:
        const StreamLineSource *mSource = nullptr;
        const StreamLineSource *mPrevious = nullptr;
    };

private:
    [[nodiscard]] bool LineIsLiveLocked(size_t lineId) const noexcept;
    [[nodiscard]] size_t IndexForLocked(size_t lineId) const noexcept;

    std::filesystem::path mDisplayName;
    std::unique_ptr<BytesProducer> mProducer;

    /** @brief A shared lock for a read, which holds nothing when the thread holds a ReadBatch for this source. */
    [[nodiscard]] std::shared_lock<std::shared_mutex> LockForRead() const;

    /** @brief Guards every member below. Readers share it, and appends and evictions take it alone. */
    mutable std::shared_mutex mLock;

    /** @brief Line text without its line ending. Appending to a deque does not move existing entries. */
    std::deque<std::string> mLines;

    /** @brief Decoded strings of each line, indexed like mLines. */
    std::deque<std::string> mLineOwnedBytes;

    /** @brief Bytes read from the producer by an earlier stage and returned for the parser to read first. */
    std::string mUnreadBytes;

    size_t mFirstAvailableLineId = 1;
    size_t mNextLineId = 1;
};

} // namespace loglib
