#include <loglib/core/error.hpp>
#include <loglib/core/stop_token.hpp>
#include <loglib/output/file_loading.hpp>
#include <loglib/output/session_bundle.hpp>
#include <loglib/parse/parsers/json_parser.hpp>
#include <loglib/table/log_table.hpp>
#include <loglib/table/table_loader.hpp>

#include <test_common/temp_dir.hpp>

#include <catch2/catch_all.hpp>

#include <zlib.h>
#include <zstd.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

using namespace loglib;

namespace
{

[[nodiscard]] std::string JsonRows(int count)
{
    std::string bytes;
    for (int i = 0; i < count; ++i)
    {
        bytes += R"({"timestamp":"2026-01-01T00:00:)" + std::string(i < 10 ? "0" : "") + std::to_string(i % 60) +
                 R"(Z","level":"info","n":)" + std::to_string(i) + R"(,"msg":"row )" + std::to_string(i) + "\"}\n";
    }
    return bytes;
}

[[nodiscard]] std::string Gzip(const std::string &input)
{
    z_stream strm{};
    REQUIRE(::deflateInit2(&strm, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) == Z_OK);
    std::string out(::deflateBound(&strm, static_cast<uLong>(input.size())), '\0');
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
    strm.next_in = const_cast<Bytef *>(reinterpret_cast<const Bytef *>(input.data()));
    strm.avail_in = static_cast<uInt>(input.size());
    strm.next_out = reinterpret_cast<Bytef *>(out.data());
    strm.avail_out = static_cast<uInt>(out.size());
    REQUIRE(::deflate(&strm, Z_FINISH) == Z_STREAM_END);
    out.resize(strm.total_out);
    ::deflateEnd(&strm);
    return out;
}

[[nodiscard]] std::string Zstd(const std::string &input)
{
    std::string out(::ZSTD_compressBound(input.size()), '\0');
    const std::size_t written = ::ZSTD_compress(out.data(), out.size(), input.data(), input.size(), 3);
    REQUIRE_FALSE(::ZSTD_isError(written));
    out.resize(written);
    return out;
}

struct Snapshot
{
    std::vector<std::string> headers;
    std::vector<std::vector<std::string>> cells;
    std::vector<ColumnType> types;

    friend bool operator==(const Snapshot &, const Snapshot &) = default;
};

[[nodiscard]] Snapshot SnapshotOf(const LogTable &table)
{
    Snapshot snapshot;
    for (size_t column = 0; column < table.ColumnCount(); ++column)
    {
        snapshot.headers.push_back(table.GetHeader(column));
        snapshot.types.push_back(table.Configuration().Configuration().columns[column].type);
    }
    for (size_t row = 0; row < table.RowCount(); ++row)
    {
        std::vector<std::string> cells;
        for (size_t column = 0; column < table.ColumnCount(); ++column)
        {
            cells.push_back(table.GetFormattedValue(row, column));
        }
        snapshot.cells.push_back(std::move(cells));
    }
    return snapshot;
}

} // namespace

TEST_CASE("LoadFile loads a plain file and converts the time column", "[file_loading]")
{
    const test_common::TempDir dir("file_loading");
    const auto path = dir.Write("plain.jsonl", JsonRows(30));

    const LoadResult result = LoadFile(path);

    CHECK(result.errors.empty());
    CHECK_FALSE(result.bundle.has_value());
    REQUIRE(result.table.RowCount() == 30);
    REQUIRE(result.table.ColumnCount() == 4);
    CHECK(result.table.GetHeader(0) == "timestamp");
    CHECK(result.table.Configuration().Configuration().columns[0].type == ColumnType::Time);
    CHECK(std::holds_alternative<TimeStamp>(result.table.GetValue(0, 0)));
    CHECK(result.table.Data().Lines().front().Source()->Path() == path);
}

TEST_CASE("LoadFile gives the same table for a gzip and a zstd copy", "[file_loading][compressed]")
{
    const test_common::TempDir dir("file_loading_compressed");
    const std::string content = JsonRows(50);
    const LoadResult plain = LoadFile(dir.Write("plain.jsonl", content));
    const LoadResult gzip = LoadFile(dir.Write("copy.jsonl.gz", Gzip(content)));
    const LoadResult zstd = LoadFile(dir.Write("copy.jsonl.zst", Zstd(content)));

    CHECK(SnapshotOf(gzip.table) == SnapshotOf(plain.table));
    CHECK(SnapshotOf(zstd.table) == SnapshotOf(plain.table));
    CHECK(gzip.table.RowCount() == 50);
    // Rows report the path the user opened, not the temporary copy.
    CHECK(gzip.table.Data().Lines().front().Source()->Path().filename() == "copy.jsonl.gz");
}

