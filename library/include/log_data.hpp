#pragma once

#include "log_line.hpp"

#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace loglib
{

class LogData
{
public:
    LogData(std::vector<std::unique_ptr<LogLine>> lines = {}, std::vector<std::string> keys = {});

    const std::vector<std::unique_ptr<LogLine>> &GetLines() const;
    const std::vector<std::string> &GetKeys() const;
    const std::unordered_map<size_t, std::pair<TimeStamp, TimeStamp>> &GetColumnMinMaxTimeStamps() const;

    void Merge(LogData &&other);
    void UpdateColumnMinMaxTimestamp(size_t column, TimeStamp timestamp);

private:
    std::vector<std::unique_ptr<LogLine>> mLines;
    std::vector<std::string> mKeys;
    std::unordered_map<size_t, std::pair<TimeStamp, TimeStamp>> mColumnMinMaxTimeStamp;
};

} // namespace loglib
