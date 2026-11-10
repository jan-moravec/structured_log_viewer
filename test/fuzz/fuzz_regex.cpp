// Fuzz target for the regex parser, fed through an in-memory line source. The pattern is a fixed built-in template with
// indented continuation lines, so the multi-line record code runs as well as the matcher. The input is the log text.

#include "fuzz_common.hpp"

#include "loglib/parse/parsers/regex_parser.hpp"
#include "loglib/parse/regex_templates.hpp"
#include "loglib/schema/source_descriptor.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string_view>

namespace
{

constexpr std::string_view TEMPLATE_NAME = "Java / log4j / SLF4J Logback";

const loglib::RegexTemplate &MultiLineTemplate()
{
    static const loglib::RegexTemplate FOUND = [] {
        for (const loglib::RegexTemplate &candidate : loglib::BuiltinRegexTemplates())
        {
            if (candidate.name == TEMPLATE_NAME && candidate.continuationMode == loglib::ContinuationMode::Indented)
            {
                return candidate;
            }
        }
        std::cerr << "built-in multi-line template not found\n";
        std::abort();
    }();
    return FOUND;
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size)
{
    // Built once: compiling the pattern for every input would cost more than most parses.
    static const loglib::RegexParser PARSER = [] {
        const loglib::RegexTemplate &tmpl = MultiLineTemplate();
        return loglib::RegexParser(tmpl.pattern, tmpl.continuationMode, tmpl.headerAnchor);
    }();
    loglib::fuzz::ParseBytes(PARSER, data, size);
    return 0;
}
