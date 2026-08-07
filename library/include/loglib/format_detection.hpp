#pragma once

#include "log_configuration.hpp"
#include "log_parser.hpp"

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

namespace loglib
{

/// Auto-detection result: the matched format and, for `Regex`, the
/// matched template's pattern (built-in or user). `regexPattern`
/// is empty for every other format and for content nothing claimed.
struct DetectedFormat
{
    LogConfiguration::Source::Format format = LogConfiguration::Source::Format::Json;
    std::string regexPattern;
};

/// Sniff @p sniffBuffer and return the first format whose parser
/// accepts it, matching `loglib::ParseFile(path)`'s order (JSON,
/// logfmt, CSV, Regex). For `Regex` we call
/// `DetectRegexTemplateFromBytes`, which walks the merged catalog
/// (built-ins ∪ user templates injected via
/// `SetExtraRegexTemplates`) in priority order; the matched
/// template's pattern is carried through so callers can persist
/// it on `LogConfiguration::Source::regexPattern`. Falls back to
/// `Json` when nothing matches so the parse surfaces the bytes as
/// errors instead of silently doing nothing.
///
/// Every input surface funnels through this function:
///   - Static file opens (`MainWindow::DetectFormatForPath`)
///   - Live-tail file opens (`MainWindow::OpenLogStreamFromPath`)
///   - Stdin (`MainWindow::OpenStdinStream`)
///   - Network stream auto-detect (`AutoDetectParser`)
/// so "same bytes → same format" holds across every input path.
[[nodiscard]] DetectedFormat DetectFormatFromBytes(std::string_view sniffBuffer);

/// File-based convenience wrapper. Reads up to `PROBE_BYTES_BUDGET`
/// bytes from @p file via `ReadProbeHead` and forwards to
/// `DetectFormatFromBytes`. Missing / unreadable files return
/// `{Format::Json, ""}` — the same fallback the byte-buffer
/// version produces on empty input.
[[nodiscard]] DetectedFormat DetectFormatForPath(const std::filesystem::path &file);

/// Construct a parser for @p format. For `Regex`, @p regexPattern
/// is pinned on the parser instance directly rather than relying
/// on `ParserOptions::configuration->source->regexPattern`. An
/// empty @p regexPattern with `Format::Regex` builds a parser
/// that surfaces a single "empty pattern" error through the sink.
///
/// Historically `MakeParserForFormat` lived in
/// `app/src/main_window.cpp`; hoisting it to the library lets
/// `AutoDetectParser` (below) build the resolved parser without
/// pulling in the app layer.
[[nodiscard]] std::unique_ptr<LogParser> MakeParserForFormat(
    LogConfiguration::Source::Format format, std::string_view regexPattern = {}
);

} // namespace loglib
