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
    LogTable(const LogData &data, LogConfiguration configuration);

    std::string GetHeader(size_t column) const;
    size_t ColumnCount() const;
    LogValue GetValue(size_t row, size_t column) const;
    std::string GetFormattedValue(size_t row, size_t column) const;
    size_t RowCount() const;
    const LogConfiguration &Configuration() const;

private:
    static std::string FormatLogValue(const std::string &format, const LogValue &value);

    const LogData &mData;
    const LogConfiguration mConfiguration;
};

} // namespace loglib
