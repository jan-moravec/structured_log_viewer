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

/// `LineSource` over a live byte producer (`TailingBytesProducer`, future
/// stdin / TCP / UDP / named-pipe). Owns the bytes that back each line
/// directly: a parallel pair of `std::deque<std::string>`s holds the
/// raw line text and any escape-decoded payload bytes referenced by the
/// matching `LogLine`'s `OwnedString` compact values. There is **no
/// session-global arena** — eviction simply drops the corresponding
/// deque entries.
///
/// Design notes:
///
///   - The `BytesProducer` drives I/O -- `Read` / `WaitForBytes` /
///     `Stop` / rotation- and status-callbacks. The parser drains it,
///     splits into lines, and calls `AppendLine` once per record.
///
///   - `BytesAreStable()` is `false`: the parser must emit
///     `OwnedString` compact values rather than `MmapSlice` ones.
///
///   - `SupportsEviction()` is `true`. `EvictBefore(firstId)` drops
///     ids `[mFirstAvailableLineId, firstId)`. The `LogTable` /
///     `LogModel` retention path is the one caller in production;
///     tests may drive it directly.
///
///   - LineId convention: 1-based monotonic ids assigned by
///     `AppendLine`.
///
///   - Thread-safety: every `LineSource` virtual is callable from any
///     thread. The parser worker thread mutates the source via
///     `AppendLine`; the GUI thread reads via `RawLine` /
///     `ResolveOwnedBytes` and may evict via `EvictBefore`. Internal
///     state is mutex-protected; `std::deque` storage means
///     `AppendLine`'s push_back never invalidates references to
///     existing entries that other threads may already be reading.
///     Callers must not retain `string_view`s returned by
///     `ResolveOwnedBytes` past the next `EvictBefore` for the same
///     line id.
class StreamLineSource final : public LineSource
{
public:
    /// @param displayName  Human-facing identity for the GUI status bar
    ///                     and any code that consumes
    ///                     `LineSource::Path()`. Typically the file
    ///                     path's string for tail-of-file producers;
    ///                     synthetic for non-filesystem sources.
    /// @param producer     The byte producer for this stream. May be
    ///                     null when the source is constructed for
    ///                     pure-test scenarios that drive `AppendLine`
    ///                     directly.
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

    /// Borrow the byte producer. Returns `nullptr` if the source was
    /// constructed without one. Used by the parser to drain bytes;
    /// ownership stays with the source.
    [[nodiscard]] BytesProducer *Producer() noexcept;
    [[nodiscard]] const BytesProducer *Producer() const noexcept;

    /// Append @p rawLine and its accompanying escape-decoded byte
    /// arena to the source. Returns the assigned 1-based monotonic
    /// `lineId`. The arena is opaque to the source — `OwnedString`
    /// compact values produced by the parser carry `(offset, length)`
    /// pairs into it, resolved later via `ResolveOwnedBytes`.
    ///
    /// `ownedBytes` may be empty when the line had no escape-decoded
    /// fields.
    size_t AppendLine(std::string rawLine, std::string ownedBytes);

    /// Number of lines currently held (post-eviction). Equals
    /// `mNextLineId - mFirstAvailableLineId`.
    [[nodiscard]] size_t Size() const noexcept;

    /// Total bytes owned by this source: line bytes + per-line owned
    /// arenas, capacity-accurate. Used by the memory-footprint
    /// benchmark; not part of the parse hot path.
    [[nodiscard]] size_t OwnedMemoryBytes() const noexcept;

private:
    [[nodiscard]] bool LineIsLiveLocked(size_t lineId) const noexcept;
    [[nodiscard]] size_t IndexForLocked(size_t lineId) const noexcept;

    std::filesystem::path mDisplayName;
    std::unique_ptr<BytesProducer> mProducer;

    /// Guards every member below it. Acquired by every `LineSource`
    /// virtual and by `AppendLine` / `EvictBefore`. Held only while
    /// reading or mutating the deques; callers receive copies (raw
    /// line) or `string_view`s that remain valid as long as the deque
    /// entry hasn't been evicted.
    mutable std::mutex mLock;

    /// Raw line text (no trailing `\n` / `\r`). `std::deque` push_back
    /// is reference-stable, so a reader that already obtained a
    /// `string_view` into a published entry is unaffected by concurrent
    /// `AppendLine`s on later entries.
    std::deque<std::string> mLines;

    /// Per-line escape-decoded byte arena. `OwnedString` compact
    /// values index into the entry whose deque index matches that of
    /// `mLines`.
    std::deque<std::string> mLineOwnedBytes;

    /// 1-based id of the first line still held in `mLines` /
    /// `mLineOwnedBytes`.
    size_t mFirstAvailableLineId = 1;

    /// 1-based id that the next `AppendLine` will assign.
    size_t mNextLineId = 1;
};

} // namespace loglib
