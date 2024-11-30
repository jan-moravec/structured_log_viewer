#pragma once

#include "log_configuration.hpp"
#include "log_line.hpp"

#include <date/date.h>
#include <date/tz.h>

namespace loglib
{

void Initialize();
std::string ParseTimestamps(LogData &logData, const LogConfiguration &configuration);
int64_t TimeStampToLocalMillisecondsSinceEpoch(TimeStamp timeStamp);
int64_t UtcMicrosecondsToLocalMilliseconds(int64_t microseconds);
TimeStamp LocalMillisecondsSinceEpochToTimeStamp(int64_t milliseconds);
std::string UtcMicrosecondsToDateTimeString(int64_t microseconds);

} // namespace loglib
