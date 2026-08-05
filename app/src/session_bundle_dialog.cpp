#include "session_bundle_dialog.hpp"

#include <loglib/session_bundle.hpp>

#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QVBoxLayout>

namespace
{

/// QSettings keys for bundle-encoder knobs. Same organisation the
/// rest of the app uses.
constexpr auto SETTINGS_LEVEL = "session_bundle/compression_level";
constexpr auto SETTINGS_WORKERS = "session_bundle/workers";
constexpr auto SETTINGS_LAST_DIR = "session_bundle/last_dir";

QString BundleFileFilter()
{
    return QStringLiteral("Session bundle (*%1);;All Files (*)").arg(loglib::SESSION_BUNDLE_EXTENSION);
}

/// Appends the `.slvbundle` extension to @p path unless it already
/// carries one (case-insensitive). Preserves an empty input so that
/// callers can distinguish "user cleared the field" from "user typed
/// a bare stem": returning `".slvbundle"` here would silently promote
/// an empty destination into a hidden dotfile in the CWD and defeat
/// the emptiness guard in `OnAccept` / `MainWindow::ExportSessionBundle`.
QString AppendBundleExtensionIfMissing(QString path)
{
    if (path.isEmpty())
    {
        return path;
    }
    const QString ext = QString::fromLatin1(loglib::SESSION_BUNDLE_EXTENSION);
    if (!path.endsWith(ext, Qt::CaseInsensitive))
    {
        path.append(ext);
    }
    return path;
}

} // namespace

SessionBundleDialog::SessionBundleDialog(
    std::size_t rowCount,
    std::size_t sourceCount,
    QString defaultStem,
    QString defaultDir,
    bool isLiveTail,
    QWidget *parent
)
    : QDialog(parent), mDefaultDir(std::move(defaultDir))
{
    setWindowTitle(tr("Export Session Bundle"));
    setModal(true);

    auto *form = new QFormLayout;
    // The main row includes a line-edit + browse button.
    auto *destinationRow = new QHBoxLayout;
    mDestinationEdit = new QLineEdit(this);
    mDestinationEdit->setToolTip(
        tr("Destination file. The .slvbundle extension is appended automatically if missing.")
    );
    mBrowseButton = new QPushButton(tr("Browse\u2026"), this);
    connect(mBrowseButton, &QPushButton::clicked, this, &SessionBundleDialog::OnBrowseClicked);
    destinationRow->addWidget(mDestinationEdit, 1);
    destinationRow->addWidget(mBrowseButton);
    form->addRow(tr("Destination:"), destinationRow);

    // Seed the destination with `<default-dir>/<stem>.slvbundle`.
    QSettings settings;
    const QString rememberedDir = settings.value(SETTINGS_LAST_DIR, mDefaultDir).toString();
    const QString effectiveDir = rememberedDir.isEmpty() ? mDefaultDir : rememberedDir;
    const QString seedName = QStringLiteral("%1%2").arg(defaultStem, QString::fromLatin1(loglib::SESSION_BUNDLE_EXTENSION));
    mDestinationEdit->setText(QDir(effectiveDir).filePath(seedName));

    mCompressionLevelSpin = new QSpinBox(this);
    mCompressionLevelSpin->setRange(1, 22);
    mCompressionLevelSpin->setValue(settings.value(SETTINGS_LEVEL, 3).toInt());
    mCompressionLevelSpin->setToolTip(
        tr("zstd compression level. 1 = fastest / worst compression, 22 = slowest / best. "
           "3 matches zstd's balanced default and is a good choice for interactive sharing.")
    );
    form->addRow(tr("Compression level:"), mCompressionLevelSpin);

    mWorkersSpin = new QSpinBox(this);
    mWorkersSpin->setRange(0, 64);
    mWorkersSpin->setSpecialValueText(tr("Single-threaded"));
    mWorkersSpin->setValue(settings.value(SETTINGS_WORKERS, 0).toInt());
    mWorkersSpin->setToolTip(
        tr("zstd worker threads for the single bundle frame. "
           "0 uses zstd's single-threaded path; positive values enable zstd multi-threading.")
    );
    form->addRow(tr("Worker threads:"), mWorkersSpin);

    mPreviewLabel = new QLabel(this);
    mPreviewLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    mPreviewLabel->setText(
        tr("The bundle will contain %L1 row(s) from %L2 source(s), plus the current filter, sort, "
           "anchor, and highlight-rule state. All retained rows are exported -- filters do not "
           "restrict the payload; they are stored so the receiving copy of the app reproduces the "
           "same view.")
            .arg(static_cast<qulonglong>(rowCount))
            .arg(static_cast<qulonglong>(sourceCount))
    );
    mPreviewLabel->setWordWrap(true);
    form->addRow(mPreviewLabel);

    mLiveTailNote = new QLabel(this);
    mLiveTailNote->setText(
        tr("Live-tail note: the bundle captures a point-in-time snapshot of the currently "
           "retained rows. Continued streaming after the export is not preserved.")
    );
    mLiveTailNote->setWordWrap(true);
    mLiveTailNote->setVisible(isLiveTail);
    form->addRow(mLiveTailNote);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("Export"));
    connect(buttons, &QDialogButtonBox::accepted, this, &SessionBundleDialog::OnAccept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addWidget(buttons);

    resize(560, sizeHint().height());
}

