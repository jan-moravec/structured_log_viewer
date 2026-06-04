#include "shortcuts_dialog.hpp"

#include "shortcut_catalog.hpp"

#include <QDialogButtonBox>
#include <QMainWindow>
#include <QShowEvent>
#include <QString>
#include <QStringList>
#include <QTextBrowser>
#include <QVBoxLayout>

namespace
{
/// Rough size of a typical catalog's HTML so the buffer rarely reallocs.
constexpr int HTML_RESERVE_BYTES = 2048;

/// Default dialog size — fits a full menu bar's shortcuts on a 1080p display.
constexpr int DIALOG_DEFAULT_WIDTH = 420;
constexpr int DIALOG_DEFAULT_HEIGHT = 520;

/// Renders the catalog as a single HTML document, one table per group.
QString BuildHtml(const QList<shortcut_catalog::Group> &groups)
{
    QString html;
    html.reserve(HTML_RESERVE_BYTES);
    html += QStringLiteral("<html><body>");
    for (const auto &group : groups)
    {
        html += QStringLiteral("<h3>") + group.title.toHtmlEscaped() + QStringLiteral("</h3>");
        html += QStringLiteral("<table cellspacing='0' cellpadding='4' width='100%'>");
        for (const auto &entry : group.entries)
        {
            html += QStringLiteral("<tr>");
            html += QStringLiteral("<td>") + entry.text.toHtmlEscaped() + QStringLiteral("</td>");
            html += QStringLiteral("<td align='right'><tt>") + entry.shortcut.toHtmlEscaped() +
                    QStringLiteral("</tt></td>");
            html += QStringLiteral("</tr>");
        }
        html += QStringLiteral("</table>");
    }
    html += QStringLiteral("</body></html>");
    return html;
}
} // namespace

ShortcutsDialog::ShortcutsDialog(QMainWindow *host, QWidget *parent)
    : QDialog(parent != nullptr ? parent : host), mHost(host)
{
    setWindowTitle(tr("Keyboard Shortcuts"));
    // Modeless so the user can keep working while the reference is open.
    setModal(false);
    resize(DIALOG_DEFAULT_WIDTH, DIALOG_DEFAULT_HEIGHT);

    auto *layout = new QVBoxLayout(this);
    mBrowser = new QTextBrowser(this);
    mBrowser->setOpenExternalLinks(false);
    mBrowser->setOpenLinks(false);
    layout->addWidget(mBrowser, 1);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::close);
    layout->addWidget(buttons);
}

void ShortcutsDialog::showEvent(QShowEvent *event)
{
    RefreshContent();
    QDialog::showEvent(event);
}

void ShortcutsDialog::RefreshContent()
{
    if (mBrowser == nullptr)
    {
        return;
    }
    const auto groups = shortcut_catalog::Build(mHost);
    mBrowser->setHtml(BuildHtml(groups));
}
