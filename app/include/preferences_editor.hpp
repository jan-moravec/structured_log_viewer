#pragma once

#include <QCheckBox>
#include <QComboBox>
#include <QFontComboBox>
#include <QSpinBox>
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
    QComboBox *mStyleComboBox;
    QFontComboBox *mFontComboBox;
    QSpinBox *mSizeSpinBox;
    QSpinBox *mStreamRetentionSpinBox;
    QCheckBox *mStreamNewestFirstCheckBox;
    QCheckBox *mStaticNewestFirstCheckBox;
};
