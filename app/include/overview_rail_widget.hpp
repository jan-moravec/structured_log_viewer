#pragma once

#include <QMetaType>
#include <QPointer>
#include <QRect>
#include <QSize>
#include <QString>
#include <QWidget>

#include <climits>
#include <cstddef>
#include <cstdint>

class OverviewRailModel;
class ThemeControl;
class QAbstractItemView;
class QMouseEvent;
class QPaintEvent;
class QHideEvent;
class QResizeEvent;
class QShowEvent;
class QTimer;
class QWheelEvent;

/// User-selectable overview-rail width preset. All three modes
/// share the same DPI-fluent base formula (`2 × scrollbar
/// extent`, etc.); the mode only scales that base.
/// Persisted as `ui/overviewRailWidth` (`"narrow"` / `"medium"`
/// / `"wide"`). Default is `Medium`.
enum class OverviewRailWidthMode
{
    Narrow,
    Medium,
    Wide,
};

/// Parse a `QSettings` string into a width mode. Unknown /
/// empty values map to `Medium` (the shipped default).
[[nodiscard]] OverviewRailWidthMode ParseOverviewRailWidthMode(const QString &value);

/// Serialise a width mode for `QSettings`.
[[nodiscard]] QString OverviewRailWidthModeToSettingsString(OverviewRailWidthMode mode);

Q_DECLARE_METATYPE(OverviewRailWidthMode)

/// Klogg / Qt-Creator-inspired match overview rail. A slim
/// vertical strip painted in a reserved right-hand viewport
/// margin of `LogTableView` (via
/// `LogTableView::AttachOverviewRail`). Renders one full-width
/// content section with three stacked layers:
///
/// - A **base wash** in `QPalette::Base` so the rail reads as an
///   integrated extension of the table's data area in every
///   theme.
/// - A **stacked-severity bin bar** per non-empty bucket. Bar
///   width encodes total row density (log-scaled); the bar is
///   split into per-level segments in severity-descending order
///   (Fatal / Error / Warn / Info / Debug / Trace / Unknown),
///   each sized by that level's `log2(count + 1)`-weighted
///   share of the bucket with a 1 px floor so rare severities
///   never round away. The log-weighted split compresses the
///   dynamic range: a 500 Trace + 1 Fatal bucket paints a
///   noticeable Fatal band instead of a single-pixel needle,
///   while still keeping the majority level clearly dominant.
///   Segment colours come from the active theme's row
///   *background* (via `ThemeControl::BackgroundFor`) so the
///   rail reads as a mini-map of the row-tinted table rather
///   than a bright pastel band. `BackgroundFor` respects
///   `mHighContrast`, so the "high contrast levels" preference
///   automatically recolours the rail with the loud
///   `levelsHighContrast` variants. Theme-unstyled levels fall
///   back to `QPalette::PlaceholderText` painted at reduced
///   alpha (`LEVEL_FALLBACK_ALPHA`).
/// - **Match / anchor overlays** repaint the whole content
///   width for buckets that carry a search hit or an anchor.
///   Anchors are drawn on top of matches so a user-set marker
///   always wins over live search state; both use only theme
///   palette colours (`QPalette::Highlight` for matches,
///   `ThemeControl::AnchorBrushFor` for anchors).
/// - A **rounded viewport indicator** showing the currently
///   visible proxy-row range, tracked from the attached table
///   view's vertical scrollbar. Anti-aliased translucent fill
///   with a 2 px cosmetic outline (the "glass thumb" look).
///
/// Rail width is DPI-fluent: `sizeHint()` builds a base from
/// `2 × max(PM_ScrollBarExtent, font 'M')` then multiplies by
/// the active `OverviewRailWidthMode` (Narrow 1.0 / Medium 1.5 /
/// Wide 2.0). Repositioning is owned by `LogTableView`; this
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

    /// Select the DPI-fluent width preset. No-op when unchanged.
    /// Calls `updateGeometry()` so `LogTableView` can re-reserve
    /// the right viewport margin.
    void SetWidthMode(OverviewRailWidthMode mode);

    [[nodiscard]] OverviewRailWidthMode WidthMode() const noexcept
    {
        return mWidthMode;
    }

    /// Retrieve the widget's currently-cached DPI-fluent width.
    /// Kept as a helper (over `sizeHint().width()`) because
    /// `sizeHint()` is const and re-runs the platform-metric
    /// query on every call. Returns `mCachedRailWidth`, refreshed
    /// as a side-effect of `sizeHint()`. Test-only.
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

    /// Bar width (in device-independent px) the level pass would
    /// paint for a bucket with @p count rows against a rail-wide
    /// @p maxCount, laid out in a level column of @p columnWidth
    /// pixels. Exposes the log-scale + `MIN_BAR_WIDTH_PX` clamp
    /// so the paint math can be verified without pixel-scraping
    /// the widget. Test-only.
    [[nodiscard]] static int WidthForCountForTest(std::uint32_t count, std::uint32_t maxCount, int columnWidth);

    /// Single content rect inside the usable underlay. Returned
    /// with `.left / .width` populated and `.top / .height` zero
    /// because each bucket's paint pass fills its own Y slice
    /// on top of this. The bin bar, match overlay, and anchor
    /// overlay all share this same horizontal slot -- the old
    /// three-column layout collapsed into a single slot with
    /// paint order handling overlap. Test-only.
    [[nodiscard]] static QRect ContentRectForTest(int underlayLeft, int underlayWidth);

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
    ///
    /// Runs synchronously (bypasses `mBucketSyncTimer`) so the
    /// first paint after a show already has correct bucket
    /// geometry. The debounce is only there to coalesce interactive
    /// window-resize storms; a show event is a discrete user action
    /// and should feel instant.
    void showEvent(QShowEvent *event) override;

    /// Cancel any pending debounced bucket-sync when the widget
    /// hides. `MainWindow::SetOverviewRailVisible(false)` also
    /// drops the model's bucket vector to skip rebuild cost while
    /// hidden; without cancelling the timer, a queued
    /// `SyncBucketCountToHeight()` firing right after the hide
    /// would immediately re-populate the buckets against the
    /// widget's persisted height, undoing the visibility-toggle
    /// optimisation.
    void hideEvent(QHideEvent *event) override;

