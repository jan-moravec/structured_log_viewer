// Fuzz target for the timestamp parsers. The first byte picks the parser. For a format of its own the text up to the
// first newline is the format and the rest is the timestamp. For the others the rest is the timestamp.
//
// A format that TimestampFormat compiles into steps must give exactly what date::parse gives, so for those formats the
// target parses with both and aborts on any difference. Formats with digits are left out of that check, because the
// general parser refuses some of them to stay safe while the steps read them as plain characters.

#include "fuzz_common.hpp"

#include "loglib/core/log_value.hpp"
#include "loglib/time/timestamp_parsing.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
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
        const loglib::TimestampFormat prepared(format);
        const bool parsed = prepared.Parse(text, scratch, REFERENCE_TIME, out);

        loglib::TimeStamp reference;
        const bool referenceParsed = loglib::TryParseGenericTimestamp(text, format, scratch, reference);
        const bool comparable = prepared.Kind() == loglib::TimestampFormatKind::Compiled &&
                                format.find_first_of("0123456789") == std::string::npos;
        if (comparable && (parsed != referenceParsed || (parsed && out != reference)))
        {
            std::cerr << "compiled format disagrees with date::parse: format '" << format << "', text '" << text
                      << "', compiled " << parsed << ' ' << out.time_since_epoch().count() << ", date::parse "
                      << referenceParsed << ' ' << reference.time_since_epoch().count() << '\n';
            std::abort();
        }
        break;
    }
    }
    return 0;
}