TEST_CASE("OpenFileSource deletes the unpacked copy when the source is destroyed", "[file_loading][compressed]")
{
    const test_common::TempDir dir("file_loading_temp");
    const auto path = dir.Write("copy.jsonl.gz", Gzip(JsonRows(10)));

    std::filesystem::path unpacked;
    {
        OpenedFile opened = OpenFileSource(path);
        unpacked = opened.effectivePath;
        CHECK(unpacked != path);
        CHECK(std::filesystem::exists(unpacked));
        CHECK(opened.source->Path() == path);
    }
    CHECK_FALSE(std::filesystem::exists(unpacked));
    CHECK(std::filesystem::exists(path));

    {
        const OpenedFile plain = OpenFileSource(dir.Write("plain.jsonl", JsonRows(3)));
        CHECK(plain.effectivePath == dir.Path() / "plain.jsonl");
    }
}

TEST_CASE("The unpacked copy stays until the table that holds its rows is destroyed", "[file_loading][compressed]")
{
    const test_common::TempDir dir("file_loading_table_temp");
    const auto path = dir.Write("copy.jsonl.gz", Gzip(JsonRows(10)));

    std::filesystem::path unpacked;
    {
        OpenedFile opened = OpenFileSource(path);
        unpacked = opened.effectivePath;
        LogTable table;
        const ParseOutcome outcome =
            ParseIntoTable(table, JsonParser{}, std::move(opened.source), ParserOptionsFor(table));
        CHECK(outcome.errors.empty());
        REQUIRE(table.RowCount() == 10);
        CHECK(std::filesystem::exists(unpacked));
        CHECK(table.GetFormattedValue(9, 2) == "9");
    }
    CHECK_FALSE(std::filesystem::exists(unpacked));
}

TEST_CASE("OpenFileSource and LoadFile throw only the documented exceptions", "[file_loading][errors]")
{
    const test_common::TempDir dir("file_loading_errors");

    SECTION("IoError for a missing file")
    {
        const auto path = dir.Path() / "missing.jsonl";
        CHECK_THROWS_AS(OpenFileSource(path), IoError);
        CHECK_THROWS_AS(LoadFile(path), IoError);
        CHECK_THROWS_WITH(LoadFile(path), Catch::Matchers::ContainsSubstring("does not exist"));
    }
    SECTION("IoError for an empty file")
    {
        const auto path = dir.Write("empty.jsonl", "");
        CHECK_THROWS_AS(LoadFile(path), IoError);
        CHECK_THROWS_WITH(LoadFile(path), Catch::Matchers::ContainsSubstring("is empty"));
    }
    SECTION("FormatError for content no parser accepts")
    {
        const auto path = dir.Write("unknown.bin", std::string("\x01\x02\x03 not a log \xff\xfe\n\x01\x02\n"));
        CHECK_THROWS_AS(LoadFile(path), FormatError);
        CHECK_THROWS_WITH(LoadFile(path), Catch::Matchers::ContainsSubstring("could not be parsed"));
    }
    SECTION("FormatError for corrupt compressed data")
    {
        std::string gzip = Gzip(JsonRows(2000));
        gzip.resize(gzip.size() / 2);
        const auto path = dir.Write("truncated.jsonl.gz", gzip);
        CHECK_THROWS_AS(OpenFileSource(path), FormatError);
        CHECK_THROWS_AS(LoadFile(path), FormatError);
    }
    SECTION("LimitError for the decompression size cap")
    {
        const auto path = dir.Write("big.jsonl.gz", Gzip(JsonRows(500)));
        OpenFileOptions options;
        options.maxDecompressedBytes = 100;
        CHECK_THROWS_AS(OpenFileSource(path, options), LimitError);
    }
    SECTION("OperationCancelled when the stop token has fired")
    {
        StopSource stopSource;
        stopSource.RequestStop();
        const auto path = dir.Write("cancel.jsonl.gz", Gzip(JsonRows(500)));
        OpenFileOptions options;
        options.stopToken = stopSource.GetToken();
        CHECK_THROWS_AS(OpenFileSource(path, options), OperationCancelled);

        LoadOptions loadOptions;
        loadOptions.stopToken = stopSource.GetToken();
        CHECK_THROWS_AS(LoadFile(dir.Write("cancel_plain.jsonl", JsonRows(500)), loadOptions), OperationCancelled);
    }
}

