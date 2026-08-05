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

/// QSettings keys for the bundle-encoder knobs.
constexpr auto SETTINGS_LEVEL = "session_bundle/compression_level";
constexpr auto SETTINGS_WORKERS = "session_bundle/workers";
constexpr auto SETTINGS_LAST_DIR = "session_bundle/last_dir";

QString BundleFileFilter()
{
    return QStringLiteral("Session bundle (*%1);;All Files (*)").arg(loglib::SESSION_BUNDLE_EXTENSION);
}

/// Appends `.slvbundle` to @p path unless it already carries it
/// (case-insensitive). Empty input is preserved so the emptiness
/// guards in `OnAccept` / `MainWindow::ExportSessionBundle` still
/// fire -- otherwise `""` would become a `.slvbundle` dotfile.
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

/// Read an int from QSettings, falling back to @p defaultValue when
/// the stored value is missing, unparsable, or outside
/// `[minValue, maxValue]`. Without this, `QSpinBox::setValue` would
/// silently clamp a corrupted value to the spinbox range and give
/// the user, say, the fastest/worst compression instead of the
/// intended balanced default.
int ClampedSettingsInt(const QSettings &settings, const char *key, int defaultValue, int minValue, int maxValue)
{
    bool ok = false;
    const int stored = settings.value(key, defaultValue).toInt(&ok);
    if (!ok || stored < minValue || stored > maxValue)
    {
        return defaultValue;
    }
    return stored;
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
    mCompressionLevelSpin->setValue(ClampedSettingsInt(settings, SETTINGS_LEVEL, 3, 1, 22));
    mCompressionLevelSpin->setToolTip(
        tr("zstd compression level. 1 = fastest / worst compression, 22 = slowest / best. "
           "3 matches zstd's balanced default and is a good choice for interactive sharing.")
    );
    form->addRow(tr("Compression level:"), mCompressionLevelSpin);

    mWorkersSpin = new QSpinBox(this);
    mWorkersSpin->setRange(0, 64);
    mWorkersSpin->setSpecialValueText(tr("Single-threaded"));
    mWorkersSpin->setValue(ClampedSettingsInt(settings, SETTINGS_WORKERS, 0, 0, 64));
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
    // Suppress Qt's built-in Save-As overwrite prompt so `OnAccept`
    // is the sole confirmation site; otherwise Browse-then-Export
    // shows the prompt twice.
    const QString chosen = QFileDialog::getSaveFileName(
        this,
        tr("Export Session Bundle"),
        startPath,
        BundleFileFilter(),
        nullptr,
        QFileDialog::DontConfirmOverwrite
    );
    if (chosen.isEmpty())
    {
        return;
    }
    mDestinationEdit->setText(AppendBundleExtensionIfMissing(chosen));
}

void SessionBundleDialog::OnAccept()
{
    // Emptiness check runs on the *pre-append* text so an empty
    // field is rejected rather than silently promoted to a
    // `.slvbundle` dotfile.
    const QString trimmed = mDestinationEdit->text().trimmed();
    if (trimmed.isEmpty())
    {
        QMessageBox::warning(this, tr("Export Session Bundle"), tr("Please choose a destination file."));
        return;
    }
    const QString dest = AppendBundleExtensionIfMissing(trimmed);
    // Re-check overwrite here too: Qt's `getSaveFileName` prompt only
    // fires for browse-picked paths, so hand-typed paths would slip
    // through.
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
