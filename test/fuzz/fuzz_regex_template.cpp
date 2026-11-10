// Fuzz target for the regex template file reader. A template that reads is also written back, and its patterns are
// compiled and run against its sample lines, because a user can save any text as a template.

#include "fuzz_common.hpp"

#include "loglib/parse/parsers/regex_parser.hpp"
#include "loglib/parse/regex_templates.hpp"

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size)
{
    try
    {
        const loglib::RegexTemplate tmpl = loglib::ParseRegexTemplate(loglib::fuzz::AsText(data, size));

        std::string error;
        (void)loglib::ValidateRegexPattern(tmpl.pattern, error);
        (void)loglib::ValidateHeaderAnchor(tmpl.headerAnchor, error);
        for (const std::string &line : tmpl.sampleLines)
        {
            (void)loglib::PatternMatchesLine(tmpl.pattern, line);
        }

        // The writer must accept everything the reader produced.
        const std::string written = loglib::SerializeRegexTemplate(tmpl);
        (void)loglib::ParseRegexTemplate(written);
    }
    catch (const std::runtime_error &) // NOLINT(bugprone-empty-catch)
    {
        // The documented failure of the reader and the writer.
    }
    return 0;
}
