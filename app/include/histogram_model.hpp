#pragma once

#include "anchor_manager.hpp"

#include <loglib/histogram_bucket_index.hpp>
#include <loglib/log_level.hpp>
#include <loglib/log_value.hpp>
#include <loglib/theme.hpp>

#include <QObject>
#include <QPointer>

#include <bitset>
#include <cstddef>
#include <optional>
#include <span>
#include <vector>

class LogModel;
class QTimer;

/// GUI-side glue between `LogModel` and `loglib::HistogramBucketIndex`.
///
/// Owns the bucket index. Subscribes to the log model's row-change
/// signals (`rowsInserted`, `rowsRemoved`, `modelReset`) and keeps
/// the index in sync. Emits `bucketsChanged` on a short coalesce
/// timer so live-tail bursts collapse to at most one repaint per
/// batch cadence.
///
/// The model does *not* subclass `QAbstractItemModel`: the widget
/// reads from `Index()` directly. Rebuild on bucket-size change goes
/// through `SetBucketSize` -> `Rebuild()` which walks the log model.
class HistogramModel : public QObject
{
    Q_OBJECT

public:
    /// Bitset shape for the per-bucket anchor slot mask. One bit per
    /// palette slot (`loglib::ANCHOR_PALETTE_SIZE`, currently 8). Bit
    /// `s` set in `mAnchorSlotPerBucket[b]` means: bucket `b` holds at
    /// least one anchor coloured with palette slot `s`. The bitmask
    /// design keeps the tick renderer allocation-free even when the
    /// same slot repeats across many rows in one bucket.
    using AnchorSlotMask = std::bitset<loglib::ANCHOR_PALETTE_SIZE>;

    /// @p logModel is borrowed; must outlive the histogram model.
    ///
    /// @p anchors is optional; when non-null, the model maintains a
    /// per-bucket anchor slot mask (see `AnchorSlotsPerBucket`) and
    /// emits `anchorBucketsChanged` whenever a bucket's mask flips.
    /// Passing `nullptr` disables anchor tracking entirely and the
    /// widget's tick strip stays hidden.
    HistogramModel(LogModel *logModel, AnchorManager *anchors, QObject *parent = nullptr);

    /// Current bucket index. Never null-referenced; may be empty.
    [[nodiscard]] const loglib::HistogramBucketIndex &Index() const noexcept
    {
        return mIndex;
    }

    /// Change the bucket rung and rebuild from `LogModel`. Always
    /// installs the manual-pin latch, even when @p size equals the
    /// current rung: the pin represents *the user's intent to fix
    /// this rung*, so an idempotent call still overrides the
    /// automatic re-pick. Skips the rebuild when @p size matches the
    /// current rung. Emits `bucketsChanged` when a rebuild fires.
    ///
    /// Callers that want an auto-adjust *without* pinning should use
    /// `ApplyAutoBucketSize` instead; callers that want to drop the
    /// pin explicitly should use `ResetBucketSizeToAuto`.
    void SetBucketSize(loglib::HistogramBucketSize size);

    /// Recompute `AutoBucketSize` from the current time range and
    /// apply it. Rebuilds only if the picked rung differs from the
    /// current one. Emits `bucketsChanged` on change.
    ///
    /// Respects the manual-pin latch: after the user has zoomed with
    /// `SetBucketSize` (Z / Shift+Z / Ctrl+wheel), this is a no-op so
    /// streaming batches can't secretly re-pick the rung under them.
    /// The user-driven "Reset zoom (auto)" path calls
    /// `ResetBucketSizeToAuto` instead, which drops the pin first.
    void ApplyAutoBucketSize();

    /// User-initiated "return to auto zoom". Clears the manual-pin
    /// latch and forces an `ApplyAutoBucketSize` recompute so the
    /// current range picks a fresh rung regardless of prior zooms.
    /// Wired to `HistogramWidget`'s context-menu "Reset zoom (auto)"
    /// entry. Emits `bucketsChanged` on change.
    void ResetBucketSizeToAuto();

    /// Full re-scan of the log model into the bucket index. Called on
    /// `modelReset` and on bucket-size change. Emits `bucketsChanged`.
    void Rebuild();

