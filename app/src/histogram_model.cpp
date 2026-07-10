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
/// live-tail batch cadence so a row burst yields one repaint.
constexpr int EMIT_COALESCE_MS = 50;

} // namespace

HistogramModel::HistogramModel(LogModel *logModel, AnchorManager *anchors, QObject *parent)
    : QObject(parent), mLogModel(logModel), mAnchors(anchors)
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
        // Column reorderings after `AppendBatch` (time bubbled to slot 0,
        // level to canonical slot) don't emit `rowsInserted`, so hook
        // `columnsMoved` explicitly. Lambda strips the unused args.
        connect(mLogModel, &QAbstractItemModel::columnsMoved, this,
                [this](const QModelIndex &, int, int, const QModelIndex &, int) { OnColumnsMoved(); });
        // Enum promotion / grow / demote can shift or introduce the
        // level column. Let `OnEnumColumnsChanged` do the diff itself.
        connect(mLogModel, &LogModel::enumColumnsChanged, this, [this](EnumColumnsChangeReason, int) {
            OnEnumColumnsChanged();
        });
    }

    if (mAnchors != nullptr)
    {
        connect(mAnchors, &AnchorManager::anchorChanged, this, &HistogramModel::OnAnchorChanged);
        connect(mAnchors, &AnchorManager::anchorsReset, this, &HistogramModel::OnAnchorsReset);
    }

    // Prime with any rows already in the model (dock created after load).
    OnModelReset();
}

void HistogramModel::SetBucketSize(loglib::HistogramBucketSize size)
{
    mBucketSizePinned = true;
    if (mIndex.BucketSize() == size)
    {
        return;
    }
    mIndex.SetBucketSize(size); // Also resets internal buckets.
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
    // Drop the pin so the auto-pick below isn't vetoed.
    mBucketSizePinned = false;
    ApplyAutoBucketSize();
}

void HistogramModel::Rebuild()
{
    InvalidateFirstRowCache();
    if (mLogModel == nullptr)
    {
        mIndex.Reset();
        SyncAnchorBucketVectorSize();
        RebuildAnchorBuckets();
        ScheduleEmit();
        return;
    }
    mIndex.Reset();
    if (mTimeColumnIndex < 0)
    {
        SyncAnchorBucketVectorSize();
        RebuildAnchorBuckets();
        ScheduleEmit();
        return;
    }
    const int rowCount = mLogModel->rowCount();
    if (rowCount > 0)
    {
        AppendRange(0, rowCount - 1);
    }
    // `AppendRange` seeded anchors for the fresh rows, but the pre-Reset
    // bucket->slot mapping is gone; re-walk the manager to resettle
    // anchors against the new geometry.
    RebuildAnchorBuckets();
    ScheduleEmit();
}

int HistogramModel::FirstAnchoredRowInBucketRange(std::size_t bucketBegin, std::size_t bucketEnd) const
{
    if (mAnchors == nullptr || mLogModel == nullptr || mTimeColumnIndex < 0 || bucketBegin >= bucketEnd)
    {
        return -1;
    }
    // The anchor set is small (single digits to tens); an O(anchors) scan
    // is cheaper than maintaining a separate bucket->anchor row index.
    // Runtime-only anchors are included so in-memory streams still route.
    int bestRow = -1;
    for (const auto &entry : mAnchors->EntriesIncludingRuntimeOnly())
    {
        const AnchorManager::Key key{.locator = entry.locator, .lineId = entry.lineId};
        const int row = mLogModel->SourceRowForAnchorKey(key);
        if (row < 0)
        {
            continue;
        }
        const auto ts = TimeStampForRow(row);
        if (!ts.has_value())
        {
            continue;
        }
        const auto bucketOpt = mIndex.BucketOf(*ts);
        if (!bucketOpt.has_value())
        {
            continue;
        }
        const std::size_t bucket = *bucketOpt;
        if (bucket < bucketBegin || bucket >= bucketEnd)
        {
            continue;
        }
        if (bestRow < 0 || row < bestRow)
        {
            bestRow = row;
        }
    }
    return bestRow;
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
    // Lazy build. First click after a data change is O(N); later
    // clicks are O(1). The cache is invalidated on every mutation.
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
    // Fast path: the index tracks precise min/max in O(1) per AddRow,
    // so we don't need a second full-model walk and don't have to
    // snap to bucket boundaries (which would inflate the range by up
    // to one bucket width and mislead `AutoBucketSize`).
    const auto minTs = mIndex.MinTimestamp();
    const auto maxTs = mIndex.MaxTimestamp();
    if (minTs.has_value() && maxTs.has_value())
    {
        return TimeRange{.min = *minTs, .max = *maxTs};
    }
    // Slow path: the index is empty but the model has rows. Happens
    // when every row's timestamp failed to parse. Walk the model
    // rather than lie about the range.
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
    return TimeRange{.min = *walkedMin, .max = *walkedMax};
}

