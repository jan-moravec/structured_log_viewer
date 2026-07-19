#pragma once

#include "anchor_manager.hpp"

#include <loglib/histogram_bucket_index.hpp>
#include <loglib/log_level.hpp>
#include <loglib/theme.hpp>

#include <QModelIndex>
#include <QObject>
#include <QPointer>

#include <bitset>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

class LogModel;
class QAbstractItemModel;
class QAbstractProxyModel;
class QTimer;

/// Data source for the overview rail. Owns a densely-bucketed
/// view of the *proxy* row space so the rail stays 1-to-1 aligned
/// with the scrollbar's usable Y range (a pixel in the rail maps
/// to a bucket of adjacent proxy rows).
///
/// Not a `QAbstractItemModel`: the rail widget reads `Buckets()`
/// directly and interprets pixel Y positions through
/// `ProxyRowForYPixel` / `FirstProxyRowInBucket`.
///
/// Keyed on the *outermost* proxy so newest-first orientation
/// (via `RowOrderProxyModel` in the chain) flips the rail for
/// free — proxy row 0 is visually the topmost row in the table
/// under both orientations.
class OverviewRailModel : public QObject
{
    Q_OBJECT

public:
    /// One entry per rail pixel row. Aggregate on purpose: the
    /// widget reads counts / tick flags / anchor slots directly on
    /// the paint path. Reuses `loglib::LevelBucket` so the rail and
    /// the histogram share the per-level counts layout — ROADMAP
    /// item 13 calls this "the histogram's bucket data structure
    /// tilted 90°".
    struct Bucket
    {
        // NOLINTBEGIN(misc-non-private-member-variables-in-classes)
        /// Per-level row counts. Slot 0 is `Unknown`, so
        /// unresolved-level rows still contribute a colour.
        loglib::LevelBucket levels{};

        /// Number of find-match rows folded into this bucket.
        uint32_t matchCount = 0;

        /// Bit `s` set means at least one row in this bucket
        /// carries an anchor coloured with palette slot `s`.
        std::bitset<loglib::ANCHOR_PALETTE_SIZE> anchorSlots;
        // NOLINTEND(misc-non-private-member-variables-in-classes)
    };

    /// @p proxyModel is the outermost proxy (`LogFilterModel` in
    /// production); may be null in constructor-time test fixtures.
    /// @p sourceModel is the underlying `LogModel`; borrowed and
    /// must outlive this object. @p anchors is optional; pass
    /// nullptr to disable anchor tick tracking.
    OverviewRailModel(
        QAbstractItemModel *proxyModel, LogModel *sourceModel, AnchorManager *anchors, QObject *parent = nullptr
    );

    /// Set the number of rail pixel rows. Triggers a synchronous
    /// rebuild on a size change so the caller sees fresh data on
    /// return. Zero is legal (widget hidden) and produces an empty
    /// bucket vector; incoming proxy signals then short-circuit
    /// inside `RebuildInternal`, which is what lets
    /// `MainWindow::SetOverviewRailVisible(false)` skip rebuild
    /// cost while hidden.
    void SetBucketCount(std::size_t nBuckets);

    /// Push the current find-match proxy rows. Only touches
    /// `matchCount`; the level counts and anchor bits are left
    /// alone. Out-of-range rows are dropped and the list is
    /// de-duplicated internally. Emits `matchesChanged` immediately
    /// — find updates are already debounced upstream.
    ///
    /// Refreshes the cached proxy row count from the live proxy
    /// before folding so tail-row hits scanned against a newer
    /// row count than the last coalesced rebuild are attributed to
    /// the right bucket.
    void SetMatchProxyRows(std::vector<int> proxyRows);

    /// Push per-bucket match counts directly, skipping the row-list
    /// path. Preferred when the caller already has per-bucket totals
    /// — avoids the O(matches) allocation for broad needles.
    ///
    /// @p perBucketCounts must have `BucketCount()` entries; a
    /// size mismatch (resize race) drops the update silently.
    /// @p totalMatches is the exact pre-cap total.
    ///
    /// Clears `mMatchProxyRows` and stores the counts as durable
    /// state so `RebuildInternal` (anchor edits, same-H resize,
    /// hide→show) can re-apply the ticks without the caller
    /// re-pushing. Emits `matchesChanged` only.
    void SetMatchBucketCounts(std::vector<uint32_t> perBucketCounts, uint32_t totalMatches);

