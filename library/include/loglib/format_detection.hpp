#pragma once

#include "log_configuration.hpp"
#include "log_parser.hpp"

#include <filesystem>
#include <memory>
#include <optional>
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
/// accepts it, mirroring `TryDetectFormatFromBytes` but folding the
/// "no probe matched" case into a defaulted `{Format::Json, ""}`
/// so callers that always need *some* parser (the streaming loop,
/// `ParseFile(path)`) can chain without an `.value_or()`.
///
/// Prefer `TryDetectFormatFromBytes` when the caller can act on
/// "not yet confident" (e.g. keep peeking on a slow network
/// stream). This overload cannot distinguish "JSON was actually
/// claimed" from "nothing claimed, defaulting to JSON".
///
/// Every input surface funnels through this function:
///   - Static file opens (`MainWindow::DetectFormatForPath`)
///   - Live-tail file opens (`MainWindow::OpenLogStreamFromPath`)
///   - Stdin (`MainWindow::OpenStdinStream`)
///   - Network stream auto-detect (`AutoDetectParser`)
/// so "same bytes → same format" holds across every input path.
[[nodiscard]] DetectedFormat DetectFormatFromBytes(std::string_view sniffBuffer);

/// Like `DetectFormatFromBytes`, but returns `std::nullopt` when
/// no probe claimed the buffer (empty input, or content that no
/// parser recognises). Used by `AutoDetectParser` to short-circuit
/// the peek loop as soon as a definitive verdict is available:
/// `DetectFormatFromBytes` would report the fallback `Json` in
/// that case, which is indistinguishable from a real JSON hit and
/// would prematurely stop peeking on a bursty non-JSON producer
/// whose first few bytes hadn't yet formed a recognisable line.
///
/// The probe order matches `DetectFormatFromBytes` exactly: JSON,
/// logfmt, regex templates, CSV.
[[nodiscard]] std::optional<DetectedFormat> TryDetectFormatFromBytes(std::string_view sniffBuffer);

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
