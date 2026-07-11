#include "overview_rail_model.hpp"

#include "log_model.hpp"

#include <loglib/log_configuration.hpp>
#include <loglib/log_level.hpp>
#include <loglib/log_table.hpp>

#include <QAbstractItemModel>
#include <QAbstractProxyModel>
#include <QModelIndex>
#include <QTimer>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace
{

/// Coalesce cadence for `bucketsChanged`. Matches the histogram's
/// 50 ms cadence and the table's live-tail batch cadence so a
/// row burst yields at most one repaint.
constexpr int EMIT_COALESCE_MS = 50;

/// Severity ranking for the dominant-level tie-break. Higher
/// value == more severe, so ties go to the row colour users are
/// more likely to want to see. Indexed by
/// `static_cast<size_t>(LogLevel)`.
constexpr std::array<int, loglib::CANONICAL_LEVEL_COUNT + 1> LEVEL_SEVERITY_RANK = {
    0, // Unknown
    1, // Trace
    2, // Debug
    3, // Info
    4, // Warn
    5, // Error
    6, // Fatal
};

} // namespace

OverviewRailModel::OverviewRailModel(
    QAbstractItemModel *proxyModel, LogModel *sourceModel, AnchorManager *anchors, QObject *parent
)
    : QObject(parent), mProxyModel(proxyModel), mSourceModel(sourceModel), mAnchors(anchors)
{
    mEmitTimer = new QTimer(this);
    mEmitTimer->setSingleShot(true);
    mEmitTimer->setInterval(EMIT_COALESCE_MS);
    connect(mEmitTimer, &QTimer::timeout, this, &OverviewRailModel::bucketsChanged);

    if (mProxyModel != nullptr)
    {
        connect(mProxyModel, &QAbstractItemModel::rowsInserted, this, &OverviewRailModel::OnRowsInserted);
        connect(mProxyModel, &QAbstractItemModel::rowsRemoved, this, &OverviewRailModel::OnRowsRemoved);
        connect(mProxyModel, &QAbstractItemModel::modelReset, this, &OverviewRailModel::OnModelReset);
        // A filter/sort change lands as `layoutChanged` on
        // `LogFilterModel`; row inserts are rare after that, so we
        // rebuild here to pick up the new proxy row set.
        connect(mProxyModel, &QAbstractItemModel::layoutChanged, this, &OverviewRailModel::OnLayoutChanged);
        // Column shape changes may move the level column even
        // when no rows insert. Not coalesced with `columnsMoved`
        // because both funnel to the same rebuild anyway.
        connect(mProxyModel, &QAbstractItemModel::columnsMoved, this, &OverviewRailModel::OnColumnsChanged);
        connect(mProxyModel, &QAbstractItemModel::columnsInserted, this, &OverviewRailModel::OnColumnsChanged);
        connect(mProxyModel, &QAbstractItemModel::columnsRemoved, this, &OverviewRailModel::OnColumnsChanged);
    }

    if (mSourceModel != nullptr)
    {
        // Level column can promote / demote mid-stream. Refresh
        // the cache and rebuild so pre-promotion `Unknown` rows
        // get re-attributed to their canonical level.
        connect(
            mSourceModel,
            &LogModel::enumColumnsChanged,
            this,
            [this](EnumColumnsChangeReason, int) { OnEnumColumnsChanged(); }
        );
    }

    if (mAnchors != nullptr)
    {
        connect(mAnchors, &AnchorManager::anchorChanged, this, &OverviewRailModel::OnAnchorChanged);
        connect(mAnchors, &AnchorManager::anchorsReset, this, &OverviewRailModel::OnAnchorsReset);
    }

    // Prime the cached column index and bucket vector.
    mLevelColumnIndex = ComputeLevelColumnIndex();
}

