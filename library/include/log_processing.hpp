#pragma once

#include "log_configuration.hpp"
#include "log_line.hpp"

#include <date/date.h>
#include <date/tz.h>
#include <sstream>

namespace loglib
{

void Initialize();
std::string ParseTimestamps(LogData &logData, const LogConfiguration &configuration);

} // namespace loglib
