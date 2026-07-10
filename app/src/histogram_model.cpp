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
        // `columnsMoved` fires when `LogModel` bubbles a freshly
        // observed `Type::Time` column to position 0 or a
        // `Type::Level` column to `CANONICAL_LEVEL_COLUMN_INDEX`.
        // These reorderings happen post-`AppendBatch` without
        // inserting new rows, so `OnRowsInserted` doesn't see them
        // and our cached column indices go stale. The signal carries
        // parent / source / destination args we don't need -- a
        // lambda drops them so the slot signature stays lean.
        connect(mLogModel, &QAbstractItemModel::columnsMoved, this,
                [this](const QModelIndex &, int, int, const QModelIndex &, int) { OnColumnsMoved(); });
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

    if (mAnchors != nullptr)
    {
        connect(mAnchors, &AnchorManager::anchorChanged, this, &HistogramModel::OnAnchorChanged);
        connect(mAnchors, &AnchorManager::anchorsReset, this, &HistogramModel::OnAnchorsReset);
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
        // Bucket geometry is now empty. Sync the anchor vector so
        // `HasAnchorTicks` doesn't outlive the buckets it references.
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
    // `AppendRange` above already grew `mAnchorSlotPerBucket` and
    // seeded anchors for freshly inserted rows. A full `Rebuild`
    // however has just wiped `mIndex`, so any earlier bucket -> slot
    // mapping is invalid; walk the anchor manager once here to
    // resettle every anchor onto the fresh bucket geometry.
    RebuildAnchorBuckets();
    ScheduleEmit();
}

int HistogramModel::FirstAnchoredRowInBucketRange(std::size_t bucketBegin, std::size_t bucketEnd) const
{
    if (mAnchors == nullptr || mLogModel == nullptr || mTimeColumnIndex < 0 || bucketBegin >= bucketEnd)
    {
        return -1;
    }
    // Anchors are typically single-digit-to-tens; iterating the
    // manager and doing an O(N) `SourceRowForAnchorKey` per hit is
    // cheaper than maintaining a separate `bucket -> anchor row`
    // index and keeping it in sync across every insert / retention
    // event. Runtime-only anchors are included so an in-memory
    // stream still routes ticks to the anchored row.
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
        return TimeRange{.min = *minTs, .max = *maxTs};
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
    return TimeRange{.min = *walkedMin, .max = *walkedMax};
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
    // Cheap guard: if neither cached column index moved the enum
    // signal was for an unrelated column (or a `Grew` on the existing
    // level column, which doesn't change any per-bucket count -- the
    // levels themselves are resolved fresh at every `LevelForRow`).
    // Bail without a rebuild so live-tail promotions on 20-column
    // logs don't repeatedly trip a full O(N) rebuild.
    //
    // We also refresh the time column here because the same
    // finalize / promote path can bubble a freshly-typed `Type::Time`
    // column to position 0 alongside a level promotion (see
    // `LogModel::EndStreaming`). Rebuilding against a stale time
    // column would feed the index the wrong column's raw values.
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
    // Rows in `mIndex` were bucketed against the *old* level slot
    // (typically `Unknown` when the column just promoted). Rebuild
    // so their canonical level shows up in the strip.
    Rebuild();
}

