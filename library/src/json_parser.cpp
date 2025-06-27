#include "loglib/json_parser.hpp"

#include "loglib/log_file.hpp"
#include "loglib/log_line.hpp"
#include "loglib/log_processing.hpp"

#include <glaze/glaze.hpp>
#include <mio/mmap.hpp>
#include <simdjson.h>

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
    simdjson::dom::parser parser;

    if (std::getline(stream, line))
    {
        try
        {
            auto result = parser.parse(line);
            return result.is_object();
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
    // Check if file exists and is not empty
    if (!std::filesystem::exists(file))
    {
        throw std::runtime_error("File '" + file.string() + "' does not exist.");
    }
    if (std::filesystem::file_size(file) == 0)
    {
        throw std::runtime_error("File '" + file.string() + "' is empty.");
    }

    // Memory map the file
    std::error_code errorMap;
    mio::mmap_source mmap = mio::make_mmap_source(file.string(), errorMap);
    if (errorMap)
    {
        throw std::runtime_error("Failed to memory map file '" + file.string() + "': " + errorMap.message());
    }

    std::set<std::string> keys;
    std::vector<LogLine> lines;
    std::vector<std::string> errors;
    auto logFile = std::make_unique<LogFile>(file);

    // Get file content as string view
    std::string_view fileContent(static_cast<const char *>(mmap.data()), mmap.size());
    // lines.reserve(std::min(size_t(10000), fileContent.size() / 200)); // Estimate based on average line length

    simdjson::dom::parser parser;
    size_t currentPos = 0;
    size_t lineNumber = 0;

    // Process each line
    while (currentPos < fileContent.size())
    {
        lineNumber++;

        // Find the end of the current line (optimize for common case)
        const char *start = fileContent.data() + currentPos;
        const char *end = fileContent.data() + fileContent.size();
        const char *newline = static_cast<const char *>(memchr(start, '\n', end - start));

        const size_t lineEnd = newline ? (newline - fileContent.data()) : fileContent.size();
        auto fileReference = logFile->CreateReference(lineEnd);
        std::string_view line(start, lineEnd - currentPos);

        // Move to the next line
        currentPos = lineEnd + 1;

        // Remove carriage return if present
        if (!line.empty() && line.back() == '\r')
        {
            line.remove_suffix(1);
        }

        // Skip empty lines
        if (!line.empty())
        {
            // Parse the line
            try
            {
                const size_t remaining = fileContent.size() - lineEnd;
                auto result = remaining >= simdjson::SIMDJSON_PADDING ? parser.parse(line.data(), line.size(), false)
                                                                      : parser.parse(line);
                if (result.error())
                {
                    errors.push_back(
                        "Error on line " + std::to_string(lineNumber) + ": " + simdjson::error_message(result.error())
                    );
                    continue;
                }

                simdjson::dom::element element = result.value();
                if (!element.is_object())
                {
                    errors.push_back("Error on line " + std::to_string(lineNumber) + ": Not a JSON object.");
                    continue;
                }

                // Parse the line and add it to our results
                auto parsedLine = ParseLine(element, keys);
                lines.emplace_back(std::move(parsedLine), std::move(fileReference));
            }
            catch (const std::exception &e)
            {
                errors.push_back("Error on line " + std::to_string(lineNumber) + ": " + e.what());
                continue;
            }
        }
    }

    if (lines.empty())
    {
        throw std::runtime_error("No valid JSON data found in the file.");
    }

    // Prepare the keys for the result
    std::vector<std::string> finalKeys;
    finalKeys.reserve(keys.size());
    std::move(keys.begin(), keys.end(), std::back_inserter(finalKeys));

    return ParseResult{LogData(std::move(logFile), std::move(lines), std::move(finalKeys)), std::move(errors)};
}

std::string JsonParser::ToString(const LogMap &values) const
{
    if (values.empty())
    {
        return "{}";
    }

    glz::json_t json;

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

    std::string result;
    const auto error = glz::write_json(json, result);
    if (error)
    {
        throw std::runtime_error("Failed to serialize JSON: " + glz::format_error(error));
    }

    return result;
}

LogMap JsonParser::ParseLine(const simdjson::dom::element &element, std::set<std::string> &keys)
{
    auto object = element.get_object();
    LogMap result;
    result.reserve(object.size());

    // Iterate through all key-value pairs in the JSON object
    for (auto [key_view, value] : object)
    {
        keys.emplace(key_view.data(), key_view.length());

        // Handle different value types based on the simdjson type
        switch (value.type())
        {
        case simdjson::dom::element_type::NULL_VALUE:
            result.emplace(std::string(key_view), std::monostate());
            break;
        case simdjson::dom::element_type::BOOL:
            result.emplace(std::string(key_view), bool(value));
            break;
        case simdjson::dom::element_type::INT64:
            result.emplace(std::string(key_view), int64_t(value));
            break;
        case simdjson::dom::element_type::UINT64:
            result.emplace(std::string(key_view), uint64_t(value));
            break;
        case simdjson::dom::element_type::DOUBLE:
            result.emplace(std::string(key_view), double(value));
            break;
        case simdjson::dom::element_type::STRING:
            result.emplace(std::string(key_view), std::string(std::string_view(value)));
            break;
        case simdjson::dom::element_type::ARRAY:
        case simdjson::dom::element_type::OBJECT: {
            result.emplace(std::string(key_view), simdjson::minify(value));
            break;
        }
        default:
            result.emplace(std::string(key_view), std::monostate());
            break;
        }
    }

    return result;
}

} // namespace loglib
