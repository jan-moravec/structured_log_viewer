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

    /// Optional canonicalizer for source paths used during anchor
    /// remapping.
    ///
    /// `LogConfiguration::AnchorEntry::locator` is documented as
    /// matching `Source::locatorDedupKeys` — canonical, and on
    /// Windows lowercased with forward-slashed separators. The
    /// writer must therefore compare each anchor against a form of
    /// `line.Source()->Path()` in the SAME canonical shape or every
    /// anchor is silently dropped from the exported bundle.
    ///
    /// When set, the writer calls this on `line.Source()->Path()`
    /// before comparing to `anchor.locator`. GUI callers must pass a
    /// wrapper around `logapp::CanonicalLocator` (or an equivalent
    /// helper) so the canonical shape matches whatever produced the
    /// entries fed via `configuration.anchors`.
    ///
    /// When unset, the writer falls back to `path::u8string()`,
    /// which is only correct when the caller-provided anchor
    /// locators are raw path strings (test fixtures).
    std::function<std::string(const std::filesystem::path &)> canonicalizeSourceLocator;
};

/// Metadata stored on the first decompressed JSONL line.
struct SessionBundleMetadata
{
    std::uint32_t formatVersion = 0;
    std::uint64_t rowCount = 0;
    LogConfiguration configuration;
};

/// Thrown by `WriteSessionBundle` when its
/// `StopToken` is triggered mid-work. Sibling to
/// `slv::exports::ExportCancelled`; a separate type so callers can
/// distinguish the two without matching on message text.
class SessionBundleCancelled : public std::exception
{
public:
    [[nodiscard]] const char *what() const noexcept override
    {
        return "Session bundle operation cancelled";
    }
};

/// Format-version mismatch (`formatVersion` is not exactly the
/// this build understands). Distinct exception so the GUI can craft
/// a "upgrade required" message rather than a generic parse error.
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

/// Write metadata plus every retained source-model row as compact JSONL
/// inside one checksummed zstd frame. The destination is replaced
/// atomically only after a successful final frame flush.
void WriteSessionBundle(
    const LogTable &table,
    const LogConfiguration &configuration,
    const std::filesystem::path &destination,
    const SessionBundleWriteOptions &options = {}
);

/// Parse and validate the first decompressed JSONL line.
[[nodiscard]] SessionBundleMetadata ParseSessionBundleMetadata(std::string_view json);

/// Cheap sniff that accepts every zstd input the decoder handles.
/// Delegates to `DecompressingByteSource::SniffCodec`, so a leading
/// skippable frame preceding a standard zstd frame is also
/// recognised. Returns `false` for missing files, unreadable files,
/// or non-zstd magic without throwing.
[[nodiscard]] bool LooksLikeSessionBundle(const std::filesystem::path &file) noexcept;

/// Current on-disk format version emitted by `WriteSessionBundle`.
/// This shape has no legacy compatibility: readers require exactly v1.
constexpr std::uint32_t SESSION_BUNDLE_FORMAT_VERSION = 1;

/// Upper bound on `rowCount` for both the writer (refuses to encode
/// more) and the reader (refuses to decode more). Duplicated in the
/// two translation units would silently disagree if bumped; keep the
/// single definition here.
constexpr std::uint64_t SESSION_BUNDLE_MAX_ROWS = 1'000'000'000ULL;

} // namespace loglib
