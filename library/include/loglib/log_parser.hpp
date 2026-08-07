#pragma once

#include "log_data.hpp"
#include "log_line.hpp"
#include "parser_options.hpp"

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace loglib
{

class FileLineSource;
class LogParseSink;
class StreamLineSource;

/// Buffered output of a synchronous parse.
struct ParseResult
{
    LogData data;
    std::vector<std::string> errors;
};

/// Shared cap on bytes scanned by every parser's `IsValid` probe
/// (JSON, logfmt, CSV, regex). Also the head-read size used by the
/// non-virtual `IsValid(path)` shim on `LogParser`, and the peek
/// budget used by `AutoDetectParser` for network streams and by
/// `MainWindow::OpenStdinStream` for pipe input. Keeping every
/// probe on the same budget guarantees "same bytes -> same format"
/// across static files, live-tail files, stdin, and network
/// auto-detect.
inline constexpr std::size_t PROBE_BYTES_BUDGET = 16 * 1024;

/// Base class for log-format parsers. New formats implement the
/// byte-buffer `IsValid(std::string_view)`, both `ParseStreaming`
/// overloads, and `ToString`. The file-based `IsValid` is a
/// non-virtual shim that reads up to `PROBE_BYTES_BUDGET` bytes
/// and forwards to the byte-buffer overload -- so the probe logic
/// lives in exactly one place per parser and can be reused
/// verbatim for stdin / network auto-detect.
///
/// The synchronous "parse a file" helper is `loglib::ParseFile`
/// (see `parse_file.hpp`); production GUI code uses
/// `ParseStreaming` directly.
class LogParser
{
public:
    virtual ~LogParser() = default;

    /// Byte-buffer probe: does @p sniffBuffer look like this
    /// parser's format? Implementations must not scan past the end
    /// of @p sniffBuffer, and should treat a partial trailing line
    /// (no terminating `\n`) the same as a complete line -- the
    /// buffer may have been truncated by the caller's probe budget.
    /// Called from `IsValid(path)` below (via `ReadProbeHead`),
    /// from `AutoDetectParser`, and from the app-level
    /// `DetectFormatFromBytes` iterator. Named distinctly from
    /// `IsValid` so calls to the file shim with a `std::string`
    /// path never resolve to the byte-buffer overload by accident.
    virtual bool IsValidBytes(std::string_view sniffBuffer) const = 0;

    /// Read up to `PROBE_BYTES_BUDGET` bytes from @p file into a
    /// buffer and forward to `IsValidBytes`. Missing / unreadable
    /// files return `false`. Not virtual: every parser's probe
    /// logic lives in `IsValidBytes`.
    bool IsValid(const std::filesystem::path &file) const;

    /// Static-file streaming entry. Emitted `LogLine`s carry @p source
    /// and the line's 0-based file id.
    virtual void ParseStreaming(FileLineSource &source, LogParseSink &sink, ParserOptions options = {}) const = 0;

    /// Live-tail streaming entry. Emitted `LogLine`s carry @p source
    /// and the 1-based monotonic id assigned by `AppendLine`.
    virtual void ParseStreaming(StreamLineSource &source, LogParseSink &sink, ParserOptions options = {}) const = 0;

    /// Renders a parsed line back to the parser's native text form.
    virtual std::string ToString(const LogLine &line) const = 0;
};

/// Read at most @p budget bytes from the head of @p file into a
/// `std::string`. Empty result on missing / unreadable files.
/// Shared helper for `LogParser::IsValid(path)` and
/// `DetectRegexTemplate(path)`.
[[nodiscard]] std::string ReadProbeHead(const std::filesystem::path &file, std::size_t budget = PROBE_BYTES_BUDGET);

} // namespace loglib
