#pragma once

#include <QDialog>
#include <QString>

#include <cstdint>
#include <optional>

class QCheckBox;
class QComboBox;
class QLineEdit;
class QRadioButton;
class QSpinBox;
class QGroupBox;
class RegexTemplateRegistry;

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

    /// Wire format on the socket. Network streams have nothing on
    /// disk to sniff, so the parser must be picked here. The choice
    /// round-trips through `QSettings` so the next session defaults
    /// to the previous pick.
    enum class Format
    {
        Json,
        Logfmt,
        Csv,
        /// Regex template (PCRE2). Requires picking a built-in
        /// template from the registry or providing a custom
        /// `(?<Name>...)` pattern.
        Regex,
    };

    /// Resolved configuration; populated only after the dialog is
    /// `accept()`ed. Inspect via the public getters below.
    struct Config
    {
        Protocol protocol = Protocol::Tcp;
        Format format = Format::Json;
        /// PCRE2 pattern, only populated when `format == Regex`.
        /// Either the pattern of the selected built-in template or
        /// the free-text user-supplied pattern.
        QString regexPattern;
        QString bindAddress;
        uint16_t port = 0;
        size_t maxConcurrentClients = 16; // TCP-only

        bool tlsEnabled = false;
        QString tlsCertChainPath;
        QString tlsPrivateKeyPath;
        QString tlsCaBundlePath;
        bool tlsRequireClientCertificate = false;
    };

    /// @p registry, if non-null, is the source for the regex
    /// template picker (built-ins ∪ user templates). The dialog
    /// snapshots it at construction; subsequent registry changes
    /// don't update the open dialog. Pass nullptr to fall back to
    /// the library's built-in catalog only (used by tests and the
    /// minimal-fixture entry points).
    explicit NetworkStreamDialog(RegexTemplateRegistry *registry = nullptr, QWidget *parent = nullptr);

    /// Snapshot of the user's choices. Only meaningful after `exec()`
    /// returned `Accepted`.
    [[nodiscard]] Config Configuration() const;

private slots:
    void OnProtocolChanged();
    void OnTlsToggled();
    void OnFormatChanged();
    void OnRegexTemplateChanged();
    void BrowseCertChain();
    void BrowsePrivateKey();
    void BrowseCaBundle();
    void Accepted();

private:
    void LoadFromSettings();
    void SaveToSettings() const;

    QRadioButton *mTcpRadio = nullptr;
    QRadioButton *mUdpRadio = nullptr;
    QComboBox *mFormat = nullptr;
    /// Regex template picker; visible only when `Format::Regex` is
    /// selected. Combobox userData carries the template's display
    /// `name` (stable across rebuilds); the sentinel "Custom..."
    /// entry uses an empty string to enable `mRegexPattern` below.
    /// Template lifecycle (create / edit / delete) lives in the
    /// dedicated `RegexTemplatesEditor` opened from
    /// `Settings -> Regex templates...`; this dialog only consumes
    /// the catalog at construction time.
    QGroupBox *mRegexGroup = nullptr;
    QComboBox *mRegexTemplate = nullptr;
    QLineEdit *mRegexPattern = nullptr;
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

    /// Owning app's regex template registry (built-ins + user
    /// templates). May be null in tests; falls back to the library's
    /// built-in catalog when so. Non-owning; the registry outlives
    /// the dialog (constructed once in `main()`).
    RegexTemplateRegistry *mRegistry = nullptr;
};
