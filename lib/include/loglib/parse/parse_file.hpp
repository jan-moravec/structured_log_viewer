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
 * @brief Parses a whole file with the given parser and returns everything it found.
 *
 * It is meant for tests, tools and anyone who wants the data without the streaming machinery. Interactive code feeds a
 * LogTable through LogTable::BeginStreaming instead. Throws std::runtime_error if the file does not exist or is empty.
 */
[[nodiscard]] ParseResult ParseFile(const LogParser &parser, const std::filesystem::path &file);

/**
 * @brief Detects the format of a file as DetectFormatForPath does and parses it with the matching parser.
 *
 * Throws std::runtime_error if the file cannot be read, is empty, or no parser recognizes it.
 */
[[nodiscard]] ParseResult ParseFile(const std::filesystem::path &file);

} // namespace loglib
