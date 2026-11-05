/**
 * @brief Line source backed by a memory-mapped file.
 *
 * A FileLineSource owns a LogFile. It resolves the offsets stored in rows against the mapped bytes, so rows from a
 * static file never copy their text. It is used when a file is opened as a whole, as opposed to followed live.
 *
 * The line id of a row is the zero-based physical line of the first line of its record, so it indexes the line offsets
 * of the file. A record that spans several lines has the id of its first line, and the other lines have no row of their
 * own. An id is not a count of lines or records, because blank lines, lines that failed to parse and continuation lines
 * take up physical lines too.
 *
 * This is part of the core module.
 */
#pragma once

#include "loglib/core/line_source.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

namespace loglib
{

class LogFile;

/**
 * @brief LineSource for a memory-mapped file that has a fixed size.
 *
 * Static parsing runs directly over the mapping, and the rows it produces resolve their MmapSlice and OwnedString cells
 * through this source. It owns the LogFile. It cannot evict lines.
 */
class FileLineSource final : public LineSource
{
public:
    /** @brief Takes ownership of file. Throws std::invalid_argument if file is null. */
    explicit FileLineSource(std::unique_ptr<LogFile> file);

    ~FileLineSource() override;

    FileLineSource(const FileLineSource &) = delete;
    FileLineSource &operator=(const FileLineSource &) = delete;

    FileLineSource(FileLineSource &&) = delete;
    FileLineSource &operator=(FileLineSource &&) = delete;

    [[nodiscard]] const std::filesystem::path &Path() const noexcept override;
    [[nodiscard]] std::string RawLine(size_t lineId) const override;

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

    /** @brief The file behind this source, for code that needs line offsets or the mapping itself. */
    [[nodiscard]] LogFile &File() noexcept;
    [[nodiscard]] const LogFile &File() const noexcept;

private:
    std::unique_ptr<LogFile> mFile;
};

} // namespace loglib
