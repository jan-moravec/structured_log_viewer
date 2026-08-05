#include "loglib/session_bundle.hpp"

#include "loglib/internal/log_configuration_glaze_meta.hpp"
#include "loglib/internal/normalized_json_row.hpp"
#include "loglib/internal/path_encoding.hpp"
#include "loglib/line_source.hpp"
#include "loglib/log_data.hpp"
#include "loglib/log_line.hpp"
#include "loglib/log_table.hpp"

#include <glaze/glaze.hpp>
#include <zstd.h>

#include <cerrno>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace loglib
{
namespace
{

constexpr std::size_t MAX_RECORD_BYTES = 256U * 1024U * 1024U;
constexpr std::size_t MAX_METADATA_BYTES = 64U * 1024U * 1024U;
constexpr std::size_t PROGRESS_INTERVAL_ROWS = 4096;

void PollStop(const StopToken &token)
{
    if (token.stop_requested())
    {
        throw SessionBundleCancelled{};
    }
}

class FileHandle
{
public:
    explicit FileHandle(const std::filesystem::path &path)
        : mPath(path), mFile(internal::OpenFileForBinaryWrite(path))
    {
        if (mFile == nullptr)
        {
            throw std::runtime_error(
                "Session bundle: failed to open '" + internal::PathToUtf8(path) +
                "' for writing (errno " + std::to_string(errno) + ")"
            );
        }
    }

    ~FileHandle()
    {
        if (mFile != nullptr)
        {
            (void)std::fclose(mFile);
        }
    }

    FileHandle(const FileHandle &) = delete;
    FileHandle &operator=(const FileHandle &) = delete;

    void Write(std::string_view bytes)
    {
        if (!bytes.empty() && std::fwrite(bytes.data(), 1, bytes.size(), mFile) != bytes.size())
        {
            throw std::runtime_error(
                "Session bundle: short write to '" + internal::PathToUtf8(mPath) + "'"
            );
        }
    }

    void Close()
    {
        if (mFile == nullptr)
        {
            return;
        }
        // Split flush + close so the FILE* is always released. Under
        // `||` short-circuiting a failed `fflush` would skip `fclose`
        // and leak the handle -- and by then `mFile = nullptr` has
        // disarmed the destructor's fallback close, so the OS
        // handle would live until process exit.
        const bool flushOk = std::fflush(mFile) == 0;
        const bool closeOk = std::fclose(mFile) == 0;
        mFile = nullptr;
        if (!flushOk || !closeOk)
        {
            throw std::runtime_error(
                "Session bundle: failed to flush '" + internal::PathToUtf8(mPath) + "'"
            );
        }
    }

private:
    std::filesystem::path mPath;
    std::FILE *mFile = nullptr;
};

class ZstdWriter
{
public:
    ZstdWriter(FileHandle &file, const SessionBundleWriteOptions &options)
        : mFile(file), mContext(ZSTD_createCCtx()), mOutput(ZSTD_CStreamOutSize())
    {
        if (mContext == nullptr)
        {
            throw std::runtime_error("Session bundle: ZSTD_createCCtx failed");
        }
        SetParameter(ZSTD_c_compressionLevel, options.compressionLevel);
        SetParameter(ZSTD_c_checksumFlag, 1);
        if (options.totalWorkers > 0)
        {
            const std::size_t result = ZSTD_CCtx_setParameter(mContext, ZSTD_c_nbWorkers, options.totalWorkers);
            if (ZSTD_isError(result))
            {
                throw std::runtime_error(
                    std::string("Session bundle: zstd worker configuration failed: ") +
                    ZSTD_getErrorName(result)
                );
            }
        }
    }

    ~ZstdWriter()
    {
        if (mContext != nullptr)
        {
            (void)ZSTD_freeCCtx(mContext);
        }
    }

    ZstdWriter(const ZstdWriter &) = delete;
    ZstdWriter &operator=(const ZstdWriter &) = delete;

    void Write(std::string_view bytes)
    {
        ZSTD_inBuffer input{bytes.data(), bytes.size(), 0};
        while (input.pos < input.size)
        {
            Drain(input, ZSTD_e_continue);
        }
    }

    void Finish()
    {
        ZSTD_inBuffer input{nullptr, 0, 0};
        std::size_t remaining = 1;
        while (remaining != 0)
        {
            remaining = Drain(input, ZSTD_e_end);
        }
    }

private:
    void SetParameter(ZSTD_cParameter parameter, int value)
    {
        const std::size_t result = ZSTD_CCtx_setParameter(mContext, parameter, value);
        if (ZSTD_isError(result))
        {
            throw std::runtime_error(
                std::string("Session bundle: zstd configuration failed: ") + ZSTD_getErrorName(result)
            );
        }
    }

    std::size_t Drain(ZSTD_inBuffer &input, ZSTD_EndDirective directive)
    {
        ZSTD_outBuffer output{mOutput.data(), mOutput.size(), 0};
        const std::size_t result = ZSTD_compressStream2(mContext, &output, &input, directive);
        if (ZSTD_isError(result))
        {
            throw std::runtime_error(
                std::string("Session bundle: zstd compression failed: ") + ZSTD_getErrorName(result)
            );
        }
        mFile.Write({mOutput.data(), output.pos});
        return result;
    }

    FileHandle &mFile;
    ZSTD_CCtx *mContext = nullptr;
    std::vector<char> mOutput;
};

std::string CanonicalizeSourceLocator(
    const std::filesystem::path &path,
    const SessionBundleWriteOptions &options
)
{
    if (options.canonicalizeSourceLocator)
    {
        return options.canonicalizeSourceLocator(path);
    }
    return internal::PathToUtf8(path);
}

/// Composite key for the anchor index below. `std::string` (not
/// `string_view`) because the canonicalizer returns by value and the
/// resulting bytes must outlive the loop that inserts them.
struct AnchorLookupKey
{
    std::string locator;
    std::uint64_t lineId = 0;

    friend bool operator==(const AnchorLookupKey &, const AnchorLookupKey &) = default;
};

struct AnchorLookupKeyHash
{
    std::size_t operator()(const AnchorLookupKey &key) const noexcept
    {
        constexpr std::size_t GOLDEN_RATIO_HASH = 0x9E3779B9U;
        constexpr std::size_t LEFT_SHIFT = 6U;
        constexpr std::size_t RIGHT_SHIFT = 2U;
        const std::size_t locatorHash = std::hash<std::string>{}(key.locator);
        const std::size_t lineIdHash = std::hash<std::uint64_t>{}(key.lineId);
        return locatorHash ^
               (lineIdHash + GOLDEN_RATIO_HASH + (locatorHash << LEFT_SHIFT) + (locatorHash >> RIGHT_SHIFT));
    }
};

/// One slot per anchor in the wanted-set. `found=false` means the
/// scan has not yet matched a row; `ambiguous=true` means the same
/// `(canonical locator, lineId)` matched twice and the anchor
/// resolves to no dense row (dropped, like the previous sentinel).
struct AnchorMatchSlot
{
    std::uint64_t row = 0;
    bool found = false;
    bool ambiguous = false;
};

/// Wanted-set keyed on anchor lookup pairs. Sized by the anchor
/// count (typically a handful) rather than by `lines.size()`, so a
/// billion-row table with two anchors doesn't allocate ~100 GiB of
/// hash-map buckets. Complexity stays O(A + N).
using AnchorWantedSet = std::unordered_map<AnchorLookupKey, AnchorMatchSlot, AnchorLookupKeyHash>;

AnchorWantedSet BuildAnchorWantedSet(const std::vector<LogConfiguration::AnchorEntry> &anchors)
{
    AnchorWantedSet wanted;
    wanted.reserve(anchors.size());
    for (const auto &anchor : anchors)
    {
        // `try_emplace`: duplicate anchor keys collapse to a single
        // slot. A duplicate anchor list would only cause the same
        // remapping to be emitted once, but we preserve the caller's
        // list on the output side (see `RemapAnchors`) -- the wanted
        // set is only used to resolve rows.
        wanted.try_emplace(AnchorLookupKey{.locator = anchor.locator, .lineId = anchor.lineId});
    }
    return wanted;
}

void PopulateAnchorMatches(
    AnchorWantedSet &wanted,
    const std::vector<LogLine> &lines,
    const SessionBundleWriteOptions &options
)
{
    if (wanted.empty())
    {
        return;
    }
    // Canonicalization is per-source (typically ~1 source, at most
    // a handful) but `lines.size()` is millions. Cache by `Source*`
    // so the QString round-trip in `canonicalizeSourceLocator`
    // runs once per source rather than once per row.
    std::unordered_map<const LineSource *, std::string> canonicalCache;

    for (std::size_t row = 0; row < lines.size(); ++row)
    {
        const LogLine &line = lines[row];
        const auto *source = line.Source();
        AnchorLookupKey key;
        key.lineId = static_cast<std::uint64_t>(line.LineId());
        if (source != nullptr)
        {
            auto cacheIt = canonicalCache.find(source);
            if (cacheIt == canonicalCache.end())
            {
                cacheIt =
                    canonicalCache.emplace(source, CanonicalizeSourceLocator(source->Path(), options)).first;
            }
            key.locator = cacheIt->second;
        }
        auto it = wanted.find(key);
        if (it == wanted.end())
        {
            continue;
        }
        if (!it->second.found)
        {
            it->second.row = static_cast<std::uint64_t>(row);
            it->second.found = true;
        }
        else
        {
            // Duplicate `(locator, lineId)` -> ambiguous. Anchor
            // drops rather than pointing at a possibly-wrong row.
            it->second.ambiguous = true;
        }
    }
}

void RemapAnchors(
    LogConfiguration &configuration,
    const std::vector<LogLine> &lines,
    std::string_view flattenedLocator,
    const SessionBundleWriteOptions &options
)
{
    if (configuration.anchors.empty())
    {
        return;
    }
    AnchorWantedSet wanted = BuildAnchorWantedSet(configuration.anchors);
    PopulateAnchorMatches(wanted, lines, options);

    std::vector<LogConfiguration::AnchorEntry> remapped;
    remapped.reserve(configuration.anchors.size());
    for (const auto &anchor : configuration.anchors)
    {
        const AnchorLookupKey lookup{.locator = anchor.locator, .lineId = anchor.lineId};
        const auto it = wanted.find(lookup);
        if (it == wanted.end() || !it->second.found || it->second.ambiguous)
        {
            continue;
        }
        auto copy = anchor;
        copy.locator.assign(flattenedLocator);
        copy.lineId = it->second.row;
        remapped.push_back(std::move(copy));
    }
    configuration.anchors = std::move(remapped);
}

std::string SerializeCompactConfiguration(const LogConfiguration &configuration)
{
    std::string json;
    const auto error = glz::write_json(configuration, json);
    if (error)
    {
        throw std::runtime_error(
            "Session bundle: failed to serialize configuration: " + glz::format_error(error, json)
        );
    }
    return json;
}

void ReplaceAtomically(const std::filesystem::path &temporary, const std::filesystem::path &destination)
{
#ifdef _WIN32
    // `MOVEFILE_WRITE_THROUGH` only affects cross-volume copies, so
    // dropping it saves a flag but does not change behaviour here
    // (temp file sits next to the destination on the same volume).
    if (::MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING) == 0)
    {
        throw std::runtime_error(
            "Session bundle: atomic replacement failed with Windows error " +
            std::to_string(static_cast<unsigned long>(::GetLastError()))
        );
    }
#else
    std::error_code error;
    std::filesystem::rename(temporary, destination, error);
    if (error)
    {
        throw std::runtime_error("Session bundle: atomic replacement failed: " + error.message());
    }
#endif
}

} // namespace

