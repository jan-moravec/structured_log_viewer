#pragma once

#include "loglib/line_source.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

namespace loglib
{

class BytesProducer;

/// `LineSource` over a live byte producer. Owns the bytes backing each
/// line in a pair of `std::deque<std::string>`s (raw text + per-line
/// owned arena). No session-global arena: eviction simply drops the
/// corresponding entries.
///
/// - `BytesAreStable()` is `false`; the parser must emit
///   `OwnedString` payloads (not `MmapSlice`).
/// - `SupportsEviction()` is `true`; `EvictBefore` is the retention
///   hook used by `LogTable` / `LogModel`.
/// - LineIds are 1-based monotonic, assigned by `AppendLine`.
/// - Thread-safe: a mutex guards all members. The parser worker
///   appends; the GUI reads and may evict. `std::deque` push_back is
///   reference-stable, so concurrent reads on existing entries are
///   safe. `string_view`s from `ResolveOwnedBytes` are invalidated by
///   `EvictBefore` on that line id.
class StreamLineSource final : public LineSource
{
public:
    /// @param displayName  GUI-facing identity (typically a file path).
    /// @param producer     Byte producer for this stream. May be null
    ///                     in tests that drive `AppendLine` directly.
    StreamLineSource(std::filesystem::path displayName, std::unique_ptr<BytesProducer> producer);

    ~StreamLineSource() override;

    StreamLineSource(const StreamLineSource &) = delete;
    StreamLineSource &operator=(const StreamLineSource &) = delete;

    StreamLineSource(StreamLineSource &&) = delete;
    StreamLineSource &operator=(StreamLineSource &&) = delete;

    [[nodiscard]] const std::filesystem::path &Path() const noexcept override;
    [[nodiscard]] std::string RawLine(size_t lineId) const override;

    [[nodiscard]] std::string_view
    ResolveMmapBytes(uint64_t offset, uint32_t length, size_t lineId) const noexcept override;

    [[nodiscard]] std::string_view
    ResolveOwnedBytes(uint64_t offset, uint32_t length, size_t lineId) const noexcept override;

    [[nodiscard]] std::span<const char> StableBytes() const noexcept override;

    uint64_t AppendOwnedBytes(size_t lineId, std::string_view bytes) override;

    [[nodiscard]] bool SupportsEviction() const noexcept override;

    void EvictBefore(size_t firstSurvivingLineId) override;
    [[nodiscard]] size_t FirstAvailableLineId() const noexcept override;

    /// Borrow the byte producer; ownership stays with the source.
    /// Returns `nullptr` if the source was constructed without one.
    [[nodiscard]] BytesProducer *Producer() noexcept;
    [[nodiscard]] const BytesProducer *Producer() const noexcept;

    /// Append @p rawLine and its escape-decoded byte arena. Returns
    /// the assigned 1-based monotonic `lineId`. `ownedBytes` may be
    /// empty if the line had no escape-decoded fields.
    size_t AppendLine(std::string rawLine, std::string ownedBytes);

    /// Number of lines currently held (post-eviction).
    [[nodiscard]] size_t Size() const noexcept;

    /// Total bytes owned: line bytes + per-line owned arenas.
    /// Capacity-accurate; benchmark-only, not on the parse hot path.
    [[nodiscard]] size_t OwnedMemoryBytes() const noexcept;

private:
    [[nodiscard]] bool LineIsLiveLocked(size_t lineId) const noexcept;
    [[nodiscard]] size_t IndexForLocked(size_t lineId) const noexcept;

    std::filesystem::path mDisplayName;
    std::unique_ptr<BytesProducer> mProducer;

    /// Guards every member below.
    mutable std::mutex mLock;

    /// Raw line text (no trailing `\n` / `\r`). `std::deque` push_back
    /// is reference-stable, so concurrent reads on existing entries
    /// remain valid.
    std::deque<std::string> mLines;

    /// Per-line escape-decoded byte arena. Same indexing as `mLines`.
    std::deque<std::string> mLineOwnedBytes;

    size_t mFirstAvailableLineId = 1;
    size_t mNextLineId = 1;
};

} // namespace loglib