void OverviewRailModel::SetBucketCount(std::size_t nBuckets)
{
    if (mBuckets.size() == nBuckets)
    {
        return;
    }
    mBuckets.assign(nBuckets, Bucket{});
    // Bit count is preserved separately because the fresh buckets
    // start at zero; recompute inside RebuildInternal.
    RebuildInternal();
    // Rail widget consumes the change immediately on the next
    // paint tick, so avoid the coalesce delay here.
    emit bucketsChanged();
}

void OverviewRailModel::SetMatchProxyRows(std::vector<int> proxyRows)
{
    mMatchProxyRows = std::move(proxyRows);
    // Match counts overlay the existing per-level bucket data;
    // rebuild to fold the fresh rows into the bucket vector.
    RebuildInternal();
    emit matchesChanged();
    emit bucketsChanged();
}

void OverviewRailModel::Rebuild()
{
    RebuildInternal();
    ScheduleEmit();
}

loglib::LogLevel OverviewRailModel::DominantLevel(std::size_t bucket) const noexcept
{
    if (bucket >= mBuckets.size())
    {
        return loglib::LogLevel::Unknown;
    }
    const auto &counts = mBuckets[bucket].levels.counts;
    // Pick the entry with the highest count, tie-broken by
    // severity rank (Fatal wins over Error wins over Warn, ...).
    // `Unknown` (slot 0) contributes to the paint like any other
    // level so unresolved rows still tint the rail.
    std::size_t bestSlot = 0;
    uint32_t bestCount = 0;
    int bestSeverity = -1;
    for (std::size_t i = 0; i < counts.size(); ++i)
    {
        const uint32_t count = counts[i];
        if (count == 0)
        {
            continue;
        }
        const int severity = LEVEL_SEVERITY_RANK[i];
        if (count > bestCount || (count == bestCount && severity > bestSeverity))
        {
            bestSlot = i;
            bestCount = count;
            bestSeverity = severity;
        }
    }
    return static_cast<loglib::LogLevel>(bestSlot);
}

int OverviewRailModel::FirstProxyRowInBucket(std::size_t bucket) const noexcept
{
    if (mProxyRowCount <= 0 || mBuckets.empty() || bucket >= mBuckets.size())
    {
        return -1;
    }
    // Inverse of the linear map `bucket = row * N / M`: the first
    // row landing in `bucket` is ceil(bucket * M / N).
    const std::size_t nBuckets = mBuckets.size();
    const long long numerator = static_cast<long long>(bucket) * static_cast<long long>(mProxyRowCount);
    long long firstRow = (numerator + static_cast<long long>(nBuckets) - 1) / static_cast<long long>(nBuckets);
    firstRow = std::clamp<long long>(firstRow, 0, mProxyRowCount - 1);
    return static_cast<int>(firstRow);
}

int OverviewRailModel::ProxyRowForYPixel(int y, int railHeight) const noexcept
{
    if (mProxyRowCount <= 0)
    {
        return -1;
    }
    if (railHeight <= 0)
    {
        return 0;
    }
    // Clamp Y into the rail's usable range.
    const int clampedY = std::clamp(y, 0, railHeight - 1);
    // Linear map: bucket = (y * bucketCount) / railHeight, then
    // return the first proxy row in that bucket (matches paint
    // math so click-and-scroll lands on the row the user sees).
    if (mBuckets.empty())
    {
        // No bucket vector yet — approximate directly with row
        // math so clicks work even before the first `SetBucketCount`.
        const long long row = (static_cast<long long>(clampedY) * static_cast<long long>(mProxyRowCount)) /
                              static_cast<long long>(railHeight);
        return static_cast<int>(std::clamp<long long>(row, 0, mProxyRowCount - 1));
    }
    const long long bucket = (static_cast<long long>(clampedY) * static_cast<long long>(mBuckets.size())) /
                             static_cast<long long>(railHeight);
    const int clampedBucket = static_cast<int>(std::clamp<long long>(bucket, 0, static_cast<long long>(mBuckets.size()) - 1));
    const int firstRow = FirstProxyRowInBucket(static_cast<std::size_t>(clampedBucket));
    return firstRow >= 0 ? firstRow : 0;
}

