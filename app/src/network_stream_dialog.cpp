#include "network_stream_dialog.hpp"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHostAddress>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QRadioButton>
#include <QSettings>
#include <QSpinBox>
#include <QVBoxLayout>

namespace
{
constexpr auto SETTINGS_GROUP = "networkStream";
constexpr auto KEY_PROTOCOL = "protocol";
constexpr auto KEY_BIND = "bindAddress";
constexpr auto KEY_PORT = "port";
constexpr auto KEY_MAX_CLIENTS = "maxConcurrentClients";
constexpr auto KEY_TLS_ENABLED = "tls/enabled";
constexpr auto KEY_TLS_CERT = "tls/certificateChain";
constexpr auto KEY_TLS_KEY = "tls/privateKey";
constexpr auto KEY_TLS_CA = "tls/caBundle";
constexpr auto KEY_TLS_REQUIRE_CLIENT = "tls/requireClientCert";

// Numeric defaults / bounds for the spin boxes. Pulled into named
// constants because clang-tidy's `cppcoreguidelines-avoid-magic-numbers`
// would otherwise flag the literals inline, and naming them also
// documents the intent for the next reader.
constexpr int MAX_PORT_NUMBER = 65535;
constexpr int DEFAULT_PORT_NUMBER = 5141;
constexpr int MAX_CONCURRENT_CLIENTS_UPPER = 4096;
constexpr int DEFAULT_MAX_CONCURRENT_CLIENTS = 16;

/// Add a "..." browse button next to @p edit that opens a file picker
/// rooted at the edit's current text and writes the selection back.
/// `parent` becomes the dialog's parent so the file picker is modal
/// over the network-stream dialog. @p accessibleName is set on the
/// button so screen readers can disambiguate the three TLS-row Browse
/// buttons (which all share the same visible label).
QHBoxLayout *PathRow(
    QWidget *parent,
    QLineEdit *edit,
    void (NetworkStreamDialog::*browseSlot)(),
    QObject *receiver,
    const QString &accessibleName
)
{
    auto *row = new QHBoxLayout();
    row->setContentsMargins(0, 0, 0, 0);
    row->addWidget(edit, 1);
    auto *button = new QPushButton(QObject::tr("Browse..."), parent);
    button->setAccessibleName(accessibleName);
    button->setToolTip(accessibleName);
    QObject::connect(button, &QPushButton::clicked, receiver, browseSlot);
    row->addWidget(button);
    return row;
}

} // namespace

