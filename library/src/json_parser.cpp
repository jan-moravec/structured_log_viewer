#include "loglib/json_parser.hpp"

#include "loglib/log_file.hpp"
#include "loglib/log_line.hpp"
#include "loglib/log_processing.hpp"

#include <fmt/format.h>
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
        throw std::runtime_error(fmt::format("File '{}' does not exist.", file.string()));
    }
    if (std::filesystem::file_size(file) == 0)
    {
        throw std::runtime_error(fmt::format("File '{}' is empty.", file.string()));
    }

    // Memory map the file
    std::error_code errorMap;
    mio::mmap_source mmap = mio::make_mmap_source(file.string(), errorMap);
    if (errorMap)
    {
        throw std::runtime_error(fmt::format("Failed to memory map file '{}': {}", file.string(), errorMap.message()));
    }

    ParseCache cache;
    std::vector<LogLine> lines;
    std::vector<std::string> errors;
    auto logFile = std::make_unique<LogFile>(file);

    // Get file content as string view
    std::string_view fileContent(static_cast<const char *>(mmap.data()), mmap.size());
    lines.reserve(2000);
    std::string linePadded;

    simdjson::ondemand::parser parser;
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
        auto fileReference = logFile->CreateReference(lineEnd + 1);
        std::string_view line(start, lineEnd - currentPos);

        // Move to the next line
        currentPos = lineEnd + 1;

        // Adjust reservation after processing some lines to get a better estimate
        if (lineNumber == 1000)
        {
            double avgLineLength = static_cast<double>(currentPos) / lineNumber;
            size_t estimatedLines = static_cast<size_t>(fileContent.size() * 1.5 / avgLineLength);
            // Re-reserve with better estimate, still capping at a reasonable maximum
            lines.reserve(estimatedLines);
        }

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
                auto result = remaining >= simdjson::SIMDJSON_PADDING
                                  ? parser.iterate(line.data(), line.size(), line.size() + remaining)
                                  : parser.iterate(simdjson::pad(linePadded.assign(line)));
                if (result.error())
                {
                    errors.push_back(
                        fmt::format("Error on line {}: {}", lineNumber, simdjson::error_message(result.error()))
                    );
                    continue;
                }

                auto object = result.get_object();
                if (object.error())
                {
                    errors.push_back(fmt::format("Error on line {}: Not a JSON object.", lineNumber));
                    continue;
                }

                auto objectValue = object.value();
                // Parse the line and add it to our results
                auto parsedLine = ParseLine(objectValue, cache);
                lines.emplace_back(std::move(parsedLine), std::move(fileReference));
            }
            catch (const std::exception &e)
            {
                errors.push_back(fmt::format("Error on line {}: {}", lineNumber, e.what()));
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
    finalKeys.reserve(cache.keyTypes.size());
    for (const auto &keyType : cache.keyTypes)
    {
        finalKeys.push_back(keyType.first);
    }
    std::sort(finalKeys.begin(), finalKeys.end());

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
        throw std::runtime_error(fmt::format("Failed to serialize JSON: {}", glz::format_error(error)));
    }

    return result;
}

