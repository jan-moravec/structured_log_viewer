#include <loglib/rotation_siblings.hpp>

#include <test_common/temp_dir.hpp>

#include <catch2/catch_all.hpp>

#include <algorithm>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

using loglib::EnumerateRotatedSiblings;
using loglib::PartitionAsRotationSeries;
using loglib::RotatedFile;
using loglib::RotationSeries;
using test_common::TempDir;

namespace
{

// Avoid active-code-page narrowing of Windows filenames.
std::vector<std::string> Basenames(const RotationSeries &series)
{
    std::vector<std::string> out;
    out.reserve(series.files.size());
    for (const RotatedFile &rf : series.files)
    {
        const std::u8string u8 = rf.path.filename().u8string();
        out.emplace_back(reinterpret_cast<const char *>(u8.data()), u8.size());
    }
    return out;
}

} // namespace

TEST_CASE("EnumerateRotatedSiblings numbered logrotate suffixes", "[RotationSiblings]")
{
    // Higher numbered suffixes are older; the primary is last.
    const TempDir dir("rotation_numbered");
    (void)dir.Write("app.log", "primary");
    (void)dir.Write("app.log.1", "one older");
    (void)dir.Write("app.log.2", "two older");
    (void)dir.Write("app.log.10", "ten older");
    (void)dir.Write("unrelated.log", "not part of the family");

    const RotationSeries series = EnumerateRotatedSiblings(dir.Path() / "app.log");

    const auto names = Basenames(series);
    REQUIRE(names.size() == 4);
    CHECK(names.front() == "app.log.10");
    CHECK(names[1] == "app.log.2");
    CHECK(names[2] == "app.log.1");
    CHECK(names.back() == "app.log");
    CHECK(series.files.back().origin == RotatedFile::Origin::Primary);
}

TEST_CASE("EnumerateRotatedSiblings compressed numbered variants", "[RotationSiblings]")
{
    const TempDir dir("rotation_numbered_compressed");
    (void)dir.Write("app.log", "primary");
    (void)dir.Write("app.log.1", "gen1");
    (void)dir.Write("app.log.2.gz", "gen2");
    (void)dir.Write("app.log.3.bz2", "gen3");
    (void)dir.Write("app.log.4.xz", "gen4");
    (void)dir.Write("app.log.5.zst", "gen5");

    const RotationSeries series = EnumerateRotatedSiblings(dir.Path() / "app.log");
    const auto names = Basenames(series);
    REQUIRE(names.size() == 6);
    CHECK(names.front() == "app.log.5.zst");
    CHECK(names[1] == "app.log.4.xz");
    CHECK(names[2] == "app.log.3.bz2");
    CHECK(names[3] == "app.log.2.gz");
    CHECK(names[4] == "app.log.1");
    CHECK(names.back() == "app.log");
}

TEST_CASE("EnumerateRotatedSiblings dated suffix variants", "[RotationSiblings]")
{
    // Earlier dates sort first across all supported separators.
    const TempDir dir("rotation_dated");
    (void)dir.Write("app.log", "primary");
    (void)dir.Write("app.log-2025-04-28", "hyphen");
    (void)dir.Write("app.log.2025-04-27", "dot");
    (void)dir.Write("app.log_2025-04-26.gz", "underscore compressed");

    const RotationSeries series = EnumerateRotatedSiblings(dir.Path() / "app.log");
    const auto names = Basenames(series);
    REQUIRE(names.size() == 4);
    CHECK(names.front() == "app.log_2025-04-26.gz");
    CHECK(names[1] == "app.log.2025-04-27");
    CHECK(names[2] == "app.log-2025-04-28");
    CHECK(names.back() == "app.log");
}

TEST_CASE("EnumerateRotatedSiblings stem-inserted dated variant", "[RotationSiblings]")
{
    const TempDir dir("rotation_dated_infix");
    (void)dir.Write("app.log", "primary");
    (void)dir.Write("app-2025-04-27.log", "yesterday");
    (void)dir.Write("app-2025-04-26.log.gz", "two days ago");
    (void)dir.Write("app-2025-04-25.txt", "unrelated ext");

    const RotationSeries series = EnumerateRotatedSiblings(dir.Path() / "app.log");
    const auto names = Basenames(series);
    REQUIRE(names.size() == 3);
    CHECK(names.front() == "app-2025-04-26.log.gz");
    CHECK(names[1] == "app-2025-04-27.log");
    CHECK(names.back() == "app.log");
}

