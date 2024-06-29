#pragma once

#include <nlohmann/json_fwd.hpp>
//#include <date/date.h>

#include <memory>
#include <chrono>
#include <string>

class LogMessage
{
public:
    LogMessage();
    ~LogMessage();

    long long timestamp = 0;
    std::string severity;
    std::string message;

private:
    //std::unique_ptr<nlohmann::json> mJson;
};

// Comparator for sorting LogMessages by timestamp
struct LogMessageComparator {
    bool operator()(const LogMessage &lhs, const LogMessage &rhs) const {
        return lhs.timestamp < rhs.timestamp;
    }
};
