#pragma once

#include <QDebug>

namespace logapp
{

/// Consistent prefix for every diagnostic emitted by the recents /
/// single-instance / session-restore subsystems. Field-support
/// triage greps the application's log output for "[StructLog]" to
/// reconstruct what happened on a user's machine; per-subsystem
/// prefixes drift across reviews, so we centralise the marker
/// here. Use via the `LOGAPP_WARN()` macro.
constexpr const char *LOG_PREFIX = "[StructLog]";

} // namespace logapp

/// `qWarning` wrapper that emits the shared `[StructLog]` prefix on
/// every fail-closed branch. Macro (not a function) so the streamed
/// arguments retain their full `<<` chain semantics and so the
/// emission site shows up under the macro expansion in
/// `qSetMessagePattern`-driven log routing.
///
/// Examples:
///   LOGAPP_WARN() << "AcquireRecentsLock: timeout after" << timeoutMs << "ms";
///   LOGAPP_WARN() << "SingleInstanceGuard: forward write failed:" << socket.errorString();
#define LOGAPP_WARN() (qWarning() << ::logapp::LOG_PREFIX) // NOLINT(cppcoreguidelines-macro-usage)
