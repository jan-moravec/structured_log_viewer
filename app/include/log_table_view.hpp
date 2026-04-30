#pragma once

#include <QTableView>

class LogTableView : public QTableView
{
    Q_OBJECT

public:
    explicit LogTableView(QWidget *parent = nullptr);

    void keyPressEvent(QKeyEvent *event) override;

public slots:
    void CopySelectedRowsToClipboard();

signals:
    /// Emitted when the user manually scrolls **away** from the bottom of
    /// the visible table (PRD 4.3.3 — VS Code terminal pattern). Wired to
    /// `MainWindow` to auto-disengage the **Follow tail** toggle.
    void userScrolledAwayFromBottom();

    /// Emitted when the user manually scrolls back to the bottom of the
    /// visible table. Wired to `MainWindow` to auto-re-engage the
    /// **Follow tail** toggle (PRD 4.3.3).
    void userScrolledToBottom();

private:
    /// True while the most recent `valueChanged` was at the bottom of the
    /// scrollbar; lets the slot emit edge-triggered signals only.
    bool mAtBottom = true;
};
