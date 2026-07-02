#include <test_common/log_format.hpp>

#include <glaze/glaze.hpp>

#include <algorithm>
#include <array>
#include <cassert>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <random>
#include <string>
#include <string_view>

namespace test_common
{

namespace
{

// True when @p value has no byte that would end a logfmt token or need
// escaping. Mirrors `loglib::BareValueIsSafe`; the `[logfmt_parser][round_trip]`
// tests guard against drift between the two copies.
bool BareValueIsSafe(std::string_view value)
{
    if (value.empty())
    {
        return false;
    }
    return std::ranges::all_of(value, [](char c) {
        const auto uc = static_cast<unsigned char>(c);
        return uc > ' ' && uc != '"' && uc != '=' && uc != '\\';
    });
}

// Append @p value as a double-quoted logfmt string with C-style escapes.
// Mirrors `loglib::AppendQuotedString`.
void AppendQuotedString(std::string &out, std::string_view value)
{
    out.push_back('"');
    for (const char c : value)
    {
        switch (c)
        {
        case '"':
            out.append("\\\"");
            break;
        case '\\':
            out.append("\\\\");
            break;
        case '\n':
            out.append("\\n");
            break;
        case '\r':
            out.append("\\r");
            break;
        case '\t':
            out.append("\\t");
            break;
        default:
            out.push_back(c);
            break;
        }
    }
    out.push_back('"');
}

std::string CompactJson(const LogRecord &value)
{
    return glz::write_json(value).value_or("");
}

void AppendLogfmtValue(std::string &out, const LogRecord &value)
{
    if (value.is_null())
    {
        // Null serializes as `key=` (empty value).
        return;
    }
    if (value.is_string())
    {
        const std::string &text = value.get_string();
        if (BareValueIsSafe(text))
        {
            out.append(text);
        }
        else
        {
            AppendQuotedString(out, text);
        }
        return;
    }
    if (value.is_boolean())
    {
        out.append(value.get_boolean() ? "true" : "false");
        return;
    }
    if (value.is_number())
    {
        // Numbers share the bare JSON token form.
        out.append(CompactJson(value));
        return;
    }
    // Array/object: embed compact JSON as a single quoted string so the
    // per-line field count still matches the JSON serialization.
    AppendQuotedString(out, CompactJson(value));
}

} // namespace

LogFormat JsonLines()
{
    return LogFormat{
        .suggestedExtension = ".jsonl",
        .writeHeader = [](const RecordSchema &) { return std::string{}; },
        .writeLine = [](const LogRecord &record) { return CompactJson(record); },
    };
}

LogFormat Logfmt()
{
    return LogFormat{
        .suggestedExtension = ".logfmt",
        .writeHeader = [](const RecordSchema &) { return std::string{}; },
        .writeLine =
            [](const LogRecord &record) {
                if (!record.is_object())
                {
                    // No key=value structure; fall back to compact JSON.
                    return CompactJson(record);
                }
                std::string out;
                bool first = true;
                for (const auto &[key, value] : record.get_object())
                {
                    if (!first)
                    {
                        out.push_back(' ');
                    }
                    first = false;
                    // logfmt has no key-quoting syntax, so keys must be
                    // bare-safe. Catches hand-crafted records in debug builds.
                    assert(BareValueIsSafe(key) && "logfmt keys must be bare-safe");
                    out.append(key);
                    out.push_back('=');
                    AppendLogfmtValue(out, value);
                }
                return out;
            },
    };
}

namespace
{

// Duplicate of `loglib::BareCellIsSafe` so `test_common` stays
// loglib-free; drift is caught by `[csv_parser][round_trip]` tests.
bool CsvCellIsBareSafe(std::string_view value) noexcept
{
    return std::ranges::all_of(value, [](char c) { return c != ',' && c != '"' && c != '\r' && c != '\n'; });
}

void AppendCsvQuoted(std::string &out, std::string_view value)
{
    out.push_back('"');
    for (const char c : value)
    {
        if (c == '"')
        {
            out.append("\"\"");
        }
        else
        {
            out.push_back(c);
        }
    }
    out.push_back('"');
}

void AppendCsvCell(std::string &out, std::string_view value)
{
    if (CsvCellIsBareSafe(value))
    {
        out.append(value.data(), value.size());
    }
    else
    {
        AppendCsvQuoted(out, value);
    }
}

void AppendCsvValue(std::string &out, const LogRecord &value)
{
    if (value.is_null())
    {
        // Empty cell -> `CsvParser` omits it from the row.
        return;
    }
    if (value.is_string())
    {
        AppendCsvCell(out, value.get_string());
        return;
    }
    if (value.is_boolean())
    {
        out.append(value.get_boolean() ? "true" : "false");
        return;
    }
    if (value.is_number())
    {
        out.append(CompactJson(value));
        return;
    }
    // Array / object: embed compact JSON in one quoted cell so wide-row
    // column counts match the JSON serialisation (lossy escape hatch).
    AppendCsvQuoted(out, CompactJson(value));
}

// Returns a pointer to @p key inside @p record, or nullptr if absent
// or @p record is not an object.
const LogRecord *FindObjectField(const LogRecord &record, const std::string &key)
{
    if (!record.is_object())
    {
        return nullptr;
    }
    for (const auto &[k, v] : record.get_object())
    {
        if (k == key)
        {
            return &v;
        }
    }
    return nullptr;
}

} // namespace

LogFormat Csv(RecordSchema schema)
{
    return LogFormat{
        .suggestedExtension = ".csv",
        .writeHeader =
            [capturedSchema = schema](const RecordSchema &paramSchema) {
                // Prefer the caller-supplied schema; fall back to the
                // captured one for factories pre-wired with a schema.
                const RecordSchema &effective = paramSchema.empty() ? capturedSchema : paramSchema;
                if (effective.empty())
                {
                    return std::string{};
                }
                std::string out;
                bool first = true;
                for (const auto &name : effective)
                {
                    if (!first)
                    {
                        out.push_back(',');
                    }
                    first = false;
                    AppendCsvCell(out, name);
                }
                return out;
            },
        .writeLine =
            [capturedSchema = std::move(schema)](const LogRecord &record) {
                if (capturedSchema.empty())
                {
                    // Headerless: walk the record's lex order. Rejected
                    // by `CsvParser::IsValid`; real fixtures pass a schema.
                    if (!record.is_object())
                    {
                        return CompactJson(record);
                    }
                    std::string out;
                    bool first = true;
                    for (const auto &[_key, value] : record.get_object())
                    {
                        if (!first)
                        {
                            out.push_back(',');
                        }
                        first = false;
                        AppendCsvValue(out, value);
                    }
                    return out;
                }
                std::string out;
                bool first = true;
                for (const auto &name : capturedSchema)
                {
                    if (!first)
                    {
                        out.push_back(',');
                    }
                    first = false;
                    if (const auto *value = FindObjectField(record, name); value != nullptr)
                    {
                        AppendCsvValue(out, *value);
                    }
                    // Missing key -> empty cell (delimiter handled above).
                }
                return out;
            },
    };
}

namespace
{

// Parse the pinned ISO-8601 timestamp shape from
// `test_common::GenerateRandomLogRecord` (`date::format("%FT%T", ...)`,
// e.g. `2026-01-01T00:00:00.001`). Fixed-width so a manual scan
// avoids the `istringstream` + `date::parse` allocation cost on
// the per-line hot path.
struct IsoTimestamp
{
    int year = 1970;
    unsigned month = 1;
    unsigned day = 1;
    int hour = 0;
    int minute = 0;
    int second = 0;
    int millisecond = 0;
    bool hasMillis = false;
};

constexpr int DigitAt(std::string_view s, std::size_t i) noexcept
{
    return static_cast<int>(s[i]) - static_cast<int>('0');
}

IsoTimestamp ParseIsoTimestamp(std::string_view iso)
{
    IsoTimestamp t;
    // Field positions assume the pinned "YYYY-MM-DDTHH:MM:SS[.mmm]" shape.
    // Records missing the timestamp fall back to epoch defaults so the
    // synthesizer never produces malformed output.
    if (iso.size() < 19)
    {
        return t;
    }
    t.year = (DigitAt(iso, 0) * 1000) + (DigitAt(iso, 1) * 100) + (DigitAt(iso, 2) * 10) + DigitAt(iso, 3);
    t.month = static_cast<unsigned>((DigitAt(iso, 5) * 10) + DigitAt(iso, 6));
    t.day = static_cast<unsigned>((DigitAt(iso, 8) * 10) + DigitAt(iso, 9));
    t.hour = (DigitAt(iso, 11) * 10) + DigitAt(iso, 12);
    t.minute = (DigitAt(iso, 14) * 10) + DigitAt(iso, 15);
    t.second = (DigitAt(iso, 17) * 10) + DigitAt(iso, 18);
    if (iso.size() >= 23 && (iso[19] == '.' || iso[19] == ','))
    {
        t.millisecond = (DigitAt(iso, 20) * 100) + (DigitAt(iso, 21) * 10) + DigitAt(iso, 22);
        t.hasMillis = true;
    }
    return t;
}

constexpr std::array<std::string_view, 12> MONTH_ABBREV = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
};
constexpr std::array<std::string_view, 7> WEEKDAY_ABBREV = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};

