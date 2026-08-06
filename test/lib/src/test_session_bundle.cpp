#include <loglib/internal/decompressing_byte_source.hpp>
#include <loglib/internal/path_encoding.hpp>
#include <loglib/log_configuration.hpp>
#include <loglib/log_processing.hpp>
#include <loglib/log_table.hpp>
#include <loglib/log_value.hpp>
#include <loglib/parse_file.hpp>
#include <loglib/session_bundle.hpp>

#include <catch2/catch_all.hpp>
#include <zstd.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{

class TempPath
{
public:
    explicit TempPath(std::string suffix)
    {
        static std::atomic<unsigned> counter{0};
        mPath = std::filesystem::temp_directory_path() /
                ("slv-session-bundle-" + std::to_string(++counter) + std::move(suffix));
    }

    ~TempPath()
    {
        try
        {
            std::error_code error;
            std::filesystem::remove(mPath, error);
            std::filesystem::remove(TemporaryPath(), error);
            // Remove randomized staging names left by an interrupted test.
            RemoveStagingSiblings();
        }
        catch (...) // NOLINT(bugprone-empty-catch): destructor must not throw during test teardown.
        {
        }
    }

    TempPath(const TempPath &) = delete;
    TempPath &operator=(const TempPath &) = delete;
    TempPath(TempPath &&) = delete;
    TempPath &operator=(TempPath &&) = delete;

    [[nodiscard]] const std::filesystem::path &Path() const noexcept { return mPath; }

    /// Legacy deterministic staging path used by compatibility tests.
    [[nodiscard]] std::filesystem::path TemporaryPath() const
    {
        std::filesystem::path temporary = mPath;
        temporary += ".tmp";
        return temporary;
    }

    /// Return whether any staging sibling remains.
    [[nodiscard]] bool HasStagingFiles() const
    {
        const std::filesystem::path parent = mPath.parent_path();
        std::error_code error;
        if (!std::filesystem::exists(parent, error))
        {
            return false;
        }
        const std::string basename = mPath.filename().string();
        const std::filesystem::directory_iterator iter(parent, error);
        return std::ranges::any_of(
            iter,
            [&basename](const std::filesystem::directory_entry &entry) {
                const std::string name = entry.path().filename().string();
                return name != basename && name.starts_with(basename) && name.ends_with(".tmp");
            }
        );
    }

private:
    std::filesystem::path mPath;

    void RemoveStagingSiblings() const
    {
        const std::filesystem::path parent = mPath.parent_path();
        std::error_code error;
        if (!std::filesystem::exists(parent, error))
        {
            return;
        }
        const std::string basename = mPath.filename().string();
        for (const auto &entry : std::filesystem::directory_iterator(parent, error))
        {
            const std::string name = entry.path().filename().string();
            if (name == basename)
            {
                continue;
            }
            if (name.starts_with(basename) && name.ends_with(".tmp"))
            {
                std::error_code removeError;
                std::filesystem::remove(entry.path(), removeError);
            }
        }
    }
};

void Write(const std::filesystem::path &path, std::string_view bytes)
{
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    REQUIRE(stream.is_open());
    stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    REQUIRE(stream.good());
}

