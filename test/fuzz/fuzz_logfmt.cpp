// Fuzz target for the logfmt parser, fed through an in-memory line source. Both the multi-line and the single-line mode
// run on every input.

#include "fuzz_common.hpp"

#include "loglib/parse/parsers/logfmt_parser.hpp"

#include <cstddef>
#include <cstdint>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size)
{
    const loglib::LogfmtParser multiline(true);
    loglib::fuzz::ParseBytes(multiline, data, size);

    const loglib::LogfmtParser singleLine(false);
    loglib::fuzz::ParseBytes(singleLine, data, size);
    return 0;
}