TEST_CASE("EnumerateRotatedSiblings rejects unrelated stems and different extensions", "[RotationSiblings]")
{
    const TempDir dir("rotation_negatives");
    (void)dir.Write("app.log", "primary");
    (void)dir.Write("apps.log.1", "different stem (apps vs app)");
    (void)dir.Write("app.txt.1", "different family (txt not log)");
    (void)dir.Write("app.log.gz", "compressed primary is not a rotated sibling");
    (void)dir.Write("app.logbackup", "similar-looking but not the pattern");

    const RotationSeries series = EnumerateRotatedSiblings(dir.Path() / "app.log");
    const auto names = Basenames(series);
    REQUIRE(names.size() == 1);
    CHECK(names.front() == "app.log");
}

TEST_CASE("EnumerateRotatedSiblings on a primary with no siblings", "[RotationSiblings]")
{
    const TempDir dir("rotation_none");
    (void)dir.Write("solo.log", "just me");
    const RotationSeries series = EnumerateRotatedSiblings(dir.Path() / "solo.log");
    REQUIRE(series.files.size() == 1);
    CHECK(series.files.front().path.filename().string() == "solo.log");
    CHECK(series.files.front().origin == RotatedFile::Origin::Primary);
}

TEST_CASE("EnumerateRotatedSiblings on a non-existent primary", "[RotationSiblings]")
{
    // A missing primary is still returned as the sole series entry.
    const std::filesystem::path bogus = "/definitely/not/a/real/path/nope.log";
    const RotationSeries series = EnumerateRotatedSiblings(bogus);
    REQUIRE(series.files.size() == 1);
    CHECK(series.files.front().path == bogus);
    CHECK(series.files.front().origin == RotatedFile::Origin::Primary);
}

TEST_CASE("PartitionAsRotationSeries auto-sorts a single family", "[RotationSiblings]")
{
    const TempDir dir("rotation_partition_one_family");
    const auto p0 = dir.Write("app.log", "primary");
    const auto p1 = dir.Write("app.log.1", "one");
    const auto p2 = dir.Write("app.log.2", "two");

    const std::vector<std::filesystem::path> input{p2, p0, p1};
    const auto partitioned = PartitionAsRotationSeries(std::span<const std::filesystem::path>(input));
    REQUIRE(partitioned.series.size() == 1);
    CHECK(partitioned.residual.empty());
    const auto names = Basenames(partitioned.series.front());
    REQUIRE(names.size() == 3);
    CHECK(names.front() == "app.log.2");
    CHECK(names[1] == "app.log.1");
    CHECK(names.back() == "app.log");
}

TEST_CASE("PartitionAsRotationSeries preserves order for unrelated files", "[RotationSiblings]")
{
    const TempDir dir("rotation_partition_unrelated");
    const auto a = dir.Write("alpha.log", "a");
    const auto b = dir.Write("beta.log", "b");

    const std::vector<std::filesystem::path> input{b, a};
    const auto partitioned = PartitionAsRotationSeries(std::span<const std::filesystem::path>(input));
    CHECK(partitioned.series.empty());
    REQUIRE(partitioned.residual.size() == 2);
    CHECK(partitioned.residual.front().filename().string() == "beta.log");
    CHECK(partitioned.residual.back().filename().string() == "alpha.log");
}

TEST_CASE("PartitionAsRotationSeries handles multi-family drop", "[RotationSiblings]")
{
    // Family order follows each family's earliest caller-listed member.
    const TempDir dir("rotation_partition_multi");
    const auto appPrimary = dir.Write("app.log", "app primary");
    const auto appOne = dir.Write("app.log.1", "app older");
    const auto otherPrimary = dir.Write("other.log", "other primary");
    const auto otherOne = dir.Write("other.log.1", "other older");

    const std::vector<std::filesystem::path> input{appOne, otherPrimary, appPrimary, otherOne};
    const auto partitioned = PartitionAsRotationSeries(std::span<const std::filesystem::path>(input));
    REQUIRE(partitioned.series.size() == 2);
    CHECK(partitioned.residual.empty());
    const auto firstNames = Basenames(partitioned.series.front());
    REQUIRE(firstNames.size() == 2);
    CHECK(firstNames.front() == "app.log.1");
    CHECK(firstNames.back() == "app.log");
    const auto secondNames = Basenames(partitioned.series.back());
    REQUIRE(secondNames.size() == 2);
    CHECK(secondNames.front() == "other.log.1");
    CHECK(secondNames.back() == "other.log");
}

TEST_CASE("PartitionAsRotationSeries selecting a rotated sibling alone still finds the primary", "[RotationSiblings]")
{
    const TempDir dir("rotation_partition_lone_rotated");
    (void)dir.Write("app.log", "primary");
    (void)dir.Write("app.log.1", "gen1");
    const auto rotated = dir.Write("app.log.2", "gen2");

    const std::vector<std::filesystem::path> input{rotated};
    const auto partitioned = PartitionAsRotationSeries(std::span<const std::filesystem::path>(input));
    REQUIRE(partitioned.series.size() == 1);
    CHECK(partitioned.residual.empty());
    const auto names = Basenames(partitioned.series.front());
    REQUIRE(names.size() == 3);
    CHECK(names.front() == "app.log.2");
    CHECK(names[1] == "app.log.1");
    CHECK(names.back() == "app.log");
}

