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

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <random>
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
#include <fcntl.h>
#include <io.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
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

/// Build a per-writer temporary path that is unique within a process
/// and (statistically) between processes.
///
/// The previous implementation hard-coded `destination + ".tmp"`,
/// which lets two concurrent writes to the same destination
/// clobber each other's staging file. Even benign parallelism -- a
/// user re-exporting on top of an autosave that is still finishing,
/// two tests exercising the writer against a shared path, or two
/// background workers racing to persist a session -- would race
/// on the shared `.tmp` and produce a corrupted final file after
/// the atomic rename.
///
/// The suffix format is `.<seed>.<counter>.tmp`:
///   - `seed` is drawn once per process from `std::random_device`.
///     Two processes writing to the same directory get different
///     seeds, so their staging files can't collide.
///   - `counter` is a process-wide atomic counter. Two concurrent
///     writers in the same process get distinct counter values,
///     so their staging files can't collide either.
///
/// Kept human-eyeballable at the debug level (hex digits, no
/// PID-style leaks) but random enough that a stale abandoned
/// `.tmp` file (from a previous run whose `catch (...)` cleanup
/// didn't reach) will not be reused unless the seed collides,
/// which is O(2^-64) between runs.
std::filesystem::path MakeStagingTempPath(const std::filesystem::path &destination)
{
    constexpr unsigned int RANDOM_DEVICE_BITS = 32U;
    constexpr std::size_t SUFFIX_BUFFER_SIZE = 64;

    static const std::uint64_t PROCESS_SEED = []() {
        std::random_device rd;
        return (static_cast<std::uint64_t>(rd()) << RANDOM_DEVICE_BITS) | static_cast<std::uint64_t>(rd());
    }();
    static std::atomic<std::uint64_t> counter{0};

    const std::uint64_t next = counter.fetch_add(1, std::memory_order_relaxed);
    std::array<char, SUFFIX_BUFFER_SIZE> suffix{};
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg,cert-err33-c) -- fixed-size buffer, format is trusted.
    const int written = std::snprintf(
        suffix.data(), suffix.size(), ".%016llx.%016llx.tmp",
        static_cast<unsigned long long>(PROCESS_SEED),
        static_cast<unsigned long long>(next)
    );
    if (written <= 0 || static_cast<std::size_t>(written) >= suffix.size())
    {
        // `snprintf` failure or unexpected truncation: fall back to a
        // deterministic-but-still-unique suffix so the write can proceed.
        // The counter alone is sufficient for in-process uniqueness.
        std::filesystem::path result = destination;
        result += ".";
        result += std::to_string(next);
        result += ".tmp";
        return result;
    }
    std::filesystem::path result = destination;
    result += suffix.data();
    return result;
}

enum class ExclusiveOpenStatus
{
    Ok,
    AlreadyExists,
    OtherError,
};