void HistogramModel::OnRowsInserted(const QModelIndex &parent, int first, int last)
{
    (void)parent; // Table model: parent is always root.

    // Parsers can promote a column to `Type::Time` (or `Type::Level`)
    // mid-stream and retroactively rewrite earlier rows' slots. When
    // that happens we must rebuild from row 0 rather than just append.
    // We check both columns here (not only in `OnEnumColumnsChanged`)
    // to avoid a wasted append+rebuild when both flip in one batch.
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
    // Retention eviction: we can't cheaply subtract removed rows
    // (their (ts, level) are gone by the time this fires). Rebuild
    // is O(N); the 1M-row benchmark clocks ~3.5 ms so it's affordable.
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
    // Drop the pin so a fresh session gets a fresh auto rung.
    mBucketSizePinned = false;
    Rebuild();
    ApplyAutoBucketSize();
}

void HistogramModel::OnEnumColumnsChanged()
{
    // Cheap guard: bail if neither tracked column moved. Unrelated
    // enum growth (or a Grew on the existing level column, since
    // levels are resolved fresh in `LevelForRow`) mustn't trigger a
    // full rebuild on every batch of a wide log.
    const int freshTimeColumn = ComputeTimeColumnIndex();
    const int freshLevelColumn = ComputeLevelColumnIndex();
    if (freshTimeColumn == mTimeColumnIndex && freshLevelColumn == mLevelColumnIndex)
    {
        return;
    }
    const bool wasUnavailable = mTimeColumnIndex < 0;
    mTimeColumnIndex = freshTimeColumn;
    mLevelColumnIndex = freshLevelColumn;
    if (wasUnavailable != (mTimeColumnIndex < 0))
    {
        emit timeColumnAvailabilityChanged(mTimeColumnIndex >= 0);
    }
    // Pre-promotion rows landed in `Unknown`; rebuild re-attributes them.
    Rebuild();
}

void HistogramModel::OnColumnsMoved()
{
    // Bail if neither tracked column moved -- e.g. the user dragged
    // an unrelated header, which mustn't repaint the strip.
    const int freshTimeColumn = ComputeTimeColumnIndex();
    const int freshLevelColumn = ComputeLevelColumnIndex();
    if (freshTimeColumn == mTimeColumnIndex && freshLevelColumn == mLevelColumnIndex)
    {
        return;
    }
    const bool wasUnavailable = mTimeColumnIndex < 0;
    mTimeColumnIndex = freshTimeColumn;
    mLevelColumnIndex = freshLevelColumn;
    if (wasUnavailable != (mTimeColumnIndex < 0))
    {
        emit timeColumnAvailabilityChanged(mTimeColumnIndex >= 0);
    }
    // The move changed column identity behind existing rows, so
    // rebuild against the fresh identity.
    Rebuild();
    ApplyAutoBucketSize();
}

