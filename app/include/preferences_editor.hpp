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
    /// Treat genuine close (X / Esc / OS shortcut) as Cancel so a
    /// live-previewed theme doesn't leak past the dialog. Ok / Cancel
    /// slots set `mClosingViaButton` to bypass this path -- they've
    /// already taken care of state.
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

    /// Fired after Ok commits the "Show level icons" toggle.
    /// `MainWindow` slots in to detach the `LevelCellDelegate`
    /// before flipping `LogModel::SetShowLevelIcons(on)` -- the
    /// delegate's self-gate keeps in-between paints correct
    /// either way, but doing the detach first lets the default
    /// delegate paint the level text again without paying the
    /// delegate's proxy-chain walk on the next paint.
    void showLevelIconsChanged(bool on);

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
    QCheckBox *mShowLevelIconsCheckBox;
    QCheckBox *mRestoreLastSessionCheckBox;
    QSpinBox *mRecentSessionsMaxSpinBox;

    /// Non-owning. Outlives this dialog (lives in `main()`).
    /// `nullptr` is tolerated for tests; the theme group skips
    /// its theme work in that case.
    ThemeControl *mTheme;

    /// Set by Ok / Cancel before they call `close()` so `closeEvent`
    /// skips the revert path. Genuine close (X / Alt+F4) leaves it false.
    bool mClosingViaButton = false;
};
