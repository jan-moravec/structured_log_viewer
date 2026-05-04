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

    /// Fired after Ok commits the **Show newest lines first** flag.
    /// `MainWindow` re-applies it via `ApplyStreamingDisplayOrder`.
    void streamingDisplayOrderChanged(bool newestFirst);

private:
    QComboBox *mStyleComboBox;
    QFontComboBox *mFontComboBox;
    QSpinBox *mSizeSpinBox;
    QSpinBox *mStreamRetentionSpinBox;
    QCheckBox *mStreamNewestFirstCheckBox;
};
