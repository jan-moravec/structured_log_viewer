# GUI Modernization Plan

This document collects concrete, Qt 6.8-grounded ideas for modernizing the
`StructuredLogViewer` main window. Each item links back to the file / line in
the current codebase that motivates the change, lists the Qt APIs to lean on,
and notes how much it pulls its weight.

The list is roughly ordered by impact-per-effort. The "Priority" tag on each
item is a rough triage:

- **P1** — high impact, low/medium effort. Tackle first.
- **P2** — high impact, but more invasive (delegate work, custom widgets).
- **P3** — polish / nice-to-have / platform-specific.

Tick items off as they land.

______________________________________________________________________

## Snapshot of the current layout

- `QMainWindow` shell with menus `File`, `Edit`, `View`, `Filters`, `Stream`,
  `Settings` (see `app/src/main_window.ui`).
- Central widget hosts a `QVBoxLayout` with the `LogTableView` and a
  hidden-by-default `FindRecordWidget` underneath
  (`app/src/main_window.cpp:445-469`, `:607-610`).
- Right dock area holds the hidden-by-default `RecordDetailDock`
  (`app/src/main_window.cpp:612-616`).
- `Stream` `QToolBar` is the only toolbar; it is `setVisible(false)` outside
  live-tail (`app/src/main_window.cpp:575-580`).
- Status bar is a single concatenated `QString` in `UpdateStreamingStatus`
  (`app/src/main_window.cpp:2098-2154`) plus the diagnostics button.
- Per-level styling already flows through `Qt::BackgroundRole` /
  `Qt::ForegroundRole` in `LogModel::data()` (`app/src/log_model.cpp:1121+`).
- Reading-position preservation for newest-first streaming is implemented in
  `LogTableView` but invisible to users (`app/src/log_table_view.cpp:173-234`).

______________________________________________________________________

## 1. Persistent top toolbar (not just `Stream`) — P1

Today the only `QToolBar` is the live-tail one, hidden outside streaming
(`app/src/main_window.cpp:575-580`). Every other action lives in a menu.

**Add a primary toolbar** with the 5–10 most common actions:

- `actionOpen` (icon: `QStyle::SP_DialogOpenButton`)
- `actionOpenLogStream` / `actionOpenNetworkStream` (split button via
  `QToolButton::setPopupMode(QToolButton::MenuButtonPopup)`)
- A `QLineEdit` search field (see item 2)
- A "Quick level filter" `QComboBox` (`All / Warnings+ / Errors+`)
- `actionAddFilter`
- `actionToggleRecordDetails`
- Theme toggle (`QStyleHints::colorScheme()`-aware, see item 11)
- Pulsing live-tail indicator (custom `QWidget` driven by
  `QPropertyAnimation`)

Use `QToolBar::setToolButtonStyle(Qt::ToolButtonTextBesideIcon)` for
discoverability. Allow movement with `setMovable(true)` and
`setAllowedAreas(Qt::AllToolBarAreas)`.

The actions already exist; this is mostly `addToolBar(tr("Main"))` +
`addAction()` calls plus icon choices.

## 2. Modern incremental find bar — P1

`FindRecordWidget` is a flat `QHBoxLayout` with two `QPushButton`s and a
`QToolButton` labelled literal `"X"` (`app/src/find_record_widget.cpp:9-36`).
That looks dated next to QtCreator / Kate / VS Code.

Replacement plan, all Qt 6 native:

- `mEdit->setPlaceholderText(tr("Find in logs…"))`
- `mEdit->setClearButtonEnabled(true)`
- `mEdit->addAction(searchIcon, QLineEdit::LeadingPosition)` for the
  magnifying-glass affordance
- Replace `Next` / `Previous` `QPushButton`s with `QToolButton`s using
  `QStyle::SP_ArrowUp` / `SP_ArrowDown`
- Add a `QLabel` showing "*n* of *m*" matches, updated live on
  `QLineEdit::textChanged`
- `QShortcut(Qt::Key_Escape, this, &FindRecordWidget::hide)` instead of an
  X button
- Animate slide-down via `QPropertyAnimation` on `maximumHeight`
- Bind `QLineEdit::returnPressed` to find-next, `Shift+Return` to find-prev
  (Chromium / VS Code convention)
- Replace the two literal checkboxes with `QToolButton` toggles embedded in
  the line edit via `QLineEdit::addAction(..., QLineEdit::TrailingPosition)`

## 3. Active filter chips above the table — P1

