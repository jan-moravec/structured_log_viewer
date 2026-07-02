#include "regex_templates_editor.hpp"

#include "regex_template_registry.hpp"

#include <loglib/parsers/regex_parser.hpp>
#include <loglib/regex_templates.hpp>

#include <QCheckBox>
#include <QCloseEvent>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QSplitter>
#include <QTimer>
#include <QVBoxLayout>

#include <cstddef>
#include <exception>
#include <utility>

namespace
{
constexpr int EDITOR_MIN_WIDTH_PX = 900;
constexpr int EDITOR_MIN_HEIGHT_PX = 560;
constexpr int LIST_INITIAL_WIDTH_PX = 280;
constexpr int STATUS_CLEAR_MS = 6000;

/// Default "user bucket" priority for new drafts. Matches the
/// convention used elsewhere (probe stably sorts by `priority`, so
/// identical user priorities preserve file-name order).
constexpr int USER_TEMPLATE_DEFAULT_PRIORITY = 100;

/// Priority spinner bounds. Built-ins ship between 10 and 30; the
/// upper bound is generous so users can stage rare formats far
/// down the probe order without saturating.
constexpr int PRIORITY_MIN = 0;
constexpr int PRIORITY_MAX = 10000;

/// Horizontal spacing (px) between the auto-detect checkbox and
/// the priority row. Wide enough to read as two controls,
/// narrow enough to keep the form compact.
constexpr int FLAGS_ROW_SPACING_PX = 16;

/// Replace every character `RegexTemplateRegistry::SanitiseTemplateName`
/// would reject with an underscore, yielding a filename-safe name.
/// Used by `Duplicate` so a built-in like "Apache / nginx Combined
/// Log Format" seeds a name Save can accept without manual editing.
[[nodiscard]] QString MakeFilenameSafe(const QString &name)
{
    QString out;
    out.reserve(name.size());
    constexpr char16_t FIRST_PRINTABLE_ASCII = 0x20U;
    for (const QChar ch : name)
    {
        if (ch == QLatin1Char('/') || ch == QLatin1Char('\\') || ch == QLatin1Char(':') || ch == QLatin1Char('<') ||
            ch == QLatin1Char('>') || ch == QLatin1Char('"') || ch == QLatin1Char('|') || ch == QLatin1Char('?') ||
            ch == QLatin1Char('*') || ch.unicode() < FIRST_PRINTABLE_ASCII)
        {
            out.append(QLatin1Char('_'));
        }
        else
        {
            out.append(ch);
        }
    }
    // Trim trailing dots / spaces (the registry rejects those
    // too) so names like "MyTemplate. " round-trip cleanly.
    while (!out.isEmpty() && (out.endsWith(QLatin1Char('.')) || out.endsWith(QLatin1Char(' '))))
    {
        out.chop(1);
    }
    return out.trimmed();
}
} // namespace

