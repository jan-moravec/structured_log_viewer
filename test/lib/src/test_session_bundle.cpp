
#include "parse_only.hpp"

#include <loglib/config/log_configuration.hpp>
#include <loglib/core/error.hpp>
#include <loglib/core/internal/path_encoding.hpp>
#include <loglib/core/log_value.hpp>
#include <loglib/core/path_key.hpp>
#include <loglib/ingest/decompressing_byte_source.hpp>
#include <loglib/output/file_loading.hpp>
#include <loglib/output/session_bundle.hpp>
#include <loglib/parse/parsers/json_parser.hpp>
#include <loglib/table/internal/timestamp_backfill.hpp>
#include <loglib/table/log_table.hpp>
#include <loglib/table/table_loader.hpp>

#include <test_common/log_line_debug.hpp>
#include <test_common/temp_dir.hpp>

#include <catch2/catch_all.hpp>
#include <zstd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{

/// Returns a directory private to this process, because test cases run
/// in parallel processes.
[[nodiscard]] const std::filesystem::path &ScratchRoot()
{
    static const test_common::TempDir ROOT("session_bundle");
    return ROOT.Path();
}

class TempPath
{
public:
    explicit TempPath(std::string suffix)
    {
        static std::atomic<unsigned> counter{0};
        mPath = ScratchRoot() / ("slv-session-bundle-" + std::to_string(++counter) + std::move(suffix));
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

    [[nodiscard]] const std::filesystem::path &Path() const noexcept
    {
        return mPath;
    }

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
        return std::ranges::any_of(iter, [&basename](const std::filesystem::directory_entry &entry) {
            const std::string name = entry.path().filename().string();
            return name != basename && name.starts_with(basename) && name.ends_with(".tmp");
        });
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
    loglib::LoadResult loaded = loglib::LoadFile(path);
    REQUIRE(loaded.errors.empty());
    return std::move(loaded.table);
}

/// Loads several JSON Lines files into one table, one source each.
[[nodiscard]] loglib::LogTable ParseTables(const std::vector<std::filesystem::path> &paths)
{
    loglib::LogTable table;
    const loglib::JsonParser parser;
    for (const auto &path : paths)
    {
        loglib::OpenedFile opened = loglib::OpenFileSource(path);
        const loglib::ParseOutcome outcome =
            loglib::ParseIntoTable(table, parser, std::move(opened.source), loglib::ParserOptionsFor(table));
        REQUIRE(outcome.errors.empty());
    }
    loglib::FinishLoading(table);
    return table;
}

[[nodiscard]] loglib::DecompressingByteSource DecodeBundle(const std::filesystem::path &path)
{
    loglib::DecompressingByteSource::Options options;
    options.discardFirstLine = true;
    return {path, {}, {}, options};
}

[[nodiscard]] std::string CompressZstd(std::string_view input)
{
    std::string compressed;
    compressed.resize(ZSTD_compressBound(input.size()));
    const std::size_t size = ZSTD_compress(compressed.data(), compressed.size(), input.data(), input.size(), 1);
    if (ZSTD_isError(size))
    {
        throw std::runtime_error(ZSTD_getErrorName(size));
    }
    compressed.resize(size);
    return compressed;
}

[[nodiscard]] loglib::FilterExpression SingleLeafExpression(loglib::FilterCondition rule)
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
    loglib::WriteSessionBundle(table, loglib::LogConfiguration{.columns = table.Layout().Columns()}, bundle.Path());

    REQUIRE(loglib::HasSessionBundleExtension(bundle.Path()));
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
    CHECK(std::get<std::int64_t>(test_common::ValueByName(row, restored.data.Keys(), "signed")) == -7);
    CHECK(std::get<std::uint64_t>(test_common::ValueByName(row, restored.data.Keys(), "unsigned")) == UINT64_MAX);
    CHECK(std::get<double>(test_common::ValueByName(row, restored.data.Keys(), "whole")) == Catch::Approx(2.0));
    CHECK(std::get<double>(test_common::ValueByName(row, restored.data.Keys(), "fraction")) == Catch::Approx(2.5));
    CHECK(std::get<bool>(test_common::ValueByName(row, restored.data.Keys(), "ok")));
    CHECK(
        loglib::AsStringView(test_common::ValueByName(row, restored.data.Keys(), "msg")) ==
        "quote: \" slash: \\ newline:\n"
    );
}

