#pragma once

#include "anchor_manager.hpp"
#include "scoped_connections.hpp"

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

/**
 * @brief Maintains a histogram bucket index for a log model.
 *
 * Source changes update the index, while repaint notifications are
 * coalesced through a short timer. Consumers read `Index()` directly.
 */
class HistogramModel : public QObject
{
    Q_OBJECT

public:
    /** @brief Bit mask of anchor palette slots represented in one bucket. */
    using AnchorSlotMask = std::bitset<loglib::ANCHOR_PALETTE_SIZE>;

    /**
     * @brief Constructs a histogram model for borrowed sources.
     * @param logModel Log model to observe, or `nullptr`.
     * @param anchors Anchor manager to observe, or `nullptr`.
     * @param parent Parent object.
     */
    HistogramModel(LogModel *logModel, AnchorManager *anchors, QObject *parent = nullptr);

    /**
     * @brief Returns the current bucket index.
     * @return The maintained index, which may be empty.
     */
    [[nodiscard]] const loglib::HistogramBucketIndex &Index() const noexcept
    {
        return mIndex;
    }

    /**
     * @brief Pins the bucket size and rebuilds when it changes.
     * @param size Bucket size to pin.
     */
    void SetBucketSize(loglib::HistogramBucketSize size);

    /** @brief Applies an automatically selected bucket size unless pinned. */
    void ApplyAutoBucketSize();

    /** @brief Clears the bucket-size pin and applies automatic sizing. */
    void ResetBucketSizeToAuto();

    /** @brief Rebuilds the bucket index from all current log rows. */
    void Rebuild();

    /**
     * @brief Replaces the borrowed log and anchor sources.
     *
     * Pending notification is canceled, old subscriptions are removed,
     * bucket state is reset, and new subscriptions are installed. A
     * deferred bind still refreshes column availability and accepts
     * incremental appends; `PumpDeferredBind()` completes the full pass.
     *
     * @param logModel Log model to observe, or `nullptr`.
     * @param anchors Anchor manager to observe, or `nullptr`.
     * @param deferRebuild Whether to postpone the full rebuild and auto-size pass.
     *
     */
    void BindSources(LogModel *logModel, AnchorManager *anchors, bool deferRebuild = false);

    /** @brief Completes a pending deferred rebuild and auto-size pass. */
    void PumpDeferredBind();

    /**
     * @brief Reports whether the bucket size was explicitly pinned.
     * @return True after `SetBucketSize()` and before an automatic reset.
     */
    [[nodiscard]] bool IsBucketSizePinned() const noexcept
    {
        return mBucketSizePinned;
    }

    /** @brief Cancels a pending coalesced `bucketsChanged()` emission. */
    void CancelPendingEmit() noexcept;

    /**
     * @brief Returns the cached time-column index.
     * @return First time column, or -1 when unavailable.
     */
    [[nodiscard]] int TimeColumnIndex() const noexcept
    {
        return mTimeColumnIndex;
    }

    /**
     * @brief Reports whether the current model has a time column.
     * @return True when `TimeColumnIndex()` is non-negative.
     */
    [[nodiscard]] bool HasTimeColumn() const noexcept
    {
        return mTimeColumnIndex >= 0;
    }

    /**
     * @brief Returns the cached level-column index.
     * @return First level column, or -1 when unavailable.
     */
    [[nodiscard]] int LevelColumnIndex() const noexcept
    {
        return mLevelColumnIndex;
    }

    /**
     * @brief Finds the first source row in a bucket.
     * @param bucketIndex Raw bucket index.
     * @return First matching source row, or -1 when unavailable.
     */
    [[nodiscard]] int FirstRowInBucket(std::size_t bucketIndex) const;

    /** @brief Minimum and maximum observed timestamps. */
    struct TimeRange
    {
        loglib::TimeStamp min;
        loglib::TimeStamp max;
    };
    /**
     * @brief Returns the observed timestamp range.
     * @return The range, or `std::nullopt` when no timestamp is available.
     */
    [[nodiscard]] std::optional<TimeRange> ObservedRange() const;

    /**
     * @brief Returns anchor palette masks parallel to the bucket vector.
     * @return A view of per-bucket anchor masks, empty without anchor tracking.
     */
    [[nodiscard]] std::span<const AnchorSlotMask> AnchorSlotsPerBucket() const noexcept
    {
        return {mAnchorSlotPerBucket.data(), mAnchorSlotPerBucket.size()};
    }

