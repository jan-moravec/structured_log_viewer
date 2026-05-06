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

namespace
{
constexpr int PREFERENCES_MIN_WIDTH_PX = 300;
constexpr int FONT_POINT_SIZE_MIN = 6;
constexpr int FONT_POINT_SIZE_MAX = 72;
constexpr int RETENTION_LINES_SPIN_SINGLE_STEP = 1000;
} // namespace

PreferencesEditor::PreferencesEditor(QWidget *parent)
    : QWidget{parent}
{
    setWindowFlags(Qt::Window);
    setWindowTitle("Preferences");
    setMinimumWidth(PREFERENCES_MIN_WIDTH_PX);

    mFontComboBox = new QFontComboBox(this);
    mSizeSpinBox = new QSpinBox(this);
    mStyleComboBox = new QComboBox(this);
    mStreamRetentionSpinBox = new QSpinBox(this);
    mStreamNewestFirstCheckBox = new QCheckBox("Show newest lines first", this);
    mStaticNewestFirstCheckBox = new QCheckBox("Show newest lines first", this);

    mSizeSpinBox->setRange(FONT_POINT_SIZE_MIN, FONT_POINT_SIZE_MAX);

    mStreamRetentionSpinBox->setRange(
        static_cast<int>(StreamingControl::MIN_RETENTION_LINES), static_cast<int>(StreamingControl::MAX_RETENTION_LINES)
    );
    mStreamRetentionSpinBox->setSingleStep(RETENTION_LINES_SPIN_SINGLE_STEP);
    mStreamRetentionSpinBox->setValue(static_cast<int>(StreamingControl::DEFAULT_RETENTION_LINES));
    mStreamRetentionSpinBox->setToolTip(
        "Maximum number of streamed lines kept in memory. Oldest lines are dropped when the cap "
        "is reached. Higher values use more memory."
    );
    mStreamNewestFirstCheckBox->setToolTip(
        "When enabled, new lines appear at the top of the stream view (oldest at the bottom). "
        "Follow newest then keeps the top of the view pinned to the most recent line."
    );
    mStaticNewestFirstCheckBox->setToolTip(
        "When enabled, files opened in static (file) mode are displayed with the last line "
        "at the top and the first line at the bottom."
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

    auto *staticGroup = new QGroupBox("Static (file mode)", this);
    auto *staticLayout = new QVBoxLayout(staticGroup);
    staticLayout->addWidget(mStaticNewestFirstCheckBox);

    layout->addWidget(appearanceGroup);
    layout->addWidget(streamingGroup);
    layout->addWidget(staticGroup);

    auto *okButton = new QPushButton("Ok", this);
    auto *cancelButton = new QPushButton("Cancel", this);

    connect(okButton, &QPushButton::clicked, this, [this]() {
        AppearanceControl::SaveConfiguration();
        // Mirror dialog edits into `StreamingControl` and persist them
        // before notifying so observers querying the static accessors
        // from a slot see the committed values.
        const auto retention = static_cast<size_t>(mStreamRetentionSpinBox->value());
        const bool streamNewestFirst = mStreamNewestFirstCheckBox->isChecked();
        const bool staticNewestFirst = mStaticNewestFirstCheckBox->isChecked();
        const bool streamNewestFirstChanged = (streamNewestFirst != StreamingControl::IsNewestFirst());
        const bool staticNewestFirstChanged = (staticNewestFirst != StreamingControl::IsStaticNewestFirst());
        StreamingControl::SetRetentionLines(retention);
        StreamingControl::SetNewestFirst(streamNewestFirst);
        StreamingControl::SetStaticNewestFirst(staticNewestFirst);
        StreamingControl::SaveConfiguration();
        emit streamingRetentionChanged(static_cast<qulonglong>(StreamingControl::RetentionLines()));
        // Only emit on a real toggle so the re-sort chain does not run
        // on every Ok click.
        if (streamNewestFirstChanged)
        {
            emit streamingDisplayOrderChanged(streamNewestFirst);
        }
        if (staticNewestFirstChanged)
        {
            emit staticDisplayOrderChanged(staticNewestFirst);
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

    auto *buttonLayout = new QHBoxLayout();
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
    mStaticNewestFirstCheckBox->setChecked(StreamingControl::IsStaticNewestFirst());
}
