#include <loglib/internal/decompressing_byte_source.hpp>
#include <loglib/log_configuration.hpp>
#include <loglib/log_table.hpp>
#include <loglib/log_value.hpp>
#include <loglib/parse_file.hpp>
#include <loglib/session_bundle.hpp>

#include <catch2/catch_all.hpp>
#include <zstd.h>

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
        std::error_code error;
        std::filesystem::remove(mPath, error);
        std::filesystem::remove(TemporaryPath(), error);
    }

    TempPath(const TempPath &) = delete;
    TempPath &operator=(const TempPath &) = delete;
    TempPath(TempPath &&) = delete;
    TempPath &operator=(TempPath &&) = delete;

    [[nodiscard]] const std::filesystem::path &Path() const noexcept { return mPath; }

    [[nodiscard]] std::filesystem::path TemporaryPath() const
    {
        std::filesystem::path temporary = mPath;
        temporary += ".tmp";
        return temporary;
    }

private:
    std::filesystem::path mPath;
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
    return loglib::LogTable(std::move(parsed.data), std::move(manager));
}

[[nodiscard]] loglib::internal::DecompressingByteSource DecodeBundle(const std::filesystem::path &path)
{
    loglib::internal::DecompressingByteSource::Options options;
    options.discardFirstLine = true;
    return loglib::internal::DecompressingByteSource(path, {}, {}, options);
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
    TempPath source(".jsonl");
    TempPath bundle(".slvbundle");
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
    TempPath source(".jsonl");
    TempPath bundle(".slvbundle");
    Write(source.Path(), R"({"service":"auth","count":4})" "\n");
    loglib::LogTable table = ParseTable(source.Path());

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
    loglib::LeafRule filter{
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
    CHECK(restored.source->locators == std::vector<std::string>{bundle.Path().string()});
    CHECK(restored.source->locatorDedupKeys == std::vector<std::string>{bundle.Path().string()});
    CHECK(restored.source->regexPattern.empty());
}

TEST_CASE("session bundle anchors are densely remapped and detached anchors are dropped", "[SessionBundle]")
{
    TempPath sourceA(".a.jsonl");
    TempPath sourceB(".b.jsonl");
    TempPath bundle(".slvbundle");
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
    // Simulates the production wiring where `AnchorManager` stores
    // anchor locators canonicalized (lowercased on Windows,
    // forward-slashed) while `line.Source()->Path()` still holds the
    // raw filesystem path. Without the callback the writer would
    // silently drop every anchor because `path::u8string()` doesn't
    // match the canonical form.
    TempPath source(".jsonl");
    TempPath bundle(".slvbundle");
    Write(source.Path(), R"({"msg":"a0"})" "\n" R"({"msg":"a1"})" "\n");
    loglib::LogTable table = ParseTable(source.Path());

    // Force a canonical form that does not equal `path::u8string()`.
    const std::string canonicalLocator = "canonical://" + source.Path().string();

    loglib::LogConfiguration configuration = table.Configuration().Configuration();
    configuration.anchors = {
        {.locator = canonicalLocator, .lineId = 0, .colorIndex = 1, .note = "kept"},
        {.locator = canonicalLocator, .lineId = 1, .colorIndex = 2, .note = "also kept"},
    };

    // Sanity: without the callback the writer drops both anchors --
    // the canonical form does not match `PathToUtf8(source->Path())`.
    {
        TempPath unbridged(".slvbundle");
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
    CHECK(anchors[0].locator == bundle.Path().string());
    CHECK(anchors[0].lineId == 0);
    CHECK(anchors[0].note == "kept");
    CHECK(anchors[1].locator == bundle.Path().string());
    CHECK(anchors[1].lineId == 1);
    CHECK(anchors[1].note == "also kept");
}

TEST_CASE("session bundle cancellation before write preserves destination and leaves no temp", "[SessionBundle]")
{
    TempPath source(".jsonl");
    TempPath bundle(".slvbundle");
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
    CHECK_FALSE(std::filesystem::exists(bundle.TemporaryPath()));
}

TEST_CASE("session bundle cancellation from progress preserves destination and removes temp", "[SessionBundle]")
{
    TempPath source(".jsonl");
    TempPath bundle(".slvbundle");
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
    CHECK_FALSE(std::filesystem::exists(bundle.TemporaryPath()));
}

TEST_CASE("successful session bundle write atomically replaces destination", "[SessionBundle]")
{
    TempPath source(".jsonl");
    TempPath bundle(".slvbundle");
    Write(source.Path(), R"({"msg":"replacement"})" "\n");
    Write(bundle.Path(), "old destination");
    loglib::LogTable table = ParseTable(source.Path());

    loglib::WriteSessionBundle(table, table.Configuration().Configuration(), bundle.Path());

    CHECK(Read(bundle.Path()) != "old destination");
    CHECK(loglib::LooksLikeSessionBundle(bundle.Path()));
    CHECK_FALSE(std::filesystem::exists(bundle.TemporaryPath()));
    auto decoded = DecodeBundle(bundle.Path());
    CHECK(loglib::ParseSessionBundleMetadata(decoded.DiscardedFirstLine()).rowCount == 1);
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
    TempPath plain(".jsonl");
    TempPath bundle(".slvbundle");
    Write(plain.Path(), R"({"msg":"plain"})" "\n");
    CHECK_FALSE(loglib::LooksLikeSessionBundle(plain.Path()));

    loglib::LogTable table = ParseTable(plain.Path());
    loglib::WriteSessionBundle(table, table.Configuration().Configuration(), bundle.Path());
    CHECK(loglib::LooksLikeSessionBundle(bundle.Path()));
}

TEST_CASE("bundle metadata extraction enforces first-line cap and newline", "[SessionBundle]")
{
    TempPath oversized(".slvbundle");
    TempPath unterminated(".slvbundle");
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
