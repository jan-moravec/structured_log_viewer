#pragma once

#include <QString>
#include <QWidget>

class QAction;
class QEvent;
class QKeyEvent;
class QLabel;
class QLineEdit;
class QObject;
class QTimer;
class QToolButton;

/// Modern incremental find bar, in the spirit of VS Code,
/// QtCreator and Kate. The bar is a horizontal `QLineEdit` flanked
/// by:
///   - flat `QToolButton`s for the regex (`.*`) and wildcard
///     (`*?`) match modes (mutually exclusive -- toggling one
///     un-toggles the other),
///   - a `QLabel` showing "*i* of *N*" or "*N* matches" live,
///   - Up / Down arrow `QToolButton`s for find-prev / find-next.
///
/// The `QLineEdit` itself provides the trailing clear button via
/// `setClearButtonEnabled(true)`. The toggles are *not* embedded
/// in the line edit via `QLineEdit::addAction(..., TrailingPosition)`
/// because that path renders the action's icon only -- the `.*`
/// and `*?` glyphs would be invisible and no `QStyle::SP_*` icon
/// reads unambiguously as regex or wildcard.
///
/// Keys (Chromium / VS Code convention):
///   - `Return`        -> find-next,
///   - `Shift+Return`  -> find-previous,
///   - `Escape`        -> dismiss the host dock.
/// There is no literal "X" button -- the host `QDockWidget`'s
/// title-bar X is the canonical close affordance.
///
/// Live match counts are driven by the parent: the widget emits
/// `MatchCountRequested` whenever the search text or its toggles
/// change, debounced through an owned `QTimer` so a fast typist
/// doesn't trigger a full-table scan on every keystroke. The
/// parent answers via `SetMatchInfo`. The `FindRecords` signal
/// still drives the "jump to next / prev" behaviour, which the
/// parent answers by updating the table view selection and
/// (optionally) calling `SetMatchInfo` with the new position.
class FindRecordWidget : public QWidget
{
    Q_OBJECT
public:
    explicit FindRecordWidget(QWidget *parent = nullptr);

public slots:
    /// Focus the search field and select its contents so the next
    /// keystroke replaces them.
    void SetEditFocus();

    /// Update the "*i* of *N*" / "*N* matches" indicator.
    ///
    /// - `total <= 0`: blanks the label text (empty needle). The
    ///   label stays in the layout with its reserved minimum
    ///   width so the trailing arrow buttons don't jitter every
    ///   time the indicator appears or disappears.
    /// - `current <= 0`: shows "%Ln matches" (no current pin yet).
    /// - `current > 0`: shows "%1 of %2".
    /// - `overflowed = true`: appends a `+` to the total ("10,000+
    ///   matches", "1 of 10,000+") so the user sees that the
    ///   parent capped the scan rather than that they happened to
    ///   land at exactly the cap. The cap lives on the parent;
    ///   this widget only renders the flag.
    void SetMatchInfo(int current, int total, bool overflowed = false);

    /// Close the host `QDockWidget` so `visibilityChanged`
    /// mirrors the View-menu toggle and a subsequent
    /// `RevealAndFocus` properly re-shows everything. No-op when
    /// the bar isn't in a dock or the dock is already hidden.
    /// Wired to the Escape shortcut.
    void DismissBar();

    /// Re-arm the debounce timer so a `MatchCountRequested` fires
    /// once after the next quiet window. Used by `MainWindow` to
    /// react to model / proxy mutations (filter changes, streaming
    /// batches) without doing a full-table scan on every signal:
    /// activity in the underlying model just keeps resetting the
    /// timer until things settle. No-op for an empty needle.
    void BumpMatchCountDebounce();

signals:
    void FindRecords(const QString &text, bool next, bool wildcards, bool regularExpressions);

    /// Emitted on text change or toggle change so the parent can
    /// recompute the total match count and answer via
    /// `SetMatchInfo`. Empty `text` means "no needle"; parents
    /// should respond with `SetMatchInfo(0, 0)`.
    void MatchCountRequested(const QString &text, bool wildcards, bool regularExpressions);

protected:
    void keyPressEvent(QKeyEvent *event) override;

    /// Intercept `Shift+Return` / `Shift+Enter` on `mEdit` before
    /// `QLineEdit` swallows it. Plain `QLineEdit::returnPressed`
    /// fires modifier-agnostic, and `QLineEdit::keyPressEvent`
    /// accepts the event without bubbling, so the parent's
    /// `keyPressEvent` never sees Shift+Return. The filter is the
    /// only reliable point to wire find-previous (Chromium /
    /// VS Code convention).
    bool eventFilter(QObject *watched, QEvent *event) override;

private slots:
    void FindNext();
    void FindPrevious();

    /// Coalesces text / toggle changes into one `MatchCountRequested`
    /// emit via a small debounce so a fast typist doesn't trigger
    /// a full-table scan on every keystroke.
    void RequestMatchCountSoon();

    /// Fires `MatchCountRequested` immediately. Used by the
    /// debounce timer.
    void EmitMatchCountRequest();

private:
    QLineEdit *mEdit = nullptr;
    QAction *mWildcardsAction = nullptr;
    QAction *mRegexAction = nullptr;
    QToolButton *mButtonNext = nullptr;
    QToolButton *mButtonPrevious = nullptr;
    QLabel *mMatchCountLabel = nullptr;

    /// Real debounce timer for `MatchCountRequested`. A single
    /// owned `QTimer::start()` per keystroke restarts the
    /// countdown so a fast typist coalesces into one match-count
    /// scan instead of N. Plain `QTimer::singleShot` does not
    /// coalesce (each call schedules a fresh fire).
    QTimer *mMatchCountTimer = nullptr;

    /// Maximum-age timer. `mMatchCountTimer` restarts on every
    /// keystroke / model bump and never fires under continuous
    /// activity (live-tail streaming, fast typing). This timer
    /// is started once when the trailing timer first arms and is
    /// *not* restarted by subsequent bumps -- so it forces an
    /// emit after at most `MATCH_COUNT_MAX_AGE_MS`, keeping the
    /// "*i* of *N*" indicator usable during continuous streaming
    /// instead of stranding the value from before the stream
    /// started. Both timers' `timeout` route through
    /// `EmitMatchCountRequest`, which stops both.
    QTimer *mMatchCountMaxAgeTimer = nullptr;

    /// Re-entrancy guard for `EmitMatchCountRequest`. Both timers
    /// route to the same slot; when their intervals elapse on the
    /// same event-loop pass, Qt enqueues two `timeout` events.
    /// The first invocation stops both timers, but the second
    /// event is already in the queue and would emit a duplicate
    /// `MatchCountRequested`. The guard collapses the duplicate.
    /// The recount is idempotent so this is purely a "don't pay
    /// twice" optimisation.
    bool mEmittingMatchCountRequest = false;
};
