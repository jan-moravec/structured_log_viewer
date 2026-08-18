# Session tabs

This note describes how multi-tab windows own sessions, bind shared docks, route
asynchronous completions, and close tabs. `MainWindow` remains the `QMainWindow`
host for chrome, dialogs, and menus. Session models and workers live on
`LogSession`. Completions enter through `SessionOperationController`.

## Tab ownership

Each window holds an ordered `WindowTab` record per page of the central tab
widget. A record stores:

- a stable `SessionInstanceId` (process-local, independent of persistence UUID)
- non-owning pointers to that tab's `LogSession` and `LogSessionView`
- last-focus widget
- persistent signal connections for that tab

Sessions are parented to the window. Views are parented to the tab widget.
`MainWindow::mSession` / `mSessionView` and the model aliases (`mModel`,
`mAnchors`, and so on) always name the **selected** tab. They are display
aliases, not operation ownership.

`HostedSession(id)` is the hosted-tab registry. A completion that cannot resolve
its origin there performs only the minimum worker cleanup required for
destruction.

## Tab-switch ordering

`OnActiveTabChanged` runs this sequence:

1. Ignore suppressed, invalid, or same-session notifications.
1. Record the outgoing view's last-focus widget when focus still lives in that
   view.
1. Drop selected-tab scoped connections.
1. Point aliases at the incoming session and view.
1. Reinstall selected-tab scoped connections.
1. Bind shared docks to the incoming `SessionBindContext` (each dock saves
   outgoing widget state during `Bind`).
1. Refresh window chrome from the incoming session (title, status, menus,
   toolbar, column visibility).
1. Restore focus and apply any queued presentation for the incoming tab.

Tab switching does not reset models, reload sources, or rebuild
`LogFilterModel` accepted-row maps. Hidden docks subscribe to the new sources
but defer full model walks until they are shown.

## Shared-dock binding

Shared docks (`Find`, `Histogram`, `Anchors`, `Parse errors`, `Record details`)
borrow the selected session. `Bind` on a live same-session context is a no-op.
A changed binding saves outgoing per-session widget state, swaps borrowed
pointers, and restores incoming state.

Histogram rebuild is deferred while the dock is hidden (`HistogramModel::BindSources(...,
deferRebuild=true)`). `PumpDeferredBind()` completes the walk on the next show.
The anchors dock skips tree population while buried.

Immediate-apply dialogs (Columns Manager, Diagnostics) close on tab switch.
Highlight Rules keeps a per-session draft: the shared editor captures the
outgoing buffer and restores the incoming draft without committing rules.

## Callback origin

Ingest, decompression, and export workers belong to the originating
`LogSession`. Finished slots resolve that session in the hosted-tab registry
through `SessionOperationController` and then call origin-parameterized helpers
(`OnStreamingFinished`, `OnDecompressionFinishedFor`, `OnExportFinishedFor`).

Those helpers read `origin->Model()`, `origin->Anchors()`, and the origin view.
They do not temporarily rebind the selected-tab aliases. Shell chrome (status
bar, Filters menu, toolbars, modal dialogs) updates only when `origin` is the
selected session. Background completions store presentation and failure notices
on the origin for `ApplyPendingPresentation` when that tab is selected.

`AutoSaveAllHostedSessions` walks hosted sessions in place. It does not activate
tabs or rebind docks.

## Close ordering

`PrepareSessionClose` is the single close-decision entry for Close Tab, Close
Window, quit, New Session, and destructive open:

1. If the Highlight Rules editor is bound to the closing session, capture its
   buffer into that session's draft.
1. Classify with `LogSession::CloseDecision()` (`Silent`, `Autosave`, or
   `Prompt`).
1. `Silent` proceeds. `Autosave` writes that session's snapshot without
   activating it; failure vetoes close. `Prompt` offers Save / Discard / Cancel
   (no Session Bundle export). Cancel leaves workers running.
1. Save or Discard then cancels and drains that session's workers.
1. Persistent connections disconnect and the tab leaves `mTabs` before models
   are destroyed. Sibling tabs keep their workers, views, and connections.
1. Closing the last tab uses the same decision model and then closes the window.
