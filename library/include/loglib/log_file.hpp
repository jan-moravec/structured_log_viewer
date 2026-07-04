#pragma once

#include <mio/mmap.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace loglib
{

/// Memory-mapped log file. Owns the mmap so `LogValue` instances can
/// hold `string_view`s into the content; move keeps the pointer stable.
/// Per-record addressing is `FileLineSource`'s job; `LogFile` only
/// holds the bytes and arenas.
class LogFile
{
public:
    /// Throws `std::runtime_error` if the file cannot be opened or mapped.
    explicit LogFile(std::filesystem::path filePath);

    LogFile(const LogFile &) = delete;
    LogFile &operator=(const LogFile &) = delete;

    LogFile(LogFile &&) noexcept = default;
    LogFile &operator=(LogFile &&) noexcept = default;

    const std::filesystem::path &GetPath() const;
    const char *Data() const;
    size_t Size() const;

    /// Trailing `'\r'` is trimmed. Throws `std::out_of_range` when out of range.
    std::string GetLine(size_t lineNumber) const;
    size_t GetLineCount() const;

    void ReserveLineOffsets(size_t count);

    /// Append a single line-boundary offset; @p position must be
    /// strictly greater than the previously registered one. Used by
    /// tests; the parser uses `AppendLineOffsets` for batched updates.
    void RegisterLineEnd(size_t position);

    /// Caller must ensure offsets are strictly increasing and start past the
    /// current last offset.
    void AppendLineOffsets(const std::vector<uint64_t> &offsets);

    /// Heap bytes owned by `mLineOffsets` (capacity, not size). Used by the
    /// memory-footprint benchmark; not part of the parse hot path.
    size_t LineOffsetsMemoryBytes() const noexcept;

    /// Sliding view over the owned-string arena (escape-decoded values
    /// that cannot live in the mmap). `LogLine` materialisation indexes
    /// into this via `(offset, length)` stored in its compact values.
    std::string_view OwnedStringsView() const noexcept;

    /// Append @p bytes to the owned-string arena and return the byte
    /// offset of the first appended byte. Single-threaded contract: the
    /// streaming pipeline serialises arena writes through Stage C.
    uint64_t AppendOwnedStrings(std::string_view bytes);

    /// Heap bytes owned by `mOwnedStrings` (capacity).
    size_t OwnedStringsMemoryBytes() const noexcept;

    /// Attach an arbitrary RAII object whose destructor will run
    /// **after** this `LogFile`'s mmap is unmapped. The intended use
    /// case is transparent decompression: the caller (`MainWindow`)
    /// creates a `DecompressingByteSource` that stages a temp file,
    /// then hands that shared_ptr in here. When the model drops the
    /// last reference to this `LogFile`, the mmap unmap runs first
    /// (member destruction order), then the anchor's dtor unlinks
    /// the temp file. That ordering matters on Windows where
    /// `std::filesystem::remove` fails while a mapping is open, and
    /// on POSIX where a stale mmap would silently outlive the file.
    ///
    /// Multiple calls chain: the newest anchor replaces the
    /// previous one, so callers who need to keep several objects
    /// alive should compose them (e.g. via a lambda capture list
    /// or a helper struct) before attaching.
    void AttachLifetimeAnchor(std::shared_ptr<void> anchor) noexcept;

private:
    std::filesystem::path mPath;

    /// Post-mmap RAII anchor; see `AttachLifetimeAnchor` for the
    /// destruction-order contract. Declared **before** `mMmap` so
    /// C++'s reverse-declaration destruction runs `mMmap`'s dtor
    /// first (unmap), then this anchor's (unlink temp file). Do
    /// not reorder without re-reading the doc comment above.
    std::shared_ptr<void> mLifetimeAnchor;

    mio::mmap_source mMmap;

    /// Byte offsets of every line boundary plus a one-past-the-last sentinel.
    std::vector<uint64_t> mLineOffsets;

    /// Concatenated escape-decoded strings referenced by this file's
    /// `LogLine` values via `(offset, length)`.
    std::string mOwnedStrings;
};

} // namespace loglib
