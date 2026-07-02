#pragma once

#include "loglib/regex_templates.hpp"

#include <cstdint>
#include <memory>
#include <vector>

namespace loglib::internal
{

/// Snapshot of the merged regex-template registry (built-ins,
/// then extras), each tier stable-sorted by `priority` ascending.
/// The same ordering the GUI picker uses, surfaced here so the
/// auto-detect probe in `parsers/regex_parser.cpp` can see
/// user-registered extras without reaching into
/// `regex_templates.cpp`.
///
/// Returned as a `shared_ptr` so the parser's compiled cache can
/// pin the snapshot for as long as it references the underlying
/// `RegexTemplate` storage. Pointer identity changes (in lockstep
/// with `TemplatesGeneration()`) whenever `SetExtraRegexTemplates`
/// swaps in a new extras set.
[[nodiscard]] std::shared_ptr<const std::vector<RegexTemplate>> MergedRegexTemplates();

/// Monotonic counter bumped on every `SetExtraRegexTemplates` call
/// (including the implicit initial build). Cheap enough for the
/// probe hot path — the parser keeps a `last_seen` value and only
/// re-acquires `MergedRegexTemplates()` when the counter advances,
/// so per-probe cost is one atomic load in the steady state.
[[nodiscard]] uint64_t TemplatesGeneration() noexcept;

} // namespace loglib::internal
