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

/// Owns a `HistogramBucketIndex` and keeps it in sync with `LogModel`.
/// Subscribes to row / column change signals and coalesces repaint
/// notifications through a 50 ms timer so live-tail bursts collapse
/// to one paint per batch. Not a `QAbstractItemModel`: the widget
/// reads `Index()` directly.
class HistogramModel : public QObject
{
    Q_OBJECT

public:
    /// One bit per anchor palette slot (see `loglib::ANCHOR_PALETTE_SIZE`,
    /// currently 8). Bit `s` set in entry `b` means bucket `b` contains
    /// at least one anchor coloured with slot `s`.
    using AnchorSlotMask = std::bitset<loglib::ANCHOR_PALETTE_SIZE>;

    /// @p logModel is borrowed and must outlive this object. @p anchors
    /// is optional; pass `nullptr` to disable anchor tracking (the
    /// widget's tick strip then stays hidden).
    HistogramModel(LogModel *logModel, AnchorManager *anchors, QObject *parent = nullptr);

    /// Current bucket index; may be empty.
    [[nodiscard]] const loglib::HistogramBucketIndex &Index() const noexcept
    {
        return mIndex;
    }

    /// Pin the bucket rung to @p size and rebuild. Always sets the
    /// manual-pin latch (even if @p size matches the current rung) so
    /// later auto-picks stay disabled until `ResetBucketSizeToAuto`.
    /// Skips the rebuild when @p size is already active.
    void SetBucketSize(loglib::HistogramBucketSize size);

    /// Recompute the auto rung from the observed time range and apply
    /// it if it differs. No-op when the user has pinned a rung via
    /// `SetBucketSize`.
    void ApplyAutoBucketSize();

    /// Drop the manual-pin latch and force an auto-pick. Wired to the
    /// widget's "Reset zoom (auto)" context menu entry.
    void ResetBucketSizeToAuto();

    /// Full rescan of the log model into the bucket index. Called on
    /// `modelReset` and after a bucket-size change.
    void Rebuild();

    /// First `Type::Time` column index, or `-1` when the log has none.
    /// Cached; refreshed on model reset, row insert, column move, and
    /// enum-column change.
    [[nodiscard]] int TimeColumnIndex() const noexcept
    {
        return mTimeColumnIndex;
    }

    [[nodiscard]] bool HasTimeColumn() const noexcept
    {
        return mTimeColumnIndex >= 0;
    }

    /// First `Type::Level` column index, or `-1` when none. Cached
    /// alongside `TimeColumnIndex`. A mid-stream promotion triggers a
    /// full rebuild so pre-promotion rows are re-attributed from
    /// `LogLevel::Unknown` to their canonical level.
    [[nodiscard]] int LevelColumnIndex() const noexcept
    {
        return mLevelColumnIndex;
    }

    /// First source row whose timestamp falls in bucket @p bucketIndex,
    /// or `-1` when the bucket has no live rows. Backed by a lazy
    /// cache: first call after a data change is O(N), subsequent
    /// calls are O(1). The cache is `mutable` so the read stays const.
    [[nodiscard]] int FirstRowInBucket(std::size_t bucketIndex) const;

    /// Observed min / max timestamp. `nullopt` when the model is empty
    /// or has no time column. Reads from the index in O(1); falls back
    /// to an O(N) walk only when every row's timestamp failed to parse.
    struct TimeRange
    {
        loglib::TimeStamp min;
        loglib::TimeStamp max;
    };
    [[nodiscard]] std::optional<TimeRange> ObservedRange() const;

    /// Per-bucket anchor slot mask, parallel to `Index().Buckets()`.
    /// Empty when no `AnchorManager` is wired. Entry `i` is the OR of
    /// every anchor slot bit for anchored rows in bucket `i`.
    [[nodiscard]] std::span<const AnchorSlotMask> AnchorSlotsPerBucket() const noexcept
    {
        return {mAnchorSlotPerBucket.data(), mAnchorSlotPerBucket.size()};
    }

    /// True when any bucket carries at least one anchor. Used to gate
    /// the widget's tick strip so anchor-free sessions paint the same
    /// pixels as before the feature landed.
    [[nodiscard]] bool HasAnchorTicks() const noexcept
    {
        return mAnchorBucketBitsSet > 0;
    }

    /// Earliest anchored source row in raw-bucket range
    /// `[bucketBegin, bucketEnd)`, or `-1` when none. The widget uses
    /// this so tick clicks land on the anchor itself, not the bucket's
    /// first row. Half-open so callers can pass a stride-folded range.
    [[nodiscard]] int FirstAnchoredRowInBucketRange(std::size_t bucketBegin, std::size_t bucketEnd) const;

signals:
    /// Bucket index has repaint-worthy new content. Coalesced (~50 ms).
    void bucketsChanged();

    /// The time column appeared or disappeared. The widget switches
    /// between the empty-state placeholder and the bars without a
    /// full rebuild.
    void timeColumnAvailabilityChanged(bool hasTimeColumn);

