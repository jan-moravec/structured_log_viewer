#include "log_data.hpp"

#include <iterator>
#include <set>

namespace loglib
{

LogData::LogData(std::vector<std::unique_ptr<LogLine>> lines, std::vector<std::string> keys)
    : mLines(std::move(lines)), mKeys(std::move(keys))
{
}

const std::vector<std::unique_ptr<LogLine>> &LogData::GetLines() const
{
    return mLines;
}

const std::vector<std::string> &LogData::GetKeys() const
{
    return mKeys;
}

const std::unordered_map<size_t, std::pair<TimeStamp, TimeStamp>> &LogData::GetColumnMinMaxTimeStamps() const
{
    return mColumnMinMaxTimeStamp;
}

void LogData::Merge(LogData &&other)
{
    mLines.reserve(mLines.size() + other.mLines.size());
    std::move(
        std::make_move_iterator(other.mLines.begin()),
        std::make_move_iterator(other.mLines.end()),
        std::back_inserter(mLines)
    );

    std::set<std::string> set(mKeys.begin(), mKeys.end());
    for (auto &&otherKey : other.mKeys)
    {
        if (set.insert(otherKey).second)
        {
            mKeys.push_back(std::move(otherKey));
        }
    }

    for (const auto &columnMinMaxTimeStamp: other.mColumnMinMaxTimeStamp)
    {
        UpdateColumnMinMaxTimestamp(columnMinMaxTimeStamp.first, columnMinMaxTimeStamp.second.first);
        UpdateColumnMinMaxTimestamp(columnMinMaxTimeStamp.first, columnMinMaxTimeStamp.second.second);
    }
}

void LogData::UpdateColumnMinMaxTimestamp(size_t column, TimeStamp timestamp)
{
    auto iterator = mColumnMinMaxTimeStamp.find(column);
    if (iterator != mColumnMinMaxTimeStamp.end())
    {
        if (timestamp < iterator->second.first)
        {
            iterator->second.first = timestamp;
        }
        else if (timestamp > iterator->second.second)
        {
            iterator->second.second = timestamp;
        }
    }
    else
    {
        mColumnMinMaxTimeStamp.emplace(column, std::make_pair(timestamp, timestamp));
    }
}

} // namespace loglib
