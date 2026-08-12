#pragma once

#include <QString>
#include <QWidget>

class QAction;
class QEvent;
class QIcon;
class QKeyEvent;
class QLabel;
class QLineEdit;
class QObject;
class QTimer;
class QToolButton;

/// Modern incremental find bar in the spirit of VS Code, QtCreator,
/// and Kate. Layout: search field with built-in clear button, regex
/// (`.*`) and wildcard (`*?`) toggles (mutually exclusive), live
/// "*i* of *N*" label, and prev / next arrow buttons.
///
/// Keys (Chromium / VS Code convention):
///   - `Return`        -> find-next
///   - `Shift+Return`  -> find-previous
///   - `Escape`        -> dismiss the host dock
///
/// Match counts are driven by the parent: the widget emits
/// `MatchCountRequested` whenever the query changes (debounced), and
/// the parent answers via `SetMatchInfo`. `FindRecords` still drives
/// jump-to-next/prev.
class FindRecordWidget : public QWidget
{
    Q_OBJECT
public:
    explicit FindRecordWidget(QWidget *parent = nullptr);

public slots:
    /// Focus the search field and select its contents.
    void SetEditFocus();

    /// Update the "*i* of *N*" / "*N* matches" indicator.
    ///
    /// - `total <= 0`: blanks the label (slot stays reserved so arrow
    ///   buttons don't jitter).
    /// - `current <= 0`: shows "*N* matches".
    /// - `current > 0`: shows "*i* of *N*".
    /// - `overflowed`: appends "+" to `total` (count is a lower
    ///   bound) and installs a tooltip explaining that the
    ///   current-match index may read as 0 once the scan
    ///   early-exits past the cursor-position cache.
    void SetMatchInfo(int current, int total, bool overflowed = false);

    /// Close the host `QDockWidget`. Wired to Escape; no-op when not
    /// hosted in a dock.
    void DismissBar();

    /// Re-arm the debounce timer so a `MatchCountRequested` fires
    /// once after the next quiet window. Lets `MainWindow` invalidate
    /// the count on model / proxy mutations without scanning per signal.
    void BumpMatchCountDebounce();

    /// Snapshot the query state (task 5.3). Read-only; safe to call
    /// on a bar whose dock is currently hidden.
    [[nodiscard]] QString queryText() const;
    [[nodiscard]] bool queryWildcards() const;
    [[nodiscard]] bool queryRegex() const;

    /// Restore the query state (task 5.3). Sets the text and the
    /// two matcher toggles WITHOUT emitting a `FindRecords`
    /// (the widget is being restored, not driven by the user);
    /// the match-count debounce is re-armed via
    /// `BumpMatchCountDebounce()` so a bind restores the "*i* of
    /// *N*" label on the next quiet window.
    void RestoreQueryState(const QString &text, bool wildcards, bool regex);

    /// Cancel any in-flight debounce timers (task 5.3). Called by
    /// `FindDock::Unbind()` so a session-owned model reset in the
    /// window between Unbind and the next Bind does not fire a
    /// `MatchCountRequested` against a stale model.
    void CancelPendingMatchCountRequest();

signals:
    void FindRecords(const QString &text, bool next, bool wildcards, bool regularExpressions);

    /// Fired on text / toggle changes; the parent answers with
    /// `SetMatchInfo`. Empty `text` means "no needle".
    void MatchCountRequested(const QString &text, bool wildcards, bool regularExpressions);

public:
    /// Catches Return / Shift+Return on `mEdit` before `QLineEdit`
    /// handles them. `QLineEdit` emits `returnPressed` but then
    /// ignores the key, so without this filter the parent
    /// `keyPressEvent` would fire FindNext a second time.
    /// Public to match `QObject::eventFilter`'s visibility.
    bool eventFilter(QObject *watched, QEvent *event) override;

protected:
    void keyPressEvent(QKeyEvent *event) override;

    /// Re-paint arrow icons on palette / style change; the default
    /// `SP_ArrowUp/Down` pixmaps are baked black and vanish on dark.
    void changeEvent(QEvent *event) override;

    /// Catches `DevicePixelRatioChange` when the bar moves between
    /// monitors of different DPI; without this the arrow icons stay
    /// rasterised at the previous DPR.
    bool event(QEvent *event) override;

private slots:
    void FindNext();
    void FindPrevious();

    /// Restart the debounce; coalesces a burst of text / toggle changes
    /// into one `MatchCountRequested` emit.
    void RequestMatchCountSoon();

    /// Fires `MatchCountRequested` now. Used by both debounce timers.
    void EmitMatchCountRequest();

private:
    QLineEdit *mEdit = nullptr;
    QAction *mWildcardsAction = nullptr;
    QAction *mRegexAction = nullptr;
    QToolButton *mButtonNext = nullptr;
    QToolButton *mButtonPrevious = nullptr;
    QLabel *mMatchCountLabel = nullptr;

    /// Trailing-edge debounce: restarted on every keystroke / bump so
    /// a fast typist coalesces N keystrokes into one match-count scan.
    QTimer *mMatchCountTimer = nullptr;

    /// Max-age timer: armed once alongside `mMatchCountTimer` and
    /// never restarted, so continuous activity (live-tail streaming,
    /// held keys) can't strand the indicator at a pre-activity value.
    QTimer *mMatchCountMaxAgeTimer = nullptr;

    /// Re-entrancy guard: both timers route to the same slot, and a
    /// simultaneous `timeout` would otherwise double-emit.
    bool mEmittingMatchCountRequest = false;

    /// Repaint the prev/next arrow icons in the current palette's
    /// `WindowText` colour. Replaces `SP_ArrowUp/Down`, which are
    /// baked black and invisible on dark themes.
    void RefreshArrowIcons();
};
