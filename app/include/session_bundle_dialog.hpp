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
/// Collects a destination path and encoder knobs (compression level,
/// worker budget) for the `.slvbundle` archive. Does not run the
/// export itself: the caller reads `Configuration()` after `accept()`
/// and dispatches the async worker (see
/// `MainWindow::ExportSessionBundle`).
///
/// Bundle scope is always "everything retained" (all rows plus every
/// filter, anchor, and highlight rule); no per-run scope toggle.
class SessionBundleDialog : public QDialog
{
    Q_OBJECT

public:
    struct Config
    {
        QString destination;
        /// zstd compression level, 1..22. `3` is zstd's default and
        /// decompresses at ~1 GiB/s on modern hardware.
        int compressionLevel = 3;
        /// zstd worker threads (0 = zstd's single-threaded path).
        int totalWorkers = 0;
    };

    /// @p rowCount    retained rows across all sources; drives the
    ///                preview label.
    /// @p sourceCount original `LineSource`s flattened into the bundle.
    /// @p defaultStem filename stem used to seed the destination.
    /// @p defaultDir  starting directory for the browse dialog.
    /// @p isLiveTail  show the live-tail snapshot caveat when true.
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
