#include "preferences_editor.hpp"

#include "appearance_control.hpp"
#include "streaming_control.hpp"

#include <QApplication>
#include <QGroupBox>
#include <QLabel>
#include <QPushButton>
#include <QStyleFactory>
#include <QVBoxLayout>

#include <cstddef>

PreferencesEditor::PreferencesEditor(QWidget *parent) : QWidget{parent}
{
    setWindowFlags(Qt::Window);
    setWindowTitle("Preferences");
    setMinimumWidth(300);

    mFontComboBox = new QFontComboBox(this);
    mSizeSpinBox = new QSpinBox(this);
    mStyleComboBox = new QComboBox(this);
    mStreamRetentionSpinBox = new QSpinBox(this);
    mStreamNewestFirstCheckBox = new QCheckBox("Show newest lines first", this);

    mSizeSpinBox->setRange(6, 72);

    mStreamRetentionSpinBox->setRange(
        static_cast<int>(StreamingControl::kMinRetentionLines), static_cast<int>(StreamingControl::kMaxRetentionLines)
    );
    mStreamRetentionSpinBox->setSingleStep(1000);
    mStreamRetentionSpinBox->setValue(static_cast<int>(StreamingControl::kDefaultRetentionLines));
    mStreamRetentionSpinBox->setToolTip(
        "Maximum number of streamed lines kept in memory. Oldest lines are dropped when the cap "
        "is reached. Higher values use more memory."
    );
    mStreamNewestFirstCheckBox->setToolTip(
        "When enabled, new lines appear at the top of the stream view (oldest at the bottom). "
        "Follow newest then keeps the top of the view pinned to the most recent line."
    );

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

    // Stream retention is applied transactionally on Ok: the spinbox
    // does not push live updates, so Cancel reverts cleanly.

    auto *layout = new QVBoxLayout(this);

    auto *appearanceGroup = new QGroupBox("Appearance", this);
    auto *appearanceLayout = new QVBoxLayout(appearanceGroup);
    appearanceLayout->addWidget(new QLabel("Select Style:"));
    appearanceLayout->addWidget(mStyleComboBox);
    appearanceLayout->addWidget(new QLabel("Select Font:"));
    appearanceLayout->addWidget(mFontComboBox);
    appearanceLayout->addWidget(new QLabel("Select Font Size:"));
    appearanceLayout->addWidget(mSizeSpinBox);

    auto *streamingGroup = new QGroupBox("Streaming", this);
    auto *streamingLayout = new QVBoxLayout(streamingGroup);
    streamingLayout->addWidget(new QLabel("Stream retention (lines):"));
    streamingLayout->addWidget(mStreamRetentionSpinBox);
    streamingLayout->addWidget(mStreamNewestFirstCheckBox);

    layout->addWidget(appearanceGroup);
    layout->addWidget(streamingGroup);

    QPushButton *okButton = new QPushButton("Ok", this);
    QPushButton *cancelButton = new QPushButton("Cancel", this);

    connect(okButton, &QPushButton::clicked, this, [this]() {
        AppearanceControl::SaveConfiguration();
        // Mirror dialog edits into `StreamingControl` and persist them
        // before notifying so observers querying the static accessors
        // from a slot see the committed values.
        const auto retention = static_cast<size_t>(mStreamRetentionSpinBox->value());
        const bool newestFirst = mStreamNewestFirstCheckBox->isChecked();
        const bool newestFirstChanged = (newestFirst != StreamingControl::IsNewestFirst());
        StreamingControl::SetRetentionLines(retention);
        StreamingControl::SetNewestFirst(newestFirst);
        StreamingControl::SaveConfiguration();
        emit streamingRetentionChanged(static_cast<qulonglong>(StreamingControl::RetentionLines()));
        // Only emit on a real toggle so the re-sort chain does not run
        // on every Ok click.
        if (newestFirstChanged)
        {
            emit streamingDisplayOrderChanged(newestFirst);
        }
        close();
    });
    connect(cancelButton, &QPushButton::clicked, this, [this]() {
        AppearanceControl::LoadConfiguration();
        // Revert the spinbox-edited values to the persisted ones; no
        // emit needed because the on-disk values are unchanged.
        StreamingControl::LoadConfiguration();
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
    mStreamRetentionSpinBox->setValue(static_cast<int>(StreamingControl::RetentionLines()));
    mStreamNewestFirstCheckBox->setChecked(StreamingControl::IsNewestFirst());
}