RegexTemplatesEditor::RegexTemplatesEditor(RegexTemplateRegistry *registry, QWidget *parent)
    : QWidget(parent), mRegistry(registry)
{
    Q_ASSERT(registry != nullptr);
    setWindowFlags(Qt::Window);
    setWindowTitle(tr("Regex templates"));
    setMinimumSize(EDITOR_MIN_WIDTH_PX, EDITOR_MIN_HEIGHT_PX);

    // ------------------------------------------------------------
    // Toolbar (top row): list-side actions on the left, folder /
    // reload on the right, keeping the CRUD verbs next to the
    // list they operate on.
    // ------------------------------------------------------------
    auto *topBar = new QHBoxLayout();
    mNewButton = new QPushButton(tr("New template..."), this);
    mNewButton->setToolTip(tr("Start a blank user template draft. Save it (or pick another row) to commit; "
                              "discard by selecting another row."));
    mDuplicateButton = new QPushButton(tr("Duplicate selected"), this);
    mDuplicateButton->setToolTip(tr("Copy the selected entry's pattern / samples / priority into a new draft. "
                                    "Works for both built-in and user templates -- the duplicate becomes a user "
                                    "template once you Save."));
    mDeleteButton = new QPushButton(tr("Delete user template"), this);
    mDeleteButton->setToolTip(tr("Delete the selected user template's JSON file. Built-in templates cannot be "
                                 "deleted; create a same-named user template to shadow one instead."));
    mOpenFolderButton = new QPushButton(tr("Open templates folder"), this);
    mOpenFolderButton->setToolTip(tr("Reveal the user regex templates folder in the OS file manager."));
    mReloadButton = new QPushButton(tr("Reload from disk"), this);
    mReloadButton->setToolTip(tr("Re-scan the user templates folder. Use after editing a JSON file outside the app."));

    topBar->addWidget(mNewButton);
    topBar->addWidget(mDuplicateButton);
    topBar->addWidget(mDeleteButton);
    topBar->addStretch(1);
    topBar->addWidget(mOpenFolderButton);
    topBar->addWidget(mReloadButton);

    // ------------------------------------------------------------
    // Main split: list on the left, form on the right. Uses
    // `QSplitter` so the user can resize per screen / pattern
    // length.
    // ------------------------------------------------------------
    auto *splitter = new QSplitter(Qt::Horizontal, this);
    splitter->setChildrenCollapsible(false);

    mListWidget = new QListWidget(splitter);
    mListWidget->setSelectionMode(QAbstractItemView::SingleSelection);
    mListWidget->setUniformItemSizes(true);
    mListWidget->setMinimumWidth(LIST_INITIAL_WIDTH_PX / 2);
    mListWidget->setToolTip(tr("Every template in the merged catalog (built-ins from the binary + user files "
                               "from <AppData>/regex_templates/). The `(user)` badge marks files you can edit "
                               "in place; built-ins are read-only and must be duplicated to customise."));

    // Right-pane form. `QFormLayout` keeps the label column tight;
    // multi-line fields use `QPlainTextEdit` so they scroll on
    // tall patterns. A monospace font is pinned on regex / samples
    // — character classes are unreadable in a proportional face.
    auto *formContainer = new QWidget(splitter);
    auto *formLayout = new QFormLayout(formContainer);

    mNameEdit = new QLineEdit(formContainer);
    mNameEdit->setPlaceholderText(tr("Template name (filename basename + picker label)"));
    mNameEdit->setToolTip(tr("Human-readable picker label. Must be unique across the merged catalog; "
                             "becomes the JSON file's basename so the same sanitisation rules as a filename "
                             "apply (no path separators, no '..', no reserved Windows device names, etc.)."));
    formLayout->addRow(tr("Name:"), mNameEdit);

    mSourceLabel = new QLabel(formContainer);
    mSourceLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    mSourceLabel->setWordWrap(true);
    formLayout->addRow(tr("Source:"), mSourceLabel);

    mPatternEdit = new QPlainTextEdit(formContainer);
    mPatternEdit->setPlaceholderText(
        tr(R"(PCRE2 pattern with named capture groups, e.g. ^(?<Level>\w+) (?<Message>.*)$)")
    );
    mPatternEdit->setToolTip(tr("PCRE2 regex with `(?<Name>...)` named capture groups; each group becomes a "
                                "column. Avoid Oniguruma-only constructs (\\K, possessive quantifiers); "
                                "PCRE2 won't compile them."));
    mPatternEdit->setLineWrapMode(QPlainTextEdit::NoWrap);
    mPatternEdit->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    mPatternEdit->setTabChangesFocus(true);
    formLayout->addRow(tr("Pattern:"), mPatternEdit);

    mSampleLinesEdit = new QPlainTextEdit(formContainer);
    mSampleLinesEdit->setPlaceholderText(tr("One sample log line per row. Used by Validate to self-test the pattern."));
    mSampleLinesEdit->setToolTip(tr("One sample line per row. Validate will compile the pattern and check that "
                                    "every sample matches. Empty rows are skipped."));
    mSampleLinesEdit->setLineWrapMode(QPlainTextEdit::NoWrap);
    mSampleLinesEdit->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    mSampleLinesEdit->setTabChangesFocus(true);
    formLayout->addRow(tr("Sample lines:"), mSampleLinesEdit);

    auto *flagsRow = new QHBoxLayout();
    mAutoDetectCheck = new QCheckBox(tr("Participate in auto-detect"), formContainer);
    mAutoDetectCheck->setToolTip(tr("When checked, the template joins the parser's IsValid probe loop "
                                    "(in priority order). Uncheck for corporate / niche templates you want "
                                    "available in the picker but not in the auto-detect cost path."));
    flagsRow->addWidget(mAutoDetectCheck);
    flagsRow->addSpacing(FLAGS_ROW_SPACING_PX);
    flagsRow->addWidget(new QLabel(tr("Priority:"), formContainer));
    mPrioritySpin = new QSpinBox(formContainer);
    mPrioritySpin->setRange(PRIORITY_MIN, PRIORITY_MAX);
    mPrioritySpin->setValue(USER_TEMPLATE_DEFAULT_PRIORITY);
    mPrioritySpin->setToolTip(tr("Probe order: lower probes first. Built-ins are curated between 10 and 30 "
                                 "(specific first). User templates default to %1 (after every shipped built-in). "
                                 "Set to a small number to override a built-in's probe priority.")
                                  .arg(USER_TEMPLATE_DEFAULT_PRIORITY));
    flagsRow->addWidget(mPrioritySpin);
    flagsRow->addStretch(1);
    formLayout->addRow(QString{}, flagsRow);

    mDescriptionEdit = new QPlainTextEdit(formContainer);
    mDescriptionEdit->setPlaceholderText(tr("Optional. Describe the format the template parses: what software emits "
                                            "it, what the columns mean, known edge cases. For ports of upstream "
                                            "patterns, also cite the source file path and licence."));
    mDescriptionEdit->setToolTip(tr("Free-form, multi-line description of the format. Typically a sentence on the "
                                    "format itself plus an optional attribution line for ported templates (e.g. "
                                    "'Adapted from lnav src/formats/<file>.json (BSD-2-Clause)'). The licence-bundle "
                                    "generator scans this field for recognisable licence citations."));
    mDescriptionEdit->setTabChangesFocus(true);
    // Word-wrap so long descriptions stay readable without
    // making the user break lines manually.
    mDescriptionEdit->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    {
        // Shipped descriptions for upstream ports (AWS Classic ELB,
        // Cisco ASA, ...) run 4-6 wrapped lines, so floor at ~6
        // rows to fit without scrolling on first load. The default
        // Expanding vertical policy still shares extra space with
        // the Pattern and Sample fields.
        const QFontMetrics fm(mDescriptionEdit->font());
        constexpr int DESCRIPTION_MIN_ROWS = 6;
        const int frameHeight = mDescriptionEdit->frameWidth() * 2;
        mDescriptionEdit->setMinimumHeight((fm.height() * DESCRIPTION_MIN_ROWS) + frameHeight);
    }
    formLayout->addRow(tr("Description:"), mDescriptionEdit);

    // ------------------------------------------------------------
    // Per-form action row (Validate / Save / Revert), right-aligned
    // so Save+Revert sit next to the fields their state applies to.
    // ------------------------------------------------------------
    auto *formButtonsRow = new QHBoxLayout();
    mValidateButton = new QPushButton(tr("Validate"), formContainer);
    mValidateButton->setToolTip(
        tr("Compile the pattern and check every sample line matches. Reports first failure. "
           "Does not write to disk.")
    );
    mSaveButton = new QPushButton(tr("Save"), formContainer);
    mSaveButton->setToolTip(tr("Write the template to <AppData>/regex_templates/<name>.json and refresh the "
                               "registry (so the parser's auto-detect probe sees it immediately)."));
    mRevertButton = new QPushButton(tr("Revert"), formContainer);
    mRevertButton->setToolTip(tr("Discard unsaved edits and reload the form from the registry."));
    formButtonsRow->addStretch(1);
    formButtonsRow->addWidget(mValidateButton);
    formButtonsRow->addWidget(mRevertButton);
    formButtonsRow->addWidget(mSaveButton);
    formLayout->addRow(QString{}, formButtonsRow);

    splitter->addWidget(mListWidget);
    splitter->addWidget(formContainer);
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({LIST_INITIAL_WIDTH_PX, EDITOR_MIN_WIDTH_PX - LIST_INITIAL_WIDTH_PX});

    // ------------------------------------------------------------
    // Status / close row (bottom).
    // ------------------------------------------------------------
    mStatusLabel = new QLabel(this);
    mStatusLabel->setWordWrap(true);
    mStatusLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    mStatusClearTimer = new QTimer(this);
    mStatusClearTimer->setSingleShot(true);
    connect(mStatusClearTimer, &QTimer::timeout, mStatusLabel, &QLabel::clear);

    auto *bottomBar = new QHBoxLayout();
    bottomBar->addWidget(mStatusLabel, 1);
    auto *closeButton = new QPushButton(tr("Close"), this);
    closeButton->setToolTip(tr("Close the editor. Prompts to confirm if there are unsaved edits."));
    bottomBar->addWidget(closeButton);

    auto *layout = new QVBoxLayout(this);
    layout->addLayout(topBar);
    auto *divider = new QFrame(this);
    divider->setFrameShape(QFrame::HLine);
    divider->setFrameShadow(QFrame::Sunken);
    layout->addWidget(divider);
    layout->addWidget(splitter, 1);
    layout->addLayout(bottomBar);

    // ------------------------------------------------------------
    // Signal wiring.
    // ------------------------------------------------------------
    connect(mListWidget, &QListWidget::currentItemChanged, this, &RegexTemplatesEditor::OnSelectionChanged);
    connect(mNewButton, &QPushButton::clicked, this, &RegexTemplatesEditor::OnNewClicked);
    connect(mDuplicateButton, &QPushButton::clicked, this, &RegexTemplatesEditor::OnDuplicateClicked);
    connect(mSaveButton, &QPushButton::clicked, this, &RegexTemplatesEditor::OnSaveClicked);
    connect(mRevertButton, &QPushButton::clicked, this, &RegexTemplatesEditor::OnRevertClicked);
    connect(mDeleteButton, &QPushButton::clicked, this, &RegexTemplatesEditor::OnDeleteClicked);
    connect(mValidateButton, &QPushButton::clicked, this, &RegexTemplatesEditor::OnValidateClicked);
    connect(mOpenFolderButton, &QPushButton::clicked, this, &RegexTemplatesEditor::OnOpenFolderClicked);
    connect(mReloadButton, &QPushButton::clicked, this, &RegexTemplatesEditor::OnReloadClicked);
    connect(closeButton, &QPushButton::clicked, this, &QWidget::close);

    // Field-edit -> dirty bridge. `MarkDirty` is idempotent and
    // cheap; programmatic form fills are gated through
    // `mSuppressDirtySignals` (set by `LoadIntoForm`).
    connect(mNameEdit, &QLineEdit::textEdited, this, &RegexTemplatesEditor::OnFieldEdited);
    connect(mPatternEdit, &QPlainTextEdit::textChanged, this, &RegexTemplatesEditor::OnFieldEdited);
    connect(mSampleLinesEdit, &QPlainTextEdit::textChanged, this, &RegexTemplatesEditor::OnFieldEdited);
    connect(mAutoDetectCheck, &QCheckBox::toggled, this, &RegexTemplatesEditor::OnFieldEdited);
    connect(mPrioritySpin, QOverload<int>::of(&QSpinBox::valueChanged), this, &RegexTemplatesEditor::OnFieldEdited);
    connect(mDescriptionEdit, &QPlainTextEdit::textChanged, this, &RegexTemplatesEditor::OnFieldEdited);

    // External registry changes (Reload from disk, a save from
    // another widget, ...) repopulate the list. Selection is
    // preserved by name when possible.
    connect(mRegistry, &RegexTemplateRegistry::templatesChanged, this, &RegexTemplatesEditor::RefreshList);

    RefreshList();
    // Default to selecting the first entry so the form isn't
    // blank on open. `RefreshList` already restored a prior
    // selection if one existed. If the list is empty (very
    // unusual — would need every built-in stripped) fall back to
    // "New draft" mode so the form has something to show.
    if (mListWidget->count() > 0 && mListWidget->currentItem() == nullptr)
    {
        mListWidget->setCurrentRow(0);
    }
    else if (mListWidget->count() == 0)
    {
        OnNewClicked();
    }
}