    /// A bucket's anchor mask changed (anchor added, removed,
    /// recoloured, or bucket geometry shifted). Not coalesced: anchor
    /// edits are user-driven and low-frequency.
    void anchorBucketsChanged();

private:
    void OnRowsInserted(const QModelIndex &parent, int first, int last);
    void OnRowsRemoved(const QModelIndex &parent, int first, int last);
    void OnModelReset();

    /// Handler for `LogModel::enumColumnsChanged`. Refreshes cached
    /// time / level column indices and rebuilds when either moved,
    /// so pre-promotion `Unknown` rows are re-attributed. Also
    /// re-checks the time column because the same batch may bubble
    /// it (see `OnColumnsMoved`). No-op when neither index changes.
    void OnEnumColumnsChanged();

    /// Handler for `QAbstractItemModel::columnsMoved`. `LogModel`
    /// reorders columns after `AppendBatch` (time bubbled to slot 0,
    /// level to `CANONICAL_LEVEL_COLUMN_INDEX`) without inserting
    /// rows, so nothing else refreshes our cached indices. A stale
    /// time index would feed the next rebuild the wrong column's
    /// values (e.g. an id column read as microseconds).
    void OnColumnsMoved();

    /// Single-anchor mutation from `AnchorManager`. Rebuilds only the
    /// bucket the changed key lives in. O(rows) worst case per event
    /// but user-driven, so cheap in aggregate.
    void OnAnchorChanged(const AnchorManager::Key &key);

    /// Bulk anchor mutation from `AnchorManager` (clear / replace).
    /// Delegates to `RebuildAnchorBuckets`.
    void OnAnchorsReset();

    /// Add source rows `[first, last]` to the index. Assumes
    /// `mTimeColumnIndex >= 0`.
    void AppendRange(int first, int last);

    [[nodiscard]] int ComputeTimeColumnIndex() const;

    [[nodiscard]] int ComputeLevelColumnIndex() const;

    /// TimeStamp for source @p row at `mTimeColumnIndex`, or nullopt
    /// when the slot is monostate or not time-shaped.
    [[nodiscard]] std::optional<loglib::TimeStamp> TimeStampForRow(int row) const;

    /// Level for source @p row via `mLevelColumnIndex`, or
    /// `LogLevel::Unknown` when none.
    [[nodiscard]] loglib::LogLevel LevelForRow(int row) const;

    /// (Re)start the coalesce timer that eventually fires `bucketsChanged`.
    void ScheduleEmit();

    /// Drop the first-row-per-bucket cache. Called from every bucket
    /// mutation path so the next `FirstRowInBucket` re-scans against
    /// fresh geometry.
    void InvalidateFirstRowCache() const noexcept;

    /// Build the first-row-per-bucket cache from a full model walk.
    /// Called lazily from `FirstRowInBucket`. Assumes the log model
    /// and time column are available. Returns by value so the caller
    /// assigns into `mFirstRowPerBucketCache` — a linear write that
    /// stays visible to `bugprone-unchecked-optional-access` (a
    /// side-effecting inner call would appear "possibly still empty"
    /// to the checker).
    [[nodiscard]] std::vector<int> BuildFirstRowCache() const;

    /// Wipe and recompute `mAnchorSlotPerBucket` from
    /// `mAnchors->Entries()`. Called from every path that changes
    /// bucket geometry. Emits `anchorBucketsChanged` when the mask
    /// vector actually changed.
    void RebuildAnchorBuckets();

    /// OR the palette slot for @p key into the mask of its bucket.
    /// Returns the affected bucket index, or nullopt when the key
    /// resolves to no live row / no timestamp / an out-of-range bucket.
    [[nodiscard]] std::optional<std::size_t> UpdateAnchorSlotForKey(const AnchorManager::Key &key);

    /// Resize `mAnchorSlotPerBucket` to match the bucket vector,
    /// keeping `mAnchorBucketBitsSet` in sync. Called before any
    /// anchor recomputation.
    void SyncAnchorBucketVectorSize();

    QPointer<LogModel> mLogModel;
    QPointer<AnchorManager> mAnchors;
    loglib::HistogramBucketIndex mIndex;
    int mTimeColumnIndex = -1;
    int mLevelColumnIndex = -1;
    QTimer *mEmitTimer = nullptr;

    /// True after the user picked a rung with `SetBucketSize`.
    /// Suppresses the automatic re-pick on subsequent rebuilds.
    bool mBucketSizePinned = false;

    /// Lazy cache: entry `i` is the first source row in bucket `i`,
    /// or `-1` when empty. `nullopt` means "stale, rebuild on next
    /// read". Mutable so `FirstRowInBucket` can stay const.
    mutable std::optional<std::vector<int>> mFirstRowPerBucketCache;

    /// Per-bucket anchor slot mask; parallel to `mIndex.Buckets()`.
    /// Empty when `mAnchors == nullptr`.
    std::vector<AnchorSlotMask> mAnchorSlotPerBucket;

    /// Running popcount of `mAnchorSlotPerBucket` so `HasAnchorTicks`
    /// stays O(1). Kept in sync at every mutation site.
    std::size_t mAnchorBucketBitsSet = 0;
};
