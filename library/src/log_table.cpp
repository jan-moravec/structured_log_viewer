#include "loglib/log_table.hpp"

#include "loglib/log_processing.hpp"

namespace loglib
{

LogTable::LogTable(LogData data, LogConfigurationManager configuration)
    : mData(std::move(data)), mConfiguration(std::move(configuration))
{
    RefreshColumnKeyIds();
}

void LogTable::Update(LogData &&data)
{
    Configuration().Update(data);
    if (!data.TimestampsAlreadyParsed())
    {
        ParseTimestamps(data, Configuration().Configuration());
    }
    Data().Merge(std::move(data));
    RefreshColumnKeyIds();
}

std::string LogTable::GetHeader(size_t column) const
{
    return mConfiguration.Configuration().columns[column].header;
}

size_t LogTable::ColumnCount() const
{
    return mConfiguration.Configuration().columns.size();
}

LogValue LogTable::GetValue(size_t row, size_t column) const
{
    if (column >= mColumnKeyIds.size())
    {
        return std::monostate{};
    }
    const auto &line = mData.Lines()[row];
    for (const KeyId id : mColumnKeyIds[column])
    {
        if (id == kInvalidKeyId)
        {
            continue;
        }
        LogValue value = line.GetValue(id);
        if (!std::holds_alternative<std::monostate>(value))
        {
            return value;
        }
    }
    return std::monostate{};
}

std::string LogTable::GetFormattedValue(size_t row, size_t column) const
{
    if (column >= mColumnKeyIds.size())
    {
        return "";
    }
    const auto &line = mData.Lines()[row];
    for (const KeyId id : mColumnKeyIds[column])
    {
        if (id == kInvalidKeyId)
        {
            continue;
        }
        LogValue value = line.GetValue(id);
        if (!std::holds_alternative<std::monostate>(value))
        {
            return FormatLogValue(mConfiguration.Configuration().columns.at(column).printFormat, value);
        }
    }
    return "";
}

size_t LogTable::RowCount() const
{
    return mData.Lines().size();
}

const LogData &LogTable::Data() const
{
    return mData;
}

LogData &LogTable::Data()
{
    return mData;
}

const LogConfigurationManager &LogTable::Configuration() const
{
    return mConfiguration;
}

LogConfigurationManager &LogTable::Configuration()
{
    return mConfiguration;
}

void LogTable::RefreshColumnKeyIds()
{
    const auto &columns = mConfiguration.Configuration().columns;
    mColumnKeyIds.clear();
    mColumnKeyIds.reserve(columns.size());
    for (const auto &column : columns)
    {
        std::vector<KeyId> ids;
        ids.reserve(column.keys.size());
        for (const auto &key : column.keys)
        {
            ids.push_back(mData.Keys().Find(key));
        }
        mColumnKeyIds.push_back(std::move(ids));
    }
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
            else if constexpr (std::is_same_v<T, std::string_view>)
            {
                return std::string(arg);
            }
            else if constexpr (std::is_same_v<T, int64_t>)
            {
                return fmt::vformat(format, fmt::make_format_args(arg));
            }
            else if constexpr (std::is_same_v<T, uint64_t>)
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
                const date::zoned_time local_time{CurrentZone(), std::chrono::round<std::chrono::milliseconds>(arg)};
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
