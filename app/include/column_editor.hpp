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

/// Modal dialog for editing one column's `header`, `type`,
/// `autoDetect`, and `visible` fields. `keys` are shown read-only
/// (owned by the parser). The type combo folds `Type::Any +
/// autoDetect` into a single "Auto-detect" entry at the top. On
/// accept, writes through `LogConfigurationManager` and refreshes
/// the diagnostics health cache.
class ColumnEditor : public QDialog
{
    Q_OBJECT

public:
    /// Bind the editor to @p columnIndex of @p model. Rejects on
    /// first show if the index is out of range.
    ColumnEditor(LogModel *model, int columnIndex, QWidget *parent = nullptr);

    /// Write the form back to the model and accept. Public so tests
    /// can drive the apply path without poking at the button box.
    void Apply();

    /// Column this editor is bound to, or -1 if unbound.
    [[nodiscard]] int ColumnIndex() const noexcept
    {
        return mColumnIndex;
    }

#ifdef LOGAPP_BUILD_TESTING
    /// Test-only direct widget accessors. `findChild<>` lookups by
    /// object name are unreliable on the GitHub-hosted Linux runner
    /// with Qt 6.8 + offscreen QPA, so tests reach the form widgets
    /// through these bypasses (mirrors the QAction workaround in
    /// `FindActionByObjectName`).
    [[nodiscard]] QLineEdit *HeaderEditForTest() const noexcept
    {
        return mHeaderEdit;
    }
    [[nodiscard]] QComboBox *TypeComboForTest() const noexcept
    {
        return mTypeCombo;
    }
    [[nodiscard]] QCheckBox *VisibleCheckForTest() const noexcept
    {
        return mVisibleCheck;
    }
#endif

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

    /// Combo selection captured at `Populate()` time. `WriteBack()`
    /// only commits a type / autoDetect change when the user actually
    /// picked a different entry -- otherwise an auto-promoted column
    /// (e.g. `(Enumeration, autoDetect=true)`, a pair the combo lacks
    /// an exact entry for) would be silently pinned to
    /// `autoDetect=false` on accept.
    int mInitialTypeChoiceIndex = -1;
};