[[nodiscard]] std::string Read(const std::filesystem::path &path)
{
    std::ifstream stream(path, std::ios::binary);
    REQUIRE(stream.is_open());
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

[[nodiscard]] loglib::LogTable ParseTable(const std::filesystem::path &path)
{
    loglib::ParseResult parsed = loglib::ParseFile(path);
    REQUIRE(parsed.errors.empty());
    loglib::LogConfigurationManager manager;
    manager.Update(parsed.data);
    return {std::move(parsed.data), std::move(manager)};
}

/// Parse a table and promote timestamp columns before serialization.
[[nodiscard]] loglib::LogTable ParseTableWithTimestamps(const std::filesystem::path &path)
{
    loglib::ParseResult parsed = loglib::ParseFile(path);
    REQUIRE(parsed.errors.empty());
    loglib::LogConfigurationManager manager;
    manager.Update(parsed.data);
    loglib::ParseTimestamps(parsed.data, manager.Configuration());
    return {std::move(parsed.data), std::move(manager)};
}

[[nodiscard]] loglib::internal::DecompressingByteSource DecodeBundle(const std::filesystem::path &path)
{
    loglib::internal::DecompressingByteSource::Options options;
    options.discardFirstLine = true;
    return {path, {}, {}, options};
}

[[nodiscard]] std::string CompressZstd(std::string_view input)
{
    std::string compressed;
    compressed.resize(ZSTD_compressBound(input.size()));
    const std::size_t size =
        ZSTD_compress(compressed.data(), compressed.size(), input.data(), input.size(), 1);
    if (ZSTD_isError(size))
    {
        throw std::runtime_error(ZSTD_getErrorName(size));
    }
    compressed.resize(size);
    return compressed;
}

[[nodiscard]] loglib::FilterExpression SingleLeafExpression(loglib::LeafRule rule)
{
    loglib::FilterExpression expression;
    expression.node = loglib::FilterExpression::Leaf{std::move(rule)};
    return expression;
}

} // namespace

TEST_CASE("session bundle v1 exports documented metadata and typed normalized JSONL", "[SessionBundle]")
{
    const TempPath source(".jsonl");
    const TempPath bundle(".slvbundle");
    Write(
        source.Path(),
        R"({"signed":-7,"unsigned":18446744073709551615,"whole":2.0,"fraction":2.5,"ok":true,"msg":"quote: \" slash: \\ newline:\n"})"
        "\n"
    );

    loglib::LogTable table = ParseTable(source.Path());
    loglib::WriteSessionBundle(table, table.Configuration().Configuration(), bundle.Path());

    REQUIRE(loglib::LooksLikeSessionBundle(bundle.Path()));
    auto decoded = DecodeBundle(bundle.Path());
    const std::string &firstLine = decoded.DiscardedFirstLine();
    CHECK(firstLine.starts_with(R"({"__slv_bundle__":{"formatVersion":1,"rowCount":1,"configuration":)"));
    CHECK(firstLine.ends_with("}}"));

    const loglib::SessionBundleMetadata metadata = loglib::ParseSessionBundleMetadata(firstLine);
    CHECK(metadata.formatVersion == 1);
    CHECK(metadata.rowCount == 1);

    const std::string jsonl = Read(decoded.EffectivePath());
    CHECK(jsonl.contains(R"("signed":-7)"));
    CHECK(jsonl.contains(R"("unsigned":18446744073709551615)"));
    CHECK(jsonl.contains(R"("whole":2.0)"));
    CHECK(jsonl.contains(R"("fraction":2.5)"));
    CHECK(jsonl.contains(R"("ok":true)"));
    CHECK(jsonl.contains(R"("msg":"quote: \" slash: \\ newline:\n")"));

    const loglib::ParseResult restored = loglib::ParseFile(decoded.EffectivePath());
    REQUIRE(restored.errors.empty());
    REQUIRE(restored.data.Lines().size() == 1);
    const loglib::LogLine &row = restored.data.Lines().front();
    CHECK(std::get<std::int64_t>(row.GetValue("signed")) == -7);
    CHECK(std::get<std::uint64_t>(row.GetValue("unsigned")) == UINT64_MAX);
    CHECK(std::get<double>(row.GetValue("whole")) == Catch::Approx(2.0));
    CHECK(std::get<double>(row.GetValue("fraction")) == Catch::Approx(2.5));
    CHECK(std::get<bool>(row.GetValue("ok")));
    CHECK(loglib::AsStringView(row.GetValue("msg")) == "quote: \" slash: \\ newline:\n");
}

