#include "loglib/session_bundle.hpp"

#include "loglib/internal/log_configuration_glaze_meta.hpp"
#include "loglib/internal/log_configuration_glaze_opts.hpp"
#include "loglib/internal/path_encoding.hpp"

#include <glaze/glaze.hpp>

#include <array>
#include <cstdio>
#include <cstring>
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
    constexpr std::uint64_t MAX_BUNDLE_ROWS = 1'000'000'000ULL;
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
    if (envelope.bundle->rowCount > MAX_BUNDLE_ROWS)
    {
        throw SessionBundleReadError("Session bundle row count exceeds the one-billion-row limit");
    }
    return SessionBundleMetadata{
        .formatVersion = envelope.bundle->formatVersion,
        .rowCount = envelope.bundle->rowCount,
        .configuration = std::move(envelope.bundle->configuration),
    };
}

bool LooksLikeSessionBundle(const std::filesystem::path &file) noexcept
{
    std::FILE *stream = internal::OpenFileForBinaryRead(file);
    if (stream == nullptr)
    {
        return false;
    }
    std::array<unsigned char, 4> magic{};
    const std::size_t count = std::fread(magic.data(), 1, magic.size(), stream);
    (void)std::fclose(stream);
    constexpr std::array<unsigned char, 4> ZSTD_MAGIC{0x28, 0xb5, 0x2f, 0xfd};
    return count == magic.size() && std::memcmp(magic.data(), ZSTD_MAGIC.data(), magic.size()) == 0;
}

} // namespace loglib
