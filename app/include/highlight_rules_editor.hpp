#pragma once

#include <loglib/log_configuration.hpp>

#include <QIcon>
#include <QWidget>

#include <utility>
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
/// Layout mirrors `RegexTemplatesEditor`: rule list on the left,
/// inline form on the right, list buttons on top (New / Duplicate
/// / Delete / Move up / Move down), form buttons on the bottom
/// (Save / Revert / Close).
///
/// Owned lazily by `MainWindow`. Edits happen on a local copy;
/// Save emits `rulesSaved(...)` and `MainWindow` mirrors the
/// vector into both the runtime `HighlightRuleSet` and
/// `LogConfigurationManager`. Delete-on-close is off so half-typed
/// edits survive close/reopen.
///
/// v1 scope: the form edits String / Number / Boolean match specs.
/// Time and Enumeration rules parse and render at runtime but are
/// read-only here -- edit the config file directly for those two.
class HighlightRulesEditor : public QWidget
{
    Q_OBJECT

public:
    /// @p rules seeds the local buffer (copied). @p columns feeds
    /// the column picker + type gating. @p theme supplies swatch
    /// icons. `MainWindow` owns the shared state that outlives the
    /// editor.
    HighlightRulesEditor(
        std::vector<loglib::LogConfiguration::HighlightRule> rules,
        std::vector<loglib::LogConfiguration::Column> columns,
        ThemeControl *theme,
        QWidget *parent = nullptr
    );

    /// Refresh the column picker after `LogConfiguration::columns`
    /// changes (`AppendKeys`, type pinning). Preserves the current
    /// rule selection.
    void SetColumns(std::vector<loglib::LogConfiguration::Column> columns);

    /// Replace the rule buffer after an external mutation (e.g.
    /// config load). Preserves the current selection when possible.
    void SetRules(std::vector<loglib::LogConfiguration::HighlightRule> rules);

signals:
    /// Fired on Save. Vector order matters (last-match-wins).
    /// `MainWindow` mirrors the result into both the runtime
    /// `HighlightRuleSet` and the persistent config.
    void rulesSaved(std::vector<loglib::LogConfiguration::HighlightRule> rules);

protected:
    /// Prompt before discarding unsaved edits on Esc / X / Alt+F4.
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
    /// Build a swatch-menu tool button. First menu entry is
    /// `Inherit` (index 0); slots 1..`HIGHLIGHT_PALETTE_SIZE` pull
    /// their icons from `ThemeControl::HighlightBrushFor`.
    [[nodiscard]] QToolButton *BuildSwatchButton(bool isForeground);

    /// Rebuild the popup menu on an existing swatch button (used
    /// on `themeChanged` so the palette icons refresh without
    /// reparenting the button).
    void RebuildSwatchMenu(QToolButton *button, bool isForeground);

    /// List label: rule name + optional `[disabled]` / `[inactive]`
    /// badges. Icon is a paired fg/bg swatch.
    [[nodiscard]] QString FormatListLabel(const loglib::LogConfiguration::HighlightRule &rule) const;
    [[nodiscard]] QIcon FormatListIcon(const loglib::LogConfiguration::HighlightRule &rule, int sizePx) const;

    /// Rebuild the list from `mLocalRules` and reload the form.
    /// Use `RefreshListItem` for per-keystroke updates so the
    /// form's cursor position isn't clobbered.
    void RebuildList(int selectRow = -1);

    /// Update only the label + icon at @p row. Leaves the form
    /// alone so `QLineEdit` cursors and selections survive.
    void RefreshListItem(int row);

    /// Populate the form from `mLocalRules[row]`. `row < 0` clears
    /// the form (empty list).
    void LoadIntoForm(int row);

    /// Read the form back into `mLocalRules[mCurrentRow]`. No-op
    /// with no selection.
    void GatherForm();

    void UpdateFormEnabled();
    void UpdateListButtons();

    /// True iff `mLocalRules != mBaseline`.
    [[nodiscard]] bool IsDirty() const;

    void MarkDirty();

    /// Prompt on unsaved edits; returns true to proceed. Returns
    /// true immediately when clean.
    [[nodiscard]] bool ConfirmDiscardEdits();

    /// Transient status message with auto-clear timer.
    void ShowStatus(const QString &message, bool isError = false);

    /// Swatch pixel size (`PM_SmallIconSize` with a fallback).
    [[nodiscard]] int SwatchIconSizePx() const;

    /// Column index for @p rule's keys, or -1 (drives the
    /// `[inactive]` badge).
    [[nodiscard]] int ResolveColumnIndex(const loglib::LogConfiguration::HighlightRule &rule) const;

    /// Populate the column combo from `mColumns`.
    void RepopulateColumnCombo();

    /// Diagnose why @p rule can't be saved. Empty string = valid.
    /// Guards: missing column, empty String needle, unbounded
    /// Number rule, Boolean rule with neither true nor false.
    /// Save is disabled while any rule fails.
    [[nodiscard]] QString ValidateRule(const loglib::LogConfiguration::HighlightRule &rule) const;

    /// First invalid rule as `(index, message)`, or `(-1, "")`.
    [[nodiscard]] std::pair<int, QString> FirstInvalidRule() const;

    std::vector<loglib::LogConfiguration::Column> mColumns;
    ThemeControl *mTheme = nullptr;

    /// Local edit buffer; a copy of what `MainWindow` passed in.
    std::vector<loglib::LogConfiguration::HighlightRule> mLocalRules;

    /// Baseline for `IsDirty` / Revert. Updated on construction,
    /// `SetRules`, and Save.
    std::vector<loglib::LogConfiguration::HighlightRule> mBaseline;

    /// Selected row, or -1 (empty list / no selection).
    int mCurrentRow = -1;

    /// Re-entrancy guard: `LoadIntoForm` writes to fields; the
    /// field change signals must not misread that as a user edit.
    bool mSuppressDirtySignals = false;

    QListWidget *mListWidget = nullptr;

    QLineEdit *mNameEdit = nullptr;
    QCheckBox *mEnabledCheck = nullptr;
    QComboBox *mColumnCombo = nullptr;
    QComboBox *mTypeCombo = nullptr;

    /// Type-specific match panes. Index matches `HighlightRule::Type`:
    /// 0=String, 1=Time (read-only), 2=Enumeration (read-only),
    /// 3=Number, 4=Boolean.
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

    QToolButton *mForegroundButton = nullptr;
    QToolButton *mBackgroundButton = nullptr;
    QCheckBox *mBoldCheck = nullptr;
    QCheckBox *mItalicCheck = nullptr;

    /// Current swatch selection; 0 = inherit. Synced into
    /// `mLocalRules` by `GatherForm`.
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
