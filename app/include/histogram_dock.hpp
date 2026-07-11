#pragma once

#include "histogram_model.hpp"

#include <QDockWidget>
#include <QPointer>

#include <cstddef>

class AnchorManager;
class HistogramWidget;
class LogModel;
class ThemeControl;
class QCloseEvent;

/// Bottom-docked strip that plots per-time-bucket, per-level row
/// counts over the current session (ROADMAP item 2).
///
/// Owns a `HistogramModel` (subscribed to `LogModel`) and a
/// `HistogramWidget` (custom paint), and forwards their signals so
/// navigation logic stays in `MainWindow`. `closed()` mirrors
/// `AnchorsDock::closed` for `MainWindow::WireDockToggle`.
class HistogramDock : public QDockWidget
{
    Q_OBJECT

public:
    /// @p anchors, when non-null, enables the tick strip above the
    /// bars (one coloured tick per palette slot in each anchored
    /// bucket). Pass `nullptr` to disable anchor tracking entirely.
    HistogramDock(LogModel *model, ThemeControl *theme, AnchorManager *anchors, QWidget *parent = nullptr);

    /// Non-owning accessor for tests.
    [[nodiscard]] HistogramModel *ModelForTest() const noexcept
    {
        return mModel;
    }

    /// Non-owning accessor for tests.
    [[nodiscard]] HistogramWidget *WidgetForTest() const noexcept
    {
        return mWidget;
    }

signals:
    /// Bar clicked (no drag). Consumer maps to a source row via
    /// `HistogramModel::FirstRowInBucket`.
    void bucketClicked(std::size_t bucketIndex);

    /// Tick-strip clicked on an anchored column. `sourceRow` is the
    /// earliest anchored source-model row in that column, so
    /// consumers can route it to `MainWindow::SelectSourceRow`.
    void anchorClicked(int sourceRow);

    /// User dragged a range and released. Bounds are epoch
    /// microseconds, inclusive.
    void timeRangeSelected(qint64 fromEpochMicros, qint64 toEpochMicros);

    /// Fired on genuine user dismissal (X / system close).
    void closed();

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    QPointer<HistogramModel> mModel;
    HistogramWidget *mWidget = nullptr;
};