// Sakamoto's day-of-week algorithm: returns 0=Sun..6=Sat for a
// proleptic Gregorian date. Avoids pulling `date::sys_days` on
// the hot path for the two templates that need a weekday name.
unsigned WeekdayIndex(int y, unsigned m, unsigned d) noexcept
{
    static constexpr std::array<int, 12> T = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    const int year = m < 3 ? y - 1 : y;
    // NOLINTNEXTLINE(readability-magic-numbers)
    const int idx = (year + (year / 4) - (year / 100) + (year / 400) + T[m - 1] + static_cast<int>(d)) % 7;
    return static_cast<unsigned>(idx);
}

// Zero-pad @p value to @p width digits (@p width in [1, 4]).
// Values wider than @p width are truncated to the low digits;
// callers pass a matching width for their field.
void AppendPadded(std::string &out, int value, std::size_t width)
{
    std::array<char, 8> buf{};
    for (std::size_t i = 0; i < width; ++i)
    {
        buf[width - 1 - i] = static_cast<char>('0' + (value % 10));
        value /= 10;
    }
    out.append(buf.data(), width);
}

// Append @p value as unpadded decimal digits ("1", "42",
// "65535"). Used for variable-width numeric fields (pid, response
// bytes) where zero padding would falsify the shape. The 16-byte
// buffer exceeds the decimal expansion of any `unsigned`, so
// `to_chars` cannot fail.
void AppendUInt(std::string &out, unsigned value)
{
    std::array<char, 16> buf{};
    const auto res = std::to_chars(buf.data(), buf.data() + buf.size(), value);
    out.append(buf.data(), static_cast<std::size_t>(res.ptr - buf.data()));
}

