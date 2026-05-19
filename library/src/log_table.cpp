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

/// Static-parse min rows before promoting `Type::Any + autoDetect` to enum.
/// Smaller files are caught at end-of-parse by `FinalizeAutoDetection`.
constexpr size_t ENUM_PROMOTION_MIN_ROWS = 4096;

/// Streaming promotion threshold; promote eagerly, dictionary/length caps guard.
constexpr size_t STREAM_PROMOTION_MIN_ROWS = 2;

/// Static-mode cardinality bail: distinct/observed ratio limit. Tight
/// because the dictionary-cap kill catches high-cardinality columns
/// first; this only governs sparse-presence candidates below cap.
constexpr double ENUM_CARDINALITY_BAIL_RATIO = 0.05;

/// Max fraction of over-cap-length or wrong-type observations before demotion.
constexpr double ENUM_HEALTH_TOLERANCE_RATIO = 0.01;

/// Minimum samples before consulting the tolerance ratio.
constexpr size_t ENUM_HEALTH_MIN_SAMPLES = 50;

/// Dict-weighted tolerance for `MaybePromoteToLevel`. Allows at most one
/// unrecognized entry per `LEVEL_DICT_TOLERANCE_RATIO` canonical ones --
/// e.g. a 4-entry dict must be 100% canonical, a 5-entry dict tolerates
/// one stray, a 10-entry dict tolerates two. Trades false-positive risk
/// against tolerance for occasional non-canonical mixed-in values.
constexpr size_t LEVEL_DICT_TOLERANCE_RATIO = 4;

/// Microsecond threshold above which `DemoteColumnFromEnum` emits a
/// stderr telemetry line; below it the demote cost is uninteresting.
constexpr int64_t DEMOTE_TELEMETRY_LOG_THRESHOLD_US = 1000;

/// Pick a terminal type from observed numeric / bool tag counts.
///
///   - bools only         -> `Boolean`
///   - integers / doubles -> `Integer` / `Floating` / `Number`
///   - bool + numeric mix -> `Any` (no specialised widget)
///   - nothing observed   -> `Any` (historical bail)
///
/// Caveat: an already-promoted enum that later sees bool slots
/// demotes to `Type::String` (via lumped `wrongTypeSlots`), not
/// `Boolean`. If that case matters, route the demote through here
/// using `EnumColumnHealth`'s per-tag counters.
LogConfiguration::Type RouteNoStringBail(
    size_t intObservations, size_t uintObservations, size_t doubleObservations, size_t boolObservations
) noexcept
{
    const bool sawIntegral = intObservations > 0 || uintObservations > 0;
    const bool sawDouble = doubleObservations > 0;
    const bool sawBool = boolObservations > 0;
    const bool sawNumeric = sawIntegral || sawDouble;
    if (sawBool && sawNumeric)
    {
        return LogConfiguration::Type::Any;
    }
    if (sawBool)
    {
        return LogConfiguration::Type::Boolean;
    }
    if (sawIntegral && sawDouble)
    {
        return LogConfiguration::Type::Number;
    }
    if (sawIntegral)
    {
        return LogConfiguration::Type::Integer;
    }
    if (sawDouble)
    {
        return LogConfiguration::Type::Floating;
    }
    return LogConfiguration::Type::Any;
}

bool IsEnumPassEligible(const LogConfiguration::Column &column) noexcept
{
    // `Enumeration` / `Level` always need per-batch encoding so even
    // user-pinned dict columns keep accumulating `DictRef`s. `Any` is
    // a candidate scan only when the user hasn't disabled auto-detect.
    if (column.type == LogConfiguration::Type::Enumeration || column.type == LogConfiguration::Type::Level)
    {
        return true;
    }
    return column.type == LogConfiguration::Type::Any && column.autoDetect;
}

} // namespace

bool LogTable::EnumColumnHealth::ShouldDemote(double tolerance, size_t minSamples) const noexcept
{
    if (totalSlots < minSamples)
    {
        return false;
    }
    const double bad = static_cast<double>(longValueSlots) + static_cast<double>(wrongTypeSlots);
    return bad > tolerance * static_cast<double>(totalSlots);
}

void LogTable::EnumCandidateTracker::Observe(std::string_view bytes)
{
    if (killed)
    {
        return;
    }
    if (valueMaxLen != 0 && bytes.size() > valueMaxLen)
    {
        ++longValueCount;
        if (presenceCount >= ENUM_HEALTH_MIN_SAMPLES &&
            static_cast<double>(longValueCount) > ENUM_HEALTH_TOLERANCE_RATIO * static_cast<double>(presenceCount))
        {
            killed = true;
            values = {};
            seen = {};
            size = 0;
        }
        return;
    }
    // O(1) membership check; transparent hashing avoids constructing a
    // temporary `std::string` from `bytes` on every call.
    if (seen.contains(bytes))
    {
        return;
    }
    if (size >= cap)
    {
        // Cap exceeded: caller flips the column to `Type::String`.
        killed = true;
        values = {};
        seen = {};
        size = 0;
        return;
    }
    values.emplace_back(bytes);
    seen.emplace(values.back());
    ++size;
}

LogTable::LogTable(LogData data, LogConfigurationManager configuration)
    : mData(std::move(data)), mConfiguration(std::move(configuration))
{
    mLastBatchDemotedKeys.clear();
    RewireSourceRegistries();
    RefreshSnapshotEnumKeys();
    RefreshColumnKeyIds();
    // Encode pre-configured enum columns and run auto-detection on
    // `Type::Any + autoDetect` candidates over the loaded data. The
    // finalize sweep then promotes small-file candidates that missed
    // the per-batch threshold.
    std::optional<size_t> firstBackfilled;
    std::optional<size_t> lastBackfilled;
    RunEnumPassForAppendBatch(0U, firstBackfilled, lastBackfilled);
    FinalizeAutoDetection();
}

