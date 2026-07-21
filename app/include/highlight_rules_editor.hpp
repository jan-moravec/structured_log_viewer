#pragma once

#include <loglib/log_configuration.hpp>

#include <QIcon>
#include <QWidget>

#include <vector>

class HighlightRuleSet;
class QCheckBox;
class QComboBox;
class QCloseEvent;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QPushButton;
class QStackedWidget;
class QTimer;
class QToolButton;
class ThemeControl;

/// Modeless editor for `LogConfiguration::highlightRules`.
///
/// Shape mirrors `RegexTemplatesEditor`: `QListWidget` of rules on
/// the left, inline form for the selected rule on the right, top-
/// bar buttons for list-level actions (New / Duplicate / Delete /
/// Move up / Move down), bottom-bar for form-level actions
/// (Save / Revert / Close).
///
/// Owned lazily by `MainWindow`; opened via `Settings -> Highlight
/// rules...`. Constructed with the current column list + rule set;
/// the editor edits a **local copy** of the rules and emits
/// `rulesSaved(...)` on Save so `MainWindow` pipes the update into
/// both `HighlightRuleSet::SetRules` and
/// `LogConfigurationManager::SetHighlightRules` atomically.
///
/// Delete-on-close is intentionally **not** set so half-typed
/// edits survive close/reopen (same as `RegexTemplatesEditor`).
///
/// v1 scope: the form exposes String / Number / Boolean match
/// specs directly. Time and Enumeration rules parse and render
/// correctly through the model layer but are read-only in this
/// editor (they can be authored by hand-editing the config file).
/// A follow-up will lift the last two into the UI.
class HighlightRulesEditor : public QWidget
{
    Q_OBJECT

public:
    /// @p columns is the current schema; drives the column-picker
    /// combo and the type gating. @p theme provides swatch icons
    /// for the palette pickers. @p rules is the initial rule set
    /// (copied; further edits are local until Save).
    ///
    /// All three parameters must outlive the editor (they live in
    /// `MainWindow` in production).
    HighlightRulesEditor(
        std::vector<loglib::LogConfiguration::HighlightRule> rules,
        std::vector<loglib::LogConfiguration::Column> columns,
        ThemeControl *theme,
        QWidget *parent = nullptr
    );

    /// Refresh the column-picker combo box after
    /// `LogConfiguration::columns` changes (`AppendKeys`, type
    /// pinning). Preserves the current selection by key. Called by
    /// `MainWindow` when the schema mutates while the editor is
    /// open.
    void SetColumns(std::vector<loglib::LogConfiguration::Column> columns);

    /// Refresh the rule list after an external mutation (e.g. a
    /// config load). Preserves the current selection when the
    /// matching rule is still present.
    void SetRules(std::vector<loglib::LogConfiguration::HighlightRule> rules);

signals:
    /// Emitted on Save with the final rule vector. Vector order is
    /// meaningful (last-match-wins). `MainWindow` pipes this into
    /// both the `HighlightRuleSet` runtime and the
    /// `LogConfigurationManager` mirror.
    void rulesSaved(std::vector<loglib::LogConfiguration::HighlightRule> rules);

protected:
    /// Warn before discarding unsaved edits on Esc / X / Alt+F4.
    void closeEvent(QCloseEvent *event) override;

private slots:
    void OnSelectionChanged();
    void OnFieldEdited();
    void OnColumnChanged();
    void OnTypeChanged();
    void OnNewClicked();
    void OnDuplicateClicked();
    void OnDeleteClicked();
    void OnMoveUpClicked();
    void OnMoveDownClicked();
    void OnSaveClicked();
    void OnRevertClicked();

private:
    /// Build the swatch-menu tool buttons for foreground / background.
    /// First entry is `Inherit` (index 0). Slots 1..HIGHLIGHT_PALETTE_SIZE
    /// pull their swatch icons from `ThemeControl::HighlightBrushFor`.
    [[nodiscard]] QToolButton *BuildSwatchButton(bool isForeground);

    /// Rebuild the popup menu on an existing swatch button. Extracted
    /// so `themeChanged` can refresh the palette without replacing
    /// the `QToolButton` itself (which would need reparenting through
    /// the layout).
    void RebuildSwatchMenu(QToolButton *button, bool isForeground);

    /// Build the list row label: rule name + `(inactive)` badge when
    /// `columnKeys` doesn't resolve against the current columns.
    /// Icon is a paired fg/bg swatch.
    [[nodiscard]] QString FormatListLabel(const loglib::LogConfiguration::HighlightRule &rule) const;
    [[nodiscard]] QIcon FormatListIcon(const loglib::LogConfiguration::HighlightRule &rule, int sizePx) const;

    /// Rebuild the list from `mLocalRules`, preserving @p selectRow
    /// (or the current row when negative). Reloads the form from the
    /// selected rule (`LoadIntoForm`); use `RefreshListItem` instead
    /// when the mutation originated from the form and reloading it
    /// would clobber the user's cursor.
    void RebuildList(int selectRow = -1);