// `d` padded to width 2 with a leading space (BSD syslog: " 4", "12").
void AppendSpacePadded2(std::string &out, unsigned value)
{
    if (value < 10)
    {
        out.push_back(' ');
        out.push_back(static_cast<char>('0' + value));
    }
    else
    {
        AppendPadded(out, static_cast<int>(value), 2);
    }
}

// Read a scalar object field as `string_view`. Returns @p fallback
// for missing / non-string values so the synthesizer's output
// stays well-formed even for minimal records (e.g. no `message`).
std::string_view FieldOr(const LogRecord &record, const std::string &key, std::string_view fallback)
{
    const LogRecord *value = FindObjectField(record, key);
    if (value == nullptr || !value->is_string())
    {
        return fallback;
    }
    return value->get_string();
}

// Read the record's `line_number` if present. Seeds the per-line
// RNG so synthesized extras stay deterministic under a pinned
// generator seed. Falls back to @p fallback when absent (fixtures
// that skip line-number injection). Uses `as<uint64_t>()` rather
// than `get_number()` so callers storing the value as `int64_t`
// (as `GenerateRandomLogRecord` does) don't trip glaze's "cannot
// get reference to double" abort.
std::uint64_t LineNumberOr(const LogRecord &record, std::uint64_t fallback)
{
    const LogRecord *value = FindObjectField(record, "line_number");
    if (value == nullptr || !value->is_number())
    {
        return fallback;
    }
    return value->as<std::uint64_t>();
}