    /// Request a coalesced rebuild (~50 ms). A burst of proxy
    /// signals collapses to a single O(rowCount) walk + one
    /// repaint per quiet window.
    void Rebuild();

    /// Rebuild synchronously. Used by callers that need fresh
    /// buckets on return (widget resize, initial attach).
    void RebuildNow();

    [[nodiscard]] std::span<const Bucket> Buckets() const noexcept
    {
        return {mBuckets.data(), mBuckets.size()};
    }

    [[nodiscard]] std::size_t BucketCount() const noexcept
    {
        return mBuckets.size();
    }

    /// Number of proxy rows the rail was last built against;
    /// `0` when the proxy is empty or unbound.
    [[nodiscard]] int ProxyRowCount() const noexcept
    {
        return mProxyRowCount;
    }

    /// Majority-count `LogLevel` in @p bucket, tie-broken by
    /// severity (Fatal > Error > … > Unknown). Returns `Unknown`
    /// for an empty / out-of-range bucket. The rail widget paints
    /// stacked severity segments instead of a single colour;
    /// retained for callers that want one representative colour
    /// (anchor tick legend, tests).
    [[nodiscard]] loglib::LogLevel DominantLevel(std::size_t bucket) const noexcept;

    /// Cached first-`Type::Level` column index, `-1` when none.
    /// Test-only.
    [[nodiscard]] int LevelColumnIndexForTest() const noexcept
    {
        return mLevelColumnIndex;
    }

    /// True iff any bucket carries at least one anchor tick.
    /// Same fast-path idea as `HistogramModel::HasAnchorTicks`.
    [[nodiscard]] bool HasAnchorTicks() const noexcept
    {
        return mAnchorBucketBitsSet > 0;
    }

    /// True iff any bucket carries at least one match tick.
    /// Tracks the *bucketed* total (not the raw row-list size) so
    /// a stale match list whose rows all fall outside the current
    /// proxy range correctly reports "no ticks".
    [[nodiscard]] bool HasMatchTicks() const noexcept
    {
        return mBucketedMatchCount > 0;
    }

    /// First proxy row in @p bucket, or `-1` for empty / out of
    /// range. O(1) via `row = ceil(bucket * M / N)`.
    [[nodiscard]] int FirstProxyRowInBucket(std::size_t bucket) const noexcept;

    /// Rail pixel Y (top-anchored, `0..railHeight-1`) → proxy row.
    /// Clamped into `[0, proxyRowCount)`; returns `-1` when the
    /// proxy is empty.
    [[nodiscard]] int ProxyRowForYPixel(int y, int railHeight) const noexcept;

signals:
    /// Buckets have repaint-worthy new content. Coalesced via the
    /// ~50 ms rebuild timer; forced paths (`SetBucketCount`,
    /// `RebuildNow`) emit synchronously.
    void bucketsChanged();

    /// Match rows changed. Not coalesced — find updates are
    /// already debounced upstream.
    void matchesChanged();

    /// Anchor mask on at least one bucket changed. Not coalesced;
    /// anchor edits are user-driven and rare.
    void anchorBucketsChanged();

private:
    // Signal handlers. All hand off to `Rebuild()` today; kept
    // as separate slots so a future incremental-append can be
    // inlined without changing the wiring.
    void OnRowsInserted(const QModelIndex &parent, int first, int last);
    void OnRowsRemoved(const QModelIndex &parent, int first, int last);
    void OnModelReset();
    void OnLayoutChanged();
    void OnColumnsChanged();
    void OnEnumColumnsChanged();
    void OnAnchorChanged(const AnchorManager::Key &key);
    void OnAnchorsReset();

    /// Full rebuild — walks the proxy, refreshes level counts +
    /// anchor bits, then re-folds the current match rows. Factored
    /// so `SetBucketCount` can run it synchronously.
    void RebuildInternal();

    /// Zero + refill `matchCount` on every bucket from
    /// `mMatchProxyRows`. `O(nBuckets + nMatchRows)`, cheap enough
    /// to run on every find keystroke without coalescing.
    void RefreshMatchTicks();

    /// Fold `mMatchProxyRows` into `matchCount`. Caller zeroes the
    /// counters first. Keeps `mBucketedMatchCount` in lockstep.
    void FoldMatchTicksIntoBuckets();

