#pragma once

#include "log_session_presentation.hpp"

#include <QDockWidget>
#include <QPointer>
#include <QString>

#include <string>
#include <vector>

class QCloseEvent;
class QListWidget;
class QPushButton;
class QLabel;
class LogSession;
struct SessionBindContext;

/**
 * @brief Dockable, session-scoped list of parse and open errors.
 *
 * The visible list is capped at `MAX_DISPLAYED_ERRORS`; older
 * entries are evicted and summarized by a dropped-count footer.
 * Dock placement participates in `QMainWindow` state persistence.
 */
class ParseErrorsDock : public QDockWidget
{
    Q_OBJECT

public:
    /** @brief Maximum number of errors retained in the visible list. */
    static constexpr int MAX_DISPLAYED_ERRORS = 1000;

    explicit ParseErrorsDock(QWidget *parent = nullptr);

    /**
     * @brief Appends an error batch to the currently bound session.
     * @param title Batch heading.
     * @param errors Error messages; an empty batch is ignored.
     *
     * The first non-empty batch after a session reset emits
     * `firstBatchArrived()`.
     */
    void AppendErrors(const QString &title, const std::vector<std::string> &errors);

    /**
     * @brief Appends an error batch to its originating session.
     * @param originating Origin session, or `nullptr` for the bound session.
     * @param title Batch heading.
     * @param errors Error messages; an empty batch is ignored.
     *
     * A batch for the bound session updates the visible list, shadow,
     * counters, and first-batch latch. A batch for another session
     * updates only that session's capped error log.
     */
    void AppendErrorsForSession(LogSession *originating, const QString &title, const std::vector<std::string> &errors);

    /** @brief Clears displayed errors without rearming first-batch notification. */
    void ClearErrors();

    /**
     * @brief Clears all error state and rearms first-batch notification.
     *
     * The bound session's stored error log is reset as well.
     */
    void ResetSessionState();

    /**
     * @brief Switches the visible error log to a session.
     * @param context Context containing the incoming session.
     *
     * A changed binding saves the outgoing visible state, clears the
     * dock, and replays the incoming session's stored batches, counts,
     * and first-batch latch. An identical non-null binding is a no-op.
     * An unbound context saves and then clears the dock. No ownership
     * is transferred.
     */
    void Bind(const SessionBindContext &context);

    /** @brief Saves current error state, releases the session alias, and clears the dock. */
    void Unbind();

    /**
     * @brief Returns the currently bound session.
     * @return The borrowed session, or `nullptr` when unbound.
     */
    [[nodiscard]] LogSession *BoundSession() const noexcept
    {
        return mBoundSession.data();
    }

    /**
     * @brief Returns the currently bound session for tests.
     * @return The borrowed session, or `nullptr` when unbound.
     */
    [[nodiscard]] LogSession *boundSessionForTest() const noexcept
    {
        return BoundSession();
    }

    /**
     * @brief Returns the number of displayed error rows.
     * @return Displayed error count, excluding evicted entries.
     */
    [[nodiscard]] int Count() const noexcept
    {
        return mErrorCount;
    }

    /**
     * @brief Returns the cumulative number of evicted errors.
     * @return Dropped count since the last clear or reset.
     */
    [[nodiscard]] int DroppedCount() const noexcept
    {
        return mDroppedCount;
    }

signals:
    /**
     * @brief Emitted whenever displayed or dropped counts change.
     * @param count Displayed error count.
     * @param droppedCount Cumulative evicted error count.
     */
    void countChanged(int count, int droppedCount);

    /** @brief Emitted for the first batch after construction or reset. */
    void firstBatchArrived();

    /** @brief Emitted when the user closes the dock. */
    void closed();

protected:
    /**
     * @brief Emits `closed()` after an accepted close.
     * @param event Close event.
     */
    void closeEvent(QCloseEvent *event) override;

private:
    /** @brief Refreshes the summary and emits current counts. */
    void RefreshSummary();

    /** @brief Copies selected errors with group context to the clipboard. */
    void CopySelection() const;

    /** @brief Evicts oldest visible errors until the display cap is met. */
    void TrimToCap();

    /** @brief Appends the dropped-count footer when needed. */
    void RebuildOverflowFooter();

    /**
     * @brief Appends the retained portion of a batch to the shadow log.
     * @param title Batch heading.
     * @param errors Original error messages.
     * @param errorsBegin Index of the first retained error.
     */
    void MirrorAppendIntoShadow(const QString &title, const std::vector<std::string> &errors, size_t errorsBegin);
    void TrimShadowToCap();
    void ReplayLogIntoVisibleList(const SessionParseErrorLog &log);

    /**
     * @brief Appends a capped batch directly to a session's stored log.
     * @param session Session whose log should receive the batch.
     * @param title Batch heading.
     * @param errors Error messages.
     *
     * Visible dock state is not changed.
     */
    void AppendErrorsIntoSessionLog(LogSession *session, const QString &title, const std::vector<std::string> &errors);

    QListWidget *mList = nullptr;
    QLabel *mSummary = nullptr;
    QPushButton *mClearButton = nullptr;

    // Running tally of error rows in `mList` so `Count()` stays O(1).
    int mErrorCount = 0;
    // Cumulative evictions since the last `ClearErrors()`.
    int mDroppedCount = 0;
    // First-batch latch; cleared only by `ResetSessionState`. Decoupled
    // from the counts so the in-dock Clear button does not re-arm the
    // automatic reveal.
    bool mHasSeenFirstBatch = false;

    // Currently bound session, or null while unbound.
    QPointer<LogSession> mBoundSession;

    // Batches received since the last clear or reset, in insertion
    // order. Bind and Unbind transfer this state without rebuilding it
    // from the rendered list.
    std::vector<SessionParseErrorBatch> mBatchShadow;
};
