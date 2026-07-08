#include "histogram_model.hpp"

#include "log_model.hpp"

#include <loglib/histogram_bucket_index.hpp>
#include <loglib/log_configuration.hpp>
#include <loglib/log_level.hpp>
#include <loglib/log_table.hpp>
#include <loglib/log_value.hpp>

#include <QAbstractItemModel>
#include <QTimer>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <optional>

namespace
{

/// Coalesce cadence for `bucketsChanged`. Matches the table's
/// existing live-tail batch cadence — a fresh burst of rows still
/// only produces one repaint per ~50 ms.
constexpr int EMIT_COALESCE_MS = 50;

} // namespace

HistogramModel::HistogramModel(LogModel *logModel, QObject *parent) : QObject(parent), mLogModel(logModel)
{
    mEmitTimer = new QTimer(this);
    mEmitTimer->setSingleShot(true);
    mEmitTimer->setInterval(EMIT_COALESCE_MS);
    connect(mEmitTimer, &QTimer::timeout, this, &HistogramModel::bucketsChanged);

    if (mLogModel != nullptr)
    {
        connect(mLogModel, &QAbstractItemModel::rowsInserted, this, &HistogramModel::OnRowsInserted);
        connect(mLogModel, &QAbstractItemModel::rowsRemoved, this, &HistogramModel::OnRowsRemoved);
        connect(mLogModel, &QAbstractItemModel::modelReset, this, &HistogramModel::OnModelReset);
        // `enumColumnsChanged` fires after `AppendBatch` when an enum
        // column is promoted / grown / demoted. The signal carries a
        // reason + column index, but we ignore both and let
        // `OnEnumColumnsChanged` re-derive the level column index --
        // an unrelated enum column promoting doesn't concern us, and
        // the cost of the diff check is trivial next to the rebuild
        // it may trigger. A lambda drops the trailing args cleanly.
        connect(mLogModel, &LogModel::enumColumnsChanged, this, [this](EnumColumnsChangeReason, int) {
            OnEnumColumnsChanged();
        });
    }

    // Prime with whatever the model already holds (typical when the
    // dock is created after a session is already loaded).
    OnModelReset();
}

void HistogramModel::SetBucketSize(loglib::HistogramBucketSize size)
{
    mBucketSizePinned = true;
    if (mIndex.BucketSize() == size)
    {
        return;
    }
    mIndex.SetBucketSize(size); // Resets internal buckets.
    Rebuild();
}

void HistogramModel::ApplyAutoBucketSize()
{
    if (mBucketSizePinned)
    {
        return;
    }
    const auto range = ObservedRange();
    if (!range.has_value())
    {
        return;
    }
    const auto picked = loglib::HistogramBucketIndex::AutoBucketSize(range->min, range->max);
    if (picked == mIndex.BucketSize())
    {
        return;
    }
    mIndex.SetBucketSize(picked);
    Rebuild();
}

void HistogramModel::ResetBucketSizeToAuto()
{
    // The pin exists to keep streaming inserts from re-picking the
    // rung under a user who explicitly zoomed. "Reset zoom (auto)"
    // is the user un-doing that pin, so drop it before delegating.
    mBucketSizePinned = false;
    ApplyAutoBucketSize();
}

void HistogramModel::Rebuild()
{
    InvalidateFirstRowCache();
    if (mLogModel == nullptr)
    {
        mIndex.Reset();
        ScheduleEmit();
        return;
    }
    mIndex.Reset();
    if (mTimeColumnIndex < 0)
    {
        ScheduleEmit();
        return;
    }
    const int rowCount = mLogModel->rowCount();
    if (rowCount > 0)
    {
        AppendRange(0, rowCount - 1);
    }
    ScheduleEmit();
}

int HistogramModel::FirstRowInBucket(std::size_t bucketIndex) const
{
    if (mLogModel == nullptr || mTimeColumnIndex < 0)
    {
        return -1;
    }
    if (bucketIndex >= mIndex.Buckets().size())
    {
        return -1;
    }
    // Lazy cache build. First click after a data change pays one
    // O(N) walk; every subsequent click across the same data is
    // O(1). The cache is invalidated eagerly from every mutation
    // path (`Rebuild`, `AppendRange`, `OnRowsRemoved`), so a stale
    // lookup is impossible without a use-after-Reset bug in the
    // caller — protected here by the size check below.
    if (!mFirstRowPerBucketCache.has_value())
    {
        BuildFirstRowCache();
    }
    const auto &cache = *mFirstRowPerBucketCache;
    if (bucketIndex >= cache.size())
    {
        return -1;
    }
    return cache[bucketIndex];
}

