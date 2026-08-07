#pragma once

#include "log_configuration.hpp"
#include "stop_token.hpp"

#include <memory>
#include <string>

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

    /// Bytes already consumed from the producer that must be
    /// reprocessed as the first input of the streaming loop. Used by
    /// `AutoDetectParser` (network-stream auto-detect) to hand the
    /// bytes it drained during format detection back to the resolved
    /// parser without swapping the producer. Non-empty only on the
    /// streaming (`StreamLineSource`) path; the static-file path
    /// ignores it. Empty by default; existing callers pay nothing.
    std::string initialCarry;
};

} // namespace loglib
