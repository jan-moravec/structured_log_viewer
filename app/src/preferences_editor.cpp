#include "preferences_editor.hpp"

#include "appearance_control.hpp"

#include <QApplication>
#include <QLabel>
#include <QPushButton>
#include <QStyleFactory>
#include <QVBoxLayout>

PreferencesEditor::PreferencesEditor(QWidget *parent) : QWidget{parent}
{
    setWindowFlags(Qt::Window);
    setWindowTitle("Preferences");
    setMinimumWidth(300);

    mFontComboBox = new QFontComboBox(this);
    mSizeSpinBox = new QSpinBox(this);
    mStyleComboBox = new QComboBox(this);

    mSizeSpinBox->setRange(6, 72);

    for (const auto &style : QStyleFactory::keys())
    {
        mStyleComboBox->addItem(style.toLower());
    }

    connect(mFontComboBox, &QFontComboBox::currentFontChanged, [this](const QFont &font) {
        QFont newFont = font;
        newFont.setPointSize(mSizeSpinBox->value());
        qApp->setFont(newFont);
    });

    connect(mSizeSpinBox, QOverload<int>::of(&QSpinBox::valueChanged), [=](int size) {
        QFont newFont = qApp->font();
        newFont.setPointSize(size);
        qApp->setFont(newFont);
    });

    connect(mStyleComboBox, &QComboBox::currentTextChanged, [=](const QString &styleName) {
        qApp->setStyle(QStyleFactory::create(styleName));
    });

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(new QLabel("Select Style:"));
    layout->addWidget(mStyleComboBox);
    layout->addWidget(new QLabel("Select Font:"));
    layout->addWidget(mFontComboBox);
    layout->addWidget(new QLabel("Select Font Size:"));
    layout->addWidget(mSizeSpinBox);

    QPushButton *okButton = new QPushButton("Ok", this);
    QPushButton *cancelButton = new QPushButton("Cancel", this);

    connect(okButton, &QPushButton::clicked, this, [this]() {
        AppearanceControl::SaveConfiguration();
        close();
    });
    connect(cancelButton, &QPushButton::clicked, this, [this]() {
        AppearanceControl::LoadConfiguration();
        close();
    });

    QHBoxLayout *buttonLayout = new QHBoxLayout();
    buttonLayout->addWidget(okButton);
    buttonLayout->addWidget(cancelButton);
    layout->addStretch(1);
    layout->addLayout(buttonLayout);
}

void PreferencesEditor::UpdateFields()
{
    mSizeSpinBox->setValue(QApplication::font().pointSize());
    mStyleComboBox->setCurrentText(QApplication::style()->name());
    mFontComboBox->setCurrentFont(qApp->font());
}
