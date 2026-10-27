#pragma once

#include <log_generator/options.hpp>

namespace log_generator
{

/**
 * @brief Generates random records until a limit in @p options is reached.
 *
 * Each record carries a 0-based `line_number` that keeps counting across
 * rotations, so a reader can detect dropped lines. Progress messages go to
 * stdout, or to stderr when the records go to stdout, and are suppressed by
 * `GeneratorOptions::quiet`. A reader closing stdout ends the run normally.
 *
 * @param options Validated settings from `AddOptions()`.
 * @throws std::exception when the output cannot be opened or written.
 */
void Run(const GeneratorOptions &options);

} // namespace log_generator
