#pragma once

#include <QDialog>

QT_BEGIN_NAMESPACE
class QMainWindow;
QT_END_NAMESPACE

/// Modeless dialog listing every shortcut on the host main window.
/// Refreshes content on every `show()` so new actions appear automatically.
class ShortcutsDialog : public QDialog
{
    Q_OBJECT
public:
    /// @p host supplies the actions to list; the dialog falls back to
    /// parenting on @p host when @p parent is null.
    explicit ShortcutsDialog(QMainWindow *host, QWidget *parent = nullptr);

protected:
    void showEvent(QShowEvent *event) override;

private:
    /// Rebuilds the rendered HTML from the host's current action tree.
    void RefreshContent();

    QMainWindow *mHost;
    class QTextBrowser *mBrowser = nullptr;
};
