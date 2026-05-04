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

/// `LineSource` over a memory-mapped, finite `LogFile`. Used by the
/// static `File → Open…` path: the TBB pipeline parses directly over
/// the mmap, and emitted `LogLine`s use this source for `MmapSlice` /
/// `OwnedString` resolution.
///
/// Owns the `LogFile`; transferable downstream via `ReleaseFile`.
/// LineIds are 0-based file-line indices.
class FileLineSource final : public LineSource
{
public:
    /// Takes ownership of @p file. Throws `std::invalid_argument` if
    /// @p file is null. After `ReleaseFile`, this source keeps
    /// resolving bytes against the released file as long as the new
    /// owner keeps it alive.
    explicit FileLineSource(std::unique_ptr<LogFile> file);

    ~FileLineSource() override;

    FileLineSource(const FileLineSource &) = delete;
    FileLineSource &operator=(const FileLineSource &) = delete;

    FileLineSource(FileLineSource &&) = delete;
    FileLineSource &operator=(FileLineSource &&) = delete;

    [[nodiscard]] const std::filesystem::path &Path() const noexcept override;
    [[nodiscard]] std::string RawLine(size_t lineId) const override;

    [[nodiscard]] std::string_view ResolveMmapBytes(uint64_t offset, uint32_t length, size_t lineId)
        const noexcept override;

    [[nodiscard]] std::string_view ResolveOwnedBytes(uint64_t offset, uint32_t length, size_t lineId)
        const noexcept override;

    [[nodiscard]] std::span<const char> StableBytes() const noexcept override;

    uint64_t AppendOwnedBytes(size_t lineId, std::string_view bytes) override;

    [[nodiscard]] bool SupportsEviction() const noexcept override;

    void EvictBefore(size_t firstSurvivingLineId) override;
    [[nodiscard]] size_t FirstAvailableLineId() const noexcept override;

    /// Direct access to the `LogFile`. Used by the parser fast path
    /// and any code that needs file-level state.
    [[nodiscard]] LogFile &File() noexcept;
    [[nodiscard]] const LogFile &File() const noexcept;

    /// Transfer ownership of the underlying `LogFile` to the caller.
    /// `File()` continues to return a non-null reference; the caller
    /// must keep the file alive at least as long as any `LogLine`
    /// referring back through this source.
    [[nodiscard]] std::unique_ptr<LogFile> ReleaseFile() noexcept;

private:
    std::unique_ptr<LogFile> mOwnedFile;
    LogFile *mFile = nullptr;
};

} // namespace loglib