// MSVC's `unordered_set` move ctor allocates a 1-cell container proxy and
// can theoretically throw `bad_array_new_length`; this `noexcept` matches
// the public move-ctor contract that callers (e.g. `std::vector::resize`)
// rely on.
// NOLINTNEXTLINE(bugprone-exception-escape)
LogTable::LogTable(LogTable &&other) noexcept
    : mData(std::move(other.mData)),
      mConfiguration(std::move(other.mConfiguration)),
      mColumnKeyIds(std::move(other.mColumnKeyIds)),
      mStageBSnapshotTimeKeys(std::move(other.mStageBSnapshotTimeKeys)),
      mPostSnapshotTimeKeys(std::move(other.mPostSnapshotTimeKeys)),
      mEnumDictionaries(std::move(other.mEnumDictionaries)),
      mEnumValueCap(other.mEnumValueCap),
      mEnumValueMaxLen(other.mEnumValueMaxLen),
      mEnumTrackers(std::move(other.mEnumTrackers)),
      mEnumColumnHealth(std::move(other.mEnumColumnHealth)),
      mIsStreaming(other.mIsStreaming),
      mLastBackfillRange(std::move(other.mLastBackfillRange)),
      mLastBatchDemotedKeys(std::move(other.mLastBatchDemotedKeys)),
      mLevelRankCache(std::move(other.mLevelRankCache))
{
    other.mIsStreaming = false;
    other.mLastBatchDemotedKeys.clear();
    // Each `LineSource` cached `&other.mEnumDictionaries`; rebind to ours.
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
    mEnumValueMaxLen = other.mEnumValueMaxLen;
    mEnumTrackers = std::move(other.mEnumTrackers);
    mEnumColumnHealth = std::move(other.mEnumColumnHealth);
    mIsStreaming = other.mIsStreaming;
    other.mIsStreaming = false;
    mLastBackfillRange = std::move(other.mLastBackfillRange);
    mLastBatchDemotedKeys = std::move(other.mLastBatchDemotedKeys);
    other.mLastBatchDemotedKeys.clear();
    mLevelRankCache = std::move(other.mLevelRankCache);
    // Each `LineSource` cached `&other.mEnumDictionaries`; rebind to ours.
    RewireSourceRegistries();
    return *this;
}

void LogTable::Update(LogData &&data)
{
    mLastBatchDemotedKeys.clear();
    const size_t oldLineCount = mData.Lines().size();
    mConfiguration.Update(data);
    if (!data.TimestampsAlreadyParsed())
    {
        ParseTimestamps(data, mConfiguration.Configuration());
    }
    mData.Merge(std::move(data));
    RewireSourceRegistries();
    // Snapshot enum keys first so `GetOrInsert` registers them into
    // `mData.Keys()`, then resolve column keys against the
    // now-complete `KeyIndex`. Resolving first would leave column
    // keys for not-yet-seen enums as `INVALID_KEY_ID`.
    RefreshSnapshotEnumKeys();
    RefreshColumnKeyIds();
    std::optional<size_t> firstBackfilled;
    std::optional<size_t> lastBackfilled;
    RunEnumPassForAppendBatch(oldLineCount, firstBackfilled, lastBackfilled);
    FinalizeAutoDetection();
}

void LogTable::Reset()
{
    mData = LogData{};
    mStageBSnapshotTimeKeys.clear();
    mPostSnapshotTimeKeys.clear();
    mEnumDictionaries.Clear();
    mEnumTrackers.clear();
    mEnumColumnHealth.clear();
    mLevelRankCache.clear();
    mIsStreaming = false;
    mLastBackfillRange.reset();
    // Match the sibling teardown paths so a reader between `Reset` and
    // the next batch-style call doesn't see stale demote ids.
    mLastBatchDemotedKeys.clear();
    RefreshColumnKeyIds();
    // Re-seed dictionaries from the configuration so a `LevelRankCache`
    // query made before the next batch sees configured Level columns
    // instead of `nullptr`. Also rebuilds any cache invalidated by a
    // freshly-loaded `levelMapping`.
    RefreshSnapshotEnumKeys();
}

