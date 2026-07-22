#include "highlight_rules_editor.hpp"

#include "theme_control.hpp"

#include <loglib/theme.hpp>

#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QIcon>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QScopeGuard>
#include <QSpacerItem>
#include <QSplitter>
#include <QStackedWidget>
#include <QStatusTipEvent>
#include <QStyle>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidgetAction>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <utility>

namespace
{

constexpr int STATUS_CLEAR_MS = 4000;
constexpr int SWATCH_ICON_FALLBACK_PX = 16;
constexpr int SWATCH_PAINT_INSET = 1;
constexpr int SWATCH_CORNER_RADIUS = 3;

// Default window size on first open (Qt persists user-resized geometry
// on subsequent shows within the same session).
constexpr int DIALOG_INITIAL_WIDTH_PX = 720;
constexpr int DIALOG_INITIAL_HEIGHT_PX = 480;

// Six fractional digits on the Number pane's spin boxes; wider than any
// realistic latency / count / ratio the user would type into a rule.
constexpr int NUMBER_SPIN_DECIMALS = 6;

// Horizontal breathing room between adjacent form groups (Min/Max spin
// pair on the Number pane; Background/Foreground swatch triad on the
// Rendering pane). Reused so the two panes stay visually aligned.
constexpr int FORM_GROUP_SPACING_PX = 12;

/// Combo labels; index matches `HighlightRule::Type`. Time /
/// Enumeration render on read-only panes in v1.
constexpr std::array<const char *, 5> TYPE_LABELS = {"Text (string)", "Time", "Enumeration", "Number", "Boolean"};

/// Combo labels; index matches `HighlightRule::Match` wire order.
constexpr std::array<const char *, 4> STRING_MATCH_LABELS = {"Exactly", "Contains", "Regular expression", "Wildcard"};

[[nodiscard]] QIcon RenderSwatchIcon(const QBrush &background, const QBrush &foreground, int sizePx, bool paintBorder)
{
    QPixmap pix(sizePx, sizePx);
    pix.fill(Qt::transparent);
    QPainter painter(&pix);
    painter.setRenderHint(QPainter::Antialiasing, true);
    if (background.style() != Qt::NoBrush)
    {
        painter.setBrush(background);
    }
    else
    {
        // "Inherit" / unset -- render a transparent tile.
        painter.setBrush(Qt::transparent);
    }
    if (paintBorder)
    {
        painter.setPen(QPen(foreground.style() != Qt::NoBrush ? foreground.color() : QColor(Qt::gray), 1));
    }
    else
    {
        painter.setPen(Qt::NoPen);
    }
    // Inset on all four sides so the stroke doesn't clip against
    // the pixmap edge on Fusion (and on Hi-DPI).
    const int side = sizePx - (2 * SWATCH_PAINT_INSET);
    painter.drawRoundedRect(
        QRectF(SWATCH_PAINT_INSET, SWATCH_PAINT_INSET, side, side), SWATCH_CORNER_RADIUS, SWATCH_CORNER_RADIUS
    );
    return QIcon{pix};
}

} // namespace

