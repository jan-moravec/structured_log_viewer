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
    /// LogfmtParser multi-line handling: when `true`, an indented
    /// continuation line (first byte is ` ` or `\t`) is appended to
    /// the prior record's last field rather than emitted as its own
    /// row of null-valued bare keys. Unindented lines behave
    /// exactly as before (the shipped "permissive prose" contract
    /// is preserved). Default `true` so shipped Logfmt files with
    /// interleaved Go/Python/Java stack dumps parse cleanly out of
    /// the box; flip to `false` in tests that need the pre-feature
    /// row semantics.
    ///
    /// No effect on JSON / CSV / Regex parsers.
    bool multilineLogfmt = true;
};

} // namespace loglib
