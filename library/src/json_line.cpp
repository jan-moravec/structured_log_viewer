#include "loglib/json_line.hpp"

namespace loglib
{

JsonLine::JsonLine(nlohmann::json &&line) : mLine(std::move(line))
{
}

LogValue JsonLine::GetRawValue(const std::string &key) const
{
    auto it = mLine.find(key);
    if (it == mLine.end())
    {
        return std::monostate();
    }

    switch (it->type())
    {
    case nlohmann::json::value_t::null:
        return std::monostate();
    case nlohmann::json::value_t::object:
        return it->dump(4);
    case nlohmann::json::value_t::array:
        return it->dump(4);
    case nlohmann::json::value_t::string:
        return it->get<std::string>();
    case nlohmann::json::value_t::boolean:
        return it->get<bool>();
    case nlohmann::json::value_t::number_integer:
        return it->get<int64_t>();
    case nlohmann::json::value_t::number_unsigned:
        return it->get<int64_t>();
    case nlohmann::json::value_t::number_float:
        return it->get<double>();
    case nlohmann::json::value_t::binary:
        return it->dump();
    case nlohmann::json::value_t::discarded:
        return std::monostate();
    default:
        return std::monostate();
    }
}

std::string JsonLine::GetLine() const
{
    return mLine.dump();
}

} // namespace loglib