TEST_CASE("Only the bundle extension makes a file a bundle", "[file_loading][bundle]")
{
    const test_common::TempDir dir("file_loading_bundle_rule");
    const std::string content = JsonRows(20);

    SECTION("a plain zstd log opens as a log")
    {
        const LoadResult result = LoadFile(dir.Write("log.jsonl.zst", Zstd(content)));
        CHECK_FALSE(result.bundle.has_value());
        CHECK(result.table.RowCount() == 20);
    }
    SECTION("a bundle opens as a bundle")
    {
        const LoadResult original = LoadFile(dir.Write("log.jsonl", content));
        const auto bundlePath = dir.Path() / "session.slvbundle";
        WriteSessionBundle(original.table, original.table.Configuration().Configuration(), bundlePath);

        const LoadResult loaded = LoadFile(bundlePath);
        REQUIRE(loaded.bundle.has_value());
        CHECK(loaded.bundle->rowCount == 20);
        CHECK(loaded.bundle->formatVersion == SESSION_BUNDLE_FORMAT_VERSION);
        CHECK(loaded.table.RowCount() == 20);
        CHECK(loaded.table.GetHeader(0) == "timestamp");
        CHECK(SnapshotOf(loaded.table) == SnapshotOf(original.table));

        const OpenedFile opened = OpenFileSource(bundlePath);
        CHECK(opened.bundle.has_value());
    }
    SECTION("a zstd log with the bundle extension is a format error")
    {
        const auto path = dir.Write("renamed.slvbundle", Zstd(content));
        CHECK_THROWS_AS(OpenFileSource(path), FormatError);
        CHECK_THROWS_AS(LoadFile(path), FormatError);
    }
    SECTION("a plain log with the bundle extension is a format error")
    {
        CHECK_THROWS_AS(OpenFileSource(dir.Write("plain.slvbundle", content)), FormatError);
    }
    SECTION("a bundle with an unsupported version is a format error")
    {
        const std::string metadata = R"({"__slv_bundle__":{"formatVersion":2,"rowCount":0,"configuration":{}}})"
                                     "\n";
        CHECK_THROWS_AS(OpenFileSource(dir.Write("future.slvbundle", Zstd(metadata))), FormatError);
    }
    SECTION("a bundle without rows loads as an empty table")
    {
        const LoadResult original = LoadFile(dir.Write("log.jsonl", content));
        const auto bundlePath = dir.Path() / "empty.slvbundle";
        WriteSessionBundle(
            original.table, original.table.Configuration().Configuration(), std::span<const std::size_t>{}, bundlePath
        );
        const LoadResult loaded = LoadFile(bundlePath);
        CHECK(loaded.table.RowCount() == 0);
        REQUIRE(loaded.bundle.has_value());
        CHECK(loaded.bundle->rowCount == 0);
    }
}

TEST_CASE("LoadFile starts from the configuration it is given", "[file_loading][configuration]")
{
    const test_common::TempDir dir("file_loading_config");
    const auto path = dir.Write("plain.jsonl", JsonRows(5));

    LogConfiguration configuration;
    configuration.columns.push_back(
        {.header = "Message", .keys = {"msg"}, .printFormat = "{}", .type = ColumnType::Any}
    );
    LoadOptions options;
    options.configuration = configuration;

    const LoadResult result = LoadFile(path, options);
    // The timestamp column is added by the data and moves to the front, the configured column keeps its header.
    size_t message = result.table.ColumnCount();
    for (size_t column = 0; column < result.table.ColumnCount(); ++column)
    {
        if (result.table.GetHeader(column) == "Message")
        {
            message = column;
        }
    }
    REQUIRE(message < result.table.ColumnCount());
    CHECK(result.table.GetFormattedValue(2, message) == "row 2");
}
