#include "loglib/rotation_siblings.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <regex>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace loglib
{

namespace
{

using SortRank = std::int64_t;

/// Rotated numbered ranks live in a huge positive range so any
/// numbered sibling sorts after every dated one when the two
/// naming conventions coexist. Numbered rank is
/// `NUMBERED_RANK_BASE - N` so higher `N` (older log) sorts first.
constexpr SortRank NUMBERED_RANK_BASE = 1'000'000'000LL;

/// Upper bound on accepted `<primary>.<N>` numbered suffixes. Any
/// value at or above this rolls the sort key into the "dated" range
/// (`days_since_epoch`, low thousands), breaking the invariant
/// that numbered siblings always sort after dated ones. Real
/// logrotate configurations peak in the low hundreds; the cap is
/// generous but finite.
constexpr std::int64_t MAX_ACCEPTED_NUMBERED_SUFFIX = NUMBERED_RANK_BASE - 1;

/// Upper bound on directory entries walked while looking for
/// rotated siblings. `EnumerateRotatedSiblings` runs on the GUI
/// thread of the app during every open flow (menu, drag-drop, CLI,
/// recent-session bounce back). A pathological directory (a mail
/// spool, a build cache, a corrupted network share) could hold
/// hundreds of thousands of entries and stall the UI for seconds.
/// Real rotated-log directories peak in the low tens; this cap
/// is generous but bounds the worst case regardless of what the
/// caller drops on. When exceeded the enumerator returns whatever
/// it has already classified plus the primary -- the module is
/// advisory, and truncated best-effort output beats a wedged UI.
constexpr std::size_t MAX_ENUMERATED_DIRECTORY_ENTRIES = 4096;

/// Filesystems on Windows and macOS are case-insensitive by
/// default: `App.LOG.1` and `app.log.1` name the same rotation
/// family. On those platforms the sibling regex must run
/// case-insensitively; Linux/BSD stay case-sensitive to match the
/// OS behaviour.
constexpr auto REGEX_CASE_FLAGS =
#if defined(_WIN32) || defined(__APPLE__)
    std::regex::icase;
#else
    std::regex::flag_type{};
#endif

/// UTF-8 rendering of a filesystem path (or path component). All
/// per-filename regex matching, deduplication, and canonical-key
/// derivation in this file runs through UTF-8 `std::string`
/// buffers -- never `path::string()`. On Windows the latter
/// narrows through the active code page and *throws*
/// `std::system_error` when the wide-string path contains a
/// character the ACP can't represent. Non-ASCII log filenames
/// (Cyrillic, CJK, accented) trip this in the wild; propagating
/// the exception through `EnumerateRotatedSiblings` would abort
/// the entire open flow, when the code contract for the module
/// is "advisory, best-effort".
[[nodiscard]] std::string PathToUtf8(const std::filesystem::path &p)
{
    // `u8string()` returns `std::u8string`; the underlying bytes
    // are already UTF-8 so a `reinterpret_cast` is safe (only the
    // character type differs).
    const std::u8string u8 = p.u8string();
    return {reinterpret_cast<const char *>(u8.data()), u8.size()};
}

/// ASCII-only lowercase. Callers use this both for
/// case-insensitive filename comparisons on Windows/macOS and for
/// the shared canonical dedup-key form.
///
/// Deliberately byte-wise ASCII: called on UTF-8 buffers (see
/// `CanonicalKeyForPath`) where a naive `std::tolower` on
/// multi-byte continuation bytes would corrupt them. The trade-off
/// is that mixed-case accented filenames (`Ë` vs `ë`) will not
/// collapse via this helper, whereas the Qt-side `CanonicalLocator`
/// (`app/include/uuid_utils.hpp`) uses `QString::toLower` and does
/// fold them. In practice Windows log paths are ASCII at the
/// directory-name level, so this only bites real users on
/// pathological names; if we ever need full Unicode case-fold
/// parity, both helpers have to switch together.
std::string ToLower(std::string s)
{
    // UTF-8 continuation bytes have the top bit set; anything below
    // `ASCII_UPPER_BOUND` is a single-byte code point where per-byte
    // `std::tolower` is well-defined. Split out for clarity vs the
    // clang-tidy `magic-number` lint.
    constexpr unsigned char ASCII_UPPER_BOUND = 0x80;
    for (char &c : s)
    {
        const auto b = static_cast<unsigned char>(c);
        if (b < ASCII_UPPER_BOUND)
        {
            c = static_cast<char>(std::tolower(b));
        }
    }
    return s;
}

/// Regex-escape @p literal into a POSIX-ECMAScript pattern.
std::string EscapeForRegex(std::string_view literal)
{
    std::string escaped;
    escaped.reserve(literal.size() * 2);
    for (const char c : literal)
    {
        switch (c)
        {
        case '.':
        case '^':
        case '$':
        case '|':
        case '(':
        case ')':
        case '[':
        case ']':
        case '{':
        case '}':
        case '\\':
        case '+':
        case '*':
        case '?':
            escaped.push_back('\\');
            [[fallthrough]];
        default:
            escaped.push_back(c);
        }
    }
    return escaped;
}

/// Bounds guarding `std::chrono::year_month_day` construction.
/// The range is deliberately generous: we only need to reject
/// obviously-garbage captures (e.g. `9999-99-99`) before letting
/// `year_month_day::ok()` do the exact calendar check.
constexpr int MIN_YEAR = 1900;
constexpr int MAX_YEAR = 9999;
constexpr unsigned MAX_MONTH = 12;
constexpr unsigned MAX_DAY = 31;

/// Convert `(YYYY, MM, DD)` to days since 1970-01-01. Uses
/// `std::chrono::year_month_day` so we don't roll our own leap-year
/// arithmetic. Returns `std::nullopt` on an invalid calendar date.
std::optional<SortRank> DaysSinceEpoch(int year, unsigned month, unsigned day)
{
    if (year < MIN_YEAR || year > MAX_YEAR || month < 1 || month > MAX_MONTH || day < 1 || day > MAX_DAY)
    {
        return std::nullopt;
    }
    const std::chrono::year_month_day ymd{
        std::chrono::year{year}, std::chrono::month{month}, std::chrono::day{day}
    };
    if (!ymd.ok())
    {
        return std::nullopt;
    }
    const std::chrono::sys_days sd{ymd};
    return sd.time_since_epoch().count();
}

/// One classified sibling candidate with its sort key.
struct Candidate
{
    std::filesystem::path path;
    RotatedFile::Origin origin;
    SortRank sortRank;
};

/// Cached regex trio for a primary basename. Building `std::regex`
/// is not cheap and the enumeration walks a whole directory, so
/// compile once per call.
struct SiblingMatchers
{
    std::regex numbered;
    std::regex datedSuffix;
    std::optional<std::regex> stemDated;
};

SiblingMatchers BuildMatchers(std::string_view primaryBasename, std::string_view primaryStem, std::string_view primaryExt)
{
    static constexpr auto REGEX_FLAGS = std::regex::ECMAScript | std::regex::optimize | REGEX_CASE_FLAGS;
    const std::string escBase = EscapeForRegex(primaryBasename);
    const std::string compressTail = R"((?:\.(?:gz|bz2|xz|zst))?)";

    SiblingMatchers m{
        .numbered = std::regex("^" + escBase + R"(\.([0-9]+))" + compressTail + "$", REGEX_FLAGS),
        .datedSuffix = std::regex(
            "^" + escBase + R"([-._](\d{4})-(\d{2})-(\d{2}))" + compressTail + "$", REGEX_FLAGS
        ),
        .stemDated = std::nullopt,
    };
    // Stem-inserted dated variant only applies when the primary
    // basename actually has a non-empty extension we can pin the
    // regex around; otherwise pattern B collapses onto pattern A.
    if (!primaryExt.empty() && !primaryStem.empty())
    {
        const std::string escStem = EscapeForRegex(primaryStem);
        const std::string escExt = EscapeForRegex(primaryExt);
        m.stemDated = std::regex(
            "^" + escStem + R"([-._](\d{4})-(\d{2})-(\d{2})\.)" + escExt + compressTail + "$", REGEX_FLAGS
        );
    }
    return m;
}

std::optional<Candidate> ClassifySibling(const std::filesystem::path &siblingPath, const SiblingMatchers &m)
{
    const std::string filename = PathToUtf8(siblingPath.filename());
    std::smatch sm;

    if (std::regex_match(filename, sm, m.numbered))
    {
        std::int64_t n = 0;
        const std::string capture = sm[1].str();
        const auto [ptr, ec] = std::from_chars(capture.data(), capture.data() + capture.size(), n);
        // Reject `.0` (logrotate starts at .1) and any value that
        // would collide with the "dated" rank range. `from_chars`
        // returns `std::errc::result_out_of_range` for values
        // beyond `int64_t`; both branches map to "skip this file"
        // so a pathological `app.log.99999999999999` doesn't quietly
        // reorder the family.
        if (ec == std::errc{} && n >= 1 && n <= MAX_ACCEPTED_NUMBERED_SUFFIX)
        {
            return Candidate{
                .path = siblingPath,
                .origin = RotatedFile::Origin::NumberedSuffix,
                .sortRank = NUMBERED_RANK_BASE - n,
            };
        }
    }

    if (std::regex_match(filename, sm, m.datedSuffix))
    {
        const int year = std::stoi(sm[1].str());
        const unsigned month = static_cast<unsigned>(std::stoi(sm[2].str()));
        const unsigned day = static_cast<unsigned>(std::stoi(sm[3].str()));
        if (auto rank = DaysSinceEpoch(year, month, day); rank.has_value())
        {
            return Candidate{
                .path = siblingPath, .origin = RotatedFile::Origin::DatedSuffix, .sortRank = *rank
            };
        }
    }

    if (m.stemDated.has_value() && std::regex_match(filename, sm, *m.stemDated))
    {
        const int year = std::stoi(sm[1].str());
        const unsigned month = static_cast<unsigned>(std::stoi(sm[2].str()));
        const unsigned day = static_cast<unsigned>(std::stoi(sm[3].str()));
        if (auto rank = DaysSinceEpoch(year, month, day); rank.has_value())
        {
            return Candidate{
                .path = siblingPath, .origin = RotatedFile::Origin::DatedSuffix, .sortRank = *rank
            };
        }
    }

    return std::nullopt;
}

/// Split @p primary into `(stem, ext)` where ext is *without* a
/// leading dot and comes from the final path segment only. If the
/// primary has no extension (or a leading-dot filename like
/// `.bashrc`), returns `(basename, "")`.
std::pair<std::string, std::string> SplitStemExt(const std::filesystem::path &primary)
{
    const std::string filename = PathToUtf8(primary.filename());
    const auto dot = filename.rfind('.');
    // Leading dot ("./.foo") -- not an extension separator; treat
    // as ext-less.
    if (dot == std::string::npos || dot == 0)
    {
        return {filename, {}};
    }
    return {filename.substr(0, dot), filename.substr(dot + 1)};
}

/// If @p path *looks like* a rotated file whose primary lives in
/// the same directory, return the primary path (whether or not
/// that primary actually exists on disk). Otherwise return
/// `std::nullopt`. Used by the multi-select smart-sort so
/// selecting `app.log.2` alone still triggers `app.log`'s series.
std::optional<std::filesystem::path> DeriveRotationPrimary(const std::filesystem::path &path)
{
    const std::string filename = PathToUtf8(path.filename());
    if (filename.empty())
    {
        return std::nullopt;
    }

    static constexpr auto DERIVE_REGEX_FLAGS = std::regex::ECMAScript | std::regex::optimize | REGEX_CASE_FLAGS;
    // Capture both the stem and the numeric suffix so we can apply
    // the same "sane range" filter `ClassifySibling` uses. Without
    // this the partitioner would happily treat `app.log.0` and
    // `app.log.99999999999999` as siblings of `app.log` even though
    // `EnumerateRotatedSiblings` rejects those on-disk. Consistent
    // rejection is important because the partitioner's union step
    // would otherwise insert the rejected candidate into the series
    // with a fabricated origin.
    static const std::regex NUMBERED_TAILING(
        R"(^(.+?)\.([0-9]+)(?:\.(?:gz|bz2|xz|zst))?$)", DERIVE_REGEX_FLAGS
    );
    static const std::regex DATED_TAILING(
        R"(^(.+?)[-._]\d{4}-\d{2}-\d{2}(?:\.(?:gz|bz2|xz|zst))?$)", DERIVE_REGEX_FLAGS
    );
    static const std::regex DATED_INFIX(
        R"(^(.+?)[-._]\d{4}-\d{2}-\d{2}\.([A-Za-z0-9]+)(?:\.(?:gz|bz2|xz|zst))?$)", DERIVE_REGEX_FLAGS
    );

    std::smatch m;
    if (std::regex_match(filename, m, NUMBERED_TAILING))
    {
        const std::string capture = m[2].str();
        std::int64_t n = 0;
        const auto [ptr, ec] = std::from_chars(capture.data(), capture.data() + capture.size(), n);
        if (ec == std::errc{} && n >= 1 && n <= MAX_ACCEPTED_NUMBERED_SUFFIX)
        {
            return path.parent_path() / m[1].str();
        }
        // Fall through: `.0` or an out-of-range suffix is not part
        // of any recognised family. Dated variants below may still
        // match on the same filename (unlikely but harmless).
    }
    // Check the "tailing" pattern (`<primary>-<date>[.<compression>]`)
    // *before* the stem-inserted variant. Both regexes accept a
    // compressed dated sibling like `app.log-2025-04-28.gz`, but
    // `DATED_INFIX` misclassifies it: it greedy-matches `.gz` as the
    // filename's extension and produces the bogus primary
    // `app.log.gz` instead of the correct `app.log`. Only names that
    // genuinely have text between the date and the compression
    // suffix (e.g. `app-2025-04-28.log.gz`) fall through to the
    // stem-inserted branch.
    if (std::regex_match(filename, m, DATED_TAILING))
    {
        return path.parent_path() / m[1].str();
    }
    if (std::regex_match(filename, m, DATED_INFIX))
    {
        // Stem-inserted dated variant: rebuild `<stem>.<ext>`.
        return path.parent_path() / (m[1].str() + "." + m[2].str());
    }
    // Note: a bare compressed name like `app.log.gz` intentionally
    // does NOT derotate. The three regexes above already handle
    // compressed rotated segments (`app.log.1.gz`,
    // `app.log-2025-04-28.gz`, `app-2025-04-28.log.gz`) via each
    // pattern's optional codec-tail group. A bare `<primary>.gz`
    // is ambiguous ("archive of the current primary" vs "an odd
    // single-generation snapshot"); silently derotating it to the
    // uncompressed primary would drop the user's opened file from
    // the resulting series unless the enumerator learns to
    // include compressed archives, which changes the semantics of
    // `NumberedSuffix` / `DatedSuffix`. Leave that as a design
    // decision for a follow-up rather than a stealth behaviour
    // change.
    return std::nullopt;
}

/// True iff @p ext (without the leading dot) is one of the
/// codec extensions we recognise as a rotated-log compression
/// suffix. Kept in sync with the `compressTail` regex fragment
/// in `BuildMatchers`.
///
/// Routes through the shared `ToLower` helper so a stray UTF-8
/// continuation byte in the input never gets corrupted by a
/// naive per-byte `std::tolower`. Extensions are ASCII in
/// practice, but the discipline matches the rest of the file.
bool IsRecognisedCodecExt(std::string_view ext)
{
    if (ext.empty())
    {
        return false;
    }
    const std::string lower = ToLower(std::string(ext));
    return lower == "gz" || lower == "bz2" || lower == "xz" || lower == "zst";
}

std::vector<Candidate> EnumerateCandidates(const std::filesystem::path &primary)
{
    std::vector<Candidate> candidates;
    std::error_code ec;
    const std::filesystem::path dir = primary.parent_path();
    if (dir.empty() || !std::filesystem::exists(dir, ec) || !std::filesystem::is_directory(dir, ec))
    {
        return candidates;
    }

    const std::string primaryBasename = PathToUtf8(primary.filename());
    if (primaryBasename.empty())
    {
        return candidates;
    }
    const auto [stem, ext] = SplitStemExt(primary);
    // A compressed primary (`app.log.gz`) is not itself part of a
    // rotation family -- the uncompressed `app.log` is. Enumerating
    // siblings against the compressed name produces false positives:
    // the stem-inserted regex built from `stem="app.log", ext="gz"`
    // spuriously accepts `app.log-2025-04-28.gz`, which is actually
    // a sibling of the *uncompressed* primary. Bail early so the
    // caller sees only the primary itself in the returned series.
    if (IsRecognisedCodecExt(ext))
    {
        return candidates;
    }
    const SiblingMatchers matchers = BuildMatchers(primaryBasename, stem, ext);

    // Case-insensitive filesystems reach the loop below through the
    // icase-aware `matchers`; the "skip the primary itself" check
    // must match that. Comparing lower-cased forms on those
    // platforms lets `App.LOG` reach `app.log` and vice versa; a
    // case-sensitive host stays byte-strict.
    const std::string primaryBasenameKey =
#if defined(_WIN32) || defined(__APPLE__)
        ToLower(primaryBasename);
#else
        primaryBasename;
#endif

    // Best-effort walk. Errors on individual entries are silently
    // skipped; the module is advisory.
    std::filesystem::directory_iterator it(dir, std::filesystem::directory_options::skip_permission_denied, ec);
    if (ec)
    {
        return candidates;
    }
    const std::filesystem::directory_iterator end;

    std::size_t entriesWalked = 0;
    while (it != end)
    {
        if (entriesWalked++ >= MAX_ENUMERATED_DIRECTORY_ENTRIES)
        {
            // Pathological directory; return whatever we've
            // classified so far. Caller sees the primary plus any
            // siblings we found before the cap. See
            // `MAX_ENUMERATED_DIRECTORY_ENTRIES` for rationale.
            break;
        }
        const std::filesystem::directory_entry &entry = *it;
        const std::filesystem::path path = entry.path();
        const std::string filename = PathToUtf8(path.filename());
        const std::string filenameKey =
#if defined(_WIN32) || defined(__APPLE__)
            ToLower(filename);
#else
            filename;
#endif
        if (filenameKey == primaryBasenameKey)
        {
            // Primary itself; the caller reattaches it after sorting.
            it.increment(ec);
            if (ec)
            {
                break;
            }
            continue;
        }
        std::error_code regEc;
        const bool isFile = entry.is_regular_file(regEc);
        if (!regEc && isFile)
        {
            if (auto candidate = ClassifySibling(path, matchers); candidate.has_value())
            {
                candidates.push_back(std::move(*candidate));
            }
        }
        it.increment(ec);
        if (ec)
        {
            break;
        }
    }
    return candidates;
}

} // namespace

std::string CanonicalKeyForPath(const std::filesystem::path &path)
{
    // Best-effort canonical form. The exact rules matter because
    // this key must byte-equal `logapp::CanonicalLocator`'s output
    // (see `app/include/uuid_utils.hpp`) for the same input; any
    // divergence silently defeats the "already in session" dedup
    // in `MainWindow::ExpandLogPathsWithRotationSiblings`.
    //
    //   - `absolute()` + `lexically_normal()` (NOT `weakly_canonical`):
    //     make the path absolute against CWD and collapse `.` /
    //     `..` components *without* touching the filesystem.
    //     `weakly_canonical` also resolves symlinks-that-exist,
    //     which the Qt-side helper (`QFileInfo::absoluteFilePath`)
    //     does NOT. Aligning here so a session saved under a
    //     symlinked directory (e.g. macOS `/tmp -> /private/tmp`,
    //     Linux `/var/log -> /mnt/logs`) matches the freshly
    //     canonicalised key of the same file on the next drop.
    //   - Forward slashes so a saved key round-trips across
    //     platforms.
    //   - UTF-8 encoded via `generic_u8string()` so the byte
    //     sequence matches Qt's `QString::toStdString()` output on
    //     every platform. Using `generic_string()` here would narrow
    //     the wide Windows path through the active code page and
    //     produce bytes that never match the UTF-8 key the app-layer
    //     `CanonicalLocator` writes into `Source::locatorDedupKeys`.
    //   - Lower-case ONLY on Windows. Intentionally mirrors the
    //     app-layer `CanonicalLocator`. Linux is case-sensitive
    //     (`App.log` and `app.log` are two distinct files). macOS's
    //     default APFS/HFS+ is case-insensitive at the FS layer,
    //     but `CanonicalLocator` preserves case there, so this
    //     helper does too -- the two must move together.
    //
    // The library helper exists so unit tests can share a
    // canonicaliser without depending on Qt.
    std::filesystem::path normalised;
    std::error_code ec;
    // `absolute` only reads CWD; it does not stat the target and
    // does not resolve symlinks (unlike `canonical` /
    // `weakly_canonical`). `lexically_normal` is a pure syntactic
    // pass. Both return the input unchanged on failure paths, so
    // even a bogus / relative path degrades to "something UTF-8
    // and forward-slashed" rather than throwing out of an
    // advisory helper.
    std::filesystem::path abs = std::filesystem::absolute(path, ec);
    if (ec || abs.empty())
    {
        abs = path;
    }
    normalised = abs.lexically_normal();
    if (normalised.empty())
    {
        normalised = path;
    }
    // `generic_u8string()` (not `generic_string()`) so the byte
    // sequence is UTF-8 on every platform AND unambiguously
    // uses forward slashes. `generic_string()` on Windows narrows
    // through the active code page and *throws*
    // `std::system_error` when the wide-string path contains a
    // character the ACP can't represent (Cyrillic, CJK, etc.) --
    // that would abort the entire caller flow, breaking this
    // module's advisory contract. See `PathToUtf8` for the same
    // discipline elsewhere in the file.
    const std::u8string u8 = normalised.generic_u8string();
    std::string s(reinterpret_cast<const char *>(u8.data()), u8.size());
#ifdef _WIN32
    return ToLower(std::move(s));
#else
    return s;
#endif
}

RotationSeries EnumerateRotatedSiblings(const std::filesystem::path &primary)
{
    RotationSeries series;
    series.primary = primary;

    std::vector<Candidate> candidates = EnumerateCandidates(primary);
    std::sort(candidates.begin(), candidates.end(), [](const Candidate &a, const Candidate &b) {
        return a.sortRank < b.sortRank;
    });

    series.files.reserve(candidates.size() + 1);
    for (Candidate &c : candidates)
    {
        RotatedFile rf;
        rf.path = std::move(c.path);
        rf.canonicalKey = CanonicalKeyForPath(rf.path);
        rf.origin = c.origin;
        series.files.push_back(std::move(rf));
    }

    RotatedFile primaryEntry;
    primaryEntry.path = primary;
    primaryEntry.canonicalKey = CanonicalKeyForPath(primary);
    primaryEntry.origin = RotatedFile::Origin::Primary;
    series.files.push_back(std::move(primaryEntry));

    return series;
}

PartitionedSelection PartitionAsRotationSeries(std::span<const std::filesystem::path> paths)
{
    PartitionedSelection out;
    if (paths.empty())
    {
        return out;
    }

    // Canonicalise each input exactly once and reuse the cached
    // key everywhere below. `CanonicalKeyForPath` allocates a
    // fresh UTF-8 buffer each call, so pre-caching avoids a
    // hot O(N * families) inner loop that would otherwise
    // re-canonicalise every input for every emitted family.
    std::vector<std::string> inputKeys;
    inputKeys.reserve(paths.size());
    for (const std::filesystem::path &p : paths)
    {
        inputKeys.push_back(CanonicalKeyForPath(p));
    }

    // For each input path, determine the primary it should hang
    // under: either the path itself, or a derotated form when the
    // filename looks like `<primary>.<N>` / `<primary>-<date>` /
    // etc. and that derotated file exists on disk (or is also
    // listed in the same input, which handles fresh-selection of
    // just the rotated segments).
    std::unordered_map<std::string, size_t> inputByKey;
    inputByKey.reserve(paths.size());
    for (size_t i = 0; i < paths.size(); ++i)
    {
        inputByKey.emplace(inputKeys[i], i);
    }

    struct GroupEntry
    {
        std::filesystem::path primary;
        std::string primaryKey; // cached canonical key of the primary
        size_t firstOrdinal = 0; // position of the first input contributing to this group
        size_t inputMemberCount = 0; // number of input paths that landed in this group
    };
    // Preserve caller order: `firstOrdinal` remembers where each
    // group's earliest listed member sits in `paths` so we emit
    // series in the same order the user saw them.
    std::unordered_map<std::string, GroupEntry> groups;
    // Per-input group-owner: `memberOwner[i]` is the primary key
    // of the group that absorbed `paths[i]`. A parallel vector
    // (rather than a `std::unordered_map<selfKey, primaryKey>`)
    // avoids a second hash lookup and lets the union step below
    // filter members by ordinal index directly.
    std::vector<std::string> memberOwner(paths.size());
    groups.reserve(paths.size());

    for (size_t i = 0; i < paths.size(); ++i)
    {
        const std::filesystem::path &p = paths[i];
        const std::string &selfKey = inputKeys[i];

        std::filesystem::path candidatePrimary = p;
        std::string candidatePrimaryKey = selfKey;
        if (auto derived = DeriveRotationPrimary(p); derived.has_value())
        {
            std::string derivedKey = CanonicalKeyForPath(*derived);
            std::error_code ec;
            const bool derivedExists = std::filesystem::exists(*derived, ec);
            const bool derivedListed = inputByKey.contains(derivedKey);
            if (derivedExists || derivedListed)
            {
                candidatePrimary = *derived;
                candidatePrimaryKey = std::move(derivedKey);
            }
        }

        auto it = groups.find(candidatePrimaryKey);
        if (it == groups.end())
        {
            groups.emplace(
                candidatePrimaryKey,
                GroupEntry{
                    .primary = candidatePrimary,
                    .primaryKey = candidatePrimaryKey,
                    .firstOrdinal = i,
                    .inputMemberCount = 1,
                }
            );
        }
        else
        {
            // Keep the invariant: `firstOrdinal` is the earliest
            // input index that landed in this group. Iteration
            // order over `paths` is monotonic so the min is almost
            // always `it->second.firstOrdinal`, but the explicit
            // `min` documents the invariant clearly.
            it->second.firstOrdinal = std::min(i, it->second.firstOrdinal);
            ++it->second.inputMemberCount;
        }
        memberOwner[i] = std::move(candidatePrimaryKey);
    }

    // Emit groups in `firstOrdinal` order so series match caller layout.
    std::vector<const GroupEntry *> ordered;
    ordered.reserve(groups.size());
    for (const auto &kv : groups)
    {
        ordered.push_back(&kv.second);
    }
    std::sort(ordered.begin(), ordered.end(), [](const GroupEntry *a, const GroupEntry *b) {
        return a->firstOrdinal < b->firstOrdinal;
    });

    // A group is only a genuine "rotation family" if it either:
    //   - contains more than one input path, or
    //   - the primary has at least one sibling on disk.
    // A lone input with no on-disk siblings degrades to residual so
    // truly unrelated files don't get wrapped in a single-entry
    // series.
    std::unordered_set<std::string> emittedInputKeys;
    emittedInputKeys.reserve(paths.size());
    for (const GroupEntry *entryPtr : ordered)
    {
        const GroupEntry &entry = *entryPtr;
        const size_t inputMemberCount = entry.inputMemberCount;
        RotationSeries series = EnumerateRotatedSiblings(entry.primary);
        const bool hasSiblingsOnDisk = series.files.size() > 1;
        const bool multipleInputsInGroup = inputMemberCount > 1;
        if (!multipleInputsInGroup && !hasSiblingsOnDisk)
        {
            continue; // will emit into residual below
        }

        // Union: on-disk siblings + any input paths listed under
        // this group that the enumerator missed (e.g. because the
        // caller listed an unusual naming variant or the primary
        // doesn't exist on disk). Dedup on canonical key.
        std::unordered_set<std::string> seen;
        seen.reserve(series.files.size() + inputMemberCount);
        for (const RotatedFile &rf : series.files)
        {
            seen.insert(rf.canonicalKey);
        }
        for (size_t i = 0; i < paths.size(); ++i)
        {
            if (memberOwner[i] != entry.primaryKey)
            {
                continue;
            }
            const std::string &k = inputKeys[i];
            if (seen.insert(k).second)
            {
                // Not classified on disk (e.g. user selected an
                // unusual variant, or the primary itself was
                // absent). Insert it before the primary as an
                // additional dated/numbered candidate; without a
                // rank we prepend to keep the primary last.
                // Origin is `CallerListed` -- lying about it
                // (e.g. claiming `NumberedSuffix`) would defeat
                // any downstream "auto-discovered vs. user-listed"
                // heuristic that reads `origin` alone.
                RotatedFile extra;
                extra.path = paths[i];
                extra.canonicalKey = k;
                extra.origin = RotatedFile::Origin::CallerListed;
                // Insert just before the primary (which sits at end).
                if (series.files.empty())
                {
                    series.files.push_back(std::move(extra));
                }
                else
                {
                    series.files.insert(series.files.end() - 1, std::move(extra));
                }
            }
        }

        // Track every input path we absorbed so residual excludes them.
        for (const RotatedFile &rf : series.files)
        {
            if (inputByKey.contains(rf.canonicalKey))
            {
                emittedInputKeys.insert(rf.canonicalKey);
            }
        }

        out.series.push_back(std::move(series));
    }

    for (size_t i = 0; i < paths.size(); ++i)
    {
        const std::string &k = inputKeys[i];
        if (!emittedInputKeys.contains(k))
        {
            out.residual.push_back(paths[i]);
            emittedInputKeys.insert(k);
        }
    }

    return out;
}

} // namespace loglib
