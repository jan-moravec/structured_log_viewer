#include "log_table.hpp"

namespace loglib
{

LogTable::LogTable(const LogData &data, LogConfiguration configuration)
    : mData(data), mConfiguration(std::move(configuration))
{
}

std::string LogTable::GetHeader(size_t column) const
{
    return mConfiguration.columns[column].header;
}

size_t LogTable::ColumnCount() const
{
    return mConfiguration.columns.size();
}

LogValue LogTable::GetValue(size_t row, size_t column) const
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

std::string LogTable::GetFormattedValue(size_t row, size_t column) const
{
    for (const auto &key : mConfiguration.columns.at(column).keys)
    {
        LogValue value = mData.GetLines()[row]->GetValue(key);
        if (!std::holds_alternative<std::monostate>(value))
        {
            return FormatLogValue(mConfiguration.columns.at(column).printFormat, value);
        }
    }

    return "";
}

size_t LogTable::RowCount() const
{
    return mData.GetLines().size();
}

const LogConfiguration &LogTable::Configuration() const
{
    return mConfiguration;
}

std::string LogTable::FormatLogValue(const std::string &format, const LogValue &value)
{
    return std::visit(
        [&format](const auto &arg) -> std::string {
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
                static_assert(std::is_same_v<T, void>, "non-exhaustive visitor!");
            }
        },
        value
    );
}

} // namespace loglib
