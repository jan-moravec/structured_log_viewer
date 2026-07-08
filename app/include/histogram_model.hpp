#pragma once

#include <loglib/histogram_bucket_index.hpp>
#include <loglib/log_level.hpp>
#include <loglib/log_value.hpp>

#include <QObject>
#include <QPointer>

#include <cstddef>
#include <optional>
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
    /// @p logModel is borrowed; must outlive the histogram model.
    explicit HistogramModel(LogModel *logModel, QObject *parent = nullptr);

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
    /// `modelReset`.
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

signals:
    /// Emitted (coalesced) whenever the bucket index has meaningful
    /// new content the widget should repaint.
    void bucketsChanged();

    /// Emitted when the time-column presence flips (log with a time
    /// column loaded / unloaded, or a config edit adds one). The
    /// widget uses this to switch between the empty-state placeholder
    /// and the bar view without a full rebuild.
    void timeColumnAvailabilityChanged(bool hasTimeColumn);

private:
    void OnRowsInserted(const QModelIndex &parent, int first, int last);
    void OnRowsRemoved(const QModelIndex &parent, int first, int last);
    void OnModelReset();

    /// Rebuild trigger for `LogModel::enumColumnsChanged`. Enum
    /// promotions / demotions can shift the first `Type::Level`
    /// column index or introduce one where none existed; either
    /// case invalidates the per-level counts baked into `mIndex`.
    /// No-op when the level column index is unchanged (e.g. a
    /// simple dictionary `Grew` on an unrelated enum column).
    void OnEnumColumnsChanged();

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

    QPointer<LogModel> mLogModel;
    loglib::HistogramBucketIndex mIndex;
    int mTimeColumnIndex = -1;
    /// Cached first `Type::Level` column index. Mirrors
    /// `mTimeColumnIndex`: updated on `modelReset`, in the row-insert
    /// flip guard, and from `OnEnumColumnsChanged`. Reads in
    /// `AppendRange` prefer this cache over hitting `LogModel`'s cache
    /// every row.
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
};
