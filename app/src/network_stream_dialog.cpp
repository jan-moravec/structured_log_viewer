#include "network_stream_dialog.hpp"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
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
constexpr auto KEY_MAX_CLIENTS = "maxClients";
constexpr auto KEY_TLS_ENABLED = "tls/enabled";
constexpr auto KEY_TLS_CERT = "tls/certificateChain";
constexpr auto KEY_TLS_KEY = "tls/privateKey";
constexpr auto KEY_TLS_CA = "tls/caBundle";
constexpr auto KEY_TLS_REQUIRE_CLIENT = "tls/requireClientCert";

/// Add a "..." browse button next to @p edit that opens a file picker
/// rooted at the edit's current text and writes the selection back.
/// `parent` becomes the dialog's parent so the file picker is modal
/// over the network-stream dialog.
QHBoxLayout *PathRow(QWidget *parent, QLineEdit *edit, void (NetworkStreamDialog::*browseSlot)(), QObject *receiver)
{
    auto *row = new QHBoxLayout();
    row->setContentsMargins(0, 0, 0, 0);
    row->addWidget(edit, 1);
    auto *button = new QPushButton(QObject::tr("Browse..."), parent);
    QObject::connect(button, &QPushButton::clicked, receiver, browseSlot);
    row->addWidget(button);
    return row;
}

} // namespace

NetworkStreamDialog::NetworkStreamDialog(QWidget *parent) : QDialog(parent)
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
    bindForm->addRow(tr("Bind address:"), mBindAddress);

    mPort = new QSpinBox(bindBox);
    mPort->setRange(0, 65535);
    mPort->setValue(5141);
    mPort->setToolTip(tr("0 requests an OS-assigned ephemeral port (useful for testing)"));
    bindForm->addRow(tr("Port:"), mPort);

    mMaxClients = new QSpinBox(bindBox);
    mMaxClients->setRange(1, 4096);
    mMaxClients->setValue(16);
    mMaxClients->setToolTip(tr("TCP only: maximum simultaneously-connected clients"));
    bindForm->addRow(tr("Max clients (TCP):"), mMaxClients);

    outerLayout->addWidget(bindBox);

    // TLS group. The widgets are always created so the layout doesn't
    // shift between TLS-on / TLS-off builds, but the whole group is
    // disabled (and a "not built in" hint shown) when LOGLIB_HAS_TLS
    // is undefined. Selecting UDP also disables it (TLS is TCP-only;
    // DTLS is out of scope per the design).
    mTlsGroup = new QGroupBox(tr("TLS (TCP only)"), this);
    auto *tlsLayout = new QFormLayout(mTlsGroup);
    mTlsEnable = new QCheckBox(tr("Enable TLS"), mTlsGroup);
    tlsLayout->addRow(mTlsEnable);

    mTlsCertPath = new QLineEdit(mTlsGroup);
    mTlsCertPath->setPlaceholderText(tr("PEM file with the server certificate chain"));
    tlsLayout->addRow(tr("Certificate chain:"), PathRow(this, mTlsCertPath, &NetworkStreamDialog::BrowseCertChain, this));

    mTlsKeyPath = new QLineEdit(mTlsGroup);
    mTlsKeyPath->setPlaceholderText(tr("PEM file with the server private key"));
    tlsLayout->addRow(tr("Private key:"), PathRow(this, mTlsKeyPath, &NetworkStreamDialog::BrowsePrivateKey, this));

    mTlsCaPath = new QLineEdit(mTlsGroup);
    mTlsCaPath->setPlaceholderText(tr("Optional PEM CA bundle for client cert verification"));
    tlsLayout->addRow(tr("CA bundle:"), PathRow(this, mTlsCaPath, &NetworkStreamDialog::BrowseCaBundle, this));

    mTlsRequireClientCert = new QCheckBox(tr("Require valid client certificate"), mTlsGroup);
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
    OnProtocolChanged();
    OnTlsToggled();
}

void NetworkStreamDialog::OnProtocolChanged()
{
    const bool isTcp = mTcpRadio->isChecked();
    mMaxClients->setEnabled(isTcp);
    // TLS is TCP-only; DTLS is out of scope.
    mTlsGroup->setEnabled(isTcp
#ifdef LOGLIB_HAS_TLS
                          && true
#else
                          && false
#endif
    );
    if (!isTcp)
    {
        mTlsEnable->setChecked(false);
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
    mMaxClients->setValue(settings.value(KEY_MAX_CLIENTS, mMaxClients->value()).toInt());

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
    settings.setValue(KEY_MAX_CLIENTS, mMaxClients->value());
    settings.setValue(KEY_TLS_ENABLED, mTlsEnable->isChecked());
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
    out.maxClients = static_cast<size_t>(mMaxClients->value());
    out.tlsEnabled = mTlsGroup->isEnabled() && mTlsEnable->isChecked();
    out.tlsCertChainPath = mTlsCertPath->text();
    out.tlsPrivateKeyPath = mTlsKeyPath->text();
    out.tlsCaBundlePath = mTlsCaPath->text();
    out.tlsRequireClientCertificate = mTlsRequireClientCert->isChecked();
    return out;
}
