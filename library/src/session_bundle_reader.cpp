#include "loglib/session_bundle.hpp"

#include "loglib/internal/decompressing_byte_source.hpp"
#include "loglib/internal/log_configuration_glaze_meta.hpp"
#include "loglib/internal/log_configuration_glaze_opts.hpp"

#include <glaze/glaze.hpp>

#include <optional>
#include <string>

namespace loglib::internal
{

struct SessionBundleMetadataFields
{
    std::uint32_t formatVersion = 0;
    std::uint64_t rowCount = 0;
    LogConfiguration configuration;
};

struct SessionBundleMetadataEnvelope
{
    std::optional<SessionBundleMetadataFields> bundle;
};

} // namespace loglib::internal

template <> struct glz::meta<loglib::internal::SessionBundleMetadataFields>
{
    using T = loglib::internal::SessionBundleMetadataFields;
    static constexpr auto value =
        object("formatVersion", &T::formatVersion, "rowCount", &T::rowCount, "configuration", &T::configuration);
};

template <> struct glz::meta<loglib::internal::SessionBundleMetadataEnvelope>
{
    using T = loglib::internal::SessionBundleMetadataEnvelope;
    static constexpr auto value = object("__slv_bundle__", &T::bundle);
};

namespace loglib
{

SessionBundleMetadata ParseSessionBundleMetadata(std::string_view json)
{
    internal::SessionBundleMetadataEnvelope envelope;
    const auto error = glz::read<internal::LOG_CONFIG_OPTS>(envelope, json);
    if (error)
    {
        throw SessionBundleReadError(
            "Session bundle metadata is malformed: " + glz::format_error(error, std::string(json))
        );
    }
    if (!envelope.bundle.has_value())
    {
        throw SessionBundleReadError("Session bundle metadata envelope is missing");
    }
    if (envelope.bundle->formatVersion != SESSION_BUNDLE_FORMAT_VERSION)
    {
        throw SessionBundleVersionError(
            "Unsupported session bundle format version " + std::to_string(envelope.bundle->formatVersion) +
            " (expected " + std::to_string(SESSION_BUNDLE_FORMAT_VERSION) + ")"
        );
    }
    if (envelope.bundle->rowCount > SESSION_BUNDLE_MAX_ROWS)
    {
        throw SessionBundleReadError(
            "Session bundle row count exceeds the " + std::to_string(SESSION_BUNDLE_MAX_ROWS) + "-row limit"
        );
    }
    return SessionBundleMetadata{
        .formatVersion = envelope.bundle->formatVersion,
        .rowCount = envelope.bundle->rowCount,
        .configuration = std::move(envelope.bundle->configuration),
    };
}

bool LooksLikeSessionBundle(const std::filesystem::path &file) noexcept
{
    // Delegate to `SniffCodec` so every zstd shape the decoder
    // accepts (including a leading skippable frame at magic
    // `0x184D2A5?`) is recognised here too.
    return internal::DecompressingByteSource::SniffCodec(file) ==
           internal::DecompressingByteSource::Codec::Zstd;
}

} // namespace loglib