void HistogramModel::AppendRange(int first, int last)
{
    if (mLogModel == nullptr || mTimeColumnIndex < 0)
    {
        return;
    }
    const int rowCount = mLogModel->rowCount();
    const int clampedLast = std::min(last, rowCount - 1);
    const bool trackAnchors = mAnchors != nullptr && !mAnchors->Empty();
    bool anchorMaskChanged = false;
    for (int row = std::max(0, first); row <= clampedLast; ++row)
    {
        const auto ts = TimeStampForRow(row);
        if (!ts.has_value())
        {
            continue;
        }
        mIndex.AddRow(*ts, LevelForRow(row));
        // Incremental anchor tick update: OR any anchored row's
        // palette slot into its bucket mask. Anchored rows are rare,
        // so the extra hashmap lookup is dwarfed by parse throughput.
        if (trackAnchors)
        {
            const auto slot = mLogModel->AnchorSlotForRow(row);
            if (slot.has_value())
            {
                const auto bucketOpt = mIndex.BucketOf(*ts);
                if (bucketOpt.has_value())
                {
                    // `AddRow` may have grown the bucket vector; resync
                    // before indexing so we don't overrun.
                    SyncAnchorBucketVectorSize();
                    auto &mask = mAnchorSlotPerBucket[*bucketOpt];
                    if (!mask.test(*slot))
                    {
                        mask.set(*slot);
                        ++mAnchorBucketBitsSet;
                        anchorMaskChanged = true;
                    }
                }
            }
        }
    }
    // Resync even if the batch had no anchored rows so a later
    // `OnAnchorChanged` doesn't index a bucket with no mask slot.
    SyncAnchorBucketVectorSize();
    // Bucket geometry shifts under `AddRow`; drop the cache rather
    // than trying to shift it incrementally.
    InvalidateFirstRowCache();
    if (anchorMaskChanged)
    {
        emit anchorBucketsChanged();
    }
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
    // Prefer our cached index over `LogModel::FirstLevelColumnIndex()`:
    // hitting LogModel per row would race its own cache-repair dance
    // inside `AppendBatch`. Non-canonical values fall through to
    // `Unknown` (slot 0), matching `DisplayLevelForRow`.
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
            // Row outside the current index (shouldn't happen in
            // practice). Leave the slot at -1 -> "no visible row".
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

void HistogramModel::SyncAnchorBucketVectorSize()
{
    if (mAnchors == nullptr)
    {
        mAnchorSlotPerBucket.clear();
        mAnchorBucketBitsSet = 0;
        return;
    }
    const std::size_t bucketCount = mIndex.Buckets().size();
    if (mAnchorSlotPerBucket.size() == bucketCount)
    {
        return;
    }
    if (mAnchorSlotPerBucket.size() > bucketCount)
    {
        // Shrink: subtract the bits we're dropping.
        for (std::size_t i = bucketCount; i < mAnchorSlotPerBucket.size(); ++i)
        {
            mAnchorBucketBitsSet -= mAnchorSlotPerBucket[i].count();
        }
        mAnchorSlotPerBucket.resize(bucketCount);
        return;
    }
    // Grow: new entries are zero-masks, so the popcount is unchanged.
    mAnchorSlotPerBucket.resize(bucketCount);
}

void HistogramModel::RebuildAnchorBuckets()
{
    // Zero, reseed from the anchor manager, then emit
    // `anchorBucketsChanged` iff the pre/post state actually differs.
    if (mAnchors == nullptr)
    {
        if (mAnchorBucketBitsSet > 0 || !mAnchorSlotPerBucket.empty())
        {
            mAnchorSlotPerBucket.clear();
            mAnchorBucketBitsSet = 0;
            emit anchorBucketsChanged();
        }
        return;
    }
    // Snapshot so we detect a real change rather than emitting on
    // every reset. Bitset comparison stays cheap at ~500 buckets.
    std::vector<AnchorSlotMask> previous;
    previous.swap(mAnchorSlotPerBucket);
    const std::size_t previousBitsSet = mAnchorBucketBitsSet;
    mAnchorBucketBitsSet = 0;

    SyncAnchorBucketVectorSize();

    if (mLogModel == nullptr || mTimeColumnIndex < 0)
    {
        if (previousBitsSet > 0)
        {
            emit anchorBucketsChanged();
        }
        return;
    }

    for (const auto &entry : mAnchors->EntriesIncludingRuntimeOnly())
    {
        const AnchorManager::Key key{.locator = entry.locator, .lineId = entry.lineId};
        (void)UpdateAnchorSlotForKey(key);
    }

    const bool changed =
        previousBitsSet != mAnchorBucketBitsSet || previous.size() != mAnchorSlotPerBucket.size() ||
        !std::equal(previous.begin(), previous.end(), mAnchorSlotPerBucket.begin(), mAnchorSlotPerBucket.end());
    if (changed)
    {
        emit anchorBucketsChanged();
    }
}

std::optional<std::size_t> HistogramModel::UpdateAnchorSlotForKey(const AnchorManager::Key &key)
{
    if (mAnchors == nullptr || mLogModel == nullptr || mTimeColumnIndex < 0)
    {
        return std::nullopt;
    }
    const int row = mLogModel->SourceRowForAnchorKey(key);
    if (row < 0)
    {
        return std::nullopt;
    }
    const auto slot = mAnchors->ColorFor(key);
    if (!slot.has_value())
    {
        return std::nullopt;
    }
    const auto ts = TimeStampForRow(row);
    if (!ts.has_value())
    {
        return std::nullopt;
    }
    const auto bucketOpt = mIndex.BucketOf(*ts);
    if (!bucketOpt.has_value())
    {
        return std::nullopt;
    }
    const std::size_t bucketIdx = *bucketOpt;
    if (bucketIdx >= mAnchorSlotPerBucket.size())
    {
        return std::nullopt;
    }
    auto &mask = mAnchorSlotPerBucket[bucketIdx];
    if (!mask.test(*slot))
    {
        mask.set(*slot);
        ++mAnchorBucketBitsSet;
    }
    return bucketIdx;
}

void HistogramModel::OnAnchorChanged(const AnchorManager::Key &key)
{
    if (mAnchors == nullptr)
    {
        return;
    }
    // Diffing per-slot is fragile (a recolour drops the old slot from
    // AnchorManager before this fires). Instead, rebuild the affected
    // bucket from scratch by walking every anchor whose row lands in
    // it -- cheap given typical anchor counts.
    SyncAnchorBucketVectorSize();

    // Resolve the changed key to its bucket. Falls back to a full
    // rebuild when the key can't be localised (rare: remove after
    // retention eviction).
    std::optional<std::size_t> targetBucket;
    if (mLogModel != nullptr && mTimeColumnIndex >= 0)
    {
        const int row = mLogModel->SourceRowForAnchorKey(key);
        if (row >= 0)
        {
            const auto ts = TimeStampForRow(row);
            if (ts.has_value())
            {
                targetBucket = mIndex.BucketOf(*ts);
            }
        }
    }

    if (!targetBucket.has_value())
    {
        RebuildAnchorBuckets();
        return;
    }

    // Rebuild just that bucket: clear, then reseed from every anchor
    // whose row lands in it.
    const std::size_t bucketIdx = *targetBucket;
    if (bucketIdx >= mAnchorSlotPerBucket.size())
    {
        return;
    }
    AnchorSlotMask &mask = mAnchorSlotPerBucket[bucketIdx];
    const AnchorSlotMask previous = mask;
    mAnchorBucketBitsSet -= mask.count();
    mask.reset();

    for (const auto &entry : mAnchors->EntriesIncludingRuntimeOnly())
    {
        const AnchorManager::Key entryKey{.locator = entry.locator, .lineId = entry.lineId};
        const int row = mLogModel->SourceRowForAnchorKey(entryKey);
        if (row < 0)
        {
            continue;
        }
        const auto ts = TimeStampForRow(row);
        if (!ts.has_value())
        {
            continue;
        }
        const auto anchorBucket = mIndex.BucketOf(*ts);
        if (!anchorBucket.has_value() || *anchorBucket != bucketIdx)
        {
            continue;
        }
        if (!mask.test(entry.colorIndex))
        {
            mask.set(entry.colorIndex);
        }
    }
    mAnchorBucketBitsSet += mask.count();
    if (mask != previous)
    {
        emit anchorBucketsChanged();
    }
}

void HistogramModel::OnAnchorsReset()
{
    RebuildAnchorBuckets();
}
