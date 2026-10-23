#include "slp_process.hpp"

#include <loglib/log_configuration.hpp>
#include <loglib/log_table.hpp>
#include <loglib/parse_file.hpp>
#include <loglib/session_bundle.hpp>

#include <catch2/catch_test_macros.hpp>

#include <bzlib.h>
#include <lzma.h>
#include <zlib.h>
#include <zstd.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

using slp::test::FixturesDir;
using slp::test::RunSlp;
using slp::test::RunSlpWithStdin;

namespace
{

[[nodiscard]] std::string ReadBytes(const std::filesystem::path &path)
{
    std::ifstream input(path, std::ios::binary);
    REQUIRE(static_cast<bool>(input));
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void WriteBytes(const std::filesystem::path &path, std::string_view bytes)
{
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    REQUIRE(static_cast<bool>(output));
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    REQUIRE(static_cast<bool>(output));
}

[[nodiscard]] std::vector<std::uint8_t> CompressGzip(const std::string &input)
{
    z_stream strm{};
    REQUIRE(::deflateInit2(&strm, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) == Z_OK);
    std::vector<std::uint8_t> out;
    out.resize(::deflateBound(&strm, static_cast<uLong>(input.size())));
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
    strm.next_in = const_cast<Bytef *>(reinterpret_cast<const Bytef *>(input.data()));
    strm.avail_in = static_cast<uInt>(input.size());
    strm.next_out = out.data();
    strm.avail_out = static_cast<uInt>(out.size());
    REQUIRE(::deflate(&strm, Z_FINISH) == Z_STREAM_END);
    out.resize(strm.total_out);
    ::deflateEnd(&strm);
    return out;
}

[[nodiscard]] std::vector<std::uint8_t> CompressBzip2(const std::string &input)
{
    std::vector<std::uint8_t> out;
    auto outLen = static_cast<unsigned int>(input.size() + (input.size() / 100) + 800);
    out.resize(outLen);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
    char *inputBuf = const_cast<char *>(input.data());
    REQUIRE(
        ::BZ2_bzBuffToBuffCompress(
            reinterpret_cast<char *>(out.data()), &outLen, inputBuf, static_cast<unsigned int>(input.size()), 9, 0, 30
        ) == BZ_OK
    );
    out.resize(outLen);
    return out;
}

[[nodiscard]] std::vector<std::uint8_t> CompressXz(const std::string &input)
{
    lzma_stream strm = LZMA_STREAM_INIT;
    REQUIRE(::lzma_easy_encoder(&strm, 3, LZMA_CHECK_CRC64) == LZMA_OK);
    std::vector<std::uint8_t> out(input.size() + 1024);
    strm.next_in = reinterpret_cast<const std::uint8_t *>(input.data());
    strm.avail_in = input.size();
    strm.next_out = out.data();
    strm.avail_out = out.size();
    lzma_ret rc = LZMA_OK;
    while (rc != LZMA_STREAM_END)
    {
        if (strm.avail_out == 0)
        {
            const std::size_t was = out.size();
            out.resize(was * 2);
            strm.next_out = out.data() + was;
            strm.avail_out = was;
        }
        rc = ::lzma_code(&strm, LZMA_FINISH);
        REQUIRE((rc == LZMA_OK || rc == LZMA_STREAM_END));
    }
    out.resize(strm.total_out);
    ::lzma_end(&strm);
    return out;
}

[[nodiscard]] std::vector<std::uint8_t> CompressZstd(const std::string &input)
{
    const std::size_t bound = ::ZSTD_compressBound(input.size());
    std::vector<std::uint8_t> out(bound);
    const std::size_t written = ::ZSTD_compress(out.data(), bound, input.data(), input.size(), 3);
    REQUIRE(!::ZSTD_isError(written));
    out.resize(written);
    return out;
}

void WriteCompressed(const std::filesystem::path &path, const std::vector<std::uint8_t> &bytes)
{
    WriteBytes(path, std::string_view(reinterpret_cast<const char *>(bytes.data()), bytes.size()));
}

[[nodiscard]] std::filesystem::path ScratchRoot()
{
    const auto root = std::filesystem::temp_directory_path() / "slp_cli_sources";
    std::error_code ec;
    std::filesystem::create_directories(root, ec);
    return root;
}

[[nodiscard]] std::string EmptyTemplates()
{
    return (FixturesDir() / "empty_templates").string();
}

[[nodiscard]] std::string TwoLinesPath()
{
    return (FixturesDir() / "two_lines.jsonl").string();
}

[[nodiscard]] std::string TwoLinesBytes()
{
    return ReadBytes(FixturesDir() / "two_lines.jsonl");
}

void ExpectTwoLinesCount(const slp::test::CommandResult &result)
{
    INFO("stdout:\n" << result.out);
    INFO("stderr:\n" << result.err);
    REQUIRE(result.exitCode == 0);
    REQUIRE(result.out == "2\n");
}

[[nodiscard]] loglib::LogTable ParseFixtureTable()
{
    loglib::ParseResult parsed = loglib::ParseFile(FixturesDir() / "two_lines.jsonl");
    REQUIRE(parsed.errors.empty());
    loglib::LogConfigurationManager manager;
    manager.Update(parsed.data);
    return {std::move(parsed.data), std::move(manager)};
}

} // namespace

TEST_CASE("slp query reads a plain JSONL file", "[cli][e2e][sources]")
{
    const auto result = RunSlp({"query", "--tz", "UTC", "--template-dir", EmptyTemplates(), "--count", TwoLinesPath()});
    ExpectTwoLinesCount(result);
}

TEST_CASE("slp query reads compressed JSONL codecs", "[cli][e2e][sources]")
{
    const std::string plain = TwoLinesBytes();
    const auto root = ScratchRoot();

    struct Case
    {
        const char *suffix;
        std::vector<std::uint8_t> (*compress)(const std::string &);
    };
    const Case cases[] = {
        {".jsonl.gz", &CompressGzip},
        {".jsonl.bz2", &CompressBzip2},
        {".jsonl.xz", &CompressXz},
        {".jsonl.zst", &CompressZstd},
    };

    for (const Case &entry : cases)
    {
        INFO("codec " << entry.suffix);
        const auto path = root / (std::string("two_lines") + entry.suffix);
        WriteCompressed(path, entry.compress(plain));
        const auto result =
            RunSlp({"query", "--tz", "UTC", "--template-dir", EmptyTemplates(), "--count", path.string()});
        ExpectTwoLinesCount(result);
    }
}

TEST_CASE("slp query concatenates multiple files in caller order", "[cli][e2e][sources]")
{
    const auto root = ScratchRoot();
    const auto first = root / "multi_a.jsonl";
    const auto second = root / "multi_b.jsonl";
    WriteBytes(first, std::string(R"({"ts":"2024-01-01T00:00:00Z","level":"info","msg":"first"})") + "\n");
    WriteBytes(second, std::string(R"({"ts":"2024-01-01T00:00:01Z","level":"info","msg":"second"})") + "\n");

    const auto result = RunSlp(
        {"query",
         "--tz",
         "UTC",
         "--template-dir",
         EmptyTemplates(),
         "-t",
         "csv",
         "--columns",
         "msg",
         "--no-header",
         first.string(),
         second.string()}
    );
    INFO("stdout:\n" << result.out);
    INFO("stderr:\n" << result.err);
    REQUIRE(result.exitCode == 0);
    REQUIRE(result.out == "first\nsecond\n");
}

TEST_CASE("slp query expands rotated siblings oldest-first", "[cli][e2e][sources]")
{
    const auto root = ScratchRoot() / "rotation";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root, ec);

