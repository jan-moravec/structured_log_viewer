#pragma once

#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QSpinBox>
#include <QTimer>
#include <QWidget>

class PreferencesEditor : public QWidget
{
    Q_OBJECT

public:
    explicit PreferencesEditor(QWidget *parent = nullptr);

    void UpdateFields();

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
    /// Repopulate `mThemeComboBox` from `ThemeControl::AvailableThemes`
    /// and select the entry that matches the current active selection
    /// (Auto becomes the first entry).
    void RepopulateThemeCombo();

    /// Refresh `mThemePreviewLabel` from the currently resolved
    /// theme (after Auto / selection resolution).
    void RefreshThemePreview();

    /// Show @p message in the transient `mThemeStatusLabel` and
    /// arm `mThemeStatusClearTimer` to wipe it after a few
    /// seconds. Used by the Duplicate / Reload buttons so user
    /// actions get visible feedback even when the resolved theme
    /// (and therefore the preview label) is unchanged. Pass an
    /// empty string to clear immediately.
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
};
