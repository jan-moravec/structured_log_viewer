#pragma once

#include "log_configuration.hpp"
#include "stop_token.hpp"

#include <memory>

namespace loglib
{

/// Public options for `LogParser::ParseStreaming`. Tuning knobs live on
/// `loglib::internal::AdvancedParserOptions`.
struct ParserOptions
{
    StopToken stopToken{};
    std::shared_ptr<const LogConfiguration> configuration;
};

} // namespace loglib
