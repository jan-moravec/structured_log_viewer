#pragma once

#include <QDialog>
#include <QString>

#include <cstdint>
#include <optional>

class QCheckBox;
class QLineEdit;
class QRadioButton;
class QSpinBox;
class QGroupBox;

/// Modal dialog for `MainWindow::OpenNetworkStream`. Lets the user
/// pick a protocol (TCP or UDP), a bind address + port, and (TCP-only,
/// only when the binary is built with TLS support) optional TLS cert /
/// key paths. Settings round-trip through `QSettings` so the next
/// invocation defaults to the previous values.
///
/// The dialog is intentionally programmatically constructed (not from
/// a `.ui` file): the layout is small, gating the TLS group on a
/// build-time `LOGLIB_HAS_TLS` flag is awkward in static UI XML, and
/// keeping it here makes the test surface (constructor + getters)
/// trivially mockable.
class NetworkStreamDialog : public QDialog
{
    Q_OBJECT

public:
    enum class Protocol
    {
        Tcp,
        Udp,
    };

    /// Resolved configuration; populated only after the dialog is
    /// `accept()`ed. Inspect via the public getters below.
    struct Config
    {
        Protocol protocol = Protocol::Tcp;
        QString bindAddress;
        uint16_t port = 0;
        size_t maxConcurrentClients = 16; // TCP-only

        bool tlsEnabled = false;
        QString tlsCertChainPath;
        QString tlsPrivateKeyPath;
        QString tlsCaBundlePath;
        bool tlsRequireClientCertificate = false;
    };

    explicit NetworkStreamDialog(QWidget *parent = nullptr);

    /// Snapshot of the user's choices. Only meaningful after `exec()`
    /// returned `Accepted`.
    [[nodiscard]] Config Configuration() const;

private slots:
    void OnProtocolChanged();
    void OnTlsToggled();
    void BrowseCertChain();
    void BrowsePrivateKey();
    void BrowseCaBundle();
    void Accepted();

private:
    void LoadFromSettings();
    void SaveToSettings() const;

    QRadioButton *mTcpRadio = nullptr;
    QRadioButton *mUdpRadio = nullptr;
    QLineEdit *mBindAddress = nullptr;
    QSpinBox *mPort = nullptr;
    QSpinBox *mMaxConcurrentClients = nullptr;

    QGroupBox *mTlsGroup = nullptr;
    QCheckBox *mTlsEnable = nullptr;
    QLineEdit *mTlsCertPath = nullptr;
    QLineEdit *mTlsKeyPath = nullptr;
    QLineEdit *mTlsCaPath = nullptr;
    QCheckBox *mTlsRequireClientCert = nullptr;

    /// Captured TCP TLS state so we can restore it when the user
    /// toggles UDP -> TCP after disabling TLS in the UDP step.
    /// `OnProtocolChanged` writes the prior TCP TLS-enable state here
    /// before forcing the checkbox off for UDP, then reads it back when
    /// returning to TCP.
    bool mTcpTlsEnableRemembered = false;
};