Active filters only live in the `Filters` menu, which is invisible until
opened. Sentry, Kibana, Datadog show a horizontal row of removable chips.

Implementation outline:

- A new `QWidget` row above the table holding a `QHBoxLayout` of
  `QToolButton`s (one per active filter), wrapped in a `QScrollArea` for
  horizontal overflow.
- Each chip:
  - Tooltip from your existing `BuildFilterTitle(...)`
    (`app/include/main_window.hpp:670`).
  - Body click → opens per-filter editor (same path as the `Filters` menu's
    sub-menu entries).
  - "×" action → `ClearFilter(filterID)`.
- QSS for `border-radius` to get the rounded "pill" look.
- Show/hide the whole row based on `mFilters.empty()`.

## 4. Structured status-bar widgets instead of one big string — P1

`UpdateStreamingStatus` builds one concatenated string today
(`app/src/main_window.cpp:2098-2154`). Each component is a separate concern;
split into permanent widgets via `QStatusBar::addPermanentWidget`:

| Slot                         | Widget                                                              |
| ---------------------------- | ------------------------------------------------------------------- |
| Mode badge                   | `QLabel` with bg color (Static / Live / Net)                        |
| Source                       | `QLabel` with elided file path                                      |
| Line count                   | `QLabel`, right-aligned, fixed width                                |
| "*n* shown of *m*"           | `QLabel` driven by `mSortFilterProxyModel->rowCount()` (see item 5) |
| Error count                  | `QPushButton` opening the diagnostics dialog                        |
| Streaming throughput         | small sparkline widget                                              |
| Indeterminate `QProgressBar` | during initial static parse                                         |
| Pulse dot                    | live-tail heartbeat                                                 |

You already use this pattern for `mDiagnosticsButton`
(`app/src/main_window.cpp:682-689`); extend it.

## 5. "*n* shown of *m*" + clear-filters affordance — P1

`mSortFilterProxyModel->rowCount()` vs the source row count tells you how
many rows are hidden by current filters. Render the difference in the status
bar (item 4) and, when `proxyCount < sourceCount`, also show a small
`QToolButton` with a funnel icon labelled **Clear filters** near the table.
It triggers `actionClearAllFilters` (`app/src/main_window.cpp:570-571`).

This removes a frequent "where did my rows go?" confusion.

## 6. Empty-state placeholder when no source is loaded — P1 — DONE

`LogTableView::paintEvent` (`app/src/log_table_view.cpp`) overlays a
shortcut card on top of the empty grid. The catalog is auto-discovered
via `ShortcutCatalog::Build` (`app/src/shortcut_catalog.cpp`), so any
new shortcut surfaces in the placeholder with no further work. Colors
read from `QPalette::WindowText` / `QPalette::PlaceholderText` so the
card adapts to light / dark themes automatically. The same catalog
backs the `Ctrl+/` shortcuts dialog from item 18.

## 7. Custom `QStyledItemDelegate` for level "pills" — P2

`LogModel::data()` already returns per-level brushes
(`app/src/log_model.cpp:1121+`). A modern look paints the level column as a
rounded pill:

```cpp
class LogLevelDelegate : public QStyledItemDelegate {
    void paint(QPainter *p, const QStyleOptionViewItem &opt,
               const QModelIndex &idx) const override {
        QStyleOptionViewItem o(opt);
        initStyleOption(&o, idx);
        const QBrush bg = idx.data(Qt::BackgroundRole).value<QBrush>();
        QRect pill = o.rect.adjusted(4, 4, -4, -4);
        QPainterPath path;
        path.addRoundedRect(pill, pill.height() / 2.0, pill.height() / 2.0);
        p->fillPath(path, bg);
        p->drawText(pill, Qt::AlignCenter, o.text);
    }
};
mTableView->setItemDelegateForColumn(levelColumn, new LogLevelDelegate(mTableView));
```

Pair with item 17 (icons in `Qt::DecorationRole`) for a level-icon + label
look that matches Console.app and modern log tools.

## 8. Funnel icon in the header for filtered columns — P2

Subclass `QHeaderView` and override `paintSection`. For each `logicalIndex`
whose `keys` appear in `mFilters`, draw a small funnel SVG (or
`QStyle::SP_FileDialogListView`) in the section's top-right corner.

- Right-click the funnel → reuse `BuildHeaderContextMenu`
  (`app/include/main_window.hpp:276`).
- Excel / Sheets / Numbers all do this; makes active filters visible without
  scrolling to the chip bar.

