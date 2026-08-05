#pragma once

#include "loglib/log_configuration.hpp"
#include "loglib/stop_token.hpp"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <functional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace loglib
{

class LogTable;

/// File extension for a session bundle produced by `WriteSessionBundle`.
constexpr const char *SESSION_BUNDLE_EXTENSION = ".slvbundle";

/// Encoder options for the single zstd frame.
struct SessionBundleWriteOptions
{
    /// zstd compression level (1 = fastest / worst, 22 = slowest / best,
    /// 3 = zstd's balanced default). Clamped by zstd to its permitted
    /// range at encode time.
    int compressionLevel = 3;

    /// Number of zstd workers. Zero uses zstd's single-threaded path.
    int totalWorkers = 0;

    /// Cooperative cancellation. When `stopToken.stop_requested()`
    /// flips to true, the writer throws `SessionBundleCancelled` and
    /// unlinks the in-flight temp file before returning.
    StopToken stopToken;

    /// Progress in source-model rows. Called on the writer thread.
    std::function<void(std::uint64_t rowsWritten, std::uint64_t rowsTotal)> progress;

    /// Canonicalizer applied to `line.Source()->Path()` before it
    /// is compared to `anchor.locator` during anchor remapping.
    ///
    /// `AnchorEntry::locator` shares the shape of
    /// `Source::locatorDedupKeys` (canonical, lowercased with
    /// forward slashes on Windows). Without a matching canonicalizer
    /// every anchor is silently dropped from the bundle. GUI callers
    /// wire this to `logapp::CanonicalLocator`; leaving it unset
    /// falls back to `path::u8string()` and is only correct when the
    /// caller-supplied anchor locators are already raw path strings
    /// (typical for test fixtures).
    std::function<std::string(const std::filesystem::path &)> canonicalizeSourceLocator;
};

/// Metadata stored on the first decompressed JSONL line.
struct SessionBundleMetadata
{
    std::uint32_t formatVersion = 0;
    std::uint64_t rowCount = 0;
    LogConfiguration configuration;
};

/// Thrown by `WriteSessionBundle` when its `StopToken` fires.
/// Distinct from `slv::exports::ExportCancelled` so callers can tell
/// bundle and row-export cancellations apart without matching text.
class SessionBundleCancelled : public std::exception
{
public:
    [[nodiscard]] const char *what() const noexcept override
    {
        return "Session bundle operation cancelled";
    }
};

/// Format-version mismatch: the bundle's `formatVersion` is not
/// what this build understands. Distinct type so the GUI can show
/// an "upgrade required" message instead of a generic parse error.
class SessionBundleVersionError : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

/// Generic bundle metadata failure.
class SessionBundleReadError : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

/// Write metadata plus every retained source-model row as compact
/// JSONL inside one checksummed zstd frame. The destination is
/// replaced atomically only after a successful final flush.
void WriteSessionBundle(
    const LogTable &table,
    const LogConfiguration &configuration,
    const std::filesystem::path &destination,
    const SessionBundleWriteOptions &options = {}
);

/// Parse and validate the first decompressed JSONL line.
[[nodiscard]] SessionBundleMetadata ParseSessionBundleMetadata(std::string_view json);

/// Cheap sniff that accepts every zstd input the decoder handles
/// (including a leading skippable frame before the standard frame,
/// via `DecompressingByteSource::SniffCodec`). Returns `false` for
/// missing / unreadable files or non-zstd magic; never throws.
[[nodiscard]] bool LooksLikeSessionBundle(const std::filesystem::path &file) noexcept;

/// On-disk format version emitted by `WriteSessionBundle`. Readers
/// require exactly this version; there is no legacy compatibility.
constexpr std::uint32_t SESSION_BUNDLE_FORMAT_VERSION = 1;

/// Shared cap on `rowCount` enforced by both writer and reader.
/// Kept as a single definition so a future bump can't silently
/// desync the two.
constexpr std::uint64_t SESSION_BUNDLE_MAX_ROWS = 1'000'000'000ULL;

} // namespace loglib
