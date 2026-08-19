#pragma once

#include "histogram_model.hpp"

#include <QDockWidget>
#include <QPointer>

#include <cstddef>

class AnchorManager;
class HistogramWidget;
class LogModel;
class LogSession;
class ThemeControl;
class QCloseEvent;
class QShowEvent;
struct SessionBindContext;

/**
 * @brief Dockable histogram of per-level rows in time buckets.
 *
 * The dock owns a `HistogramModel` and `HistogramWidget`, and
 * forwards their navigation signals.
 */
class HistogramDock : public QDockWidget
{
    Q_OBJECT

public:
    /**
     * @brief Constructs a histogram dock for borrowed sources.
     * @param model Log model to observe.
     * @param theme Theme service used by the widget.
     * @param anchors Anchor manager to observe, or `nullptr` to disable anchor ticks.
     * @param parent Parent widget.
     */
    HistogramDock(LogModel *model, ThemeControl *theme, AnchorManager *anchors, QWidget *parent = nullptr);

    /**
     * @brief Returns the owned histogram model for tests.
     * @return A borrowed pointer to the model.
     */
    [[nodiscard]] HistogramModel *ModelForTest() const noexcept
    {
        return mModel;
    }

    /**
     * @brief Returns the owned histogram widget for tests.
     * @return A borrowed pointer to the widget.
     */
    [[nodiscard]] HistogramWidget *WidgetForTest() const noexcept
    {
        return mWidget;
    }

    /**
     * @brief Binds the histogram to a session context.
     * @param context Incoming session, model, and anchor sources.
     *
     * A changed binding saves the outgoing bucket-size pin, cancels
     * pending coalesced notification, swaps sources, and restores the
     * incoming pin. Rebuild is deferred while hidden and completed on
     * the next show. An identical non-null binding is a no-op. No
     * ownership is transferred.
     */
    void Bind(const SessionBindContext &context);

    /** @brief Saves current state and releases all borrowed session sources. */
    void Unbind();

    /**
     * @brief Returns the currently bound session for tests.
     * @return The borrowed session, or `nullptr` when unbound.
     */
    [[nodiscard]] LogSession *boundSessionForTest() const noexcept;

signals:
    /**
     * @brief Emitted when a histogram bucket is clicked.
     * @param bucketIndex Raw bucket index.
     */
    void bucketClicked(std::size_t bucketIndex);

    /**
     * @brief Emitted when an anchor tick is clicked.
     * @param sourceRow Earliest anchored source row in the clicked column.
     */
    void anchorClicked(int sourceRow);

    /**
     * @brief Emitted when the user selects an inclusive time range.
     * @param fromEpochMicros Inclusive range start in epoch microseconds.
     * @param toEpochMicros Inclusive range end in epoch microseconds.
     */
    void timeRangeSelected(qint64 fromEpochMicros, qint64 toEpochMicros);

    /** @brief Emitted when the user closes the dock. */
    void closed();

protected:
    /**
     * @brief Emits `closed()` after an accepted close.
     * @param event Close event.
     */
    void closeEvent(QCloseEvent *event) override;
    /**
     * @brief Completes a deferred source rebuild when shown.
     * @param event Show event.
     */
    void showEvent(QShowEvent *event) override;

private:
    /** @brief Saves the active bucket-size pin into the bound session. */
    void SaveStateIntoBoundSession();

    /**
     * @brief Restores a session's pinned bucket size.
     * @param session Session whose presentation state should be applied.
     */
    void RestoreStateFromSession(LogSession *session);

    QPointer<HistogramModel> mModel;
    HistogramWidget *mWidget = nullptr;

    // Session whose histogram state is currently mirrored.
    QPointer<LogSession> mBoundSession;

    // Defers a full rebuild until a hidden dock becomes visible.
    bool mDeferredRebuildOnShow = false;
};