TEST_CASE("session bundle metadata names an unknown configuration key", "[SessionBundle]")
{
    const std::string metadata =
        R"({"__slv_bundle__":{"formatVersion":1,"rowCount":0,"configuration":{"version":1,"columns":[],"extraKey":1}}})";
    try
    {
        static_cast<void>(loglib::ParseSessionBundleMetadata(metadata));
        FAIL("the metadata was accepted");
    }
    catch (const loglib::FormatError &error)
    {
        CHECK(std::string(error.what()).contains("unknown key 'extraKey'"));
    }
}

TEST_CASE("session bundle metadata preserves investigation configuration and flattens source", "[SessionBundle]")
{
    const TempPath source(".jsonl");
    const TempPath bundle(".slvbundle");
    Write(
        source.Path(),
        R"({"service":"auth","count":4})"
        "\n"
    );
    const loglib::LogTable table = ParseTable(source.Path());

    loglib::LogConfiguration configuration;
    configuration.columns = {
        {.header = "Count renamed",
         .keys = {"count"},
         .printFormat = "%d",
         .type = loglib::ColumnType::Integer,
         .parseFormats = {},
         .visible = false,
         .levelMapping = {},
         .autoDetect = false},
        {.header = "Service",
         .keys = {"service"},
         .printFormat = {},
         .type = loglib::ColumnType::String,
         .parseFormats = {},
         .visible = true,
         .levelMapping = {},
         .autoDetect = false},
    };
    const loglib::FilterCondition filter{
        .type = loglib::FilterCondition::Type::String,
        .columnKeys = {"service"},
        .matchType = loglib::FilterCondition::Match::Exactly,
        .filterString = "auth",
        .filterBegin = std::nullopt,
        .filterEnd = std::nullopt,
        .filterMinValue = std::nullopt,
        .filterMaxValue = std::nullopt,
        .filterValues = {},
    };
    configuration.view.expression = SingleLeafExpression(filter);
    configuration.view.sort = {.columnKeys = {"service"}, .direction = loglib::SortDirection::Descending};
    configuration.source = loglib::SourceDescriptor{
        .kind = loglib::SourceDescriptor::Kind::NetworkStream,
        .format = {.format = loglib::LogFormat::Regex, .regex = {.pattern = "(?<service>.*)"}},
        .locators = {{.path = "tcp://127.0.0.1:9000", .key = "tcp://127.0.0.1:9000"}},
    };
    configuration.view.highlightRules = {
        {.name = "Auth",
         .enabled = true,
         .condition =
             {.type = loglib::FilterCondition::Type::String,
              .columnKeys = {"service"},
              .matchType = loglib::FilterCondition::Match::Contains,
              .filterString = "auth"},
         .style = {.foregroundIndex = 2, .backgroundIndex = 3, .bold = true, .italic = false}},
    };

    loglib::WriteSessionBundle(table, configuration, bundle.Path());
    auto decoded = DecodeBundle(bundle.Path());
    const loglib::LogConfiguration restored =
        loglib::ParseSessionBundleMetadata(decoded.DiscardedFirstLine()).configuration;

    REQUIRE(restored.columns.size() == 2);
    CHECK(restored.columns[0].header == "Count renamed");
    CHECK(restored.columns[0].keys == std::vector<std::string>{"count"});
    CHECK(restored.columns[0].visible == false);
    CHECK(restored.columns[0].type == loglib::ColumnType::Integer);
    CHECK(restored.columns[0].autoDetect == false);
    CHECK(restored.columns[1].header == "Service");
    CHECK(restored.columns[1].keys == std::vector<std::string>{"service"});
    CHECK(restored.columns[1].visible);
    CHECK(restored.view.expression == configuration.view.expression);
    CHECK(restored.view.sort.columnKeys == std::vector<std::string>{"service"});
    CHECK(restored.view.sort.direction == loglib::SortDirection::Descending);
    CHECK(restored.view.highlightRules == configuration.view.highlightRules);

    REQUIRE(restored.source.has_value());
    CHECK(restored.source->kind == loglib::SourceDescriptor::Kind::File);
    CHECK(restored.source->format.format == loglib::LogFormat::Json);
    // The embedded source is the bundle itself: its path is the destination as written and its key is the path key
    // that anchors refer to.
    REQUIRE(restored.source->locators.size() == 1);
    CHECK(restored.source->locators[0].path == bundle.Path().string());
    CHECK(restored.source->locators[0].key == loglib::MakePathKey(bundle.Path()));
    CHECK(restored.source->format.regex.pattern.empty());
}

