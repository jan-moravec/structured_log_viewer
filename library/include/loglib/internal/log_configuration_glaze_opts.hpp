#pragma once

#include <glaze/glaze.hpp>

#include <cstdint>

namespace loglib::internal
{

/// Shared glaze options for `LogConfiguration` Read/Write.
///
/// Two behaviours are pinned here that the implicit defaults get wrong
/// for our wire format:
///
/// 1. `prettify = true` + 4-space `indentation_width`: every persisted
///    session JSON is human-inspectable on disk (the recents store is
///    a debugging surface, not just a serialisation target).
///
/// 2. `error_on_unknown_keys = false`: critical for back-compat. Glaze
///    7.x defaults to *erroring* on unknown keys, which would make any
///    pre-`locators` session JSON (still containing `"locator":"..."`)
///    fail to parse and lose its columns / filters / sort along with
///    the source binding. The header on `LogConfiguration::Source`
///    promises "rebind failure is non-fatal (columns and filters
///    still apply)"; this option makes that promise hold for *both*
///    backwards-compat (legacy `locator`) and forwards-compat (a
///    future field added by a newer build that we want older builds
///    to ignore rather than reject wholesale).
struct LogConfigOpts : glz::opts
{
    uint8_t indentation_width = 4; // NOLINT(readability-identifier-naming)
};
// Designated-init in member-declaration order
// (`error_on_unknown_keys` then `prettify`); MSVC enforces that
// order even for designated initialisers.
constexpr LogConfigOpts LOG_CONFIG_OPTS{
    {.error_on_unknown_keys = false, .prettify = true}
};

} // namespace loglib::internal
