#pragma once

#include <chrono>
#include <string>
#include <unordered_map>
#include <variant>

namespace loglib
{

using TimeStamp = std::chrono::time_point<std::chrono::system_clock, std::chrono::microseconds>;
using LogValue = std::variant<std::string, int64_t, double, bool, TimeStamp, std::monostate>;

class LogLine
{
public:
    virtual ~LogLine() = default;

    virtual LogValue GetRawValue(const std::string &key) const = 0;
    virtual std::string GetLine() const = 0;

    virtual LogValue GetValue(const std::string &key) const;
    virtual void SetExtraValue(const std::string &key, const LogValue &value);

private:
    std::unordered_map<std::string, LogValue> mExtraValues;
};

} // namespace loglib
