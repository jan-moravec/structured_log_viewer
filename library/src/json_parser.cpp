#include "loglib/json_parser.hpp"

#include "loglib/json_parser.hpp"
#include "loglib/log_file.hpp"
#include "loglib/log_line.hpp"
#include "loglib/log_processing.hpp"

#include <nlohmann/json.hpp>

#include <fstream>

namespace loglib
{

bool JsonParser::IsValid(const std::filesystem::path &file) const
{
    std::ifstream stream(file);
    if (!stream.is_open())
    {
        return false;
    }

    std::string line;
    if (std::getline(stream, line))
    {
        try
        {
            nlohmann::json json = nlohmann::json::parse(line);
            if (json.is_object())
            {
                return true;
            }
        }
        catch (...)
        {
            return false;
        }
    }

    return false;
}

ParseResult JsonParser::Parse(const std::filesystem::path &file) const
{
    std::ifstream stream(file);
    if (!stream.is_open())
    {
        throw std::runtime_error("Failed to open file " + file.string());
    }

    std::set<std::string> keys;
    std::vector<LogLine> lines;
    std::vector<std::string> errors;
    std::string line;
    auto logFile = std::make_unique<LogFile>(file);

    while (std::getline(stream, line))
    {
        try
        {
            lines.emplace_back(ParseLine(nlohmann::json::parse(line), keys), logFile->CreateReference(stream.tellg()));
        }
        catch (const std::exception &e)
        {
            errors.emplace_back(
                "Line " + std::to_string(lines.size() + errors.size()) + " '" + line + "': " + e.what()
            );
        }
    }

    std::vector<std::string> finalKeys;
    finalKeys.reserve(keys.size());
    std::move(keys.begin(), keys.end(), std::back_inserter(finalKeys));

    return ParseResult{LogData(std::move(logFile), std::move(lines), std::move(finalKeys)), std::move(errors)};
}

std::string JsonParser::ToString(const LogMap &values) const
{
    // The default type nlohmann::json uses a std::map to store JSON objects, and thus stores object keys sorted
    // alphabetically.
    nlohmann::json json = nlohmann::json::object();
    for (const auto &pair : values)
    {
        const auto &key = pair.first;
        const auto &value = pair.second;

        std::visit(
            [&json, &key](const auto &val) {
                using T = std::decay_t<decltype(val)>;
                if constexpr (std::is_same_v<T, std::monostate>)
                {
                    json[key] = nullptr;
                }
                else if constexpr (std::is_same_v<T, TimeStamp>)
                {
                    json[key] = TimeStampToDateTimeString(val);
                }
                else
                {
                    json[key] = val;
                }
            },
            value
        );
    }
    return json.dump();
}

LogMap JsonParser::ParseLine(const nlohmann::json &json, std::set<std::string> &keys)
{
    if (!json.is_object())
    {
        throw std::runtime_error("Line must be JSON object");
    }

    LogMap result;

    for (const auto &item : json.items())
    {
        keys.insert(item.key());

        switch (item.value().type())
        {
        case nlohmann::json::value_t::null:
            result.emplace(item.key(), std::monostate());
            break;
        case nlohmann::json::value_t::object:
            result.emplace(item.key(), item.value().dump(4));
            break;
        case nlohmann::json::value_t::array:
            result.emplace(item.key(), item.value().dump(4));
            break;
        case nlohmann::json::value_t::string:
            result.emplace(item.key(), item.value().get<std::string>());
            break;
        case nlohmann::json::value_t::boolean:
            result.emplace(item.key(), item.value().get<bool>());
            break;
        case nlohmann::json::value_t::number_integer:
            result.emplace(item.key(), item.value().get<int64_t>());
            break;
        case nlohmann::json::value_t::number_unsigned:
            result.emplace(item.key(), item.value().get<uint64_t>());
            break;
        case nlohmann::json::value_t::number_float:
            result.emplace(item.key(), item.value().get<double>());
            break;
        case nlohmann::json::value_t::binary:
            result.emplace(item.key(), item.value().dump());
            break;
        case nlohmann::json::value_t::discarded:
            result.emplace(item.key(), std::monostate());
            break;
        default:
            result.emplace(item.key(), std::monostate());
            break;
        }
    }

    return result;
}
} // namespace loglib