    /// First `Type::Time` column index in the current configuration,
    /// or `-1` when the log has none. Cached; refreshed on
    /// `modelReset`, on `rowsInserted` (mid-stream column-type
    /// flips), on `columnsMoved` (post-batch time-column bubble),
    /// and on `enumColumnsChanged`.
    [[nodiscard]] int TimeColumnIndex() const noexcept
    {
        return mTimeColumnIndex;
    }

    /// True when the current configuration has a `Type::Time` column.
    [[nodiscard]] bool HasTimeColumn() const noexcept
    {
        return mTimeColumnIndex >= 0;
    }

    /// First `Type::Level` column index in the current configuration,
    /// or `-1` when the log has none. Cached; refreshed on `modelReset`,
    /// row-insert flip detection, and `LogModel::enumColumnsChanged`.
    /// Mid-stream promotion of an enum column to `Type::Level` triggers
    /// a full rebuild so earlier rows (previously slotted into
    /// `LogLevel::Unknown`) are re-attributed to their canonical level.
    [[nodiscard]] int LevelColumnIndex() const noexcept
    {
        return mLevelColumnIndex;
    }

    /// First source-model row whose timestamp falls in bucket @p index.
    /// Returns `-1` when the bucket has no rows in the current log
    /// model (possible if a proxy/eviction cleared it since the last
    /// rebuild).
    ///
    /// Backed by a lazy `firstRowPerBucket` cache built on first call
    /// after each `Rebuild()` / `AppendRange` cycle. First call after
    /// a data change costs one O(N) scan; subsequent clicks are O(1).
    /// The cache is `mutable` so the read path can populate it without
    /// giving up the const contract callers rely on.
    [[nodiscard]] int FirstRowInBucket(std::size_t bucketIndex) const;

    /// Min / max timestamp observed in the log model. `nullopt` when
    /// the model has no rows or no time column.
    ///
    /// Derived from `mIndex` in O(1) whenever the bucket index is
    /// non-empty: `[BucketStart(0), BucketEnd(last))` is a tight
    /// superset of the observed range (buckets are anchored on
    /// timestamp truncation, so the true min / max sit inside those
    /// bounds). Falls back to an O(N) walk only when the caller
    /// needs the exact min / max and `mIndex` is empty (e.g. every
    /// row's timestamp failed to parse). Used by `ApplyAutoBucketSize`.
    struct TimeRange
    {
        loglib::TimeStamp min;
        loglib::TimeStamp max;
    };
    [[nodiscard]] std::optional<TimeRange> ObservedRange() const;

    /// Per-bucket anchor slot mask, parallel to `Index().Buckets()`.
    /// Returns an empty span when no `AnchorManager` is wired. Entry
    /// `i` is the OR of every anchor slot bit for anchored rows whose
    /// timestamp falls in bucket `i`. The widget's tick strip iterates
    /// this span (folded through the same `ComputeVisualLayout` as
    /// the bars) so ticks stay aligned with the columns underneath.
    [[nodiscard]] std::span<const AnchorSlotMask> AnchorSlotsPerBucket() const noexcept
    {
        return {mAnchorSlotPerBucket.data(), mAnchorSlotPerBucket.size()};
    }

    /// True when any bucket currently carries at least one anchor.
    /// The widget uses this to gate the tick strip overlay pass so a
    /// no-anchor session paints pixel-identical to before the tick
    /// feature.
    [[nodiscard]] bool HasAnchorTicks() const noexcept
    {
        return mAnchorBucketBitsSet > 0;
    }

    /// Source-model row index of the earliest anchored row whose
    /// timestamp falls inside `[bucketBegin, bucketEnd)`, or `-1`
    /// when no anchored row is found (or the manager / model isn't
    /// wired). Used by `HistogramWidget` to route tick clicks to
    /// the anchored row itself rather than the bucket's first row
    /// (which may be an unrelated non-anchor row).
    ///
    /// Half-open range in raw-bucket coordinates so the widget can
    /// pass the merged `[begin, end)` produced by its `stride > 1`
    /// fold and land on any anchor visible in that visual column.
    /// O(anchors) — the anchor set is small in practice.
    [[nodiscard]] int FirstAnchoredRowInBucketRange(std::size_t bucketBegin, std::size_t bucketEnd) const;

signals:
    /// Emitted (coalesced) whenever the bucket index has meaningful
    /// new content the widget should repaint.
    void bucketsChanged();

