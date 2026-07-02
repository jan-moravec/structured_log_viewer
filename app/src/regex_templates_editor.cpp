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

/// Sentinel "user bucket" priority shown to new drafts. Matches
/// the convention used elsewhere (the registry's user templates
/// land in the same bucket; the library probe stably sorts by
/// `priority` so identical user priorities preserve file-name order).
constexpr int USER_TEMPLATE_DEFAULT_PRIORITY = 100;

/// Priority spinner bounds. Built-ins ship between 10 and 30; the
/// upper bound is generous so users can stage rare formats far down
/// the probe order without saturating the spinner.
constexpr int PRIORITY_MIN = 0;
constexpr int PRIORITY_MAX = 10000;

/// Horizontal padding between the auto-detect checkbox and the
/// priority row, in pixels. Big enough to read as two logical
/// controls (not one), small enough to keep the form compact.
constexpr int FLAGS_ROW_SPACING_PX = 16;

/// Replace every character `RegexTemplateRegistry::SanitiseTemplateName`
/// would reject with an underscore so the result is filename-safe.
/// Used by `Duplicate` to seed a name a built-in's
/// `"Apache / nginx Combined Log Format"` flows through without
/// the user having to manually edit the slashes out before Save.
/// Conservative: matches the registry's blacklist exactly (no
/// extra cleverness like collapsing runs of underscores), so the
/// suggested name stays readable.
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
    // Trim trailing dots / spaces (the registry also rejects
    // those) so a name like "MyTemplate. " round-trips cleanly.
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
    // Toolbar (top row) -- list-side actions on the left, folder /
    // reload on the right. Keeps the heavy CRUD verbs (New,
    // Duplicate, Delete) close to the list they operate on.
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
    // Main split: list on the left, form on the right. `QSplitter`
    // so the user can resize per their screen / pattern length.
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
    // multi-line fields (pattern, samples) use `QPlainTextEdit` so
    // they can scroll on tall patterns. A monospace font is pinned
    // on the regex / samples fields because regex character classes
    // are unreadable in a proportional face.
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
    // forcing the user to break lines manually.
    mDescriptionEdit->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    {
        // The shipped descriptions for upstream-ported templates
        // (AWS Classic ELB, Cisco ASA, ...) are 4-6 wrapped lines,
        // so floor the field at ~6 rows so they fit without
        // scrolling on first load. The widget keeps its default
        // Expanding vertical size policy, so it shares any extra
        // vertical space with Pattern and Sample lines the same
        // way they share it with each other when the window
        // grows.
        const QFontMetrics fm(mDescriptionEdit->font());
        constexpr int DESCRIPTION_MIN_ROWS = 6;
        const int frameHeight = mDescriptionEdit->frameWidth() * 2;
        mDescriptionEdit->setMinimumHeight((fm.height() * DESCRIPTION_MIN_ROWS) + frameHeight);
    }
    formLayout->addRow(tr("Description:"), mDescriptionEdit);

    // ------------------------------------------------------------
    // Per-form action row (Validate / Save / Revert). Right-aligned
    // so the Save+Revert pair sits next to the buttons their state
    // applies to.
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
    // cheap; rebuilding the registry connections is what's gated
    // through `mSuppressDirtySignals` (set by `LoadIntoForm`).
    connect(mNameEdit, &QLineEdit::textEdited, this, &RegexTemplatesEditor::OnFieldEdited);
    connect(mPatternEdit, &QPlainTextEdit::textChanged, this, &RegexTemplatesEditor::OnFieldEdited);
    connect(mSampleLinesEdit, &QPlainTextEdit::textChanged, this, &RegexTemplatesEditor::OnFieldEdited);
    connect(mAutoDetectCheck, &QCheckBox::toggled, this, &RegexTemplatesEditor::OnFieldEdited);
    connect(mPrioritySpin, QOverload<int>::of(&QSpinBox::valueChanged), this, &RegexTemplatesEditor::OnFieldEdited);
    connect(mDescriptionEdit, &QPlainTextEdit::textChanged, this, &RegexTemplatesEditor::OnFieldEdited);

    // External registry changes (Reload from disk, a different
    // widget saving a template, ...) repopulate the list. The
    // current selection is preserved by name when possible.
    connect(mRegistry, &RegexTemplateRegistry::templatesChanged, this, &RegexTemplatesEditor::RefreshList);

    RefreshList();
    // Default to selecting the first entry so the form isn't blank
    // on open. `RefreshList` already restored a selection if any
    // exists, but in the empty-list case (no entries at all -- very
    // unusual but possible if every built-in were ever stripped)
    // we still want the form in a sensible "New draft" mode.
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
    // Stash any in-flight draft so a refresh that misses the
    // current row doesn't silently throw away the user's typing.
    // (The Confirmation prompt only fires on user-driven actions;
    // an external `templatesChanged()` mustn't surprise us.)
    if (mDirty && !mIsNewDraft)
    {
        // Built-in / user toggle could have flipped if a user file
        // appeared on disk that shadows the built-in we're editing.
        // Leave the form alone: the next user click will trigger
        // `ConfirmDiscardEdits` and they can react then.
    }

    {
        const QSignalBlocker blocker(mListWidget);
        mListWidget->clear();
        const QList<RegexTemplateRegistry::Listing> entries = mRegistry->Available();
        for (const RegexTemplateRegistry::Listing &row : entries)
        {
            auto *item = new QListWidgetItem(FormatListLabel(row.name, row.fromUser, row.autoDetect), mListWidget);
            item->setData(Qt::UserRole, row.name);
            // Tooltip with the same metadata as the badge so users
            // can verify priority / source without opening the form.
            item->setToolTip(
                tr("%1\nSource: %2\nPriority: %3\nAuto-detect: %4")
                    .arg(row.name, row.fromUser ? tr("user (editable)") : tr("built-in (read-only)"))
                    .arg(row.priority)
                    .arg(row.autoDetect ? tr("yes") : tr("no (manual only)"))
            );
        }
    }

    // Restore selection by name if possible. If the previously
    // selected entry vanished (user deleted it, etc.), fall back
    // to selecting the first row -- which clears any unsaved-draft
    // state via `OnSelectionChanged`.
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
        // Keep the draft alive across an external refresh; nothing
        // in the list is selected.
        mListWidget->clearSelection();
    }
}

