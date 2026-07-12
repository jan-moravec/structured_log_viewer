#pragma once

#include <QPointer>
#include <QRect>
#include <QSize>
#include <QWidget>

#include <climits>
#include <cstddef>

class OverviewRailModel;
class ThemeControl;
class QAbstractItemView;
class QMouseEvent;
class QPaintEvent;
class QResizeEvent;
class QShowEvent;
class QWheelEvent;

/// Klogg / Qt-Creator-inspired match overview rail. A slim
/// vertical strip painted in a reserved right-hand viewport
/// margin of `LogTableView` (via
/// `LogTableView::AttachOverviewRail`). Renders:
///
/// - A **subtle wash background** (`QPalette::Base` blended
///   toward `QPalette::Window`) so the rail reads as an
///   integrated frame element in every theme.
/// - A **dominant-level colour underlay** per bucket, drawn at
///   ~50 % alpha over the wash. Severity ties break in favour
///   of the higher level (Fatal beats Error beats Warn).
/// - **Find match ticks** for buckets containing at least one
///   find match, painted in the palette's Highlight accent.
/// - **Anchor tick bands** per set anchor palette slot,
///   coloured via `ThemeControl::AnchorBrushFor`.
/// - A **rounded viewport indicator** showing the currently
///   visible proxy-row range, tracked from the attached table
///   view's vertical scrollbar. Anti-aliased translucent fill
///   with a 2 px cosmetic outline (the "glass thumb" look).
///
/// Rail width is DPI-fluent: `sizeHint()` returns
/// `max(fontMetrics('M'), style()->PM_ScrollBarExtent)` so the
/// rail scales with the platform's own DPI-aware scrollbar
/// sizing. Repositioning is owned by `LogTableView`; this
/// widget just paints and forwards mouse events.
class OverviewRailWidget : public QWidget
{
    Q_OBJECT

public:
    /// @p model is borrowed and must outlive this widget.
    /// @p theme is optional; when null, level colours fall back
    /// to `QPalette::Highlight` and anchor bands to
    /// `QPalette::Highlight`. @p tableView is used solely to
    /// resolve viewport-indicator geometry (via its vertical
    /// scrollbar); the widget does not take ownership.
    OverviewRailWidget(
        OverviewRailModel *model, ThemeControl *theme, QAbstractItemView *tableView, QWidget *parent = nullptr
    );

    [[nodiscard]] QSize sizeHint() const override;
    [[nodiscard]] QSize minimumSizeHint() const override;

    /// Retrieve the widget's currently-cached DPI-fluent width.
    /// Kept as a helper (over `sizeHint().width()`) because
    /// `sizeHint()` is const and re-runs the platform-metric
    /// query on every call. Test-only.
    [[nodiscard]] int RailWidthForTest() const;

    /// Rail rect for the viewport indicator overlay. Empty
    /// when there is no attached table view or the model
    /// reports zero rows. Test-only.
    [[nodiscard]] QRect ViewportIndicatorRectForTest() const;

    /// Bucket at pixel Y, or nullopt when the widget is empty
    /// / height 0. Test-only.
    [[nodiscard]] int BucketAtYForTest(int y) const;

    /// Y coordinate of the top of bucket @p bucket. Test-only.
    [[nodiscard]] int YForBucketForTest(std::size_t bucket) const;

signals:
    /// Emitted after the widget resolves a click / drag Y to a
    /// proxy row. `MainWindow` connects this to
    /// `ScrollToProxyRow`; @p replaceSelection is `true` on a
    /// fresh click (the user is committing to that row, so it's
    /// safe to replace the existing selection) and `false`
    /// during a drag scrub (the user is exploring, so any
    /// existing multi-selection is preserved).
    void proxyRowClicked(int proxyRow, bool replaceSelection);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void changeEvent(QEvent *event) override;

    /// Re-sync the model's bucket count on every show. Needed
    /// because `MainWindow::SetOverviewRailVisible(false)` drops
    /// the bucket vector to skip rebuilds while the rail is
    /// hidden — a subsequent show whose viewport height didn't
    /// change would not re-fire `resizeEvent`, and the rail
    /// would paint blank against a zero-bucket model.
    void showEvent(QShowEvent *event) override;

private:
    /// Rail Y range that maps 1-to-1 to proxy rows. Excludes a
    /// 2 px inset at the top and bottom so tick outlines don't
    /// clip against the widget edges.
    [[nodiscard]] QRect InteractiveRailRect() const;

    /// Compute the currently-visible proxy-row Y range on the
    /// rail, or an empty rect when the row count is 0.
    [[nodiscard]] QRect ComputeViewportIndicatorRect() const;

    /// Push the current widget height into the model so the
    /// bucket vector matches the rail's usable pixel count.
    /// Idempotent when the height matches.
    void SyncBucketCountToHeight();

    /// Resolve mouse Y to a proxy row and emit
    /// `proxyRowClicked`. Filters out consecutive same-row
    /// emissions during a drag so downstream handlers aren't
    /// spammed with duplicate scrolls. @p replaceSelection is
    /// forwarded to the signal so the initial click and the
    /// scrubbing moves can request different selection policies.
    void EmitProxyRowForY(int y, bool replaceSelection);

    QPointer<OverviewRailModel> mModel;
    QPointer<ThemeControl> mTheme;
    QPointer<QAbstractItemView> mTableView;

    /// Last emitted proxy row during an active drag. Sentinel
    /// `INT_MIN` while idle so the first click still emits.
    /// Default-initialised in-class so a future ctor overload
    /// that forgets the init list can't leak an indeterminate
    /// value into the first `EmitProxyRowForY` comparison.
    int mLastEmittedRow = INT_MIN;

    /// True while the user is holding the left button on the
    /// rail. Used to differentiate a wheel-through-rail scroll
    /// (idle state, forward to scrollbar) from a drag scrub
    /// (active state, do not forward).
    bool mDragging = false;
};
