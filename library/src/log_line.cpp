#include "log_line.hpp"

namespace loglib
{

LogValue LogLine::GetValue(const std::string &key) const
{
    if (key.empty())
    {
        return std::monostate();
    }

    const auto extraValueInterator = mExtraValues.find(key);
    if (extraValueInterator != mExtraValues.end())
    {
        return extraValueInterator->second;
    }

    return GetRawValue(key);
}

void LogLine::SetExtraValue(const std::string &key, const LogValue &value)
{
    mExtraValues[key] = value;
}

} // namespace loglib