TEST_CASE("session bundle rewrites Stdin source kind to File", "[SessionBundle]")
{
    // A stdin session's pipe is not reachable after the process
    // exits, so `WriteSessionBundle` must flatten
    // `Source::Kind::Stdin` down to `Kind::File` (pointing at the
    // bundle itself). Without the rewrite, a restored stdin
    // bundle would carry an on-disk "kind":"stdin" that older
    // builds cannot parse and that has no reopen path.
    const TempPath source(".jsonl");
    const TempPath bundle(".slvbundle");
    Write(
        source.Path(),
        R"({"msg":"one"})"
        "\n"
    );
    const loglib::LogTable table = ParseTable(source.Path());

    loglib::LogConfiguration configuration;
    configuration.source = loglib::SourceDescriptor{
        .kind = loglib::SourceDescriptor::Kind::Stdin,
        .format = {.format = loglib::LogFormat::Logfmt},
        .locators = {{.path = "<stdin>", .key = "<stdin>"}},
    };

    loglib::WriteSessionBundle(table, configuration, bundle.Path());
    const auto decoded = DecodeBundle(bundle.Path());
    const loglib::LogConfiguration restored =
        loglib::ParseSessionBundleMetadata(decoded.DiscardedFirstLine()).configuration;

    REQUIRE(restored.source.has_value());
    CHECK(restored.source->kind == loglib::SourceDescriptor::Kind::File);
    REQUIRE(restored.source->locators.size() == 1);
    CHECK(restored.source->locators[0].path == bundle.Path().string());
    CHECK(restored.source->locators[0].key == loglib::MakePathKey(bundle.Path()));
}

TEST_CASE("session bundle anchors are densely remapped and detached anchors are dropped", "[SessionBundle]")
{
    const TempPath sourceA(".a.jsonl");
    const TempPath sourceB(".b.jsonl");
    const TempPath bundle(".slvbundle");
    Write(
        sourceA.Path(),
        R"({"msg":"a0"})"
        "\n"
        R"({"msg":"a1"})"
        "\n"
    );
    Write(
        sourceB.Path(),
        R"({"msg":"b0"})"
        "\n"
    );

    loglib::LogTable table = ParseTables({sourceA.Path(), sourceB.Path()});

    loglib::LogConfiguration configuration{.columns = table.Layout().Columns()};
    configuration.view.anchors = {
        {.pathKey = loglib::MakePathKey(sourceA.Path()), .lineId = 1, .colorIndex = 1, .note = "second A"},
        {.pathKey = loglib::MakePathKey(sourceB.Path()), .lineId = 0, .colorIndex = 2, .note = "first B"},
        {.pathKey = loglib::MakePathKey("detached.json"), .lineId = 99, .colorIndex = 3, .note = "drop me"},
    };
    loglib::WriteSessionBundle(table, configuration, bundle.Path());

    auto decoded = DecodeBundle(bundle.Path());
    const auto anchors = loglib::ParseSessionBundleMetadata(decoded.DiscardedFirstLine()).configuration.view.anchors;
    REQUIRE(anchors.size() == 2);
    CHECK(anchors[0].pathKey == loglib::MakePathKey(bundle.Path()));
    CHECK(anchors[0].lineId == 1);
    CHECK(anchors[0].note == "second A");
    CHECK(anchors[1].pathKey == loglib::MakePathKey(bundle.Path()));
    CHECK(anchors[1].lineId == 2);
    CHECK(anchors[1].note == "first B");
}