// Deterministic per-line PRNG. `minstd_rand` is cheap and keeps
// the per-record synthesizer cost near-zero; we only need enough
// entropy to spread pool picks across a large fixture.
using LineRng = std::minstd_rand;

// SplitMix64-style hash so consecutive `lineNumber`s land on
// distant PRNG trajectories. Necessary because `minstd_rand`'s
// first output correlates strongly with its seed — near-identical
// seeds would pick the same pool entry on the first `Pick(...)`.
LineRng MakeLineRng(std::uint64_t lineNumber) noexcept
{
    std::uint64_t x = lineNumber + 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27U)) * 0x94D049BB133111EBULL;
    x = x ^ (x >> 31U);
    const auto seed = static_cast<std::uint32_t>(x);
    // `minstd_rand` disallows a zero seed.
    return LineRng{seed == 0 ? 0x9E3779B9U : seed};
}

template <std::size_t N> std::string_view Pick(const std::array<std::string_view, N> &pool, LineRng &rng)
{
    return pool[std::uniform_int_distribution<std::size_t>{0, N - 1}(rng)];
}

// Static pools shared across the Apache / syslog synthesizers.
// Small on purpose — each pool only needs enough entropy to
// spread bytes across a large fixture without dominating the
// per-line cost.
constexpr std::array<std::string_view, 6> SYSLOG_HOSTS = {"host-a", "host-b", "host-c", "web01", "db02", "edge03"};
constexpr std::array<std::string_view, 6> SYSLOG_PROGRAMS = {"systemd", "sshd", "kernel", "cron", "networkd", "auditd"};

constexpr std::array<std::string_view, 6> APACHE_CLIENT_IPS = {
    "127.0.0.1", "10.0.0.5", "10.1.10.51", "192.168.1.42", "203.0.113.7", "198.51.100.9"
};
constexpr std::array<std::string_view, 4> APACHE_AUTH_USERS = {"-", "-", "frank", "bob"};
constexpr std::array<std::string_view, 5> APACHE_VERBS = {"GET", "POST", "PUT", "HEAD", "DELETE"};
constexpr std::array<std::string_view, 6> APACHE_PATHS = {
    "/",
    "/index.html",
    "/api/v1/users",
    "/static/app.css",
    "/api/v1/orders?id=42",
    "/health",
};
constexpr std::array<std::string_view, 6> APACHE_STATUSES = {"200", "201", "301", "304", "404", "500"};
constexpr std::array<std::string_view, 4> APACHE_REFERRERS = {
    "-", "https://example.com/", "https://google.com/search?q=foo", "https://news.example/"
};
constexpr std::array<std::string_view, 4> APACHE_AGENTS = {
    "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 Chrome/120.0.0.0 Safari/537.36",
    "curl/8.5.0",
    "python-requests/2.31.0",
    "aws-sdk-go/1.55.0",
};

constexpr std::array<std::string_view, 5> APACHE_ERROR_MODULES = {"core", "ssl", "http", "access", "authz_core"};
constexpr std::array<std::string_view, 4> APACHE_ERROR_LEVELS = {"notice", "warn", "error", "debug"};