HighlightRulesEditor::HighlightRulesEditor(
    std::vector<loglib::LogConfiguration::HighlightRule> rules,
    std::vector<loglib::LogConfiguration::Column> columns,
    ThemeControl *theme,
    QWidget *parent
)
    : QWidget(parent, Qt::Window),
      mColumns(std::move(columns)),
      mTheme(theme),
      mLocalRules(std::move(rules)),
      mBaseline(mLocalRules)
{
    setWindowTitle(tr("Highlight rules"));
    setAttribute(Qt::WA_DeleteOnClose, false);
    resize(DIALOG_INITIAL_WIDTH_PX, DIALOG_INITIAL_HEIGHT_PX);

    // -- List side ---------------------------------------------------------
    mListWidget = new QListWidget(this);
    mListWidget->setSelectionMode(QAbstractItemView::SingleSelection);
    mListWidget->setUniformItemSizes(true);
    connect(mListWidget, &QListWidget::currentRowChanged, this, [this](int) { OnSelectionChanged(); });

    mNewButton = new QPushButton(tr("&New"), this);
    mDuplicateButton = new QPushButton(tr("D&uplicate"), this);
    mDeleteButton = new QPushButton(tr("&Delete"), this);
    mMoveUpButton = new QPushButton(tr("Move u&p"), this);
    mMoveDownButton = new QPushButton(tr("Move do&wn"), this);
    connect(mNewButton, &QPushButton::clicked, this, &HighlightRulesEditor::OnNewClicked);
    connect(mDuplicateButton, &QPushButton::clicked, this, &HighlightRulesEditor::OnDuplicateClicked);
    connect(mDeleteButton, &QPushButton::clicked, this, &HighlightRulesEditor::OnDeleteClicked);
    connect(mMoveUpButton, &QPushButton::clicked, this, &HighlightRulesEditor::OnMoveUpClicked);
    connect(mMoveDownButton, &QPushButton::clicked, this, &HighlightRulesEditor::OnMoveDownClicked);

    auto *listButtons = new QHBoxLayout();
    listButtons->addWidget(mNewButton);
    listButtons->addWidget(mDuplicateButton);
    listButtons->addWidget(mDeleteButton);
    listButtons->addStretch();
    listButtons->addWidget(mMoveUpButton);
    listButtons->addWidget(mMoveDownButton);

    auto *leftLayout = new QVBoxLayout();
    leftLayout->addLayout(listButtons);
    leftLayout->addWidget(mListWidget);
    auto *leftPane = new QWidget(this);
    leftPane->setLayout(leftLayout);

    // -- Form side ---------------------------------------------------------
    mNameEdit = new QLineEdit(this);
    mEnabledCheck = new QCheckBox(tr("Enabled"), this);
    mEnabledCheck->setChecked(true);
    mColumnCombo = new QComboBox(this);
    mTypeCombo = new QComboBox(this);
    for (const char *label : TYPE_LABELS)
    {
        mTypeCombo->addItem(QString::fromLatin1(label));
    }
    connect(mNameEdit, &QLineEdit::textEdited, this, [this](const QString &) { OnFieldEdited(); });
    connect(mEnabledCheck, &QCheckBox::toggled, this, [this](bool) { OnFieldEdited(); });
    connect(mColumnCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) {
        OnColumnChanged();
    });
    connect(mTypeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) { OnTypeChanged(); });

    // String pane
    mStringMatchCombo = new QComboBox(this);
    for (const char *label : STRING_MATCH_LABELS)
    {
        mStringMatchCombo->addItem(QString::fromLatin1(label));
    }
    mStringNeedleEdit = new QLineEdit(this);
    mStringNeedleEdit->setPlaceholderText(tr("Pattern or literal"));
    connect(mStringMatchCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) {
        OnFieldEdited();
    });
    connect(mStringNeedleEdit, &QLineEdit::textEdited, this, [this](const QString &) { OnFieldEdited(); });

    auto *stringPane = new QWidget(this);
    {
        auto *layout = new QHBoxLayout(stringPane);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->addWidget(mStringMatchCombo);
        layout->addWidget(mStringNeedleEdit, 1);
    }

    // Number pane
    mNumberMinEnabled = new QCheckBox(tr("Min"), this);
    mNumberMinValue = new QDoubleSpinBox(this);
    mNumberMinValue->setRange(std::numeric_limits<double>::lowest(), std::numeric_limits<double>::max());
    mNumberMinValue->setDecimals(NUMBER_SPIN_DECIMALS);
    // Scale the arrow-key step by the value's magnitude; the
    // default step of 1.0 is unusable across the full double range.
    mNumberMinValue->setStepType(QDoubleSpinBox::AdaptiveDecimalStepType);
    mNumberMaxEnabled = new QCheckBox(tr("Max"), this);
    mNumberMaxValue = new QDoubleSpinBox(this);
    mNumberMaxValue->setRange(std::numeric_limits<double>::lowest(), std::numeric_limits<double>::max());
    mNumberMaxValue->setDecimals(NUMBER_SPIN_DECIMALS);
    mNumberMaxValue->setStepType(QDoubleSpinBox::AdaptiveDecimalStepType);
    connect(mNumberMinEnabled, &QCheckBox::toggled, this, [this](bool on) {
        mNumberMinValue->setEnabled(on);
        OnFieldEdited();
    });
    connect(mNumberMinValue, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double) {
        OnFieldEdited();
    });
    connect(mNumberMaxEnabled, &QCheckBox::toggled, this, [this](bool on) {
        mNumberMaxValue->setEnabled(on);
        OnFieldEdited();
    });
    connect(mNumberMaxValue, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double) {
        OnFieldEdited();
    });

    auto *numberPane = new QWidget(this);
    {
        auto *layout = new QHBoxLayout(numberPane);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->addWidget(mNumberMinEnabled);
        layout->addWidget(mNumberMinValue, 1);
        layout->addSpacing(FORM_GROUP_SPACING_PX);
        layout->addWidget(mNumberMaxEnabled);
        layout->addWidget(mNumberMaxValue, 1);
    }

    // Boolean pane
    mBoolIncludeTrue = new QCheckBox(tr("true"), this);
    mBoolIncludeFalse = new QCheckBox(tr("false"), this);
    connect(mBoolIncludeTrue, &QCheckBox::toggled, this, [this](bool) { OnFieldEdited(); });
    connect(mBoolIncludeFalse, &QCheckBox::toggled, this, [this](bool) { OnFieldEdited(); });

    auto *boolPane = new QWidget(this);
    {
        auto *layout = new QHBoxLayout(boolPane);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->addWidget(new QLabel(tr("Match:"), this));
        layout->addWidget(mBoolIncludeTrue);
        layout->addWidget(mBoolIncludeFalse);
        layout->addStretch();
    }

    // Read-only panes for Time / Enumeration. Two *separate*
    // widgets even though the message is identical: a
    // `QStackedWidget` only holds each `QWidget` in one slot at a
    // time, so sharing a pointer between two slots silently
    // shifts every `HighlightRule::Type` index past `Time` off by
    // one.
    auto buildReadOnlyPane = [this]() -> QWidget * {
        auto *pane = new QWidget(this);
        auto *layout = new QVBoxLayout(pane);
        layout->setContentsMargins(0, 0, 0, 0);
        auto *label = new QLabel(
            tr("This rule type isn't editable from the GUI in this version.\n"
               "Time and Enumeration rules still match at runtime; edit their\n"
               "match parameters by hand in the configuration file."),
            pane
        );
        label->setWordWrap(true);
        layout->addWidget(label);
        return pane;
    };
    auto *timeReadOnlyPane = buildReadOnlyPane();
    auto *enumReadOnlyPane = buildReadOnlyPane();

    mMatchStack = new QStackedWidget(this);
    // Order MUST match `HighlightRule::Type`:
    //   0=String, 1=Time, 2=Enumeration, 3=Number, 4=Boolean.
    mMatchStack->insertWidget(0, stringPane);
    mMatchStack->insertWidget(1, timeReadOnlyPane);
    mMatchStack->insertWidget(2, enumReadOnlyPane);
    mMatchStack->insertWidget(3, numberPane);
    mMatchStack->insertWidget(4, boolPane);

    // Style pickers
    mForegroundButton = BuildSwatchButton(/*isForeground=*/true);
    mBackgroundButton = BuildSwatchButton(/*isForeground=*/false);
    mBoldCheck = new QCheckBox(tr("Bold"), this);
    mItalicCheck = new QCheckBox(tr("Italic"), this);
    connect(mBoldCheck, &QCheckBox::toggled, this, [this](bool) { OnFieldEdited(); });
    connect(mItalicCheck, &QCheckBox::toggled, this, [this](bool) { OnFieldEdited(); });

    auto *stylePane = new QGroupBox(tr("Rendering"), this);
    {
        auto *layout = new QHBoxLayout(stylePane);
        layout->addWidget(new QLabel(tr("Background:"), this));
        layout->addWidget(mBackgroundButton);
        layout->addSpacing(FORM_GROUP_SPACING_PX);
        layout->addWidget(new QLabel(tr("Foreground:"), this));
        layout->addWidget(mForegroundButton);
        layout->addSpacing(FORM_GROUP_SPACING_PX);
        layout->addWidget(mBoldCheck);
        layout->addWidget(mItalicCheck);
        layout->addStretch();
    }

    // Form layout
    auto *formGrid = new QGridLayout();
    int row = 0;
    formGrid->addWidget(new QLabel(tr("Name:"), this), row, 0);
    formGrid->addWidget(mNameEdit, row++, 1);
    formGrid->addWidget(mEnabledCheck, row++, 1);
    formGrid->addWidget(new QLabel(tr("Column:"), this), row, 0);
    formGrid->addWidget(mColumnCombo, row++, 1);
    formGrid->addWidget(new QLabel(tr("Match type:"), this), row, 0);
    formGrid->addWidget(mTypeCombo, row++, 1);
    formGrid->addWidget(new QLabel(tr("Match:"), this), row, 0);
    formGrid->addWidget(mMatchStack, row++, 1);
    formGrid->addWidget(stylePane, row++, 0, 1, 2);
    formGrid->setColumnStretch(1, 1);

    auto *rightLayout = new QVBoxLayout();
    rightLayout->addLayout(formGrid);
    rightLayout->addStretch();
    auto *rightPane = new QWidget(this);
    rightPane->setLayout(rightLayout);

    auto *splitter = new QSplitter(Qt::Horizontal, this);
    splitter->addWidget(leftPane);
    splitter->addWidget(rightPane);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 2);

    // -- Bottom bar --------------------------------------------------------
    mStatusLabel = new QLabel(this);
    mStatusLabel->setWordWrap(true);
    mStatusClearTimer = new QTimer(this);
    mStatusClearTimer->setSingleShot(true);
    connect(mStatusClearTimer, &QTimer::timeout, this, [this]() { mStatusLabel->clear(); });

    mSaveButton = new QPushButton(tr("&Save"), this);
    mSaveButton->setDefault(true);
    mRevertButton = new QPushButton(tr("&Revert"), this);
    mCloseButton = new QPushButton(tr("&Close"), this);
    connect(mSaveButton, &QPushButton::clicked, this, &HighlightRulesEditor::OnSaveClicked);
    connect(mRevertButton, &QPushButton::clicked, this, &HighlightRulesEditor::OnRevertClicked);
    connect(mCloseButton, &QPushButton::clicked, this, &HighlightRulesEditor::close);

    auto *bottomBar = new QHBoxLayout();
    bottomBar->addWidget(mStatusLabel, 1);
    bottomBar->addWidget(mSaveButton);
    bottomBar->addWidget(mRevertButton);
    bottomBar->addWidget(mCloseButton);

    auto *mainLayout = new QVBoxLayout(this);
    mainLayout->addWidget(splitter, 1);
    mainLayout->addLayout(bottomBar);

    // Repaint on theme change: rebuild swatch popups (icons +
    // captures close over the new brushes), reload the form so
    // the current swatch button repaints, and refresh every list
    // row's icon.
    if (mTheme != nullptr)
    {
        connect(mTheme, &ThemeControl::themeChanged, this, [this]() {
            if (mForegroundButton != nullptr)
            {
                RebuildSwatchMenu(mForegroundButton, /*isForeground=*/true);
            }
            if (mBackgroundButton != nullptr)
            {
                RebuildSwatchMenu(mBackgroundButton, /*isForeground=*/false);
            }
            LoadIntoForm(mCurrentRow);
            // Refresh row icons without disturbing the form.
            for (std::size_t i = 0; i < mLocalRules.size(); ++i)
            {
                RefreshListItem(static_cast<int>(i));
            }
        });
    }

    RepopulateColumnCombo();
    RebuildList(mLocalRules.empty() ? -1 : 0);
    UpdateFormEnabled();
    UpdateListButtons();
}

