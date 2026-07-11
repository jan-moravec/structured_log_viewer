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
    /// One entry per rail pixel row. Aggregate on purpose:
    /// `OverviewRailWidget` reads the counts, tick flags, and
    /// anchor slots directly on the paint path. Reuses
    /// `loglib::LevelBucket` (same struct the histogram
    /// consumes) so the two features share their per-level
    /// counts layout — ROADMAP item 13 explicitly calls this
    /// "the histogram's bucket data structure tilted 90°".
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

    /// Set the number of rail pixel rows. Triggers a rebuild when
    /// the bucket count changes. Zero is legal (widget hidden /
    /// zero-height) and produces an empty bucket vector.
    void SetBucketCount(std::size_t nBuckets);

    /// Push the current find-match proxy rows. Coalesced with
    /// bucket rebuilds through the same 50 ms timer. Rows outside
    /// the current proxy range are silently dropped.
    void SetMatchProxyRows(std::vector<int> proxyRows);

    /// Rebuild every bucket from scratch. Called from every
    /// signal that shifts the proxy row set. O(rowCount +
    /// matchCount + anchorCount).
    void Rebuild();

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

    /// Dominant `LogLevel` in @p bucket, or `Unknown` when the
    /// bucket is empty or out of range. Ties break in favour of
    /// higher severity so a bucket with equal error and info rows
    /// paints red, not blue (triage bias).
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
    /// Widget uses this to skip the tick pass entirely.
    [[nodiscard]] bool HasMatchTicks() const noexcept
    {
        return !mMatchProxyRows.empty();
    }

    /// First proxy row whose bucket falls in @p bucket, or `-1`
    /// when the bucket is empty or out of range. Backed by the
    /// linear mapping (proxy row -> bucket = row * N / M) so it's
    /// O(1) once the row count is known.
    [[nodiscard]] int FirstProxyRowInBucket(std::size_t bucket) const noexcept;

    /// Rail pixel Y (top-anchored, 0..railHeight-1) → proxy row.
    /// Clamped into `[0, proxyRowCount)`; returns `-1` when the
    /// proxy is empty. Rail widget uses this on click / drag.
    [[nodiscard]] int ProxyRowForYPixel(int y, int railHeight) const noexcept;

signals:
    /// Buckets have repaint-worthy new content. Coalesced (~50 ms).
    void bucketsChanged();

    /// Match rows changed. Not coalesced — user typing in the
    /// find bar is already debounced upstream, and one paint per
    /// find recompute is desirable.
    void matchesChanged();

    /// Anchor mask on at least one bucket changed. Not coalesced;
    /// anchor edits are user-driven and low-frequency.
    void anchorBucketsChanged();

private:
    // Signal handlers. All hand off to `Rebuild()` today; kept
    // as separate slots so we can inline incremental append later
    // without disturbing the wiring surface.
    void OnRowsInserted(const QModelIndex &parent, int first, int last);
    void OnRowsRemoved(const QModelIndex &parent, int first, int last);
    void OnModelReset();
    void OnLayoutChanged();
    void OnColumnsChanged();
    void OnEnumColumnsChanged();
    void OnAnchorChanged(const AnchorManager::Key &key);
    void OnAnchorsReset();

    /// Full rebuild body — factored so `SetBucketCount` and
    /// `SetMatchProxyRows` can trigger it inline without
    /// re-entering the coalesce timer.
    void RebuildInternal();

    /// (Re)start the coalesce timer. Fires `bucketsChanged`
    /// after one quiet window.
    void ScheduleEmit();

    /// Bucket index for proxy row @p proxyRow, or -1 when out of
    /// range or when the bucket vector is empty. Public math but
    /// kept private so callers go through `FirstProxyRowInBucket`
    /// / `ProxyRowForYPixel` for the direction they need.
    [[nodiscard]] int BucketForProxyRow(int proxyRow) const noexcept;

    /// Walk the proxy chain down to a source `LogModel` row.
    /// Returns -1 when the chain doesn't terminate at `mSourceModel`
    /// or when any proxy hides the row.
    [[nodiscard]] int ProxyToSourceRow(int proxyRow) const noexcept;

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

    std::vector<Bucket> mBuckets;
    std::vector<int> mMatchProxyRows;

    /// Cached row count from the last rebuild. Used by
    /// `ProxyRowForYPixel` so the widget doesn't have to
    /// re-query the proxy after every scroll.
    int mProxyRowCount = 0;

    /// Cached first-`Type::Level` column index in source-model
    /// coords, `-1` when none. Refreshed on model reset and
    /// column-shape signals.
    int mLevelColumnIndex = -1;

    /// Running popcount of `mBuckets[*].anchorSlots` so
    /// `HasAnchorTicks` stays O(1).
    std::size_t mAnchorBucketBitsSet = 0;

    QTimer *mEmitTimer = nullptr;
};
