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
    /// Takes ownership of @p file for the source's lifetime. Throws
    /// `std::invalid_argument` if @p file is null. Ownership can be
    /// transferred downstream (typically into `LogData::mSources`) via
    /// `ReleaseFile`; after release the source still resolves bytes
    /// against the released `LogFile` for as long as the new owner
    /// keeps it alive.
    explicit FileLineSource(std::unique_ptr<LogFile> file);

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
    /// caller. After the call this source still resolves bytes against
    /// the released file (the caller is responsible for keeping it alive
    /// at least as long as any `LogLine` referring back through this
    /// source) — `File()` continues to return a non-null reference. The
    /// intended use is the static-path take-data transition where the
    /// file migrates into `LogData::mSources` and this source is then
    /// destroyed.
    [[nodiscard]] std::unique_ptr<LogFile> ReleaseFile() noexcept;

private:
    std::unique_ptr<LogFile> mOwnedFile;
    LogFile *mFile = nullptr;
};

} // namespace loglib
