#pragma once

#include <QDebug>

namespace logapp
{

/// Consistent prefix for every diagnostic emitted by the recents /
/// single-instance / session-restore subsystems. Field-support
/// triage greps the application's log output for "[StructLog]" to
/// reconstruct what happened on a user's machine; per-subsystem
/// prefixes drift across reviews, so we centralise the marker
/// here. Use via the `logapp::LogWarning()` helper below.
constexpr const char *LOG_PREFIX = "[StructLog]";

/// `qWarning` wrapper that emits the shared `[StructLog]` prefix on
/// every fail-closed branch. The helper returns the `QDebug` rvalue
/// chained against `LOG_PREFIX`; callers continue with the usual
/// `<<` streaming.
///
/// Pre-fix this was a `LOGAPP_WARN()` macro of the same shape. The
/// switch to an `inline` function removes the macro footgun (a
/// stray semicolon after a macro expansion is a different beast
/// than after a function call), gives `qSetMessagePattern` a real
/// function name to attribute against, and lets the helper be
/// referenced unqualified from inside the `logapp` namespace.
///
/// Examples:
///   logapp::LogWarning() << "AcquireRecentsLock: timeout after" << timeoutMs << "ms";
///   logapp::LogWarning() << "SingleInstanceGuard: forward write failed:" << socket.errorString();
inline QDebug LogWarning()
{
    return qWarning() << LOG_PREFIX;
}

} // namespace logapp
