#pragma once

#include <QDebug>

namespace logapp
{

/// Shared prefix on every diagnostic emitted by the recents /
/// single-instance / session-restore subsystems. Field-support
/// triage greps for "[StructLog]" in the application's log output.
constexpr const char *LOG_PREFIX = "[StructLog]";

/// `qWarning` wrapper that emits the shared `[StructLog]` prefix.
/// Stream additional context with `<<` as usual.
///
/// Example:
///   logapp::LogWarning() << "SingleInstanceGuard: forward write failed:" << socket.errorString();
inline QDebug LogWarning()
{
    return qWarning() << LOG_PREFIX;
}

} // namespace logapp