QToolButton *HighlightRulesEditor::BuildSwatchButton(bool isForeground)
{
    auto *button = new QToolButton(this);
    button->setPopupMode(QToolButton::InstantPopup);
    button->setToolButtonStyle(Qt::ToolButtonIconOnly);
    button->setIconSize(QSize(SwatchIconSizePx() + 4, SwatchIconSizePx() + 4));
    RebuildSwatchMenu(button, isForeground);
    // Default "inherit" icon; overwritten by `LoadIntoForm`.
    button->setIcon(RenderSwatchIcon(QBrush{}, QBrush(Qt::gray), SwatchIconSizePx(), true));
    return button;
}

void HighlightRulesEditor::RebuildSwatchMenu(QToolButton *button, bool isForeground)
{
    if (button == nullptr)
    {
        return;
    }
    // `deleteLater` is safer than `delete` in case a pending
    // action handler still holds a pointer into the old menu.
    if (QMenu *old = button->menu())
    {
        button->setMenu(nullptr);
        old->deleteLater();
    }

    auto *menu = new QMenu(button);
    // Slot 0 = "Inherit".
    QAction *inherit = menu->addAction(tr("Inherit"));
    inherit->setData(0);
    connect(inherit, &QAction::triggered, this, [this, button, isForeground]() {
        if (isForeground)
        {
            mSelectedForegroundIndex = 0;
        }
        else
        {
            mSelectedBackgroundIndex = 0;
        }
        button->setIcon(RenderSwatchIcon(QBrush{}, QBrush(Qt::gray), SwatchIconSizePx(), true));
        button->setText(tr("Inherit"));
        OnFieldEdited();
    });

    if (mTheme != nullptr)
    {
        for (std::uint8_t slot = 1; slot <= loglib::HIGHLIGHT_PALETTE_SIZE; ++slot)
        {
            const QBrush bg = mTheme->HighlightBrushFor(slot, Qt::BackgroundRole);
            const QBrush fg = mTheme->HighlightBrushFor(slot, Qt::ForegroundRole);
            QAction *act = menu->addAction(RenderSwatchIcon(bg, fg, SwatchIconSizePx(), true), tr("Slot %1").arg(slot));
            act->setData(slot);
            connect(act, &QAction::triggered, this, [this, button, slot, bg, fg, isForeground]() {
                if (isForeground)
                {
                    mSelectedForegroundIndex = slot;
                }
                else
                {
                    mSelectedBackgroundIndex = slot;
                }
                button->setIcon(RenderSwatchIcon(bg, fg, SwatchIconSizePx(), true));
                OnFieldEdited();
            });
        }
    }
    button->setMenu(menu);
}