    /// Re-apply `mMatchBucketCounts` when sizes still agree (same
    /// H since the last `SetMatchBucketCounts`). No-op otherwise;
    /// caller must rescan. Lets the bucket-counts path survive
    /// coalesced rebuilds.
    void ApplyStoredMatchBucketCounts();

    /// (Re)start the coalesce timer. Timeout runs `RebuildInternal`
    /// and emits `bucketsChanged`.
    void ScheduleRebuild();

    /// Timer slot: run `RebuildInternal` and emit `bucketsChanged`.
    void OnRebuildTimeout();

    /// Bucket index for proxy row @p proxyRow, or `-1` for out of
    /// range / empty bucket vector.
    [[nodiscard]] int BucketForProxyRow(int proxyRow) const noexcept;

    /// Walk the proxy chain down to a source `LogModel` row.
    /// Returns `-1` when the chain doesn't terminate at
    /// `mSourceModel`. Uses the cached `mProxyChain` so the hot
    /// path skips a `qobject_cast` per row.
    [[nodiscard]] int ProxyToSourceRow(int proxyRow) const noexcept;

    /// (Re)populate `mProxyChain` / `mProxyChainTerminatesAtSource`.
    /// Called from the constructor and on `modelReset` so the
    /// rebuild hot path can skip repeated `qobject_cast`s.
    void RebuildProxyChainCache();

    /// Level lookup for source row @p sourceRow. Returns
    /// `LogLevel::Unknown` when no level column is configured
    /// or the slot is unmapped.
    [[nodiscard]] loglib::LogLevel LevelForSourceRow(int sourceRow) const noexcept;

    /// Linear scan for the first `Type::Level` column in the
    /// current configuration; `-1` when none.
    [[nodiscard]] int ComputeLevelColumnIndex() const noexcept;

    /// Emit `anchorBucketsChanged` when the aggregate bit-count
    /// changed. Called after every full rebuild.
    void EmitAnchorChangeIfDifferent(std::size_t previousBitsSet);

    QPointer<QAbstractItemModel> mProxyModel;
    QPointer<LogModel> mSourceModel;
    QPointer<AnchorManager> mAnchors;

    /// Cached proxy layers between `mProxyModel` and `mSourceModel`,
    /// populated once from `RebuildProxyChainCache`. Empty when the
    /// outermost proxy IS the source model. `QPointer` so an inner
    /// proxy destroyed without a `modelReset` zeroes the slot;
    /// `ProxyToSourceRow` short-circuits to `-1` rather than crash.
    std::vector<QPointer<QAbstractProxyModel>> mProxyChain;

    /// True iff `mProxyChain` terminates at `mSourceModel`.
    /// `ProxyToSourceRow` short-circuits to `-1` when false.
    bool mProxyChainTerminatesAtSource = false;

    /// One-shot flag preventing per-row `qWarning` spam when the
    /// chain drifts off the cached source model. Reset on every
    /// `RebuildProxyChainCache`. `mutable` because
    /// `ProxyToSourceRow` is `const`.
    mutable bool mProxyChainDriftWarned = false;

    std::vector<Bucket> mBuckets;
    std::vector<int> mMatchProxyRows;

    /// Durable per-bucket match totals from the last successful
    /// `SetMatchBucketCounts`. Mutually exclusive with a non-empty
    /// `mMatchProxyRows`: whichever API is active clears the other
    /// so a rebuild never double-folds.
    std::vector<uint32_t> mMatchBucketCounts;

    /// Exact total paired with `mMatchBucketCounts` (may exceed
    /// the sum of a capped scan's row list upstream).
    uint32_t mMatchBucketTotal = 0;

    /// Cached row count from the last rebuild. Used by
    /// `ProxyRowForYPixel` so scroll paths skip a proxy query.
    int mProxyRowCount = 0;

    /// Cached first `Type::Level` column index in source-model
    /// coords, `-1` when none.
    int mLevelColumnIndex = -1;

    /// Running popcount of `mBuckets[*].anchorSlots` so
    /// `HasAnchorTicks` stays O(1).
    std::size_t mAnchorBucketBitsSet = 0;

    /// Running sum of `mBuckets[*].matchCount` — the source of
    /// truth for `HasMatchTicks`.
    uint32_t mBucketedMatchCount = 0;

    /// Coalesce timer for `Rebuild()`. Timeout runs
    /// `OnRebuildTimeout` once and emits `bucketsChanged`.
    QTimer *mRebuildTimer = nullptr;
};
