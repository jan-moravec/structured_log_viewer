#pragma once

#include "log_configuration.hpp"
#include "log_data.hpp"

#include <date/date.h>
#include <date/tz.h>
#include <fmt/format.h>

#include <string>

namespace loglib
{

class LogTable
{
public:
    LogTable() = default;
    LogTable(LogData data, LogConfigurationManager configuration);

    // Deleted copy constructor and copy assignment operator for efficiency.
    LogTable(LogTable &) = delete;
    LogTable &operator=(LogTable &) = delete;

    // Defaulted move constructor and move assignment operator.
    LogTable(LogTable &&) = default;
    LogTable &operator=(LogTable &&) = default;

    void Update(LogData &&data);

    std::string GetHeader(size_t column) const;
    size_t ColumnCount() const;
    LogValue GetValue(size_t row, size_t column) const;
    std::string GetFormattedValue(size_t row, size_t column) const;
    size_t RowCount() const;

    const LogData &Data() const;
    LogData &Data();

    const LogConfigurationManager &Configuration() const;
    LogConfigurationManager &Configuration();

private:
    static std::string FormatLogValue(const std::string &format, const LogValue &value);

    LogData mData;
    LogConfigurationManager mConfiguration;
};

} // namespace loglib
