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

private:
    QComboBox *mStyleComboBox;
    QFontComboBox *mFontComboBox;
    QSpinBox *mSizeSpinBox;
};