void LogTable::BeginStreaming(std::unique_ptr<LineSource> source)
{
    mLastBackfillRange.reset();
    mLastBatchDemotedKeys.clear();
    // Eager promotion + no cardinality bail until `FinalizeAutoDetection`.
    mIsStreaming = true;

    if (source)
    {
        std::vector<LogLine> noLines;
        LogData fresh(std::move(source), std::move(noLines), KeyIndex{});
        // Both pipelines promote `Type::Time` inline.
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
    mEnumColumnHealth.clear();
    mLevelRankCache.clear();
    RewireSourceRegistries();

    // Snapshot inserts the time-column keys before KeyId resolution runs.
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

    // Splice in the new source; existing lines/keys stay so prior rows
    // remain visible and KeyIds line up across files.
    source->SetEnumDictionaries(&mEnumDictionaries);
    mData.Sources().push_back(std::move(source));
}

void LogTable::AppendBatch(StreamedBatch batch)
{
    mLastBackfillRange.reset();
    mLastBatchDemotedKeys.clear();

    if (!batch.newKeys.empty())
    {
        mConfiguration.AppendKeys(batch.newKeys);
    }

    // Snapshot before append so slice back-fill only touches new rows.
    const size_t oldLineCount = mData.Lines().size();

    if (!batch.lines.empty() || !batch.localLineOffsets.empty())
    {
        mData.AppendBatch(std::move(batch.lines), batch.localLineOffsets);
    }

    if (!batch.newKeys.empty())
    {
        RefreshColumnKeyIdsForKeys(batch.newKeys);
    }

    // Back-fill post-snapshot time columns. First observation: back-fill
    // all rows and report via `mLastBackfillRange`. Known post-snapshot
    // column: back-fill only the appended slice.
    const auto &columns = mConfiguration.Configuration().columns;
    std::optional<size_t> firstBackfilled;
    std::optional<size_t> lastBackfilled;
    for (size_t columnIndex = 0; columnIndex < columns.size(); ++columnIndex)
    {
        const auto &column = columns[columnIndex];
        if (column.type != LogConfiguration::Type::Time)
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

const std::vector<KeyId> &LogTable::LastBatchDemotedKeys() const noexcept
{
    return mLastBatchDemotedKeys;
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

std::string_view LogTable::GetValueOrFormatted(size_t row, size_t column, std::string &buffer) const
{
    if (column >= mColumnKeyIds.size() || row >= mData.Lines().size())
    {
        return {};
    }
    const auto &columns = mConfiguration.Configuration().columns;
    const auto &line = mData.Lines()[row];
    for (const KeyId id : mColumnKeyIds[column])
    {
        if (id == INVALID_KEY_ID)
        {
            continue;
        }
        LogValue value = line.GetValue(id);
        if (std::holds_alternative<std::monostate>(value))
        {
            continue;
        }
        if (const auto *sv = std::get_if<std::string_view>(&value); sv != nullptr)
        {
            return *sv;
        }
        if (const auto *s = std::get_if<std::string>(&value); s != nullptr)
        {
            // `std::string` alternative is rare (escape-decoded JSON
            // strings); copy into the caller's buffer so the returned
            // view has a stable lifetime regardless of the `value`
            // local going out of scope.
            buffer.assign(*s);
            return buffer;
        }
        buffer = FormatLogValue(columns[column].printFormat, value);
        return buffer;
    }
    return {};
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

    // Release per-line storage for evicted rows. Non-evicting sources no-op.
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
        // Past-the-end id clears all live entries.
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
        if (column.type != LogConfiguration::Type::Time)
        {
            continue;
        }
        for (const std::string &key : column.keys)
        {
            // GetOrInsert so the snapshot holds valid ids on a fresh KeyIndex.
            const KeyId id = mData.Keys().GetOrInsert(key);
            mStageBSnapshotTimeKeys.insert(id);
        }
    }
}

void LogTable::RefreshSnapshotEnumKeys()
{
    // Pre-create dictionaries for every configured enum / level column.
    // Multi-key columns share one canonical dictionary via `Alias`.
    // Idempotent. Level columns are an Enumeration subtype and share
    // the same dictionary plumbing.
    //
    // Drop the level rank cache first so a freshly-loaded `levelMapping`
    // is honoured: `RefreshLevelRankCache` is append-only on existing
    // entries and would otherwise keep the stale mapping.
    mLevelRankCache.clear();
    const auto &columns = mConfiguration.Configuration().columns;
    for (size_t columnIndex = 0; columnIndex < columns.size(); ++columnIndex)
    {
        const auto &column = columns[columnIndex];
        if (column.type != LogConfiguration::Type::Enumeration && column.type != LogConfiguration::Type::Level)
        {
            continue;
        }
        std::optional<KeyId> canonical;
        for (const std::string &key : column.keys)
        {
            const KeyId id = mData.Keys().GetOrInsert(key);
            if (!canonical.has_value())
            {
                (void)mEnumDictionaries.GetOrInsert(id, mEnumValueCap);
                canonical = id;
            }
            else
            {
                if (!mEnumDictionaries.Alias(*canonical, id))
                {
                    fmt::print(
                        stderr,
                        "[loglib] RefreshSnapshotEnumKeys: failed to alias key {} onto canonical {} for column "
                        "'{}'\n",
                        static_cast<uint32_t>(id),
                        static_cast<uint32_t>(*canonical),
                        column.header
                    );
                }
            }
        }
        // Seed the rank cache for saved `Type::Level` columns so it's
        // ready by the time the first encode pass runs.
        if (column.type == LogConfiguration::Type::Level)
        {
            RefreshLevelRankCache(columnIndex);
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

void LogTable::SetEnumValueMaxLen(uint32_t maxLen) noexcept
{
    mEnumValueMaxLen = maxLen;
}

uint32_t LogTable::EnumValueMaxLen() const noexcept
{
    return mEnumValueMaxLen;
}

std::optional<EnumValueId> LogTable::GetEnumValueId(size_t row, size_t column) const noexcept
{
    if (column >= mColumnKeyIds.size() || row >= mData.Lines().size())
    {
        return std::nullopt;
    }
    const auto &line = mData.Lines()[row];
    // `LogLine::GetEnumValueId` does one `FindCompact` walk and
    // returns nullopt for absent or non-`DictRef` slots, so a single
    // call covers what the old `IsDictRef + GetEnumValueId` pair
    // walked the line twice for.
    for (const KeyId id : mColumnKeyIds[column])
    {
        if (id == INVALID_KEY_ID)
        {
            continue;
        }
        if (auto vid = line.GetEnumValueId(id); vid.has_value())
        {
            return vid;
        }
    }
    return std::nullopt;
}

LogTable::EnumColumnLookup LogTable::ResolveEnumColumn(size_t columnIndex) const noexcept
{
    const auto &columns = mConfiguration.Configuration().columns;
    if (columnIndex >= columns.size())
    {
        return {};
    }
    const auto &column = columns[columnIndex];
    if (column.keys.empty())
    {
        return {};
    }
    // The first key is canonical; aliases share its dictionary entry.
    const KeyId canonicalKey = Keys().Find(column.keys.front());
    if (canonicalKey == INVALID_KEY_ID)
    {
        return {};
    }
    // `dictionary` is null when the column is not currently
    // `Type::Enumeration`; still useful to the caller alongside the
    // resolved key.
    return {.canonicalKey = canonicalKey, .dictionary = mEnumDictionaries.Find(canonicalKey)};
}

namespace
{

/// `true` iff a slot tag is a valid representation of @p declaredType.
/// Mirrors the variant alternatives accepted by the sort/filter
/// comparators in `log_compare.cpp` / `log_filter.cpp`; keep the two
/// sides in sync. `Type::Any` accepts every present tag (no
/// expectation imposed).
bool TagMatchesType(internal::CompactTag tag, LogConfiguration::Type declaredType) noexcept
{
    using Type = LogConfiguration::Type;
    using Tag = internal::CompactTag;
    if (tag == Tag::Monostate)
    {
        return false;
    }
    switch (declaredType)
    {
    case Type::Any:
        return true;
    case Type::String:
        return tag == Tag::MmapSlice || tag == Tag::OwnedString || tag == Tag::DictRef;
    case Type::Boolean:
        return tag == Tag::Bool;
    case Type::Integer:
        return tag == Tag::Int64 || tag == Tag::Uint64;
    case Type::Floating:
        return tag == Tag::Double;
    case Type::Number:
        return tag == Tag::Int64 || tag == Tag::Uint64 || tag == Tag::Double;
    case Type::Time:
        // `Timestamp` is the natural tag; epoch integers ride the
        // same comparator so they count as matching for the
        // diagnostic.
        return tag == Tag::Timestamp || tag == Tag::Int64 || tag == Tag::Uint64;
    case Type::Enumeration:
    case Type::Level:
        // Encoded-as-dict is the only "matching" state. An OwnedString
        // slot on a user-pinned enum column is the over-cap value the
        // diagnostic exists to surface.
        return tag == Tag::DictRef;
    }
    return false;
}

} // namespace

LogTable::ColumnTypeHealth LogTable::ComputeColumnTypeHealth(size_t columnIndex) const
{
    ColumnTypeHealth health;
    const auto &columns = mConfiguration.Configuration().columns;
    if (columnIndex >= columns.size())
    {
        return health;
    }
    const auto &column = columns[columnIndex];
    const auto &lines = mData.Lines();
    health.totalSlots = lines.size();
    if (column.keys.empty())
    {
        return health;
    }

    // Resolve every alias `KeyId` once; absent ones drop out. The
    // canonical alias is conventionally first, but all entries can
    // carry slots (alias-list reorders preserve their `KeyId`s).
    std::vector<KeyId> aliasKeys;
    aliasKeys.reserve(column.keys.size());
    for (const std::string &key : column.keys)
    {
        const KeyId id = mData.Keys().Find(key);
        if (id != INVALID_KEY_ID)
        {
            aliasKeys.push_back(id);
        }
    }
    if (aliasKeys.empty())
    {
        // No `KeyId`s interned yet (column from a config that hasn't
        // seen any data): every row counts as absent.
        return health;
    }

    for (const LogLine &line : lines)
    {
        const internal::CompactLogValue *slot = nullptr;
        for (const KeyId id : aliasKeys)
        {
            slot = line.FindCompact(id);
            if (slot != nullptr)
            {
                break;
            }
        }
        if (slot == nullptr || slot->tag == internal::CompactTag::Monostate)
        {
            continue;
        }
        ++health.presentSlots;
        if (TagMatchesType(slot->tag, column.type))
        {
            ++health.matchingSlots;
        }
    }
    return health;
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

    // Active (`Type::Enumeration`) and candidate (`Type::Any +
    // autoDetect`) columns are handled inline in a single walk; every
    // other type is skipped.

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

    // Scratch reused across the per-column loop.
    std::vector<KeyId> resolvedKeys;
    auto resolveKeys = [&](size_t columnIndex) {
        resolvedKeys.clear();
        resolvedKeys.reserve(mColumnKeyIds[columnIndex].size());
        for (const KeyId id : mColumnKeyIds[columnIndex])
        {
            if (id != INVALID_KEY_ID)
            {
                resolvedKeys.push_back(id);
            }
        }
    };

    for (size_t columnIndex = 0; columnIndex < columns.size(); ++columnIndex)
    {
        const auto &column = columns[columnIndex];
        if (!IsEnumPassEligible(column))
        {
            continue;
        }
        if (columnIndex >= mColumnKeyIds.size())
        {
            continue;
        }

        // Active enum / level column: encode the appended slice.
        // Length / wrong-type hits accrue against the health budget;
        // dictionary-cap overflow demotes immediately. Level columns
        // share the encoding path; their rank cache is refreshed below
        // to pick up any newly-added dictionary entries.
        if (column.type == LogConfiguration::Type::Enumeration || column.type == LogConfiguration::Type::Level)
        {
            resolveKeys(columnIndex);
            if (resolvedKeys.empty())
            {
                continue;
            }
            EnumColumnHealth &health = mEnumColumnHealth[resolvedKeys.front()];

            // Snapshot dict size to gate the `Enumeration -> Level`
            // re-check on growth: the promotion rule is dict-weighted,
            // so without new entries the answer cannot change.
            const EnumDictionary *dictBefore = mEnumDictionaries.Find(resolvedKeys.front());
            const size_t oldDictSize = (dictBefore != nullptr) ? dictBefore->Size() : 0;

            const bool encodeOk = EncodeColumnRange(resolvedKeys, oldLineCount, totalRows, health);
            // User-pinned enum/level columns (`autoDetect == false`)
            // degrade gracefully: dict cap and health-budget breaches
            // leave the column at its declared type; mismatched slots
            // simply stay un-encoded (visible as raw strings).
            if (!encodeOk)
            {
                if (column.autoDetect)
                {
                    DemoteColumnFromEnum(columnIndex);
                    recordBackfill(columnIndex);
                }
                continue;
            }
            if (column.autoDetect && health.ShouldDemote(ENUM_HEALTH_TOLERANCE_RATIO, ENUM_HEALTH_MIN_SAMPLES))
            {
                DemoteColumnFromEnum(columnIndex);
                recordBackfill(columnIndex);
                continue;
            }
            // Level-promotion re-check: an Enumeration column whose
            // key matches `IsLogLevelKey` may finally satisfy the
            // dict tolerance once canonical entries arrive. Gated on
            // dict growth + name match so unrelated columns pay
            // nothing. Skipped entirely when the user pinned the
            // column at `Enumeration` (autoDetect == false).
            if (column.type == LogConfiguration::Type::Enumeration && column.autoDetect)
            {
                const EnumDictionary *dictAfter = mEnumDictionaries.Find(resolvedKeys.front());
                const size_t newDictSize = (dictAfter != nullptr) ? dictAfter->Size() : 0;
                if (newDictSize > oldDictSize && std::ranges::any_of(column.keys, IsLogLevelKey))
                {
                    MaybePromoteToLevel(columnIndex);
                }
            }
            if (column.type == LogConfiguration::Type::Level)
            {
                // Idempotent: a just-promoted column's cache is already
                // populated, an existing one picks up this batch's new
                // entries.
                RefreshLevelRankCache(columnIndex);
            }
            continue;
        }

        if (column.keys.empty())
        {
            continue;
        }

        // Candidate scan. Tracker keyed on the canonical `KeyId` so
        // a header rename via `LogConfigurationManager::SetColumnHeader`
        // cannot orphan the running counters (an alias-list reorder
        // is already safe because we always key on `keys.front()` ->
        // canonical id). On bail / kill the column type flips to a
        // terminal (`string` / `integer` / `floating` / `number`) or
        // stays at `any`, all of which remove it from
        // `IsEnumPassEligible` for the next batch -- the type itself
        // enforces kill-once-stay-killed.
        const size_t promotionMinRows = mIsStreaming ? STREAM_PROMOTION_MIN_ROWS : ENUM_PROMOTION_MIN_ROWS;

        resolveKeys(columnIndex);
        if (resolvedKeys.empty())
        {
            continue;
        }
        const KeyId trackerKey = resolvedKeys.front();

        auto trackerIt = mEnumTrackers.find(trackerKey);
        if (trackerIt == mEnumTrackers.end())
        {
            trackerIt = mEnumTrackers.emplace(trackerKey, EnumCandidateTracker{mEnumValueCap, mEnumValueMaxLen}).first;
        }
        EnumCandidateTracker &tracker = trackerIt->second;

        // Sampled scan: bail once we have enough evidence (or hit
        // `scanCap`). `presenceCount` (slot present, any tag) is
        // decoupled from `rowsObserved` so leading sparse rows don't
        // trigger the no-string bail prematurely.
        const size_t scanCap = 2 * promotionMinRows;
        for (size_t row = oldLineCount; row < totalRows; ++row)
        {
            const LogLine &line = mData.Lines()[row];
            const internal::CompactLogValue *slot = nullptr;
            for (const KeyId id : resolvedKeys)
            {
                slot = line.FindCompact(id);
                if (slot != nullptr)
                {
                    break;
                }
            }
            ++tracker.rowsObserved;
            if (slot != nullptr)
            {
                ++tracker.presenceCount;
                std::optional<std::string_view> bytes = line.PeekStringView(*slot);
                if (bytes.has_value())
                {
                    tracker.Observe(*bytes);
                }
                else if (slot->tag == internal::CompactTag::Int64)
                {
                    ++tracker.intObservations;
                }
                else if (slot->tag == internal::CompactTag::Uint64)
                {
                    ++tracker.uintObservations;
                }
                else if (slot->tag == internal::CompactTag::Double)
                {
                    ++tracker.doubleObservations;
                }
                else if (slot->tag == internal::CompactTag::Bool)
                {
                    ++tracker.boolObservations;
                }
            }
            if (tracker.killed)
            {
                break;
            }
            if (tracker.size > 0 && tracker.size <= mEnumValueCap && tracker.presenceCount >= promotionMinRows)
            {
                break;
            }
            if (tracker.rowsObserved >= scanCap)
            {
                break;
            }
        }

        if (tracker.killed)
        {
            // Too varied to enumerate; route to `string`.
            mConfiguration.SetColumnType(columnIndex, LogConfiguration::Type::String);
            mEnumTrackers.erase(trackerIt);
            continue;
        }

        if (tracker.size > 0 && tracker.size <= mEnumValueCap && tracker.presenceCount >= promotionMinRows)
        {
            PromoteColumnToEnum(columnIndex);
            mEnumTrackers.erase(trackerIt);
            recordBackfill(columnIndex);
            continue;
        }

        // Bail on candidates unlikely to stabilise. Static mode
        // applies the cardinality bail; stream mode skips it.
        if (tracker.rowsObserved >= scanCap)
        {
            // Sparse column not yet seen: reset the scanCap budget
            // and try again on the next batch.
            if (tracker.presenceCount == 0)
            {
                tracker.rowsObserved = 0;
                continue;
            }
            const bool noStringSeen = tracker.size == 0;
            const bool highCardinality = !mIsStreaming && tracker.size > 0 &&
                                         static_cast<double>(tracker.size) >
                                             ENUM_CARDINALITY_BAIL_RATIO * static_cast<double>(tracker.presenceCount);
            if (noStringSeen)
            {
                mConfiguration.SetColumnType(
                    columnIndex,
                    RouteNoStringBail(
                        tracker.intObservations,
                        tracker.uintObservations,
                        tracker.doubleObservations,
                        tracker.boolObservations
                    )
                );
                mEnumTrackers.erase(trackerIt);
            }
            else if (highCardinality)
            {
                mConfiguration.SetColumnType(columnIndex, LogConfiguration::Type::String);
                mEnumTrackers.erase(trackerIt);
            }
        }
    }
}

bool LogTable::FinalizeAutoDetection()
{
    // Permissive sweep over surviving candidate trackers; runs at
    // end-of-static-parse and end-of-stream. Idempotent.
    if (mEnumTrackers.empty())
    {
        mIsStreaming = false;
        return false;
    }

    bool promoted = false;
    const auto &columns = mConfiguration.Configuration().columns;
    for (size_t columnIndex = 0; columnIndex < columns.size(); ++columnIndex)
    {
        const auto &column = columns[columnIndex];
        if (column.type != LogConfiguration::Type::Any || !column.autoDetect || column.keys.empty())
        {
            continue;
        }
        // Trackers are keyed by canonical `KeyId` (see
        // `mEnumTrackers` doc); look up via the column's first key.
        const KeyId trackerKey = mData.Keys().Find(column.keys.front());
        if (trackerKey == INVALID_KEY_ID)
        {
            continue;
        }
        auto trackerIt = mEnumTrackers.find(trackerKey);
        if (trackerIt == mEnumTrackers.end())
        {
            continue;
        }
        const EnumCandidateTracker &tracker = trackerIt->second;
        if (tracker.killed)
        {
            mConfiguration.SetColumnType(columnIndex, LogConfiguration::Type::String);
            continue;
        }
        if (tracker.size > 0 && tracker.size <= mEnumValueCap && tracker.presenceCount >= 2)
        {
            PromoteColumnToEnum(columnIndex);
            promoted = true;
            continue;
        }
        if (tracker.size == 0 && tracker.presenceCount > 0)
        {
            mConfiguration.SetColumnType(
                columnIndex,
                RouteNoStringBail(
                    tracker.intObservations,
                    tracker.uintObservations,
                    tracker.doubleObservations,
                    tracker.boolObservations
                )
            );
            continue;
        }
        // Insufficient evidence: leave at `Type::Any + autoDetect` for
        // a future re-load.
    }

    mEnumTrackers.clear();
    mIsStreaming = false;
    return promoted;
}

void LogTable::OnUserChangedColumnType(size_t columnIndex, LogConfiguration::Type previousType)
{
    const auto &columns = mConfiguration.Configuration().columns;
    if (columnIndex >= columns.size())
    {
        return;
    }
    // Re-resolve through a fresh snapshot at each access so the type
    // mutation below cannot expose a stale-reference window.
    const auto snapshot = [this, columnIndex]() -> LogConfiguration::Column {
        return mConfiguration.Configuration().columns[columnIndex];
    };

    // Drop any stale candidate state so a subsequent flip back to
    // `Type::Any + autoDetect` starts the auto-detector clean; the
    // existing entries were keyed off a different `(type, autoDetect)`
    // contract.
    {
        const auto col = snapshot();
        if (!col.keys.empty())
        {
            const KeyId canonical = mData.Keys().Find(col.keys.front());
            if (canonical != INVALID_KEY_ID)
            {
                mEnumTrackers.erase(canonical);
            }
        }
    }

    // Refresh column key ids first: a column whose canonical key
    // hasn't been observed yet still resolves correctly through
    // `GetOrInsert` below for the Time/Enum paths, but the cached
    // ids matter for the encoding/back-fill walks.
    RefreshColumnKeyIds();

    const auto column = snapshot();
    switch (column.type)
    {
    case LogConfiguration::Type::Time:
    {
        // Seed sensible defaults when the column wasn't already a
        // Time column. Auto-detected Time columns ship with these;
        // user-pinned ones from the editor would otherwise carry
        // empty vectors and silently fail to parse anything.
        if (column.printFormat.empty())
        {
            mConfiguration.SetColumnPrintFormat(columnIndex, "%F %H:%M:%S");
        }
        if (column.parseFormats.empty())
        {
            mConfiguration.SetColumnParseFormats(columnIndex, {"%FT%T%Ez", "%F %T%Ez", "%FT%T", "%F %T"});
        }
        BackfillTimestampColumn(
            mConfiguration.Configuration().columns[columnIndex],
            std::span<LogLine>(mData.Lines()),
            BackfillErrors::Discard
        );
        break;
    }
    case LogConfiguration::Type::Enumeration:
    case LogConfiguration::Type::Level:
    {
        // Seed the canonical dictionary and any aliases; idempotent
        // on a column that was already promoted by the streaming
        // detector. Walks every existing row, encoding string-typed
        // slots as `DictRef` and accruing length / wrong-type
        // observations against the health budget so a subsequent
        // batch sees the same shape the detector would have.
        RefreshSnapshotEnumKeys();
        RefreshColumnKeyIds();
        if (column.keys.empty())
        {
            break;
        }
        const KeyId canonical = mData.Keys().Find(column.keys.front());
        if (canonical == INVALID_KEY_ID)
        {
            // No data yet for this key: nothing to encode. The next
            // batch will populate the dictionary through the normal
            // streaming path.
            break;
        }
        EnumColumnHealth &health = mEnumColumnHealth[canonical];
        // Health survives across user edits, but a re-promote on a
        // previously-demoted column should start with a clean budget
        // so an old breach doesn't immediately re-demote. A no-op
        // transition within the enum family (e.g. toggling autoDetect
        // on an already-Enumeration column, or sub-promoting
        // Enumeration <-> Level) keeps the accumulated budget so the
        // user doesn't lose hard-won evidence on a cosmetic edit.
        const bool previousWasEnumLike = previousType == LogConfiguration::Type::Enumeration ||
                                         previousType == LogConfiguration::Type::Level;
        if (!previousWasEnumLike)
        {
            health = EnumColumnHealth{};
        }
        if (!EncodeColumnRangeAsEnum(
                mConfiguration.Configuration().columns[columnIndex], 0U, mData.Lines().size(), health
            ))
        {
            // Hard cap overflow: bail back to the user's pin would
            // contradict the explicit choice. Fall back to String so
            // sort/filter remain sane; the diagnostic surface will
            // explain why. Editor-driven, so don't record into the
            // batch-scoped demote list (see DemoteColumnFromEnum doc).
            DemoteColumnFromEnum(columnIndex, /*recordForBatch=*/false);
            return;
        }
        if (column.type == LogConfiguration::Type::Level)
        {
            RefreshLevelRankCache(columnIndex);
        }
        break;
    }
    case LogConfiguration::Type::Any:
    case LogConfiguration::Type::String:
    case LogConfiguration::Type::Boolean:
    case LogConfiguration::Type::Integer:
    case LogConfiguration::Type::Floating:
    case LogConfiguration::Type::Number:
    {
        // When leaving `Type::Time` we have to drop the strftime-style
        // `printFormat`/`parseFormats` we seeded on entry. Leaving them
        // in place breaks rendering for fresh rows: the new type's
        // formatter (`fmt::vformat`) treats `"%F %H:%M:%S"` as a
        // literal, so integer / float columns would render the format
        // string for every row. Existing `Timestamp` slots remain --
        // they keep formatting via `date::format` until the column is
        // rewritten -- so the user still sees their old timestamps,
        // just no longer auto-formatted into the new type's grammar.
        if (previousType == LogConfiguration::Type::Time)
        {
            mConfiguration.SetColumnPrintFormat(columnIndex, "{}");
            mConfiguration.SetColumnParseFormats(columnIndex, {});
        }

        // Transition away from a dict-backed type. If the column
        // currently has dictionary state, materialise every
        // `DictRef` back to an `OwnedString` exactly like
        // `DemoteColumnFromEnum` does -- but skip the
        // SetColumnType-to-String it bakes in, because the user
        // already chose the destination type.
        if (column.keys.empty())
        {
            break;
        }
        const KeyId canonical = mData.Keys().Find(column.keys.front());
        if (canonical == INVALID_KEY_ID)
        {
            break;
        }
        if (mEnumDictionaries.Find(canonical) != nullptr)
        {
            // Mark as Enumeration so `DemoteColumnFromEnum`'s
            // type-guard accepts the call, then restore the user's
            // pick. The intermediate write is invisible to anyone
            // outside this function; no `dataChanged` is emitted
            // here either way. Editor-driven, so don't record into
            // the batch-scoped demote list.
            const auto targetType = column.type;
            mConfiguration.SetColumnType(columnIndex, LogConfiguration::Type::Enumeration);
            DemoteColumnFromEnum(columnIndex, /*recordForBatch=*/false);
            mConfiguration.SetColumnType(columnIndex, targetType);
        }
        mEnumColumnHealth.erase(canonical);
        mLevelRankCache.erase(canonical);
        break;
    }
    }
}

bool LogTable::EncodeColumnRange(
    std::span<const KeyId> aliasKeys, size_t rowBegin, size_t rowEnd, EnumColumnHealth &health
)
{
    if (aliasKeys.empty())
    {
        return true;
    }
    EnumDictionary *dict = nullptr;
    {
        EnumDictionary &ref = mEnumDictionaries.GetOrInsert(aliasKeys.front(), mEnumValueCap);
        dict = &ref;
    }
    auto &lines = mData.Lines();
    for (size_t row = rowBegin; row < rowEnd && row < lines.size(); ++row)
    {
        LogLine &line = lines[row];
        // At most one DictRef per row: aliases share the dictionary.
        bool encoded = false;
        bool sawLong = false;
        bool sawWrongType = false;
        bool alreadyEncoded = false;
        for (const KeyId id : aliasKeys)
        {
            internal::CompactLogValue *slot = line.FindCompactMutable(id);
            if (slot == nullptr)
            {
                continue;
            }
            if (slot->tag == internal::CompactTag::DictRef)
            {
                alreadyEncoded = true;
                break;
            }
            const auto bytes = line.PeekStringView(*slot);
            if (!bytes.has_value())
            {
                // Wrong-type slot in an expected enum column.
                sawWrongType = true;
                continue;
            }
            if (mEnumValueMaxLen != 0 && bytes->size() > mEnumValueMaxLen)
            {
                // Long value: accrues against the health budget.
                sawLong = true;
                continue;
            }
            const EnumValueId vid = dict->Insert(*bytes);
            if (vid == INVALID_ENUM_VALUE_ID)
            {
                // Hard dictionary cap; caller demotes immediately.
                return false;
            }
            *slot = internal::CompactLogValue::MakeDictRef(vid);
            encoded = true;
            break;
        }
        if (alreadyEncoded)
        {
            continue;
        }
        if (encoded)
        {
            ++health.totalSlots;
        }
        else if (sawLong)
        {
            ++health.totalSlots;
            ++health.longValueSlots;
        }
        else if (sawWrongType)
        {
            ++health.totalSlots;
            ++health.wrongTypeSlots;
        }
    }
    return true;
}

bool LogTable::EncodeColumnRangeAsEnum(
    const LogConfiguration::Column &column, size_t rowBegin, size_t rowEnd, EnumColumnHealth &health
)
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
    if (keyIds.empty())
    {
        return true;
    }
    return EncodeColumnRange(keyIds, rowBegin, rowEnd, health);
}

void LogTable::PromoteColumnToEnum(size_t columnIndex)
{
    if (columnIndex >= mConfiguration.Configuration().columns.size())
    {
        return;
    }
    {
        const auto &snapshot = mConfiguration.Configuration().columns[columnIndex];
        if (snapshot.type == LogConfiguration::Type::Enumeration || snapshot.type == LogConfiguration::Type::Level)
        {
            return;
        }
    }

    // Copy the small POD-ish fields we need so the rest of the function
    // doesn't depend on the live columns vector across mutations.
    const std::vector<std::string> columnKeys = mConfiguration.Configuration().columns[columnIndex].keys;
    const std::string headerKey = mConfiguration.Configuration().columns[columnIndex].header;

    mConfiguration.SetColumnType(columnIndex, LogConfiguration::Type::Enumeration);
    // Pre-create canonical dictionary and alias-wire so the encode hot
    // path can skip alias bookkeeping.
    KeyId canonicalKey = INVALID_KEY_ID;
    {
        std::optional<KeyId> canonical;
        for (const std::string &key : columnKeys)
        {
            const KeyId id = mData.Keys().GetOrInsert(key);
            if (!canonical.has_value())
            {
                (void)mEnumDictionaries.GetOrInsert(id, mEnumValueCap);
                canonical = id;
            }
            else
            {
                if (!mEnumDictionaries.Alias(*canonical, id))
                {
                    fmt::print(
                        stderr,
                        "[loglib] PromoteColumnToEnum: failed to alias key {} onto canonical {} for column '{}'\n",
                        static_cast<uint32_t>(id),
                        static_cast<uint32_t>(*canonical),
                        headerKey
                    );
                }
            }
        }
        if (canonical.has_value())
        {
            canonicalKey = *canonical;
        }
    }

    // Encode all existing rows; this seeds the health tracker. Hard cap
    // demotes immediately; the tolerance check catches an unscanned tail
    // whose shape does not actually match an enum.
    EnumColumnHealth &health = mEnumColumnHealth[canonicalKey];
    if (!EncodeColumnRangeAsEnum(mConfiguration.Configuration().columns[columnIndex], 0U, mData.Lines().size(), health))
    {
        DemoteColumnFromEnum(columnIndex);
        return;
    }
    if (health.ShouldDemote(ENUM_HEALTH_TOLERANCE_RATIO, ENUM_HEALTH_MIN_SAMPLES))
    {
        DemoteColumnFromEnum(columnIndex);
        return;
    }

    // Two-signal level detection: cheap key-name match first, then the
    // dictionary check inside `MaybePromoteToLevel`. The dictionary is
    // freshly filled here, so the check has all the data it needs;
    // subsequent batches only re-evaluate when the dictionary grows.
    if (std::ranges::any_of(columnKeys, IsLogLevelKey))
    {
        MaybePromoteToLevel(columnIndex);
    }
}

void LogTable::DemoteColumnFromEnum(size_t columnIndex, bool recordForBatch)
{
    const auto &columns = mConfiguration.Configuration().columns;
    if (columnIndex >= columns.size())
    {
        return;
    }
    const auto &column = columns[columnIndex];
    if (column.type != LogConfiguration::Type::Enumeration && column.type != LogConfiguration::Type::Level)
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

    // Record the canonical KeyId before the registry erase below so
    // `LogModel`'s post-batch detector can scope its `Demoted` emit
    // even on the silent `Type::Any + autoDetect -> Enumeration ->
    // String` transition (no dict survives on either side of the
    // batch). Skipped for editor-driven demotes (see header doc): the
    // editor signals via `LogModel::ApplyColumnTypeEdit`, so adding
    // here would risk a double-emit on the next `AppendBatch`.
    if (recordForBatch && !keyIds.empty())
    {
        mLastBatchDemotedKeys.push_back(keyIds.front());
    }

    auto &lines = mData.Lines();

    const auto demoteStart = std::chrono::steady_clock::now();
    size_t convertedSlots = 0;

    if (!keyIds.empty())
    {
        // Resolve via canonical key; aliases share the entry. Bytes from
        // `Resolve` are stable until `Erase` below; `AppendOwnedBytes`
        // copies them.
        const EnumDictionary *dict = mEnumDictionaries.Find(keyIds.front());
        for (auto &line : lines)
        {
            LineSource *source = line.Source();
            const size_t lineId = line.LineId();
            for (const KeyId id : keyIds)
            {
                internal::CompactLogValue *slot = line.FindCompactMutable(id);
                if (slot == nullptr || slot->tag != internal::CompactTag::DictRef)
                {
                    continue;
                }
                std::string_view bytes;
                if (dict != nullptr)
                {
                    bytes = dict->Resolve(static_cast<EnumValueId>(slot->payload));
                }
                if (source != nullptr)
                {
                    const uint64_t offset = source->AppendOwnedBytes(lineId, bytes);
                    *slot = internal::CompactLogValue::MakeOwnedString(offset, static_cast<uint32_t>(bytes.size()));
                }
                else
                {
                    *slot = internal::CompactLogValue::MakeMonostate();
                }
                ++convertedSlots;
            }
        }
        mEnumDictionaries.Erase(keyIds.front());
    }

    // Route to `Type::String` (terminal): a dictionary or
    // health-budget breach is a string-cardinality conclusion, not
    // "we don't know".
    mConfiguration.SetColumnType(columnIndex, LogConfiguration::Type::String);

    // Health is keyed on the canonical `KeyId` (same key the
    // registry and rank cache use), so the erase doesn't have to
    // round-trip through `column.header`.
    if (!keyIds.empty())
    {
        mEnumColumnHealth.erase(keyIds.front());
        // Drop the rank cache when a Level column demotes. Keyed by
        // the same canonical `KeyId` the registry uses.
        mLevelRankCache.erase(keyIds.front());
    }

    const auto demoteElapsed =
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - demoteStart);
    if (demoteElapsed.count() > DEMOTE_TELEMETRY_LOG_THRESHOLD_US)
    {
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

void LogTable::MaybePromoteToLevel(size_t columnIndex)
{
    if (columnIndex >= mConfiguration.Configuration().columns.size())
    {
        return;
    }
    // Re-read the column on each access so the function tolerates the
    // `SetColumnType` mutation below.
    const auto snapshotColumn = [this, columnIndex]() -> const LogConfiguration::Column & {
        return mConfiguration.Configuration().columns[columnIndex];
    };
    if (snapshotColumn().type != LogConfiguration::Type::Enumeration)
    {
        return;
    }
    // User-pinned `Enumeration` columns stay `Enumeration`; the user
    // explicitly opted out of further auto-promotion.
    if (!snapshotColumn().autoDetect)
    {
        return;
    }

    // Signal 1: at least one configured key must match the level alias list.
    if (!std::ranges::any_of(snapshotColumn().keys, IsLogLevelKey))
    {
        return;
    }

    // Signal 2: dictionary content. Aliases share the canonical entry,
    // so the first resolved key is enough.
    KeyId canonical = INVALID_KEY_ID;
    for (const std::string &key : snapshotColumn().keys)
    {
        canonical = mData.Keys().Find(key);
        if (canonical != INVALID_KEY_ID)
        {
            break;
        }
    }
    if (canonical == INVALID_KEY_ID)
    {
        return;
    }
    const EnumDictionary *dict = mEnumDictionaries.Find(canonical);
    if (dict == nullptr || dict->Size() == 0)
    {
        return;
    }

    // Dict-weighted tolerance: at most one unrecognized entry per
    // `LEVEL_DICT_TOLERANCE_RATIO` canonical ones, with at least one
    // canonical entry. Honours any per-column `levelMapping` overrides.
    size_t canonicalEntries = 0;
    size_t unrecognizedEntries = 0;
    {
        const auto &column = snapshotColumn();
        for (size_t valueId = 0; valueId < dict->Size(); ++valueId)
        {
            const std::string_view bytes = dict->Resolve(static_cast<EnumValueId>(valueId));
            if (ResolveLevel(bytes, column.levelMapping).has_value())
            {
                ++canonicalEntries;
            }
            else
            {
                ++unrecognizedEntries;
            }
        }
    }
    if (canonicalEntries == 0 || unrecognizedEntries * LEVEL_DICT_TOLERANCE_RATIO > canonicalEntries)
    {
        return;
    }

    mConfiguration.SetColumnType(columnIndex, LogConfiguration::Type::Level);
    RefreshLevelRankCache(columnIndex);
}

void LogTable::RefreshLevelRankCache(size_t columnIndex)
{
    const auto &columns = mConfiguration.Configuration().columns;
    if (columnIndex >= columns.size())
    {
        return;
    }
    const auto &column = columns[columnIndex];
    if (column.type != LogConfiguration::Type::Level)
    {
        return;
    }

    KeyId canonical = INVALID_KEY_ID;
    for (const std::string &key : column.keys)
    {
        canonical = mData.Keys().Find(key);
        if (canonical != INVALID_KEY_ID)
        {
            break;
        }
    }
    if (canonical == INVALID_KEY_ID)
    {
        // Column is `Type::Level` but no key has been observed yet.
        // Leave the cache empty; `LevelRankCache` returns nullptr until
        // the canonical key is registered.
        return;
    }
    const EnumDictionary *dict = mEnumDictionaries.Find(canonical);
    if (dict == nullptr)
    {
        mLevelRankCache[canonical] = {};
        return;
    }

    std::vector<LogLevel> &ranks = mLevelRankCache[canonical];
    // Append-only growth on existing entries; rebuild from scratch only
    // if the dictionary shrank (happens only on `Reset`). Callers that
    // need a re-read of cached entries against a new `levelMapping`
    // must clear the cache first (see `RefreshSnapshotEnumKeys`).
    if (ranks.size() > dict->Size())
    {
        ranks.clear();
    }
    ranks.reserve(dict->Size());
    for (size_t valueId = ranks.size(); valueId < dict->Size(); ++valueId)
    {
        const std::string_view bytes = dict->Resolve(static_cast<EnumValueId>(valueId));
        const std::optional<LogLevel> level = ResolveLevel(bytes, column.levelMapping);
        ranks.push_back(level.value_or(LogLevel::Unknown));
    }
}

std::optional<LogLevel> LogTable::GetLevelForRow(size_t row, size_t columnIndex) const noexcept
{
    const auto &columns = mConfiguration.Configuration().columns;
    if (columnIndex >= columns.size())
    {
        return std::nullopt;
    }
    const auto &column = columns[columnIndex];
    if (column.type != LogConfiguration::Type::Level || column.keys.empty())
    {
        return std::nullopt;
    }
    const KeyId canonical = mData.Keys().Find(column.keys.front());
    if (canonical == INVALID_KEY_ID)
    {
        return std::nullopt;
    }
    const auto cacheIt = mLevelRankCache.find(canonical);
    if (cacheIt == mLevelRankCache.end())
    {
        return std::nullopt;
    }
    const std::optional<EnumValueId> id = GetEnumValueId(row, columnIndex);
    if (!id.has_value())
    {
        return std::nullopt;
    }
    if (static_cast<size_t>(*id) >= cacheIt->second.size())
    {
        return std::nullopt;
    }
    const LogLevel level = cacheIt->second[static_cast<size_t>(*id)];
    if (level == LogLevel::Unknown)
    {
        return std::nullopt;
    }
    return level;
}

const std::vector<LogLevel> *LogTable::LevelRankCache(size_t columnIndex) const noexcept
{
    const auto &columns = mConfiguration.Configuration().columns;
    if (columnIndex >= columns.size())
    {
        return nullptr;
    }
    const auto &column = columns[columnIndex];
    if (column.type != LogConfiguration::Type::Level || column.keys.empty())
    {
        return nullptr;
    }
    const KeyId canonical = mData.Keys().Find(column.keys.front());
    if (canonical == INVALID_KEY_ID)
    {
        return nullptr;
    }
    const auto it = mLevelRankCache.find(canonical);
    if (it == mLevelRankCache.end())
    {
        return nullptr;
    }
    return &it->second;
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
