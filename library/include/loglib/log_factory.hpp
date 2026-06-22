#pragma once

#include "log_parser.hpp"

#include <memory>

namespace loglib
{

class LogFactory
{
public:
    // The numeric value of each `Parser` is persisted on disk (via
    // `LogConfiguration::Source::Format`) and is also the auto-detect
    // probe order in `loglib::ParseFile(path)` and
    // `MainWindow::DetectFormatForPath`. Append before `Count`; never
    // reorder existing values.
    enum class Parser
    {
        Json,
        Logfmt,
        Csv,
        Count
    };

    static std::unique_ptr<LogParser> Create(Parser parser);
};

} // namespace loglib
