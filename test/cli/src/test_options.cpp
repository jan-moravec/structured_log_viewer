#include "diagnostics.hpp"
#include "options.hpp"

#include <catch2/catch_all.hpp>

#include <chrono>
#include <cstdlib>
#include <optional>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <stdlib.h>
#endif

namespace
{

void SetEnv(const char *name, const char *value)
{
#ifdef _WIN32
    (void)SetEnvironmentVariableA(name, value);
#else
    (void)setenv(name, value, 1);
#endif
}

void UnsetEnv(const char *name)
{
#ifdef _WIN32
    (void)SetEnvironmentVariableA(name, nullptr);
#else
    (void)unsetenv(name);
#endif
}

[[nodiscard]] std::optional<std::string> CurrentEnv(const char *name)
{
#ifdef _WIN32
    const DWORD needed = GetEnvironmentVariableA(name, nullptr, 0);
    if (needed == 0)
    {
        return std::nullopt;
    }
    std::string value(static_cast<std::size_t>(needed), '\0');
    const DWORD written = GetEnvironmentVariableA(name, value.data(), needed);
    if (written == 0)
    {
        return std::nullopt;
    }
    value.resize(written);
    return value;
#else
    const char *value = std::getenv(name);
    if (value == nullptr)
    {
        return std::nullopt;
    }
    return std::string(value);
#endif
}

class EnvGuard
{
public:
    explicit EnvGuard(const char *name)
        : mName(name)
    {
        if (const std::optional<std::string> current = CurrentEnv(name))
        {
            mHad = true;
            mPrevious = *current;
        }
    }

    ~EnvGuard()
    {
        if (mHad)
        {
            SetEnv(mName.c_str(), mPrevious.c_str());
        }
        else
        {
            UnsetEnv(mName.c_str());
        }
    }

    EnvGuard(const EnvGuard &) = delete;
    EnvGuard &operator=(const EnvGuard &) = delete;

private:
    std::string mName;
    std::string mPrevious;
    bool mHad = false;
};

[[nodiscard]] std::expected<slp::Options, slp::UsageError> Parse(const std::vector<std::string> &words)
{
    std::vector<std::string> owned = words;
    std::vector<char *> argv;
    argv.reserve(owned.size());
    for (std::string &word : owned)
    {
        argv.push_back(word.data());
    }
    return slp::ParseArguments(static_cast<int>(argv.size()), argv.data());
}

} // namespace

TEST_CASE("top-level help and version", "[cli][options]")
{
    const auto missing = Parse({"slp"});
    REQUIRE(missing.has_value());
    CHECK(missing->kind == slp::ParseKind::MissingCommand);
    CHECK(missing->helpText.find("query") != std::string::npos);
    CHECK(missing->helpText.find("Exit codes:") != std::string::npos);
    CHECK(missing->helpText.find("130") != std::string::npos);

    const auto help = Parse({"slp", "--help"});
    REQUIRE(help.has_value());
    CHECK(help->kind == slp::ParseKind::Help);
    CHECK(help->helpText.find("Exit codes:") != std::string::npos);

    const auto queryHelp = Parse({"slp", "query", "--help"});
    REQUIRE(queryHelp.has_value());
    CHECK(queryHelp->kind == slp::ParseKind::Help);
    CHECK(queryHelp->helpText.find("query") != std::string::npos);
    CHECK(queryHelp->helpText.find("--batch-size") == std::string::npos);
    CHECK(queryHelp->helpText.find("Advanced options:") == std::string::npos);
    CHECK(queryHelp->helpText.find("Exit codes:") != std::string::npos);

    const auto advanced = Parse({"slp", "query", "--help-advanced"});
    REQUIRE(advanced.has_value());
    CHECK(advanced->helpText.find("Advanced options:") != std::string::npos);
    CHECK(advanced->helpText.find("--batch-size") != std::string::npos);

    const auto version = Parse({"slp", "--version"});
    REQUIRE(version.has_value());
    CHECK(version->kind == slp::ParseKind::Version);
    const std::string line = slp::VersionLine();
    CHECK(line.find("StructuredLogProcessor") != std::string::npos);
    CHECK(line.find("loglib") != std::string::npos);
}

