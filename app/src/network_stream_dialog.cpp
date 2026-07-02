#include "network_stream_dialog.hpp"

#include "regex_template_registry.hpp"

#include <loglib/parsers/regex_parser.hpp>
#include <loglib/regex_templates.hpp>

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHostAddress>
#include <QLabel>
#include <QLineEdit>
#include <QList>
#include <QMessageBox>
#include <QPushButton>
#include <QRadioButton>
#include <QSettings>
#include <QSpinBox>
#include <QVBoxLayout>

#include <optional>

namespace
{
constexpr auto SETTINGS_GROUP = "networkStream";
constexpr auto KEY_PROTOCOL = "protocol";
constexpr auto KEY_FORMAT = "format";
constexpr auto KEY_BIND = "bindAddress";
constexpr auto KEY_PORT = "port";
constexpr auto KEY_MAX_CLIENTS = "maxConcurrentClients";
constexpr auto KEY_TLS_ENABLED = "tls/enabled";
constexpr auto KEY_TLS_CERT = "tls/certificateChain";
constexpr auto KEY_TLS_KEY = "tls/privateKey";
constexpr auto KEY_TLS_CA = "tls/caBundle";
constexpr auto KEY_TLS_REQUIRE_CLIENT = "tls/requireClientCert";
constexpr auto KEY_REGEX_PATTERN = "regex/pattern";

/// Sentinel combobox userData that means "use the custom-pattern
/// line edit". Real templates carry their `name` as userData, so
/// the sentinel is the empty QString — no `name` field can ever
/// resolve to empty (the registry back-fills a basename otherwise).
/// A function (not a static constant) so no QString lives in static
/// storage (clang-tidy `cert-err58-cpp` flags throwing static init).
[[nodiscard]] QString RegexTemplateCustomData()
{
    return QString{};
}

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

NetworkStreamDialog::NetworkStreamDialog(RegexTemplateRegistry *registry, QWidget *parent)
    : QDialog(parent), mRegistry(registry)
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

    // Format picker. No file to sniff for a network stream, so the
    // user picks up front. Combobox (not radios) leaves room for
    // future formats.
    auto *formatBox = new QGroupBox(tr("Format"), this);
    auto *formatLayout = new QHBoxLayout(formatBox);
    mFormat = new QComboBox(formatBox);
    mFormat->addItem(tr("JSON Lines"), QVariant::fromValue(static_cast<int>(Format::Json)));
    mFormat->addItem(tr("logfmt"), QVariant::fromValue(static_cast<int>(Format::Logfmt)));
    mFormat->addItem(tr("CSV"), QVariant::fromValue(static_cast<int>(Format::Csv)));
    mFormat->addItem(tr("Regex template"), QVariant::fromValue(static_cast<int>(Format::Regex)));
    mFormat->setToolTip(tr("Wire format of the bytes flowing over the socket. "
                           "For CSV, the first inbound line is treated as the header; if multiple TCP clients "
                           "connect, the first arriving line sets the column schema for every client \u2014 "
                           "coordinate the header across producers or restrict CSV to a single producer. "
                           "Regex template lets you pick a built-in PCRE2 pattern (syslog, Apache/nginx access "
                           "log, ...) or supply a custom one with named capture groups."));
    mFormat->setAccessibleName(tr("Wire format"));
    formatLayout->addWidget(mFormat);
    formatLayout->addStretch(1);
    outerLayout->addWidget(formatBox);
    connect(mFormat, &QComboBox::currentIndexChanged, this, &NetworkStreamDialog::OnFormatChanged);

    // Regex template picker + custom-pattern field. Hidden unless
    // `Format::Regex` is selected. Populated from the merged
    // registry (built-ins ∪ user templates) when available,
    // otherwise from the library's built-in catalog (test path).
    // The trailing "Custom..." entry unlocks the line-edit for a
    // user-supplied pattern.
    mRegexGroup = new QGroupBox(tr("Regex template"), this);
    auto *regexLayout = new QFormLayout(mRegexGroup);
    mRegexTemplate = new QComboBox(mRegexGroup);
    if (mRegistry != nullptr)
    {
        const QList<RegexTemplateRegistry::Listing> entries = mRegistry->Available();
        for (const auto &row : entries)
        {
            QString label = row.name;
            if (row.fromUser)
            {
                label = tr("%1 (user)").arg(row.name);
            }
            if (!row.autoDetect)
            {
                label = tr("%1 (manual only)").arg(label);
            }
            mRegexTemplate->addItem(label, QVariant(row.name));
        }
    }
    else
    {
        for (const loglib::RegexTemplate &t : loglib::BuiltinRegexTemplates())
        {
            const QString name = QString::fromStdString(t.name);
            mRegexTemplate->addItem(name, QVariant(name));
        }
    }
    mRegexTemplate->addItem(tr("Custom..."), QVariant(RegexTemplateCustomData()));
    mRegexTemplate->setToolTip(tr("Pick a built-in or user PCRE2 template, or 'Custom...' to write your own. "
                                  "User templates live in <AppData>/regex_templates/*.json and shadow built-ins "
                                  "with the same name."));
    mRegexTemplate->setAccessibleName(tr("Regex template"));
    regexLayout->addRow(tr("Template:"), mRegexTemplate);

