#include "overview_rail_model.hpp"

#include "log_model.hpp"

#include <loglib/log_configuration.hpp>
#include <loglib/log_level.hpp>
#include <loglib/log_table.hpp>

#include <QAbstractItemModel>
#include <QAbstractProxyModel>
#include <QDebug>
#include <QModelIndex>
#include <QPointer>
#include <QTimer>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace
{

/// Coalesce cadence for the full rebuild + `bucketsChanged`
/// emit. Matches the histogram and live-tail batch cadence, so a
/// per-row `rowsInserted` volley collapses to one O(rowCount)
/// walk instead of one per row.
constexpr int REBUILD_COALESCE_MS = 50;

/// Severity ranking for the dominant-level tie-break; higher =
/// more severe. Indexed by `static_cast<size_t>(LogLevel)`.
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
    mRebuildTimer = new QTimer(this);
    mRebuildTimer->setSingleShot(true);
    mRebuildTimer->setInterval(REBUILD_COALESCE_MS);
    connect(mRebuildTimer, &QTimer::timeout, this, &OverviewRailModel::OnRebuildTimeout);

    if (mProxyModel != nullptr)
    {
        connect(mProxyModel, &QAbstractItemModel::rowsInserted, this, &OverviewRailModel::OnRowsInserted);
        connect(mProxyModel, &QAbstractItemModel::rowsRemoved, this, &OverviewRailModel::OnRowsRemoved);
        connect(mProxyModel, &QAbstractItemModel::modelReset, this, &OverviewRailModel::OnModelReset);
        // A filter/sort change lands as `layoutChanged` on
        // `LogFilterModel`; rebuild to pick up the new row set.
        connect(mProxyModel, &QAbstractItemModel::layoutChanged, this, &OverviewRailModel::OnLayoutChanged);
        // Column shape changes may move the level column even
        // when no rows insert.
        connect(mProxyModel, &QAbstractItemModel::columnsMoved, this, &OverviewRailModel::OnColumnsChanged);
        connect(mProxyModel, &QAbstractItemModel::columnsInserted, this, &OverviewRailModel::OnColumnsChanged);
        connect(mProxyModel, &QAbstractItemModel::columnsRemoved, this, &OverviewRailModel::OnColumnsChanged);
    }

    if (mSourceModel != nullptr)
    {
        // Level column can promote / demote mid-stream; refresh
        // the cache and rebuild so pre-promotion `Unknown` rows
        // get re-attributed. Skip dictionary `Grew` events — they
        // can't move the level column, and every batch of a wide
        // log would otherwise pay an `O(nColumns)` scan.
        connect(
            mSourceModel,
            &LogModel::enumColumnsChanged,
            this,
            [this](EnumColumnsChangeReason reason, int)
            {
                if (reason == EnumColumnsChangeReason::Grew)
                {
                    return;
                }
                OnEnumColumnsChanged();
            }
        );
    }

    if (mAnchors != nullptr)
    {
        connect(mAnchors, &AnchorManager::anchorChanged, this, &OverviewRailModel::OnAnchorChanged);
        connect(mAnchors, &AnchorManager::anchorsReset, this, &OverviewRailModel::OnAnchorsReset);
    }

    // Prime the cached column index and bucket vector.
    mLevelColumnIndex = ComputeLevelColumnIndex();
    RebuildProxyChainCache();
}

void OverviewRailModel::SetBucketCount(std::size_t nBuckets)
{
    if (mBuckets.size() == nBuckets)
    {
        return;
    }
    // Track whether the previous state had anything the widget
    // could have painted. Dropping to zero from an already-empty
    // state means no repaint is needed downstream.
    const bool preStateHadContent =
        !mBuckets.empty() && (mProxyRowCount > 0 || mBucketedMatchCount > 0 || mAnchorBucketBitsSet > 0);

    mBuckets.assign(nBuckets, Bucket{});
    // Supersede any in-flight coalesced rebuild; the sync one
    // below is fresher.
    if (mRebuildTimer != nullptr && mRebuildTimer->isActive())
    {
        mRebuildTimer->stop();
    }
    // Synchronous so the widget's next paint sees fresh geometry.
    // Zero-bucket state is the fast-path: `RebuildInternal`
    // short-circuits on `mBuckets.empty()` and subsequent proxy
    // signals stay cheap while the rail is hidden.
    RebuildInternal();
    if (nBuckets != 0 || preStateHadContent)
    {
        emit bucketsChanged();
    }
}

