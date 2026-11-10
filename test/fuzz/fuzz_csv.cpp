// Fuzz target for the CSV parser, fed through an in-memory line source.

#include "fuzz_common.hpp"

#include "loglib/parse/parsers/csv_parser.hpp"

#include <cstddef>
#include <cstdint>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size)
{
    const loglib::CsvParser parser;
    loglib::fuzz::ParseBytes(parser, data, size);
    return 0;
}