TEST_CASE("global flags work before and after the subcommand", "[cli][options]")
{
    const auto before = Parse({"slp", "-q", "--threads", "4", "query", "app.log"});
    const auto after = Parse({"slp", "query", "-q", "--threads", "4", "app.log"});
    REQUIRE(before.has_value());
    REQUIRE(after.has_value());
    CHECK(before->quiet);
    CHECK(after->quiet);
    CHECK(before->threads == 4);
    CHECK(after->threads == 4);
    CHECK(before->command == slp::Command::Query);
    REQUIRE(before->sources.size() == 1);
    CHECK(before->sources[0].kind == slp::SourceKind::Path);
}

TEST_CASE("NO_COLOR forces never and the template directory flag wins", "[cli][options]")
{
    EnvGuard color("NO_COLOR");
    EnvGuard templates("SLV_TEMPLATE_DIR");
    SetEnv("NO_COLOR", "1");
    SetEnv("SLV_TEMPLATE_DIR", "from-env");

    const auto forced = Parse({"slp", "formats", "--color", "always"});
    REQUIRE(forced.has_value());
    CHECK(forced->color == slp::ColorMode::Never);

    UnsetEnv("NO_COLOR");
    const auto always = Parse({"slp", "formats", "--color", "always"});
    REQUIRE(always.has_value());
    CHECK(always->color == slp::ColorMode::Always);

    const auto fromEnv = Parse({"slp", "formats"});
    REQUIRE(fromEnv.has_value());
    REQUIRE(fromEnv->templateDir.has_value());
    CHECK(fromEnv->templateDir->filename() == "from-env");

    const auto fromFlag = Parse({"slp", "--template-dir", "from-flag", "formats"});
    REQUIRE(fromFlag.has_value());
    REQUIRE(fromFlag->templateDir.has_value());
    CHECK(fromFlag->templateDir->filename() == "from-flag");
}

TEST_CASE("mutual exclusions and source rules exit as usage errors", "[cli][options]")
{
    const auto lastAndOffset = Parse({"slp", "query", "--last", "2", "--offset", "1", "a.log"});
    REQUIRE_FALSE(lastAndOffset.has_value());
    CHECK(lastAndOffset.error().message.find("--last") != std::string::npos);
    CHECK(lastAndOffset.error().message.find("--offset") != std::string::npos);

    const auto bothRegex = Parse({"slp", "query", "--regex", "a", "--regex-template", "b", "a.log"});
    REQUIRE_FALSE(bothRegex.has_value());
    CHECK(bothRegex.error().message.find("--regex") != std::string::npos);

    const auto formatClash = Parse({"slp", "query", "--format", "json", "--regex", "a", "a.log"});
    REQUIRE_FALSE(formatClash.has_value());

    const auto twoStdin = Parse({"slp", "query", "-", "-"});
    REQUIRE_FALSE(twoStdin.has_value());
    CHECK(twoStdin.error().message.find("once") != std::string::npos);

    const auto stdinAndNet = Parse({"slp", "tail", "-", "tcp://127.0.0.1:9"});
    REQUIRE_FALSE(stdinAndNet.has_value());

    const auto netOnQuery = Parse({"slp", "query", "tcp://127.0.0.1:9"});
    REQUIRE_FALSE(netOnQuery.has_value());
    CHECK(netOnQuery.error().message.find("slp tail") != std::string::npos);

    const auto unknown = Parse({"slp", "query", "--not-a-flag"});
    REQUIRE_FALSE(unknown.has_value());
    CHECK(unknown.error().message.find("--not-a-flag") != std::string::npos);
    CHECK_FALSE(unknown.error().usage.empty());

    const auto bundle = Parse({"slp", "bundle", "a.log"});
    REQUIRE_FALSE(bundle.has_value());
    CHECK(bundle.error().message.find("--output") != std::string::npos);

    const auto strict = Parse({"slp", "validate", "--strict", "a.log"});
    REQUIRE_FALSE(strict.has_value());
    CHECK(strict.error().message.find("--max-error-rate") != std::string::npos);

    const auto rate = Parse({"slp", "query", "--max-error-rate", "1", "a.log"});
    REQUIRE_FALSE(rate.has_value());
    CHECK(rate.error().message.find("--strict") != std::string::npos);

    const auto column = Parse({"slp", "stats", "--by", "column", "a.log"});
    REQUIRE_FALSE(column.has_value());
    CHECK(column.error().message.find("--column") != std::string::npos);

    const auto retention = Parse({"slp", "tail", "--retention", "10", "a.log"});
    REQUIRE_FALSE(retention.has_value());
    CHECK(retention.error().message.find("--retention") != std::string::npos);
}