void RegexTemplatesEditor::RefreshList()
{
    const QString preserveName = mCurrentName;
    // Preserve any in-flight draft so a refresh that misses the
    // current row doesn't silently throw the user's typing away.
    // (The confirmation prompt only fires on user-driven actions;
    // an external `templatesChanged()` must not surprise us.)
    if (mDirty && !mIsNewDraft)
    {
        // A user file appearing on disk could flip built-in/user
        // for the row we're editing. Leave the form alone; the
        // next user click will trigger `ConfirmDiscardEdits`.
    }

    {
        const QSignalBlocker blocker(mListWidget);
        mListWidget->clear();
        const QList<RegexTemplateRegistry::Listing> entries = mRegistry->Available();
        for (const RegexTemplateRegistry::Listing &row : entries)
        {
            auto *item = new QListWidgetItem(FormatListLabel(row.name, row.fromUser, row.autoDetect), mListWidget);
            item->setData(Qt::UserRole, row.name);
            // Tooltip echoes the badge plus priority / source so
            // users can inspect a row without opening the form.
            item->setToolTip(
                tr("%1\nSource: %2\nPriority: %3\nAuto-detect: %4")
                    .arg(row.name, row.fromUser ? tr("user (editable)") : tr("built-in (read-only)"))
                    .arg(row.priority)
                    .arg(row.autoDetect ? tr("yes") : tr("no (manual only)"))
            );
        }
    }

    // Restore selection by name if the entry still exists. If it
    // vanished (deleted, etc.), fall back to the first row —
    // which clears any unsaved-draft state via `OnSelectionChanged`.
    if (!preserveName.isEmpty())
    {
        for (int i = 0; i < mListWidget->count(); ++i)
        {
            if (mListWidget->item(i)->data(Qt::UserRole).toString() == preserveName)
            {
                mListWidget->setCurrentRow(i);
                return;
            }
        }
    }
    if (mIsNewDraft)
    {
        // Preserve the draft across an external refresh; nothing
        // in the list is selected while it's live.
        mListWidget->clearSelection();
    }
}