std::optional<HistogramModel::TimeRange> HistogramModel::ObservedRange() const
{
    if (mLogModel == nullptr || mTimeColumnIndex < 0)
    {
        return std::nullopt;
    }
    // Fast path: the index tracks precise min / max timestamps as
    // rows are added (O(1) per `AddRow`). Reading them here avoids
    // the second full-model walk that used to fire on every
    // `OnModelReset` alongside `Rebuild`, *and* avoids the earlier
    // bucket-boundary approximation that inflated the range by up
    // to one `BucketWidth()` — a difference big enough to make
    // `AutoBucketSize` pick a coarser rung than the true range
    // warrants after a `SetBucketSize` -> `ResetBucketSizeToAuto`
    // round-trip on a tight span.
    const auto minTs = mIndex.MinTimestamp();
    const auto maxTs = mIndex.MaxTimestamp();
    if (minTs.has_value() && maxTs.has_value())
    {
        return TimeRange{*minTs, *maxTs};
    }
    // Slow path: the index is empty but the model still holds rows.
    // Happens only when every row's timestamp failed to parse (so
    // `AddRow` was never called). Walk the model rather than lie
    // about the range.
    const int rowCount = mLogModel->rowCount();
    if (rowCount == 0)
    {
        return std::nullopt;
    }
    std::optional<loglib::TimeStamp> walkedMin;
    std::optional<loglib::TimeStamp> walkedMax;
    for (int row = 0; row < rowCount; ++row)
    {
        const auto ts = TimeStampForRow(row);
        if (!ts.has_value())
        {
            continue;
        }
        if (!walkedMin.has_value() || *ts < *walkedMin)
        {
            walkedMin = ts;
        }
        if (!walkedMax.has_value() || *ts > *walkedMax)
        {
            walkedMax = ts;
        }
    }
    if (!walkedMin.has_value() || !walkedMax.has_value())
    {
        return std::nullopt;
    }
    return TimeRange{*walkedMin, *walkedMax};
}

void HistogramModel::OnRowsInserted(const QModelIndex &parent, int first, int last)
{
    (void)parent; // `LogModel` is a table model — parent is always root.

    // Detect a time-column availability flip mid-stream: the JSON /
    // logfmt / regex parsers can promote a column to `Type::Time`
    // after the first `AppendBatch`, and `LogTable` retroactively
    // rewrites the earlier string slots to `TimeStamp` values. When
    // that happens, we have to rebuild from row 0 rather than just
    // append the fresh range.
    //
    // The level column can flip in the same window (enum promoted to
    // `Type::Level` on the same batch that adds the first level
    // values), so re-check it alongside. `enumColumnsChanged` also
    // covers this via `OnEnumColumnsChanged`, but the signal is
    // emitted *after* `rowsInserted`; catching it here avoids one
    // wasted `AppendRange` + one wasted rebuild when both columns
    // flip in the same batch.
    const int freshTimeColumn = ComputeTimeColumnIndex();
    const int freshLevelColumn = ComputeLevelColumnIndex();
    if (freshTimeColumn != mTimeColumnIndex || freshLevelColumn != mLevelColumnIndex)
    {
        const bool wasUnavailable = mTimeColumnIndex < 0;
        mTimeColumnIndex = freshTimeColumn;
        mLevelColumnIndex = freshLevelColumn;
        if (wasUnavailable != (mTimeColumnIndex < 0))
        {
            emit timeColumnAvailabilityChanged(mTimeColumnIndex >= 0);
        }
        // Rebuild picks up every earlier row whose slot was rewritten
        // and re-attributes previously-Unknown levels to their
        // canonical slot after a mid-stream level promotion.
        Rebuild();
        ApplyAutoBucketSize();
        return;
    }

    if (mTimeColumnIndex < 0)
    {
        return;
    }
    AppendRange(first, last);
    ScheduleEmit();
}

void HistogramModel::OnRowsRemoved(const QModelIndex &parent, int first, int last)
{
    (void)parent;
    (void)first;
    (void)last;
    // Retention eviction: we can't cheaply subtract the removed
    // rows from the index (their `(ts, level)` are already gone).
    // Rebuild is O(N) but the acceptance-bar benchmark runs the
    // whole 1M-row rebuild in ~3.5 ms, so we can afford it.
    Rebuild();
}

void HistogramModel::OnModelReset()
{
    const int newTimeColumn = ComputeTimeColumnIndex();
    const bool availabilityChanged = (newTimeColumn >= 0) != (mTimeColumnIndex >= 0);
    mTimeColumnIndex = newTimeColumn;
    mLevelColumnIndex = ComputeLevelColumnIndex();
    if (availabilityChanged)
    {
        emit timeColumnAvailabilityChanged(mTimeColumnIndex >= 0);
    }
    // Reset unpin so a fresh session picks the auto rung again.
    mBucketSizePinned = false;
    Rebuild();
    ApplyAutoBucketSize();
}

void HistogramModel::OnEnumColumnsChanged()
{
    // Cheap guard: if the level column index is unchanged the enum
    // signal was for an unrelated column (or a `Grew` on the existing
    // level column, which doesn't change any per-bucket count -- the
    // levels themselves are resolved fresh at every `LevelForRow`).
    // Bail without a rebuild so live-tail promotions on 20-column
    // logs don't repeatedly trip a full O(N) rebuild.
    const int freshLevelColumn = ComputeLevelColumnIndex();
    if (freshLevelColumn == mLevelColumnIndex)
    {
        return;
    }
    mLevelColumnIndex = freshLevelColumn;
    // Rows in `mIndex` were bucketed against the *old* level slot
    // (typically `Unknown` when the column just promoted). Rebuild
    // so their canonical level shows up in the strip.
    Rebuild();
}