void OverviewRailModel::OnRowsInserted(const QModelIndex &parent, int first, int last)
{
    (void)parent;
    (void)first;
    (void)last;
    Rebuild();
}

void OverviewRailModel::OnRowsRemoved(const QModelIndex &parent, int first, int last)
{
    (void)parent;
    (void)first;
    (void)last;
    Rebuild();
}

void OverviewRailModel::OnModelReset()
{
    mLevelColumnIndex = ComputeLevelColumnIndex();
    Rebuild();
}

void OverviewRailModel::OnLayoutChanged()
{
    Rebuild();
}

void OverviewRailModel::OnColumnsChanged()
{
    const int fresh = ComputeLevelColumnIndex();
    if (fresh == mLevelColumnIndex)
    {
        return;
    }
    mLevelColumnIndex = fresh;
    Rebuild();
}

void OverviewRailModel::OnEnumColumnsChanged()
{
    const int fresh = ComputeLevelColumnIndex();
    if (fresh == mLevelColumnIndex)
    {
        // A grow / demote of an unrelated column: skip the
        // rebuild to keep append hot paths cheap.
        return;
    }
    mLevelColumnIndex = fresh;
    Rebuild();
}

void OverviewRailModel::OnAnchorChanged(const AnchorManager::Key & /*key*/)
{
    // Anchor edits are rare; a full rebuild costs the same
    // O(rowCount) as folding a single anchor into its bucket, and
    // keeps the running popcount trivially in sync.
    Rebuild();
}

void OverviewRailModel::OnAnchorsReset()
{
    Rebuild();
}

void OverviewRailModel::RebuildInternal()
{
    const std::size_t previousBitsSet = mAnchorBucketBitsSet;

    // Zero everything except the vector capacity.
    for (auto &bucket : mBuckets)
    {
        bucket.levels.counts.fill(0);
        bucket.matchCount = 0;
        bucket.anchorSlots.reset();
    }
    mAnchorBucketBitsSet = 0;

    if (mProxyModel == nullptr)
    {
        mProxyRowCount = 0;
        EmitAnchorChangeIfDifferent(previousBitsSet);
        return;
    }
    mProxyRowCount = mProxyModel->rowCount();
    if (mBuckets.empty() || mProxyRowCount <= 0)
    {
        EmitAnchorChangeIfDifferent(previousBitsSet);
        return;
    }

    const bool trackAnchors = mAnchors != nullptr && !mAnchors->Empty();
    const std::size_t nBuckets = mBuckets.size();

    // One linear walk over the proxy: bucket per row, mapped
    // to source once for the level lookup and (optionally) the
    // anchor slot lookup. On a 1M-row session with 500 buckets
    // this is a handful of ms — dominated by the source-mapping
    // walk, not by the bucket increment.
    for (int proxyRow = 0; proxyRow < mProxyRowCount; ++proxyRow)
    {
        const std::size_t bucketIdx =
            (static_cast<std::size_t>(proxyRow) * nBuckets) / static_cast<std::size_t>(mProxyRowCount);
        const std::size_t bounded = std::min(bucketIdx, nBuckets - 1);
        auto &bucket = mBuckets[bounded];

        const int sourceRow = ProxyToSourceRow(proxyRow);
        if (sourceRow < 0)
        {
            // Proxy chain hides this row on the way down. Count
            // it as `Unknown` so the rail still shows *something*
            // where the row is — better than a gap that hides
            // the fact that a row is present.
            ++bucket.levels.counts[static_cast<std::size_t>(loglib::LogLevel::Unknown)];
            continue;
        }
        const loglib::LogLevel level = LevelForSourceRow(sourceRow);
        ++bucket.levels.counts[static_cast<std::size_t>(level)];

        if (trackAnchors)
        {
            const auto slot = mSourceModel->AnchorSlotForRow(sourceRow);
            if (slot.has_value())
            {
                auto &mask = bucket.anchorSlots;
                if (!mask.test(*slot))
                {
                    mask.set(*slot);
                    ++mAnchorBucketBitsSet;
                }
            }
        }
    }

    // Fold match rows in. `mFindMatchCache->sortedRows` is a
    // sorted, deduplicated proxy-row list — bucket assignment
    // matches the row pass above.
    for (const int proxyRow : mMatchProxyRows)
    {
        if (proxyRow < 0 || proxyRow >= mProxyRowCount)
        {
            continue;
        }
        const std::size_t bucketIdx =
            (static_cast<std::size_t>(proxyRow) * nBuckets) / static_cast<std::size_t>(mProxyRowCount);
        const std::size_t bounded = std::min(bucketIdx, nBuckets - 1);
        ++mBuckets[bounded].matchCount;
    }

    EmitAnchorChangeIfDifferent(previousBitsSet);
}