## 9. Free up vertical space with platform integrations — P3

- On macOS: `QMainWindow::setUnifiedTitleAndToolBarOnMac(true)` merges the
  toolbar into the title bar (Big Sur+ native look).
- Let users `QToolBar::setMovable(true)` and
  `setAllowedAreas(Qt::AllToolBarAreas)` so power users can dock the
  search/filter toolbar on the side.

## 10. Word-wrap and "expand row" affordances — P2

JSON log lines often have giant `message` fields; default elide truncates
them. Add a `View → Wrap message column` toggle:

- When on:
  - `mTableView->setWordWrap(true)`
  - `mTableView->setTextElideMode(Qt::ElideNone)`
  - `mTableView->resizeRowsToContents()` on `modelReset` / batch arrival
- Or per-row expand: store a `Qt::UserRole` flag for expanded rows; the
  delegate's `sizeHint` returns the wrapped height for those rows only.

## 11. Auto-follow OS color scheme — P2

Qt 6.5+ exposes `QGuiApplication::styleHints()->colorScheme()` plus the
`colorSchemeChanged` signal. Wire `ThemeControl`
(`app/include/theme_control.hpp`) so an "Auto" theme follows the OS scheme
on Windows 11 / macOS automatically.

## 12. Surface the reading-position preservation — P2

`LogTableView::SaveAnchorIfShouldPreserve` /
`LogTableView::RestoreAnchorIfSaved`
(`app/src/log_table_view.cpp:173-234`) already preserve scroll position
during streaming inserts in newest-first mode. Make it discoverable:

- Show a floating "↓ *N* new lines" pill at the bottom (or top) of the
  viewport when the user has scrolled away and new rows arrived.
- Click → scroll to tail + re-engage Follow.
- Implementation: a `QToolButton` parented to `mTableView->viewport()`,
  repositioned in `resizeEvent`, fade-in via `QGraphicsOpacityEffect` +
  `QPropertyAnimation`.

Matches the YouTube comments / Slack pattern.

## 13. Mini-map / overview scrollbar — P2

The single biggest UX win for log viewers. Long files benefit hugely from a
custom scrollbar that paints density / errors.

Approach:

- Subclass `QScrollBar`, or install a thin custom widget alongside
  `mTableView->verticalScrollBar()`.
- Paint a vertical strip where each pixel ≈ one row, colored by level.
  Errors stand out as red ticks. Click-to-seek.
- Source data: walk the source model in fixed-size buckets and cache one
  aggregate level per bucket; invalidate on batch arrival.

Frequent feature in modern viewers — worth the custom-widget investment.

## 14. Convert `Find` and "Errors / Parse Issues" into `QDockWidget`s — P2

`RecordDetailDock` (`app/include/record_detail_dock.hpp`) is already a real
`QDockWidget`. Do the same for Find and for an errors panel:

- Free floating / dockable / closable chrome
- Saved positions via `QMainWindow::saveState()` /
  `restoreState()` keyed in `QSettings`
- Tabified docks (drag one onto the other) — Qt does this automatically
- Lets `Find` move to the bottom area where it belongs by IDE convention

## 15. Title bar + window-state polish — P3

Today:

```cpp
this->setWindowTitle("Structured Log Viewer");      // main_window.cpp:442
ApplyThemedWindowIcon();                            // main_window.cpp:443
```

Improvements:

- `setWindowFilePath(mStreamingFileName)` — Qt auto-integrates with the
  platform's proxy-icon affordance (small file icon next to the title on
  macOS / Windows).
- Update title per session change to e.g.
  `"<filename> — Structured Log Viewer [Live tail · 12k lines]"`.
- Use `[*]` placeholder + `setWindowModified(true)` when the session has
  unsaved filter edits.

## 16. Persist window / dock geometry properly — P1

`QMainWindow::saveState()` / `saveGeometry()`, round-tripped through
`QSettings(QSettings::UserScope, ...)`, give you "remember where the user
put everything" for free.

`SessionHistoryManager`
(`app/include/session_history_manager.hpp`) persists sessions, but
per-window chrome state appears unaddressed. One `restoreState(...)` line
in the constructor and a matching save in `closeEvent` is the entire
change.

## 17. Per-level / per-column icons via `Qt::DecorationRole` — P2

`LogModel::data()` already returns `Qt::BackgroundRole` /
`Qt::ForegroundRole`. Returning a small `QIcon` from `Qt::DecorationRole`
on the level column gives the leading-icon look used by Console.app,
Visual Studio Output, and most modern log viewers.

