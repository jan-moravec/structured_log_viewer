// Fuzz target for the JSON lines parser, fed through an in-memory line source.

#include "fuzz_common.hpp"

#include "loglib/parse/parsers/json_parser.hpp"

#include <cstddef>
#include <cstdint>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size)
{
    const loglib::JsonParser parser;
    loglib::fuzz::ParseBytes(parser, data, size);
    return 0;
}
