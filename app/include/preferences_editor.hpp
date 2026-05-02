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
    /// Emitted on Ok after `StreamingControl::SaveConfiguration()` has
    /// committed the new retention cap to `QSettings`. `MainWindow`
    /// re-applies it via `LogModel::SetRetentionCap` so a running stream
    /// picks up the new value immediately (PRD 4.5.5 / task 5.12).
    void streamingRetentionChanged(qulonglong retentionLines);

    /// Emitted on Ok after `StreamingControl::SaveConfiguration()` has
    /// committed the **Show newest lines first** flag. `MainWindow`
    /// re-applies it to `StreamOrderProxyModel` so the visible order
    /// flips immediately, including for a stream that is already in
    /// flight.
    void streamingDisplayOrderChanged(bool newestFirst);

private:
    QComboBox *mStyleComboBox;
    QFontComboBox *mFontComboBox;
    QSpinBox *mSizeSpinBox;
    QSpinBox *mStreamRetentionSpinBox;
    QCheckBox *mStreamNewestFirstCheckBox;
};