/// Open @p path exclusively for binary writing. Fails cleanly (rather
/// than truncating) if the file already exists so the writer can
/// retry with a fresh suffix instead of clobbering a concurrent
/// writer's staging file. The seed+counter naming in
/// `MakeStagingTempPath` makes an actual collision astronomically
/// unlikely; this is the safety net that turns an unreproducible
/// bug into a bounded retry.
///
/// Localised here rather than in `path_encoding.hpp` so the Win32
/// headers this needs (`CreateFileW`, `_open_osfhandle`) stay
/// contained in the one translation unit that requires them.
std::FILE *OpenExclusiveForBinaryWrite(
    const std::filesystem::path &path, ExclusiveOpenStatus &outStatus
) noexcept
{
    outStatus = ExclusiveOpenStatus::OtherError;
#ifdef _WIN32
    // `HANDLE` is a Windows typedef for `void *`, so any const-
    // qualified form reads as "pointer const" not "pointee const";
    // suppress the clang-tidy warning that treats the typedef as
    // ambiguous. The pointer must not be reassigned once
    // `CreateFileW` returns.
    // NOLINTNEXTLINE(misc-misplaced-const)
    HANDLE const handle = ::CreateFileW(
        path.native().c_str(),
        GENERIC_WRITE,
        0, // no sharing while we are actively writing the staging file
        nullptr,
        CREATE_NEW, // fails with ERROR_FILE_EXISTS if the file exists
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );
    if (handle == INVALID_HANDLE_VALUE)
    {
        const unsigned long win32Error = ::GetLastError();
        outStatus = (win32Error == ERROR_FILE_EXISTS || win32Error == ERROR_ALREADY_EXISTS)
                        ? ExclusiveOpenStatus::AlreadyExists
                        : ExclusiveOpenStatus::OtherError;
        return nullptr;
    }
    // NOLINTNEXTLINE(performance-no-int-to-ptr) -- CRT idiom; `_open_osfhandle` takes intptr_t by design.
    const int fd = ::_open_osfhandle(reinterpret_cast<intptr_t>(handle), _O_BINARY);
    if (fd == -1)
    {
        // `_open_osfhandle` failure keeps the HANDLE alive; close it
        // manually so we do not leak the underlying kernel object.
        ::CloseHandle(handle);
        outStatus = ExclusiveOpenStatus::OtherError;
        return nullptr;
    }
    std::FILE *fp = ::_fdopen(fd, "wb");
    if (fp == nullptr)
    {
        // `_close` on a CRT fd also releases the wrapped HANDLE, so
        // there is nothing else to unwind here.
        ::_close(fd);
        outStatus = ExclusiveOpenStatus::OtherError;
        return nullptr;
    }
    outStatus = ExclusiveOpenStatus::Ok;
    return fp;
#else
    const int fd = ::open(
        path.c_str(),
        O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC,
        S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH
    );
    if (fd == -1)
    {
        outStatus =
            (errno == EEXIST) ? ExclusiveOpenStatus::AlreadyExists : ExclusiveOpenStatus::OtherError;
        return nullptr;
    }
    std::FILE *fp = ::fdopen(fd, "wb");
    if (fp == nullptr)
    {
        ::close(fd);
        outStatus = ExclusiveOpenStatus::OtherError;
        return nullptr;
    }
    outStatus = ExclusiveOpenStatus::Ok;
    return fp;
#endif
}

