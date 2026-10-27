#pragma once

#include <glaze/glaze.hpp>

#include <cstdint>
#include <stdexcept>
#include <string>

namespace slp
{

/** @brief glaze options for `--output-format json` reports: pretty-printed with two-space indents. */
struct ReportJsonOptions : glz::opts
{
    std::uint8_t indentation_width = 2;
};

inline constexpr ReportJsonOptions REPORT_JSON_OPTIONS{{.prettify = true}};

/**
 * @brief Serializes @p report as a pretty-printed JSON document.
 *
 * Members map to keys by name, in declaration order. Unset optionals are
 * left out.
 *
 * @param report Aggregate whose members glaze can reflect.
 * @return The document, ending in a newline.
 */
template <typename Report> [[nodiscard]] std::string WriteJsonReport(const Report &report)
{
    std::string json;
    if (const auto error = glz::write<REPORT_JSON_OPTIONS>(report, json))
    {
        throw std::runtime_error("could not serialize the JSON report: " + glz::format_error(error));
    }
    json.push_back('\n');
    return json;
}

} // namespace slp