void RegexTemplatesEditor::OnSelectionChanged()
{
    const QListWidgetItem *item = mListWidget->currentItem();
    const QString name = (item == nullptr) ? QString{} : item->data(Qt::UserRole).toString();

    // No-op when the selection didn't change (Qt also fires
    // `currentItemChanged` on programmatic list rebuilds).
    if (name == mCurrentName && !mIsNewDraft)
    {
        return;
    }

    if (!ConfirmDiscardEdits())
    {
        // Revert the selection without re-entering this slot.
        const QSignalBlocker blocker(mListWidget);
        if (mCurrentName.isEmpty())
        {
            mListWidget->setCurrentItem(nullptr);
        }
        else
        {
            for (int i = 0; i < mListWidget->count(); ++i)
            {
                if (mListWidget->item(i)->data(Qt::UserRole).toString() == mCurrentName)
                {
                    mListWidget->setCurrentRow(i);
                    break;
                }
            }
        }
        return;
    }

    mIsNewDraft = false;
    LoadIntoForm(name);
}

void RegexTemplatesEditor::OnFieldEdited()
{
    if (mSuppressDirtySignals)
    {
        return;
    }
    MarkDirty();
}

void RegexTemplatesEditor::OnNewClicked()
{
    if (!ConfirmDiscardEdits())
    {
        return;
    }
    {
        const QSignalBlocker blocker(mListWidget);
        mListWidget->setCurrentItem(nullptr);
    }
    mCurrentName.clear();
    mIsNewDraft = true;

    // Seed the form with sensible defaults so the user only has
    // to fill name + pattern.
    mSuppressDirtySignals = true;
    mNameEdit->clear();
    mPatternEdit->clear();
    mSampleLinesEdit->clear();
    mAutoDetectCheck->setChecked(true);
    mPrioritySpin->setValue(USER_TEMPLATE_DEFAULT_PRIORITY);
    mDescriptionEdit->clear();
    mSourceLabel->setText(tr("New user template (not yet saved)"));
    mSuppressDirtySignals = false;

    mNameEdit->setReadOnly(false);
    mPatternEdit->setReadOnly(false);
    mSampleLinesEdit->setReadOnly(false);
    mAutoDetectCheck->setEnabled(true);
    mPrioritySpin->setEnabled(true);
    mDescriptionEdit->setReadOnly(false);
    mDeleteButton->setEnabled(false);
    mDuplicateButton->setEnabled(false);

    // Blank drafts start "dirty" so Save enables immediately.
    // A missing name / pattern is caught in `OnSaveClicked`.
    MarkDirty();

    mNameEdit->setFocus();
    ShowStatus(tr("Authoring a new user template. Save to commit."));
}

