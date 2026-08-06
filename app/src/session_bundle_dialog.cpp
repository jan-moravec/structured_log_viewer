#include "session_bundle_dialog.hpp"

#include <loglib/session_bundle.hpp>

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLayout>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QThread>
#include <QVBoxLayout>

#include <algorithm>

namespace
{

constexpr int MIN_ZSTD_LEVEL = 1;
constexpr int MAX_ZSTD_LEVEL = 22;
constexpr int DEFAULT_ZSTD_LEVEL = 3;
constexpr int MAX_WORKER_THREADS = 64;
constexpr int WORKER_THREAD_CAP = 8;
constexpr int DIALOG_PREFERRED_WIDTH = 560;
/// Align the advanced form with the checkbox label.
constexpr int ADVANCED_FORM_INDENT_PX = 16;

/// QSettings keys for the bundle-encoder knobs.
constexpr auto SETTINGS_LEVEL = "session_bundle/compression_level";
constexpr auto SETTINGS_WORKERS = "session_bundle/workers";
constexpr auto SETTINGS_LAST_DIR = "session_bundle/last_dir";

/// Use up to `WORKER_THREAD_CAP` workers; zero means single-threaded.
int DefaultWorkerThreads() noexcept
{
    const int ideal = QThread::idealThreadCount();
    if (ideal <= 1)
    {
        return 0;
    }
    return std::min(ideal, WORKER_THREAD_CAP);
}

QString BundleFileFilter()
{
    return QStringLiteral("Session bundle (*%1);;All Files (*)").arg(loglib::SESSION_BUNDLE_EXTENSION);
}

/// Append `.slvbundle` when needed, preserving empty input.
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

/// Read a bounded setting or return @p defaultValue.
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
    const QString &defaultStem,
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

    mPreviewLabel = new QLabel(this);
    mPreviewLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    mPreviewLabel->setText(
        tr("Exports %L1 rows with the current filter, sort, anchors, and highlight rules.")
            .arg(static_cast<qulonglong>(rowCount))
    );
    mPreviewLabel->setWordWrap(true);
    form->addRow(mPreviewLabel);

    mLiveTailNote = new QLabel(this);
    mLiveTailNote->setText(
        tr("Live-tail snapshot: the tail session stops when the export starts. Existing rows are "
           "captured in the bundle, but subsequent streaming from the tailed source is not preserved.")
    );
    mLiveTailNote->setWordWrap(true);
    mLiveTailNote->setVisible(isLiveTail);
    form->addRow(mLiveTailNote);

    // A sibling container collapses without `QGroupBox` title padding.
    mAdvancedToggle = new QCheckBox(tr("Advanced options"), this);
    mAdvancedToggle->setChecked(false);

    mAdvancedContainer = new QWidget(this);
    auto *advancedForm = new QFormLayout(mAdvancedContainer);
    // Visually nest the form under the disclosure checkbox.
    advancedForm->setContentsMargins(ADVANCED_FORM_INDENT_PX, 0, 0, 0);

    mCompressionLevelSpin = new QSpinBox(mAdvancedContainer);
    mCompressionLevelSpin->setRange(MIN_ZSTD_LEVEL, MAX_ZSTD_LEVEL);
    mCompressionLevelSpin->setValue(
        ClampedSettingsInt(settings, SETTINGS_LEVEL, DEFAULT_ZSTD_LEVEL, MIN_ZSTD_LEVEL, MAX_ZSTD_LEVEL)
    );
    mCompressionLevelSpin->setToolTip(
        tr("zstd compression level. 1 = fastest / worst compression, 22 = slowest / best. "
           "3 matches zstd's balanced default and is a good choice for interactive sharing.")
    );
    advancedForm->addRow(tr("Compression level:"), mCompressionLevelSpin);

    mWorkersSpin = new QSpinBox(mAdvancedContainer);
    mWorkersSpin->setRange(0, MAX_WORKER_THREADS);
    mWorkersSpin->setSpecialValueText(tr("Single-threaded"));
    mWorkersSpin->setValue(
        ClampedSettingsInt(settings, SETTINGS_WORKERS, DefaultWorkerThreads(), 0, MAX_WORKER_THREADS)
    );
    mWorkersSpin->setToolTip(
        tr("zstd worker threads for the single bundle frame. "
           "0 uses zstd's single-threaded path; positive values enable zstd multi-threading.")
    );
    advancedForm->addRow(tr("Worker threads:"), mWorkersSpin);

    mAdvancedContainer->setVisible(false);

    connect(mAdvancedToggle, &QCheckBox::toggled, this, [this](bool checked) {
        mAdvancedContainer->setVisible(checked);
        // Refresh the height immediately while preserving user-set width.
        if (auto *lay = layout())
        {
            lay->activate();
        }
        resize(width(), sizeHint().height());
    });

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("Export"));
    connect(buttons, &QDialogButtonBox::accepted, this, &SessionBundleDialog::OnAccept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addWidget(mAdvancedToggle);
    layout->addWidget(mAdvancedContainer);
    layout->addWidget(buttons);

    resize(DIALOG_PREFERRED_WIDTH, sizeHint().height());
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
    // `OnAccept` handles overwrite confirmation for all paths.
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
    // Validate before appending the extension.
    const QString trimmed = mDestinationEdit->text().trimmed();
    if (trimmed.isEmpty())
    {
        QMessageBox::warning(this, tr("Export Session Bundle"), tr("Please choose a destination file."));
        return;
    }
    const QString dest = AppendBundleExtensionIfMissing(trimmed);
    // Confirm both browsed and manually entered destinations here.
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
