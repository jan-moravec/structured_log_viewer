/**
 * @brief Synchronous helpers that parse a whole file.
 *
 * ParseFile runs a parser over a file and returns all rows and errors at once, either with a given parser or with the
 * one that format detection chooses. It suits tests, tools and scripts. Interactive code uses the streaming path
 * instead.
 *
 * This is part of the parse module.
 */
#pragma once

#include "loglib/parse/log_parser.hpp"

#include <filesystem>

namespace loglib
{

/**
 * @brief Synchronous one-shot file parse. Drives @p parser into an internal
 * BufferingSink and materialises the full ParseResult. Used by
 * tests, log_generator, and any caller wanting LogData without
 * the streaming machinery. Interactive code instead feeds a LogTable
 * through LogTable::BeginStreaming.
 *
 * Throws std::runtime_error if @p file does not exist or is empty.
 */
[[nodiscard]] ParseResult ParseFile(const LogParser &parser, const std::filesystem::path &file);

/**
 * @brief Auto-detecting variant: probes the formats listed in
 * DetectFormatForPath and runs the first match.
 * Throws std::runtime_error if no parser claims the file.
 */
[[nodiscard]] ParseResult ParseFile(const std::filesystem::path &file);

} // namespace loglib
