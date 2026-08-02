#pragma once

#include "row_exporter.hpp"

#include <QDialog>
#include <QString>

#include <cstddef>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;

/// Modal-per-window dialog for **File -> Export Filtered Rows...**.
///
/// Collects a destination path, an export format, and a few
/// format-specific toggles. Does NOT run the export itself; the
/// caller reads `Configuration()` after `accept()` and dispatches
/// the async worker (see `MainWindow::ExportFilteredRows`).
///
/// Built programmatically (like `NetworkStreamDialog`) because
/// format-conditional toggles are cleaner in C++ than `.ui` XML.
class ExportDialog : public QDialog
{
    Q_OBJECT

public:
    struct Config
    {
        slv::exports::ExportFormat format = slv::exports::ExportFormat::JsonLines;
        QString destination;
        /// Export selection only. Ignored (and toggle disabled)
        /// when no rows are selected.
        bool selectionOnly = false;
        /// Emit CSV / Markdown header row. Ignored by JSON /
        /// Snapshot.
        bool includeHeaderRow = true;
        /// Emit hidden columns in CSV / Markdown. JSON always
        /// includes every field; Snapshot is row-shape.
        bool includeHiddenColumns = false;
    };

    /// @p rowCountFiltered  rows in the current filter proxy (drives
    ///                      the preview label).
    /// @p rowCountSelected  rows in the current selection; zero
    ///                      disables the "selection only" toggle.
    /// @p defaultStem       filename stem used to seed the destination.
    /// @p defaultDir        starting directory for the browse dialog.
    /// @p isLiveTail        show the live-tail snapshot caveat when true.
    ExportDialog(
        std::size_t rowCountFiltered,
        std::size_t rowCountSelected,
        QString defaultStem,
        QString defaultDir,
        bool isLiveTail,
        QWidget *parent = nullptr
    );

    /// Only meaningful after `exec()` returned `Accepted`.
    [[nodiscard]] Config Configuration() const;

private slots:
    void OnFormatChanged();
    void OnSelectionToggled();
    void OnBrowseClicked();
    void OnAccept();

private:
    /// Rebuild the row-count preview label.
    void RefreshPreview();

    /// Swap the destination extension to match the selected
    /// format, but only if the current path ends in a *known*
    /// format extension (so a hand-typed `.txt` is preserved).
    void UpdateExtensionSuggestion();

    /// Enable / disable format-specific toggles.
    void UpdateOptionVisibility();

    std::size_t mRowCountFiltered = 0;
    std::size_t mRowCountSelected = 0;
    QString mDefaultDir;

    QComboBox *mFormatCombo = nullptr;
    QLineEdit *mDestinationEdit = nullptr;
    QPushButton *mBrowseButton = nullptr;
    QCheckBox *mSelectionOnly = nullptr;
    QCheckBox *mIncludeHeader = nullptr;
    QCheckBox *mIncludeHidden = nullptr;
    QLabel *mPreviewLabel = nullptr;
    QLabel *mLiveTailNote = nullptr;
    QLabel *mRowSizeWarning = nullptr;
};