void OverviewRailModel::ScheduleEmit()
{
    if (!mEmitTimer->isActive())
    {
        mEmitTimer->start();
    }
}

int OverviewRailModel::BucketForProxyRow(int proxyRow) const noexcept
{
    if (mProxyRowCount <= 0 || mBuckets.empty() || proxyRow < 0 || proxyRow >= mProxyRowCount)
    {
        return -1;
    }
    const std::size_t bucketIdx =
        (static_cast<std::size_t>(proxyRow) * mBuckets.size()) / static_cast<std::size_t>(mProxyRowCount);
    return static_cast<int>(std::min(bucketIdx, mBuckets.size() - 1));
}

int OverviewRailModel::ProxyToSourceRow(int proxyRow) const noexcept
{
    if (mProxyModel == nullptr || mSourceModel == nullptr)
    {
        return -1;
    }
    QModelIndex idx = mProxyModel->index(proxyRow, 0);
    QAbstractItemModel *current = mProxyModel;
    // Walk through however many `QAbstractProxyModel` layers the
    // chain has (in production it's LogFilterModel ->
    // RowOrderProxyModel -> LogModel).
    while (auto *proxy = qobject_cast<QAbstractProxyModel *>(current))
    {
        idx = proxy->mapToSource(idx);
        current = proxy->sourceModel();
        if (current == nullptr)
        {
            return -1;
        }
    }
    if (current != mSourceModel || !idx.isValid())
    {
        return -1;
    }
    return idx.row();
}

loglib::LogLevel OverviewRailModel::LevelForSourceRow(int sourceRow) const noexcept
{
    if (mSourceModel == nullptr || mLevelColumnIndex < 0)
    {
        return loglib::LogLevel::Unknown;
    }
    // Prefer LogTable::GetLevelForRow directly over
    // `LogModel::LevelForRow`: the private helper caches
    // `mFirstLevelColumnCache` inside LogModel, which self-repairs
    // during `AppendBatch`. Reading through our own cached index
    // sidesteps the race the histogram model documents.
    const auto level = mSourceModel->Table().GetLevelForRow(
        static_cast<std::size_t>(sourceRow), static_cast<std::size_t>(mLevelColumnIndex)
    );
    return level.value_or(loglib::LogLevel::Unknown);
}

int OverviewRailModel::ComputeLevelColumnIndex() const noexcept
{
    if (mSourceModel == nullptr)
    {
        return -1;
    }
    const auto &config = mSourceModel->Configuration();
    for (std::size_t i = 0; i < config.columns.size(); ++i)
    {
        if (config.columns[i].type == loglib::LogConfiguration::Type::Level)
        {
            return static_cast<int>(i);
        }
    }
    return -1;
}

void OverviewRailModel::EmitAnchorChangeIfDifferent(std::size_t previousBitsSet)
{
    if (previousBitsSet != mAnchorBucketBitsSet)
    {
        emit anchorBucketsChanged();
    }
}