    mRegexPattern = new QLineEdit(mRegexGroup);
    mRegexPattern->setPlaceholderText(tr(R"(PCRE2 pattern, e.g. ^(?<Level>\w+) (?<Message>.*)$)"));
    mRegexPattern->setToolTip(tr("PCRE2 regex with `(?<Name>...)` named capture groups; each group becomes a column. "
                                 "Read-only when a built-in template is selected (the pattern preview comes from "
                                 "the registry)."));
    mRegexPattern->setAccessibleName(tr("Regex pattern"));
    regexLayout->addRow(tr("Pattern:"), mRegexPattern);

    // Signpost pointing at the dedicated editor. Template CRUD
    // lives behind `Settings -> Regex templates...`; showing the
    // hint here saves the user from hunting through menus when
    // they want to save a tweaked pattern.
    auto *templatesHint = new QLabel(
        tr("Manage templates from <b>Settings &rarr; Regex templates...</b>"), mRegexGroup
    );
    templatesHint->setToolTip(tr("Use the dedicated editor (Settings menu) to create, edit, validate, or delete "
                                 "regex templates. This dialog only consumes the catalog."));
    templatesHint->setTextFormat(Qt::RichText);
    templatesHint->setWordWrap(true);
    regexLayout->addRow(QString{}, templatesHint);

