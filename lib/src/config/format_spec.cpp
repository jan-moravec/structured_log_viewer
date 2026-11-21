#include "loglib/config/format_spec.hpp"

#include <optional>
#include <string_view>

namespace loglib
{

std::string_view ContinuationModeName(ContinuationMode mode) noexcept
{
    switch (mode)
    {
    case ContinuationMode::None:
        return "none";
    case ContinuationMode::Indented:
        return "indented";
    case ContinuationMode::UntilNextHeader:
        return "untilNextHeader";
    }
    return "none";
}

std::optional<ContinuationMode> ParseContinuationMode(std::string_view name) noexcept
{
    if (name == "none")
    {
        return ContinuationMode::None;
    }
    if (name == "indented")
    {
        return ContinuationMode::Indented;
    }
    if (name == "untilNextHeader")
    {
        return ContinuationMode::UntilNextHeader;
    }
    return std::nullopt;
}

} // namespace loglib
