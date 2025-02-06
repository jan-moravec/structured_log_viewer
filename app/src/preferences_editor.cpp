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

    // Create UI elements

    mFontComboBox = new QFontComboBox(this);
    mSizeSpinBox = new QSpinBox(this);
    mStyleComboBox = new QComboBox(this);

    // Configure font size selection
    mSizeSpinBox->setRange(6, 72);

    // Populate style selection with available QStyles
    for (const auto &style : QStyleFactory::keys())
    {
        mStyleComboBox->addItem(style.toLower());
    }

    // Connect font selection change
    connect(mFontComboBox, &QFontComboBox::currentFontChanged, [this](const QFont &font) {
        QFont newFont = font;
        newFont.setPointSize(mSizeSpinBox->value());
        qApp->setFont(newFont);
    });

    // Connect font size change
    connect(mSizeSpinBox, QOverload<int>::of(&QSpinBox::valueChanged), [=](int size) {
        QFont newFont = qApp->font();
        newFont.setPointSize(size);
        qApp->setFont(newFont);
    });

    // Connect style selection change
    connect(mStyleComboBox, &QComboBox::currentTextChanged, [=](const QString &styleName) {
        qApp->setStyle(QStyleFactory::create(styleName)); // Apply new QStyle globally
    });

    // Add widgets to layout
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
