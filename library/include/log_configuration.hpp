#pragma once

#include "log_data.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <string>
#include <vector>

namespace loglib
{

struct LogConfiguration
{
    enum class Type
    {
        Any,
        Time
    };

    struct Column
    {
        std::string header;
        std::vector<std::string> keys;
        std::string printFormat;
        Type type = Type::Any;
        std::vector<std::string> parseFormats;

        NLOHMANN_DEFINE_TYPE_INTRUSIVE(Column, header, keys, printFormat, type, parseFormats)
    };

    std::vector<Column> columns;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE(LogConfiguration, columns)
};

NLOHMANN_JSON_SERIALIZE_ENUM(
    LogConfiguration::Type, {{LogConfiguration::Type::Any, "any"}, {LogConfiguration::Type::Time, "time"}}
)

void UpdateConfiguration(LogConfiguration &configuration, const LogData &logData);
void SerializeConfiguration(const std::filesystem::path &path, const LogConfiguration &configuration);
LogConfiguration DeserializeConfiguration(const std::filesystem::path &path);

} // namespace loglib