#ifdef LOGAPP_BUILD_TESTING
public:
    /// Flush any pending debounced `SyncBucketCountToHeight()`
    /// call now. Tests that resize the widget after `show()` and
    /// expect the bucket vector to update before the next paint
    /// call this to skip the debounce wait. No-op when no sync is
    /// pending.
    void FlushPendingBucketSyncForTest();
#endif

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
    /// Idempotent when the height matches. Called synchronously
    /// from `showEvent` and via `mBucketSyncTimer` from
    /// `resizeEvent` so a drag-resize storm coalesces to a
    /// single `SetBucketCount(H)` call (see
    /// `BUCKET_SYNC_DEBOUNCE_MS` in the .cpp for the rationale).
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

    /// Active width preset. Default Medium matches the
    /// Preferences / `QSettings` default so a brand-new widget
    /// agrees with a first-launch MainWindow before settings load.
    OverviewRailWidthMode mWidthMode = OverviewRailWidthMode::Medium;

    /// Last DPI-fluent width computed by `sizeHint()`. Mutable so
    /// `sizeHint() const` can refresh it as a side-effect, and
    /// `RailWidthForTest()` can return it without re-running the
    /// platform-metric query.
    mutable int mCachedRailWidth = 0;

    /// Bucket-edge Y-coordinates cache (one entry per bucket edge,
    /// so `nBuckets + 1` entries). Recomputed only when the
    /// `(nBuckets, railTop, railHeight)` triple changes. Drag-scroll
    /// bursts trigger many paints per second and this vector would
    /// otherwise allocate `~2 KB` per paint on a 500-px rail — a
    /// small win in isolation, but on the hot path it shows up
    /// under a profiler at heavy tail-append cadence.
    std::vector<int> mCachedYEdges;
    std::size_t mCachedYEdgesBuckets = 0;
    int mCachedYEdgesRailTop = INT_MIN;
    int mCachedYEdgesRailHeight = INT_MIN;

    /// Debounce timer for `SyncBucketCountToHeight()` on
    /// `resizeEvent`. Interactive window-drag resizes fire a
    /// resize event per changed pixel, and each `SetBucketCount(H)`
    /// call reallocates buckets, drops the durable find-match
    /// counts (sizes disagree), and forces a synchronous
    /// full-table rescan through the `MainWindow`
    /// `bucketsChanged` -> `PushFindMatchesToOverviewRail`
    /// lambda. Coalescing collapses a drag burst into one
    /// bucket-count update + one rescan.
    QTimer *mBucketSyncTimer = nullptr;
};