NetworkStreamDialog::NetworkStreamDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Open Network Stream"));
    setModal(true);

    auto *outerLayout = new QVBoxLayout(this);

    // Protocol picker.
    auto *protoBox = new QGroupBox(tr("Protocol"), this);
    auto *protoLayout = new QHBoxLayout(protoBox);
    mTcpRadio = new QRadioButton(tr("TCP"), protoBox);
    mUdpRadio = new QRadioButton(tr("UDP"), protoBox);
    mTcpRadio->setChecked(true);
    protoLayout->addWidget(mTcpRadio);
    protoLayout->addWidget(mUdpRadio);
    protoLayout->addStretch(1);
    outerLayout->addWidget(protoBox);

    connect(mTcpRadio, &QRadioButton::toggled, this, &NetworkStreamDialog::OnProtocolChanged);

    // Common bind / port / max-clients form.
    auto *bindBox = new QGroupBox(tr("Bind"), this);
    auto *bindForm = new QFormLayout(bindBox);

    mBindAddress = new QLineEdit("0.0.0.0", bindBox);
    mBindAddress->setToolTip(tr("0.0.0.0 = IPv4-any, :: = IPv6-any dual-stack, 127.0.0.1 = loopback only"));
    mBindAddress->setAccessibleName(tr("Bind address"));
    bindForm->addRow(tr("Bind address:"), mBindAddress);

    mPort = new QSpinBox(bindBox);
    mPort->setRange(0, MAX_PORT_NUMBER);
    mPort->setValue(DEFAULT_PORT_NUMBER);
    mPort->setToolTip(tr("0 requests an OS-assigned ephemeral port (useful for testing)"));
    mPort->setAccessibleName(tr("Bind port"));
    bindForm->addRow(tr("Port:"), mPort);

    mMaxConcurrentClients = new QSpinBox(bindBox);
    mMaxConcurrentClients->setRange(1, MAX_CONCURRENT_CLIENTS_UPPER);
    mMaxConcurrentClients->setValue(DEFAULT_MAX_CONCURRENT_CLIENTS);
    mMaxConcurrentClients->setToolTip(tr("TCP only: maximum simultaneously-connected clients"));
    mMaxConcurrentClients->setAccessibleName(tr("Maximum concurrent TCP clients"));
    bindForm->addRow(tr("Max concurrent clients (TCP):"), mMaxConcurrentClients);

    outerLayout->addWidget(bindBox);

    // TLS group. The widgets are always created so the layout doesn't
    // shift between TLS-on / TLS-off builds, but the whole group is
    // disabled (and a "not built in" hint shown) when LOGLIB_HAS_TLS
    // is undefined. Selecting UDP also disables it (TLS is TCP-only;
    // DTLS is out of scope per the design).
    mTlsGroup = new QGroupBox(tr("TLS (TCP only)"), this);
    auto *tlsLayout = new QFormLayout(mTlsGroup);
    mTlsEnable = new QCheckBox(tr("Enable TLS"), mTlsGroup);
    mTlsEnable->setAccessibleName(tr("Enable TLS for this TCP listener"));
    tlsLayout->addRow(mTlsEnable);

    mTlsCertPath = new QLineEdit(mTlsGroup);
    mTlsCertPath->setPlaceholderText(tr("PEM file with the server certificate chain"));
    mTlsCertPath->setAccessibleName(tr("TLS certificate chain path"));
    tlsLayout->addRow(
        tr("Certificate chain:"),
        PathRow(this, mTlsCertPath, &NetworkStreamDialog::BrowseCertChain, this, tr("Browse for certificate chain"))
    );

    mTlsKeyPath = new QLineEdit(mTlsGroup);
    mTlsKeyPath->setPlaceholderText(tr("PEM file with the server private key"));
    mTlsKeyPath->setAccessibleName(tr("TLS private key path"));
    tlsLayout->addRow(
        tr("Private key:"),
        PathRow(this, mTlsKeyPath, &NetworkStreamDialog::BrowsePrivateKey, this, tr("Browse for private key"))
    );

    mTlsCaPath = new QLineEdit(mTlsGroup);
    mTlsCaPath->setPlaceholderText(tr("Optional PEM CA bundle for client cert verification"));
    mTlsCaPath->setAccessibleName(tr("TLS CA bundle path"));
    tlsLayout->addRow(
        tr("CA bundle:"), PathRow(this, mTlsCaPath, &NetworkStreamDialog::BrowseCaBundle, this, tr("Browse for CA bundle"))
    );

    mTlsRequireClientCert = new QCheckBox(tr("Require valid client certificate"), mTlsGroup);
    mTlsRequireClientCert->setAccessibleName(tr("Require valid client certificate"));
    tlsLayout->addRow(mTlsRequireClientCert);

#ifndef LOGLIB_HAS_TLS
    // Surface the build-time absence so users do not silently lose
    // TLS when they expect it. `setEnabled(false)` greys the whole
    // group; the tooltip explains why.
    mTlsGroup->setEnabled(false);
    mTlsGroup->setToolTip(tr("This build was compiled without LOGLIB_NETWORK_TLS=ON; rebuild with TLS to enable."));
#endif

    connect(mTlsEnable, &QCheckBox::toggled, this, &NetworkStreamDialog::OnTlsToggled);

    outerLayout->addWidget(mTlsGroup);

    // OK / Cancel.
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &NetworkStreamDialog::Accepted);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    outerLayout->addWidget(buttons);

    LoadFromSettings();
    // Seed the remembered TCP TLS toggle from whatever LoadFromSettings
    // applied, so the first UDP -> TCP roundtrip restores the same
    // value. Without this seed, switching to UDP first and back to TCP
    // would always restore "off" regardless of the persisted choice.
    mTcpTlsEnableRemembered = mTlsEnable->isChecked();
    OnProtocolChanged();
    OnTlsToggled();
}