Use `QStyle::SP_MessageBoxInformation` / `SP_MessageBoxWarning` /
`SP_MessageBoxCritical` for a zero-asset start; swap to themed SVGs once a
designer is involved.

## 18. Keyboard discoverability — P3 — DONE

- `actionShowShortcuts` (`Ctrl+/`) opens `ShortcutsDialog`
  (`app/src/shortcuts_dialog.cpp`), which renders the auto-discovered
  catalog from `ShortcutCatalog::Build`. The dialog refreshes on every
  `show()` so newly registered shortcuts appear without a restart.
- `MainWindow::FinaliseActionMetadata` (called once at the end of the
  constructor) suffixes every `QAction` tool tip with its shortcut text
  and copies the tool tip into `statusTip()` when missing, so toolbar
  buttons surface the shortcut and `QMainWindow` shows a status-bar hint
  on hover for free. Idempotent; tooltips that already mention the
  shortcut literal are left alone.

## 19. Monospace font for log content — P3 — DONE

Log content reads monospaced by default via a `QTableView::item`
QSS rule applied in `MainWindow::ApplyTableStyleSheet`, which resolves
to `QFontDatabase::systemFont(QFontDatabase::FixedFont)` (Cascadia
Mono / SF Mono / Monospace per platform). Scoping to `::item` keeps
the widget's own font metrics on the system default — header,
scrollbar, geometry calculations are unchanged. When the active theme
sets `app.fontFamily`, the rule is omitted so the user's chosen
family wins end-to-end. No delegate, no per-column logic; users
retain full control via theme overrides.

## 20. Platform integration polish — P3 — DONE

- `MainWindow::DefaultOpenDir()` returns the last-used directory
  (persisted in `QSettings` under `ui/lastOpenDir`) or
  `QStandardPaths::DocumentsLocation` when nothing has been picked
  yet. Threaded through every `QFileDialog::getOpenFileName(s)` /
  `getSaveFileName` call site (`OpenFiles`, `OpenLogStream`,
  `LoadConfiguration`, `SaveConfiguration`, `SaveSession`); each
  records the parent directory back via `RememberLastOpenDir`.
- Native dialogs are kept (no `DontUseNativeDialog`).
- `dragEnterEvent` / `dragMoveEvent` now require
  `urls().first().isLocalFile()` before accepting, so remote
  payloads turn the cursor into the no-drop indicator.

## 21. Small Qt 6 modernizations easy to miss — P3 — PARTIAL

- `Q_DECLARE_TR_FUNCTIONS` in non-`QObject` helpers instead of
  `QString::fromLatin1` — **DROPPED** after audit. The remaining
  `fromLatin1` calls are non-user-facing identifiers
  (configuration keys, type tags); the rewrite would not surface a
  translatable string to any user.
- `QHeaderView::setSortIndicatorClearable(true)` (Qt 6.1+) — **DONE**.
  Wired in the `mTableView` setup so users cycle Asc -> Desc ->
  none with one extra header click.
- `QLocale::system().toString(qint64)` for grouped digit separators
  in the status bar — **DONE** in `UpdateStreamingStatus`. Lines /
  errors / dropped / paused-buffered counts now read "12,345 lines".
- `QElapsedTimer` + 1 Hz refresh for "MM:SS since start" — **DONE**.
  `mLiveTailTimer` is started in `OpenLogStreamFromPath` /
  `OpenNetworkStream` and stopped in `OnStreamingFinished`;
  `mLiveTailTickTimer` (1 s) drives `UpdateStreamingStatus` so the
  elapsed string ticks while the user watches.
- `QApplication::setStyle(QStyleFactory::create("Fusion"))` —
  not yet adopted; tracked for a follow-up since it changes look
  across the entire app and warrants a separate pass.

______________________________________________________________________

## Suggested first wave

If you tackle these five first, the rest compounds nicely on top:

1. **Filter chips above the table** (item 3) — turns invisible state into
   visible state, biggest UX gain.
1. **Modern incremental find bar** with match counter (item 2).
1. **Structured status bar** instead of one concatenated string (item 4)
   plus the "*n* shown of *m*" indicator (item 5).
1. **Mini-map scrollbar** or at least error ticks (item 13) — log viewers
   live or die on this.
1. **Level pill delegate** + decoration icons (items 7, 17) — instant
   modern feel.

Backed up by item 16 (persist window state) so the user's customizations
survive a restart.