TEST_CASE("session bundle matches anchors to sources by path key whatever the spelling", "[SessionBundle]")
{
    const TempPath source(".jsonl");
    const TempPath bundle(".slvbundle");
    Write(
        source.Path(),
        R"({"msg":"a0"})"
        "\n"
        R"({"msg":"a1"})"
        "\n"
    );
    loglib::LogTable table = ParseTable(source.Path());

    // The key of a roundabout spelling of the same file, as a saved session may hold it.
    const std::string roundabout = loglib::MakePathKey(source.Path().parent_path() / "." / source.Path().filename());

    loglib::LogConfiguration configuration{.columns = table.Layout().Columns()};
    configuration.view.anchors = {
        {.pathKey = roundabout, .lineId = 0, .colorIndex = 1, .note = "kept"},
        {.pathKey = roundabout, .lineId = 1, .colorIndex = 2, .note = "also kept"},
        {.pathKey = "not/a/key", .lineId = 0, .colorIndex = 3, .note = "other file - drop"},
    };
    loglib::WriteSessionBundle(table, configuration, bundle.Path());

    auto decoded = DecodeBundle(bundle.Path());
    const auto anchors = loglib::ParseSessionBundleMetadata(decoded.DiscardedFirstLine()).configuration.view.anchors;
    REQUIRE(anchors.size() == 2);
    // Anchors flatten to the path key of the bundle, which is the key of its embedded source.
    const std::string bundleKey = loglib::MakePathKey(bundle.Path());
    CHECK(anchors[0].pathKey == bundleKey);
    CHECK(anchors[0].lineId == 0);
    CHECK(anchors[0].note == "kept");
    CHECK(anchors[1].pathKey == bundleKey);
    CHECK(anchors[1].lineId == 1);
    CHECK(anchors[1].note == "also kept");
}

TEST_CASE("session bundle drops anchors whose `(path key, lineId)` matches multiple flattened rows", "[SessionBundle]")
{
    // The same file loaded twice gives two rows with the same `(path key, lineId)`.
    const TempPath source(".jsonl");
    const TempPath bundle(".slvbundle");
    Write(
        source.Path(),
        R"({"msg":"a0"})"
        "\n"
    );

    loglib::LogTable table = ParseTables({source.Path(), source.Path()});

    loglib::LogConfiguration configuration{.columns = table.Layout().Columns()};
    configuration.view.anchors = {
        {.pathKey = loglib::MakePathKey(source.Path()), .lineId = 0, .colorIndex = 1, .note = "ambiguous - drop me"},
    };
    loglib::WriteSessionBundle(table, configuration, bundle.Path());

    auto decoded = DecodeBundle(bundle.Path());
    const auto anchors = loglib::ParseSessionBundleMetadata(decoded.DiscardedFirstLine()).configuration.view.anchors;
    CHECK(anchors.empty());
}

TEST_CASE("session bundle drops anchors with an empty path key", "[SessionBundle]")
{
    // An empty path key cannot identify a source row.
    const TempPath source(".jsonl");
    const TempPath bundle(".slvbundle");
    Write(
        source.Path(),
        R"({"msg":"a0"})"
        "\n"
        R"({"msg":"a1"})"
        "\n"
    );
    loglib::LogTable table = ParseTable(source.Path());

    loglib::LogConfiguration configuration{.columns = table.Layout().Columns()};
    configuration.view.anchors = {
        {.pathKey = "", .lineId = 0, .colorIndex = 1, .note = "empty key - drop"},
        {.pathKey = "", .lineId = 1, .colorIndex = 2, .note = "empty key - drop"},
    };

    loglib::WriteSessionBundle(table, configuration, bundle.Path());

    auto decoded = DecodeBundle(bundle.Path());
    const auto anchors = loglib::ParseSessionBundleMetadata(decoded.DiscardedFirstLine()).configuration.view.anchors;
    CHECK(anchors.empty());
}

