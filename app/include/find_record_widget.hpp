#pragma once

#include <QPointer>
#include <QString>
#include <QWidget>

class QAction;
class QKeyEvent;
class QLabel;
class QLineEdit;
class QPropertyAnimation;
class QShowEvent;
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
///   - `Escape` hides the bar (no literal "X" button),
///   - `Return` triggers find-next, `Shift+Return` find-prev
///     (Chromium / VS Code convention),
///   - slide-in / slide-out animated via `QPropertyAnimation` on
///     `maximumHeight`.
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

    /// Animated reveal. Use this from the parent instead of
    /// `show()` so the slide-in transition runs.
    void RevealAnimated();

    /// Animated hide. The widget remains in the layout but its
    /// `maximumHeight` is driven to 0 before `hide()`.
    void HideAnimated();

signals:
    void FindRecords(const QString &text, bool next, bool wildcards, bool regularExpressions);

    /// Emitted on text change or toggle change so the parent can
    /// recompute the total match count and answer via
    /// `SetMatchInfo`. Empty `text` means "no needle"; parents
    /// should respond with `SetMatchInfo(0, 0)`.
    void MatchCountRequested(const QString &text, bool wildcards, bool regularExpressions);

protected:
    void keyPressEvent(QKeyEvent *event) override;
    void showEvent(QShowEvent *event) override;

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

    /// Last expanded height captured in `showEvent`, used as the
    /// target for `RevealAnimated`.
    int mNaturalHeight = 0;
    QPointer<QPropertyAnimation> mAnimation;
};
