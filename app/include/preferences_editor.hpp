#pragma once

#include "overview_rail_widget.hpp"

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
    ///
    /// Regex template CRUD is not here; it lives in
    /// `RegexTemplatesEditor` (`Settings -> Regex templates...`).
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

    /// Fired on every "Show level icons" toggle (live preview) and
    /// again on Cancel/close to revert. `MainWindow` flips the
    /// model + delegate; `QSettings` is written only on Ok and only
    /// when the value actually changed.
    void showLevelIconsChanged(bool on);

    /// Fired on every "High contrast levels" toggle (live preview)
    /// and again on Cancel/close to revert. `MainWindow` forwards
    /// to `ThemeControl::SetHighContrast`, which reuses the normal
    /// `themeChanged()` repaint path.
    void highContrastLevelsChanged(bool on);

    /// Fired on every combo change (live preview) and on
    /// Cancel/close to revert. `QSettings` is only written on
    /// Ok when the value actually changed.
    void overviewRailWidthChanged(OverviewRailWidthMode mode);

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
    /// whether the active theme ships a `levelColumnOverride`. The
    /// disabled-state tooltip explains why the toggle is a no-op.
    /// Re-runs on every `themeChanged()` so the state stays in sync.
    void RefreshLevelIconsCheckboxAvailability();

    /// Same as `RefreshLevelIconsCheckboxAvailability` but gates
    /// the high-contrast checkbox on a non-empty `levelsHighContrast`
    /// block.
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
    QComboBox *mOverviewRailWidthComboBox;
    QCheckBox *mRestoreLastSessionCheckBox;
    QSpinBox *mRecentSessionsMaxSpinBox;

    /// Non-owning. Outlives this dialog (lives in `main()`).
    /// `nullptr` is tolerated for tests; the theme group skips
    /// its theme work in that case.
    ThemeControl *mTheme;

    /// Set by Ok / Cancel before they call `close()` so `closeEvent`
    /// skips the revert path. Genuine close (X / Alt+F4) leaves it false.
    bool mClosingViaButton = false;

    /// Captured at dialog-open time so Cancel/close can roll back
    /// any live preview the user toggled. Ok persists the current
    /// state; Cancel/close re-emits the change signal with these
    /// initial values.
    bool mInitialShowLevelIcons = true;
    bool mInitialHighContrastLevels = false;
    OverviewRailWidthMode mInitialOverviewRailWidth = OverviewRailWidthMode::Medium;
};
