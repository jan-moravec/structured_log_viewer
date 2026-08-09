#include <loglib/rotation_siblings.hpp>

#include <test_common/temp_dir.hpp>

#include <catch2/catch_all.hpp>

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

std::vector<std::string> Basenames(const RotationSeries &series)
{
    std::vector<std::string> out;
    out.reserve(series.files.size());
    for (const RotatedFile &rf : series.files)
    {
        out.push_back(rf.path.filename().string());
    }
    return out;
}

} // namespace

TEST_CASE("EnumerateRotatedSiblings numbered logrotate suffixes", "[RotationSiblings]")
{
    // logrotate numbered: `app.log`, `app.log.1`, `app.log.2`, ...
    // Higher N is older, primary is last.
    const TempDir dir("rotation_numbered");
    (void)dir.Write("app.log", "primary");
    (void)dir.Write("app.log.1", "one older");
    (void)dir.Write("app.log.2", "two older");
    (void)dir.Write("app.log.10", "ten older");
    // Decoy: different family entirely.
    (void)dir.Write("unrelated.log", "not part of the family");

    const RotationSeries series = EnumerateRotatedSiblings(dir.Path() / "app.log");

    const auto names = Basenames(series);
    REQUIRE(names.size() == 4);
    CHECK(names.front() == "app.log.10");
    CHECK(names[1] == "app.log.2");
    CHECK(names[2] == "app.log.1");
    CHECK(names.back() == "app.log");
    // The tail always carries the Primary origin marker.
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
    // Dated suffix variants: `-`, `.`, `_`, with/without compression.
    // Earlier date == older, primary appears last.
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
    // `app-2025-04-28.log` variant (date inserted between stem and ext).
    const TempDir dir("rotation_dated_infix");
    (void)dir.Write("app.log", "primary");
    (void)dir.Write("app-2025-04-27.log", "yesterday");
    (void)dir.Write("app-2025-04-26.log.gz", "two days ago");
    // Decoy: same stem but different extension family.
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
    // Files that superficially share text with the primary but don't
    // spell a rotated sibling stay out of the series.
    const TempDir dir("rotation_negatives");
    (void)dir.Write("app.log", "primary");
    (void)dir.Write("apps.log.1", "different stem (apps vs app)");
    (void)dir.Write("app.txt.1", "different family (txt not log)");
    (void)dir.Write("app.log.gz", "compressed primary is not a rotated sibling");
    (void)dir.Write("app.logbackup", "similar-looking but not the pattern");

    const RotationSeries series = EnumerateRotatedSiblings(dir.Path() / "app.log");
    const auto names = Basenames(series);
    // Only the primary itself survives.
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
    // No directory / no file: still returns a single-entry series so
    // callers can iterate blindly. Nothing to enumerate but the
    // primary path is echoed back.
    const std::filesystem::path bogus = "/definitely/not/a/real/path/nope.log";
    const RotationSeries series = EnumerateRotatedSiblings(bogus);
    REQUIRE(series.files.size() == 1);
    CHECK(series.files.front().path == bogus);
    CHECK(series.files.front().origin == RotatedFile::Origin::Primary);
}

TEST_CASE("PartitionAsRotationSeries auto-sorts a single family", "[RotationSiblings]")
{
    // User selects [app.log.2, app.log, app.log.1] out of order.
    // Expander should emit one series in oldest -> newest form.
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
    // Two unrelated files (each without siblings) should stay in
    // residual in caller order.
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
    // User drops [app.log.1, other.log, app.log, other.log.1].
    // Two families; each internally sorted; series order follows
    // the position of their earliest listed member.
    const TempDir dir("rotation_partition_multi");
    const auto appPrimary = dir.Write("app.log", "app primary");
    const auto appOne = dir.Write("app.log.1", "app older");
    const auto otherPrimary = dir.Write("other.log", "other primary");
    const auto otherOne = dir.Write("other.log.1", "other older");

    const std::vector<std::filesystem::path> input{appOne, otherPrimary, appPrimary, otherOne};
    const auto partitioned = PartitionAsRotationSeries(std::span<const std::filesystem::path>(input));
    REQUIRE(partitioned.series.size() == 2);
    CHECK(partitioned.residual.empty());
    // First series: whichever primary appears earliest in the
    // input. `appOne` derotates to `app.log` at position 0 →ing so
    // the `app` family wins first slot.
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
    // User opens `app.log.2` on its own. `app.log` exists next to
    // it on disk. The partitioner should pull in the whole family.
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

TEST_CASE("PartitionAsRotationSeries: lone log with no siblings stays in residual", "[RotationSiblings]")
{
    // A lone `app.log` with nothing else in its directory should
    // not be reported as a single-entry series (that would force
    // the caller to unpack every input into a wrapper struct even
    // when no expansion happened). It lands in residual instead.
    const TempDir dir("rotation_partition_lone_primary");
    const auto lone = dir.Write("app.log", "just this");
    const std::vector<std::filesystem::path> input{lone};
    const auto partitioned = PartitionAsRotationSeries(std::span<const std::filesystem::path>(input));
    CHECK(partitioned.series.empty());
    REQUIRE(partitioned.residual.size() == 1);
    CHECK(partitioned.residual.front().filename().string() == "app.log");
}

TEST_CASE("PartitionAsRotationSeries: mixed families and a residual", "[RotationSiblings]")
{
    // One family (app.log) plus one unrelated lone file (report.log)
    // should show up as one series + one residual entry.
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