int HighlightRulesEditor::SwatchIconSizePx() const
{
    if (const QStyle *s = style(); s != nullptr)
    {
        const int metric = s->pixelMetric(QStyle::PM_SmallIconSize, nullptr, this);
        if (metric > 0)
        {
            return metric;
        }
    }
    return SWATCH_ICON_FALLBACK_PX;
}

QString HighlightRulesEditor::FormatListLabel(const loglib::LogConfiguration::HighlightRule &rule) const
{
    QString label = QString::fromStdString(rule.name);
    if (label.isEmpty())
    {
        label = tr("(unnamed)");
    }
    if (!rule.enabled)
    {
        label += tr(" [disabled]");
    }
    if (ResolveColumnIndex(rule) < 0)
    {
        label += tr(" [inactive]");
    }
    return label;
}

QIcon HighlightRulesEditor::FormatListIcon(const loglib::LogConfiguration::HighlightRule &rule, int sizePx) const
{
    if (mTheme == nullptr)
    {
        return {};
    }
    const QBrush bg = mTheme->HighlightBrushFor(rule.backgroundIndex, Qt::BackgroundRole);
    const QBrush fg = mTheme->HighlightBrushFor(rule.foregroundIndex, Qt::ForegroundRole);
    return RenderSwatchIcon(bg, fg, sizePx, true);
}

int HighlightRulesEditor::ResolveColumnIndex(const loglib::LogConfiguration::HighlightRule &rule) const
{
    if (rule.columnKeys.empty())
    {
        return -1;
    }
    for (std::size_t i = 0; i < mColumns.size(); ++i)
    {
        const auto &keys = mColumns[i].keys;
        const bool allPresent = std::ranges::all_of(rule.columnKeys, [&keys](const std::string &k) {
            return std::ranges::find(keys, k) != keys.end();
        });
        if (allPresent)
        {
            return static_cast<int>(i);
        }
    }
    return -1;
}

void HighlightRulesEditor::RepopulateColumnCombo()
{
    if (mColumnCombo == nullptr)
    {
        return;
    }
    const QSignalBlocker block(mColumnCombo);
    mColumnCombo->clear();
    mColumnCombo->addItem(tr("(none)"), QVariant::fromValue(QStringList{}));
    // Tooltip on the "(none)" slot: catches the misclick before
    // the "(inactive)" list badge flags it post-hoc.
    mColumnCombo->setItemData(
        0, tr("Rules without a column can't match anything. Pick a column to activate this rule."), Qt::ToolTipRole
    );
    for (const auto &col : mColumns)
    {
        // Store only the *primary* key, not the full alias list.
        // `ResolveColumnByKeys` uses subset-match semantics, so
        // persisting a single key keeps the rule portable across
        // sources that share the primary key but emit different
        // alias sets.
        QStringList keys;
        if (!col.keys.empty())
        {
            keys.push_back(QString::fromStdString(col.keys.front()));
        }
        mColumnCombo->addItem(QString::fromStdString(col.header), QVariant::fromValue(keys));
    }
}

void HighlightRulesEditor::SetColumns(std::vector<loglib::LogConfiguration::Column> columns)
{
    mColumns = std::move(columns);
    RepopulateColumnCombo();
    RebuildList(mCurrentRow);
}