    WriteBytes(
        root / "app.log.2", std::string(R"({"ts":"2024-01-01T00:00:00Z","level":"info","msg":"oldest"})") + "\n"
    );
    WriteBytes(
        root / "app.log.1", std::string(R"({"ts":"2024-01-01T00:00:01Z","level":"info","msg":"middle"})") + "\n"
    );
    WriteBytes(root / "app.log", std::string(R"({"ts":"2024-01-01T00:00:02Z","level":"info","msg":"newest"})") + "\n");

    const auto expanded = RunSlp(
        {"query",
         "--tz",
         "UTC",
         "--template-dir",
         EmptyTemplates(),
         "-v",
         "-t",
         "csv",
         "--columns",
         "msg",
         "--no-header",
         (root / "app.log").string()}
    );
    INFO("stdout:\n" << expanded.out);
    INFO("stderr:\n" << expanded.err);
    REQUIRE(expanded.exitCode == 0);
    REQUIRE(expanded.out == "oldest\nmiddle\nnewest\n");
    REQUIRE(expanded.err.find("app.log.2") != std::string::npos);
    REQUIRE(expanded.err.find("rotated sibling") != std::string::npos);

    const auto disabled = RunSlp(
        {"query",
         "--tz",
         "UTC",
         "--template-dir",
         EmptyTemplates(),
         "--no-rotation-history",
         "-t",
         "csv",
         "--columns",
         "msg",
         "--no-header",
         (root / "app.log").string()}
    );
    REQUIRE(disabled.exitCode == 0);
    REQUIRE(disabled.out == "newest\n");
}

TEST_CASE("slp query reads stdin", "[cli][e2e][sources]")
{
    const auto result =
        RunSlpWithStdin({"query", "--tz", "UTC", "--template-dir", EmptyTemplates(), "--count", "-"}, TwoLinesBytes());
    ExpectTwoLinesCount(result);
}

TEST_CASE("slp query reads a session bundle", "[cli][e2e][sources]")
{
    const auto bundlePath = ScratchRoot() / "two_lines.slvbundle";
    {
        loglib::LogTable table = ParseFixtureTable();
        loglib::WriteSessionBundle(table, table.Configuration().Configuration(), bundlePath);
    }

    const auto result =
        RunSlp({"query", "--tz", "UTC", "--template-dir", EmptyTemplates(), "--count", bundlePath.string()});
    ExpectTwoLinesCount(result);
}

TEST_CASE("slp query maps missing paths to exit 3", "[cli][e2e][sources]")
{
    const auto missing = (ScratchRoot() / "does-not-exist.jsonl").string();
    const auto result = RunSlp({"query", "--tz", "UTC", "--template-dir", EmptyTemplates(), missing});
    INFO("stderr:\n" << result.err);
    REQUIRE(result.exitCode == 3);
    REQUIRE(result.err.find("does-not-exist.jsonl") != std::string::npos);
}

TEST_CASE("slp query rejects network sources outside tail", "[cli][e2e][sources]")
{
    const auto result = RunSlp({"query", "tcp://127.0.0.1:514"});
    INFO("stderr:\n" << result.err);
    REQUIRE(result.exitCode == 2);
    REQUIRE(result.err.find("slp tail") != std::string::npos);
}
