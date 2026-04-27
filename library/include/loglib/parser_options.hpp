#pragma once

#include "log_configuration.hpp"

#include <memory>
#include <stop_token>

namespace loglib
{

/// Public options for `LogParser::ParseStreaming`. Tuning knobs live on
/// `loglib::internal::AdvancedParserOptions`.
struct ParserOptions
{
    std::stop_token stopToken{};
    std::shared_ptr<const LogConfiguration> configuration;
};

} // namespace loglib
