#pragma once

#include <QDialog>

QT_BEGIN_NAMESPACE
class QMainWindow;
QT_END_NAMESPACE

/// Modeless dialog listing every shortcut on the host main window,
/// grouped by `ShortcutCatalog::Build`. Refreshes the rendered
/// content on every `show()` so newly registered actions appear
/// without a restart.
class ShortcutsDialog : public QDialog
{
    Q_OBJECT
public:
    /// @p host is the main window whose menu bar / actions are mined.
    /// The dialog parents to it so it tracks the host's lifetime.
    explicit ShortcutsDialog(QMainWindow *host, QWidget *parent = nullptr);

protected:
    void showEvent(QShowEvent *event) override;

private:
    /// Rebuild the rendered HTML from the live action tree. Cheap
    /// (one walk of the menu bar / action list).
    void RefreshContent();

    QMainWindow *mHost;
    class QTextBrowser *mBrowser = nullptr;
};