constexpr std::array<std::string_view, 6> JAVA_LOGGERS = {
    "com.example.App",
    "com.example.Worker$Inner",
    "o.s.web.servlet.PageNotFound",
    "org.hibernate.SQL",
    "io.netty.channel.DefaultChannelPipeline",
    "com.example.service.UserService",
};
constexpr std::array<std::string_view, 4> JAVA_THREADS = {
    "main", "http-nio-8080-exec-1", "pool-1-thread-3", "scheduler-2"
};

// Map the lowercase LEVELS pool from `log_generator.cpp` onto the SLF4J
// tokens the Java template accepts. Unknown levels fall through to INFO.
std::string_view SlfjLevel(std::string_view level)
{
    if (level == "trace")
    {
        return "TRACE";
    }
    if (level == "debug")
    {
        return "DEBUG";
    }
    if (level == "info")
    {
        return "INFO";
    }
    if (level == "warning")
    {
        return "WARN";
    }
    if (level == "error")
    {
        return "ERROR";
    }
    if (level == "fatal")
    {
        return "FATAL";
    }
    return "INFO";
}

// Append "YYYY-MM-DD HH:MM:SS,mmm" (comma-decimal, Logback default).
void AppendJavaTimestamp(std::string &out, const IsoTimestamp &t)
{
    AppendPadded(out, t.year, 4);
    out.push_back('-');
    AppendPadded(out, static_cast<int>(t.month), 2);
    out.push_back('-');
    AppendPadded(out, static_cast<int>(t.day), 2);
    out.push_back(' ');
    AppendPadded(out, t.hour, 2);
    out.push_back(':');
    AppendPadded(out, t.minute, 2);
    out.push_back(':');
    AppendPadded(out, t.second, 2);
    out.push_back(',');
    AppendPadded(out, t.millisecond, 3);
}

// Append "MMM DD HH:MM:SS" (BSD syslog, day is space-padded to width 2).
void AppendSyslogTimestamp(std::string &out, const IsoTimestamp &t)
{
    const unsigned monthIdx = t.month == 0 ? 0U : t.month - 1U;
    out.append(MONTH_ABBREV[monthIdx % MONTH_ABBREV.size()]);
    out.push_back(' ');
    AppendSpacePadded2(out, t.day);
    out.push_back(' ');
    AppendPadded(out, t.hour, 2);
    out.push_back(':');
    AppendPadded(out, t.minute, 2);
    out.push_back(':');
    AppendPadded(out, t.second, 2);
}

// Append "DD/Mon/YYYY:HH:MM:SS +0000" (Apache CLF).
void AppendApacheClfTimestamp(std::string &out, const IsoTimestamp &t)
{
    AppendPadded(out, static_cast<int>(t.day), 2);
    out.push_back('/');
    const unsigned monthIdx = t.month == 0 ? 0U : t.month - 1U;
    out.append(MONTH_ABBREV[monthIdx % MONTH_ABBREV.size()]);
    out.push_back('/');
    AppendPadded(out, t.year, 4);
    out.push_back(':');
    AppendPadded(out, t.hour, 2);
    out.push_back(':');
    AppendPadded(out, t.minute, 2);
    out.push_back(':');
    AppendPadded(out, t.second, 2);
    out.append(" +0000");
}