void RegexTemplatesEditor::OnDuplicateClicked()
{
    if (mCurrentName.isEmpty() && !mIsNewDraft)
    {
        return;
    }
    if (!ConfirmDiscardEdits())
    {
        return;
    }

    // Snapshot the form (not the registry) so half-typed edits on
    // a user template can still seed the duplicate.
    QString sourceName;
    loglib::RegexTemplate base;
    try
    {
        base = GatherForm();
        sourceName = QString::fromStdString(base.name);
    }
    catch (const std::exception &)
    {
        // Form was malformed; fall back to the registry version.
        if (!mCurrentName.isEmpty())
        {
            const auto loaded = mRegistry->Load(mCurrentName);
            if (loaded.has_value())
            {
                base = *loaded;
                sourceName = mCurrentName;
            }
        }
    }

    // Find a unique "<name>-copy[ N]" so repeated clicks don't
    // collide. Same convention as the themes editor. Base name
    // runs through `MakeFilenameSafe` so a built-in like "Apache
    // / nginx Combined Log Format" becomes "Apache _ nginx ...
    // -copy" — a name Save can accept without throwing.
    const QString safeSource = MakeFilenameSafe(sourceName);
    const QString seed = safeSource.isEmpty() ? tr("Untitled") : safeSource;
    QString candidate = seed + tr("-copy");
    int suffix = 2;
    while (mRegistry->Load(candidate).has_value())
    {
        candidate = seed + tr("-copy ") + QString::number(suffix);
        ++suffix;
    }

    base.name = candidate.toStdString();
    // Duplicates always become user templates. Default to
    // participating in auto-detect at the user-bucket priority;
    // the user can flip either for a manual-only override.
    base.autoDetect = true;
    base.priority = USER_TEMPLATE_DEFAULT_PRIORITY;
    // Leave `description` alone: an inherited attribution line
    // gives the user something to build on, and an empty one
    // stays empty so they can write their own.

    {
        const QSignalBlocker blocker(mListWidget);
        mListWidget->setCurrentItem(nullptr);
    }
    mCurrentName.clear();
    mIsNewDraft = true;

    mSuppressDirtySignals = true;
    mNameEdit->setText(candidate);
    mPatternEdit->setPlainText(QString::fromStdString(base.pattern));
    {
        QString samples;
        for (const std::string &line : base.sampleLines)
        {
            if (!samples.isEmpty())
            {
                samples += QLatin1Char('\n');
            }
            samples += QString::fromStdString(line);
        }
        mSampleLinesEdit->setPlainText(samples);
    }
    mAutoDetectCheck->setChecked(base.autoDetect);
    mPrioritySpin->setValue(base.priority);
    mDescriptionEdit->setPlainText(QString::fromStdString(base.description));
    mSourceLabel->setText(tr("Duplicating \"%1\" -- pick a name and Save").arg(sourceName));
    mSuppressDirtySignals = false;

    mNameEdit->setReadOnly(false);
    mPatternEdit->setReadOnly(false);
    mSampleLinesEdit->setReadOnly(false);
    mAutoDetectCheck->setEnabled(true);
    mPrioritySpin->setEnabled(true);
    mDescriptionEdit->setReadOnly(false);
    mDeleteButton->setEnabled(false);
    mDuplicateButton->setEnabled(false);

    MarkDirty();
    mNameEdit->setFocus();
    mNameEdit->selectAll();
    ShowStatus(tr("Duplicated to \"%1\". Edit and Save to commit.").arg(candidate));
}

