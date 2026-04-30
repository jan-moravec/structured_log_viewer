#pragma once

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
    /// Emitted on Ok after `StreamingControl::SaveConfiguration()` has
    /// committed the new retention cap to `QSettings`. `MainWindow`
    /// re-applies it via `LogModel::SetRetentionCap` so a running stream
    /// picks up the new value immediately (PRD 4.5.5 / task 5.12).
    void streamingRetentionChanged(qulonglong retentionLines);

private:
    QComboBox *mStyleComboBox;
    QFontComboBox *mFontComboBox;
    QSpinBox *mSizeSpinBox;
    QSpinBox *mStreamRetentionSpinBox;
};