void HighlightRulesEditor::SetRules(std::vector<loglib::LogConfiguration::HighlightRule> rules)
{
    // No `ConfirmDiscardEdits` prompt: this is called from
    // `ApplyLoadedConfiguration` after the runtime + config have
    // already been replaced. Prompting could leave Save writing a
    // stale buffer over the freshly-loaded config. Any dirty
    // buffer is lost -- we surface a status-bar hint so it isn't
    // silent. The close-event still guards manual dismissal.
    const bool discardedDirtyBuffer = IsDirty();
    mLocalRules = std::move(rules);
    mBaseline = mLocalRules;
    RebuildList(mLocalRules.empty() ? -1 : 0);
    UpdateListButtons();
    UpdateFormEnabled();
    if (discardedDirtyBuffer)
    {
        ShowStatus(tr("Unsaved highlight-rule edits were discarded because a new configuration was loaded."), true);
    }
}

void HighlightRulesEditor::RebuildList(int selectRow)
{
    if (mListWidget == nullptr)
    {
        return;
    }
    const int keep = (selectRow >= 0) ? selectRow : mCurrentRow;
    const QSignalBlocker block(mListWidget);
    mListWidget->clear();
    for (const auto &rule : mLocalRules)
    {
        auto *item = new QListWidgetItem(FormatListIcon(rule, SwatchIconSizePx()), FormatListLabel(rule));
        item->setToolTip(FormatListLabel(rule));
        mListWidget->addItem(item);
    }
    const int rowCount = mListWidget->count();
    if (rowCount == 0)
    {
        mCurrentRow = -1;
        LoadIntoForm(-1);
        return;
    }
    const int selected = std::clamp(keep, 0, rowCount - 1);
    mListWidget->setCurrentRow(selected);
    // `currentRowChanged` only fires when the row actually
    // changes; force the reload otherwise.
    if (mCurrentRow == selected)
    {
        LoadIntoForm(selected);
    }
    else
    {
        mCurrentRow = selected;
        LoadIntoForm(selected);
    }
}

void HighlightRulesEditor::OnSelectionChanged()
{
    if (mListWidget == nullptr)
    {
        return;
    }
    const int row = mListWidget->currentRow();
    mCurrentRow = row;
    LoadIntoForm(row);
    UpdateListButtons();
}

void HighlightRulesEditor::LoadIntoForm(int row)
{
    mSuppressDirtySignals = true;
    const auto guard = qScopeGuard([this]() { mSuppressDirtySignals = false; });

    if (row < 0 || std::cmp_greater_equal(row, mLocalRules.size()))
    {
        mNameEdit->clear();
        mEnabledCheck->setChecked(true);
        mColumnCombo->setCurrentIndex(0);
        mTypeCombo->setCurrentIndex(0);
        mMatchStack->setCurrentIndex(0);
        mStringMatchCombo->setCurrentIndex(1); // Contains -- friendliest default.
        mStringNeedleEdit->clear();
        mNumberMinEnabled->setChecked(false);
        mNumberMinValue->setValue(0);
        mNumberMinValue->setEnabled(false);
        mNumberMaxEnabled->setChecked(false);
        mNumberMaxValue->setValue(0);
        mNumberMaxValue->setEnabled(false);
        mBoolIncludeTrue->setChecked(false);
        mBoolIncludeFalse->setChecked(false);
        mSelectedForegroundIndex = 0;
        mSelectedBackgroundIndex = 0;
        mBoldCheck->setChecked(false);
        mItalicCheck->setChecked(false);
        UpdateFormEnabled();
        return;
    }

    const auto &rule = mLocalRules[row];
    // `QLineEdit::setText` resets the cursor and clears the
    // selection; skip when the text already matches so the caret
    // survives a keystroke.
    if (mNameEdit->text() != QString::fromStdString(rule.name))
    {
        mNameEdit->setText(QString::fromStdString(rule.name));
    }
    mEnabledCheck->setChecked(rule.enabled);

    // Match by keys with the same subset semantics as
    // `ResolveColumnIndex` so the combo agrees with the paint path.
    int comboIndex = 0; // "(none)" slot.
    const int resolved = ResolveColumnIndex(rule);
    if (resolved >= 0)
    {
        comboIndex = resolved + 1;
    }
    mColumnCombo->setCurrentIndex(comboIndex);

    const int typeIdx = static_cast<int>(rule.type);
    mTypeCombo->setCurrentIndex(typeIdx);
    mMatchStack->setCurrentIndex(typeIdx);

    // Type-specific fields.
    using RT = loglib::LogConfiguration::HighlightRule::Type;
    switch (rule.type)
    {
    case RT::String:
    {
        const int matchIdx = rule.matchType.has_value() ? static_cast<int>(*rule.matchType) : 1;
        mStringMatchCombo->setCurrentIndex(matchIdx);
        const QString needle = rule.filterString.has_value() ? QString::fromStdString(*rule.filterString) : QString{};
        // Keep-cursor guard, see `mNameEdit` above.
        if (mStringNeedleEdit->text() != needle)
        {
            mStringNeedleEdit->setText(needle);
        }
        break;
    }
    case RT::Number:
    {
        mNumberMinEnabled->setChecked(rule.filterMinValue.has_value());
        mNumberMinValue->setEnabled(rule.filterMinValue.has_value());
        mNumberMinValue->setValue(rule.filterMinValue.value_or(0.0));
        mNumberMaxEnabled->setChecked(rule.filterMaxValue.has_value());
        mNumberMaxValue->setEnabled(rule.filterMaxValue.has_value());
        mNumberMaxValue->setValue(rule.filterMaxValue.value_or(0.0));
        break;
    }
    case RT::Boolean:
    {
        bool includeTrue = false;
        bool includeFalse = false;
        for (const auto &v : rule.filterValues)
        {
            std::string lower = v;
            std::ranges::transform(lower, lower.begin(), [](unsigned char ch) {
                return static_cast<char>(std::tolower(ch));
            });
            if (lower == "true")
            {
                includeTrue = true;
            }
            else if (lower == "false")
            {
                includeFalse = true;
            }
        }
        mBoolIncludeTrue->setChecked(includeTrue);
        mBoolIncludeFalse->setChecked(includeFalse);
        break;
    }
    case RT::Time:
    case RT::Enumeration:
    default:
        break;
    }

    mSelectedForegroundIndex = rule.foregroundIndex;
    mSelectedBackgroundIndex = rule.backgroundIndex;
    if (mTheme != nullptr)
    {
        const QBrush fgBg = mTheme->HighlightBrushFor(rule.foregroundIndex, Qt::BackgroundRole);
        const QBrush fgFg = mTheme->HighlightBrushFor(rule.foregroundIndex, Qt::ForegroundRole);
        mForegroundButton->setIcon(
            rule.foregroundIndex == 0 ? RenderSwatchIcon(QBrush{}, QBrush(Qt::gray), SwatchIconSizePx(), true)
                                      : RenderSwatchIcon(fgBg, fgFg, SwatchIconSizePx(), true)
        );
        const QBrush bgBg = mTheme->HighlightBrushFor(rule.backgroundIndex, Qt::BackgroundRole);
        const QBrush bgFg = mTheme->HighlightBrushFor(rule.backgroundIndex, Qt::ForegroundRole);
        mBackgroundButton->setIcon(
            rule.backgroundIndex == 0 ? RenderSwatchIcon(QBrush{}, QBrush(Qt::gray), SwatchIconSizePx(), true)
                                      : RenderSwatchIcon(bgBg, bgFg, SwatchIconSizePx(), true)
        );
    }
    mBoldCheck->setChecked(rule.bold);
    mItalicCheck->setChecked(rule.italic);

    UpdateFormEnabled();
}

