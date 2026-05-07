#include "loglib/log_table.hpp"

#include "loglib/file_line_source.hpp"
#include "loglib/internal/compact_log_value.hpp"
#include "loglib/log_processing.hpp"

#include <date/date.h>
#include <date/tz.h>
#include <fmt/format.h>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace loglib
{

namespace
{

/// `Type::any` columns become candidates for enum auto-detection only
/// after this many rows have been observed. Below the threshold the
/// data is too sparse for the heuristic to be meaningful.
constexpr size_t ENUM_PROMOTION_MIN_ROWS = 256;

/// Skip the enum pass for columns of these other types.
bool IsEnumPassEligible(LogConfiguration::Type type) noexcept
{
    return type == LogConfiguration::Type::any || type == LogConfiguration::Type::enumeration;
}

} // namespace

bool LogTable::EnumCandidateTracker::Observe(std::string_view bytes)
{
    ++rowsObserved;
    if (killed)
    {
        return false;
    }
    // O(1) "have we seen this exact byte sequence before?" via the
    // transparent-hash mirror of `values`. Pre-fix this was an O(N)
    // linear scan of `values`; with `cap = DEFAULT_ENUM_VALUE_CAP =
    // 64` and the (cap+1)th-value kill rule, that meant up to 64
    // string compares per observed row pre-promotion. The compares
    // are branch-predictor-friendly ASCII compares, but the hash
    // path is strictly cheaper at any cap >> 8 and bounded by `cap`
    // memory.
    if (seen.contains(bytes))
    {
        return false;
    }
    if (size >= cap)
    {
        // Cap+1 distinct values: the column is not (and never will be) a
        // promotion candidate. Drop the buffer to free memory; we keep
        // observing only to maintain `rowsObserved` for parity with the
        // `killed == false` path, but we do not record the new value.
        // `LogTable` then folds the configured canonical key string
        // into `mEnumPermanentlyKilled` so the column is never
        // re-tracked.
        killed = true;
        values = {};
        seen = {};
        size = 0;
        return true;
    }
    values.emplace_back(bytes);
    seen.emplace(values.back());
    ++size;
    return true;
}

LogTable::LogTable(LogData data, LogConfigurationManager configuration)
    : mData(std::move(data)), mConfiguration(std::move(configuration))
{
    RewireSourceRegistries();
    RefreshSnapshotEnumKeys();
    RefreshColumnKeyIds();
    // Quiescence enum pass over freshly-loaded data: encodes any
    // pre-configured `Type::enumeration` columns and auto-detects
    // candidates among `Type::any` columns.
    std::optional<size_t> firstBackfilled;
    std::optional<size_t> lastBackfilled;
    RunEnumPassForAppendBatch(0U, firstBackfilled, lastBackfilled);
}

LogTable::LogTable(LogTable &&other) noexcept
    : mData(std::move(other.mData)),
      mConfiguration(std::move(other.mConfiguration)),
      mColumnKeyIds(std::move(other.mColumnKeyIds)),
      mStageBSnapshotTimeKeys(std::move(other.mStageBSnapshotTimeKeys)),
      mPostSnapshotTimeKeys(std::move(other.mPostSnapshotTimeKeys)),
      mEnumDictionaries(std::move(other.mEnumDictionaries)),
      mEnumValueCap(other.mEnumValueCap),
      mEnumTrackers(std::move(other.mEnumTrackers)),
      mEnumPermanentlyKilled(std::move(other.mEnumPermanentlyKilled)),
      mScratchResolvedKeyIds(std::move(other.mScratchResolvedKeyIds)),
      mLastBackfillRange(std::move(other.mLastBackfillRange))
{
    // Every `LineSource` in `mData.Sources()` cached a pointer to
    // `other.mEnumDictionaries`; rebind to *this so resolution lands
    // on the moved-into registry.
    RewireSourceRegistries();
}

LogTable &LogTable::operator=(LogTable &&other) noexcept
{
    if (this == &other)
    {
        return *this;
    }
    mData = std::move(other.mData);
    mConfiguration = std::move(other.mConfiguration);
    mColumnKeyIds = std::move(other.mColumnKeyIds);
    mStageBSnapshotTimeKeys = std::move(other.mStageBSnapshotTimeKeys);
    mPostSnapshotTimeKeys = std::move(other.mPostSnapshotTimeKeys);
    mEnumDictionaries = std::move(other.mEnumDictionaries);
    mEnumValueCap = other.mEnumValueCap;
    mEnumTrackers = std::move(other.mEnumTrackers);
    mEnumPermanentlyKilled = std::move(other.mEnumPermanentlyKilled);
    mScratchResolvedKeyIds = std::move(other.mScratchResolvedKeyIds);
    mLastBackfillRange = std::move(other.mLastBackfillRange);
    // Every `LineSource` in `mData.Sources()` cached a pointer to
    // `other.mEnumDictionaries`; rebind to *this so resolution lands
    // on the moved-into registry.
    RewireSourceRegistries();
    return *this;
}

void LogTable::Update(LogData &&data)
{
    const size_t oldLineCount = mData.Lines().size();
    mConfiguration.Update(data);
    if (!data.TimestampsAlreadyParsed())
    {
        ParseTimestamps(data, mConfiguration.Configuration());
    }
    mData.Merge(std::move(data));
    // The merge installs the merged-in source under the canonical
    // `LogData::mSources`; rebind every source's registry pointer.
    RewireSourceRegistries();
    RefreshColumnKeyIds();
    // Snapshot enum keys *after* the merged `LogData` rebuild so any
    // configured `Type::enumeration` column (whether pre-existing or
    // freshly grown by `mConfiguration.Update(data)`) gets a registry
    // entry; without this the encode pass below would silently treat
    // the column as uninstalled and skip it.
    RefreshSnapshotEnumKeys();
    // Quiescence enum pass: process the newly-merged slice.
    std::optional<size_t> firstBackfilled;
    std::optional<size_t> lastBackfilled;
    RunEnumPassForAppendBatch(oldLineCount, firstBackfilled, lastBackfilled);
}

void LogTable::Reset()
{
    // Wipe per-parse state; preserve `mConfiguration`.
    mData = LogData{};
    mStageBSnapshotTimeKeys.clear();
    mPostSnapshotTimeKeys.clear();
    mEnumDictionaries.Clear();
    mEnumTrackers.clear();
    mEnumPermanentlyKilled.clear();
    mScratchResolvedKeyIds.clear();
    mLastBackfillRange.reset();
    RefreshColumnKeyIds();
}

void LogTable::BeginStreaming(std::unique_ptr<LineSource> source)
{
    mLastBackfillRange.reset();

    if (source)
    {
        std::vector<LogLine> noLines;
        LogData fresh(std::move(source), std::move(noLines), KeyIndex{});
        // Both pipelines promote `Type::time` inline; later-discovered
        // time columns are back-filled in `AppendBatch`.
        fresh.MarkTimestampsParsed();
        mData = std::move(fresh);
    }
    else
    {
        mData = LogData{};
        mData.MarkTimestampsParsed();
    }

    mEnumDictionaries.Clear();
    mEnumTrackers.clear();
    mEnumPermanentlyKilled.clear();
    RewireSourceRegistries();

    // Order matters: snapshot inserts the time-column keys before the
    // KeyId resolution runs.
    RefreshSnapshotTimeKeys();
    RefreshSnapshotEnumKeys();
    RefreshColumnKeyIds();
}

void LogTable::AppendStreaming(std::unique_ptr<LineSource> source)
{
    assert(source != nullptr);
    if (source == nullptr)
    {
        return;
    }

    mLastBackfillRange.reset();

    // Splice in the new source. Lines and keys are preserved so prior
    // rows stay visible and KeyIds line up across files.
    source->SetEnumDictionaries(&mEnumDictionaries);
    mData.Sources().push_back(std::move(source));
    // Snapshot KeyIds and column-key cache stay valid: configuration
    // is gated for the session, and `mPostSnapshotTimeKeys` keeps
    // tracking late-discovered time keys for `AppendBatch` back-fill.
}

void LogTable::AppendBatch(StreamedBatch batch)
{
    mLastBackfillRange.reset();

    if (!batch.newKeys.empty())
    {
        mConfiguration.AppendKeys(batch.newKeys);
    }

    // Pre-append snapshot so slice back-fill targets only new rows;
    // first-observation columns still back-fill all rows.
    const size_t oldLineCount = mData.Lines().size();

    if (!batch.lines.empty() || !batch.localLineOffsets.empty())
    {
        mData.AppendBatch(std::move(batch.lines), batch.localLineOffsets);
    }

    if (!batch.newKeys.empty())
    {
        RefreshColumnKeyIdsForKeys(batch.newKeys);
    }

    // Back-fill post-snapshot time columns (Stage B handles snapshot
    // ones). First observation: back-fill all rows and record
    // `mLastBackfillRange` so `dataChanged` reaches existing rows.
    // Known post-snapshot column: back-fill only the appended slice.
    const auto &columns = mConfiguration.Configuration().columns;
    std::optional<size_t> firstBackfilled;
    std::optional<size_t> lastBackfilled;
    for (size_t columnIndex = 0; columnIndex < columns.size(); ++columnIndex)
    {
        const auto &column = columns[columnIndex];
        if (column.type != LogConfiguration::Type::time)
        {
            continue;
        }

        bool stageBHandled = false;
        bool firstObservation = false;
        bool needsSliceBackfill = false;
        std::vector<KeyId> columnKeyIds;
        columnKeyIds.reserve(column.keys.size());
        for (const std::string &key : column.keys)
        {
            const KeyId id = mData.Keys().Find(key);
            columnKeyIds.push_back(id);
            if (id == INVALID_KEY_ID)
            {
                continue;
            }
            if (mStageBSnapshotTimeKeys.contains(id))
            {
                stageBHandled = true;
                continue;
            }
            if (mPostSnapshotTimeKeys.contains(id))
            {
                needsSliceBackfill = true;
            }
            else
            {
                firstObservation = true;
            }
        }

        if (stageBHandled)
        {
            continue;
        }

        // Streaming has no consumer for per-line errors.
        if (firstObservation)
        {
            BackfillTimestampColumn(column, std::span<LogLine>(mData.Lines()), BackfillErrors::Discard);
            for (const KeyId id : columnKeyIds)
            {
                if (id != INVALID_KEY_ID)
                {
                    mPostSnapshotTimeKeys.insert(id);
                }
            }
            if (!firstBackfilled.has_value())
            {
                firstBackfilled = columnIndex;
            }
            lastBackfilled = columnIndex;
        }
        else if (needsSliceBackfill)
        {
            if (oldLineCount < mData.Lines().size())
            {
                const std::span<LogLine> slice(
                    mData.Lines().data() + oldLineCount, mData.Lines().size() - oldLineCount
                );
                BackfillTimestampColumn(column, slice, BackfillErrors::Discard);
            }
        }
    }

    // Enum pass: tracker update, demote-on-overflow, encode-if-fits,
    // and quiescence-driven auto-promotion. Mirrors the time-backfill
    // loop's `firstBackfilled` / `lastBackfilled` bookkeeping.
    RunEnumPassForAppendBatch(oldLineCount, firstBackfilled, lastBackfilled);

    if (firstBackfilled.has_value())
    {
        mLastBackfillRange = std::make_pair(*firstBackfilled, *lastBackfilled);
    }
}

LogTable::AppendBatchPreview LogTable::PreviewAppend(const StreamedBatch &batch) const
{
    AppendBatchPreview preview;
    preview.newRowCount = mData.Lines().size() + batch.lines.size();
    preview.newColumnCount =
        mConfiguration.Configuration().columns.size() + mConfiguration.CountAppendableKeys(batch.newKeys);
    return preview;
}

const std::optional<std::pair<size_t, size_t>> &LogTable::LastBackfillRange() const noexcept
{
    return mLastBackfillRange;
}

void LogTable::MoveColumn(size_t srcIndex, size_t destIndex)
{
    if (srcIndex == destIndex || srcIndex >= mColumnKeyIds.size() || destIndex >= mColumnKeyIds.size())
    {
        return;
    }
    mConfiguration.MoveColumn(srcIndex, destIndex);
    using Diff = std::vector<std::vector<KeyId>>::difference_type;
    auto begin = mColumnKeyIds.begin();
    if (srcIndex > destIndex)
    {
        std::rotate(
            std::next(begin, static_cast<Diff>(destIndex)),
            std::next(begin, static_cast<Diff>(srcIndex)),
            std::next(begin, static_cast<Diff>(srcIndex + 1))
        );
    }
    else
    {
        std::rotate(
            std::next(begin, static_cast<Diff>(srcIndex)),
            std::next(begin, static_cast<Diff>(srcIndex + 1)),
            std::next(begin, static_cast<Diff>(destIndex + 1))
        );
    }
}

void LogTable::ReserveLineOffsets(size_t count)
{
    if (count == 0)
    {
        return;
    }
    if (FileLineSource *fileSource = mData.FrontFileSource(); fileSource != nullptr)
    {
        fileSource->File().ReserveLineOffsets(count);
    }
}

std::string LogTable::GetHeader(size_t column) const
{
    return mConfiguration.Configuration().columns[column].header;
}

size_t LogTable::ColumnCount() const
{
    return mConfiguration.Configuration().columns.size();
}

LogValue LogTable::GetValue(size_t row, size_t column) const
{
    if (column >= mColumnKeyIds.size() || row >= mData.Lines().size())
    {
        return std::monostate{};
    }
    const auto &line = mData.Lines()[row];
    for (const KeyId id : mColumnKeyIds[column])
    {
        if (id == INVALID_KEY_ID)
        {
            continue;
        }
        LogValue value = line.GetValue(id);
        if (!std::holds_alternative<std::monostate>(value))
        {
            return value;
        }
    }
    return std::monostate{};
}

std::string LogTable::GetFormattedValue(size_t row, size_t column) const
{
    if (column >= mColumnKeyIds.size() || row >= mData.Lines().size())
    {
        return "";
    }
    const std::string &printFormat = mConfiguration.Configuration().columns.at(column).printFormat;
    const auto &line = mData.Lines()[row];
    for (const KeyId id : mColumnKeyIds[column])
    {
        if (id == INVALID_KEY_ID)
        {
            continue;
        }
        const LogValue value = line.GetValue(id);
        if (!std::holds_alternative<std::monostate>(value))
        {
            return FormatLogValue(printFormat, value);
        }
    }
    return std::string{};
}

size_t LogTable::RowCount() const
{
    return mData.Lines().size();
}

const LogData &LogTable::Data() const noexcept
{
    return mData;
}

LogData &LogTable::Data() noexcept
{
    return mData;
}

void LogTable::EvictPrefixRows(size_t count)
{
    if (count == 0)
    {
        return;
    }
    auto &lines = mData.Lines();

    // Release per-line storage for the rows we're about to drop.
    // Non-evicting sources (e.g. mmap'd `FileLineSource`) no-op.
    auto evictSource = [&](size_t firstSurvivingLineId) {
        for (auto &source : mData.Sources())
        {
            if (source != nullptr && source->SupportsEviction())
            {
                source->EvictBefore(firstSurvivingLineId);
            }
        }
    };

    if (count >= lines.size())
    {
        // Past-the-end id so the source clears all live entries.
        size_t firstSurvivingLineId = 0;
        if (!lines.empty())
        {
            firstSurvivingLineId = lines.back().LineId() + 1;
        }
        lines.clear();
        evictSource(firstSurvivingLineId);
        return;
    }

    const size_t firstSurvivingLineId = lines[count].LineId();
    lines.erase(lines.begin(), lines.begin() + static_cast<std::ptrdiff_t>(count));
    evictSource(firstSurvivingLineId);
}

KeyIndex &LogTable::Keys()
{
    return mData.Keys();
}

const KeyIndex &LogTable::Keys() const
{
    return mData.Keys();
}

const LogConfigurationManager &LogTable::Configuration() const
{
    return mConfiguration;
}

LogConfigurationManager &LogTable::Configuration()
{
    return mConfiguration;
}

void LogTable::RefreshColumnKeyIds()
{
    const auto &columns = mConfiguration.Configuration().columns;
    mColumnKeyIds.clear();
    mColumnKeyIds.reserve(columns.size());
    for (const auto &column : columns)
    {
        std::vector<KeyId> ids;
        ids.reserve(column.keys.size());
        for (const auto &key : column.keys)
        {
            ids.push_back(mData.Keys().Find(key));
        }
        mColumnKeyIds.push_back(std::move(ids));
    }
}

void LogTable::RefreshColumnKeyIdsForKeys(const std::vector<std::string> &newKeys)
{
    if (newKeys.empty())
    {
        return;
    }

    std::unordered_set<std::string_view> newKeySet;
    newKeySet.reserve(newKeys.size());
    for (const std::string &key : newKeys)
    {
        newKeySet.emplace(key);
    }

    const auto &columns = mConfiguration.Configuration().columns;
    if (mColumnKeyIds.size() < columns.size())
    {
        mColumnKeyIds.resize(columns.size());
    }

    for (size_t columnIndex = 0; columnIndex < columns.size(); ++columnIndex)
    {
        const auto &column = columns[columnIndex];

        bool affected = mColumnKeyIds[columnIndex].size() != column.keys.size();
        if (!affected)
        {
            for (const std::string &key : column.keys)
            {
                if (newKeySet.contains(std::string_view(key)))
                {
                    affected = true;
                    break;
                }
            }
        }
        if (!affected)
        {
            continue;
        }

        std::vector<KeyId> ids;
        ids.reserve(column.keys.size());
        for (const std::string &key : column.keys)
        {
            ids.push_back(mData.Keys().Find(key));
        }
        mColumnKeyIds[columnIndex] = std::move(ids);
    }
}

void LogTable::RefreshSnapshotTimeKeys()
{
    mStageBSnapshotTimeKeys.clear();
    mPostSnapshotTimeKeys.clear();
    const auto &columns = mConfiguration.Configuration().columns;
    for (const auto &column : columns)
    {
        if (column.type != LogConfiguration::Type::time)
        {
            continue;
        }
        for (const std::string &key : column.keys)
        {
            // `GetOrInsert` so the snapshot holds valid ids on the
            // fresh post-`BeginStreaming` `KeyIndex`.
            const KeyId id = mData.Keys().GetOrInsert(key);
            mStageBSnapshotTimeKeys.insert(id);
        }
    }
}

void LogTable::RefreshSnapshotEnumKeys()
{
    // Pre-create empty dictionaries for every configured enum column so
    // that the enum pass treats their slots as already-encoded targets
    // (encode-if-fits) rather than tracker candidates. Strings will
    // arrive as `OwnedString` / `MmapSlice` from Stage B; the encode
    // pass rewrites them. Multi-key columns share one canonical
    // dictionary via `Alias` so `GetValue` over any key resolves it.
    const auto &columns = mConfiguration.Configuration().columns;
    for (const auto &column : columns)
    {
        if (column.type != LogConfiguration::Type::enumeration)
        {
            continue;
        }
        std::optional<KeyId> canonical;
        for (const std::string &key : column.keys)
        {
            const KeyId id = mData.Keys().GetOrInsert(key);
            if (!canonical.has_value())
            {
                // Stamp the active runtime cap on first creation so
                // pre-configured `Type::enumeration` columns honour
                // the same `AdvancedParserOptions::enumValueCap` knob
                // as auto-promoted ones.
                (void)mEnumDictionaries.GetOrInsert(id, mEnumValueCap);
                canonical = id;
            }
            else
            {
                mEnumDictionaries.Alias(*canonical, id);
            }
        }
    }
}

void LogTable::RewireSourceRegistries()
{
    for (auto &source : mData.Sources())
    {
        if (source != nullptr)
        {
            source->SetEnumDictionaries(&mEnumDictionaries);
        }
    }
}

const EnumDictionaryRegistry &LogTable::EnumDictionaries() const noexcept
{
    return mEnumDictionaries;
}

void LogTable::SetEnumValueCap(uint16_t cap) noexcept
{
    mEnumValueCap = std::clamp<uint16_t>(cap, 1, MAX_ENUM_VALUES);
}

uint16_t LogTable::EnumValueCap() const noexcept
{
    return mEnumValueCap;
}

std::optional<EnumValueId> LogTable::GetEnumValueId(size_t row, size_t column) const noexcept
{
    if (column >= mColumnKeyIds.size() || row >= mData.Lines().size())
    {
        return std::nullopt;
    }
    const auto &line = mData.Lines()[row];
    for (const KeyId id : mColumnKeyIds[column])
    {
        if (id == INVALID_KEY_ID)
        {
            continue;
        }
        if (line.IsDictRef(id))
        {
            return line.GetEnumValueId(id);
        }
    }
    return std::nullopt;
}

void LogTable::RunEnumPassForAppendBatch(
    size_t oldLineCount, std::optional<size_t> &firstBackfilled, std::optional<size_t> &lastBackfilled
)
{
    const auto &columns = mConfiguration.Configuration().columns;
    const size_t totalRows = mData.Lines().size();
    if (totalRows == 0 || oldLineCount >= totalRows)
    {
        return;
    }

    auto recordBackfill = [&](size_t columnIndex) {
        if (!firstBackfilled.has_value() || columnIndex < *firstBackfilled)
        {
            firstBackfilled = columnIndex;
        }
        if (!lastBackfilled.has_value() || columnIndex > *lastBackfilled)
        {
            lastBackfilled = columnIndex;
        }
    };

    // Reuse `mColumnKeyIds` to avoid per-batch `KeyIndex::Find` calls;
    // the cache is kept in sync by `RefreshColumnKeyIdsForKeys`.
    for (size_t columnIndex = 0; columnIndex < columns.size(); ++columnIndex)
    {
        // Refresh per iteration: promotion / demotion mutates `columns[columnIndex].type`.
        const auto &column = columns[columnIndex];
        if (!IsEnumPassEligible(column.type))
        {
            continue;
        }
        if (columnIndex >= mColumnKeyIds.size())
        {
            continue;
        }

        // Filter out unresolved (`INVALID_KEY_ID`) entries once so
        // every per-row loop below sees only present KeyIds. Hoisted
        // onto a member-scoped scratch buffer so the per-(column,batch)
        // allocation only happens until the vector reaches its
        // steady-state capacity.
        mScratchResolvedKeyIds.clear();
        mScratchResolvedKeyIds.reserve(mColumnKeyIds[columnIndex].size());
        for (const KeyId id : mColumnKeyIds[columnIndex])
        {
            if (id != INVALID_KEY_ID)
            {
                mScratchResolvedKeyIds.push_back(id);
            }
        }
        if (mScratchResolvedKeyIds.empty())
        {
            continue;
        }

        if (column.type == LogConfiguration::Type::enumeration)
        {
            if (!EncodeColumnRange(mScratchResolvedKeyIds, oldLineCount, totalRows))
            {
                DemoteColumnFromEnum(columnIndex);
                recordBackfill(columnIndex);
            }
            continue;
        }

        // Type::any candidate. Skip columns whose tracker has previously
        // overflowed (kill-once-stay-killed). Without this guard a
        // demoted enum column that drifts back under the cap on later
        // batches would re-promote and demote in a loop, churning
        // dictionaries and `dataChanged` notifications.
        //
        // The tracker is keyed on `column.keys.front()` -- the first
        // *configured* key string, which is stable for the column's
        // lifetime. Earlier KeyId-based keying was unstable for
        // multi-key columns: the key used to be
        // `mScratchResolvedKeyIds.front()`, which flips whenever a new
        // alias key arrives in a later batch.
        if (column.keys.empty())
        {
            continue;
        }
        const std::string &trackerKey = column.keys.front();
        // `configLocksAny`: a column whose `Type::any` came from a
        // loaded configuration is implicitly locked by the user.
        // Only data-driven discoveries (`Update`/`AppendKeys`) are
        // eligible for auto-promotion to `Type::enumeration`. The
        // saved configuration is treated as authoritative; if a user
        // wants a column to stay `any`, they save it that way.
        // Demotion is unaffected: a configured `Type::enumeration`
        // column that overflows the cap demotes via the
        // `Type::enumeration` branch above and stays demoted via
        // `mEnumPermanentlyKilled`.
        if (!mConfiguration.IsAutoDiscoveredColumn(trackerKey))
        {
            continue;
        }
        if (mEnumPermanentlyKilled.contains(trackerKey))
        {
            continue;
        }
        auto trackerIt = mEnumTrackers.find(trackerKey);
        if (trackerIt == mEnumTrackers.end())
        {
            trackerIt = mEnumTrackers.emplace(trackerKey, EnumCandidateTracker{mEnumValueCap}).first;
        }
        EnumCandidateTracker &tracker = trackerIt->second;

        // Walk the new-batch slice for tracker observations. The
        // tracker is monotonic so prior-batch observations are already
        // accounted for in `tracker.values` / `tracker.size`.
        //
        // `valueHolder` is hoisted to the outer-loop scope so its
        // lifetime spans `tracker.Observe(*bytes)`. `OwnedString` slots
        // (every value materialised through `StreamLineSource`, plus
        // escape-decoded values from `FileLineSource`) materialise as
        // a `std::string` owned by `valueHolder`; if the holder went
        // out of scope at `break`, `bytes` would dangle into freed
        // heap. Test fixtures hide this via SSO; non-SSO column values
        // (>15 bytes) make it crash under ASan.
        for (size_t row = oldLineCount; row < totalRows; ++row)
        {
            const auto &line = mData.Lines()[row];
            LogValue valueHolder;
            std::optional<std::string_view> bytes;
            for (const KeyId id : mScratchResolvedKeyIds)
            {
                valueHolder = line.GetValue(id);
                if (auto sv = AsStringView(valueHolder); sv.has_value())
                {
                    bytes = *sv;
                    break;
                }
            }
            if (bytes.has_value())
            {
                tracker.Observe(*bytes);
            }
            else
            {
                ++tracker.rowsObserved;
            }
            if (tracker.killed)
            {
                break;
            }
        }

        if (tracker.killed)
        {
            mEnumPermanentlyKilled.insert(trackerKey);
            mEnumTrackers.erase(trackerIt);
            continue;
        }

        if (tracker.size > 0 && tracker.size <= mEnumValueCap && tracker.rowsObserved >= ENUM_PROMOTION_MIN_ROWS)
        {
            PromoteColumnToEnum(columnIndex);
            mEnumTrackers.erase(trackerIt);
            recordBackfill(columnIndex);
        }
    }
}

bool LogTable::EncodeColumnRange(const std::vector<KeyId> &keyIds, size_t rowBegin, size_t rowEnd)
{
    if (keyIds.empty())
    {
        return true;
    }
    // The canonical dictionary is keyed on `keyIds.front()`; every
    // other KeyId in the column is an alias so `GetValue(otherKey)`
    // resolves the same `EnumValueId` -> bytes mapping. Stamp the
    // current cap on first creation so the dictionary honours the
    // runtime knob from `AdvancedParserOptions::enumValueCap`.
    EnumDictionary &dict = mEnumDictionaries.GetOrInsert(keyIds.front(), mEnumValueCap);
    for (size_t k = 1; k < keyIds.size(); ++k)
    {
        mEnumDictionaries.Alias(keyIds.front(), keyIds[k]);
    }
    auto &lines = mData.Lines();
    for (size_t row = rowBegin; row < rowEnd && row < lines.size(); ++row)
    {
        auto &line = lines[row];
        // At most one DictRef slot per row: a row that already has a
        // DictRef for any of the column's keys (e.g. via an earlier
        // `AppendBatch`) is considered already-encoded. For rows with
        // values under multiple keys (e.g. both `level` and
        // `severity`), encoding under the first matching key is
        // sufficient because every other key aliases the same
        // dictionary; encoding the rest would mean two `DictRef`s
        // resolving to the same value, doubling memory for nothing.
        bool encodedThisRow = false;
        for (const KeyId id : keyIds)
        {
            if (line.IsDictRef(id))
            {
                encodedThisRow = true;
                break;
            }
            const LogValue v = line.GetValue(id);
            if (std::holds_alternative<std::monostate>(v))
            {
                continue;
            }
            const auto sv = AsStringView(v);
            if (!sv.has_value())
            {
                continue;
            }
            const EnumValueId vid = dict.Insert(*sv);
            if (vid == INVALID_ENUM_VALUE_ID)
            {
                return false;
            }
            line.SetEnumDictRef(id, vid);
            encodedThisRow = true;
            break;
        }
        (void)encodedThisRow;
    }
    return true;
}

bool LogTable::EncodeColumnRangeAsEnum(const LogConfiguration::Column &column, size_t rowBegin, size_t rowEnd)
{
    std::vector<KeyId> keyIds;
    keyIds.reserve(column.keys.size());
    for (const std::string &key : column.keys)
    {
        const KeyId id = mData.Keys().Find(key);
        if (id != INVALID_KEY_ID)
        {
            keyIds.push_back(id);
        }
    }
    return EncodeColumnRange(keyIds, rowBegin, rowEnd);
}

void LogTable::PromoteColumnToEnum(size_t columnIndex)
{
    const auto &columns = mConfiguration.Configuration().columns;
    if (columnIndex >= columns.size())
    {
        return;
    }
    if (columns[columnIndex].type == LogConfiguration::Type::enumeration)
    {
        return;
    }

    mConfiguration.SetColumnType(columnIndex, LogConfiguration::Type::enumeration);

    // Encode every existing row. EncodeColumnRangeAsEnum may bail with
    // false if a previously-untracked value pushes past the cap during
    // the walk (the tracker only saw a subset before rotation); in that
    // case demote immediately.
    if (!EncodeColumnRangeAsEnum(columns[columnIndex], 0U, mData.Lines().size()))
    {
        DemoteColumnFromEnum(columnIndex);
    }
}

void LogTable::DemoteColumnFromEnum(size_t columnIndex)
{
    const auto &columns = mConfiguration.Configuration().columns;
    if (columnIndex >= columns.size())
    {
        return;
    }
    const auto &column = columns[columnIndex];
    if (column.type != LogConfiguration::Type::enumeration)
    {
        return;
    }

    std::vector<KeyId> keyIds;
    keyIds.reserve(column.keys.size());
    for (const std::string &key : column.keys)
    {
        const KeyId id = mData.Keys().Find(key);
        if (id != INVALID_KEY_ID)
        {
            keyIds.push_back(id);
        }
    }

    auto &lines = mData.Lines();

    // Tracks demote cost so the benchmark can flag a regression if a
    // big-table demote spikes. The per-line bytes copy is O(rows) and
    // currently runs synchronously on the caller's thread; a future
    // batched/yielding rewrite is gated on this measurement.
    const auto demoteStart = std::chrono::steady_clock::now();
    size_t convertedSlots = 0;

    if (!keyIds.empty())
    {
        // Resolve dict refs *before* erasing the registry entry: the
        // registry pointer is still installed on every line source, so
        // `line.GetValue(id)` returns the dict-resolved bytes.
        for (auto &line : lines)
        {
            for (const KeyId id : keyIds)
            {
                if (!line.IsDictRef(id))
                {
                    continue;
                }
                std::string bytes;
                const LogValue v = line.GetValue(id);
                if (auto sv = AsStringView(v); sv.has_value())
                {
                    bytes.assign(*sv);
                }
                // `SetValue` with a `std::string` appends bytes to the
                // source's owned-string arena and replaces the slot
                // with `OwnedString`. The dictionary lookup pointer
                // chases through the registry that still has this
                // column registered; we drop the entry below.
                line.SetValue(id, LogValue{std::move(bytes)});
                ++convertedSlots;
            }
        }
        mEnumDictionaries.Erase(keyIds.front());
        // Permanently kill the canonical configured key so the next
        // pass over `Type::any` does not re-tracker / re-promote this
        // column. Keyed on the column-key string (matching the
        // tracker / kill-set type) so multi-key columns with
        // out-of-order key arrival stay killed regardless of which
        // KeyId resolved first.
        if (!column.keys.empty())
        {
            mEnumPermanentlyKilled.insert(column.keys.front());
        }
    }

    mConfiguration.SetColumnType(columnIndex, LogConfiguration::Type::any);

    const auto demoteElapsed =
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - demoteStart);
    if (demoteElapsed.count() > 1000)
    {
        // Surfaced via stderr (no logger dependency in `loglib`) so the
        // benchmark fixture can grep for it without pulling in a
        // structured-log dep. Threshold deliberately low: a sub-ms
        // demote on a pathological workload is interesting context for
        // the eventual batched rewrite.
        fmt::print(
            stderr,
            "[loglib] DemoteColumnFromEnum column={} rows={} slots={} elapsed={}us\n",
            columnIndex,
            lines.size(),
            convertedSlots,
            demoteElapsed.count()
        );
    }
}

std::string LogTable::FormatLogValue(const std::string &format, const LogValue &value)
{
    return std::visit(
        [&format](const auto &arg) -> std::string {
            using T = std::decay_t<decltype(arg)>;
            if constexpr (std::is_same_v<T, std::string>)
            {
                return arg;
            }
            else if constexpr (std::is_same_v<T, std::string_view>)
            {
                return std::string(arg);
            }
            else if constexpr (std::is_same_v<T, int64_t> || std::is_same_v<T, uint64_t> || std::is_same_v<T, double> ||
                               std::is_same_v<T, bool>)
            {
                return fmt::vformat(format, fmt::make_format_args(arg));
            }
            else if constexpr (std::is_same_v<T, TimeStamp>)
            {
                const date::zoned_time localTime{CurrentZone(), std::chrono::round<std::chrono::milliseconds>(arg)};
                return date::format(format, localTime);
            }
            else if constexpr (std::is_same_v<T, std::monostate>)
            {
                return {};
            }
            else
            {
                static_assert(std::is_same_v<T, void>, "non-exhaustive visitor!");
            }
        },
        value
    );
}

} // namespace loglib