class FileHandle
{
public:
    /// Adopt @p alreadyOpen (from `OpenExclusiveForBinaryWrite`).
    /// Ownership transfers unconditionally; passing a null pointer
    /// is a programming error and traps loudly rather than
    /// silently producing a handle that no-ops every write.
    explicit FileHandle(std::filesystem::path path, std::FILE *alreadyOpen)
        : mPath(std::move(path)), mFile(alreadyOpen)
    {
        if (mFile == nullptr)
        {
            throw std::runtime_error(
                "Session bundle: FileHandle received a null FILE* for '" +
                internal::PathToUtf8(mPath) + "'"
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

    /// Flush userspace + kernel buffers, then close. Every layer
    /// runs unconditionally: skipping `fclose` after a failed
    /// `fflush` would leak the FILE*, and skipping the durable-sync
    /// step after a successful flush would leave a `.tmp` that is
    /// not power-fail safe once `ReplaceAtomically` renames it into
    /// place. See the `SyncToDisk` helper below for the platform
    /// break-down.
    void Close()
    {
        if (mFile == nullptr)
        {
            return;
        }
        // Split flush + sync + close so the FILE* is always released.
        // Short-circuiting on a failed step would skip `fclose` and
        // leak the handle -- and by then `mFile = nullptr` has
        // disarmed the destructor's fallback close, so the OS
        // handle would live until process exit.
        const bool flushOk = std::fflush(mFile) == 0;
        const bool syncOk = flushOk && SyncToDisk(mFile);
        const bool closeOk = std::fclose(mFile) == 0;
        mFile = nullptr;
        if (!flushOk || !syncOk || !closeOk)
        {
            throw std::runtime_error(
                "Session bundle: failed to durably flush '" + internal::PathToUtf8(mPath) + "'"
            );
        }
    }

private:
    /// Push the kernel-side page cache for @p file to durable
    /// storage. On Windows this is `FlushFileBuffers`; on POSIX
    /// it is `fsync`. Called between `fflush` and `fclose` so a
    /// hard crash between `WriteSessionBundle`'s `file.Close()`
    /// and its subsequent `ReplaceAtomically` cannot leave a
    /// `.tmp` whose contents are on disk in name only. The
    /// rename itself is atomic on both platforms, so once we
    /// sync + rename the destination is fully durable.
    static bool SyncToDisk(std::FILE *file) noexcept
    {
#ifdef _WIN32
        const int fd = ::_fileno(file);
        if (fd == -1)
        {
            return false;
        }
        // NOLINTNEXTLINE(performance-no-int-to-ptr) -- CRT idiom; `_get_osfhandle` returns intptr_t by design.
        auto *const handle = reinterpret_cast<HANDLE>(::_get_osfhandle(fd));
        if (handle == INVALID_HANDLE_VALUE)
        {
            return false;
        }
        return ::FlushFileBuffers(handle) != 0;
#else
        const int fd = ::fileno(file);
        if (fd == -1)
        {
            return false;
        }
        return ::fsync(fd) == 0;
#endif
    }

    std::filesystem::path mPath;
    std::FILE *mFile = nullptr;
};

/// Retry-capable staging-file open. Rotates through fresh names from
/// `MakeStagingTempPath` on collision and gives up after a bounded
/// number of attempts. On failure `outTemporary` is left untouched;
/// on success it holds the path that was actually opened, ready to
/// hand to `ReplaceAtomically` later.
std::FILE *OpenStagingFileWithRetry(
    const std::filesystem::path &destination, std::filesystem::path &outTemporary
)
{
    // The `MakeStagingTempPath` suffix draws from a 64-bit random
    // seed plus a process-local monotonic counter, so an actual
    // collision requires either a seed match between two processes
    // (~2^-64) or a leftover staging file from a previous run whose
    // catch-block cleanup did not reach. Eight attempts trivially
    // covers both cases and still fails fast if the destination
    // directory is fundamentally not writable.
    constexpr int MAX_ATTEMPTS = 8;

    ExclusiveOpenStatus lastStatus = ExclusiveOpenStatus::OtherError;
#ifdef _WIN32
    unsigned long lastWin32Error = 0;
#else
    int lastErrno = 0;
#endif
    std::filesystem::path attemptedPath;
    for (int attempt = 0; attempt < MAX_ATTEMPTS; ++attempt)
    {
        attemptedPath = MakeStagingTempPath(destination);
        ExclusiveOpenStatus status = ExclusiveOpenStatus::OtherError;
        std::FILE *fp = OpenExclusiveForBinaryWrite(attemptedPath, status);
        if (status == ExclusiveOpenStatus::Ok)
        {
            outTemporary = attemptedPath;
            return fp;
        }
        // Snapshot the OS error before another syscall clobbers it.
#ifdef _WIN32
        lastWin32Error = ::GetLastError();
#else
        lastErrno = errno;
#endif
        lastStatus = status;
        if (status != ExclusiveOpenStatus::AlreadyExists)
        {
            // Hard I/O error (permission denied, no such directory,
            // out of file handles, ...). Retrying under a different
            // suffix will not help; propagate immediately.
            break;
        }
        // Collision: fall through and generate a fresh suffix.
    }
    std::string reason;
#ifdef _WIN32
    reason = "Windows error " + std::to_string(lastWin32Error);
#else
    reason = "errno " + std::to_string(lastErrno);
#endif
    if (lastStatus == ExclusiveOpenStatus::AlreadyExists)
    {
        reason += " (staging path collided " + std::to_string(MAX_ATTEMPTS) + " times)";
    }
    throw std::runtime_error(
        "Session bundle: failed to open staging file for '" + internal::PathToUtf8(destination) +
        "': " + reason
    );
}

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

/// Composite key for the anchor index below. Stores by `std::string`
/// because the canonicalizer returns by value; the bytes must outlive
/// the insertion loop.
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
/// `(canonical locator, lineId)` matched more than once, so the
/// anchor cannot resolve to a single dense row and is dropped.
struct AnchorMatchSlot
{
    std::uint64_t row = 0;
    bool found = false;
    bool ambiguous = false;
};

/// Wanted-set keyed on anchor lookup pairs. Sized by the anchor
/// count (typically a handful), not by `lines.size()`, so a billion
/// rows with two anchors don't blow up the map. O(A + N) overall.
using AnchorWantedSet = std::unordered_map<AnchorLookupKey, AnchorMatchSlot, AnchorLookupKeyHash>;

AnchorWantedSet BuildAnchorWantedSet(const std::vector<LogConfiguration::AnchorEntry> &anchors)
{
    AnchorWantedSet wanted;
    wanted.reserve(anchors.size());
    for (const auto &anchor : anchors)
    {
        // `try_emplace`: duplicate anchor keys collapse to one slot.
        // The wanted-set only resolves rows; `RemapAnchors` preserves
        // the caller's original list on the output side.
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
    // Cache canonical locators per `Source*` so the (potentially
    // expensive) `canonicalizeSourceLocator` callback runs once per
    // source rather than once per row.
    std::unordered_map<const LineSource *, std::string> canonicalCache;

    for (std::size_t row = 0; row < lines.size(); ++row)
    {
        const LogLine &line = lines[row];
        const auto *source = line.Source();
        // Skip rows with no source: they cannot be anchor targets.
        // Without this guard, an anchor whose `locator` is empty
        // (e.g. from a corrupted or hand-edited config) would false-
        // match every null-source row that happens to share its
        // `lineId`, producing a nonsensical dense-row remap that
        // gets persisted into the bundle's embedded anchors.
        // Legitimate anchors always reference a source file, so
        // requiring `source != nullptr` is the correct semantics.
        if (source == nullptr)
        {
            continue;
        }
        auto cacheIt = canonicalCache.find(source);
        if (cacheIt == canonicalCache.end())
        {
            cacheIt =
                canonicalCache.emplace(source, CanonicalizeSourceLocator(source->Path(), options)).first;
        }
        AnchorLookupKey key;
        key.lineId = static_cast<std::uint64_t>(line.LineId());
        key.locator = cacheIt->second;
        // Belt-and-braces: even for a real source, if the canonical
        // locator normalized to the empty string (custom
        // `canonicalizeSourceLocator` returning "" for a path outside
        // the recognised session root, for instance) we must not let
        // an anchor with an empty `locator` false-match against it.
        if (key.locator.empty())
        {
            continue;
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
            // Duplicate `(locator, lineId)`: drop rather than pick
            // an arbitrary row.
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

/// strftime format that matches `AppendTimestampJson`'s output:
/// ISO-8601 with a `T` date-time separator, microsecond fraction,
/// and a trailing `Z`. Kept in the writer so the writer's format
/// choice and the reader's parse configuration cannot drift.
constexpr std::string_view BUNDLE_TIME_PARSE_FORMAT = "%FT%T";

/// Ensure every `Type::Time` column in @p configuration has a
/// `parseFormat` capable of parsing the ISO-8601 timestamps this
/// writer emits. Auto-detected Time columns already ship with
/// `%FT%T` in their defaults, so this is a no-op there. Manually
/// configured columns (e.g. `parseFormats = {"%d/%m/%Y %H:%M:%S"}`)
/// would otherwise round-trip to 100% mismatched timestamps on
/// import because the exported values are always ISO-8601 UTC,
/// regardless of how the source produced them.
///
/// Prepended, not appended, so the fast-path ISO parser is tried
/// first: it beats `date::from_stream` by roughly an order of
/// magnitude on the hot import loop.
void NormalizeTimeColumnParseFormats(LogConfiguration &configuration)
{
    for (auto &column : configuration.columns)
    {
        if (column.type != LogConfiguration::Type::Time)
        {
            continue;
        }
        const auto exists = std::ranges::any_of(column.parseFormats, [](const std::string &format) {
            return format == BUNDLE_TIME_PARSE_FORMAT;
        });
        if (!exists)
        {
            column.parseFormats.insert(column.parseFormats.begin(), std::string(BUNDLE_TIME_PARSE_FORMAT));
        }
    }
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
    // Temp file lives next to the destination on the same volume,
    // so `MOVEFILE_WRITE_THROUGH` (a cross-volume knob) is a no-op
    // and omitted.
    if (::MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING) == 0)
    {
        const unsigned long win32Error = ::GetLastError();
        // `ERROR_SHARING_VIOLATION` (32) is by far the most common
        // failure users hit on Windows: they still have the target
        // file open in another viewer, an antivirus scanner has a
        // transient handle on it, or a sync client (OneDrive /
        // Dropbox) is uploading it. Surfacing the bare Win32 code
        // is opaque -- the user cannot correlate "error 32" with
        // "close the file"; pair the code with the exact remediation.
        const std::string destUtf8 = internal::PathToUtf8(destination);
        if (win32Error == ERROR_SHARING_VIOLATION)
        {
            throw std::runtime_error(
                "Session bundle: cannot replace '" + destUtf8 +
                "' because another program has it open (Windows error 32, ERROR_SHARING_VIOLATION). "
                "Close the file in the other program (log viewer, editor, antivirus scan, sync client) and try again."
            );
        }
        if (win32Error == ERROR_ACCESS_DENIED)
        {
            throw std::runtime_error(
                "Session bundle: cannot replace '" + destUtf8 +
                "' due to a permissions error (Windows error 5, ERROR_ACCESS_DENIED). "
                "Check that you can write to the destination folder and that the target file is not read-only."
            );
        }
        throw std::runtime_error(
            "Session bundle: atomic replacement of '" + destUtf8 +
            "' failed with Windows error " + std::to_string(win32Error)
        );
    }
#else
    std::error_code error;
    std::filesystem::rename(temporary, destination, error);
    if (error)
    {
        throw std::runtime_error(
            "Session bundle: atomic replacement of '" + internal::PathToUtf8(destination) +
            "' failed: " + error.message()
        );
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
    NormalizeTimeColumnParseFormats(embedded);

    // `locators` holds the display path (raw UTF-8) while
    // `locatorDedupKeys` holds the canonical byte-equality form
    // (Windows: lowercased with forward slashes via
    // `logapp::CanonicalLocator`). Anchors also flatten to the
    // canonical locator so they match the dedup key after import;
    // otherwise a Windows round-trip drops every anchor.
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

    // Per-writer staging path (see `MakeStagingTempPath`): the
    // previous fixed `.tmp` suffix could race with a concurrent
    // export to the same destination. `OpenStagingFileWithRetry`
    // opens the file exclusively (`CREATE_NEW` / `O_EXCL`) and
    // rotates through fresh names on the astronomically unlikely
    // collision so a stale sibling from a crashed run cannot be
    // silently truncated.
    std::filesystem::path temporary;
    std::FILE *stagingFile = OpenStagingFileWithRetry(destination, temporary);

    try
    {
        FileHandle file(temporary, stagingFile);
        // `FileHandle` now owns the FILE*; null out the local so the
        // catch(...) block below does not double-close it.
        stagingFile = nullptr;
        ZstdWriter writer(file, options);
        writer.Write(metadata);

        // Hoisted row buffer: reused (clear + append) across every
        // row so the amortised capacity survives the loop. Prior to
        // the out-parameter overload this loop allocated a fresh
        // `std::string` per row, which is a measurable fraction of
        // the write cost at multi-million-row scale.
        std::string rowBuffer;
        for (std::size_t row = 0; row < lines.size(); ++row)
        {
            if ((row % PROGRESS_INTERVAL_ROWS) == 0)
            {
                PollStop(options.stopToken);
            }
            rowBuffer.clear();
            internal::SerializeNormalizedJsonRow(lines[row], keys, rowBuffer);
            if (rowBuffer.size() > MAX_RECORD_BYTES)
            {
                throw std::length_error("Session bundle row exceeds the 256 MiB limit");
            }
            rowBuffer.push_back('\n');
            writer.Write(rowBuffer);
            if (options.progress && ((row + 1) % PROGRESS_INTERVAL_ROWS) == 0)
            {
                options.progress(row + 1, lines.size());
            }
        }

        PollStop(options.stopToken);
        writer.Finish();
        file.Close();
        // Fire the terminal progress tick exactly once. Skip it if
        // the row count landed on an interval boundary (the loop
        // already emitted `(N, N)`); still fire for an empty bundle
        // so callers always see one final tick.
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
        // Ownership rescue: if the exception fired between
        // `OpenStagingFileWithRetry` returning and `FileHandle`
        // taking over, close the raw handle before removing the
        // staging file. Once `FileHandle` has ownership the local
        // is nulled out and this branch is a no-op.
        if (stagingFile != nullptr)
        {
            (void)std::fclose(stagingFile);
            stagingFile = nullptr;
        }
        std::error_code cleanupError;
        std::filesystem::remove(temporary, cleanupError);
        throw;
    }
}

} // namespace loglib
