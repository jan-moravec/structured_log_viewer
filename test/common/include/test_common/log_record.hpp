#pragma once

#include <glaze/glaze.hpp>

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