void HistogramModel::AppendRange(int first, int last)
{
    if (mLogModel == nullptr || mTimeColumnIndex < 0)
    {
        return;
    }
    const int rowCount = mLogModel->rowCount();
    const int clampedLast = std::min(last, rowCount - 1);
    for (int row = std::max(0, first); row <= clampedLast; ++row)
    {
        const auto ts = TimeStampForRow(row);
        if (!ts.has_value())
        {
            continue;
        }
        mIndex.AddRow(*ts, LevelForRow(row));
    }
    // Bucket geometry (origin + count) shifts under `AddRow`, so
    // any incremental cache would need to shift too. Drop and let
    // the next `FirstRowInBucket` rebuild instead — that's cheaper
    // than re-anchoring the cache on every batch, and the click
    // rate is typically much lower than the streaming batch rate.
    InvalidateFirstRowCache();
}

int HistogramModel::ComputeTimeColumnIndex() const
{
    if (mLogModel == nullptr)
    {
        return -1;
    }
    const auto &config = mLogModel->Configuration();
    for (std::size_t i = 0; i < config.columns.size(); ++i)
    {
        if (config.columns[i].type == loglib::LogConfiguration::Type::Time)
        {
            return static_cast<int>(i);
        }
    }
    return -1;
}

int HistogramModel::ComputeLevelColumnIndex() const
{
    if (mLogModel == nullptr)
    {
        return -1;
    }
    const auto &config = mLogModel->Configuration();
    for (std::size_t i = 0; i < config.columns.size(); ++i)
    {
        if (config.columns[i].type == loglib::LogConfiguration::Type::Level)
        {
            return static_cast<int>(i);
        }
    }
    return -1;
}

std::optional<loglib::TimeStamp> HistogramModel::TimeStampForRow(int row) const
{
    if (mLogModel == nullptr || mTimeColumnIndex < 0)
    {
        return std::nullopt;
    }
    const auto value = mLogModel->Table().GetValue(static_cast<std::size_t>(row), static_cast<std::size_t>(mTimeColumnIndex));
    const auto epochMicros = loglib::AsEpochMicroseconds(value);
    if (!epochMicros.has_value())
    {
        return std::nullopt;
    }
    return loglib::TimeStamp{std::chrono::microseconds{*epochMicros}};
}

loglib::LogLevel HistogramModel::LevelForRow(int row) const
{
    if (mLogModel == nullptr || mLevelColumnIndex < 0)
    {
        return loglib::LogLevel::Unknown;
    }
    // Prefer our cached column index over `LogModel::FirstLevelColumnIndex()`:
    // both are cheap, but hitting LogModel every row invalidated its
    // own cache-repair invariants under the invalidate-then-restore
    // dance in `AppendBatch`. Using the mirror also keeps the level
    // slot consistent with the column identity our rebuild guards
    // (`OnRowsInserted` / `OnEnumColumnsChanged`) reacted to.
    //
    // Values that don't resolve to a canonical level fall through to
    // `Unknown` (slot 0). `DisplayLevelForRow` would also fold them
    // into `Unknown` for display, but we deliberately skip that hop
    // -- the bucket slot is a semantic bucket, not a rendering hint,
    // and the two happen to line up.
    const auto level = mLogModel->Table().GetLevelForRow(
        static_cast<std::size_t>(row), static_cast<std::size_t>(mLevelColumnIndex)
    );
    return level.value_or(loglib::LogLevel::Unknown);
}

void HistogramModel::ScheduleEmit()
{
    if (!mEmitTimer->isActive())
    {
        mEmitTimer->start();
    }
}

void HistogramModel::InvalidateFirstRowCache() const noexcept
{
    mFirstRowPerBucketCache.reset();
}

void HistogramModel::BuildFirstRowCache() const
{
    // Preconditions verified by the sole caller (`FirstRowInBucket`).
    Q_ASSERT(mLogModel != nullptr);
    Q_ASSERT(mTimeColumnIndex >= 0);
    std::vector<int> cache(mIndex.Buckets().size(), -1);
    const int rowCount = mLogModel->rowCount();
    for (int row = 0; row < rowCount; ++row)
    {
        const auto ts = TimeStampForRow(row);
        if (!ts.has_value())
        {
            continue;
        }
        const auto bucketOpt = mIndex.BucketOf(*ts);
        if (!bucketOpt.has_value())
        {
            // Row lives outside the current index (shouldn't happen
            // in practice — the index is rebuilt from the same
            // rows). Skip rather than resize; the missing bucket
            // stays at `-1` and reports "no visible row".
            continue;
        }
        const std::size_t bucketIdx = *bucketOpt;
        if (bucketIdx < cache.size() && cache[bucketIdx] == -1)
        {
            cache[bucketIdx] = row;
        }
    }
    mFirstRowPerBucketCache = std::move(cache);
}