    /// Emitted when the time-column presence flips (log with a time
    /// column loaded / unloaded, or a config edit adds one). The
    /// widget uses this to switch between the empty-state placeholder
    /// and the bar view without a full rebuild.
    void timeColumnAvailabilityChanged(bool hasTimeColumn);

    /// Emitted when any bucket's anchor slot mask changes (anchor
    /// added, removed, recoloured, or the bucket geometry itself
    /// shifted under a `Rebuild`). Not coalesced — anchor edits are
    /// user-driven and low-frequency, so paying one repaint per
    /// change is preferable to hiding it behind a 50 ms timer.
    void anchorBucketsChanged();

private:
    void OnRowsInserted(const QModelIndex &parent, int first, int last);
    void OnRowsRemoved(const QModelIndex &parent, int first, int last);
    void OnModelReset();

    /// Rebuild trigger for `LogModel::enumColumnsChanged`. Enum
    /// promotions / demotions can shift the first `Type::Level`
    /// column index or introduce one where none existed; either
    /// case invalidates the per-level counts baked into `mIndex`.
    /// Also refreshes `mTimeColumnIndex` because the same batch
    /// that promoted the level column may have bubbled the time
    /// column to its canonical slot without insering new rows --
    /// a stale time index would then feed the rebuild the wrong
    /// column's values (see `OnColumnsMoved`).
    /// No-op when neither cached index changes (e.g. a simple
    /// dictionary `Grew` on an unrelated enum column).
    void OnEnumColumnsChanged();

    /// Rebuild trigger for `QAbstractItemModel::columnsMoved`.
    /// `LogModel` reorders columns after `AppendBatch` (bubbling
    /// freshly-observed `Type::Time` columns to position 0, and
    /// `Type::Level` columns to `CANONICAL_LEVEL_COLUMN_INDEX`).
    /// Those reorderings mutate the column identity behind our
    /// cached `mTimeColumnIndex` / `mLevelColumnIndex` without
    /// inserting rows, so neither `OnRowsInserted` nor
    /// `OnEnumColumnsChanged` would refresh them on their own.
    /// A stale time index feeds subsequent rebuilds the wrong
    /// column's values (e.g. an integer id column read as
    /// microseconds since epoch).
    void OnColumnsMoved();

    /// Single-anchor mutation from `AnchorManager::anchorChanged`.
    /// Resolves @p key to its source row, extracts the timestamp,
    /// finds the target bucket, and rebuilds only that bucket's
    /// mask from `AnchorManager`. O(rows) worst case per event, but
    /// per-anchor cost is dwarfed by the user-facing frequency.
    void OnAnchorChanged(const AnchorManager::Key &key);

    /// Bulk anchor mutation from `AnchorManager::anchorsReset`
    /// (`ClearAll`, `Replace`, or a multi-key bulk op). Delegates to
    /// `RebuildAnchorBuckets`.
    void OnAnchorsReset();

    /// Walk source-model rows `[first, last]` and add them to the
    /// index. Assumes `mTimeColumnIndex >= 0`.
    void AppendRange(int first, int last);

    /// Compute the first `Type::Time` column index from the log
    /// model's configuration.
    [[nodiscard]] int ComputeTimeColumnIndex() const;

    /// Compute the first `Type::Level` column index from the log
    /// model's configuration. `-1` when none.
    [[nodiscard]] int ComputeLevelColumnIndex() const;

    /// Extract `TimeStamp` for source-model @p row at
    /// `mTimeColumnIndex`. Returns `nullopt` for monostate or
    /// non-time-shaped slots.
    [[nodiscard]] std::optional<loglib::TimeStamp> TimeStampForRow(int row) const;

    /// Level for source-model @p row via the log model's first
    /// `Type::Level` column, or `LogLevel::Unknown` when none.
    [[nodiscard]] loglib::LogLevel LevelForRow(int row) const;