TEST_CASE("session bundle metadata preserves investigation configuration and flattens source", "[SessionBundle]")
{
    const TempPath source(".jsonl");
    const TempPath bundle(".slvbundle");
    Write(source.Path(), R"({"service":"auth","count":4})" "\n");
    const loglib::LogTable table = ParseTable(source.Path());

    loglib::LogConfiguration configuration;
    configuration.columns = {
        {.header = "Count renamed",
         .keys = {"count"},
         .printFormat = "%d",
         .type = loglib::LogConfiguration::Type::Integer,
         .parseFormats = {},
         .visible = false,
         .levelMapping = {},
         .autoDetect = false},
        {.header = "Service",
         .keys = {"service"},
         .printFormat = {},
         .type = loglib::LogConfiguration::Type::String,
         .parseFormats = {},
         .visible = true,
         .levelMapping = {},
         .autoDetect = false},
    };
    const loglib::LeafRule filter{
        .type = loglib::LeafRule::Type::String,
        .columnKeys = {"service"},
        .matchType = loglib::LeafRule::Match::Exactly,
        .filterString = "auth",
        .filterBegin = std::nullopt,
        .filterEnd = std::nullopt,
        .filterMinValue = std::nullopt,
        .filterMaxValue = std::nullopt,
        .filterValues = {},
    };
    configuration.expression = SingleLeafExpression(filter);
    configuration.sort = {.columnIndex = 1, .descending = true};
    configuration.source = loglib::LogConfiguration::Source{
        .kind = loglib::LogConfiguration::Source::Kind::NetworkStream,
        .format = loglib::LogConfiguration::Source::Format::Regex,
        .locators = {"tcp://127.0.0.1:9000"},
        .locatorDedupKeys = {"tcp://127.0.0.1:9000"},
        .regexPattern = "(?<service>.*)",
    };
    configuration.highlightRules = {
        {.name = "Auth",
         .enabled = true,
         .columnKeys = {"service"},
         .type = loglib::LeafRule::Type::String,
         .matchType = loglib::LeafRule::Match::Contains,
         .filterString = "auth",
         .filterBegin = std::nullopt,
         .filterEnd = std::nullopt,
         .filterMinValue = std::nullopt,
         .filterMaxValue = std::nullopt,
         .filterValues = {},
         .foregroundIndex = 2,
         .backgroundIndex = 3,
         .bold = true,
         .italic = false},
    };

    loglib::WriteSessionBundle(table, configuration, bundle.Path());
    auto decoded = DecodeBundle(bundle.Path());
    const loglib::LogConfiguration restored =
        loglib::ParseSessionBundleMetadata(decoded.DiscardedFirstLine()).configuration;

    REQUIRE(restored.columns.size() == 2);
    CHECK(restored.columns[0].header == "Count renamed");
    CHECK(restored.columns[0].keys == std::vector<std::string>{"count"});
    CHECK(restored.columns[0].visible == false);
    CHECK(restored.columns[0].type == loglib::LogConfiguration::Type::Integer);
    CHECK(restored.columns[0].autoDetect == false);
    CHECK(restored.columns[1].header == "Service");
    CHECK(restored.columns[1].keys == std::vector<std::string>{"service"});
    CHECK(restored.columns[1].visible);
    CHECK(restored.expression == configuration.expression);
    CHECK(restored.sort.columnIndex == 1);
    CHECK(restored.sort.descending);
    CHECK(restored.highlightRules == configuration.highlightRules);

    REQUIRE(restored.source.has_value());
    CHECK(restored.source->kind == loglib::LogConfiguration::Source::Kind::File);
    CHECK(restored.source->format == loglib::LogConfiguration::Source::Format::Json);
    // With no canonicalizer set, both `locators` and
    // `locatorDedupKeys` use `internal::PathToUtf8()`, so this
    // default-options test expects them to match. The canonicalizer
    // contract is exercised in the dedicated test below.
    CHECK(restored.source->locators == std::vector<std::string>{bundle.Path().string()});
    CHECK(restored.source->locatorDedupKeys == std::vector<std::string>{bundle.Path().string()});
    CHECK(restored.source->regexPattern.empty());
}

