#pragma once

#include "loglib/regex_templates.hpp"

#include <cstdint>
#include <memory>
#include <vector>

namespace loglib::internal
{

/// Snapshot of the merged regex-template registry (built-ins
/// first, then extras), each tier stable-sorted by `priority`
/// ascending. The same ordering rule the GUI picker uses, surfaced
/// here so the auto-detect probe in `parsers/regex_parser.cpp` can
/// see user-registered extras without poking at the
/// `regex_templates.cpp` internals directly.
///
/// Returned as a `shared_ptr` so the parser's compiled cache can
/// pin the snapshot for as long as it holds references to the
/// underlying `RegexTemplate` storage. Pointer identity changes
/// (in lockstep with `TemplatesGeneration()`) whenever
/// `SetExtraRegexTemplates` swaps in a new extras set.
[[nodiscard]] std::shared_ptr<const std::vector<RegexTemplate>> MergedRegexTemplates();

/// Monotonic counter bumped on every `SetExtraRegexTemplates` call
/// (including the implicit initial build). Cheap to read on the
/// probe hot path — the parser keeps a `last_seen` value and only
/// re-acquires `MergedRegexTemplates()` when the counter advances,
/// so the per-probe cost is one atomic load when the registry is
/// stable.
[[nodiscard]] uint64_t TemplatesGeneration() noexcept;

} // namespace loglib::internal
