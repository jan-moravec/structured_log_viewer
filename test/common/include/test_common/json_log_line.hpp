#pragma once

#include <glaze/glaze.hpp>

#include <string>
#include <variant>
#include <vector>

namespace test_common
{

// Lightweight JSON-line abstraction shared between the Catch2-based tests/
// benchmarks and the standalone `log_generator` console app. Mirrors what
// used to live as `TestJsonLogFile::Line` inside `test/lib/include/common.hpp`
// but with no Catch2 dependency so binaries that don't link the test framework
// can still consume it.
class JsonLogLine
{
public:
    using Type = std::variant<std::string, glz::generic_sorted_u64>;

    JsonLogLine(const char *line);
    JsonLogLine(glz::generic_sorted_u64 json);

    Type data;

    std::string ToString() const;
    void Parse(std::vector<std::string> &strings, std::vector<glz::generic_sorted_u64> &jsons) const;
};

} // namespace test_common