void HistogramModel::OnColumnsMoved()
{
    // Reorderings that don't touch our two tracked columns are
    // no-ops -- most notably the user dragging an unrelated column
    // in the header, which mustn't repaint the strip.
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
    // The move mutated column identity behind rows already in the
    // index (`AddRow` recorded values from the *old* column slot).
    // Rebuild from row 0 against the fresh identity.
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
        // Anchor tick incremental update: for each streamed row that
        // happens to be anchored (typical count is single-digit
        // across the whole session), OR its palette slot into the
        // bucket-slot mask. Cost is one hashmap lookup + one
        // `BucketOf` per anchored row -- effectively free next to
        // the log parse throughput.
        if (trackAnchors)
        {
            const auto slot = mLogModel->AnchorSlotForRow(row);
            if (slot.has_value())
            {
                const auto bucketOpt = mIndex.BucketOf(*ts);
                if (bucketOpt.has_value())
                {
                    // The bucket vector grows as `AddRow` observes
                    // fresh timestamps, so resync before indexing so
                    // we can't write past the vector end even when
                    // the current row extended the range.
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
    // Ensure the anchor vector matches the (possibly expanded)
    // bucket vector even when the batch itself had no anchored rows
    // -- otherwise a later `OnAnchorChanged` could index a bucket
    // that has no slot in `mAnchorSlotPerBucket`.
    SyncAnchorBucketVectorSize();
    // Bucket geometry (origin + count) shifts under `AddRow`, so
    // any incremental cache would need to shift too. Drop and let
    // the next `FirstRowInBucket` rebuild instead — that's cheaper
    // than re-anchoring the cache on every batch, and the click
    // rate is typically much lower than the streaming batch rate.
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

void HistogramModel::SyncAnchorBucketVectorSize()
{
    if (mAnchors == nullptr)
    {
        // Anchor tracking disabled -- the vector stays empty. Keep
        // the popcount in sync so `HasAnchorTicks` returns false.
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
        // Shrink: subtract the bits we're dropping from the running
        // popcount. Only Rebuild + Reset shrink, and both wipe the
        // whole vector, but the accounting stays honest for any
        // future partial-shrink path.
        for (std::size_t i = bucketCount; i < mAnchorSlotPerBucket.size(); ++i)
        {
            mAnchorBucketBitsSet -= mAnchorSlotPerBucket[i].count();
        }
        mAnchorSlotPerBucket.resize(bucketCount);
        return;
    }
    // Grow: default-constructed masks are zero, so `mAnchorBucketBitsSet`
    // doesn't need adjustment.
    mAnchorSlotPerBucket.resize(bucketCount);
}

void HistogramModel::RebuildAnchorBuckets()
{
    // Zero the vector and popcount, then reseed from the anchor
    // manager. Emits `anchorBucketsChanged` iff the pre/post state
    // differs (measured via the popcount + a cheap tail-comparison).
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
    // Snapshot the previous state so we can detect a genuine change
    // rather than emitting on every reset. Comparing bitsets is
    // cheap (one `memcmp`-shaped call per bucket) so the snapshot
    // path is fine even at 500 buckets.
    std::vector<AnchorSlotMask> previous;
    previous.swap(mAnchorSlotPerBucket);
    const std::size_t previousBitsSet = mAnchorBucketBitsSet;
    mAnchorBucketBitsSet = 0;

    SyncAnchorBucketVectorSize();

    if (mLogModel == nullptr || mTimeColumnIndex < 0)
    {
        // Nothing to bucket against. Emit only if the previous state
        // was non-empty.
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
    // The bit we need to flip depends on whether this event is an
    // add/recolour or a remove. Rather than diffing per-slot (harder
    // once recolour is in play — the old slot is gone from
    // `AnchorManager` by the time this fires), just rebuild the
    // affected bucket from every remaining anchor whose row lands
    // in it. For typical anchor counts (single digits) that's a few
    // hashmap lookups + `BucketOf` calls, well below the paint budget.
    SyncAnchorBucketVectorSize();

    // Resolve the changed key to the bucket it *was* in, so we know
    // which bucket's mask to rebuild. If the key resolves to a live
    // row, use its current bucket; otherwise fall back to a full
    // scan (rare: removal after retention eviction).
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
        // We can't localise the change — fall back to a full rebuild.
        // This still hits the O(anchors) upper bound rather than an
        // O(rows) scan; anchors are the small set here.
        RebuildAnchorBuckets();
        return;
    }

    // Recompute just that one bucket: clear its bits, subtract from
    // the running popcount, then reseed from every anchor whose row
    // resolves into it.
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
