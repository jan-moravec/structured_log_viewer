// Fuzz target for the timestamp parsers. The first byte picks the parser. For the general parser the text up to the
// first newline is the format and the rest is the timestamp. For the others the rest is the timestamp.

#include "fuzz_common.hpp"

#include "loglib/core/log_value.hpp"
#include "loglib/time/timestamp_parsing.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size)
{
    if (size == 0)
    {
        return 0;
    }

    using namespace std::chrono;
    static const system_clock::time_point REFERENCE_TIME{sys_days{year{2024} / June / 15}};
    static loglib::TimestampParseScratch scratch;

    const std::uint8_t selector = data[0];
    const std::string_view rest = loglib::fuzz::AsText(data + 1, size - 1);
    loglib::TimeStamp out;

    switch (selector % 6U)
    {
    case 0:
        (void)loglib::TryParseIsoTimestamp(rest, 'T', out);
        break;
    case 1:
        (void)loglib::TryParseIsoTimestamp(rest, ' ', out);
        break;
    case 2:
        (void)loglib::TryParseIsoOffsetTimestamp(rest, 'T', out);
        break;
    case 3:
        (void)loglib::TryParseIsoOffsetTimestamp(rest, ' ', out);
        break;
    case 4:
        (void)loglib::TryParseSyslogRfc3164Timestamp(rest, out, REFERENCE_TIME);
        break;
    default:
    {
        const std::size_t newline = rest.find('\n');
        const std::string format{rest.substr(0, newline)};
        const std::string_view text = newline == std::string_view::npos ? std::string_view{} : rest.substr(newline + 1);
        const loglib::TimestampFormatKind kind = loglib::ClassifyTimestampFormat(format);
        (void)loglib::TryParseTimestamp(text, format, kind, scratch, REFERENCE_TIME, out);
        (void)loglib::TryParseGenericTimestamp(text, format, scratch, out);
        break;
    }
    }
    return 0;
}
