#pragma once

#include <glaze/glaze.hpp>

#include <string>
#include <vector>

namespace test_common
{

// Canonical, format-agnostic log record shared by every generator and
// `LogFormat` serializer. Backed by Glaze's ordered generic JSON value,
// whose object map is lexicographically sorted by key (`generic_sorted_map`),
// so all formats iterate the same fields in the same deterministic order.
// Nested values (arrays / objects) are supported for the wide-row generator.
using LogRecord = glz::generic_sorted_u64;

// Column order for schema-bearing formats (CSV, regex templates). Empty for
// self-describing formats (JSON lines, logfmt).
using RecordSchema = std::vector<std::string>;

} // namespace test_common