void WriteSessionBundle(
    const LogTable &table,
    const LogConfiguration &configuration,
    const std::filesystem::path &destination,
    const SessionBundleWriteOptions &options
)
{
    PollStop(options.stopToken);
    if (destination.empty() || destination.filename().empty())
    {
        throw std::invalid_argument("WriteSessionBundle requires a destination file");
    }

    const auto &lines = table.Data().Lines();
    const auto &keys = table.Data().Keys();
    if (lines.size() > SESSION_BUNDLE_MAX_ROWS)
    {
        throw std::length_error(
            "Session bundle row count exceeds the " + std::to_string(SESSION_BUNDLE_MAX_ROWS) + "-row limit"
        );
    }

    LogConfiguration embedded = configuration;

    // `locators` is display-shape (raw UTF-8 path); `locatorDedupKeys`
    // is the canonical form used for byte-equality dedup elsewhere
    // (lowercased with forward slashes on Windows via
    // `logapp::CanonicalLocator`). Anchors also flatten to the
    // canonical locator so `AnchorEntry::locator` matches the dedup
    // key after import -- otherwise a Windows round-trip would drop
    // every anchor because the case-preserving display path never
    // matches the lowercased anchor locator.
    const std::string displayLocator = internal::PathToUtf8(destination);
    const std::string dedupLocator = CanonicalizeSourceLocator(destination, options);
    RemapAnchors(embedded, lines, dedupLocator, options);
    embedded.source = LogConfiguration::Source{
        .kind = LogConfiguration::Source::Kind::File,
        .format = LogConfiguration::Source::Format::Json,
        .locators = {displayLocator},
        .locatorDedupKeys = {dedupLocator},
        .regexPattern = {},
    };

    const std::string configJson = SerializeCompactConfiguration(embedded);
    std::string metadata =
        "{\"__slv_bundle__\":{\"formatVersion\":" +
        std::to_string(SESSION_BUNDLE_FORMAT_VERSION) +
        ",\"rowCount\":" + std::to_string(static_cast<std::uint64_t>(lines.size())) +
        ",\"configuration\":" + configJson + "}}\n";
    if (metadata.size() > MAX_METADATA_BYTES)
    {
        throw std::length_error("Session bundle metadata exceeds the 64 MiB limit");
    }

    std::filesystem::path temporary = destination;
    temporary += ".tmp";
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);

    try
    {
        FileHandle file(temporary);
        ZstdWriter writer(file, options);
        writer.Write(metadata);

        for (std::size_t row = 0; row < lines.size(); ++row)
        {
            if ((row % PROGRESS_INTERVAL_ROWS) == 0)
            {
                PollStop(options.stopToken);
            }
            std::string json = internal::SerializeNormalizedJsonRow(lines[row], keys);
            if (json.size() > MAX_RECORD_BYTES)
            {
                throw std::length_error("Session bundle row exceeds the 256 MiB limit");
            }
            json.push_back('\n');
            writer.Write(json);
            if (options.progress && ((row + 1) % PROGRESS_INTERVAL_ROWS) == 0)
            {
                options.progress(row + 1, lines.size());
            }
        }

        PollStop(options.stopToken);
        writer.Finish();
        file.Close();
        // Fire the final tick exactly once. The inner loop already
        // emits at `(row + 1, lines.size())` when
        // `(row + 1) % PROGRESS_INTERVAL_ROWS == 0`, so a row count
        // that lands on an interval boundary would otherwise see
        // two `progress(lines.size(), lines.size())` calls (harmless
        // but confuses GUI throttling / test expectations). Skip the
        // tail call when the loop already delivered it; still fire
        // when there were zero rows so callers see one terminal
        // tick even for an empty bundle.
        const bool loopEmittedFinalTick =
            !lines.empty() && (lines.size() % PROGRESS_INTERVAL_ROWS) == 0;
        if (options.progress && !loopEmittedFinalTick)
        {
            options.progress(lines.size(), lines.size());
        }
        ReplaceAtomically(temporary, destination);
    }
    catch (...)
    {
        std::error_code cleanupError;
        std::filesystem::remove(temporary, cleanupError);
        throw;
    }
}

} // namespace loglib