TEST_CASE(
    "session bundle uses canonicalizeSourceLocator for embedded source dedup key",
    "[SessionBundle]"
)
{
    // The GUI wires `canonicalizeSourceLocator` to `CanonicalLocator`
    // so anchors round-trip. The same callback must also shape
    // `Source::locatorDedupKeys`; otherwise a mixed-case bundle
    // would import with two different keys depending on the
    // consumer (GUI vs. non-GUI).
    const TempPath source(".jsonl");
    const TempPath bundle(".slvbundle");
    Write(source.Path(), R"({"msg":"x"})" "\n");
    loglib::LogTable table = ParseTable(source.Path());

    loglib::SessionBundleWriteOptions options;
    options.canonicalizeSourceLocator = [](const std::filesystem::path &p) {
        std::string out = "canonical://" + p.string();
        return out;
    };
    loglib::WriteSessionBundle(table, table.Configuration().Configuration(), bundle.Path(), options);

    auto decoded = DecodeBundle(bundle.Path());
    const loglib::LogConfiguration restored =
        loglib::ParseSessionBundleMetadata(decoded.DiscardedFirstLine()).configuration;

    REQUIRE(restored.source.has_value());
    // Display locator is unchanged (raw UTF-8 of the destination).
    CHECK(restored.source->locators == std::vector<std::string>{bundle.Path().string()});
    // Dedup key runs through the callback, matching the shape
    // anchors are compared against.
    CHECK(
        restored.source->locatorDedupKeys ==
        std::vector<std::string>{"canonical://" + bundle.Path().string()}
    );
}

TEST_CASE("session bundle anchors are densely remapped and detached anchors are dropped", "[SessionBundle]")
{
    const TempPath sourceA(".a.jsonl");
    const TempPath sourceB(".b.jsonl");
    const TempPath bundle(".slvbundle");
    Write(sourceA.Path(), R"({"msg":"a0"})" "\n" R"({"msg":"a1"})" "\n");
    Write(sourceB.Path(), R"({"msg":"b0"})" "\n");

    loglib::ParseResult parsedA = loglib::ParseFile(sourceA.Path());
    loglib::ParseResult parsedB = loglib::ParseFile(sourceB.Path());
    REQUIRE(parsedA.errors.empty());
    REQUIRE(parsedB.errors.empty());
    parsedA.data.Merge(std::move(parsedB.data));
    loglib::LogConfigurationManager manager;
    manager.Update(parsedA.data);
    loglib::LogTable table(std::move(parsedA.data), std::move(manager));

    loglib::LogConfiguration configuration = table.Configuration().Configuration();
    configuration.anchors = {
        {.locator = sourceA.Path().string(), .lineId = 1, .colorIndex = 1, .note = "second A"},
        {.locator = sourceB.Path().string(), .lineId = 0, .colorIndex = 2, .note = "first B"},
        {.locator = "detached.json", .lineId = 99, .colorIndex = 3, .note = "drop me"},
    };
    loglib::WriteSessionBundle(table, configuration, bundle.Path());

    auto decoded = DecodeBundle(bundle.Path());
    const auto anchors =
        loglib::ParseSessionBundleMetadata(decoded.DiscardedFirstLine()).configuration.anchors;
    REQUIRE(anchors.size() == 2);
    CHECK(anchors[0].locator == bundle.Path().string());
    CHECK(anchors[0].lineId == 1);
    CHECK(anchors[0].note == "second A");
    CHECK(anchors[1].locator == bundle.Path().string());
    CHECK(anchors[1].lineId == 2);
    CHECK(anchors[1].note == "first B");
}