// Append "Www Mon DD HH:MM:SS.mmm YYYY" (Apache error log form).
// Emits millis only when the source record carried them, so both
// sample-line variants of the pattern get covered.
void AppendApacheErrorTimestamp(std::string &out, const IsoTimestamp &t)
{
    const unsigned wd = WeekdayIndex(t.year, t.month == 0 ? 1U : t.month, t.day == 0 ? 1U : t.day);
    out.append(WEEKDAY_ABBREV[wd]);
    out.push_back(' ');
    const unsigned monthIdx = t.month == 0 ? 0U : t.month - 1U;
    out.append(MONTH_ABBREV[monthIdx % MONTH_ABBREV.size()]);
    out.push_back(' ');
    AppendPadded(out, static_cast<int>(t.day), 2);
    out.push_back(' ');
    AppendPadded(out, t.hour, 2);
    out.push_back(':');
    AppendPadded(out, t.minute, 2);
    out.push_back(':');
    AppendPadded(out, t.second, 2);
    if (t.hasMillis)
    {
        out.push_back('.');
        AppendPadded(out, t.millisecond, 3);
    }
    out.push_back(' ');
    AppendPadded(out, t.year, 4);
}

// Strip characters that would terminate a `"..."` field in Apache
// CLF (quotes and control characters). The generator's message
// pool is already safe; user-injected records may not be, so
// scrub defensively.
void AppendCleanQuotedText(std::string &out, std::string_view text)
{
    for (const char c : text)
    {
        if (c == '"' || c == '\r' || c == '\n')
        {
            out.push_back(' ');
        }
        else
        {
            out.push_back(c);
        }
    }
}

} // namespace

LogFormat SyslogRfc3164Format()
{
    return LogFormat{
        .suggestedExtension = ".log",
        .writeHeader = [](const RecordSchema &) { return std::string{}; },
        .writeLine =
            [](const LogRecord &record) {
                LineRng rng = MakeLineRng(LineNumberOr(record, 0));
                const IsoTimestamp ts = ParseIsoTimestamp(FieldOr(record, "timestamp", ""));

                std::string out;
                AppendSyslogTimestamp(out, ts);
                out.push_back(' ');
                out.append(Pick(SYSLOG_HOSTS, rng));
                out.push_back(' ');
                out.append(Pick(SYSLOG_PROGRAMS, rng));
                // Emit `[pid]` on odd lines so both sample-line
                // variants of the pattern get exercised at ~50/50.
                if ((std::uniform_int_distribution<int>{0, 1}(rng)) != 0)
                {
                    out.push_back('[');
                    AppendUInt(out, static_cast<unsigned>(std::uniform_int_distribution<int>{1, 9999}(rng)));
                    out.push_back(']');
                }
                out.append(": ");
                AppendCleanQuotedText(out, FieldOr(record, "message", "message"));
                return out;
            },
    };
}

LogFormat ApacheCombinedFormat()
{
    return LogFormat{
        .suggestedExtension = ".log",
        .writeHeader = [](const RecordSchema &) { return std::string{}; },
        .writeLine =
            [](const LogRecord &record) {
                LineRng rng = MakeLineRng(LineNumberOr(record, 0));
                const IsoTimestamp ts = ParseIsoTimestamp(FieldOr(record, "timestamp", ""));

                std::string out;
                out.append(Pick(APACHE_CLIENT_IPS, rng));
                out.append(" - ");
                out.append(Pick(APACHE_AUTH_USERS, rng));
                out.append(" [");
                AppendApacheClfTimestamp(out, ts);
                out.append("] \"");
                out.append(Pick(APACHE_VERBS, rng));
                out.push_back(' ');
                out.append(Pick(APACHE_PATHS, rng));
                out.append(" HTTP/1.1\" ");
                out.append(Pick(APACHE_STATUSES, rng));
                out.push_back(' ');
                // Response bytes: `-` (unknown) on ~1/4 of lines,
                // digits otherwise.
                if ((std::uniform_int_distribution<int>{0, 3}(rng)) == 0)
                {
                    out.push_back('-');
                }
                else
                {
                    AppendUInt(out, static_cast<unsigned>(std::uniform_int_distribution<int>{1, 999999}(rng)));
                }
                out.append(" \"");
                AppendCleanQuotedText(out, Pick(APACHE_REFERRERS, rng));
                out.append("\" \"");
                AppendCleanQuotedText(out, Pick(APACHE_AGENTS, rng));
                out.push_back('"');
                return out;
            },
    };
}

