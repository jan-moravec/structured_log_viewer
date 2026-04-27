#pragma once

#include "log_configuration.hpp"

#include <memory>
#include <stop_token>

namespace loglib
{

/**
 * @brief Public option bundle accepted by `LogParser::ParseStreaming`.
 *
 * Holds only the two fields an external consumer needs:
 *   - `stopToken`: cooperative cancellation. The parser polls this in Stage A
 *     and drains the in-flight pipeline when stop has been requested.
 *   - `configuration`: optional `LogConfiguration` describing the dataset.
 *     When non-null, Stage B promotes any `Type::time` columns inline so the
 *     downstream `LogTable::Update` can skip the redundant whole-data
 *     timestamp pass.
 *
 * Tuning knobs (thread count, batch size, ntokens, scratch caches, telemetry)
 * live on `loglib::internal::AdvancedParserOptions`, behind
 * `<loglib/internal/parser_options.hpp>`. A default-constructed
 * `ParserOptions{}` paired with default-constructed `AdvancedParserOptions{}`
 * reproduces the legacy synchronous `Parse(path)` behaviour.
 */
struct ParserOptions
{
    std::stop_token stopToken{};
    std::shared_ptr<const LogConfiguration> configuration;
};

} // namespace loglib