void HighlightRulesEditor::UpdateFormEnabled()
{
    const bool haveSelection = mCurrentRow >= 0 && std::cmp_less(mCurrentRow, mLocalRules.size());
    for (QWidget *w :
         {static_cast<QWidget *>(mNameEdit),
          static_cast<QWidget *>(mEnabledCheck),
          static_cast<QWidget *>(mColumnCombo),
          static_cast<QWidget *>(mTypeCombo),
          static_cast<QWidget *>(mForegroundButton),
          static_cast<QWidget *>(mBackgroundButton),
          static_cast<QWidget *>(mBoldCheck),
          static_cast<QWidget *>(mItalicCheck),
          static_cast<QWidget *>(mMatchStack)})
    {
        if (w != nullptr)
        {
            w->setEnabled(haveSelection);
        }
    }
    // Read-only pane (Time / Enumeration) grays the whole stack.
    if (haveSelection)
    {
        const auto ruleType = mLocalRules[static_cast<std::size_t>(mCurrentRow)].type;
        const bool isReadOnly =
            (ruleType == loglib::LogConfiguration::HighlightRule::Type::Time ||
             ruleType == loglib::LogConfiguration::HighlightRule::Type::Enumeration);
        if (isReadOnly)
        {
            mMatchStack->setEnabled(false);
        }
    }
}

void HighlightRulesEditor::UpdateListButtons()
{
    const int row = mCurrentRow;
    const int total = static_cast<int>(mLocalRules.size());
    const bool haveSelection = row >= 0 && row < total;
    mDuplicateButton->setEnabled(haveSelection);
    mDeleteButton->setEnabled(haveSelection);
    mMoveUpButton->setEnabled(haveSelection && row > 0);
    mMoveDownButton->setEnabled(haveSelection && row < total - 1);
    // Save is gated on dirty + all-valid. Revert stays enabled --
    // it's the escape hatch from an invalid state.
    const auto [invalidRow, invalidMessage] = FirstInvalidRule();
    const bool allValid = invalidRow < 0;
    mSaveButton->setEnabled(IsDirty() && allValid);
    mRevertButton->setEnabled(IsDirty());
    if (!allValid)
    {
        // Persistent (no auto-clear) so the reason stays visible
        // while the user fixes it.
        mStatusLabel->setText(tr("Rule %1: %2").arg(invalidRow + 1).arg(invalidMessage));
        mStatusLabel->setStyleSheet(QStringLiteral("color: #B91C1C;"));
        mStatusClearTimer->stop();
    }
    else if (mStatusLabel != nullptr && mStatusLabel->styleSheet().contains(QStringLiteral("#B91C1C")))
    {
        // Clear a stale "Rule N: ..." message now that all pass.
        mStatusLabel->clear();
        mStatusLabel->setStyleSheet(QString{});
    }
}

QString HighlightRulesEditor::ValidateRule(const loglib::LogConfiguration::HighlightRule &rule) const
{
    if (rule.columnKeys.empty())
    {
        return tr("pick a column to match against.");
    }
    using RT = loglib::LogConfiguration::HighlightRule::Type;
    switch (rule.type)
    {
    case RT::String:
        if (!rule.matchType.has_value())
        {
            return tr("pick a string match type.");
        }
        if (!rule.filterString.has_value() || rule.filterString->empty())
        {
            return tr("enter a pattern or literal to match.");
        }
        return {};
    case RT::Number:
        if (!rule.filterMinValue.has_value() && !rule.filterMaxValue.has_value())
        {
            return tr("set a minimum or maximum value.");
        }
        return {};
    case RT::Boolean:
    {
        bool includeTrue = false;
        bool includeFalse = false;
        for (const std::string &v : rule.filterValues)
        {
            std::string lower = v;
            std::ranges::transform(lower, lower.begin(), [](unsigned char ch) {
                return static_cast<char>(std::tolower(ch));
            });
            if (lower == "true")
            {
                includeTrue = true;
            }
            else if (lower == "false")
            {
                includeFalse = true;
            }
        }
        if (!includeTrue && !includeFalse)
        {
            return tr("tick at least one of true / false.");
        }
        return {};
    }
    case RT::Time:
        if (!rule.filterBegin.has_value() && !rule.filterEnd.has_value())
        {
            return tr("Time rules need begin or end bounds (edit via config file in v1).");
        }
        return {};
    case RT::Enumeration:
        if (rule.filterValues.empty())
        {
            return tr("Enumeration rules need at least one value (edit via config file in v1).");
        }
        return {};
    default:
        return {};
    }
}