void RegexTemplatesEditor::OnSaveClicked()
{
    loglib::RegexTemplate tmpl;
    try
    {
        tmpl = GatherForm();
    }
    catch (const std::exception &ex)
    {
        ShowStatus(QString::fromUtf8(ex.what()), /*isError=*/true);
        return;
    }

    // Compile-check before writing so the saved file is always
    // valid PCRE2. Front-loaded here (not only in Validate) so a
    // Save without a prior Validate can't put a broken pattern
    // on disk.
    std::string regexError;
    if (!loglib::ValidateRegexPattern(tmpl.pattern, regexError))
    {
        ShowStatus(
            tr("Pattern does not compile: %1").arg(QString::fromStdString(regexError)),
            /*isError=*/true
        );
        return;
    }

    const QString name = QString::fromStdString(tmpl.name);
    if (name.isEmpty())
    {
        // Save is the only consumer that requires a name (as the
        // file basename); Validate and Duplicate don't, so the
        // check lives here rather than in `GatherForm`.
        ShowStatus(tr("Template name must not be empty."), /*isError=*/true);
        mNameEdit->setFocus();
        return;
    }

    // Collision check for new drafts: refuse to silently overwrite
    // an existing user template. Editing an existing user template
    // is fine (that's the edit flow), and overwriting a built-in
    // name intentionally shadows it.
    if (mIsNewDraft && mRegistry->IsUserTemplate(name))
    {
        const auto reply = QMessageBox::question(
            this,
            tr("Save regex template"),
            tr("A user template named \"%1\" already exists. Overwrite?").arg(name),
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No
        );
        if (reply != QMessageBox::Yes)
        {
            return;
        }
    }

    // Transition out of draft mode *before* the save so
    // `RefreshList` (fired synchronously from `SaveUserTemplate`'s
    // `templatesChanged()`) sees the just-saved name in
    // `mCurrentName` and restores selection. Doing it after would
    // leave `mCurrentName` empty during the refresh and the new
    // row would land unselected.
    const bool wasNewDraft = mIsNewDraft;
    const QString previousName = mCurrentName;
    mIsNewDraft = false;
    mCurrentName = name;
    try
    {
        mRegistry->SaveUserTemplate(name, std::move(tmpl));
    }
    catch (const std::exception &ex)
    {
        // Roll back so a failed save doesn't leave the editor
        // pretending the draft was committed. Form contents are
        // untouched so the user's typing stays intact.
        mIsNewDraft = wasNewDraft;
        mCurrentName = previousName;
        ShowStatus(QString::fromUtf8(ex.what()), /*isError=*/true);
        return;
    }

    MarkClean();
    ShowStatus(tr("Saved \"%1\".").arg(name));
}

void RegexTemplatesEditor::OnRevertClicked()
{
    if (mIsNewDraft)
    {
        mIsNewDraft = false;
        mCurrentName.clear();
        LoadIntoForm(QString{});
        // Re-select the first row so the user has somewhere to
        // pick up from after discarding the draft.
        if (mListWidget->count() > 0)
        {
            mListWidget->setCurrentRow(0);
        }
        ShowStatus(tr("Discarded the unsaved draft."));
        return;
    }
    LoadIntoForm(mCurrentName);
    ShowStatus(tr("Reverted to the on-disk version."));
}

void RegexTemplatesEditor::OnDeleteClicked()
{
    if (mCurrentName.isEmpty() || !mRegistry->IsUserTemplate(mCurrentName))
    {
        return;
    }
    const auto reply = QMessageBox::question(
        this,
        tr("Delete user template"),
        tr("Delete the user template \"%1\"?\n\nThe file under <AppData>/regex_templates/ will be removed. "
           "If a built-in template shares the same name, it will reappear in the picker (unshadowed).")
            .arg(mCurrentName),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No
    );
    if (reply != QMessageBox::Yes)
    {
        return;
    }

    try
    {
        mRegistry->DeleteUserTemplate(mCurrentName);
    }
    catch (const std::exception &ex)
    {
        ShowStatus(QString::fromUtf8(ex.what()), /*isError=*/true);
        return;
    }

    ShowStatus(tr("Deleted \"%1\".").arg(mCurrentName));
    mDirty = false;
    mCurrentName.clear();
    // `templatesChanged()` re-runs RefreshList; we just clear the
    // form here.
    LoadIntoForm(QString{});
    if (mListWidget->count() > 0)
    {
        mListWidget->setCurrentRow(0);
    }
}

