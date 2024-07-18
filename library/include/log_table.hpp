#pragma once

#include "log_data.hpp"
#include "log_configuration.hpp"

#include <fmt/format.h>
#include <date/date.h>
#include <date/tz.h>

#include <string>

namespace loglib
{

class LogTable
{
public:
    LogTable(const LogData &data, LogConfiguration configuration): mData(data), mConfiguration(std::move(configuration))
    {}

    std::string GetHeader(size_t column) const
    {
        return mConfiguration.columns[column].header;
    }

    size_t ColumnCount() const
    {
        return mConfiguration.columns.size();
    }

    LogValue GetValue(size_t row, size_t column) const
    {
        for (const std::string &key : mConfiguration.columns.at(column).keys)
        {
            LogValue value = mData.GetLines()[row]->GetRawValue(key);
            if (!std::holds_alternative<std::monostate>(value))
            {
                return value;
            }
        }

        return std::monostate();
    }

    std::string GetFormattedValue(size_t row, size_t column) const
    {
        for (const auto &key : mConfiguration.columns.at(column).keys)
        {
            LogValue value = mData.GetLines()[row]->GetValue(key);
            if (!std::holds_alternative<std::monostate>(value))
            {
                return formatLogValue(mConfiguration.columns.at(column).printFormat, value);
            }
        }

        return "";
    }

    size_t RowCount() const
    {
        return mData.GetLines().size();
    }

private:
    static std::string formatLogValue(const std::string &format, const LogValue& value) {
        return std::visit(
            [&format](const auto &arg)
            {
                using T = std::decay_t<decltype(arg)>;
                if constexpr (std::is_same_v<T, std::string>)
                {
                    return arg;
                }
                else if constexpr (std::is_same_v<T, int64_t>)
                {
                    return fmt::vformat(format, fmt::make_format_args(arg));
                }
                else if constexpr (std::is_same_v<T, double>)
                {
                    return fmt::vformat(format, fmt::make_format_args(arg));
                }
                else if constexpr (std::is_same_v<T, bool>)
                {
                    return fmt::vformat(format, fmt::make_format_args(arg));
                }
                else if constexpr (std::is_same_v<T, TimeStamp>)
                {
                    static auto tz = date::current_zone();
                    date::zoned_time local_time{tz, std::chrono::round<std::chrono::milliseconds>(arg)};
                    return date::format(format, local_time);
                }
                else if constexpr (std::is_same_v<T, std::monostate>)
                {
                    return std::string();
                }
                else
                {
                    static_assert(false, "non-exhaustive visitor!");
                }
            },
            value);
    }

    const LogData &mData;
    const LogConfiguration mConfiguration;
};

}