std::pair<int, QString> HighlightRulesEditor::FirstInvalidRule() const
{
    for (std::size_t i = 0; i < mLocalRules.size(); ++i)
    {
        const QString message = ValidateRule(mLocalRules[i]);
        if (!message.isEmpty())
        {
            return {static_cast<int>(i), message};
        }
    }
    return {-1, QString{}};
}

bool HighlightRulesEditor::IsDirty() const
{
    return mLocalRules != mBaseline;
}

void HighlightRulesEditor::MarkDirty()
{
    UpdateListButtons();
}

void HighlightRulesEditor::OnFieldEdited()
{
    if (mSuppressDirtySignals)
    {
        return;
    }
    GatherForm();
    // Refresh just this row's label / icon so the form's line-
    // edits keep their cursor. `RebuildList` is for structural
    // mutations (new / duplicate / delete / move).
    RefreshListItem(mCurrentRow);
    MarkDirty();
}

void HighlightRulesEditor::OnColumnChanged()
{
    if (mSuppressDirtySignals)
    {
        return;
    }
    // v1: no type gating on column change; a type-mismatched rule
    // simply renders inert.
    GatherForm();
    RefreshListItem(mCurrentRow);
    MarkDirty();
}

void HighlightRulesEditor::OnTypeChanged()
{
    if (mSuppressDirtySignals)
    {
        return;
    }
    mMatchStack->setCurrentIndex(mTypeCombo->currentIndex());
    UpdateFormEnabled();
    GatherForm();
    RefreshListItem(mCurrentRow);
    MarkDirty();
}

void HighlightRulesEditor::RefreshListItem(int row)
{
    if (mListWidget == nullptr || row < 0 || std::cmp_greater_equal(row, mLocalRules.size()))
    {
        return;
    }
    QListWidgetItem *item = mListWidget->item(row);
    if (item == nullptr)
    {
        return;
    }
    // Belt-and-braces: `setText` / `setIcon` don't signal today,
    // but block anyway in case a future proxy view does.
    const QSignalBlocker block(mListWidget);
    const auto &rule = mLocalRules[static_cast<std::size_t>(row)];
    item->setIcon(FormatListIcon(rule, SwatchIconSizePx()));
    const QString label = FormatListLabel(rule);
    item->setText(label);
    item->setToolTip(label);
}

void HighlightRulesEditor::GatherForm()
{
    if (mCurrentRow < 0 || std::cmp_greater_equal(mCurrentRow, mLocalRules.size()))
    {
        return;
    }
    auto &rule = mLocalRules[static_cast<std::size_t>(mCurrentRow)];
    rule.name = mNameEdit->text().toStdString();
    rule.enabled = mEnabledCheck->isChecked();

    // Column keys come from the combo item's `QStringList` (a
    // single primary key per column; empty for "(none)" -- see
    // `RepopulateColumnCombo`).
    const QVariant columnData = mColumnCombo->currentData();
    rule.columnKeys.clear();
    if (columnData.canConvert<QStringList>())
    {
        for (const QString &k : columnData.toStringList())
        {
            rule.columnKeys.push_back(k.toStdString());
        }
    }

    rule.type = static_cast<loglib::LogConfiguration::HighlightRule::Type>(mTypeCombo->currentIndex());

    // Only reset fields the editor owns. Time / Enumeration are
    // authored via the config file, so any style / name tweak
    // routed through here must preserve their match spec.
    // Flipping the type to (say) String is still safe: the
    // `String` branch owns and repopulates its own fields, and
    // `CompileRule` reads only the fields relevant to `rule.type`.
    using RT = loglib::LogConfiguration::HighlightRule::Type;
    switch (rule.type)
    {
    case RT::String:
        rule.filterBegin.reset();
        rule.filterEnd.reset();
        rule.filterMinValue.reset();
        rule.filterMaxValue.reset();
        rule.filterValues.clear();
        rule.matchType = static_cast<loglib::LogConfiguration::HighlightRule::Match>(mStringMatchCombo->currentIndex());
        rule.filterString = mStringNeedleEdit->text().toStdString();
        break;
    case RT::Number:
        rule.matchType.reset();
        rule.filterString.reset();
        rule.filterBegin.reset();
        rule.filterEnd.reset();
        rule.filterValues.clear();
        rule.filterMinValue.reset();
        rule.filterMaxValue.reset();
        if (mNumberMinEnabled->isChecked())
        {
            rule.filterMinValue = mNumberMinValue->value();
        }
        if (mNumberMaxEnabled->isChecked())
        {
            rule.filterMaxValue = mNumberMaxValue->value();
        }
        break;
    case RT::Boolean:
        rule.matchType.reset();
        rule.filterString.reset();
        rule.filterBegin.reset();
        rule.filterEnd.reset();
        rule.filterMinValue.reset();
        rule.filterMaxValue.reset();
        rule.filterValues.clear();
        if (mBoolIncludeTrue->isChecked())
        {
            rule.filterValues.emplace_back("true");
        }
        if (mBoolIncludeFalse->isChecked())
        {
            rule.filterValues.emplace_back("false");
        }
        break;
    case RT::Time:
    case RT::Enumeration:
    default:
        // Read-only in v1: leave the match-spec fields untouched
        // so hand-authored config values survive editor-only edits.
        break;
    }

    rule.foregroundIndex = mSelectedForegroundIndex;
    rule.backgroundIndex = mSelectedBackgroundIndex;
    rule.bold = mBoldCheck->isChecked();
    rule.italic = mItalicCheck->isChecked();
}

