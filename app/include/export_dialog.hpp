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
/// The dialog collects a destination path, an export format, and a
/// small set of format-specific toggles. It does NOT run the export
/// itself — the caller reads `Configuration()` after `accept()` and
/// dispatches the async worker (see `MainWindow::ExportFilteredRows`).
///
/// Constructed programmatically (mirrors `NetworkStreamDialog`)
/// because the layout is small and the format-conditional toggles
/// are cleaner in C++ than in `.ui` XML.
class ExportDialog : public QDialog
{
    Q_OBJECT

public:
    struct Config
    {
        slv::exports::ExportFormat format = slv::exports::ExportFormat::JsonLines;
        QString destination;
        /// User asked for selection-only. Ignored if no selection
        /// exists (dialog disables the toggle in that case).
        bool selectionOnly = false;
        /// Emit the CSV / Markdown header row (ignored for JSON /
        /// Snapshot).
        bool includeHeaderRow = true;
        /// Emit hidden columns in CSV / Markdown (JSON always
        /// includes all fields; Snapshot is row-level not
        /// column-oriented).
        bool includeHiddenColumns = false;
    };

    /// @p rowCountFiltered is the number of rows in the current
    /// filter proxy; drives the row-count preview label.
    /// @p rowCountSelected is the count that would land in the
    /// exported slice if `Export selection only` is on. When zero,
    /// the toggle is disabled.
    /// @p defaultStem is the source file's basename (no extension)
    /// used to seed the destination path.
    /// @p defaultDir is where the destination browser opens.
    /// @p isLiveTail annotates the dialog with a caveat about
    /// snapshot semantics during live tail.
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

    /// Update destination extension when the user picks a new
    /// format (only if the current path ends in a known format
    /// extension so we don't blow away a hand-typed name).
    void UpdateExtensionSuggestion();

    /// Show / hide format-specific toggles.
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
