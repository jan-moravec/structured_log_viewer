#pragma once

#include "log_data.hpp"

#include <string>
#include <vector>
#include <unordered_map>

namespace loglib
{

struct LogConfiguration
{
    enum class Type {
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
    };

    std::unordered_map<size_t, Column> columns;
};

void UpdateConfiguration(LogConfiguration &configuration, const LogData &logData);

}
