#pragma once

#include "log_parser.hpp"

#include <filesystem>

namespace loglib
{

/// Synchronous one-shot file parse. Drives @p parser into an internal
/// `BufferingSink` and materialises the full `ParseResult`. Used by
/// tests, `log_generator`, and any caller wanting `LogData` without
/// the streaming/GUI machinery. Production GUI code instead routes
/// through `LogModel::BeginStreaming(FileLineSource, ...)`.
///
/// Throws `std::runtime_error` if @p file does not exist or is empty.
[[nodiscard]] ParseResult ParseFile(const LogParser &parser, const std::filesystem::path &file);

/// Auto-detecting variant: probes parsers registered with
/// `LogFactory::Create` via `IsValid` and runs the first match.
/// Throws `std::runtime_error` if no parser claims the file.
[[nodiscard]] ParseResult ParseFile(const std::filesystem::path &file);

} // namespace loglib