TEST_CASE("canonicalizeSourceLocator bridges canonicalized anchors to raw source paths", "[SessionBundle]")
{
    // Mirrors production: `AnchorManager` stores canonicalized
    // locators while `line.Source()->Path()` is the raw filesystem
    // path. Without the callback the writer silently drops every
    // anchor because `PathToUtf8()` doesn't match the canonical
    // form.
    const TempPath source(".jsonl");
    const TempPath bundle(".slvbundle");
    Write(source.Path(), R"({"msg":"a0"})" "\n" R"({"msg":"a1"})" "\n");
    loglib::LogTable table = ParseTable(source.Path());

    // Force a canonical form that does not equal `PathToUtf8()`.
    const std::string canonicalLocator = "canonical://" + source.Path().string();

    loglib::LogConfiguration configuration = table.Configuration().Configuration();
    configuration.anchors = {
        {.locator = canonicalLocator, .lineId = 0, .colorIndex = 1, .note = "kept"},
        {.locator = canonicalLocator, .lineId = 1, .colorIndex = 2, .note = "also kept"},
    };

    // Sanity: without the callback the writer drops both anchors --
    // the canonical form does not match `PathToUtf8(source->Path())`.
    {
        const TempPath unbridged(".slvbundle");
        loglib::WriteSessionBundle(table, configuration, unbridged.Path());
        auto decoded = DecodeBundle(unbridged.Path());
        const auto anchors =
            loglib::ParseSessionBundleMetadata(decoded.DiscardedFirstLine()).configuration.anchors;
        CHECK(anchors.empty());
    }

    loglib::SessionBundleWriteOptions options;
    options.canonicalizeSourceLocator = [](const std::filesystem::path &p) {
        return "canonical://" + p.string();
    };
    loglib::WriteSessionBundle(table, configuration, bundle.Path(), options);

    auto decoded = DecodeBundle(bundle.Path());
    const auto anchors =
        loglib::ParseSessionBundleMetadata(decoded.DiscardedFirstLine()).configuration.anchors;
    REQUIRE(anchors.size() == 2);
    // Anchors flatten to the canonical destination form so they
    // match `Source::locatorDedupKeys` on the receiving side (the
    // canonicalizer runs against the destination path too).
    const std::string canonicalBundle = "canonical://" + bundle.Path().string();
    CHECK(anchors[0].locator == canonicalBundle);
    CHECK(anchors[0].lineId == 0);
    CHECK(anchors[0].note == "kept");
    CHECK(anchors[1].locator == canonicalBundle);
    CHECK(anchors[1].lineId == 1);
    CHECK(anchors[1].note == "also kept");
}

TEST_CASE("session bundle drops anchors whose `(locator, lineId)` matches multiple flattened rows", "[SessionBundle]")
{
    // Both sources map `(locator, lineId)` to the same anchor key.
    const TempPath sourceA(".a.jsonl");
    const TempPath sourceB(".b.jsonl");
    const TempPath bundle(".slvbundle");
    Write(sourceA.Path(), R"({"msg":"a0"})" "\n");
    Write(sourceB.Path(), R"({"msg":"b0"})" "\n");

    loglib::ParseResult parsedA = loglib::ParseFile(sourceA.Path());
    loglib::ParseResult parsedB = loglib::ParseFile(sourceB.Path());
    REQUIRE(parsedA.errors.empty());
    REQUIRE(parsedB.errors.empty());
    parsedA.data.Merge(std::move(parsedB.data));
    loglib::LogConfigurationManager manager;
    manager.Update(parsedA.data);
    loglib::LogTable table(std::move(parsedA.data), std::move(manager));

    loglib::LogConfiguration configuration = table.Configuration().Configuration();
    configuration.anchors = {
        {.locator = "collapsed", .lineId = 0, .colorIndex = 1, .note = "ambiguous - drop me"},
    };

    // Force the ambiguous canonical locator.
    loglib::SessionBundleWriteOptions options;
    options.canonicalizeSourceLocator = [](const std::filesystem::path &) { return std::string("collapsed"); };
    loglib::WriteSessionBundle(table, configuration, bundle.Path(), options);

    auto decoded = DecodeBundle(bundle.Path());
    const auto anchors =
        loglib::ParseSessionBundleMetadata(decoded.DiscardedFirstLine()).configuration.anchors;
    CHECK(anchors.empty());
}

TEST_CASE("session bundle drops anchors with an empty `locator`", "[SessionBundle]")
{
    // Empty locators cannot identify a source row.
    const TempPath source(".jsonl");
    const TempPath bundle(".slvbundle");
    Write(source.Path(), R"({"msg":"a0"})" "\n" R"({"msg":"a1"})" "\n");
    loglib::LogTable table = ParseTable(source.Path());

    loglib::LogConfiguration configuration = table.Configuration().Configuration();
    configuration.anchors = {
        {.locator = "", .lineId = 0, .colorIndex = 1, .note = "empty locator - drop"},
        {.locator = "", .lineId = 1, .colorIndex = 2, .note = "empty locator - drop"},
    };

    loglib::WriteSessionBundle(table, configuration, bundle.Path());

    auto decoded = DecodeBundle(bundle.Path());
    const auto anchors =
        loglib::ParseSessionBundleMetadata(decoded.DiscardedFirstLine()).configuration.anchors;
    CHECK(anchors.empty());
}

