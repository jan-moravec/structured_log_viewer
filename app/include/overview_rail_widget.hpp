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

/// Width preset for the overview rail. Scales the shared
/// DPI-fluent base formula. Persisted as `ui/overviewRailWidth`
/// (`"narrow"` / `"medium"` / `"wide"`); default `Medium`.
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

/// Klogg / Qt-Creator-inspired match overview rail. A vertical
/// strip painted in a reserved right-hand viewport margin of
/// `LogTableView` (via `LogTableView::AttachOverviewRail`).
///
/// Paint layers, bottom-up:
///
/// - **Base wash** in `QPalette::Base` so the rail reads as an
///   extension of the table's data area.
/// - **Stacked-severity bin bar** per non-empty bucket. Bar width
///   encodes total density (log-scaled); the bar splits into
///   per-level segments in severity-descending order, each sized
///   by that level's `log2(count + 1)` share with a 1 px floor
///   so rare severities never round away. Colours come from the
///   theme's row background (`ThemeControl::BackgroundFor`), which
///   also respects the "high contrast levels" preference; unstyled
///   levels fall back to `QPalette::PlaceholderText`.
/// - **Match / anchor overlays** repaint the full content width
///   for buckets that carry a hit. Anchors > matches so a user
///   marker wins over live search state.
/// - **Viewport indicator** — anti-aliased translucent fill with
///   a cosmetic outline, tracking the attached view's scrollbar
///   (the "glass thumb" look).
///
/// Width is DPI-fluent: `sizeHint()` uses
/// `2 × max(PM_ScrollBarExtent, font 'M')` scaled by the active
/// width mode (Narrow 1.0 / Medium 1.5 / Wide 2.0). Repositioning
/// is `LogTableView`'s job; the widget only paints and forwards
/// mouse events.
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

    /// Cached DPI-fluent width from the last `sizeHint()`. Test-only.
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

    /// Content rect (`.left / .width` set, `.top / .height` zero).
    /// Bin bar, match overlay, and anchor overlay all share this
    /// horizontal slot; paint order handles overlap. Test-only.
    [[nodiscard]] static QRect ContentRectForTest(int underlayLeft, int underlayWidth);

signals:
    /// Emitted after resolving a click / drag Y to a proxy row.
    /// @p replaceSelection is `true` on a fresh click (commit) and
    /// `false` during a drag scrub (leave the selection alone).
    void proxyRowClicked(int proxyRow, bool replaceSelection);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void changeEvent(QEvent *event) override;

    /// Re-sync the model's bucket count on show. Needed because
    /// `MainWindow::SetOverviewRailVisible(false)` drops the
    /// bucket vector, and a same-height re-show wouldn't fire
    /// `resizeEvent`. Runs synchronously so the first paint has
    /// correct geometry.
    void showEvent(QShowEvent *event) override;

    /// Cancel any pending debounced bucket-sync on hide. Without
    /// this, a queued `SyncBucketCountToHeight()` firing after
    /// `SetOverviewRailVisible(false)` would immediately
    /// re-populate the buckets and undo the toggle optimisation.
    void hideEvent(QHideEvent *event) override;

#ifdef LOGAPP_BUILD_TESTING
public:
    /// Flush any pending debounced `SyncBucketCountToHeight()`
    /// now. Lets tests skip the debounce wait after resizing.
    /// No-op when no sync is pending.
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

    /// Push the current widget height into the model. Idempotent
    /// when the height matches. Called synchronously on `showEvent`
    /// and coalesced via `mBucketSyncTimer` on `resizeEvent` (see
    /// `BUCKET_SYNC_DEBOUNCE_MS`).
    void SyncBucketCountToHeight();

    /// Resolve mouse Y to a proxy row and emit `proxyRowClicked`.
    /// De-duplicates consecutive same-row emissions during a drag.
    void EmitProxyRowForY(int y, bool replaceSelection);

    QPointer<OverviewRailModel> mModel;
    QPointer<ThemeControl> mTheme;
    QPointer<QAbstractItemView> mTableView;

    /// Last emitted proxy row during an active drag; `INT_MIN`
    /// while idle so the first click still emits.
    int mLastEmittedRow = INT_MIN;

    /// True while the left button is held on the rail. Used to
    /// route wheel events (forward to scrollbar when idle; swallow
    /// during a drag scrub).
    bool mDragging = false;

    /// Active width preset (default matches Preferences /
    /// `QSettings` default so a brand-new widget agrees with a
    /// first-launch MainWindow before settings load).
    OverviewRailWidthMode mWidthMode = OverviewRailWidthMode::Medium;

    /// Last DPI-fluent width computed by `sizeHint()`. `mutable`
    /// so the `const` `sizeHint()` can refresh it.
    mutable int mCachedRailWidth = 0;

    /// Bucket-edge Y cache (`nBuckets + 1` entries). Recomputed
    /// only when `(nBuckets, railTop, railHeight)` changes so
    /// drag-scroll bursts skip the ~2 KB alloc per paint.
    std::vector<int> mCachedYEdges;
    std::size_t mCachedYEdgesBuckets = 0;
    int mCachedYEdgesRailTop = INT_MIN;
    int mCachedYEdgesRailHeight = INT_MIN;

    /// Debounce for `SyncBucketCountToHeight()` on `resizeEvent`.
    /// Each `SetBucketCount(H)` invalidates the durable find-match
    /// counts and (with Find open) forces a full rescan, so a
    /// per-pixel drag burst needs to be coalesced.
    QTimer *mBucketSyncTimer = nullptr;
};
