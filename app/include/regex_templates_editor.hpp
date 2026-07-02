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

/// Modeless editor for the merged regex-template catalog. Shows a
/// list of every built-in + user template alongside an inline form
/// for the selected entry's fields (`name`, `pattern`, `sampleLines`,
/// `autoDetect`, `priority`, `description`). Built-ins are
/// read-only — the `Duplicate` button is the path to customise
/// them — while user templates can be created, edited, validated,
/// saved, and deleted in place.
///
/// Opened from `MainWindow` via `Settings -> Regex templates...`.
/// Constructed lazily; deleted-on-close is intentionally **not**
/// set so selection and half-typed edits survive close/reopen
/// (mirrors `PreferencesEditor`).
///
/// Built programmatically (not from a `.ui` file): the layout is
/// small and the built-in vs user enablement logic is awkward in
/// static XML.
class RegexTemplatesEditor : public QWidget
{
    Q_OBJECT

public:
    /// @p registry is the merged catalog the editor reads / writes
    /// through and must outlive this widget (lives in `main()` in
    /// production). Passing nullptr is a programming error — the
    /// editor has nothing to manage without a registry.
    explicit RegexTemplatesEditor(RegexTemplateRegistry *registry, QWidget *parent = nullptr);

    /// Repopulate the list after an external `registry->Reload()`
    /// or a `templatesChanged()` signal. Preserves the current
    /// selection by name if the entry still exists.
    void RefreshList();

protected:
    /// Warn before discarding unsaved edits when the user closes
    /// via Esc / X / Alt+F4.
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
    /// Build the list row label. Includes `(user)` / `(manual only)`
    /// badges so the source is visible without opening the form.
    [[nodiscard]] static QString FormatListLabel(const QString &name, bool fromUser, bool autoDetect);

    /// Populate the form from the registry entry @p name. An empty
    /// name clears the form (e.g. after Delete or in "New template"
    /// mode). Controls are disabled for built-ins and enabled for
    /// user entries and new drafts.
    void LoadIntoForm(const QString &name);

    /// Read the form's current state as a `RegexTemplate`. Throws
    /// on malformed input (empty name, etc.) so callers can surface
    /// a status message instead of writing garbage.
    [[nodiscard]] loglib::RegexTemplate GatherForm() const;

    /// Mark the form dirty; enables Save / Revert. Idempotent;
    /// called by every field's change signal.
    void MarkDirty();
    void MarkClean();

    /// Show a transient status message at the bottom of the dialog
    /// (auto-clears after a few seconds).
    void ShowStatus(const QString &message, bool isError = false);

    /// True iff the current form is editable — a user template is
    /// selected or a new draft is being authored. Built-ins are
    /// read-only.
    [[nodiscard]] bool IsCurrentEditable() const;

    /// Prompt "discard unsaved edits?" and return true to proceed.
    /// Returns true immediately when there are no unsaved edits.
    [[nodiscard]] bool ConfirmDiscardEdits();

    RegexTemplateRegistry *mRegistry = nullptr;

    QListWidget *mListWidget = nullptr;

    QLineEdit *mNameEdit = nullptr;
    QPlainTextEdit *mPatternEdit = nullptr;
    QPlainTextEdit *mSampleLinesEdit = nullptr;
    QCheckBox *mAutoDetectCheck = nullptr;
    QSpinBox *mPrioritySpin = nullptr;
    /// Multi-line so descriptions can hold a paragraph covering
    /// the format and an optional upstream attribution.
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

    /// Selected entry's name. Empty in "New template..." mode
    /// (no list row backs it).
    QString mCurrentName;

    /// True when authoring an unsaved new template (no list row
    /// backs it). Controls Save's create-vs-overwrite semantics
    /// and the close-without-saving confirmation.
    bool mIsNewDraft = false;

    /// True when the form has unsaved edits relative to the loaded
    /// template. Drives Save / Revert enablement and the close
    /// confirmation.
    bool mDirty = false;

    /// Re-entrancy guard: programmatic form fills via
    /// `LoadIntoForm` must not be misread as user edits by field
    /// change signals.
    bool mSuppressDirtySignals = false;
};
