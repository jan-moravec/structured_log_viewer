#include <loglib/core/file_line_source.hpp>
#include <loglib/ingest/decompressing_byte_source.hpp>
#include <loglib/ingest/tailing_bytes_producer.hpp>
#include <loglib/output/file_loading.hpp>
#include <loglib/output/session_bundle.hpp>
#include <loglib/schema/log_configuration.hpp>
#include <loglib/table/log_table.hpp>

#include <test_common/temp_dir.hpp>
#include <test_common/wait.hpp>

#include <catch2/catch_all.hpp>
#include <zstd.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace loglib;

namespace
{

constexpr std::string_view CONTENT =
    "{\"id\":1,\"msg\":\"first\"}\n{\"id\":2,\"msg\":\"second\"}\n{\"id\":3,\"msg\":\"third\"}\n";

/** A folder and a file name with characters outside every single-byte code page. */
struct PathPair
{
    std::filesystem::path ascii;
    std::filesystem::path unicode;
};

/** The same file name in an ASCII folder and in a folder with non-ASCII characters. */
PathPair MakePaths(const test_common::TempDir &dir, const std::string &asciiName, const std::u8string &unicodeName)
{
    const std::filesystem::path asciiFolder = dir.Path() / "plain_folder";
    const std::filesystem::path unicodeFolder = dir.Path() / std::filesystem::path(u8"Ordner_äöü_日本");
    std::filesystem::create_directories(asciiFolder);
    std::filesystem::create_directories(unicodeFolder);
    return PathPair{.ascii = asciiFolder / asciiName, .unicode = unicodeFolder / std::filesystem::path(unicodeName)};
}

void WriteBytes(const std::filesystem::path &path, std::string_view bytes)
{
    std::ofstream out(path, std::ios::binary);
    REQUIRE(out.is_open());
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

std::string ReadBytes(const std::filesystem::path &path)
{
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in.is_open());
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::string BytesOf(std::span<const char> bytes)
{
    return {bytes.data(), bytes.size()};
}

std::string Compress(std::string_view bytes)
{
    std::string out(ZSTD_compressBound(bytes.size()), '\0');
    const std::size_t size = ZSTD_compress(out.data(), out.size(), bytes.data(), bytes.size(), 3);
    REQUIRE_FALSE(ZSTD_isError(size));
    out.resize(size);
    return out;
}

/** The text of every cell of a table, row by row. */
std::vector<std::string> CellsOf(const LogTable &table)
{
    std::vector<std::string> cells;
    for (std::size_t row = 0; row < table.RowCount(); ++row)
    {
        for (std::size_t column = 0; column < table.ColumnCount(); ++column)
        {
            cells.push_back(table.GetHeader(column) + "=" + table.GetFormattedValue(row, column));
        }
    }
    return cells;
}

} // namespace

TEST_CASE("FileLineSource reads the same bytes through a non-ASCII path", "[paths][unicode]")
{
    const test_common::TempDir dir("unicode_file");
    const PathPair paths = MakePaths(dir, "Protokoll.log", u8"Protokoll_äöü_日本.log");
    WriteBytes(paths.ascii, CONTENT);
    WriteBytes(paths.unicode, CONTENT);

    const FileLineSource plain(paths.ascii);
    const FileLineSource unicode(paths.unicode);
    CHECK(BytesOf(unicode.StableBytes()) == BytesOf(plain.StableBytes()));
    CHECK(BytesOf(unicode.StableBytes()) == CONTENT);
    CHECK(unicode.Path() == paths.unicode);
}

TEST_CASE("OpenFileSource and LoadFile read the same rows through a non-ASCII path", "[paths][unicode]")
{
    const test_common::TempDir dir("unicode_load");
    const PathPair paths = MakePaths(dir, "Protokoll.jsonl", u8"Protokoll_äöü_日本.jsonl");
    WriteBytes(paths.ascii, CONTENT);
    WriteBytes(paths.unicode, CONTENT);

    const OpenedFile plainOpened = OpenFileSource(paths.ascii);
    const OpenedFile unicodeOpened = OpenFileSource(paths.unicode);
    REQUIRE(unicodeOpened.source != nullptr);
    CHECK(BytesOf(unicodeOpened.source->StableBytes()) == BytesOf(plainOpened.source->StableBytes()));
    CHECK(unicodeOpened.effectivePath == paths.unicode);

    const LoadResult plain = LoadFile(paths.ascii);
    const LoadResult unicode = LoadFile(paths.unicode);
    CHECK(unicode.errors.empty());
    REQUIRE(unicode.table.RowCount() == 3);
    CHECK(CellsOf(unicode.table) == CellsOf(plain.table));
}

TEST_CASE("A compressed copy opens through a non-ASCII path", "[paths][unicode]")
{
    const test_common::TempDir dir("unicode_zstd");
    const PathPair paths = MakePaths(dir, "Protokoll.jsonl.zst", u8"Protokoll_äöü_日本.jsonl.zst");
    WriteBytes(paths.ascii, Compress(CONTENT));
    WriteBytes(paths.unicode, Compress(CONTENT));

    const DecompressingByteSource plain(paths.ascii);
    const DecompressingByteSource unicode(paths.unicode);
    CHECK(ReadBytes(unicode.EffectivePath()) == CONTENT);
    CHECK(ReadBytes(unicode.EffectivePath()) == ReadBytes(plain.EffectivePath()));

    const OpenedFile opened = OpenFileSource(paths.unicode);
    REQUIRE(opened.source != nullptr);
    CHECK(BytesOf(opened.source->StableBytes()) == CONTENT);

    const LoadResult loaded = LoadFile(paths.unicode);
    CHECK(loaded.table.RowCount() == 3);
}

TEST_CASE("A session bundle is written and read through a non-ASCII path", "[paths][unicode]")
{
    const test_common::TempDir dir("unicode_bundle");
    const PathPair paths = MakePaths(dir, "Protokoll.jsonl", u8"Protokoll_äöü_日本.jsonl");
    WriteBytes(paths.ascii, CONTENT);
    const LoadResult loaded = LoadFile(paths.ascii);

    const std::filesystem::path plainBundle = paths.ascii.parent_path() / "Sitzung.slvbundle";
    const std::filesystem::path unicodeBundle =
        paths.unicode.parent_path() / std::filesystem::path(u8"Sitzung_äöü_日本.slvbundle");
    WriteSessionBundle(loaded.table, loaded.table.Configuration().Configuration(), plainBundle);
    WriteSessionBundle(loaded.table, loaded.table.Configuration().Configuration(), unicodeBundle);
    REQUIRE(std::filesystem::exists(unicodeBundle));

    const LoadResult fromPlain = LoadFile(plainBundle);
    const LoadResult fromUnicode = LoadFile(unicodeBundle);
    REQUIRE(fromUnicode.bundle.has_value());
    CHECK(fromUnicode.bundle->rowCount == 3);
    CHECK(CellsOf(fromUnicode.table) == CellsOf(fromPlain.table));
    CHECK(CellsOf(fromUnicode.table) == CellsOf(loaded.table));
}

TEST_CASE("LogConfigurationManager saves and loads through a non-ASCII path", "[paths][unicode]")
{
    const test_common::TempDir dir("unicode_config");
    const PathPair paths = MakePaths(dir, "Einstellungen.json", u8"Einstellungen_äöü_日本.json");

    LogConfiguration configuration;
    Column column;
    column.header = "Meldung";
    column.keys = {"msg"};
    configuration.columns.push_back(column);

    LogConfigurationManager manager;
    manager.SetConfiguration(configuration);
    manager.Save(paths.ascii);
    manager.Save(paths.unicode);
    REQUIRE(std::filesystem::exists(paths.unicode));
    CHECK(ReadBytes(paths.unicode) == ReadBytes(paths.ascii));

    LogConfigurationManager loaded;
    loaded.Load(paths.unicode);
    REQUIRE(loaded.Configuration().columns.size() == 1);
    CHECK(loaded.Configuration().columns.front().header == "Meldung");
    CHECK(loaded.Configuration().columns.front().keys == std::vector<std::string>{"msg"});
}

TEST_CASE("TailingBytesProducer reads the same bytes through a non-ASCII path", "[paths][unicode][fs-events]")
{
    const test_common::TempDir dir("unicode_tail");
    const PathPair paths = MakePaths(dir, "Protokoll.log", u8"Protokoll_äöü_日本.log");
    WriteBytes(paths.ascii, CONTENT);
    WriteBytes(paths.unicode, CONTENT);

    TailingBytesProducer::Options options;
    options.disableNativeWatcher = true;
    options.pollInterval = std::chrono::milliseconds{25};

    const auto readAll = [&](const std::filesystem::path &path) {
        TailingBytesProducer producer(path, /*retentionLines=*/100, options);
        std::string received;
        std::array<char, 256> buffer{};
        const bool complete = test_common::WaitUntil([&] {
            while (const std::size_t got = producer.Read(std::span<char>(buffer)))
            {
                received.append(buffer.data(), got);
            }
            return received.size() >= CONTENT.size();
        });
        REQUIRE(complete);
        return received;
    };

    CHECK(readAll(paths.unicode) == CONTENT);
    CHECK(readAll(paths.unicode) == readAll(paths.ascii));
}