void RegexTemplatesEditor::OnValidateClicked()
{
    loglib::RegexTemplate tmpl;
    try
    {
        tmpl = GatherForm();
    }
    catch (const std::exception &ex)
    {
        ShowStatus(QString::fromUtf8(ex.what()), /*isError=*/true);
        return;
    }

    std::string regexError;
    if (!loglib::ValidateRegexPattern(tmpl.pattern, regexError))
    {
        ShowStatus(
            tr("Pattern does not compile: %1").arg(QString::fromStdString(regexError)),
            /*isError=*/true
        );
        return;
    }

    if (tmpl.sampleLines.empty())
    {
        ShowStatus(tr("Pattern compiles. No sample lines to self-test against."));
        return;
    }

    // Walk the samples through the compiled template via the
    // library helper. We only need pass/fail, not the groups.
    std::size_t passed = 0;
    std::size_t failedIndex = 0;
    bool sawFailure = false;
    for (std::size_t i = 0; i < tmpl.sampleLines.size(); ++i)
    {
        if (loglib::PatternMatchesLine(tmpl.pattern, tmpl.sampleLines[i]))
        {
            ++passed;
        }
        else if (!sawFailure)
        {
            sawFailure = true;
            failedIndex = i;
        }
    }
    if (sawFailure)
    {
        ShowStatus(
            tr("Pattern compiles. %1/%2 samples matched. First failure (sample line #%3): %4")
                .arg(passed)
                .arg(tmpl.sampleLines.size())
                .arg(failedIndex + 1)
                .arg(QString::fromStdString(tmpl.sampleLines[failedIndex])),
            /*isError=*/true
        );
        return;
    }
    ShowStatus(tr("Pattern compiles. All %1 sample lines matched.").arg(tmpl.sampleLines.size()));
}

void RegexTemplatesEditor::OnOpenFolderClicked()
{
    if (!RegexTemplateRegistry::RevealUserTemplatesDir())
    {
        ShowStatus(
            tr("Could not open the user templates folder. The folder is at: %1")
                .arg(RegexTemplateRegistry::UserTemplatesDir().absolutePath()),
            /*isError=*/true
        );
    }
}

void RegexTemplatesEditor::OnReloadClicked()
{
    if (!ConfirmDiscardEdits())
    {
        return;
    }
    mRegistry->Reload();
    // `templatesChanged()` already fired RefreshList from inside
    // `Reload` — just acknowledge.
    ShowStatus(tr("Reloaded regex templates from disk."));
}

QString RegexTemplatesEditor::FormatListLabel(const QString &name, bool fromUser, bool autoDetect)
{
    QString label = name;
    if (fromUser)
    {
        label = tr("%1 (user)").arg(name);
    }
    if (!autoDetect)
    {
        label = tr("%1 (manual only)").arg(label);
    }
    return label;
}

void RegexTemplatesEditor::LoadIntoForm(const QString &name)
{
    mCurrentName = name;
    mSuppressDirtySignals = true;

    if (name.isEmpty())
    {
        mNameEdit->clear();
        mPatternEdit->clear();
        mSampleLinesEdit->clear();
        mAutoDetectCheck->setChecked(true);
        mPrioritySpin->setValue(USER_TEMPLATE_DEFAULT_PRIORITY);
        mDescriptionEdit->clear();
        mSourceLabel->clear();

        mNameEdit->setReadOnly(true);
        mPatternEdit->setReadOnly(true);
        mSampleLinesEdit->setReadOnly(true);
        mAutoDetectCheck->setEnabled(false);
        mPrioritySpin->setEnabled(false);
        mDescriptionEdit->setReadOnly(true);
        mDeleteButton->setEnabled(false);
        mDuplicateButton->setEnabled(false);
        MarkClean();
        mSuppressDirtySignals = false;
        return;
    }

    const auto loaded = mRegistry->Load(name);
    const bool isUser = mRegistry->IsUserTemplate(name);

    if (!loaded.has_value())
    {
        // The list claimed this template exists but the load
        // failed (file gone between scans, malformed JSON, ...).
        // Clear the form and let the status line report it.
        mNameEdit->clear();
        mPatternEdit->clear();
        mSampleLinesEdit->clear();
        mDescriptionEdit->clear();
        mSourceLabel->setText(tr("Could not load \"%1\" (try Reload from disk)").arg(name));
        mNameEdit->setReadOnly(true);
        mPatternEdit->setReadOnly(true);
        mSampleLinesEdit->setReadOnly(true);
        mAutoDetectCheck->setEnabled(false);
        mPrioritySpin->setEnabled(false);
        mDescriptionEdit->setReadOnly(true);
        mDeleteButton->setEnabled(false);
        mDuplicateButton->setEnabled(false);
        MarkClean();
        mSuppressDirtySignals = false;
        return;
    }

    const loglib::RegexTemplate &t = *loaded;
    mNameEdit->setText(QString::fromStdString(t.name));
    mPatternEdit->setPlainText(QString::fromStdString(t.pattern));
    {
        QString samples;
        for (const std::string &line : t.sampleLines)
        {
            if (!samples.isEmpty())
            {
                samples += QLatin1Char('\n');
            }
            samples += QString::fromStdString(line);
        }
        mSampleLinesEdit->setPlainText(samples);
    }
    mAutoDetectCheck->setChecked(t.autoDetect);
    mPrioritySpin->setValue(t.priority);
    mDescriptionEdit->setPlainText(QString::fromStdString(t.description));
    mSourceLabel->setText(isUser ? tr("User template (editable in place)") : tr("Built-in template (read-only)"));

    // Built-ins are read-only — the bytes live in the binary, so
    // editing here would be misleading. Duplicate is the path to
    // customise.
    const bool editable = isUser;
    mNameEdit->setReadOnly(!editable);
    mPatternEdit->setReadOnly(!editable);
    mSampleLinesEdit->setReadOnly(!editable);
    mAutoDetectCheck->setEnabled(editable);
    mPrioritySpin->setEnabled(editable);
    mDescriptionEdit->setReadOnly(!editable);
    mDeleteButton->setEnabled(editable);
    mDuplicateButton->setEnabled(true);

    MarkClean();
    mSuppressDirtySignals = false;
}

