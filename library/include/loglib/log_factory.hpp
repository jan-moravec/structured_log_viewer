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
    //
    // `Regex` is intentionally last: it probes every built-in regex
    // template against the file and is the slowest to refuse, so we
    // only run it once the other three have rejected the file. That
    // way the existing JSON / logfmt / CSV open path is unchanged.
    enum class Parser
    {
        Json,
        Logfmt,
        Csv,
        Regex,
        Count
    };

    /// Returns a no-arg-constructed parser. For `Regex` this means
    /// "auto-detect only": `IsValid` works (it probes the built-in
    /// template registry), but `ParseStreaming` will refuse to run
    /// until a pattern is supplied via `ParserOptions::configuration`
    /// or `RegexParser`'s explicit-pattern constructor. See
    /// `loglib::ParseFile(path)` for the auto-detection wiring.
    static std::unique_ptr<LogParser> Create(Parser parser);
};

} // namespace loglib
