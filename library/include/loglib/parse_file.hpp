#pragma once

#include "log_parser.hpp"

#include <filesystem>

namespace loglib
{

/// Synchronous, buffered "parse a file to a `ParseResult`" helper.
///
/// Drives @p parser's `ParseStreaming(FileLineSource&, sink, ...)`
/// virtual into an internal `BufferingSink`, materialising the full
/// parse result in memory. Used by tests, the `log_generator` tool,
/// and any caller that genuinely wants a one-shot `LogData` without
/// dealing with the streaming / GUI machinery.
///
/// Throws `std::runtime_error` if @p file does not exist or is empty.
///
/// Production GUI code does **not** use this helper -- the static-
/// file open path runs through `LogModel::BeginStreaming(FileLineSource,
/// ...)` so it shares the live-tail's incremental update model.
[[nodiscard]] ParseResult ParseFile(const LogParser &parser, const std::filesystem::path &file);

/// Auto-detecting variant: probes every parser registered through
/// `LogFactory::Create` (in declaration order) via its `IsValid` content
/// sniff, then runs the first match through `ParseFile(parser, file)`
/// above. Throws `std::runtime_error` if no parser claims the file.
[[nodiscard]] ParseResult ParseFile(const std::filesystem::path &file);

} // namespace loglib