    outerLayout->addWidget(mRegexGroup);
    connect(mRegexTemplate, &QComboBox::currentIndexChanged, this, &NetworkStreamDialog::OnRegexTemplateChanged);

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
        tr("CA bundle:"),
        PathRow(this, mTlsCaPath, &NetworkStreamDialog::BrowseCaBundle, this, tr("Browse for CA bundle"))
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
    // Sync initial regex-group visibility and pattern preview to
    // the loaded format / template selection.
    OnFormatChanged();
    OnRegexTemplateChanged();
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
        // can put it back on the next TCP toggle. Capture
        // unconditionally: gating on `mTlsEnable->isChecked()` would
        // leave a stale `true` here if the user disabled TLS on TCP
        // before switching to UDP, which would then re-enable TLS on
        // the next UDP -> TCP roundtrip and (worse) persist that
        // stale `true` to QSettings via `SaveToSettings`. Forcing the
        // checkbox off keeps the UDP view tidy regardless.
        mTcpTlsEnableRemembered = mTlsEnable->isChecked();
        mTlsEnable->setChecked(false);
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

void NetworkStreamDialog::OnFormatChanged()
{
    const bool isRegex = mFormat->currentData().toInt() == static_cast<int>(Format::Regex);
    mRegexGroup->setVisible(isRegex);
}

void NetworkStreamDialog::OnRegexTemplateChanged()
{
    const QString templateName = mRegexTemplate->currentData().toString();
    if (templateName.isEmpty())
    {
        // "Custom..." sentinel: hand the field back to the user
        // without clobbering whatever they already typed (a prior
        // custom session may have persisted text worth keeping).
        mRegexPattern->setReadOnly(false);
        return;
    }
    // Resolve by name. Prefer the merged registry (built-ins ∪
    // user templates); fall back to the library catalog when
    // constructed without one (tests).
    if (mRegistry != nullptr)
    {
        const auto tmpl = mRegistry->Load(templateName);
        if (tmpl.has_value())
        {
            mRegexPattern->setText(QString::fromStdString(tmpl->pattern));
        }
    }
    else
    {
        const std::string stdName = templateName.toStdString();
        for (const loglib::RegexTemplate &t : loglib::BuiltinRegexTemplates())
        {
            if (t.name == stdName)
            {
                mRegexPattern->setText(QString::fromStdString(t.pattern));
                break;
            }
        }
    }
    mRegexPattern->setReadOnly(true);
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
    if (mFormat->currentData().toInt() == static_cast<int>(Format::Regex))
    {
        // Trim early so the same value is persisted, handed to
        // `MainWindow::OpenNetworkStream`, and compiled by PCRE2
        // below. Without this, leading/trailing whitespace would
        // leak into the saved pattern (and the parser at restore).
        const QString trimmedPattern = mRegexPattern->text().trimmed();
        if (trimmedPattern != mRegexPattern->text())
        {
            mRegexPattern->setText(trimmedPattern);
        }
        if (trimmedPattern.isEmpty())
        {
            QMessageBox::warning(
                this,
                tr("Open Network Stream"),
                tr("A regex pattern is required when the wire format is 'Regex template'. "
                   "Pick a built-in template or write a custom PCRE2 pattern with `(?<Name>...)` groups.")
            );
            mRegexPattern->setFocus();
            return;
        }
        // Pre-compile so syntax errors and "no named capture
        // groups" surface here rather than as a single error on
        // the first inbound line (at which point the dialog is
        // gone and the bad pattern already persisted).
        std::string regexError;
        if (!loglib::ValidateRegexPattern(trimmedPattern.toStdString(), regexError))
        {
            QMessageBox::warning(
                this,
                tr("Open Network Stream"),
                tr("The regex pattern is not valid:\n\n%1").arg(QString::fromStdString(regexError))
            );
            mRegexPattern->setFocus();
            return;
        }
    }

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
    const QString formatName = settings.value(KEY_FORMAT, "json").toString();
    if (formatName.compare("logfmt", Qt::CaseInsensitive) == 0)
    {
        mFormat->setCurrentIndex(mFormat->findData(static_cast<int>(Format::Logfmt)));
    }
    else if (formatName.compare("csv", Qt::CaseInsensitive) == 0)
    {
        mFormat->setCurrentIndex(mFormat->findData(static_cast<int>(Format::Csv)));
    }
    else if (formatName.compare("regex", Qt::CaseInsensitive) == 0)
    {
        mFormat->setCurrentIndex(mFormat->findData(static_cast<int>(Format::Regex)));
    }
    else
    {
        mFormat->setCurrentIndex(mFormat->findData(static_cast<int>(Format::Json)));
    }

    // Restore the prior pattern and resolve template-vs-custom by
    // looking the stored bytes up in the merged registry. A
    // registry upgrade that changes a pattern then silently moves
    // the user from "matched template" to "custom" without losing
    // their text — they only lose the named-template tag.
    // `FindTemplateByPattern` consults the same merged catalog
    // the probe loop uses, so user matches are honoured.
    const QString storedPattern = settings.value(KEY_REGEX_PATTERN).toString();
    if (!storedPattern.isEmpty())
    {
        mRegexPattern->setText(storedPattern);
        const std::string storedStdString = storedPattern.toStdString();
        QVariant pickerData = QVariant(RegexTemplateCustomData());
        if (const std::optional<loglib::RegexTemplate> match = loglib::FindTemplateByPattern(storedStdString);
            match.has_value())
        {
            pickerData = QVariant(QString::fromStdString(match->name));
        }
        const int comboIdx = mRegexTemplate->findData(pickerData);
        if (comboIdx >= 0)
        {
            mRegexTemplate->setCurrentIndex(comboIdx);
        }
        else
        {
            // The stored template name has vanished from the
            // catalog since last launch. Fall back to "Custom..."
            // so the pattern text stays editable.
            mRegexTemplate->setCurrentIndex(mRegexTemplate->findData(QVariant(RegexTemplateCustomData())));
        }
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
    {
        const auto formatValue = mFormat->currentData().toInt();
        const char *formatName = "json";
        if (formatValue == static_cast<int>(Format::Logfmt))
        {
            formatName = "logfmt";
        }
        else if (formatValue == static_cast<int>(Format::Csv))
        {
            formatName = "csv";
        }
        else if (formatValue == static_cast<int>(Format::Regex))
        {
            formatName = "regex";
        }
        settings.setValue(KEY_FORMAT, formatName);
    }
    settings.setValue(KEY_REGEX_PATTERN, mRegexPattern->text());
    settings.setValue(KEY_BIND, mBindAddress->text());
    settings.setValue(KEY_PORT, mPort->value());
    settings.setValue(KEY_MAX_CLIENTS, mMaxConcurrentClients->value());
    // Persist the *intended* TCP TLS choice so the next session
    // restores it even if the user happened to be on UDP at accept
    // time (where the live checkbox is forced off).
    const bool persistedTlsEnabled = mTcpRadio->isChecked() ? mTlsEnable->isChecked() : mTcpTlsEnableRemembered;
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
    {
        const auto formatValue = mFormat->currentData().toInt();
        if (formatValue == static_cast<int>(Format::Logfmt))
        {
            out.format = Format::Logfmt;
        }
        else if (formatValue == static_cast<int>(Format::Csv))
        {
            out.format = Format::Csv;
        }
        else if (formatValue == static_cast<int>(Format::Regex))
        {
            out.format = Format::Regex;
        }
        else
        {
            out.format = Format::Json;
        }
    }
    out.regexPattern = mRegexPattern->text();
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