void RegexTemplatesEditor::OnSelectionChanged()
{
    QListWidgetItem *item = mListWidget->currentItem();
    const QString name = (item == nullptr) ? QString{} : item->data(Qt::UserRole).toString();

    // No-op when the selection didn't actually change (Qt fires
    // `currentItemChanged` on programmatic list rebuilds too).
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

    // A blank draft starts "dirty" so the Save button is enabled
    // immediately; the user has to type a name + pattern before
    // it'll actually commit (validated in `OnSaveClicked`).
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

    // Snapshot the form (not the registry) so a half-typed edit on
    // a user template still serves as the duplication base.
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
    // collide. Same convention as the themes editor. The base name
    // runs through `MakeFilenameSafe` so duplicating a built-in
    // like "Apache / nginx Combined Log Format" produces a name
    // that Save can actually accept ("Apache _ nginx Combined Log
    // Format-copy") instead of throwing on the first save attempt.
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
    // Duplicates become user templates regardless of the source.
    // Default to participating in auto-detect at the user bucket
    // priority; the user can flip those if they want a manual-only
    // override.
    base.autoDetect = true;
    base.priority = USER_TEMPLATE_DEFAULT_PRIORITY;
    // Leave `description` alone on duplicate: an empty one stays
    // empty so the user can write their own, while a description
    // inherited from the source (built-in attribution line, etc.)
    // is preserved so the user has something to build on.

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
    // valid PCRE2. Front-loaded here (rather than only in
    // `OnValidateClicked`) so a `Save` without a prior `Validate`
    // can't smuggle a broken pattern onto disk.
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
        // Save is the only consumer that needs a name (the file
        // basename); Validate / Duplicate don't, so the check
        // lives here rather than in `GatherForm`.
        ShowStatus(tr("Template name must not be empty."), /*isError=*/true);
        mNameEdit->setFocus();
        return;
    }

    // Collision check: when authoring a brand-new draft, refuse to
    // silently overwrite an existing user template (the user may
    // not realise there's already a file). Editing an existing
    // user template is allowed (that's the whole point of the
    // edit flow); overwriting a built-in name shadows it
    // intentionally.
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

    // Transition out of draft mode *before* the save call so that
    // `RefreshList` (fired synchronously from
    // `SaveUserTemplate`'s `templatesChanged()` signal) sees the
    // just-saved name in `mCurrentName` and restores the row
    // selection. Doing it after the save leaves `mCurrentName`
    // empty during the refresh, so the new row lands unselected
    // and the user has to click it manually.
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
        // Roll back so the failed save doesn't leave the editor
        // pretending the draft is committed. The form contents
        // are untouched; the user's typing stays intact.
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
        // start from after discarding the draft.
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
    // `templatesChanged()` re-runs RefreshList; we just need to
    // make sure the form clears.
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
    // helper the library already exposes for the same purpose.
    // `RegexLineMatch` returns a match struct; we don't need the
    // groups here, just success/failure.
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
    // `Reload`. Just acknowledge.
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
        // The list said this template exists but the load failed
        // (file disappeared between scans, malformed JSON, ...).
        // Clear the form and let the status line carry the news.
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

    // Built-ins are read-only -- the bytes live in the binary, so
    // editing the form would be misleading. The Duplicate button
    // is the path to customise.
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
    // file. Validate and Duplicate also call this helper but
    // genuinely don't care -- "Apache / nginx Combined Log Format"
    // is a perfectly valid template name to compile-check, and the
    // Duplicate path is going to overwrite the name anyway. Pattern
    // emptiness *is* checked here because every consumer (Validate
    // / Duplicate / Save) needs a pattern to do anything useful.
    const QString name = mNameEdit->text().trimmed();
    // Trim the pattern to match `NetworkStreamDialog::Accepted`:
    // `QPlainTextEdit` happily accumulates a stray trailing
    // newline / spaces from paste operations, and persisting that
    // whitespace onto disk causes both a pointless diff on every
    // save round-trip *and* a mismatch against the same pattern
    // typed into the network-stream dialog (which does trim). No
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
        // Empty rows are skipped: trailing newlines in the editor
        // would otherwise produce phantom empty samples that
        // Validate would always fail.
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
    // Subtle visual differentiation: error rows in a warning hue,
    // success / info rows in the regular palette. Done via stylesheet
    // (cheap, theme-respecting because we only set foreground).
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
