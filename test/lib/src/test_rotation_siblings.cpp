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

// UTF-8 filename via `u8string()`; a raw `path::string()` throws
// on Windows when the wide-string filename contains a character
// the active code page cannot represent (Cyrillic, CJK, ...).
// Fixtures below currently use ASCII names but the helper is
// shared, and a future non-ASCII fixture would crash inside
// `path::string()` before reaching any assertion.
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
    // input. `appOne` derotates to `app.log` at position 0, so
    // the `app` family wins the first slot.
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

TEST_CASE(
    "PartitionAsRotationSeries derives the correct primary for a compressed dated sibling",
    "[RotationSiblings]"
)
{
    // Regression: `DeriveRotationPrimary` used to check the
    // stem-inserted dated pattern (`app-2025-04-28.log`) *before*
    // the tailing dated pattern (`app.log-2025-04-28`), so a
    // compressed dated sibling like `app.log-2025-04-28.gz` would
    // greedy-match the compression suffix as the "extension" and
    // yield the bogus primary `app.log.gz`. The correct primary is
    // `app.log`, and the family must pull in the plain-suffix
    // siblings sitting next to it.
    const TempDir dir("rotation_partition_dated_compressed_derives");
    (void)dir.Write("app.log", "primary");
    (void)dir.Write("app.log-2025-04-27", "yesterday");
    const auto compressedDated = dir.Write("app.log-2025-04-28.gz", "today rotated");

    const std::vector<std::filesystem::path> input{compressedDated};
    const auto partitioned = PartitionAsRotationSeries(std::span<const std::filesystem::path>(input));
    REQUIRE(partitioned.series.size() == 1);
    CHECK(partitioned.residual.empty());
    const auto names = Basenames(partitioned.series.front());
    // Three siblings (oldest -> newest): the earlier plain-suffix
    // date, the compressed one, then the primary.
    REQUIRE(names.size() == 3);
    // Note: `2025-04-28.gz` (today's rotation) sorts newer than
    // `2025-04-27` (yesterday), so it lands second, and `app.log`
    // (the primary) is always last.
    CHECK(names.front() == "app.log-2025-04-27");
    CHECK(names[1] == "app.log-2025-04-28.gz");
    CHECK(names.back() == "app.log");
    CHECK(partitioned.series.front().files.back().origin == RotatedFile::Origin::Primary);
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

TEST_CASE(
    "PartitionAsRotationSeries: caller-listed rotated input that no longer exists on disk "
    "reports Origin::CallerListed",
    "[RotationSiblings]"
)
{
    // Regression: the partitioner's "union step" folds caller
    // inputs that group under a family primary but that the
    // enumerator did not classify on disk (e.g. the caller
    // recorded the file in a session, but by the time the file
    // is reopened the rotated segment has since been deleted /
    // moved). Pre-fix the code labelled these paths
    // `Origin::NumberedSuffix`, which is a lie: downstream
    // heuristics that read `origin` alone to distinguish
    // "auto-discovered by rotation detection" from "listed by
    // the caller" would wrongly count them as auto-added
    // siblings and arm the sibling toast / Undo affordance for
    // a set the user assembled by hand.
    //
    // Reproduce the case with a phantom rotated file: `app.log`
    // and `app.log.1` exist on disk, the caller lists
    // `[app.log.5]` (which does *not* exist). `DeriveRotationPrimary`
    // maps `app.log.5` -> `app.log`; the enumerator walks the
    // directory and finds only `app.log.1` (plus reattaching
    // `app.log` as the primary). The union step then folds
    // `app.log.5` into the series -- and that entry is the one
    // whose origin must read `CallerListed`.
    const TempDir dir("rotation_partition_caller_listed");
    (void)dir.Write("app.log", "primary");
    (void)dir.Write("app.log.1", "older");
    const auto phantom = dir.Path() / "app.log.5"; // deliberately NOT written to disk

    const std::vector<std::filesystem::path> input{phantom};
    const auto partitioned = PartitionAsRotationSeries(std::span<const std::filesystem::path>(input));
    REQUIRE(partitioned.series.size() == 1);
    const auto &series = partitioned.series.front();
    // Family has 3 entries: existing `.1` (Numbered), the
    // caller-listed phantom `.5` (CallerListed), then the
    // primary (Primary).
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

#if defined(_WIN32) || defined(__APPLE__)
TEST_CASE("EnumerateRotatedSiblings is case-insensitive on Windows/macOS", "[RotationSiblings]")
{
    // Windows / APFS filesystems compare filenames without regard to
    // case. `App.LOG.1` names the same rotation family as `app.log`.
    // On Linux the same test would be a decoy (see the negative case
    // above), so this expectation is platform-gated.
    const TempDir dir("rotation_case_insensitive");
    (void)dir.Write("app.log", "primary");
    (void)dir.Write("App.LOG.1", "one older, weird case");
    (void)dir.Write("APP.log.2.GZ", "two older, ALL CAPS + upper ext");

    const RotationSeries series = EnumerateRotatedSiblings(dir.Path() / "app.log");
    // 3 entries: primary + 2 siblings. Byte-strict comparison would
    // reject the mixed-case ones and yield only the primary.
    REQUIRE(series.files.size() == 3);
    // The primary always sits last (Origin::Primary).
    CHECK(series.files.back().origin == RotatedFile::Origin::Primary);
}
#endif

TEST_CASE(
    "EnumerateRotatedSiblings rejects pathological numbered suffixes", "[RotationSiblings]"
)
{
    // A numeric suffix that would overflow `MAX_ACCEPTED_NUMBERED_SUFFIX`
    // must not silently reorder the family. logrotate configurations
    // never reach these values in practice; guard against pathological
    // input regardless.
    const TempDir dir("rotation_numbered_overflow");
    (void)dir.Write("app.log", "primary");
    (void)dir.Write("app.log.1", "sane sibling");
    // 1e13 is well above `MAX_ACCEPTED_NUMBERED_SUFFIX` (~1e9) and
    // would wrap the sort key into the "dated" rank range.
    (void)dir.Write("app.log.10000000000000", "pathological");

    const RotationSeries series = EnumerateRotatedSiblings(dir.Path() / "app.log");
    // The primary and the sane sibling; the pathological file is
    // ignored (its rank would collide with the dated range).
    REQUIRE(series.files.size() == 2);
    CHECK(series.files.front().path.filename().string() == "app.log.1");
    CHECK(series.files.back().path.filename().string() == "app.log");
}

TEST_CASE("EnumerateRotatedSiblings rejects `.0` numbered suffix", "[RotationSiblings]")
{
    // logrotate starts at `.1`; `.0` is either a user typo or a
    // homegrown archive naming convention. Either way it is not part
    // of the family (accepting it would emit two "primary" entries).
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
    "PartitionAsRotationSeries rejects `.0` and pathological numbered suffixes for derivation",
    "[RotationSiblings]"
)
{
    // Regression: `DeriveRotationPrimary` used to accept any
    // `<primary>.<N>` numeric suffix (including `.0` and overflow
    // values), so a lone drop of `app.log.0` next to a real
    // `app.log` was pulled into the family via the union step in
    // `PartitionAsRotationSeries`. This contradicted the strict
    // `>= 1 && <= MAX_ACCEPTED_NUMBERED_SUFFIX` filter that
    // `ClassifySibling` applies inside `EnumerateRotatedSiblings`.
    // The partitioner now rejects the same suffixes at derivation
    // time so the two entry points agree.
    const TempDir dir("rotation_partition_reject_zero_and_overflow");
    (void)dir.Write("app.log", "primary");
    const auto zeroPath = dir.Write("app.log.0", "not a rotated sibling");
    const auto overflowPath =
        dir.Write("app.log.10000000000000", "pathological, would collide with dated rank range");

    // Drop `[app.log.0, app.log.10000000000000]` alone. Neither
    // derotates to `app.log`, so both end up as residuals rather
    // than being smuggled into a synthetic single-family series.
    const std::vector<std::filesystem::path> input{zeroPath, overflowPath};
    const auto partitioned = PartitionAsRotationSeries(std::span<const std::filesystem::path>(input));
    CHECK(partitioned.series.empty());
    REQUIRE(partitioned.residual.size() == 2);
    CHECK(partitioned.residual.front().filename().string() == "app.log.0");
    CHECK(partitioned.residual.back().filename().string() == "app.log.10000000000000");
}

TEST_CASE(
    "EnumerateRotatedSiblings returns primary-only when the primary is compressed",
    "[RotationSiblings]"
)
{
    // Regression: opening `app.log.gz` (a compressed file) as the
    // primary must NOT pull in siblings of the *uncompressed*
    // `app.log`. `SplitStemExt` for `app.log.gz` returns
    // `stem="app.log", ext="gz"`, which lets the stem-inserted
    // dated regex spuriously accept `app.log-2025-04-28.gz` as a
    // sibling of `app.log.gz` even though it is actually a
    // rotated companion of `app.log`. The enumerator should bail
    // early when the primary basename ends in a recognised codec
    // extension, since a compressed file is not itself part of a
    // rotation family.
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
    // Regression on two fronts:
    //   * On Linux the helper used to lower-case unconditionally,
    //     collapsing `App.log` and `app.log` (two distinct files on
    //     a case-sensitive FS) into the same key -- which either
    //     silently dropped the second file from
    //     `PartitionAsRotationSeries` or merged unrelated files
    //     into one "family".
    //   * On macOS the helper lower-cased to match the platform's
    //     case-insensitive default FS, but the app-layer
    //     `CanonicalLocator` (which populates
    //     `Source::locatorDedupKeys`) did NOT. macOS paths always
    //     contain uppercase (`/Users/...`), so the two key
    //     flavours never matched and drop-into-session dedup in
    //     `MainWindow::ExpandLogPathsWithRotationSiblings` always
    //     missed -- causing duplicate rows on every drop.
    // The current contract: lower-case only on Windows; every
    // other platform preserves case. This mirrors `CanonicalLocator`.
    const std::string upperKey = loglib::CanonicalKeyForPath("/logs/App.log");
    const std::string lowerKey = loglib::CanonicalKeyForPath("/logs/app.log");
    CHECK(upperKey != lowerKey);
}
#endif

TEST_CASE("CanonicalKeyForPath produces UTF-8 bytes for non-ASCII paths", "[RotationSiblings]")
{
    // Regression on two fronts:
    //   * `CanonicalKeyForPath` used to run `path::generic_string()`,
    //     which on Windows narrows the wide path through the
    //     active code page. The app-layer `CanonicalLocator`
    //     produces UTF-8, so the two byte flavours never matched
    //     for non-ASCII paths -- drop-into-session dedup silently
    //     missed and duplicate rows accumulated on every drop.
    //   * Worse, on Windows `path::generic_string()` *throws*
    //     `std::system_error` when the wide-string path contains
    //     a character the ACP can't represent (Cyrillic, CJK,
    //     accented). That propagated out of the sibling-expander
    //     and aborted the entire open flow. The current impl
    //     routes through `path::generic_u8string()` which is
    //     lossless on every platform and never throws.
    // Passing a `path` constructed from a UTF-8 `char8_t` literal
    // exercises the same underlying wide-string storage on Windows
    // without touching the ACP.
    const std::filesystem::path p(u8"/logs/\u65e5\u5fd7/app.log"); // /logs/日志/app.log
    std::string key;
    REQUIRE_NOTHROW(key = loglib::CanonicalKeyForPath(p));
    // The `日志` characters are three UTF-8 bytes each. Check the
    // literal UTF-8 byte sequence appears somewhere in the key --
    // an ACP-narrowed key (e.g. CP-1252 on Windows) would replace
    // them with `?` fallbacks and this substring search would fail.
    const std::string_view utf8Marker(reinterpret_cast<const char *>(u8"\u65e5\u5fd7"), 6);
    CHECK(key.contains(utf8Marker));
}

TEST_CASE(
    "EnumerateRotatedSiblings does not throw when the primary basename is non-ASCII",
    "[RotationSiblings]"
)
{
    // Regression: `EnumerateRotatedSiblings` used to call
    // `primary.filename().string()` (and a matching `.string()`
    // on every walked directory entry). On Windows, `.string()`
    // throws `std::system_error` when the wide-string path
    // contains a character not representable in the active code
    // page -- Cyrillic, CJK, and accented Latin trip it. The
    // exception propagated out of the module and aborted the
    // caller's open flow, in blatant violation of the module's
    // "advisory / best-effort" contract. Even against a
    // non-existent primary the helper must return cleanly (empty
    // series) rather than throw.
    const std::filesystem::path nonAsciiPrimary(u8"/nonexistent/\u65e5\u5fd7/\u041f\u0440\u0438\u043c\u0435\u0440.log");
    RotationSeries series;
    REQUIRE_NOTHROW(series = EnumerateRotatedSiblings(nonAsciiPrimary));
    // The parent dir doesn't exist so no siblings can be
    // enumerated, but `EnumerateRotatedSiblings` always
    // reattaches the primary itself as the final `Origin::Primary`
    // entry (callers rely on `series.files.back()` for the
    // primary). What matters here is that the call *completed*
    // without throwing on the non-ASCII path -- the pre-fix
    // implementation aborted inside `.string()` before ever
    // reaching this point.
    REQUIRE(series.files.size() == 1);
    CHECK(series.files.front().origin == RotatedFile::Origin::Primary);
}
