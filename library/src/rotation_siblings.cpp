#include "loglib/rotation_siblings.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
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

/// Numbered suffix N has rank `NUMBERED_RANK_BASE - N`.
constexpr SortRank NUMBERED_RANK_BASE = 1'000'000'000LL;

/// Largest numbered suffix that still produces a positive rank.
constexpr std::int64_t MAX_ACCEPTED_NUMBERED_SUFFIX = NUMBERED_RANK_BASE - 1;

/// Bounds the synchronous directory scan. Results may be truncated.
constexpr std::size_t MAX_ENUMERATED_DIRECTORY_ENTRIES = 4096;

/// Match case-insensitively on Windows/macOS and sensitively elsewhere.
constexpr auto REGEX_CASE_FLAGS =
#if defined(_WIN32) || defined(__APPLE__)
    std::regex::icase;
#else
    std::regex::flag_type{};
#endif

/// Converts without locale-dependent narrowing; path matching uses UTF-8.
[[nodiscard]] std::string PathToUtf8(const std::filesystem::path &p)
{
    // `char8_t` and `char` differ in type, not byte representation.
    const std::u8string u8 = p.u8string();
    return {reinterpret_cast<const char *>(u8.data()), u8.size()};
}

/// Lowercases ASCII bytes without altering multibyte UTF-8 sequences.
/// This is not Unicode case folding.
std::string ToLower(std::string s)
{
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

/// Escapes @p literal for an ECMAScript regular expression.
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

/// Broad bounds applied before exact calendar validation.
constexpr int MIN_YEAR = 1900;
constexpr int MAX_YEAR = 9999;
constexpr unsigned MAX_MONTH = 12;
constexpr unsigned MAX_DAY = 31;

/// Returns days since 1970-01-01, or `nullopt` for an invalid date.
std::optional<SortRank> DaysSinceEpoch(int year, unsigned month, unsigned day)
{
    if (year < MIN_YEAR || year > MAX_YEAR || month < 1 || month > MAX_MONTH || day < 1 || day > MAX_DAY)
    {
        return std::nullopt;
    }
    const std::chrono::year_month_day ymd{std::chrono::year{year}, std::chrono::month{month}, std::chrono::day{day}};
    if (!ymd.ok())
    {
        return std::nullopt;
    }
    const std::chrono::sys_days sd{ymd};
    return sd.time_since_epoch().count();
}

struct Candidate
{
    std::filesystem::path path;
    RotatedFile::Origin origin;
    SortRank sortRank;
};

/// Compile once per scan because `std::regex` construction is expensive.
struct SiblingMatchers
{
    std::regex numbered;
    std::regex datedSuffix;
    std::optional<std::regex> stemDated;
};

SiblingMatchers BuildMatchers(
    std::string_view primaryBasename, std::string_view primaryStem, std::string_view primaryExt
)
{
    static constexpr auto REGEX_FLAGS = std::regex::ECMAScript | std::regex::optimize | REGEX_CASE_FLAGS;
    const std::string escBase = EscapeForRegex(primaryBasename);
    const std::string compressTail = R"((?:\.(?:gz|bz2|xz|zst))?)";

    SiblingMatchers m{
        .numbered = std::regex("^" + escBase + R"(\.([0-9]+))" + compressTail + "$", REGEX_FLAGS),
        .datedSuffix = std::regex("^" + escBase + R"([-._](\d{4})-(\d{2})-(\d{2}))" + compressTail + "$", REGEX_FLAGS),
        .stemDated = std::nullopt,
    };
    // Without both parts, the stem-inserted form duplicates the suffix form.
    if (!primaryExt.empty() && !primaryStem.empty())
    {
        const std::string escStem = EscapeForRegex(primaryStem);
        const std::string escExt = EscapeForRegex(primaryExt);
        m.stemDated =
            std::regex("^" + escStem + R"([-._](\d{4})-(\d{2})-(\d{2})\.)" + escExt + compressTail + "$", REGEX_FLAGS);
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
        // Accept only positive suffixes that keep the computed rank positive.
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
            return Candidate{.path = siblingPath, .origin = RotatedFile::Origin::DatedSuffix, .sortRank = *rank};
        }
    }

    if (m.stemDated.has_value() && std::regex_match(filename, sm, *m.stemDated))
    {
        const int year = std::stoi(sm[1].str());
        const unsigned month = static_cast<unsigned>(std::stoi(sm[2].str()));
        const unsigned day = static_cast<unsigned>(std::stoi(sm[3].str()));
        if (auto rank = DaysSinceEpoch(year, month, day); rank.has_value())
        {
            return Candidate{.path = siblingPath, .origin = RotatedFile::Origin::DatedSuffix, .sortRank = *rank};
        }
    }

    return std::nullopt;
}