loglib::RegexTemplate RegexTemplatesEditor::GatherForm() const
{
    loglib::RegexTemplate t;
    // Name is collected as-is; filename safety (slashes, reserved
    // Win32 device names, etc.) is enforced by `SaveUserTemplate`
    // because it only matters when the name is about to become a
    // file. Validate / Duplicate don't care — "Apache / nginx ..."
    // is a valid name to compile-check, and Duplicate overwrites
    // it anyway. Pattern emptiness *is* checked here because every
    // consumer needs a pattern to do anything useful.
    const QString name = mNameEdit->text().trimmed();
    // Trim the pattern (matches `NetworkStreamDialog::Accepted`).
    // `QPlainTextEdit` accumulates stray trailing whitespace from
    // paste, and persisting that would cause a pointless diff on
    // every save round-trip and a mismatch with the same pattern
    // typed into the network-stream dialog (which trims). No
    // shipped pattern relies on leading or trailing literal
    // whitespace — anchors are `^...$`, real whitespace lives
    // inside `\s+` / `\s*` — so trimming is safe.
    const QString pattern = mPatternEdit->toPlainText().trimmed();
    if (pattern.isEmpty())
    {
        throw std::runtime_error("Pattern must not be empty.");
    }

    t.name = name.toStdString();
    t.pattern = pattern.toStdString();

    const QStringList lines = mSampleLinesEdit->toPlainText().split(QLatin1Char('\n'));
    t.sampleLines.reserve(static_cast<std::size_t>(lines.size()));
    for (const QString &line : lines)
    {
        // Skip empty rows — trailing newlines would otherwise
        // produce phantom samples that Validate always fails.
        if (line.isEmpty())
        {
            continue;
        }
        t.sampleLines.push_back(line.toStdString());
    }

    t.autoDetect = mAutoDetectCheck->isChecked();
    t.priority = mPrioritySpin->value();
    t.description = mDescriptionEdit->toPlainText().toStdString();
    return t;
}

void RegexTemplatesEditor::MarkDirty()
{
    mDirty = true;
    mSaveButton->setEnabled(IsCurrentEditable());
    mRevertButton->setEnabled(true);
}

void RegexTemplatesEditor::MarkClean()
{
    mDirty = false;
    mSaveButton->setEnabled(false);
    mRevertButton->setEnabled(false);
}

bool RegexTemplatesEditor::IsCurrentEditable() const
{
    if (mIsNewDraft)
    {
        return true;
    }
    return !mCurrentName.isEmpty() && mRegistry->IsUserTemplate(mCurrentName);
}

void RegexTemplatesEditor::ShowStatus(const QString &message, bool isError)
{
    mStatusLabel->setText(message);
    // Subtle visual differentiation: errors in a warning hue,
    // info in the regular palette. Only sets foreground so the
    // stylesheet stays theme-respecting.
    mStatusLabel->setStyleSheet(isError ? QStringLiteral("color: #b00020;") : QString{});
    if (message.isEmpty())
    {
        mStatusClearTimer->stop();
        return;
    }
    mStatusClearTimer->start(STATUS_CLEAR_MS);
}

bool RegexTemplatesEditor::ConfirmDiscardEdits()
{
    if (!mDirty)
    {
        return true;
    }
    const auto reply = QMessageBox::question(
        this,
        tr("Discard changes?"),
        tr("You have unsaved edits to \"%1\". Discard them?")
            .arg(mIsNewDraft ? tr("(new draft)") : mCurrentName),
        QMessageBox::Discard | QMessageBox::Cancel,
        QMessageBox::Cancel
    );
    if (reply != QMessageBox::Discard)
    {
        return false;
    }
    MarkClean();
    return true;
}

void RegexTemplatesEditor::closeEvent(QCloseEvent *event)
{
    if (!ConfirmDiscardEdits())
    {
        event->ignore();
        return;
    }
    QWidget::closeEvent(event);
}
