#include "loglib/theme.hpp"

#include "loglib/internal/log_configuration_glaze_opts.hpp"
#include "loglib/internal/theme_glaze_meta.hpp"
#include "loglib/log_level.hpp"

#include <glaze/glaze.hpp>

#include <stdexcept>
#include <string>

namespace loglib
{

LevelStyle StyleForLevel(const Theme &theme, LogLevel level)
{
    const std::string_view canonical = CanonicalLevelName(level);
    const auto it = theme.levels.find(std::string(canonical));
    if (it == theme.levels.end())
    {
        return {};
    }
    return it->second;
}

Theme ParseTheme(std::string_view content)
{
    // `error_on_unknown_keys=false` lets new schema fields roll
    // forward without breaking older themes.
    constexpr auto OPTS = loglib::internal::LOG_CONFIG_OPTS;

    Theme parsed;
    const auto error = glz::read<OPTS>(parsed, content);
    if (error)
    {
        throw std::runtime_error("Failed to parse theme JSON: " + glz::format_error(error, std::string(content)));
    }
    return parsed;
}

std::string SerializeTheme(const Theme &theme)
{
    constexpr auto OPTS = loglib::internal::LOG_CONFIG_OPTS;

    std::string json;
    const auto error = glz::write<OPTS>(theme, json);
    if (error)
    {
        throw std::runtime_error("Failed to serialise theme JSON: " + glz::format_error(error));
    }
    return json;
}

} // namespace loglib
