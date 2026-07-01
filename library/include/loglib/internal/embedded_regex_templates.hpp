#pragma once

#include <span>
#include <string_view>

namespace loglib::internal
{

/// One built-in regex template, as embedded into the library at
/// build time. `source` is a short human-readable identifier (the
/// JSON file's basename, used only in diagnostics if the embedded
/// bytes fail to parse); `json` is the verbatim file contents.
///
/// The data lives in a CMake-generated translation unit
/// (`library/src/regex_templates_embedded.cpp`) emitted from
/// `library/data/regex_templates/*.json` at configure time. The
/// generator is intentionally trivial (one `EmbeddedRegexTemplate`
/// per file, listed in alphabetical-by-slug order) so adding /
/// removing a built-in template only touches the data directory
/// and the build glue; the parse and probe paths in
/// `regex_templates.cpp` and `parsers/regex_parser.cpp` stay
/// untouched.
struct EmbeddedRegexTemplate
{
    std::string_view source;
    std::string_view json;
};

/// Returns the embedded built-in catalog. Defined in the
/// CMake-generated `regex_templates_embedded.cpp`.
[[nodiscard]] std::span<const EmbeddedRegexTemplate> EmbeddedBuiltinRegexTemplates() noexcept;

} // namespace loglib::internal