void HighlightRulesEditor::OnNewClicked()
{
    loglib::LogConfiguration::HighlightRule rule;
    rule.name = "New rule";
    rule.enabled = true;
    rule.type = loglib::LogConfiguration::HighlightRule::Type::String;
    rule.matchType = loglib::LogConfiguration::HighlightRule::Match::Contains;
    // Empty needle keeps the rule inactive until the user types
    // (see `CompileRule` and `ValidateRule`); the line edit's
    // placeholder guides them.
    rule.filterString = std::string{};
    if (!mColumns.empty() && !mColumns[0].keys.empty())
    {
        // Single primary key -- see `RepopulateColumnCombo`.
        rule.columnKeys = {mColumns[0].keys.front()};
    }
    mLocalRules.push_back(std::move(rule));
    RebuildList(static_cast<int>(mLocalRules.size()) - 1);
    UpdateListButtons();
    MarkDirty();
}

void HighlightRulesEditor::OnDuplicateClicked()
{
    if (mCurrentRow < 0 || std::cmp_greater_equal(mCurrentRow, mLocalRules.size()))
    {
        return;
    }
    loglib::LogConfiguration::HighlightRule copy = mLocalRules[static_cast<std::size_t>(mCurrentRow)];
    copy.name += " (copy)";
    mLocalRules.insert(mLocalRules.begin() + mCurrentRow + 1, std::move(copy));
    RebuildList(mCurrentRow + 1);
    UpdateListButtons();
    MarkDirty();
}

void HighlightRulesEditor::OnDeleteClicked()
{
    if (mCurrentRow < 0 || std::cmp_greater_equal(mCurrentRow, mLocalRules.size()))
    {
        return;
    }
    mLocalRules.erase(mLocalRules.begin() + mCurrentRow);
    const int newRow = mLocalRules.empty() ? -1 : std::min(mCurrentRow, static_cast<int>(mLocalRules.size()) - 1);
    RebuildList(newRow);
    UpdateListButtons();
    MarkDirty();
}

void HighlightRulesEditor::OnMoveUpClicked()
{
    if (mCurrentRow <= 0 || std::cmp_greater_equal(mCurrentRow, mLocalRules.size()))
    {
        return;
    }
    std::swap(mLocalRules[mCurrentRow], mLocalRules[mCurrentRow - 1]);
    RebuildList(mCurrentRow - 1);
    UpdateListButtons();
    MarkDirty();
}

void HighlightRulesEditor::OnMoveDownClicked()
{
    if (mCurrentRow < 0 || mCurrentRow >= static_cast<int>(mLocalRules.size()) - 1)
    {
        return;
    }
    std::swap(mLocalRules[mCurrentRow], mLocalRules[mCurrentRow + 1]);
    RebuildList(mCurrentRow + 1);
    UpdateListButtons();
    MarkDirty();
}

void HighlightRulesEditor::OnSaveClicked()
{
    if (!IsDirty())
    {
        return;
    }
    // Belt-and-braces: Save is disabled while any rule is invalid,
    // but a synthetic click could still land here. Refuse rather
    // than emitting a rule that compiles to nothing downstream.
    if (const auto [invalidRow, invalidMessage] = FirstInvalidRule(); invalidRow >= 0)
    {
        ShowStatus(tr("Rule %1: %2").arg(invalidRow + 1).arg(invalidMessage), /*isError=*/true);
        return;
    }
    mBaseline = mLocalRules;
    emit rulesSaved(mLocalRules);
    UpdateListButtons();
    ShowStatus(tr("Highlight rules saved."), false);
}

void HighlightRulesEditor::OnRevertClicked()
{
    if (!IsDirty())
    {
        return;
    }
    mLocalRules = mBaseline;
    RebuildList(mLocalRules.empty() ? -1 : std::min(mCurrentRow, static_cast<int>(mLocalRules.size()) - 1));
    UpdateListButtons();
    ShowStatus(tr("Reverted to saved state."), false);
}

void HighlightRulesEditor::ShowStatus(const QString &message, bool isError)
{
    mStatusLabel->setText(message);
    mStatusLabel->setStyleSheet(isError ? QStringLiteral("color: #B91C1C;") : QString{});
    mStatusClearTimer->start(STATUS_CLEAR_MS);
}

bool HighlightRulesEditor::ConfirmDiscardEdits()
{
    if (!IsDirty())
    {
        return true;
    }
    const QMessageBox::StandardButton result = QMessageBox::warning(
        this,
        tr("Discard unsaved changes?"),
        tr("The rule list has unsaved edits. Discard them?"),
        QMessageBox::Discard | QMessageBox::Cancel,
        QMessageBox::Cancel
    );
    return result == QMessageBox::Discard;
}

void HighlightRulesEditor::closeEvent(QCloseEvent *event)
{
    if (!ConfirmDiscardEdits())
    {
        event->ignore();
        return;
    }
    QWidget::closeEvent(event);
}