/// Splits the filename into `(stem, extension-without-dot)`.
/// Leading-dot names are treated as extensionless.
std::pair<std::string, std::string> SplitStemExt(const std::filesystem::path &primary)
{
    const std::string filename = PathToUtf8(primary.filename());
    const auto dot = filename.rfind('.');
    if (dot == std::string::npos || dot == 0)
    {
        return {filename, {}};
    }
    return {filename.substr(0, dot), filename.substr(dot + 1)};
}

/// Derives a same-directory primary from a recognized rotated filename.
/// The derived path need not exist.
std::optional<std::filesystem::path> DeriveRotationPrimary(const std::filesystem::path &path)
{
    const std::string filename = PathToUtf8(path.filename());
    if (filename.empty())
    {
        return std::nullopt;
    }

    static constexpr auto DERIVE_REGEX_FLAGS = std::regex::ECMAScript | std::regex::optimize | REGEX_CASE_FLAGS;
    // Apply the same numbered-suffix range as `ClassifySibling`.
    static const std::regex NUMBERED_TAILING(R"(^(.+?)\.([0-9]+)(?:\.(?:gz|bz2|xz|zst))?$)", DERIVE_REGEX_FLAGS);
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
    }
    // Test the suffix form first because compressed suffix names also
    // satisfy the more general stem-inserted expression.
    if (std::regex_match(filename, m, DATED_TAILING))
    {
        return path.parent_path() / m[1].str();
    }
    if (std::regex_match(filename, m, DATED_INFIX))
    {
        return path.parent_path() / (m[1].str() + "." + m[2].str());
    }
    // A bare `<primary>.<codec>` is not a recognized rotation form.
    return std::nullopt;
}

/// The accepted codecs must match `BuildMatchers`' compression suffix.
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
    // A codec extension cannot serve as the primary extension; treating it
    // as one would misclassify companions of the uncompressed file.
    if (IsRecognisedCodecExt(ext))
    {
        return candidates;
    }
    const SiblingMatchers matchers = BuildMatchers(primaryBasename, stem, ext);

    // Use the same platform case rules for matching and primary exclusion.
    const std::string primaryBasenameKey =
#if defined(_WIN32) || defined(__APPLE__)
        ToLower(primaryBasename);
#else
        primaryBasename;
#endif

    // Directory errors produce the candidates collected so far.
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
    // Normalize syntactically without resolving symlinks or requiring the
    // target to exist. This must remain compatible with locator dedup keys.
    std::filesystem::path normalised;
    std::error_code ec;
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
    // Generic UTF-8 preserves non-ASCII paths and uses forward slashes.
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

    // Canonicalization allocates, so cache one key per input.
    std::vector<std::string> inputKeys;
    inputKeys.reserve(paths.size());
    for (const std::filesystem::path &p : paths)
    {
        inputKeys.push_back(CanonicalKeyForPath(p));
    }

    // A derived primary owns the input only when it exists or is selected.
    std::unordered_map<std::string, size_t> inputByKey;
    inputByKey.reserve(paths.size());
    for (size_t i = 0; i < paths.size(); ++i)
    {
        inputByKey.emplace(inputKeys[i], i);
    }

    struct GroupEntry
    {
        std::filesystem::path primary;
        std::string primaryKey;
        size_t firstOrdinal = 0;
        size_t inputMemberCount = 0;
    };
    std::unordered_map<std::string, GroupEntry> groups;
    // Parallel ownership preserves duplicate input ordinals.
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
            it->second.firstOrdinal = std::min(i, it->second.firstOrdinal);
            ++it->second.inputMemberCount;
        }
        memberOwner[i] = std::move(candidatePrimaryKey);
    }

    // Emit families by their earliest input member.
    std::vector<const GroupEntry *> ordered;
    ordered.reserve(groups.size());
    for (const auto &kv : groups)
    {
        ordered.push_back(&kv.second);
    }
    std::sort(ordered.begin(), ordered.end(), [](const GroupEntry *a, const GroupEntry *b) {
        return a->firstOrdinal < b->firstOrdinal;
    });

    // A lone input without an on-disk sibling remains residual.
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
            continue;
        }

        // Merge explicit inputs missed by enumeration, deduplicated by key.
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
                RotatedFile extra;
                extra.path = paths[i];
                extra.canonicalKey = k;
                extra.origin = RotatedFile::Origin::CallerListed;
                // Unranked explicit inputs retain input order before the primary.
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