void OverviewRailModel::SetMatchProxyRows(std::vector<int> proxyRows)
{
    mMatchProxyRows = std::move(proxyRows);
    // Row-list path owns match state; drop the durable bucket
    // counts so a later rebuild can't re-apply a stale fold.
    mMatchBucketCounts.clear();
    mMatchBucketTotal = 0;
    // Sort + unique so `FoldMatchTicksIntoBuckets` (which
    // increments per entry) doesn't double-count a duplicated row.
    std::sort(mMatchProxyRows.begin(), mMatchProxyRows.end());
    mMatchProxyRows.erase(std::unique(mMatchProxyRows.begin(), mMatchProxyRows.end()), mMatchProxyRows.end());
    // Refresh `mProxyRowCount` from the live proxy — it can lag
    // by one coalesce window under live-tail inserts, and folding
    // with a stale (smaller) denominator would drop tail-row hits
    // or bunch them into the last bucket.
    if (mProxyModel != nullptr)
    {
        mProxyRowCount = mProxyModel->rowCount();
    }
    RefreshMatchTicks();
    // Only `matchesChanged`: level counts / anchor bits are
    // unchanged, so an extra `bucketsChanged` would just queue a
    // duplicate paint request.
    emit matchesChanged();
}

void OverviewRailModel::SetMatchBucketCounts(std::vector<uint32_t> perBucketCounts, uint32_t totalMatches)
{
    // Size mismatch → rail resized between the caller computing
    // counts and this call landing. Drop the update; the resize
    // triggers its own rebuild and the next find debounce
    // recomputes. Validate BEFORE mutating so a dropped update
    // leaves the model untouched.
    if (perBucketCounts.size() != mBuckets.size())
    {
        return;
    }
    // Same reasoning as in `SetMatchProxyRows`: the caller scanned
    // against the live proxy, and subsequent rail math needs the
    // matching denominator.
    if (mProxyModel != nullptr)
    {
        mProxyRowCount = mProxyModel->rowCount();
    }
    // Clear the raw-rows path so a rebuild doesn't double-fold on
    // top of these pre-bucketed values. Retain the counts as
    // durable state for `RebuildInternal` to re-apply.
    mMatchProxyRows.clear();
    mMatchBucketCounts = std::move(perBucketCounts);
    mMatchBucketTotal = totalMatches;
    ApplyStoredMatchBucketCounts();
    emit matchesChanged();
}

void OverviewRailModel::Rebuild()
{
    ScheduleRebuild();
}

void OverviewRailModel::RebuildNow()
{
    if (mRebuildTimer != nullptr && mRebuildTimer->isActive())
    {
        mRebuildTimer->stop();
    }
    RebuildInternal();
    emit bucketsChanged();
}

loglib::LogLevel OverviewRailModel::DominantLevel(std::size_t bucket) const noexcept
{
    if (bucket >= mBuckets.size())
    {
        return loglib::LogLevel::Unknown;
    }
    const auto &counts = mBuckets[bucket].levels.counts;
    // Majority-count level, tie-broken by severity. Retained for
    // callers that want a single colour per bucket; the widget's
    // paint pass instead stacks per-level segments so rare
    // high-severity anomalies stay visible.
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
    // Direct pixel-to-row map (`row = y * rowCount / railHeight`)
    // gives sub-bucket precision, so scrubbing a bucket that spans
    // many rows still moves smoothly. Bucket mapping is paint-only.
    const int clampedY = std::clamp(y, 0, railHeight - 1);
    const long long row = (static_cast<long long>(clampedY) * static_cast<long long>(mProxyRowCount)) /
                          static_cast<long long>(railHeight);
    return static_cast<int>(std::clamp<long long>(row, 0, mProxyRowCount - 1));
}