TEST_CASE("session bundle round-trips non-ASCII destination paths", "[SessionBundle]")
{
    // Non-ASCII destinations must survive writing and reopening.
    const TempPath source(".jsonl");
    Write(source.Path(), R"({"msg":"row"})" "\n");
    loglib::LogTable table = ParseTable(source.Path());

    static std::atomic<unsigned> counter{0};
    // Use explicit UTF-8 bytes to avoid compiler charset dependence.
    const std::string utf8Filename =
        "slv-bundle-"
        "\xC5\xBE"     // U+017E z-with-caron
        "\xC3\xA9"     // U+00E9 e-acute
        "\xE6\x97\xA5" // U+65E5 CJK "day"
        "-" + std::to_string(++counter) + ".slvbundle";
    // Avoid Windows active-code-page conversion.
    const std::filesystem::path bundlePath = std::filesystem::temp_directory_path() / loglib::internal::Utf8ToPath(utf8Filename);
    std::error_code cleanupError;
    std::filesystem::remove(bundlePath, cleanupError);
    try
    {
        loglib::WriteSessionBundle(table, table.Configuration().Configuration(), bundlePath);
        REQUIRE(std::filesystem::exists(bundlePath));
        CHECK(loglib::LooksLikeSessionBundle(bundlePath));

        loglib::internal::DecompressingByteSource::Options options;
        options.discardFirstLine = true;
        const loglib::internal::DecompressingByteSource decoded(bundlePath, {}, {}, options);
        const auto metadata = loglib::ParseSessionBundleMetadata(decoded.DiscardedFirstLine());
        CHECK(metadata.rowCount == 1);
    }
    catch (...)
    {
        std::filesystem::remove(bundlePath, cleanupError);
        throw;
    }
    std::filesystem::remove(bundlePath, cleanupError);
}

TEST_CASE("session bundle cancellation before write preserves destination and leaves no temp", "[SessionBundle]")
{
    const TempPath source(".jsonl");
    const TempPath bundle(".slvbundle");
    Write(source.Path(), R"({"msg":"row"})" "\n");
    Write(bundle.Path(), "existing destination");
    loglib::LogTable table = ParseTable(source.Path());

    loglib::StopSource stop;
    stop.request_stop();
    loglib::SessionBundleWriteOptions options;
    options.stopToken = stop.get_token();
    CHECK_THROWS_AS(
        loglib::WriteSessionBundle(table, table.Configuration().Configuration(), bundle.Path(), options),
        loglib::SessionBundleCancelled
    );
    CHECK(Read(bundle.Path()) == "existing destination");
    CHECK_FALSE(bundle.HasStagingFiles());
}

