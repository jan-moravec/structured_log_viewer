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

    /// Fired on every toggle of the "Show level icons" checkbox
    /// (live preview) and again on Cancel/close to revert to the
    /// pre-dialog state when needed. `MainWindow` calls
    /// `LogModel::SetShowLevelIcons(on)` (which emits a tightly-
    /// scoped `DecorationRole` change on the level column) and then
    /// `ApplyLevelCellDelegate()` so the delegate attaches /
    /// detaches on the right column. The delegate's self-gate keeps
    /// in-between paints correct regardless of ordering. Persisting
    /// to `QSettings` happens only on Ok (the dialog writes
    /// `ui/showLevelIcons` from the final state and skips the write
    /// when the value didn't actually change from the persisted
    /// value).
    void showLevelIconsChanged(bool on);

    /// Fired on every toggle of the "High contrast levels"
    /// checkbox (live preview) and again on Cancel/close to revert.
    /// `MainWindow` forwards to `ThemeControl::SetHighContrast(on)`,
    /// which re-runs `BuildStyleCache` and emits `themeChanged()` so
    /// the table re-paints with the boosted (or subtle) row colours.
    /// Persisting to `QSettings` happens only on Ok with the same
    /// "only-on-real-change" gate as the level-icons toggle.
    void highContrastLevelsChanged(bool on);

private:
    /// Refill the theme combo from `ThemeControl::AvailableThemes`
    /// and select the active entry (Auto is the first entry).
    void RepopulateThemeCombo();

    /// Update the preview label from the resolved theme.
    void RefreshThemePreview();

    /// Show a transient status message that auto-clears after a
    /// few seconds. Empty string clears immediately.
    void ShowThemeStatus(const QString &message);

    /// Enable / disable the "Show level icons" checkbox based on
    /// whether the *currently-applied* theme ships a
    /// `levelColumnOverride`. Called from `UpdateFields()` (dialog
    /// open) and on every `ThemeControl::themeChanged` emit while
    /// the dialog is alive (user picks a different theme, OS dark
    /// flip in Auto mode, etc.) so the checkbox state stays in
    /// sync with what a toggle would actually do.
    void RefreshLevelIconsCheckboxAvailability();

    /// Same idea as `RefreshLevelIconsCheckboxAvailability`, but
    /// gates the high-contrast checkbox on whether the active theme
    /// ships a non-empty `levelsHighContrast` block. Themes that
    /// omit it (most user themes by default) grey the toggle out
    /// with an explanatory tooltip so the user can tell the no-op
    /// state apart from a "stuck" checkbox.
    void RefreshHighContrastCheckboxAvailability();

    QComboBox *mThemeComboBox;
    QLabel *mThemePreviewLabel;
    QLabel *mThemeStatusLabel;
    QTimer *mThemeStatusClearTimer;
    QSpinBox *mStreamRetentionSpinBox;
    QCheckBox *mStreamNewestFirstCheckBox;
    QCheckBox *mStaticNewestFirstCheckBox;
    QCheckBox *mShowLevelIconsCheckBox;
    QCheckBox *mHighContrastLevelsCheckBox;
    QCheckBox *mRestoreLastSessionCheckBox;
    QSpinBox *mRecentSessionsMaxSpinBox;

    /// Non-owning. Outlives this dialog (lives in `main()`).
    /// `nullptr` is tolerated for tests; the theme group skips
    /// its theme work in that case.
    ThemeControl *mTheme;

    /// Set by Ok / Cancel before they call `close()` so `closeEvent`
    /// skips the revert path. Genuine close (X / Alt+F4) leaves it false.
    bool mClosingViaButton = false;

    /// Captured by `UpdateFields()` at dialog-open time so
    /// Cancel/close can rewind a live preview that the user
    /// toggled inside the dialog. Both flags mirror the persisted
    /// values: when the dialog opens they are the same as
    /// `QSettings::value(ui/...)`. On Ok the dialog persists the
    /// current state to `QSettings`; on Cancel/close it re-emits
    /// the corresponding signal with these initial values so the
    /// model / theme controller revert.
    bool mInitialShowLevelIcons = true;
    bool mInitialHighContrastLevels = false;
};
