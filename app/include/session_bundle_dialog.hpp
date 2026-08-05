#pragma once

#include <QDialog>
#include <QString>

#include <cstddef>

class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;

/// Modal dialog for **File -> Export Session Bundle...**.
///
/// Collects a destination path plus a small set of encoder knobs
/// (compression level, worker budget) for the `.slvbundle` archive.
/// Does NOT run the export itself; the caller reads `Configuration()`
/// after `accept()` and dispatches the async worker (see
/// `MainWindow::ExportSessionBundle`).
///
/// The bundle scope is always "everything retained" (all rows, all
/// filters / anchors / highlight rules) -- there is no per-run
/// scope toggle. This matches the option the plan settled on
/// (`bundle_scope_ui: all_only_always`).
class SessionBundleDialog : public QDialog
{
    Q_OBJECT

public:
    struct Config
    {
        QString destination;
        /// zstd compression level, 1..22. `3` matches zstd's own
        /// default and yields ~1 GiB/s decompression on modern
        /// hardware.
        int compressionLevel = 3;
        /// zstd worker threads for the single frame.
        /// `0` selects zstd's single-threaded path.
        int totalWorkers = 0;
    };

    /// @p rowCount   number of retained rows across all sources
    ///               (drives the preview label).
    /// @p sourceCount number of original `LineSource`s being
    ///                flattened into the bundle.
    /// @p defaultStem  filename stem used to seed the destination.
    /// @p defaultDir   starting directory for the browse dialog.
    /// @p isLiveTail   show the live-tail snapshot caveat when true.
    SessionBundleDialog(
        std::size_t rowCount,
        std::size_t sourceCount,
        QString defaultStem,
        QString defaultDir,
        bool isLiveTail,
        QWidget *parent = nullptr
    );

    /// Only meaningful after `exec()` returned `Accepted`.
    [[nodiscard]] Config Configuration() const;

private slots:
    void OnBrowseClicked();
    void OnAccept();

private:
    QString mDefaultDir;

    QLineEdit *mDestinationEdit = nullptr;
    QPushButton *mBrowseButton = nullptr;
    QSpinBox *mCompressionLevelSpin = nullptr;
    QSpinBox *mWorkersSpin = nullptr;
    QLabel *mPreviewLabel = nullptr;
    QLabel *mLiveTailNote = nullptr;
};
