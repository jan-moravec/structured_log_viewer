#pragma once

#include "histogram_model.hpp"

#include <QDockWidget>
#include <QPointer>

#include <cstddef>

class HistogramWidget;
class LogModel;
class ThemeControl;
class QCloseEvent;

/// Dockable bottom strip that renders a per-time-bucket, per-level
/// row-count histogram over the current session (ROADMAP item 2).
///
/// The dock owns a `HistogramModel` (which subscribes to `LogModel`)
/// and a `HistogramWidget` (custom paint). It forwards two signals
/// to `MainWindow` so navigation lives outside the dock:
///   - `bucketClicked(bucketIndex)` — jump the table to the first
///     row in the bucket.
///   - `timeRangeSelected(fromUs, toUs)` — install a `Type::Time`
///     range filter on `LogFilterModel`.
///
/// `closed()` fires on genuine user dismissal (X / system close);
/// `MainWindow::WireDockToggle` uses it to keep the toolbar / menu
/// action check state in sync.
class HistogramDock : public QDockWidget
{
    Q_OBJECT

public:
    HistogramDock(LogModel *model, ThemeControl *theme, QWidget *parent = nullptr);

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
    /// User clicked (no drag) on a bucket. Consumer maps to a source
    /// row via `HistogramModel::FirstRowInBucket`.
    void bucketClicked(std::size_t bucketIndex);

    /// User dragged a range and released. Bounds are epoch
    /// microseconds, inclusive.
    void timeRangeSelected(qint64 fromEpochMicros, qint64 toEpochMicros);

    /// Emitted on genuine user dismissal (X, system close).
    void closed();

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    QPointer<HistogramModel> mModel;
    HistogramWidget *mWidget = nullptr;
};
