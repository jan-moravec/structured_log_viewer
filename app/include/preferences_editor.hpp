#pragma once

#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QSpinBox>
#include <QTimer>
#include <QWidget>

class QCloseEvent;
class ThemeControl;

class PreferencesEditor : public QWidget
{
    Q_OBJECT

public:
    /// @p theme owns the live theme state the dialog reads /
    /// writes through (`Active()`, `Apply(...)`, `SaveConfiguration()`,
    /// `themeChanged` signal). Production wires it through
    /// `MainWindow`; tests can pass `nullptr` for a dialog that
    /// renders without theme group behaviour.
    explicit PreferencesEditor(ThemeControl *theme = nullptr, QWidget *parent = nullptr);

    void UpdateFields();

protected:
    /// Treat closing the window (X button, Esc, OS shortcut) as
    /// Cancel: revert any live-previewed theme to the persisted
    /// selection and reload the streaming/session config. Without
    /// this, a Dark preview leaks past the dialog until the next
    /// application restart, because `QWidget::close()` does not go
    /// through the Cancel slot.
    ///
    /// Both Ok and Cancel slots set `mClosingViaButton` and call
    /// `close()`, so this slot fires for them too. Skipping the
    /// revert in those paths avoids an extra disk read on Ok and
    /// a redundant theme round-trip on Cancel; the bypass is keyed
    /// on the flag rather than just an idempotency claim because
    /// `StreamingControl::LoadConfiguration` is not free and could
    /// surface transient I/O errors during a perfectly normal Ok.
    void closeEvent(QCloseEvent *event) override;

signals:
    /// Fired after Ok commits the new retention cap to `QSettings`.
    /// `MainWindow` re-applies it via `LogModel::SetRetentionCap`.
    void streamingRetentionChanged(qulonglong retentionLines);

    /// Fired after Ok commits the **stream-mode** "Show newest lines
    /// first" flag. `MainWindow::ApplyDisplayOrder` only honours it when
    /// the current session is a stream session.
    void streamingDisplayOrderChanged(bool newestFirst);

    /// Fired after Ok commits the **static-mode** "Show newest lines
    /// first" flag. `MainWindow::ApplyDisplayOrder` only honours it when
    /// the current session is a static (file-mode) session.
    void staticDisplayOrderChanged(bool newestFirst);

private:
    /// Refill the theme combo from `ThemeControl::AvailableThemes`
    /// and select the active entry (Auto is the first entry).
    void RepopulateThemeCombo();

    /// Update the preview label from the resolved theme.
    void RefreshThemePreview();

    /// Show a transient status message that auto-clears after a
    /// few seconds. Empty string clears immediately.
    void ShowThemeStatus(const QString &message);

    QComboBox *mThemeComboBox;
    QLabel *mThemePreviewLabel;
    QLabel *mThemeStatusLabel;
    QTimer *mThemeStatusClearTimer;
    QSpinBox *mStreamRetentionSpinBox;
    QCheckBox *mStreamNewestFirstCheckBox;
    QCheckBox *mStaticNewestFirstCheckBox;
    QCheckBox *mRestoreLastSessionCheckBox;
    QSpinBox *mRecentSessionsMaxSpinBox;

    /// Non-owning. Outlives this dialog (lives in `main()`).
    /// `nullptr` is tolerated for tests; the theme group skips
    /// its theme work in that case.
    ThemeControl *mTheme;

    /// Set by the Ok / Cancel button slots immediately before they
    /// call `close()`, so `closeEvent` can skip the revert path.
    /// The X button / Alt+F4 / programmatic `close()` from
    /// elsewhere all leave it `false`, which is the case the
    /// revert exists for.
    bool mClosingViaButton = false;
};
