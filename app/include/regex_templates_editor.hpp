#pragma once

#include "loglib/regex_templates.hpp"

#include <QWidget>

class QCheckBox;
class QCloseEvent;
class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class QTimer;
class RegexTemplateRegistry;

/// Modeless editor for the merged regex-template catalog. Owns the
/// list view (every built-in + user template from the merged
/// registry) and an inline form for the selected entry's fields
/// (`name`, `pattern`, `sampleLines`, `autoDetect`, `priority`,
/// `description`). Built-ins are read-only (their bytes live in the
/// binary; the `Duplicate` button is the path to customise them);
/// user templates can be created, edited, validated, saved, and
/// deleted in-place.
///
/// Opened from `MainWindow` via `Settings -> Regex templates...`.
/// Constructed lazily (`MainWindow` keeps a `QPointer` so the
/// second click raises an existing instance); deleted-on-close is
/// **not** set so the dialog state (current selection, half-typed
/// pattern) survives close/reopen, mirroring `PreferencesEditor`.
///
/// The dialog is intentionally programmatically constructed (not
/// from a `.ui` file): the layout is small and the conditional
/// "built-in vs user" enablement is awkward to express in static
/// XML.
class RegexTemplatesEditor : public QWidget
{
    Q_OBJECT

public:
    /// @p registry is the merged regex-template catalog the editor
    /// reads / writes through. Must outlive this widget (lives in
    /// `main()` for the production window). Passing nullptr is a
    /// programming error: the editor has nothing to manage without
    /// a registry.
    explicit RegexTemplatesEditor(RegexTemplateRegistry *registry, QWidget *parent = nullptr);

    /// Repopulate the list (after an external `registry->Reload()`
    /// or a `templatesChanged()` notification). Preserves the
    /// current selection by name if the entry still exists.
    void RefreshList();

protected:
    /// Warn before discarding unsaved edits when the user closes
    /// the window with Esc / X / Alt+F4.
    void closeEvent(QCloseEvent *event) override;

private slots:
    void OnSelectionChanged();
    void OnFieldEdited();
    void OnNewClicked();
    void OnDuplicateClicked();
    void OnSaveClicked();
    void OnRevertClicked();
    void OnDeleteClicked();
    void OnValidateClicked();
    void OnOpenFolderClicked();
    void OnReloadClicked();

private:
    /// Build the per-row label shown in the list. Includes the
    /// `(user)` / `(manual only)` badges so the source of each
    /// entry is visible without opening the right-pane form.
    [[nodiscard]] static QString FormatListLabel(const QString &name, bool fromUser, bool autoDetect);

    /// Populate the right-pane form from the registry entry named
    /// @p name. Empty name clears the form (e.g. after Delete or
    /// during `New template...`). Disables the editor controls
    /// for built-in entries; enables them for user entries or new
    /// drafts.
    void LoadIntoForm(const QString &name);

    /// Collect the form's current state into a `RegexTemplate`.
    /// Used by Save and Validate; throws on malformed input
    /// (empty name, etc.) so the caller can surface a status
    /// message instead of writing garbage.
    [[nodiscard]] loglib::RegexTemplate GatherForm() const;

    /// Mark the form dirty so the Save / Revert buttons enable.
    /// Idempotent; called by every field's change signal.
    void MarkDirty();
    void MarkClean();

    /// Show a transient status message at the bottom of the
    /// dialog. Auto-clears after a few seconds.
    void ShowStatus(const QString &message, bool isError = false);

    /// True iff the current form state is editable (a user
    /// template is selected, or a brand-new draft is being
    /// authored). Built-ins are read-only.
    [[nodiscard]] bool IsCurrentEditable() const;

    /// Prompt the user "discard unsaved edits?" and return true
    /// to proceed. Returns true when there are no unsaved edits.
    [[nodiscard]] bool ConfirmDiscardEdits();

    RegexTemplateRegistry *mRegistry = nullptr;

    QListWidget *mListWidget = nullptr;

    QLineEdit *mNameEdit = nullptr;
    QPlainTextEdit *mPatternEdit = nullptr;
    QPlainTextEdit *mSampleLinesEdit = nullptr;
    QCheckBox *mAutoDetectCheck = nullptr;
    QSpinBox *mPrioritySpin = nullptr;
    /// Multi-line so descriptions can comfortably hold a paragraph
    /// covering the format + (optionally) an upstream attribution.
    QPlainTextEdit *mDescriptionEdit = nullptr;
    QLabel *mSourceLabel = nullptr;

    QPushButton *mNewButton = nullptr;
    QPushButton *mDuplicateButton = nullptr;
    QPushButton *mSaveButton = nullptr;
    QPushButton *mRevertButton = nullptr;
    QPushButton *mDeleteButton = nullptr;
    QPushButton *mValidateButton = nullptr;
    QPushButton *mOpenFolderButton = nullptr;
    QPushButton *mReloadButton = nullptr;

    QLabel *mStatusLabel = nullptr;
    QTimer *mStatusClearTimer = nullptr;

    /// Currently selected entry's name. Empty when the form is in
    /// "New template..." mode (no list-row backing it).
    QString mCurrentName;

    /// True when authoring a brand-new template that has not yet
    /// been saved (no list-row backing it). Controls Save's
    /// "create vs overwrite" semantics + the close-without-saving
    /// confirmation.
    bool mIsNewDraft = false;

    /// True when the form has unsaved edits relative to the
    /// loaded template. Drives Save / Revert button enablement
    /// and the close-confirmation prompt.
    bool mDirty = false;

    /// Re-entrancy guard: programmatic form repopulation (via
    /// `LoadIntoForm`) must not be misread as user edits by the
    /// field-change signals.
    bool mSuppressDirtySignals = false;
};
