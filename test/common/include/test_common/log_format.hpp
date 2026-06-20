#pragma once

#include <test_common/log_record.hpp>

#include <functional>
#include <string>
#include <string_view>

namespace test_common
{

// A pluggable serializer that turns `LogRecord`s into a format's native
// on-disk text. The same RNG-driven record sequence can be fed through any
// `LogFormat`, keeping benchmark numbers across formats directly comparable.
//
// `writeHeader` is emitted once before the records (empty for self-describing
// formats such as JSON lines / logfmt; populated by schema-bearing formats
// like CSV). `writeLine` serializes one record to a single output line
// (without the trailing newline).
struct LogFormat
{
    std::string_view suggestedExtension;
    std::function<std::string(const RecordSchema &)> writeHeader;
    std::function<std::string(const LogRecord &)> writeLine;
};

// JSON Lines: one compact JSON object per line.
LogFormat JsonLines();

// logfmt (Heroku / `kr/logfmt` flavour): whitespace-separated `key=value`
// pairs. Fields are emitted in the record's lexicographic key order; values
// are quoted only when they are not bare-safe. Nested arrays / objects are
// serialized as compact JSON inside a single quoted logfmt string so the
// field count stays identical across formats.
LogFormat Logfmt();

} // namespace test_common
