#pragma once

#include <QDockWidget>
#include <QPointer>

class FindRecordWidget;
class LogSession;
class QCloseEvent;
class QShowEvent;
class QWidget;
struct SessionBindContext;

/**
 * @brief Dockable host for `FindRecordWidget`.
 *
 * The dock can be placed along the top or bottom edge, and its
 * position participates in `QMainWindow` state persistence.
 */
class FindDock : public QDockWidget
{
    Q_OBJECT

public:
    explicit FindDock(QWidget *parent = nullptr);

    /**
     * @brief Returns the hosted find widget.
     * @return A borrowed pointer owned by the dock.
     */
    [[nodiscard]] FindRecordWidget *Widget() const noexcept
    {
        return mWidget;
    }

    /**
     * @brief Reveals the dock and focuses the search field.
     *
     * Focus outside the dock is saved for restoration on close.
     */
    void RevealAndFocus();

    /**
     * @brief Switches the visible query state to a session.
     * @param context Context containing the incoming session.
     *
     * A changed binding saves the outgoing query, cancels both
     * match-count timers, restores the incoming query, and re-arms
     * counting for non-empty text. An identical non-null binding is
     * a no-op. An unbound context saves and then clears the query.
     * Match counts themselves are not persisted, and no ownership is
     * transferred.
     */
    void Bind(const SessionBindContext &context);

    /**
     * @brief Saves the current query, cancels timers, and releases the session alias.
     */
    void Unbind();

    /**
     * @brief Returns the currently bound session for tests.
     * @return The borrowed session, or `nullptr` when unbound.
     */
    [[nodiscard]] LogSession *boundSessionForTest() const noexcept;

signals:
    /** @brief Emitted when the user closes the dock. */
    void closed();

    /** @brief Emitted when the find dock becomes visible. */
    void revealed();

protected:
    /**
     * @brief Restores saved focus after an accepted close.
     * @param event Close event.
     */
    void closeEvent(QCloseEvent *event) override;

    /**
     * @brief Emits `revealed()` after the dock is shown.
     * @param event Show event.
     */
    void showEvent(QShowEvent *event) override;

private:
    FindRecordWidget *mWidget = nullptr;

    // Widget that held focus before the last reveal. `QPointer`
    // guards against destruction while the bar is open.
    QPointer<QWidget> mFocusBeforeReveal;

    // Currently bound session, or null while unbound.
    QPointer<LogSession> mBoundSession;
};
