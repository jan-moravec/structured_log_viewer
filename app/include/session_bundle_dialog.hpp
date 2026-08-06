#pragma once

#include <QDialog>
#include <QString>

#include <cstddef>

class QCheckBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QWidget;

/// Modal dialog for **File -> Export Session Bundle...**.
///
/// Collects the destination and optional encoder settings. The caller
/// starts the export after the dialog is accepted.
class SessionBundleDialog : public QDialog
{
    Q_OBJECT

public:
    struct Config
    {
        QString destination;
        /// zstd compression level, 1..22.
        int compressionLevel = 3;
        /// zstd worker threads (0 = zstd's single-threaded path).
        int totalWorkers = 0;
    };

    /// @p rowCount    retained rows shown in the preview.
    /// @p defaultStem filename stem used to seed the destination.
    /// @p defaultDir  starting directory for the browse dialog.
    /// @p isLiveTail  show the live-tail snapshot caveat when true.
    SessionBundleDialog(
        std::size_t rowCount, const QString &defaultStem, QString defaultDir, bool isLiveTail, QWidget *parent = nullptr
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
    QCheckBox *mAdvancedToggle = nullptr;
    QWidget *mAdvancedContainer = nullptr;
    QSpinBox *mCompressionLevelSpin = nullptr;
    QSpinBox *mWorkersSpin = nullptr;
    QLabel *mPreviewLabel = nullptr;
    QLabel *mLiveTailNote = nullptr;
};
