#pragma once

#include "log_parser.hpp"

#include <memory>

namespace loglib
{

class LogFactory
{
public:
    // The numeric value of each `Parser` enumerator is persisted on
    // disk (via `LogConfiguration::Source::Format` round-trip) and
    // is also the dispatch order used by `loglib::ParseFile(path)`
    // and `MainWindow::DetectFormatForPath` when probing parsers.
    // Append new values **before** `Count`, never reorder existing
    // ones.
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