LogMap JsonParser::ParseLine(simdjson::ondemand::object &object, ParseCache &cache)
{
    LogMap result;

    auto countFieldsResult = object.count_fields();
    if (countFieldsResult.error())
    {
        throw std::runtime_error(
            fmt::format("Error counting fields: {}", simdjson::error_message(countFieldsResult.error()))
        );
    }
    if (countFieldsResult.value() == 0)
    {
        return result;
    }
    result.reserve(std::ceil(countFieldsResult.value() / result.max_load_factor()));

    // Iterate through all key-value pairs in the JSON object
    for (auto field : object)
    {
        auto key_result = field.unescaped_key();
        if (key_result.error())
        {
            continue; // Skip fields with invalid keys
        }

        std::string_view key_view = key_result.value();
        std::string key_str(key_view);

        // Handle different value types
        auto value = field.value();

        // Check if we have cached type information
        auto typeIt = cache.keyTypes.find(key_str);
        if (typeIt != cache.keyTypes.end())
        {
            bool cacheValid = false;

            // Use cached type information
            auto type = typeIt->second;
            switch (type)
            {
            case simdjson::ondemand::json_type::boolean: {
                bool bool_value;
                if (!value.get(bool_value))
                {
                    result.emplace(std::move(key_str), bool_value);
                    cacheValid = true;
                }
                break;
            }
            case simdjson::ondemand::json_type::number: {
                // Use cached number type if available
                auto numTypeIt = cache.numberTypes.find(key_str);
                if (numTypeIt != cache.numberTypes.end())
                {
                    switch (numTypeIt->second)
                    {
                    case simdjson::ondemand::number_type::signed_integer: {
                        int64_t int_value;
                        if (!value.get(int_value))
                        {
                            result.emplace(std::move(key_str), int_value);
                            cacheValid = true;
                        }
                        break;
                    }
                    case simdjson::ondemand::number_type::unsigned_integer: {
                        uint64_t uint_value;
                        if (!value.get(uint_value))
                        {
                            result.emplace(std::move(key_str), uint_value);
                            cacheValid = true;
                        }
                        break;
                    }
                    case simdjson::ondemand::number_type::floating_point_number: {
                        double double_value;
                        if (!value.get(double_value))
                        {
                            result.emplace(std::move(key_str), double_value);
                            cacheValid = true;
                        }
                        break;
                    }
                    default:
                        break;
                    }
                }
                break;
            }
            case simdjson::ondemand::json_type::string: {
                std::string_view string_view;
                if (!value.get(string_view))
                {
                    result.emplace(std::move(key_str), std::string(string_view));
                    cacheValid = true;
                }
                break;
            }
            case simdjson::ondemand::json_type::array:
            case simdjson::ondemand::json_type::object: {
                // For objects and arrays, we'll use the raw JSON string
                std::string_view json_view;
                if (!value.raw_json().get(json_view))
                {
                    result.emplace(std::move(key_str), std::string(json_view));
                    cacheValid = true;
                }
                break;
            }
            case simdjson::ondemand::json_type::null:
                if (value.is_null())
                {
                    result.emplace(std::move(key_str), std::monostate());
                    cacheValid = true;
                }
                break;
            default:
                break;
            }

            if (cacheValid)
            {
                continue;
            }
        }

        // Get the type of the value and cache it
        auto type_result = value.type();
        if (type_result.error())
        {
            result.emplace(std::move(key_str), std::monostate());
            continue;
        }

        auto type = type_result.value();
        cache.keyTypes[key_str] = type;

        switch (type)
        {
        case simdjson::ondemand::json_type::boolean: {
            bool bool_value;
            if (!value.get(bool_value))
            {
                result.emplace(std::move(key_str), bool_value);
            }
            else
            {
                result.emplace(std::move(key_str), std::monostate());
            }
            break;
        }
        case simdjson::ondemand::json_type::number: {
            // Check the number type
            auto number_type = value.get_number_type();
            if (number_type.error())
            {
                result.emplace(std::move(key_str), std::monostate());
                break;
            }

            // Cache the number type
            cache.numberTypes[key_str] = number_type.value();

            switch (number_type.value())
            {
            case simdjson::ondemand::number_type::signed_integer: {
                int64_t int_value;
                if (!value.get(int_value))
                {
                    result.emplace(std::move(key_str), int_value);
                }
                else
                {
                    result.emplace(std::move(key_str), std::monostate());
                }
                break;
            }
            case simdjson::ondemand::number_type::unsigned_integer: {
                uint64_t uint_value;
                if (!value.get(uint_value))
                {
                    result.emplace(std::move(key_str), uint_value);
                }
                else
                {
                    result.emplace(std::move(key_str), std::monostate());
                }
                break;
            }
            case simdjson::ondemand::number_type::floating_point_number: {
                double double_value;
                if (!value.get(double_value))
                {
                    result.emplace(std::move(key_str), double_value);
                }
                else
                {
                    result.emplace(std::move(key_str), std::monostate());
                }
                break;
            }
            default:
                result.emplace(std::move(key_str), std::monostate());
                break;
            }
            break;
        }
        case simdjson::ondemand::json_type::string: {
            std::string_view string_view;
            if (!value.get(string_view))
            {
                result.emplace(std::move(key_str), std::string(string_view));
            }
            else
            {
                result.emplace(std::move(key_str), std::monostate());
            }
            break;
        }
        case simdjson::ondemand::json_type::array:
        case simdjson::ondemand::json_type::object: {
            // For objects and arrays, we'll use the raw JSON string
            std::string_view json_view;
            if (!value.raw_json().get(json_view))
            {
                result.emplace(std::move(key_str), std::string(json_view));
            }
            else
            {
                result.emplace(std::move(key_str), std::monostate());
            }
            break;
        }
        case simdjson::ondemand::json_type::null:
            result.emplace(std::move(key_str), std::monostate());
            break;
        default:
            result.emplace(std::move(key_str), std::monostate());
            break;
        }
    }

    return result;
}

} // namespace loglib
