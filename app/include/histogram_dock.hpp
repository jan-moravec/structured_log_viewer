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
    /// @p anchors, when non-null, drives a small tick strip above the
    /// bars: every bucket containing at least one anchored row gets a
    /// coloured tick per palette slot. Passing `nullptr` disables the
    /// strip entirely (used by the empty / anchor-free tests).
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
    /// User clicked (no drag) on a bucket. Consumer maps to a source
    /// row via `HistogramModel::FirstRowInBucket`.
    void bucketClicked(std::size_t bucketIndex);

    /// User clicked (no drag) directly on the anchor tick strip.
    /// `sourceRow` is the earliest anchored source-model row in the
    /// clicked visual column. Consumers route this to
    /// `MainWindow::SelectSourceRow` so the click lands on the
    /// anchored row itself rather than the bucket's first row.
    void anchorClicked(int sourceRow);

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
