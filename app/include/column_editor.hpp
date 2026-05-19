#pragma once

#include "log_model.hpp"

#include <loglib/log_configuration.hpp>

#include <QDialog>
#include <QPointer>

class QCheckBox;
class QComboBox;
class QDialogButtonBox;
class QLabel;
class QLineEdit;

/// Modal dialog that edits every user-controllable field of a
/// `loglib::LogConfiguration::Column`: the display header, the
/// configured `Type` (collapsed onto a single combo that folds the
/// `Type::Any + autoDetect` pair into an "Auto-detect" entry at the
/// top), and the `visible` flag. `keys` are surfaced read-only -- the
/// parser owns the key->column binding -- but the user can still
/// rename the header that drives the GUI label.
///
/// On `accept()`, the dialog writes the changes through
/// `LogConfigurationManager`'s typed mutators, then calls
/// `LogModel::RefreshColumnHealth()` so the diagnostics cache picks
/// up the new type immediately. Header / decoration / visibility
/// changes ride the existing `dataChanged` / `headerDataChanged`
/// signals.
class ColumnEditor : public QDialog
{
    Q_OBJECT

public:
    /// Construct an editor bound to @p columnIndex of @p model. If
    /// @p columnIndex is out of range the dialog rejects on first
    /// show; production callers always pass a valid index from the
    /// header context menu or the diagnostics dialog row.
    ColumnEditor(LogModel *model, int columnIndex, QWidget *parent = nullptr);

    /// Apply the form's current state to the model and emit
    /// `accept()`. Public so the test seam can drive the apply path
    /// without having to fish out the QDialogButtonBox.
    void Apply();

    /// Inspector for the column index this editor is currently
    /// bound to. -1 once a future `Rebind(...)` call swaps the
    /// instance to a different column (not yet wired -- reserved
    /// for the `Columns Manager` dialog).
    [[nodiscard]] int ColumnIndex() const noexcept
    {
        return mColumnIndex;
    }

private:
    void Populate();
    void WriteBack();

    QPointer<LogModel> mModel;
    int mColumnIndex = -1;

    QLineEdit *mHeaderEdit = nullptr;
    QLabel *mKeysLabel = nullptr;
    QComboBox *mTypeCombo = nullptr;
    QCheckBox *mVisibleCheck = nullptr;
    QLabel *mHealthLabel = nullptr;
    QDialogButtonBox *mButtonBox = nullptr;
};
