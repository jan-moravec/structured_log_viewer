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
    /// When enabled, Logfmt treats space- or tab-prefixed lines as
    /// continuations of the preceding record's last source-order field.
    /// Has no effect on other parsers.
    bool multilineLogfmt = true;

};

} // namespace loglib
