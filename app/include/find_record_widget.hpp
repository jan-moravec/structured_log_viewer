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

/// Modern incremental find bar.
///
/// Looks and feels like the find bar in VS Code, QtCreator, and
/// Kate:
///   - leading magnifying-glass icon and trailing clear button
///     inside the `QLineEdit`,
///   - trailing toggle "buttons" (regex / wildcards) embedded in
///     the line edit via `QLineEdit::addAction(...,
///     TrailingPosition)`,
///   - Up / Down arrow `QToolButton`s for find-prev / find-next,
///   - `QLabel` showing "*i* of *N*" or "*N* matches" live,
///   - `Escape` closes the host dock (no literal "X" button),
///   - `Return` triggers find-next, `Shift+Return` find-prev
///     (Chromium / VS Code convention).
///
/// Live match counts are driven by the parent: the widget emits
/// `MatchCountRequested` whenever the search text or its toggles
/// change, and the parent answers via `SetMatchInfo`. The
/// `FindRecords` signal still drives the "jump to next / prev"
/// behaviour, which the parent answers by updating the table view
/// selection and (optionally) calling `SetMatchInfo` with the new
/// position.
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
    /// - `total <= 0`: hides the label entirely (empty needle).
    /// - `current <= 0`: shows "%n matches" (no current pin yet).
    /// - `current > 0`: shows "%1 of %2".
    void SetMatchInfo(int current, int total);

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
};