TEST_CASE("PartitionAsRotationSeries derives the correct primary for a compressed dated sibling", "[RotationSiblings]")
{
    // A compressed tail-dated sibling derives `app.log`, not `app.log.gz`.
    const TempDir dir("rotation_partition_dated_compressed_derives");
    (void)dir.Write("app.log", "primary");
    (void)dir.Write("app.log-2025-04-27", "yesterday");
    const auto compressedDated = dir.Write("app.log-2025-04-28.gz", "today rotated");

    const std::vector<std::filesystem::path> input{compressedDated};
    const auto partitioned = PartitionAsRotationSeries(std::span<const std::filesystem::path>(input));
    REQUIRE(partitioned.series.size() == 1);
    CHECK(partitioned.residual.empty());
    const auto names = Basenames(partitioned.series.front());
    REQUIRE(names.size() == 3);
    CHECK(names.front() == "app.log-2025-04-27");
    CHECK(names[1] == "app.log-2025-04-28.gz");
    CHECK(names.back() == "app.log");
    CHECK(partitioned.series.front().files.back().origin == RotatedFile::Origin::Primary);
}

TEST_CASE("PartitionAsRotationSeries: lone log with no siblings stays in residual", "[RotationSiblings]")
{
    const TempDir dir("rotation_partition_lone_primary");
    const auto lone = dir.Write("app.log", "just this");
    const std::vector<std::filesystem::path> input{lone};
    const auto partitioned = PartitionAsRotationSeries(std::span<const std::filesystem::path>(input));
    CHECK(partitioned.series.empty());
    REQUIRE(partitioned.residual.size() == 1);
    CHECK(partitioned.residual.front().filename().string() == "app.log");
}

TEST_CASE(
    "PartitionAsRotationSeries: caller-listed rotated input that no longer exists on disk "
    "reports Origin::CallerListed",
    "[RotationSiblings]"
)
{
    // A missing caller-listed segment retains `CallerListed` after unioning.
    const TempDir dir("rotation_partition_caller_listed");
    (void)dir.Write("app.log", "primary");
    (void)dir.Write("app.log.1", "older");
    const auto phantom = dir.Path() / "app.log.5"; // deliberately NOT written to disk

    const std::vector<std::filesystem::path> input{phantom};
    const auto partitioned = PartitionAsRotationSeries(std::span<const std::filesystem::path>(input));
    REQUIRE(partitioned.series.size() == 1);
    const auto &series = partitioned.series.front();
    REQUIRE(series.files.size() == 3);
    CHECK(series.files.back().origin == RotatedFile::Origin::Primary);

    const auto it = std::find_if(series.files.begin(), series.files.end(), [](const RotatedFile &rf) {
        return rf.path.filename() == std::filesystem::path("app.log.5");
    });
    REQUIRE(it != series.files.end());
    CHECK(it->origin == RotatedFile::Origin::CallerListed);
}

TEST_CASE("PartitionAsRotationSeries: mixed families and a residual", "[RotationSiblings]")
{
    const TempDir dir("rotation_partition_mixed_residual");
    const auto app0 = dir.Write("app.log", "primary");
    const auto app1 = dir.Write("app.log.1", "older");
    const auto report = dir.Write("report.log", "report");

    const std::vector<std::filesystem::path> input{app0, app1, report};
    const auto partitioned = PartitionAsRotationSeries(std::span<const std::filesystem::path>(input));
    REQUIRE(partitioned.series.size() == 1);
    REQUIRE(partitioned.residual.size() == 1);
    CHECK(partitioned.residual.front().filename().string() == "report.log");
    const auto names = Basenames(partitioned.series.front());
    REQUIRE(names.size() == 2);
    CHECK(names.front() == "app.log.1");
    CHECK(names.back() == "app.log");
}

#if defined(_WIN32) || defined(__APPLE__)
TEST_CASE("EnumerateRotatedSiblings is case-insensitive on Windows/macOS", "[RotationSiblings]")
{
    // Windows and default macOS filesystems match sibling names by case-folding.
    const TempDir dir("rotation_case_insensitive");
    (void)dir.Write("app.log", "primary");
    (void)dir.Write("App.LOG.1", "one older, weird case");
    (void)dir.Write("APP.log.2.GZ", "two older, ALL CAPS + upper ext");

    const RotationSeries series = EnumerateRotatedSiblings(dir.Path() / "app.log");
    REQUIRE(series.files.size() == 3);
    CHECK(series.files.back().origin == RotatedFile::Origin::Primary);
}
#endif