TEST_CASE("session bundle round-trips non-ASCII destination paths", "[SessionBundle]")
{
    // Non-ASCII destinations must survive writing and reopening.
    const TempPath source(".jsonl");
    Write(
        source.Path(),
        R"({"msg":"row"})"
        "\n"
    );
    loglib::LogTable table = ParseTable(source.Path());

    static std::atomic<unsigned> counter{0};
    // Use explicit UTF-8 bytes to avoid compiler charset dependence.
    const std::string utf8Filename = "slv-bundle-"
                                     "\xC5\xBE"     // U+017E z-with-caron
                                     "\xC3\xA9"     // U+00E9 e-acute
                                     "\xE6\x97\xA5" // U+65E5 CJK "day"
                                     "-" +
                                     std::to_string(++counter) + ".slvbundle";
    // Avoid Windows active-code-page conversion.
    const std::filesystem::path bundlePath = ScratchRoot() / loglib::internal::Utf8ToPath(utf8Filename);
    std::error_code cleanupError;
    std::filesystem::remove(bundlePath, cleanupError);
    try
    {
        loglib::WriteSessionBundle(table, loglib::LogConfiguration{.columns = table.Layout().Columns()}, bundlePath);
        REQUIRE(std::filesystem::exists(bundlePath));
        CHECK(loglib::HasSessionBundleExtension(bundlePath));

        loglib::DecompressingByteSource::Options options;
        options.discardFirstLine = true;
        const loglib::DecompressingByteSource decoded(bundlePath, {}, {}, options);
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
    Write(
        source.Path(),
        R"({"msg":"row"})"
        "\n"
    );
    Write(bundle.Path(), "existing destination");
    loglib::LogTable table = ParseTable(source.Path());

    loglib::StopSource stop;
    stop.RequestStop();
    loglib::SessionBundleWriteOptions options;
    options.stopToken = stop.GetToken();
    CHECK_THROWS_AS(
        loglib::WriteSessionBundle(
            table, loglib::LogConfiguration{.columns = table.Layout().Columns()}, bundle.Path(), options
        ),
        loglib::OperationCancelled
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
    options.stopToken = stop.GetToken();
    options.progress = [&stop](std::uint64_t written, std::uint64_t) {
        if (written == 4096)
        {
            stop.RequestStop();
        }
    };
    CHECK_THROWS_AS(
        loglib::WriteSessionBundle(
            table, loglib::LogConfiguration{.columns = table.Layout().Columns()}, bundle.Path(), options
        ),
        loglib::OperationCancelled
    );
    CHECK(Read(bundle.Path()) == "existing destination");
    CHECK_FALSE(bundle.HasStagingFiles());
}

TEST_CASE("successful session bundle write atomically replaces destination", "[SessionBundle]")
{
    const TempPath source(".jsonl");
    const TempPath bundle(".slvbundle");
    Write(
        source.Path(),
        R"({"msg":"replacement"})"
        "\n"
    );
    Write(bundle.Path(), "old destination");
    loglib::LogTable table = ParseTable(source.Path());

    loglib::WriteSessionBundle(table, loglib::LogConfiguration{.columns = table.Layout().Columns()}, bundle.Path());

    CHECK(Read(bundle.Path()) != "old destination");
    CHECK(loglib::HasSessionBundleExtension(bundle.Path()));
    CHECK_FALSE(bundle.HasStagingFiles());
    auto decoded = DecodeBundle(bundle.Path());
    CHECK(loglib::ParseSessionBundleMetadata(decoded.DiscardedFirstLine()).rowCount == 1);
}

TEST_CASE("session bundle writer preserves unrelated .tmp siblings in the destination directory", "[SessionBundle]")
{
    // The writer must not remove unrelated `.tmp` siblings.
    const TempPath source(".jsonl");
    const TempPath bundle(".slvbundle");
    Write(
        source.Path(),
        R"({"msg":"only"})"
        "\n"
    );

    // Plant a decoy that resembles, but is not, a staging file.
    const std::filesystem::path decoy = std::filesystem::path(bundle.Path().string() + ".manual-decoy.tmp");
    constexpr std::string_view DECOY_CONTENTS = "manual-decoy-must-survive-write";
    Write(decoy, DECOY_CONTENTS);

    loglib::LogTable table = ParseTable(source.Path());
    loglib::WriteSessionBundle(table, loglib::LogConfiguration{.columns = table.Layout().Columns()}, bundle.Path());

    CHECK(loglib::HasSessionBundleExtension(bundle.Path()));
    REQUIRE(std::filesystem::exists(decoy));
    CHECK(Read(decoy) == std::string(DECOY_CONTENTS));

    // Remove the decoy before fixture cleanup.
    std::error_code cleanupError;
    std::filesystem::remove(decoy, cleanupError);
}

TEST_CASE("session bundle metadata rejects malformed envelope versions and excessive rows", "[SessionBundle]")
{
    CHECK_THROWS_AS(loglib::ParseSessionBundleMetadata("not json"), loglib::FormatError);
    CHECK_THROWS_AS(loglib::ParseSessionBundleMetadata("{}"), loglib::FormatError);
    CHECK_THROWS_AS(
        loglib::ParseSessionBundleMetadata(R"({"__slv_bundle__":{"formatVersion":0,"rowCount":0,"configuration":{}}})"),
        loglib::FormatError
    );
    CHECK_THROWS_AS(
        loglib::ParseSessionBundleMetadata(R"({"__slv_bundle__":{"formatVersion":2,"rowCount":0,"configuration":{}}})"),
        loglib::FormatError
    );
    CHECK_THROWS_AS(
        loglib::ParseSessionBundleMetadata(
            R"({"__slv_bundle__":{"formatVersion":1,"rowCount":1000000001,"configuration":{}}})"
        ),
        loglib::FormatError
    );
}

TEST_CASE("HasSessionBundleExtension is the one rule for what a bundle is", "[SessionBundle]")
{
    CHECK(loglib::HasSessionBundleExtension("session.slvbundle"));
    CHECK(loglib::HasSessionBundleExtension("dir/SESSION.SLVBundle"));
    CHECK_FALSE(loglib::HasSessionBundleExtension("session.zst"));
    CHECK_FALSE(loglib::HasSessionBundleExtension("session.jsonl"));
    CHECK_FALSE(loglib::HasSessionBundleExtension("slvbundle"));
    CHECK_FALSE(loglib::HasSessionBundleExtension("session.slvbundle.gz"));
}

TEST_CASE("session bundle round-trips ISO-8601 Time column values", "[SessionBundle]")
{
    // Normalized UTC timestamps must remain typed after reopening.
    const TempPath source(".jsonl");
    const TempPath bundle(".slvbundle");
    Write(
        source.Path(),
        R"({"ts":"2025-04-25T12:34:56.123456","msg":"row0"})"
        "\n"
        R"({"ts":"2025-04-25T12:34:57.789012","msg":"row1"})"
        "\n"
    );

    loglib::LogTable table = ParseTable(source.Path());
    // Confirm the source table contains typed timestamps.
    REQUIRE(table.Data().Lines().size() == 2);
    const loglib::TimeStamp originalRow0 =
        std::get<loglib::TimeStamp>(test_common::ValueByName(table.Data().Lines()[0], table.Data().Keys(), "ts"));
    const loglib::TimeStamp originalRow1 =
        std::get<loglib::TimeStamp>(test_common::ValueByName(table.Data().Lines()[1], table.Data().Keys(), "ts"));

    loglib::WriteSessionBundle(table, loglib::LogConfiguration{.columns = table.Layout().Columns()}, bundle.Path());

    auto decoded = DecodeBundle(bundle.Path());
    const std::string jsonl = Read(decoded.EffectivePath());
    // Confirm the normalized on-disk UTC form.
    CHECK(jsonl.contains(R"("ts":"2025-04-25T12:34:56.123456Z")"));

    const loglib::LogTable reloaded = ParseTable(decoded.EffectivePath());
    REQUIRE(reloaded.Data().Lines().size() == 2);
    CHECK(
        std::get<loglib::TimeStamp>(
            test_common::ValueByName(reloaded.Data().Lines()[0], reloaded.Data().Keys(), "ts")
        ) == originalRow0
    );
    CHECK(
        std::get<loglib::TimeStamp>(
            test_common::ValueByName(reloaded.Data().Lines()[1], reloaded.Data().Keys(), "ts")
        ) == originalRow1
    );
}

TEST_CASE("session bundle prepends bundle-canonical Time parseFormat to embedded configuration", "[SessionBundle]")
{
    // Embedded Time columns must parse the normalized bundle format.
    const TempPath source(".jsonl");
    const TempPath bundle(".slvbundle");
    Write(
        source.Path(),
        R"({"ts":"25/04/2025 12:34:56","msg":"row"})"
        "\n"
    );
    loglib::LogTable table = ParseTable(source.Path());

    loglib::LogConfiguration configuration{.columns = table.Layout().Columns()};
    // Use a custom source format with auto-detection disabled.
    for (auto &column : configuration.columns)
    {
        if (column.keys.size() == 1 && column.keys.front() == "ts")
        {
            column.type = loglib::ColumnType::Time;
            column.parseFormats = {"%d/%m/%Y %H:%M:%S"};
            column.autoDetect = false;
        }
    }

    loglib::WriteSessionBundle(table, configuration, bundle.Path());
    auto decoded = DecodeBundle(bundle.Path());
    const loglib::LogConfiguration restored =
        loglib::ParseSessionBundleMetadata(decoded.DiscardedFirstLine()).configuration;

    const auto tsColumn = std::ranges::find_if(restored.columns, [](const loglib::Column &column) {
        return column.keys.size() == 1 && column.keys.front() == "ts";
    });
    REQUIRE(tsColumn != restored.columns.end());
    CHECK(tsColumn->type == loglib::ColumnType::Time);
    REQUIRE(tsColumn->parseFormats.size() >= 2);
    // Keep the fast ISO parser first.
    CHECK(tsColumn->parseFormats.front() == "%FT%T");
    CHECK(std::ranges::find(tsColumn->parseFormats, std::string("%d/%m/%Y %H:%M:%S")) != tsColumn->parseFormats.end());
}

TEST_CASE("bundle metadata extraction enforces first-line cap and newline", "[SessionBundle]")
{
    const TempPath oversized(".slvbundle");
    const TempPath unterminated(".slvbundle");
    Write(oversized.Path(), CompressZstd("0123456789\n{}\n"));
    Write(
        unterminated.Path(), CompressZstd(R"({"__slv_bundle__":{"formatVersion":1,"rowCount":0,"configuration":{}}})")
    );

    loglib::DecompressingByteSource::Options capped;
    capped.discardFirstLine = true;
    capped.maxDiscardedFirstLineBytes = 8;
    CHECK_THROWS_AS(loglib::DecompressingByteSource(oversized.Path(), {}, {}, capped), loglib::LimitError);

    loglib::DecompressingByteSource::Options normal;
    normal.discardFirstLine = true;
    CHECK_THROWS_AS(loglib::DecompressingByteSource(unterminated.Path(), {}, {}, normal), loglib::FormatError);
}

TEST_CASE("WriteSessionBundle with a row list writes exactly those rows in that order", "[SessionBundle][row_list]")
{
    const TempPath source(".jsonl");
    std::string content;
    for (int i = 0; i < 5; ++i)
    {
        content += R"({"n":)" + std::to_string(i) + "}\n";
    }
    Write(source.Path(), content);
    loglib::LogTable table = ParseTable(source.Path());
    REQUIRE(table.RowCount() == 5);
    const loglib::LogConfiguration configuration{.columns = table.Layout().Columns()};

    SECTION("a list of rows")
    {
        const TempPath bundle(".slvbundle");
        const std::vector<std::size_t> rows = {3, 1, 4};
        loglib::WriteSessionBundle(table, configuration, rows, bundle.Path());

        const auto decoded = DecodeBundle(bundle.Path());
        CHECK(loglib::ParseSessionBundleMetadata(decoded.DiscardedFirstLine()).rowCount == 3);
        CHECK(Read(decoded.EffectivePath()) == "{\"n\":3}\n{\"n\":1}\n{\"n\":4}\n");
        CHECK(table.RowCount() == 5);
    }
    SECTION("an empty list writes no rows")
    {
        const TempPath bundle(".slvbundle");
        loglib::WriteSessionBundle(table, configuration, std::span<const std::size_t>{}, bundle.Path());

        const auto decoded = DecodeBundle(bundle.Path());
        CHECK(loglib::ParseSessionBundleMetadata(decoded.DiscardedFirstLine()).rowCount == 0);
        CHECK(Read(decoded.EffectivePath()).empty());
    }
    SECTION("no list writes every row")
    {
        const TempPath bundle(".slvbundle");
        loglib::WriteSessionBundle(table, configuration, bundle.Path());

        const auto decoded = DecodeBundle(bundle.Path());
        CHECK(loglib::ParseSessionBundleMetadata(decoded.DiscardedFirstLine()).rowCount == 5);
    }
}
