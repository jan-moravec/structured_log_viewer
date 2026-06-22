#include "loglib/regex_templates.hpp"

#include <algorithm>
#include <string_view>

namespace loglib
{

namespace
{

/// Build the registry once. We intentionally keep the list small (six
/// formats) — `IsValid` probes them in order against every file the
/// previous parsers rejected, so each extra entry costs auto-detect
/// time on unrecognised inputs.
///
/// Patterns are PCRE2 (anchored `^...$` for scan speed). Named groups
/// become column keys. Group names follow lnav / grok conventions
/// where applicable so a user pivoting from those tools sees familiar
/// column headers.
///
/// Source attribution: patterns are adapted (or, where the format is
/// trivial, written from scratch) by cross-referencing lnav's
/// `src/formats/*.json` (BSD-2-Clause) and the logstash core grok
/// patterns (`logstash-plugins/logstash-patterns-core`, Apache-2.0).
/// See per-template comments below.
const std::vector<RegexTemplate> &Registry()
{
    static const std::vector<RegexTemplate> TEMPLATES = {
        // Adapted from lnav's `src/formats/syslog_log.json` `std`
        // pattern (BSD-2-Clause), simplified to the common
        // `program[pid]: msg` shape.
        RegexTemplate{
            .name = "Syslog (RFC3164)",
            .pattern =
                R"(^(?<timestamp>[A-Z][a-z]{2}\s+\d{1,2}\s+\d{2}:\d{2}:\d{2}))"
                R"(\s+(?<hostname>\S+))"
                R"(\s+(?<program>[^\s\[:]+))"
                R"((?:\[(?<pid>\d+)\])?:\s+(?<message>.*)$)",
            .sampleLines =
                {
                    "Apr 28 04:02:03 host-a systemd: System starting",
                    "Jun 27 01:47:20 host-b configd[17]: network changed",
                    "Jan  4 10:23:26 host-c CRON[1234]: (root) CMD (test -e /tmp)",
                },
        },
        // Adapted from lnav's `src/formats/syslog_log.json` `rfc5424`
        // pattern (BSD-2-Clause). `structured` accepts either `-`
        // (none) or one `[...]` block with embedded quoted strings.
        RegexTemplate{
            .name = "Syslog (RFC5424)",
            .pattern = R"(^<(?<priority>\d+)>(?<version>\d+))"
                       R"(\s+(?<timestamp>\S+))"
                       R"(\s+(?<hostname>\S+))"
                       R"(\s+(?<appname>\S+))"
                       R"(\s+(?<procid>\S+))"
                       R"(\s+(?<msgid>\S+))"
                       R"(\s+(?<structured>\[(?:[^\]"]|"(?:\\.|[^"])*")*\]|-))"
                       R"(\s*(?<message>.*)$)",
            .sampleLines =
                {
                    R"(<46>1 2017-04-27T07:50:47.381967+02:00 logserver rsyslogd - - [origin software="rsyslogd"] start)",
                    "<30>1 2017-04-27T07:59:12+02:00 host dhclient - - - DHCPREQUEST on eth0",
                    "<78>1 2017-04-27T08:09:01+02:00 host CRON 1472 - - (root) CMD (test)",
                },
        },
        // Adapted from grok `COMMONAPACHELOG` (Apache-2.0); flattened
        // by inlining `IPORHOST`/`USER`/`NUMBER` macros into plain
        // PCRE2 character classes. Matches Apache / nginx CLF.
        RegexTemplate{
            .name = "Apache/nginx Common Log Format",
            // Custom raw-string delimiter `x(...)x` because the
            // PCRE2 source contains `)"` literals (HTTP request line
            // closer) that would otherwise terminate the default
            // `R"(...)"` raw string.
            .pattern = R"x(^(?<clientip>\S+))x"
                       R"x(\s+(?<ident>\S+))x"
                       R"x(\s+(?<auth>\S+))x"
                       R"x(\s+\[(?<timestamp>[^\]]+)\])x"
                       R"x(\s+"(?:(?<verb>\S+)\s+(?<request>\S+)(?:\s+HTTP/(?<httpversion>\S+))?|-)")x"
                       R"x(\s+(?<response>\d+))x"
                       R"x(\s+(?<bytes>\d+|-)$)x",
            .sampleLines =
                {
                    R"(127.0.0.1 - frank [10/Oct/2000:13:55:36 -0700] "GET /apache_pb.gif HTTP/1.0" 200 2326)",
                    R"(10.1.10.51 - - [23/Dec/2014:21:20:35 +0000] "POST /api/1/rest/foo HTTP/1.1" 200 -)",
                    R"(::1 - - [01/Jan/2024:00:00:00 +0000] "HEAD / HTTP/2.0" 304 0)",
                },
        },
        // Adapted from grok `COMBINEDAPACHELOG` (Apache-2.0): common
        // log format plus the trailing referer + user-agent fields
        // that nginx and Apache emit by default.
        RegexTemplate{
            .name = "Apache/nginx Combined Log Format",
            // `R"x(...)x"` for the same reason as the common-log
            // template above.
            .pattern = R"x(^(?<clientip>\S+))x"
                       R"x(\s+(?<ident>\S+))x"
                       R"x(\s+(?<auth>\S+))x"
                       R"x(\s+\[(?<timestamp>[^\]]+)\])x"
                       R"x(\s+"(?:(?<verb>\S+)\s+(?<request>\S+)(?:\s+HTTP/(?<httpversion>\S+))?|-)")x"
                       R"x(\s+(?<response>\d+))x"
                       R"x(\s+(?<bytes>\d+|-))x"
                       R"x(\s+"(?<referrer>[^"]*)")x"
                       R"x(\s+"(?<agent>[^"]*)"$)x",
            .sampleLines =
                {
                    R"(10.112.72.172 - - [11/Feb/2013:06:43:36 +0000] "GET /client/ HTTP/1.1" 200 5778 "-" "Mozilla/5.0")",
                    R"(1.2.3.4 - bob [10/Feb/2012:16:41:07 -0500] "GET / HTTP/1.0" 200 368 "https://example.com" "curl/8.5.0")",
                },
        },
        // Written from scratch; cross-checked against lnav's
        // `src/formats/error_log.json` shape. Handles both the legacy
        // `[time] [level] [client x] msg` and the modern
        // `[time] [module:level] [pid N] [client x] msg` Apache 2.4
        // error log formats.
        RegexTemplate{
            .name = "Apache error log",
            .pattern = R"(^\[(?<timestamp>[^\]]+)\])"
                       R"(\s+\[(?<level>[^\]]+)\])"
                       R"((?:\s+\[pid\s+(?<pid>\d+)(?::tid\s+\d+)?\])?)"
                       R"((?:\s+\[client\s+(?<client>[^\]]+)\])?)"
                       R"(\s+(?<message>.*)$)",
            .sampleLines =
                {
                    "[Wed Oct 11 14:32:52 2000] [error] [client 127.0.0.1] File does not exist: /var/www/html/foo",
                    "[Tue Apr 19 16:38:38.290122 2011] [ssl:warn] [pid 1234] [client 127.0.0.1:50318] AH02032: log",
                    "[Mon Jan 01 00:00:00.000000 2024] [core:notice] [pid 9999] AH00094: Command line: 'httpd'",
                },
        },
        // Written from scratch from qlogexplorer's README example
        // (https://github.com/rafaelfassi/qlogexplorer) of a generic
        // `[LEVEL] timestamp thread message` log shape, translated to
        // PCRE2 syntax (originally `(?<name>...)` which PCRE2 also
        // accepts verbatim). Kept last because its anchor is loose
        // enough to catch ad-hoc app logs.
        RegexTemplate{
            .name = "Generic bracketed level",
            .pattern = R"(^\[(?<Level>\w+)\])"
                       R"(\s+(?<Timestamp>\S+))"
                       R"(\s+(?<Thread>\S+))"
                       R"(\s+(?<Message>.*)$)",
            .sampleLines =
                {
                    "[INFO] 2022-02-18T15:37:10.354 0xBF32 System starting",
                    "[WARNING] 2022-02-19T15:37:13.427 0xBF32 Not in UTC timezone",
                    "[ERROR] 2024-01-01T00:00:00.000 main Connection refused",
                },
        },
    };
    return TEMPLATES;
}

} // namespace

std::span<const RegexTemplate> BuiltinRegexTemplates() noexcept
{
    return Registry();
}

const RegexTemplate *FindBuiltinByPattern(std::string_view pattern) noexcept
{
    const auto &registry = Registry();
    const auto it = std::ranges::find_if(registry, [&](const RegexTemplate &t) { return t.pattern == pattern; });
    return it != registry.end() ? &*it : nullptr;
}

} // namespace loglib
