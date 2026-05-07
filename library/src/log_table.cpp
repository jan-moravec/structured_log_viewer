#include "loglib/log_table.hpp"

#include "loglib/file_line_source.hpp"
#include "loglib/internal/compact_log_value.hpp"
#include "loglib/log_processing.hpp"

#include <date/date.h>
#include <date/tz.h>
#include <fmt/format.h>

#include <algorithm>
#include <cassert>
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
    for (uint16_t i = 0; i < size; ++i)
    {
        if (values[i] == bytes)
        {
            return false;
        }
    }
    if (size >= MAX_ENUM_VALUES)
    {
        // Cap+1 distinct values: the column is not (and never will be) a
        // promotion candidate. Drop the buffer to free memory; we keep
        // observing only to maintain `rowsObserved` for parity with the
        // `killed == false` path, but we do not record the new value.
        killed = true;
        for (auto &slot : values)
        {
            slot.clear();
            slot.shrink_to_fit();
        }
        size = 0;
        return true;
    }
    values[size].assign(bytes);
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
                (void)mEnumDictionaries.GetOrInsert(id);
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
        // every per-row loop below sees only present KeyIds.
        std::vector<KeyId> resolvedKeyIds;
        resolvedKeyIds.reserve(mColumnKeyIds[columnIndex].size());
        for (const KeyId id : mColumnKeyIds[columnIndex])
        {
            if (id != INVALID_KEY_ID)
            {
                resolvedKeyIds.push_back(id);
            }
        }
        if (resolvedKeyIds.empty())
        {
            continue;
        }

        if (column.type == LogConfiguration::Type::enumeration)
        {
            if (!EncodeColumnRange(resolvedKeyIds, oldLineCount, totalRows))
            {
                DemoteColumnFromEnum(columnIndex);
                recordBackfill(columnIndex);
            }
            continue;
        }

        // Type::any candidate. Pick a single tracker for the column
        // keyed on the first resolved KeyId so multi-key columns share
        // observations (and so `mEnumTrackers.erase` with the same key
        // works after promotion).
        const KeyId trackerKey = resolvedKeyIds.front();
        EnumCandidateTracker &tracker = mEnumTrackers[trackerKey];

        // Walk the new-batch slice for tracker observations. The
        // tracker is monotonic so prior-batch observations are already
        // accounted for in `tracker.values` / `tracker.size`.
        for (size_t row = oldLineCount; row < totalRows; ++row)
        {
            const auto &line = mData.Lines()[row];
            std::optional<std::string_view> bytes;
            for (const KeyId id : resolvedKeyIds)
            {
                const LogValue value = line.GetValue(id);
                if (auto sv = AsStringView(value); sv.has_value())
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
            mEnumTrackers.erase(trackerKey);
            continue;
        }

        if (tracker.size > 0 && tracker.size <= MAX_ENUM_VALUES && tracker.rowsObserved >= ENUM_PROMOTION_MIN_ROWS)
        {
            PromoteColumnToEnum(columnIndex);
            mEnumTrackers.erase(trackerKey);
            recordBackfill(columnIndex);
        }
    }
}

std::optional<std::string> LogTable::ResolveColumnBytes(const LogConfiguration::Column &column, size_t rowIndex) const
{
    // Cold path: only used by the legacy entry point that doesn't have
    // the cached KeyId list handy. The hot path inside
    // `RunEnumPassForAppendBatch` walks `mColumnKeyIds` directly.
    if (rowIndex >= mData.Lines().size())
    {
        return std::nullopt;
    }
    const auto &line = mData.Lines()[rowIndex];
    for (const std::string &key : column.keys)
    {
        const KeyId id = mData.Keys().Find(key);
        if (id == INVALID_KEY_ID)
        {
            continue;
        }
        const LogValue value = line.GetValue(id);
        if (auto sv = AsStringView(value); sv.has_value())
        {
            return std::string(*sv);
        }
    }
    return std::nullopt;
}

bool LogTable::EncodeColumnRange(const std::vector<KeyId> &keyIds, size_t rowBegin, size_t rowEnd)
{
    if (keyIds.empty())
    {
        return true;
    }
    // The canonical dictionary is keyed on `keyIds.front()`; every
    // other KeyId in the column is an alias so `GetValue(otherKey)`
    // resolves the same `EnumValueId` -> bytes mapping.
    EnumDictionary &dict = mEnumDictionaries.GetOrInsert(keyIds.front());
    for (size_t k = 1; k < keyIds.size(); ++k)
    {
        mEnumDictionaries.Alias(keyIds.front(), keyIds[k]);
    }
    auto &lines = mData.Lines();
    for (size_t row = rowBegin; row < rowEnd && row < lines.size(); ++row)
    {
        auto &line = lines[row];
        for (const KeyId id : keyIds)
        {
            if (line.IsDictRef(id))
            {
                continue;
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
        }
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
            }
        }
        mEnumDictionaries.Erase(keyIds.front());
    }

    mConfiguration.SetColumnType(columnIndex, LogConfiguration::Type::any);
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
