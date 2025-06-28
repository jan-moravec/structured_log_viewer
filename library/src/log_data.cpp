#include "loglib/log_data.hpp"

#include <iterator>
#include <set>
#include <unordered_set>

namespace loglib
{

LogData::LogData(std::unique_ptr<LogFile> file, std::vector<LogLine> lines, std::vector<std::string> keys)
    : mLines(std::move(lines)), mKeys(std::move(keys))
{
    mFiles.push_back(std::move(file));
}

const std::vector<std::unique_ptr<LogFile>> &LogData::Files() const
{
    return mFiles;
}

std::vector<std::unique_ptr<LogFile>> &LogData::Files()
{
    return mFiles;
}

const std::vector<LogLine> &LogData::Lines() const
{
    return mLines;
}

std::vector<LogLine> &LogData::Lines()
{
    return mLines;
}

const std::vector<std::string> &LogData::Keys() const
{
    return mKeys;
}

std::vector<std::string> &LogData::Keys()
{
    return mKeys;
}

void LogData::Merge(LogData &&other)
{
    mFiles.reserve(mFiles.size() + other.mFiles.size());
    std::move(
        std::make_move_iterator(other.mFiles.begin()),
        std::make_move_iterator(other.mFiles.end()),
        std::back_inserter(mFiles)
    );

    mLines.reserve(mLines.size() + other.mLines.size());
    std::move(
        std::make_move_iterator(other.mLines.begin()),
        std::make_move_iterator(other.mLines.end()),
        std::back_inserter(mLines)
    );

    std::unordered_set<std::string> existingKeys(mKeys.begin(), mKeys.end());
    for (auto &&key : other.mKeys)
    {
        if (existingKeys.find(key) == existingKeys.end())
        {
            mKeys.push_back(std::move(key));
        }
    }
}

} // namespace loglib