SessionBundleDialog::Config SessionBundleDialog::Configuration() const
{
    Config cfg;
    cfg.destination = AppendBundleExtensionIfMissing(mDestinationEdit->text().trimmed());
    cfg.compressionLevel = mCompressionLevelSpin->value();
    cfg.totalWorkers = mWorkersSpin->value();
    return cfg;
}

void SessionBundleDialog::OnBrowseClicked()
{
    const QString seed = mDestinationEdit->text().trimmed();
    const QString startPath = seed.isEmpty()
                                   ? QDir(mDefaultDir).filePath(QStringLiteral("session%1")
                                                                    .arg(QString::fromLatin1(loglib::SESSION_BUNDLE_EXTENSION)))
                                   : seed;
    const QString chosen = QFileDialog::getSaveFileName(
        this, tr("Export Session Bundle"), startPath, BundleFileFilter()
    );
    if (chosen.isEmpty())
    {
        return;
    }
    mDestinationEdit->setText(AppendBundleExtensionIfMissing(chosen));
}

void SessionBundleDialog::OnAccept()
{
    // Emptiness check runs on the *pre-append* trimmed text: a bare
    // empty field would otherwise be silently rescued by
    // `AppendBundleExtensionIfMissing` (used to append the extension
    // unconditionally, so `""` became `".slvbundle"` and this
    // validation was dead code).
    const QString trimmed = mDestinationEdit->text().trimmed();
    if (trimmed.isEmpty())
    {
        QMessageBox::warning(this, tr("Export Session Bundle"), tr("Please choose a destination file."));
        return;
    }
    const QString dest = AppendBundleExtensionIfMissing(trimmed);
    // Warn on overwrite -- Qt's `getSaveFileName` handles this only
    // when the user goes through the file dialog; hand-typed paths
    // bypass it, so re-check.
    const QFileInfo destInfo(dest);
    if (destInfo.exists())
    {
        const auto response = QMessageBox::question(
            this,
            tr("Overwrite File?"),
            tr("'%1' already exists. Overwrite?").arg(destInfo.fileName()),
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No
        );
        if (response != QMessageBox::Yes)
        {
            return;
        }
    }

    QSettings settings;
    settings.setValue(SETTINGS_LEVEL, mCompressionLevelSpin->value());
    settings.setValue(SETTINGS_WORKERS, mWorkersSpin->value());
    settings.setValue(SETTINGS_LAST_DIR, destInfo.absolutePath());

    mDestinationEdit->setText(dest);
    accept();
}