    /**
     * @brief Reports whether any bucket contains an anchor.
     * @return True when at least one anchor bit is set.
     */
    [[nodiscard]] bool HasAnchorTicks() const noexcept
    {
        return mAnchorBucketBitsSet > 0;
    }

    /**
     * @brief Finds the earliest anchored row in a half-open bucket range.
     * @param bucketBegin First raw bucket index.
     * @param bucketEnd One-past-last raw bucket index.
     * @return Earliest anchored source row, or -1 when unavailable.
     */
    [[nodiscard]] int FirstAnchoredRowInBucketRange(std::size_t bucketBegin, std::size_t bucketEnd) const;

signals:
    /** @brief Emitted after repaint-worthy bucket changes are coalesced. */
    void bucketsChanged();

    /**
     * @brief Emitted when time-column availability changes.
     * @param hasTimeColumn Whether a time column is now available.
     */
    void timeColumnAvailabilityChanged(bool hasTimeColumn);

    /** @brief Emitted immediately when per-bucket anchor masks change. */
    void anchorBucketsChanged();

private:
    void OnRowsInserted(const QModelIndex &parent, int first, int last);
    void OnRowsRemoved(const QModelIndex &parent, int first, int last);
    void OnModelReset();

    /** @brief Refreshes cached enum-column indices and rebuilds when needed. */
    void OnEnumColumnsChanged();

    /** @brief Refreshes cached indices after source columns move. */
    void OnColumnsMoved();

    /**
     * @brief Refreshes anchor masks affected by one key.
     * @param key Anchor key that changed.
     */
    void OnAnchorChanged(const AnchorManager::Key &key);

    /** @brief Rebuilds anchor masks after a bulk anchor change. */
    void OnAnchorsReset();

    /**
     * @brief Adds an inclusive source-row range to the index.
     * @param first First source row.
     * @param last Last source row.
     */
    void AppendRange(int first, int last);

    [[nodiscard]] int ComputeTimeColumnIndex() const;

    [[nodiscard]] int ComputeLevelColumnIndex() const;

    /**
     * @brief Reads a timestamp from a source row.
     * @param row Source row.
     * @return Timestamp, or `std::nullopt` when unavailable.
     */
    [[nodiscard]] std::optional<loglib::TimeStamp> TimeStampForRow(int row) const;

    /**
     * @brief Reads the canonical level from a source row.
     * @param row Source row.
     * @return Row level, or `LogLevel::Unknown` when unavailable.
     */
    [[nodiscard]] loglib::LogLevel LevelForRow(int row) const;

    /** @brief Starts the coalesced bucket-change timer if idle. */
    void ScheduleEmit();

    /** @brief Invalidates the lazy first-row-per-bucket cache. */
    void InvalidateFirstRowCache() const noexcept;

    /**
     * @brief Builds the first-source-row cache from the current model.
     * @return One source row per bucket, using -1 for empty buckets.
     */
    [[nodiscard]] std::vector<int> BuildFirstRowCache() const;

    /** @brief Rebuilds all per-bucket anchor masks. */
    void RebuildAnchorBuckets();

    /**
     * @brief Adds one anchor key's palette slot to its bucket mask.
     * @param key Anchor key to resolve.
     * @return Affected bucket index, or `std::nullopt` when unavailable.
     */
    [[nodiscard]] std::optional<std::size_t> UpdateAnchorSlotForKey(const AnchorManager::Key &key);

    /** @brief Resizes anchor masks to match the current bucket vector. */
    void SyncAnchorBucketVectorSize();

    QPointer<LogModel> mLogModel;
    QPointer<AnchorManager> mAnchors;
    loglib::HistogramBucketIndex mIndex;
    int mTimeColumnIndex = -1;
    int mLevelColumnIndex = -1;
    QTimer *mEmitTimer = nullptr;

    // Suppresses automatic bucket-size selection after a user choice.
    bool mBucketSizePinned = false;

    // True while a deferred source rebuild remains outstanding.
    bool mDeferredBindPending = false;

    // Lazy first-source-row cache. `nullopt` means stale.
    mutable std::optional<std::vector<int>> mFirstRowPerBucketCache;

    // Per-bucket anchor slot masks, parallel to `mIndex.Buckets()`.
    std::vector<AnchorSlotMask> mAnchorSlotPerBucket;

    // Running anchor-slot count keeps `HasAnchorTicks` O(1).
    std::size_t mAnchorBucketBitsSet = 0;

    // Subscriptions owned by the current source binding.
    ScopedConnections mSourceConnections;

    /** @brief Installs subscriptions for the current source aliases. */
    void InstallSourceSubscriptions();
};
