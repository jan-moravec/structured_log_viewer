#pragma once

// Glaze names a local lambda `emit`; Qt's `emit` macro would rewrite it in
// translation units that include Qt headers first.
#pragma push_macro("emit")
#undef emit
#include <glaze/glaze.hpp>
#pragma pop_macro("emit")

#include <string>
#include <vector>

namespace test_common
{

// Canonical, format-agnostic log record. Backed by Glaze's ordered generic
// JSON value, so keys iterate in deterministic lexicographic order across
// every format. Nested arrays/objects are supported (used by the wide-row
// generator).
using LogRecord = glz::generic_sorted_u64;

// Column order for schema-bearing formats (CSV, regex templates). Empty
// for self-describing formats (JSON lines, logfmt).
using RecordSchema = std::vector<std::string>;

} // namespace test_common
