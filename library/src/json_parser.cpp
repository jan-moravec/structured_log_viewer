#include "json_parser.hpp"

#include "json_line.hpp"

#include <fstream>
#include <set>

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
        return ParseResult{LogData{}, "Failed to open file " + file.string()};
    }

    std::set<std::string> keys;
    std::vector<std::unique_ptr<LogLine>> lines;
    std::string error;
    size_t counter = 0;
    std::string line;
    while (std::getline(stream, line))
    {
        counter++;
        try
        {
            nlohmann::json json = nlohmann::json::parse(line);
            if (!json.is_object())
            {
                throw std::runtime_error("Line must be JSON object");
            }
            for (const auto &item : json.items())
            {
                keys.insert(item.key());
            }
            lines.push_back(std::make_unique<JsonLine>(std::move(json)));
        }
        catch (const std::exception &e)
        {
            if (error.empty())
            {
                error = "Error parsing file " + file.string() + ":\n";
            }
            error += "- line " + std::to_string(counter) + ": " + e.what() + "\n";
        }
    }

    std::vector<std::string> finalKeys;
    finalKeys.reserve(keys.size());
    std::move(std::make_move_iterator(keys.begin()), std::make_move_iterator(keys.end()), std::back_inserter(finalKeys));
    return ParseResult{LogData(std::move(lines), std::move(finalKeys)), error};
}

}