TEST_CASE("tail accepts network sources and retention bounds", "[cli][options]")
{
    const auto tail = Parse({"slp", "tail", "tcp://127.0.0.1:514", "udp://:9", "app.slvbundle"});
    REQUIRE(tail.has_value());
    REQUIRE(tail->sources.size() == 3);
    CHECK(tail->sources[0].kind == slp::SourceKind::Tcp);
    CHECK(tail->sources[0].host == "127.0.0.1");
    CHECK(tail->sources[0].port == 514);
    CHECK(tail->sources[1].kind == slp::SourceKind::Udp);
    CHECK(tail->sources[1].host.empty());
    CHECK(tail->sources[1].port == 9);
    CHECK(tail->sources[2].kind == slp::SourceKind::Bundle);
    CHECK(tail->outputFormat == "auto");

    const auto unbounded = Parse({"slp", "tail", "--retention", "0", "-"});
    REQUIRE(unbounded.has_value());
    REQUIRE(unbounded->retention.has_value());
    CHECK(*unbounded->retention == 0);
    REQUIRE(unbounded->sources.size() == 1);
    CHECK(unbounded->sources[0].kind == slp::SourceKind::Stdin);

    const auto duration = Parse({"slp", "tail", "--for", "30s", "a.log"});
    REQUIRE(duration.has_value());
    REQUIRE(duration->duration.has_value());
    CHECK(*duration->duration == std::chrono::seconds(30));
}

TEST_CASE("size and duration suffixes", "[cli][options]")
{
    const auto mebi = slp::ParseSizeSuffix("1MiB");
    REQUIRE(mebi.has_value());
    CHECK(*mebi == 1024ULL * 1024ULL);

    const auto bytes = slp::ParseSizeSuffix("12");
    REQUIRE(bytes.has_value());
    CHECK(*bytes == 12);

    CHECK_FALSE(slp::ParseSizeSuffix("nope").has_value());
    CHECK_FALSE(slp::ParseSizeSuffix("1XB").has_value());

    const auto seconds = slp::ParseDurationSuffix("30s");
    const auto minutes = slp::ParseDurationSuffix("5m");
    const auto hours = slp::ParseDurationSuffix("1h");
    REQUIRE(seconds.has_value());
    REQUIRE(minutes.has_value());
    REQUIRE(hours.has_value());
    CHECK(*seconds == std::chrono::seconds(30));
    CHECK(*minutes == std::chrono::minutes(5));
    CHECK(*hours == std::chrono::hours(1));
    CHECK_FALSE(slp::ParseDurationSuffix("1d").has_value());
    CHECK_FALSE(slp::ParseDurationSuffix("30").has_value());

    const auto batch = Parse({"slp", "query", "--batch-size", "1KiB", "a.log"});
    REQUIRE(batch.has_value());
    REQUIRE(batch->batchSizeBytes.has_value());
    CHECK(*batch->batchSizeBytes == 1024);
}

TEST_CASE("diagnostics lines keep data and errors apart", "[cli][options]")
{
    CHECK(slp::FormatErrorLine("", "the file is missing") == "slp: the file is missing.");
    CHECK(slp::FormatErrorLine("query", "the file is missing.") == "slp query: the file is missing.");
    CHECK(slp::FormatStageLine("parse", 12, 4) == "stage=parse rows=12 ms=4");
    CHECK(slp::FormatWarningLine("timezone data was not found") == "slp: timezone data was not found.");
}
