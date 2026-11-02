#pragma once

#include <glaze/glaze.hpp>

#include <cstdint>

namespace loglib::internal
{

/// Shared glaze options for `LogConfiguration` read/write.
///
/// Pinned behaviours:
/// 1. `prettify = true` + 4-space indent so session JSON is
///    human-inspectable (the recents store is a debugging surface).
/// 2. `error_on_unknown_keys = false` so legacy fields (e.g. the
///    pre-`locators` `"locator"` shape) and future fields added by
///    newer builds load instead of throwing -- backing the
///    "rebind failure is non-fatal" promise on `Source`.
struct LogConfigOpts : glz::opts
{
    uint8_t indentation_width = 4; // NOLINT(readability-identifier-naming)
};
// MSVC enforces member-declaration order in designated init.
constexpr LogConfigOpts LOG_CONFIG_OPTS{{.error_on_unknown_keys = false, .prettify = true}};

} // namespace loglib::internal