    /// Fire `bucketsChanged` on the coalesce timer (single-shot restart).
    void ScheduleEmit();

    /// Drop the `firstRowPerBucket` cache. Called from every path
    /// that mutates the bucket index (`Rebuild`, `AppendRange`,
    /// `OnRowsRemoved`) so a subsequent `FirstRowInBucket` call
    /// re-scans against fresh geometry rather than returning stale
    /// row indices into a rebased vector.
    void InvalidateFirstRowCache() const noexcept;

    /// Populate `mFirstRowPerBucketCache` from a full model walk.
    /// Idempotent; called lazily from `FirstRowInBucket`. Assumes
    /// `mTimeColumnIndex >= 0` and `mLogModel != nullptr`.
    void BuildFirstRowCache() const;

    /// Wipe and recompute `mAnchorSlotPerBucket` from
    /// `mAnchors->Entries()`. Called from `OnAnchorsReset`, from
    /// every path that changes bucket geometry (`Rebuild` and
    /// `AppendRange` — see comment there for why the cheaper
    /// incremental route isn't safe), and eagerly from the ctor
    /// after `OnModelReset`. No-op when `mAnchors == nullptr`. Fires
    /// `anchorBucketsChanged()` iff `mAnchorSlotPerBucket` actually
    /// changed (measured via a running popcount cache — `Rebuild`
    /// zeroes `mAnchorSlotPerBucket` first, so we compare against
    /// the previous non-empty state).
    void RebuildAnchorBuckets();

    /// Compute the bucket mask for @p key against the current index
    /// and record it into `mAnchorSlotPerBucket`. Returns the bucket
    /// affected (for callers that want to emit precise change signals);
    /// `nullopt` when the key doesn't resolve to a live row, has no
    /// timestamp, or lands outside the current bucket range.
    [[nodiscard]] std::optional<std::size_t> UpdateAnchorSlotForKey(const AnchorManager::Key &key);

    /// Reserve `mAnchorSlotPerBucket` to match `mIndex.Buckets().size()`
    /// (append-default-constructed masks on grow, truncate on shrink).
    /// Keeps the mAnchorBucketBitsSet running count in sync so the
    /// `HasAnchorTicks()` gate never lies. Called from `Rebuild` /
    /// `AppendRange` right before any anchor recomputation.
    void SyncAnchorBucketVectorSize();

    QPointer<LogModel> mLogModel;
    QPointer<AnchorManager> mAnchors;
    loglib::HistogramBucketIndex mIndex;
    int mTimeColumnIndex = -1;
    /// Cached first `Type::Level` column index. Mirrors
    /// `mTimeColumnIndex`: updated on `modelReset`, in the row-insert
    /// flip guard, on `columnsMoved`, and from `OnEnumColumnsChanged`.
    /// Reads in `AppendRange` prefer this cache over hitting
    /// `LogModel`'s cache every row.
    int mLevelColumnIndex = -1;
    QTimer *mEmitTimer = nullptr;

    /// True when the user has pinned a rung with `SetBucketSize`.
    /// Suppresses the automatic re-pick that would otherwise fire on
    /// every rebuild.
    bool mBucketSizePinned = false;

    /// Lazy cache: entry `i` is the first source-model row whose
    /// timestamp falls in bucket `i`, or `-1` when that bucket has
    /// no live rows. `nullopt` means "cache stale, rebuild on next
    /// read". Mutable so `FirstRowInBucket` can be `const` (the
    /// invariant is that the cache reflects the current `mIndex`;
    /// reads only observe it, they don't mutate observable state).
    mutable std::optional<std::vector<int>> mFirstRowPerBucketCache;

    /// Per-bucket anchor slot mask; parallel to `mIndex.Buckets()`.
    /// Empty when `mAnchors == nullptr`. Grows and shrinks with the
    /// bucket vector via `SyncAnchorBucketVectorSize`.
    std::vector<AnchorSlotMask> mAnchorSlotPerBucket;

    /// Running popcount of `mAnchorSlotPerBucket`, so `HasAnchorTicks`
    /// is O(1) and doesn't have to scan the whole vector per paint.
    /// Kept in sync alongside every mutation site.
    std::size_t mAnchorBucketBitsSet = 0;
};
