#include "loglib/log_line.hpp"

namespace loglib
{
LogLine::LogLine(LogMap values, LogFileReference fileReference)
    : mValues(std::move(values)), mFileReference(std::move(fileReference))
{
}

LogValue LogLine::GetValue(const std::string &key) const
{
    if (key.empty())
    {
        return std::monostate();
    }

    const auto iterator = mValues.find(key);
    if (iterator != mValues.end())
    {
        return iterator->second;
    }

    return std::monostate();
}

void LogLine::SetValue(const std::string &key, LogValue value)
{
    mValues[key] = value;
}

std::vector<std::string> LogLine::GetKeys() const
{
    std::vector<std::string> keys;
    keys.reserve(mValues.size());
    for (const auto &pair : mValues)
    {
        keys.push_back(pair.first);
    }
    return keys;
}

const LogFileReference &LogLine::FileReference() const
{
    return mFileReference;
}

const LogMap &LogLine::Values() const
{
    return mValues;
}

LogMap &LogLine::Values()
{
    return mValues;
}

} // namespace loglib