void OverviewRailModel::OnRowsInserted(const QModelIndex & /*parent*/, int /*first*/, int /*last*/)
{
    // `first` / `last` are ignored today — a burst of per-row
    // inserts coalesces via `Rebuild()`. Signature is kept intact
    // to leave room for a future tail-append fast path.
    Rebuild();
}

void OverviewRailModel::OnRowsRemoved(const QModelIndex & /*parent*/, int /*first*/, int /*last*/)
{
    Rebuild();
}

void OverviewRailModel::OnModelReset()
{
    mLevelColumnIndex = ComputeLevelColumnIndex();
    // A reset may swap the terminal source or drop / add a proxy
    // layer under us; refresh the chain cache before rebuilding.
    RebuildProxyChainCache();
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
        // Unrelated column changed; skip the rebuild.
        return;
    }
    mLevelColumnIndex = fresh;
    Rebuild();
}

void OverviewRailModel::OnAnchorChanged(const AnchorManager::Key & /*key*/)
{
    // Anchor edits are user-driven and rare, so rebuild in bulk
    // rather than fold per key. A targeted fold would need a
    // source-row → proxy-row inverse mapping we don't maintain;
    // the worst case is the same O(rowCount).
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
    mBucketedMatchCount = 0;

    if (mProxyModel == nullptr || mSourceModel == nullptr)
    {
        // `QPointer` may have zeroed under us; the pre-zeroed
        // buckets already leave the rail in a "no data" state.
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

    // Single linear walk over the proxy: one source-row mapping
    // per row for the level (and optional anchor) lookup. Cost is
    // dominated by the source mapping, not the bucket increment.
    LogModel *const sourceModel = mSourceModel;
    for (int proxyRow = 0; proxyRow < mProxyRowCount; ++proxyRow)
    {
        const std::size_t bucketIdx =
            (static_cast<std::size_t>(proxyRow) * nBuckets) / static_cast<std::size_t>(mProxyRowCount);
        const std::size_t bounded = std::min(bucketIdx, nBuckets - 1);
        auto &bucket = mBuckets[bounded];

        const int sourceRow = ProxyToSourceRow(proxyRow);
        if (sourceRow < 0)
        {
            // Outer proxy exposes a row an inner proxy hides —
            // breaks the visibility invariant. In production
            // (`LogFilterModel` outermost) this is dead code.
            Q_ASSERT_X(false, "OverviewRailModel::RebuildInternal", "proxy row has no reachable source row");
            continue;
        }
        const loglib::LogLevel level = LevelForSourceRow(sourceRow);
        ++bucket.levels.counts[static_cast<std::size_t>(level)];

        if (trackAnchors)
        {
            const auto slot = sourceModel->AnchorSlotForRow(sourceRow);
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

    // Fold current match rows in as part of the same walk. When
    // the bucketed API owns match state (row list empty, durable
    // counts retained), re-apply those counts so anchor edits /
    // hide→show / same-H resize don't wipe find highlights.
    FoldMatchTicksIntoBuckets();
    if (mMatchProxyRows.empty())
    {
        ApplyStoredMatchBucketCounts();
    }

    EmitAnchorChangeIfDifferent(previousBitsSet);
}

void OverviewRailModel::RefreshMatchTicks()
{
    mBucketedMatchCount = 0;
    for (auto &bucket : mBuckets)
    {
        bucket.matchCount = 0;
    }
    if (mBuckets.empty() || mProxyRowCount <= 0)
    {
        return;
    }
    FoldMatchTicksIntoBuckets();
}

void OverviewRailModel::ScheduleRebuild()
{
    // Rail hidden → skip the timer. A rebuild would short-circuit
    // and emit a redundant `bucketsChanged` that fans out to
    // queued widget updates (each a no-op, but not free). The
    // next `SetBucketCount(H)` on re-show runs its own rebuild.
    if (mBuckets.empty())
    {
        return;
    }
    if (mRebuildTimer != nullptr && !mRebuildTimer->isActive())
    {
        mRebuildTimer->start();
    }
}

void OverviewRailModel::OnRebuildTimeout()
{
    RebuildInternal();
    emit bucketsChanged();
}

void OverviewRailModel::FoldMatchTicksIntoBuckets()
{
    if (mBuckets.empty() || mProxyRowCount <= 0)
    {
        return;
    }
    const std::size_t nBuckets = mBuckets.size();
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
        ++mBucketedMatchCount;
    }
}

void OverviewRailModel::ApplyStoredMatchBucketCounts()
{
    if (mMatchBucketCounts.size() != mBuckets.size() || mBuckets.empty())
    {
        return;
    }
    mBucketedMatchCount = mMatchBucketTotal;
    for (std::size_t i = 0; i < mBuckets.size(); ++i)
    {
        mBuckets[i].matchCount = mMatchBucketCounts[i];
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
    if (!mProxyChainTerminatesAtSource)
    {
        return -1;
    }
    QModelIndex idx = mProxyModel->index(proxyRow, 0);
    for (const QPointer<QAbstractProxyModel> &proxy : mProxyChain)
    {
        // A `QPointer` slot may zero if the referenced proxy was
        // destroyed without a `modelReset`; return `-1` rather
        // than crash.
        if (proxy.isNull())
        {
            return -1;
        }
        idx = proxy->mapToSource(idx);
    }
    if (!idx.isValid())
    {
        return -1;
    }
    // Chain drift: a mid-chain proxy silently swapped its source
    // and we'd fold rows from the wrong table. Assert in debug,
    // one-shot warn in release, and refuse the row.
    Q_ASSERT_X(
        idx.model() == mSourceModel, "OverviewRailModel::ProxyToSourceRow",
        "proxy chain no longer terminates at the cached source model"
    );
    if (idx.model() != mSourceModel)
    {
        // Flag prevents log spam at scan cadence (~1 M rows).
        if (!mProxyChainDriftWarned)
        {
            mProxyChainDriftWarned = true;
            qWarning() << "OverviewRailModel::ProxyToSourceRow: proxy chain no longer terminates at the "
                          "cached source model. Bucket counts may misattribute rows until the next "
                          "modelReset refreshes the chain cache.";
        }
        return -1;
    }
    return idx.row();
}

void OverviewRailModel::RebuildProxyChainCache()
{
    mProxyChain.clear();
    mProxyChainTerminatesAtSource = false;
    // Fresh cache re-arms the one-shot drift warning so a
    // subsequent drift after e.g. a dictionary rebuild still logs.
    mProxyChainDriftWarned = false;
    if (mProxyModel == nullptr || mSourceModel == nullptr)
    {
        return;
    }
    // Trivial case: the "proxy" already IS the source model.
    if (mProxyModel == mSourceModel)
    {
        mProxyChainTerminatesAtSource = true;
        return;
    }
    QAbstractItemModel *current = mProxyModel;
    while (auto *proxy = qobject_cast<QAbstractProxyModel *>(current))
    {
        mProxyChain.push_back(QPointer<QAbstractProxyModel>(proxy));
        current = proxy->sourceModel();
        if (current == nullptr)
        {
            mProxyChain.clear();
            return;
        }
    }
    mProxyChainTerminatesAtSource = (current == mSourceModel);
    if (!mProxyChainTerminatesAtSource)
    {
        mProxyChain.clear();
    }
}

loglib::LogLevel OverviewRailModel::LevelForSourceRow(int sourceRow) const noexcept
{
    if (mSourceModel == nullptr || mLevelColumnIndex < 0)
    {
        return loglib::LogLevel::Unknown;
    }
    // Read `LogTable::GetLevelForRow` directly instead of going
    // through `LogModel::LevelForRow`; our own cached index
    // sidesteps the `AppendBatch` cache race the histogram
    // model documents.
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