    /// Update only the label + icon for the row at @p row. Does NOT
    /// call `LoadIntoForm`, so the current cursor position and text
    /// selection in the form's `QLineEdit`s survive a keystroke.
    /// No-op when @p row is out of range or the list widget's
    /// corresponding item does not exist.
    void RefreshListItem(int row);

    /// Populate the form from `mLocalRules[row]`. Empty row / -1
    /// clears the form (Delete of last rule).
    void LoadIntoForm(int row);

    /// Read the form and write back to `mLocalRules[mCurrentRow]`.
    /// No-op when no rule is selected.
    void GatherForm();

    /// Enable / disable form controls based on `mCurrentRow`.
    void UpdateFormEnabled();

    /// Enable / disable list-toolbar buttons based on selection +
    /// `mLocalRules.size()`.
    void UpdateListButtons();

    /// True iff `mLocalRules` differs from `mBaseline`.
    [[nodiscard]] bool IsDirty() const;

    void MarkDirty();

    /// Prompt "discard unsaved edits?"; returns true to proceed.
    /// Returns true immediately when clean.
    [[nodiscard]] bool ConfirmDiscardEdits();

    /// Show a transient status message (auto-clears).
    void ShowStatus(const QString &message, bool isError = false);

    /// Pixel size for swatch icons; matches `PM_SmallIconSize`.
    [[nodiscard]] int SwatchIconSizePx() const;

    /// Look up a column by rule's `columnKeys`. Returns -1 if not
    /// resolved (drives the `(inactive)` badge).
    [[nodiscard]] int ResolveColumnIndex(const loglib::LogConfiguration::HighlightRule &rule) const;

    /// Populate the column picker combo from `mColumns`.
    void RepopulateColumnCombo();

    std::vector<loglib::LogConfiguration::Column> mColumns;
    ThemeControl *mTheme = nullptr;

    /// The rule set the editor edits. Copy of what `MainWindow`
    /// last passed via the constructor / `SetRules`.
    std::vector<loglib::LogConfiguration::HighlightRule> mLocalRules;

    /// Snapshot for the `IsDirty` / Revert comparison. Updated on
    /// construction, `SetRules`, and Save.
    std::vector<loglib::LogConfiguration::HighlightRule> mBaseline;

    /// Currently selected row in `mLocalRules`, or -1 for "no
    /// selection" (empty list).
    int mCurrentRow = -1;

    /// Re-entrancy guard: programmatic form fills via
    /// `LoadIntoForm` must not be misread as user edits by the
    /// field change signals.
    bool mSuppressDirtySignals = false;

    QListWidget *mListWidget = nullptr;

    QLineEdit *mNameEdit = nullptr;
    QCheckBox *mEnabledCheck = nullptr;
    QComboBox *mColumnCombo = nullptr;
    QComboBox *mTypeCombo = nullptr;

    /// Stacked type-specific match widgets. Index matches
    /// `HighlightRule::Type` enum order:
    /// 0 = String, 1 = Time (read-only), 2 = Enumeration (read-only),
    /// 3 = Number, 4 = Boolean.
    QStackedWidget *mMatchStack = nullptr;

    // String pane
    QComboBox *mStringMatchCombo = nullptr;
    QLineEdit *mStringNeedleEdit = nullptr;

    // Number pane
    QCheckBox *mNumberMinEnabled = nullptr;
    QDoubleSpinBox *mNumberMinValue = nullptr;
    QCheckBox *mNumberMaxEnabled = nullptr;
    QDoubleSpinBox *mNumberMaxValue = nullptr;

    // Boolean pane
    QCheckBox *mBoolIncludeTrue = nullptr;
    QCheckBox *mBoolIncludeFalse = nullptr;

    // Read-only pane for Time / Enumeration.
    QLabel *mReadOnlyLabel = nullptr;

    QToolButton *mForegroundButton = nullptr;
    QToolButton *mBackgroundButton = nullptr;
    QCheckBox *mBoldCheck = nullptr;
    QCheckBox *mItalicCheck = nullptr;

    /// Selected palette slot for the current rule. Sync'd into
    /// `mLocalRules` by `GatherForm`. `0` = inherit.
    std::uint8_t mSelectedForegroundIndex = 0;
    std::uint8_t mSelectedBackgroundIndex = 0;

    QPushButton *mNewButton = nullptr;
    QPushButton *mDuplicateButton = nullptr;
    QPushButton *mDeleteButton = nullptr;
    QPushButton *mMoveUpButton = nullptr;
    QPushButton *mMoveDownButton = nullptr;
    QPushButton *mSaveButton = nullptr;
    QPushButton *mRevertButton = nullptr;
    QPushButton *mCloseButton = nullptr;

    QLabel *mStatusLabel = nullptr;
    QTimer *mStatusClearTimer = nullptr;
};
