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
/// Pre-allocated buffer roughly sized to fit the catalog of a real
/// session (file + edit + view + filters + stream + settings + other)
/// without realloc.
constexpr int HTML_RESERVE_BYTES = 2048;

/// Default dialog dimensions: tall enough to show the entire menu
/// bar's worth of shortcuts on a 1080p display without scrolling.
constexpr int DIALOG_DEFAULT_WIDTH = 420;
constexpr int DIALOG_DEFAULT_HEIGHT = 520;

/// Render the whole catalog as a single HTML document. Tables keep
/// the action label and the shortcut on the same baseline without
/// requiring custom widgets per row.
QString BuildHtml(const QList<ShortcutCatalog::Group> &groups)
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
    // Modeless so the user can keep using the window while the
    // reference is open.
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
    const auto groups = ShortcutCatalog::Build(mHost);
    mBrowser->setHtml(BuildHtml(groups));
}