void NetworkStreamDialog::OnProtocolChanged()
{
    const bool isTcp = mTcpRadio->isChecked();
    mMaxConcurrentClients->setEnabled(isTcp);
#ifdef LOGLIB_HAS_TLS
    constexpr bool TLS_BUILT_IN = true;
#else
    constexpr bool TLS_BUILT_IN = false;
#endif
    mTlsGroup->setEnabled(isTcp && TLS_BUILT_IN);
    if (!isTcp)
    {
        // Remember whatever TCP-side TLS choice the user had so we
        // can put it back on the next TCP toggle. Forcing the
        // checkbox off avoids surprising the user with an enabled-but-
        // disabled TLS state when they later return to TCP.
        if (mTlsEnable->isChecked())
        {
            mTcpTlsEnableRemembered = true;
            mTlsEnable->setChecked(false);
        }
    }
    else if (mTcpTlsEnableRemembered && TLS_BUILT_IN)
    {
        mTlsEnable->setChecked(true);
    }
    OnTlsToggled();
}

void NetworkStreamDialog::OnTlsToggled()
{
    const bool tlsActive = mTlsGroup->isEnabled() && mTlsEnable->isChecked();
    mTlsCertPath->setEnabled(tlsActive);
    mTlsKeyPath->setEnabled(tlsActive);
    mTlsCaPath->setEnabled(tlsActive);
    mTlsRequireClientCert->setEnabled(tlsActive);
}

void NetworkStreamDialog::BrowseCertChain()
{
    const QString picked = QFileDialog::getOpenFileName(
        this, tr("Select Certificate Chain (PEM)"), mTlsCertPath->text(), tr("PEM files (*.pem *.crt);;All files (*)")
    );
    if (!picked.isEmpty())
    {
        mTlsCertPath->setText(picked);
    }
}

void NetworkStreamDialog::BrowsePrivateKey()
{
    const QString picked = QFileDialog::getOpenFileName(
        this, tr("Select Private Key (PEM)"), mTlsKeyPath->text(), tr("PEM files (*.pem *.key);;All files (*)")
    );
    if (!picked.isEmpty())
    {
        mTlsKeyPath->setText(picked);
    }
}

void NetworkStreamDialog::BrowseCaBundle()
{
    const QString picked = QFileDialog::getOpenFileName(
        this, tr("Select CA Bundle (PEM)"), mTlsCaPath->text(), tr("PEM files (*.pem *.crt);;All files (*)")
    );
    if (!picked.isEmpty())
    {
        mTlsCaPath->setText(picked);
    }
}

void NetworkStreamDialog::Accepted()
{
    // Front-load every validation that would otherwise surface as a
    // post-accept exception inside `MainWindow::OpenNetworkStream`
    // (where the dialog has already been dismissed and the bad config
    // persisted to QSettings). Each failure pops a warning and
    // returns; the dialog stays open so the user can fix the field.
    const QString bind = mBindAddress->text().trimmed();
    if (bind.isEmpty())
    {
        QMessageBox::warning(this, tr("Open Network Stream"), tr("Bind address must not be empty."));
        mBindAddress->setFocus();
        return;
    }
    QHostAddress probe;
    if (!probe.setAddress(bind))
    {
        QMessageBox::warning(
            this,
            tr("Open Network Stream"),
            tr("Bind address '%1' is not a valid IPv4 or IPv6 literal.\n\n"
               "Use 0.0.0.0 / :: to listen on all interfaces, or 127.0.0.1 / ::1 for loopback only.")
                .arg(bind)
        );
        mBindAddress->setFocus();
        return;
    }

    if (mTcpRadio->isChecked() && mTlsGroup->isEnabled() && mTlsEnable->isChecked())
    {
        const QString cert = mTlsCertPath->text().trimmed();
        const QString key = mTlsKeyPath->text().trimmed();
        if (cert.isEmpty() || key.isEmpty())
        {
            QMessageBox::warning(
                this,
                tr("Open Network Stream"),
                tr("TLS is enabled but the certificate chain and private key are required.")
            );
            (cert.isEmpty() ? mTlsCertPath : mTlsKeyPath)->setFocus();
            return;
        }
        // Path-existence checks: cheap, run on the GUI thread, and
        // give a clearer diagnostic than OpenSSL's "cannot open file"
        // returned from the producer constructor.
        if (!QFileInfo::exists(cert))
        {
            QMessageBox::warning(
                this, tr("Open Network Stream"), tr("Certificate chain file does not exist:\n%1").arg(cert)
            );
            mTlsCertPath->setFocus();
            return;
        }
        if (!QFileInfo::exists(key))
        {
            QMessageBox::warning(this, tr("Open Network Stream"), tr("Private key file does not exist:\n%1").arg(key));
            mTlsKeyPath->setFocus();
            return;
        }
        const QString ca = mTlsCaPath->text().trimmed();
        if (!ca.isEmpty() && !QFileInfo::exists(ca))
        {
            QMessageBox::warning(this, tr("Open Network Stream"), tr("CA bundle file does not exist:\n%1").arg(ca));
            mTlsCaPath->setFocus();
            return;
        }
    }

    SaveToSettings();
    accept();
}