TEST_CASE("session bundle cancellation from progress preserves destination and removes temp", "[SessionBundle]")
{
    const TempPath source(".jsonl");
    const TempPath bundle(".slvbundle");
    std::string rows;
    rows.reserve(70'000);
    for (std::size_t i = 0; i < 4097; ++i)
    {
        rows += "{\"i\":" + std::to_string(i) + "}\n";
    }
    Write(source.Path(), rows);
    Write(bundle.Path(), "existing destination");
    loglib::LogTable table = ParseTable(source.Path());

    loglib::StopSource stop;
    loglib::SessionBundleWriteOptions options;
    options.stopToken = stop.get_token();
    options.progress = [&stop](std::uint64_t written, std::uint64_t) {
        if (written == 4096)
        {
            stop.request_stop();
        }
    };
    CHECK_THROWS_AS(
        loglib::WriteSessionBundle(table, table.Configuration().Configuration(), bundle.Path(), options),
        loglib::SessionBundleCancelled
    );
    CHECK(Read(bundle.Path()) == "existing destination");
    CHECK_FALSE(bundle.HasStagingFiles());
}

TEST_CASE("successful session bundle write atomically replaces destination", "[SessionBundle]")
{
    const TempPath source(".jsonl");
    const TempPath bundle(".slvbundle");
    Write(source.Path(), R"({"msg":"replacement"})" "\n");
    Write(bundle.Path(), "old destination");
    loglib::LogTable table = ParseTable(source.Path());

    loglib::WriteSessionBundle(table, table.Configuration().Configuration(), bundle.Path());

    CHECK(Read(bundle.Path()) != "old destination");
    CHECK(loglib::LooksLikeSessionBundle(bundle.Path()));
    CHECK_FALSE(bundle.HasStagingFiles());
    auto decoded = DecodeBundle(bundle.Path());
    CHECK(loglib::ParseSessionBundleMetadata(decoded.DiscardedFirstLine()).rowCount == 1);
}

TEST_CASE("session bundle writer preserves unrelated .tmp siblings in the destination directory", "[SessionBundle]")
{
    // The writer must not remove unrelated `.tmp` siblings.
    const TempPath source(".jsonl");
    const TempPath bundle(".slvbundle");
    Write(source.Path(), R"({"msg":"only"})" "\n");

    // Plant a decoy that resembles, but is not, a staging file.
    const std::filesystem::path decoy = std::filesystem::path(bundle.Path().string() + ".manual-decoy.tmp");
    constexpr std::string_view DECOY_CONTENTS = "manual-decoy-must-survive-write";
    Write(decoy, DECOY_CONTENTS);

    loglib::LogTable table = ParseTable(source.Path());
    loglib::WriteSessionBundle(table, table.Configuration().Configuration(), bundle.Path());

    CHECK(loglib::LooksLikeSessionBundle(bundle.Path()));
    REQUIRE(std::filesystem::exists(decoy));
    CHECK(Read(decoy) == std::string(DECOY_CONTENTS));

    // Remove the decoy before fixture cleanup.
    std::error_code cleanupError;
    std::filesystem::remove(decoy, cleanupError);
}

TEST_CASE("session bundle metadata rejects malformed envelope versions and excessive rows", "[SessionBundle]")
{
    CHECK_THROWS_AS(loglib::ParseSessionBundleMetadata("not json"), loglib::SessionBundleReadError);
    CHECK_THROWS_AS(loglib::ParseSessionBundleMetadata("{}"), loglib::SessionBundleReadError);
    CHECK_THROWS_AS(
        loglib::ParseSessionBundleMetadata(
            R"({"__slv_bundle__":{"formatVersion":0,"rowCount":0,"configuration":{}}})"
        ),
        loglib::SessionBundleVersionError
    );
    CHECK_THROWS_AS(
        loglib::ParseSessionBundleMetadata(
            R"({"__slv_bundle__":{"formatVersion":2,"rowCount":0,"configuration":{}}})"
        ),
        loglib::SessionBundleVersionError
    );
    CHECK_THROWS_AS(
        loglib::ParseSessionBundleMetadata(
            R"({"__slv_bundle__":{"formatVersion":1,"rowCount":1000000001,"configuration":{}}})"
        ),
        loglib::SessionBundleReadError
    );
}

TEST_CASE("LooksLikeSessionBundle distinguishes plain input from produced zstd bundle", "[SessionBundle]")
{
    const TempPath plain(".jsonl");
    const TempPath bundle(".slvbundle");
    Write(plain.Path(), R"({"msg":"plain"})" "\n");
    CHECK_FALSE(loglib::LooksLikeSessionBundle(plain.Path()));

    loglib::LogTable table = ParseTable(plain.Path());
    loglib::WriteSessionBundle(table, table.Configuration().Configuration(), bundle.Path());
    CHECK(loglib::LooksLikeSessionBundle(bundle.Path()));
}

TEST_CASE("session bundle round-trips ISO-8601 Time column values", "[SessionBundle]")
{
    // Normalized UTC timestamps must remain typed after reopening.
    const TempPath source(".jsonl");
    const TempPath bundle(".slvbundle");
    Write(
        source.Path(),
        R"({"ts":"2025-04-25T12:34:56.123456","msg":"row0"})" "\n"
        R"({"ts":"2025-04-25T12:34:57.789012","msg":"row1"})" "\n"
    );

    loglib::LogTable table = ParseTableWithTimestamps(source.Path());
    // Confirm the source table contains typed timestamps.
    REQUIRE(table.Data().Lines().size() == 2);
    const loglib::TimeStamp originalRow0 =
        std::get<loglib::TimeStamp>(table.Data().Lines()[0].GetValue("ts"));
    const loglib::TimeStamp originalRow1 =
        std::get<loglib::TimeStamp>(table.Data().Lines()[1].GetValue("ts"));

    loglib::WriteSessionBundle(table, table.Configuration().Configuration(), bundle.Path());

    auto decoded = DecodeBundle(bundle.Path());
    const std::string jsonl = Read(decoded.EffectivePath());
    // Confirm the normalized on-disk UTC form.
    CHECK(jsonl.contains(R"("ts":"2025-04-25T12:34:56.123456Z")"));

    loglib::LogTable reloaded = ParseTableWithTimestamps(decoded.EffectivePath());
    REQUIRE(reloaded.Data().Lines().size() == 2);
    CHECK(std::get<loglib::TimeStamp>(reloaded.Data().Lines()[0].GetValue("ts")) == originalRow0);
    CHECK(std::get<loglib::TimeStamp>(reloaded.Data().Lines()[1].GetValue("ts")) == originalRow1);
}

TEST_CASE(
    "session bundle prepends bundle-canonical Time parseFormat to embedded configuration",
    "[SessionBundle]"
)
{
    // Embedded Time columns must parse the normalized bundle format.
    const TempPath source(".jsonl");
    const TempPath bundle(".slvbundle");
    Write(source.Path(), R"({"ts":"25/04/2025 12:34:56","msg":"row"})" "\n");
    loglib::LogTable table = ParseTable(source.Path());

    loglib::LogConfiguration configuration = table.Configuration().Configuration();
    // Use a custom source format with auto-detection disabled.
    for (auto &column : configuration.columns)
    {
        if (column.keys.size() == 1 && column.keys.front() == "ts")
        {
            column.type = loglib::LogConfiguration::Type::Time;
            column.parseFormats = {"%d/%m/%Y %H:%M:%S"};
            column.autoDetect = false;
        }
    }

    loglib::WriteSessionBundle(table, configuration, bundle.Path());
    auto decoded = DecodeBundle(bundle.Path());
    const loglib::LogConfiguration restored =
        loglib::ParseSessionBundleMetadata(decoded.DiscardedFirstLine()).configuration;

    const auto tsColumn =
        std::ranges::find_if(restored.columns, [](const loglib::LogConfiguration::Column &column) {
            return column.keys.size() == 1 && column.keys.front() == "ts";
        });
    REQUIRE(tsColumn != restored.columns.end());
    CHECK(tsColumn->type == loglib::LogConfiguration::Type::Time);
    REQUIRE(tsColumn->parseFormats.size() >= 2);
    // Keep the fast ISO parser first.
    CHECK(tsColumn->parseFormats.front() == "%FT%T");
    CHECK(std::ranges::find(tsColumn->parseFormats, std::string("%d/%m/%Y %H:%M:%S")) !=
          tsColumn->parseFormats.end());
}

TEST_CASE("bundle metadata extraction enforces first-line cap and newline", "[SessionBundle]")
{
    const TempPath oversized(".slvbundle");
    const TempPath unterminated(".slvbundle");
    Write(oversized.Path(), CompressZstd("0123456789\n{}\n"));
    Write(
        unterminated.Path(),
        CompressZstd(R"({"__slv_bundle__":{"formatVersion":1,"rowCount":0,"configuration":{}}})")
    );

    loglib::internal::DecompressingByteSource::Options capped;
    capped.discardFirstLine = true;
    capped.maxDiscardedFirstLineBytes = 8;
    CHECK_THROWS_AS(
        loglib::internal::DecompressingByteSource(oversized.Path(), {}, {}, capped),
        std::runtime_error
    );

    loglib::internal::DecompressingByteSource::Options normal;
    normal.discardFirstLine = true;
    CHECK_THROWS_AS(
        loglib::internal::DecompressingByteSource(unterminated.Path(), {}, {}, normal),
        std::runtime_error
    );
}
