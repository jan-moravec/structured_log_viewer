#pragma once

#include "loglib/line_source.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

namespace loglib
{

class LogFile;

/// `LineSource` over a memory-mapped, finite `LogFile`. The static
/// `File → Open…` path runs through this source: the existing TBB
/// pipeline drives parsing directly over the mmap (preserving the
/// `[large]` / `[wide]` / `[allocations]` benchmark headroom), and the
/// resulting `LogLine`s reference this source for their `MmapSlice` /
/// `OwnedString` resolution.
///
/// Owns the underlying `LogFile` for the source's lifetime; transferable
/// to a downstream owner (`LogData`) via `ReleaseFile`.
///
/// LineId convention: 0-based file-line indices, matching
/// `LogFile::GetLine(size_t)`.
class FileLineSource final : public LineSource
{
public:
    /// Owning ctor: takes ownership of @p file for the source's
    /// lifetime. Throws `std::invalid_argument` if @p file is null.
    explicit FileLineSource(std::unique_ptr<LogFile> file);

    /// Borrowing ctor: caller must keep @p file alive for the
    /// source's lifetime. Used by the parser's `LogFile&` overloads —
    /// the `BufferingSink` and the `JsonParser::ParseStreaming(LogFile&,
    /// ...)` shim wrap a caller-owned `LogFile` in a stack-local
    /// borrowing source so all `LogLine`s end up tagged with a
    /// `LineSource *` at construction time. `ReleaseFile()` returns
    /// `nullptr` in this mode.
    explicit FileLineSource(LogFile &file) noexcept;

    ~FileLineSource() override;

    FileLineSource(const FileLineSource &) = delete;
    FileLineSource &operator=(const FileLineSource &) = delete;

    FileLineSource(FileLineSource &&) = delete;
    FileLineSource &operator=(FileLineSource &&) = delete;

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

    /// Direct access to the underlying `LogFile`. Used by the parser fast
    /// path (which drives the TBB pipeline against the mmap directly)
    /// and by any other code that legitimately needs file-level state
    /// (`AppendLineOffsets`, `AppendOwnedStrings`).
    [[nodiscard]] LogFile &File() noexcept;
    [[nodiscard]] const LogFile &File() const noexcept;

    /// Release the underlying `LogFile`, transferring ownership to the
    /// caller. After the call this source still functions for byte
    /// resolution against the released file (the caller is responsible
    /// for keeping it alive at least as long as any `LogLine` referring
    /// back through this source) — `File()` continues to return a
    /// non-null reference. The intended use is the `BufferingSink`
    /// take-data transition in the static path: the file moves into
    /// `LogData::mSources` while this source is destroyed.
    [[nodiscard]] std::unique_ptr<LogFile> ReleaseFile() noexcept;

private:
    std::unique_ptr<LogFile> mOwnedFile;
    LogFile *mFile = nullptr;
};

} // namespace loglib
