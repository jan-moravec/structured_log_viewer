#include <test_common/json_log_line.hpp>

#include <type_traits>
#include <utility>

namespace test_common
{

JsonLogLine::JsonLogLine(const char *line)
{
    glz::generic_sorted_u64 json;
    auto error = glz::read_json(json, line);
    if (!error)
    {
        data = std::move(json);
    }
    else
    {
        data = std::string(line);
    }
}

JsonLogLine::JsonLogLine(glz::generic_sorted_u64 json) : data(std::move(json))
{
}

std::string JsonLogLine::ToString() const
{
    return std::visit(
        [](const auto &payload) -> std::string {
            using T = std::decay_t<decltype(payload)>;
            if constexpr (std::is_same_v<T, glz::generic_sorted_u64>)
            {
                return glz::write_json(payload).value_or("");
            }
            else if constexpr (std::is_same_v<T, std::string>)
            {
                return payload;
            }
        },
        data
    );
}

void JsonLogLine::Parse(std::vector<std::string> &strings, std::vector<glz::generic_sorted_u64> &jsons) const
{
    std::visit(
        [&](const auto &payload) {
            using T = std::decay_t<decltype(payload)>;
            if constexpr (std::is_same_v<T, glz::generic_sorted_u64>)
            {
                jsons.push_back(payload);
            }
            else if constexpr (std::is_same_v<T, std::string>)
            {
                strings.push_back(payload);
            }
        },
        data
    );
}

} // namespace test_common