TEST_CASE("EnumerateRotatedSiblings rejects pathological numbered suffixes", "[RotationSiblings]")
{
    // Suffixes above the accepted limit are excluded from the family.
    const TempDir dir("rotation_numbered_overflow");
    (void)dir.Write("app.log", "primary");
    (void)dir.Write("app.log.1", "sane sibling");
    (void)dir.Write("app.log.10000000000000", "pathological");

    const RotationSeries series = EnumerateRotatedSiblings(dir.Path() / "app.log");
    REQUIRE(series.files.size() == 2);
    CHECK(series.files.front().path.filename().string() == "app.log.1");
    CHECK(series.files.back().path.filename().string() == "app.log");
}

TEST_CASE("EnumerateRotatedSiblings rejects `.0` numbered suffix", "[RotationSiblings]")
{
    const TempDir dir("rotation_numbered_zero");
    (void)dir.Write("app.log", "primary");
    (void)dir.Write("app.log.0", "not a rotated sibling");
    (void)dir.Write("app.log.1", "actually rotated");

    const RotationSeries series = EnumerateRotatedSiblings(dir.Path() / "app.log");
    REQUIRE(series.files.size() == 2);
    CHECK(series.files.front().path.filename().string() == "app.log.1");
    CHECK(series.files.back().path.filename().string() == "app.log");
}

TEST_CASE(
    "PartitionAsRotationSeries rejects `.0` and pathological numbered suffixes for derivation", "[RotationSiblings]"
)
{
    // Derivation applies the same numbered-suffix limits as enumeration.
    const TempDir dir("rotation_partition_reject_zero_and_overflow");
    (void)dir.Write("app.log", "primary");
    const auto zeroPath = dir.Write("app.log.0", "not a rotated sibling");
    const auto overflowPath = dir.Write("app.log.10000000000000", "pathological, would collide with dated rank range");

    const std::vector<std::filesystem::path> input{zeroPath, overflowPath};
    const auto partitioned = PartitionAsRotationSeries(std::span<const std::filesystem::path>(input));
    CHECK(partitioned.series.empty());
    REQUIRE(partitioned.residual.size() == 2);
    CHECK(partitioned.residual.front().filename().string() == "app.log.0");
    CHECK(partitioned.residual.back().filename().string() == "app.log.10000000000000");
}

TEST_CASE("EnumerateRotatedSiblings returns primary-only when the primary is compressed", "[RotationSiblings]")
{
    // A compressed primary never adopts the uncompressed file's siblings.
    const TempDir dir("rotation_compressed_primary_bails");
    (void)dir.Write("app.log", "uncompressed primary");
    const auto compressedPrimary = dir.Write("app.log.gz", "compressed");
    (void)dir.Write("app.log-2025-04-28.gz", "sibling of the uncompressed primary");

    const RotationSeries series = EnumerateRotatedSiblings(compressedPrimary);
    REQUIRE(series.files.size() == 1);
    CHECK(series.files.front().path.filename().string() == "app.log.gz");
    CHECK(series.files.front().origin == RotatedFile::Origin::Primary);
}

#ifndef _WIN32
TEST_CASE("CanonicalKeyForPath preserves case on non-Windows platforms", "[RotationSiblings]")
{
    // Non-Windows canonical keys preserve case to match app-layer keys.
    const std::string upperKey = loglib::CanonicalKeyForPath("/logs/App.log");
    const std::string lowerKey = loglib::CanonicalKeyForPath("/logs/app.log");
    CHECK(upperKey != lowerKey);
}
#endif

TEST_CASE("CanonicalKeyForPath produces UTF-8 bytes for non-ASCII paths", "[RotationSiblings]")
{
    // Canonical keys use UTF-8 without Windows code-page narrowing.
    const std::filesystem::path p(u8"/logs/\u65e5\u5fd7/app.log"); // /logs/日志/app.log
    std::string key;
    REQUIRE_NOTHROW(key = loglib::CanonicalKeyForPath(p));
    // Match the six UTF-8 bytes for the two CJK characters.
    const std::string_view utf8Marker(reinterpret_cast<const char *>(u8"\u65e5\u5fd7"), 6);
    CHECK(key.contains(utf8Marker));
}

TEST_CASE("EnumerateRotatedSiblings does not throw when the primary basename is non-ASCII", "[RotationSiblings]")
{
    // Non-ASCII primary names remain best-effort and non-throwing.
    const std::filesystem::path nonAsciiPrimary(u8"/nonexistent/\u65e5\u5fd7/\u041f\u0440\u0438\u043c\u0435\u0440.log");
    RotationSeries series;
    REQUIRE_NOTHROW(series = EnumerateRotatedSiblings(nonAsciiPrimary));
    REQUIRE(series.files.size() == 1);
    CHECK(series.files.front().origin == RotatedFile::Origin::Primary);
}