void NetworkStreamDialog::LoadFromSettings()
{
    QSettings settings;
    settings.beginGroup(SETTINGS_GROUP);
    const QString protocolName = settings.value(KEY_PROTOCOL, "tcp").toString();
    if (protocolName.compare("udp", Qt::CaseInsensitive) == 0)
    {
        mUdpRadio->setChecked(true);
    }
    else
    {
        mTcpRadio->setChecked(true);
    }
    mBindAddress->setText(settings.value(KEY_BIND, mBindAddress->text()).toString());
    mPort->setValue(settings.value(KEY_PORT, mPort->value()).toInt());
    mMaxConcurrentClients->setValue(settings.value(KEY_MAX_CLIENTS, mMaxConcurrentClients->value()).toInt());

    mTlsEnable->setChecked(settings.value(KEY_TLS_ENABLED, false).toBool());
    mTlsCertPath->setText(settings.value(KEY_TLS_CERT).toString());
    mTlsKeyPath->setText(settings.value(KEY_TLS_KEY).toString());
    mTlsCaPath->setText(settings.value(KEY_TLS_CA).toString());
    mTlsRequireClientCert->setChecked(settings.value(KEY_TLS_REQUIRE_CLIENT, false).toBool());
    settings.endGroup();
}

void NetworkStreamDialog::SaveToSettings() const
{
    QSettings settings;
    settings.beginGroup(SETTINGS_GROUP);
    settings.setValue(KEY_PROTOCOL, mTcpRadio->isChecked() ? "tcp" : "udp");
    settings.setValue(KEY_BIND, mBindAddress->text());
    settings.setValue(KEY_PORT, mPort->value());
    settings.setValue(KEY_MAX_CLIENTS, mMaxConcurrentClients->value());
    // Persist the *intended* TCP TLS choice so the next session
    // restores it even if the user happened to be on UDP at accept
    // time (where the live checkbox is forced off).
    const bool persistedTlsEnabled =
        mTcpRadio->isChecked() ? mTlsEnable->isChecked() : mTcpTlsEnableRemembered;
    settings.setValue(KEY_TLS_ENABLED, persistedTlsEnabled);
    settings.setValue(KEY_TLS_CERT, mTlsCertPath->text());
    settings.setValue(KEY_TLS_KEY, mTlsKeyPath->text());
    settings.setValue(KEY_TLS_CA, mTlsCaPath->text());
    settings.setValue(KEY_TLS_REQUIRE_CLIENT, mTlsRequireClientCert->isChecked());
    settings.endGroup();
}

NetworkStreamDialog::Config NetworkStreamDialog::Configuration() const
{
    Config out;
    out.protocol = mTcpRadio->isChecked() ? Protocol::Tcp : Protocol::Udp;
    out.bindAddress = mBindAddress->text();
    out.port = static_cast<uint16_t>(mPort->value());
    out.maxConcurrentClients = static_cast<size_t>(mMaxConcurrentClients->value());
    out.tlsEnabled = mTlsGroup->isEnabled() && mTlsEnable->isChecked();
    out.tlsCertChainPath = mTlsCertPath->text();
    out.tlsPrivateKeyPath = mTlsKeyPath->text();
    out.tlsCaBundlePath = mTlsCaPath->text();
    out.tlsRequireClientCertificate = mTlsRequireClientCert->isChecked();
    return out;
}
