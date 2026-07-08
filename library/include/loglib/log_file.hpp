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

    /// Explicit `= default` spelling makes the rule-of-five
    /// explicit. Reverse-declaration-order member destruction
    /// (mmap first, then anchor) is exactly what upholds the
    /// contract on `mLifetimeAnchor`.
    ~LogFile() = default;

    LogFile(const LogFile &) = delete;
    LogFile &operator=(const LogFile &) = delete;

    LogFile(LogFile &&) noexcept = default;
    /// Deliberately deleted: the synthesised operator would assign
    /// members in declaration order (`mLifetimeAnchor` before
    /// `mMmap`), releasing the anchor -- and unlinking the temp
    /// file -- while the mmap still holds the mapping. On Windows
    /// `remove` silently fails then, leaking the temp. All callers
    /// hold `LogFile` via `unique_ptr`, so deletion is cheaper than
    /// a hand-rolled swap-and-destroy.
    LogFile &operator=(LogFile &&) noexcept = delete;

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

    /// Attach an RAII object whose destructor runs **after** this
    /// `LogFile`'s mmap is unmapped in `~LogFile`. Used by
    /// transparent decompression so the temp file only gets
    /// unlinked once the mapping is torn down (Windows silently
    /// fails `remove` while a mapping is open; POSIX would leave a
    /// stale mmap over a deleted file).
    ///
    /// Multiple attaches are supported and composed LIFO
    /// (last-attached destroys first); every anchor still runs
    /// after the mmap unmap. Move-assignment on `LogFile` is
    /// deleted because the synthesised operator would violate this
    /// ordering -- attach to the destination `LogFile` explicitly.
    void AttachLifetimeAnchor(std::shared_ptr<void> anchor) noexcept;

private:
    std::filesystem::path mPath;

    /// Post-mmap RAII anchor; see `AttachLifetimeAnchor`. Declared
    /// **before** `mMmap` so reverse-declaration destruction
    /// unmaps first, then releases the anchor. Do not reorder;
    /// move-assignment is deleted for the same reason.
    std::shared_ptr<void> mLifetimeAnchor;

    mio::mmap_source mMmap;

    /// Byte offsets of every line boundary plus a one-past-the-last sentinel.
    std::vector<uint64_t> mLineOffsets;

    /// Concatenated escape-decoded strings referenced by this file's
    /// `LogLine` values via `(offset, length)`.
    std::string mOwnedStrings;
};

} // namespace loglib