LogFormat ApacheCommonFormat()
{
    return LogFormat{
        .suggestedExtension = ".log",
        .writeHeader = [](const RecordSchema &) { return std::string{}; },
        .writeLine =
            [](const LogRecord &record) {
                LineRng rng = MakeLineRng(LineNumberOr(record, 0));
                const IsoTimestamp ts = ParseIsoTimestamp(FieldOr(record, "timestamp", ""));

                std::string out;
                out.append(Pick(APACHE_CLIENT_IPS, rng));
                out.append(" - ");
                out.append(Pick(APACHE_AUTH_USERS, rng));
                out.append(" [");
                AppendApacheClfTimestamp(out, ts);
                out.append("] \"");
                out.append(Pick(APACHE_VERBS, rng));
                out.push_back(' ');
                out.append(Pick(APACHE_PATHS, rng));
                out.append(" HTTP/1.1\" ");
                out.append(Pick(APACHE_STATUSES, rng));
                out.push_back(' ');
                if ((std::uniform_int_distribution<int>{0, 3}(rng)) == 0)
                {
                    out.push_back('-');
                }
                else
                {
                    AppendUInt(out, static_cast<unsigned>(std::uniform_int_distribution<int>{1, 999999}(rng)));
                }
                return out;
            },
    };
}

LogFormat ApacheErrorFormat()
{
    return LogFormat{
        .suggestedExtension = ".log",
        .writeHeader = [](const RecordSchema &) { return std::string{}; },
        .writeLine =
            [](const LogRecord &record) {
                LineRng rng = MakeLineRng(LineNumberOr(record, 0));
                const IsoTimestamp ts = ParseIsoTimestamp(FieldOr(record, "timestamp", ""));

                std::string out;
                out.push_back('[');
                AppendApacheErrorTimestamp(out, ts);
                out.append("] [");
                out.append(Pick(APACHE_ERROR_MODULES, rng));
                out.push_back(':');
                out.append(Pick(APACHE_ERROR_LEVELS, rng));
                out.append("] [pid ");
                AppendUInt(out, static_cast<unsigned>(std::uniform_int_distribution<int>{1, 65535}(rng)));
                out.append("] [client ");
                out.append(Pick(APACHE_CLIENT_IPS, rng));
                out.push_back(':');
                AppendUInt(out, static_cast<unsigned>(std::uniform_int_distribution<int>{1024, 65535}(rng)));
                out.append("] ");
                AppendCleanQuotedText(out, FieldOr(record, "message", "message"));
                return out;
            },
    };
}

LogFormat JavaLogFormat()
{
    return LogFormat{
        .suggestedExtension = ".log",
        .writeHeader = [](const RecordSchema &) { return std::string{}; },
        .writeLine =
            [](const LogRecord &record) {
                LineRng rng = MakeLineRng(LineNumberOr(record, 0));
                IsoTimestamp ts = ParseIsoTimestamp(FieldOr(record, "timestamp", ""));
                // Force a millisecond field so `AppendJavaTimestamp`
                // always emits the comma-decimal fractional part the
                // template accepts.
                ts.hasMillis = true;

                std::string out;
                AppendJavaTimestamp(out, ts);
                out.push_back(' ');
                out.append(SlfjLevel(FieldOr(record, "level", "info")));
                out.append(" [");
                out.append(Pick(JAVA_THREADS, rng));
                out.append("] ");
                out.append(Pick(JAVA_LOGGERS, rng));
                out.append(" - ");
                AppendCleanQuotedText(out, FieldOr(record, "message", "message"));
                return out;
            },
    };
}

RecordSchema DeriveSchemaFromRecord(const LogRecord &record)
{
    RecordSchema schema;
    if (!record.is_object())
    {
        return schema;
    }
    for (const auto &[key, _value] : record.get_object())
    {
        schema.emplace_back(key);
    }
    return schema;
}

} // namespace test_common
