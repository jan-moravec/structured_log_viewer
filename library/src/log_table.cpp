#include "loglib/log_table.hpp"

#include "loglib/file_line_source.hpp"
#include "loglib/internal/compact_log_value.hpp"
#include "loglib/log_processing.hpp"

#include <date/date.h>
#include <date/tz.h>
#include <fmt/format.h>

#include <algorithm>
#include <array>
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

/// Minimum rows observed in static-parse mode before a `Type::unknown`
/// column can be promoted to enumeration (large files: dampens the
/// cost of false-positive promote/demote cycles when scanning millions
/// of rows).
constexpr size_t ENUM_PROMOTION_MIN_ROWS = 256;

/// Tighter promotion threshold (static mode) for columns whose
/// canonical key matches one of the well-known enum names. Modeled on
/// hl's hard-coded predefined fields: `level` / `severity` / `status`
/// etc. carry low-cardinality enums in real-world logs, and the cost
/// of being wrong is bounded.
constexpr size_t ENUM_PROMOTION_MIN_ROWS_WELL_KNOWN = 16;

/// Stream-mode promotion threshold (file tail / UDP / stdin). Streams
/// produce data in small batches and the user is watching live, so
/// promote eagerly. The dictionary cap (default 64) and length cap
/// still protect against false positives; the cardinality bail does
/// not apply in stream mode (meaningless at low row counts).
///
/// Trade-off: at threshold = 2, any column whose first 2 rows are
/// strings is promoted. A wide log with several free-form string
/// columns (e.g. `message`, `request_id`, `trace_id`) will pay an
/// encode-then-materialise round trip per column once distinct
/// values exceed the cap. The length cap catches most of these on
/// first sight, and the demote path is correct, so we accept the
/// churn in exchange for getting enum filter chips in the UI within
/// the first couple of rows. Bumping this constant trades UX
/// responsiveness for fewer promote/demote cycles.
constexpr size_t STREAM_PROMOTION_MIN_ROWS = 2;

/// Static-mode cardinality bail: kill any candidate whose
/// distinct/observed ratio exceeds this threshold once
/// `rowsObserved >= 2 * promotionMinRows`. A column still adding new
/// strings every fourth row is unlikely to ever stabilise as a finite
/// enum; the permanently-killed type transition short-circuits future
/// candidate scans.
constexpr double ENUM_CARDINALITY_BAIL_RATIO = 0.25;

/// Maximum fraction of over-cap-length or wrong-type observations a
/// column may carry before the auto-detector demotes it back to
/// `Type::string`. Replaces the pre-1.7 single-strike length-cap kill
/// (which was too eager: one stray long line in an otherwise
/// well-shaped enum column locked the column out of promotion for the
/// whole session). Applies uniformly to candidate columns and to
/// active `Type::enumeration` columns regardless of provenance —
/// user-pinned columns are honoured up to the same tolerance, then
/// demoted when the noise exceeds 5%.
constexpr double ENUM_HEALTH_TOLERANCE_RATIO = 0.05;

/// Minimum sample size before the tolerance ratio is consulted. Avoids
/// demoting on a single observation where a 1/1 over-cap ratio would
/// trip the threshold. Works for both the candidate scan
/// (`presenceCount`) and the active-column health (`totalSlots`).
constexpr size_t ENUM_HEALTH_MIN_SAMPLES = 20;

/// Routes a no-string-bail tracker to a concrete numeric / any type
/// based on `intObservations` / `uintObservations` / `doubleObservations`.
/// Mirrors the per-batch and finalize flows. See the
/// `LogConfiguration::Type` docblock.
///
/// `intObservations` and `uintObservations` are tracked separately so
/// future numeric widgets can differentiate signed from unsigned, but
/// today the routing collapses both into `Type::integer`. Mixing any
/// integer kind with a double routes to `Type::number`.
LogConfiguration::Type RouteNoStringBail(
    size_t intObservations, size_t uintObservations, size_t doubleObservations
) noexcept
{
    const bool sawIntegral = intObservations > 0 || uintObservations > 0;
    const bool sawDouble = doubleObservations > 0;
    if (sawIntegral && sawDouble)
    {
        return LogConfiguration::Type::number;
    }
    if (sawIntegral)
    {
        return LogConfiguration::Type::integer;
    }
    if (sawDouble)
    {
        return LogConfiguration::Type::floating;
    }
    return LogConfiguration::Type::any;
}

/// Canonical-key strings (case-insensitive match) recognised as
/// well-known enum columns. Kept short and conservative — the cost
/// of a false positive is a promote/demote cycle on the first
/// over-cap value, so we only list keys that are overwhelmingly
/// enum-shaped in practice.
///
/// A few entries (`module`, `service`, `component`, `region`,
/// `category`, `kind`) are *not* always low-cardinality — Python
/// module names, microservice meshes, or geo-regions can blow past
/// the default 64-value cap. We accept that cost knowingly: the
/// false-positive path is bounded by the dictionary cap and routes
/// through `DemoteColumnFromEnum` -> `Type::string` (terminal), so
/// the column self-corrects within one batch and never oscillates.
/// The win on real-world logs where these keys ARE small enums
/// (the common case) outweighs a one-time demote on the rest.
constexpr std::array<std::string_view, 13> WELL_KNOWN_ENUM_KEYS = {
    "level",
    "severity",
    "loglevel",
    "log.level",
    "status",
    "kind",
    "category",
    "env",
    "environment",
    "region",
    "service",
    "component",
    "module",
};

bool EqualsIgnoreCaseAscii(std::string_view a, std::string_view b) noexcept
{
    if (a.size() != b.size())
    {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i)
    {
        const unsigned char ca = static_cast<unsigned char>(a[i]);
        const unsigned char cb = static_cast<unsigned char>(b[i]);
        const unsigned char la = (ca >= 'A' && ca <= 'Z') ? static_cast<unsigned char>(ca + ('a' - 'A')) : ca;
        const unsigned char lb = (cb >= 'A' && cb <= 'Z') ? static_cast<unsigned char>(cb + ('a' - 'A')) : cb;
        if (la != lb)
        {
            return false;
        }
    }
    return true;
}

bool IsWellKnownEnumKey(std::string_view canonicalKey) noexcept
{
    for (const std::string_view candidate : WELL_KNOWN_ENUM_KEYS)
    {
        if (EqualsIgnoreCaseAscii(canonicalKey, candidate))
        {
            return true;
        }
    }
    return false;
}

bool IsEnumPassEligible(LogConfiguration::Type type) noexcept
{
    // `unknown` -> candidate scan; `enumeration` -> per-batch encode.
    // Every other type is terminal and skipped by the enum pass.
    return type == LogConfiguration::Type::unknown || type == LogConfiguration::Type::enumeration;
}

} // namespace

bool LogTable::EnumColumnHealth::ShouldDemote(double tolerance, size_t minSamples) const noexcept
{
    if (totalSlots < minSamples)
    {
        return false;
    }
    // Sum in `double` to dodge any `size_t` overflow if the budgets
    // ever grow pathological (the addition is otherwise safe today,
    // but the type allows a wrap that the threshold check could not
    // detect).
    const double bad = static_cast<double>(longValueSlots) + static_cast<double>(wrongTypeSlots);
    return bad > tolerance * static_cast<double>(totalSlots);
}

void LogTable::EnumCandidateTracker::Observe(std::string_view bytes)
{
    // Caller increments `presenceCount` and `rowsObserved` for every
    // row scanned (present or absent); this method only handles the
    // string-typed slot path.
    if (killed)
    {
        return;
    }
    if (valueMaxLen != 0 && bytes.size() > valueMaxLen)
    {
        // Long values accrue against a tolerance budget instead of
        // killing on first sight. The candidate stays alive while
        // the over-cap fraction stays under
        // `ENUM_HEALTH_TOLERANCE_RATIO`; once we have enough samples
        // we kill if the budget is blown. Min-sample gate avoids
        // demoting a column whose first observation happens to be a
        // single long line.
        ++longValueCount;
        if (presenceCount >= ENUM_HEALTH_MIN_SAMPLES &&
            static_cast<double>(longValueCount) > ENUM_HEALTH_TOLERANCE_RATIO * static_cast<double>(presenceCount))
        {
            killed = true;
            values = {};
            size = 0;
        }
        return;
    }
    // Linear membership check over the flat values vector. At
    // `cap=64` the working set fits comfortably in L1 and most
    // observations hit existing values early in the scan.
    for (const std::string &existing : values)
    {
        if (existing == bytes)
        {
            return;
        }
    }
    if (size >= cap)
    {
        // (cap+1)th distinct value: hard cap (no tolerance — the
        // dictionary itself cannot grow past `MAX_ENUM_VALUES` slots).
        // Drop the buffer; caller flips the column type to
        // `Type::string` so the next batch skips it.
        killed = true;
        values = {};
        size = 0;
        return;
    }
    values.emplace_back(bytes);
    ++size;
}

LogTable::LogTable(LogData data, LogConfigurationManager configuration)
    : mData(std::move(data)), mConfiguration(std::move(configuration))
{
    RewireSourceRegistries();
    RefreshSnapshotEnumKeys();
    RefreshColumnKeyIds();
    // Encode pre-configured enum columns and run auto-detection on
    // `Type::unknown` ones over the loaded data. Static parse: the
    // finalize sweep then promotes any small-file candidates that
    // didn't reach the per-batch threshold.
    std::optional<size_t> firstBackfilled;
    std::optional<size_t> lastBackfilled;
    RunEnumPassForAppendBatch(0U, firstBackfilled, lastBackfilled);
    FinalizeAutoDetection();
}

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
      mLastBackfillRange(std::move(other.mLastBackfillRange))
{
    other.mIsStreaming = false;
    // Sources cached a pointer to `other.mEnumDictionaries`; rebind.
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
    // Sources cached a pointer to `other.mEnumDictionaries`; rebind.
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
    RewireSourceRegistries();
    // Order matches the constructor: snapshot the enum keys first so
    // `GetOrInsert` registers any not-yet-seen `Type::enumeration`
    // keys into `mData.Keys()`, then resolve column keys against the
    // now-complete `KeyIndex`. Resolving first would leave
    // `mColumnKeyIds` populated with `INVALID_KEY_ID` for columns
    // whose configured keys had not yet appeared in the data, and
    // the immediately-following enum pass would then skip them.
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
    mIsStreaming = false;
    mLastBackfillRange.reset();
    RefreshColumnKeyIds();
}

void LogTable::BeginStreaming(std::unique_ptr<LineSource> source)
{
    mLastBackfillRange.reset();
    // Eager promotion + no cardinality bail until `FinalizeAutoDetection`.
    mIsStreaming = true;

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
    mEnumColumnHealth.clear();
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

    // Splice the new source in; lines and keys are preserved so prior
    // rows stay visible and KeyIds line up across files.
    source->SetEnumDictionaries(&mEnumDictionaries);
    mData.Sources().push_back(std::move(source));
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
    // Pre-create empty dictionaries for every configured enum column
    // so the encode pass treats their slots as already-promoted
    // targets. Multi-key columns share one canonical dictionary via
    // `Alias`. Idempotent: snapshot, promotion, and re-promotion
    // paths all route through this same canonical-key + alias
    // pattern so `EncodeColumnRange`'s hot loop can skip the per-row
    // alias bookkeeping.
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

    // Phase 1.3: rebuild active/candidate index lists once and bail
    // out when both are empty. Typical narrow logs with no enum-shaped
    // columns now skip the per-row work entirely. The type itself
    // gates candidacy: only `Type::unknown` columns are scanned;
    // every other type (including the terminal `string` / `integer` /
    // `floating` / `number` / `any` produced by previous bail paths)
    // is skipped automatically.
    //
    // Active (`Type::enumeration`) and candidate (`Type::unknown`)
    // columns are handled inline in a single column walk. Empty
    // alias-id sets are filtered after `resolveKeys` rather than
    // pre-bucketed, since each column owns a tiny alias set
    // (almost always 1) and the duplicated work is below noise.

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

    // Function-local scratch: reused across the per-column loop and
    // freed at function exit. Heap-allocated once on first push;
    // typical alias counts are 1, so the buffer never grows.
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
        if (!IsEnumPassEligible(column.type))
        {
            continue;
        }
        if (columnIndex >= mColumnKeyIds.size())
        {
            continue;
        }

        // Active enum column: encode pre-existing / pre-configured
        // enum columns over the appended slice. Length cap and
        // wrong-type observations accrue against the column's
        // health budget (`mEnumColumnHealth`); a hard dictionary-cap
        // overflow demotes immediately. The percentile policy
        // applies uniformly here regardless of whether the column
        // was auto-promoted or user-pinned.
        if (column.type == LogConfiguration::Type::enumeration)
        {
            resolveKeys(columnIndex);
            if (resolvedKeys.empty())
            {
                continue;
            }
            EnumColumnHealth &health = mEnumColumnHealth[column.header];
            if (!EncodeColumnRange(resolvedKeys, oldLineCount, totalRows, health))
            {
                DemoteColumnFromEnum(columnIndex);
                recordBackfill(columnIndex);
                continue;
            }
            if (health.ShouldDemote(ENUM_HEALTH_TOLERANCE_RATIO, ENUM_HEALTH_MIN_SAMPLES))
            {
                DemoteColumnFromEnum(columnIndex);
                recordBackfill(columnIndex);
            }
            continue;
        }

        if (column.keys.empty())
        {
            continue;
        }

        // Candidate scan. Tracker keyed on `column.header` so an
        // alias-list reorder cannot orphan the running counters. On
        // bail / kill the column type is flipped to a terminal
        // (`string` / `integer` / `floating` / `number` / `any`), which
        // removes it from `IsEnumPassEligible` for the next batch --
        // the type itself enforces kill-once-stay-killed.
        const std::string &trackerKey = column.header;
        // Well-known threshold consults every JSON field name in
        // the alias list (not just the first), so that reordering
        // `column.keys` to put a vendor-specific synonym first
        // (`["log_level", "level"]`) does not drop the column out
        // of the well-known fast path.
        const bool wellKnown = std::any_of(column.keys.begin(), column.keys.end(), [](const std::string &key) {
            return IsWellKnownEnumKey(key);
        });
        const size_t promotionMinRows =
            mIsStreaming ? STREAM_PROMOTION_MIN_ROWS
                         : (wellKnown ? ENUM_PROMOTION_MIN_ROWS_WELL_KNOWN : ENUM_PROMOTION_MIN_ROWS);

        resolveKeys(columnIndex);
        if (resolvedKeys.empty())
        {
            continue;
        }

        auto trackerIt = mEnumTrackers.find(trackerKey);
        if (trackerIt == mEnumTrackers.end())
        {
            trackerIt = mEnumTrackers.emplace(trackerKey, EnumCandidateTracker{mEnumValueCap, mEnumValueMaxLen}).first;
        }
        EnumCandidateTracker &tracker = trackerIt->second;

        // Phase 1.5: peek string bytes directly off the slot — skips
        // both the LogValue variant construction and the AsStringView
        // alternative-check on numeric / monostate / DictRef slots.
        //
        // Phase 2.1 sampled scan: once we have enough presences to
        // make a promote decision (size > 0 && presenceCount >=
        // promotionMinRows), stop scanning rows for this column in
        // this batch.
        //
        // `presenceCount` (slot present, any tag) is decoupled from
        // `rowsObserved` (loop progress) so a sparse column with
        // leading missing rows does not get killed by the no-string
        // bail before its strings show up.
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
            }
            if (tracker.killed)
            {
                break;
            }
            // Sampled scan: have we seen enough strings to decide?
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
            // Length-cap tolerance exceeded or hard dict cap hit:
            // column is too varied to enumerate. Route to `string`
            // so the next batch skips it and so save/load preserves
            // the conclusion.
            mConfiguration.SetColumnType(columnIndex, LogConfiguration::Type::string);
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

        // Bail on candidates that are unlikely to ever stabilise.
        // Static mode: applies the cardinality bail; stream mode skips
        // it (low row counts make the ratio meaningless and the user
        // is watching live).
        if (tracker.rowsObserved >= scanCap)
        {
            // Skip the no-string bail entirely if the column has
            // never appeared yet -- a sparse column whose first
            // presence falls past the scanCap window deserves a
            // chance in a later batch. We also reset `rowsObserved`
            // so the next batch gets a fresh scanCap budget instead
            // of bailing immediately on its first row.
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
                    RouteNoStringBail(tracker.intObservations, tracker.uintObservations, tracker.doubleObservations)
                );
                mEnumTrackers.erase(trackerIt);
            }
            else if (highCardinality)
            {
                mConfiguration.SetColumnType(columnIndex, LogConfiguration::Type::string);
                mEnumTrackers.erase(trackerIt);
            }
        }
    }
}

bool LogTable::FinalizeAutoDetection()
{
    // Permissive sweep over surviving candidate trackers. Runs at
    // end-of-static-parse (constructor / `Update`) and at end-of-stream
    // (`LogModel::EndStreaming(false)`). Idempotent: a re-entrant call
    // sees an empty `mEnumTrackers` and only flips `mIsStreaming` off.
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
        if (column.type != LogConfiguration::Type::unknown || column.keys.empty())
        {
            continue;
        }
        auto trackerIt = mEnumTrackers.find(column.header);
        if (trackerIt == mEnumTrackers.end())
        {
            continue;
        }
        const EnumCandidateTracker &tracker = trackerIt->second;
        if (tracker.killed)
        {
            // Defensive: per-batch loop normally erases killed
            // entries but the column is still `Type::unknown` here,
            // so honour the kill.
            mConfiguration.SetColumnType(columnIndex, LogConfiguration::Type::string);
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
                RouteNoStringBail(tracker.intObservations, tracker.uintObservations, tracker.doubleObservations)
            );
            continue;
        }
        // Insufficient evidence (e.g. a single string row, or the
        // column has not appeared yet in any row): leave
        // `Type::unknown`. A future re-load with more rows can still
        // decide.
    }

    mEnumTrackers.clear();
    mIsStreaming = false;
    return promoted;
}

bool LogTable::EncodeColumnRange(
    std::span<const KeyId> aliasKeys, size_t rowBegin, size_t rowEnd, EnumColumnHealth &health
)
{
    if (aliasKeys.empty())
    {
        return true;
    }
    // Phase 1.4: dictionary + alias setup is hoisted to
    // `EnsureColumnDictionary` (snapshot + promotion). Here we just
    // look the canonical dictionary up — a hot-path-cheap hash hit.
    EnumDictionary *dict = nullptr;
    {
        EnumDictionary &ref = mEnumDictionaries.GetOrInsert(aliasKeys.front(), mEnumValueCap);
        dict = &ref;
    }
    auto &lines = mData.Lines();
    for (size_t row = rowBegin; row < rowEnd && row < lines.size(); ++row)
    {
        LogLine &line = lines[row];
        // At most one DictRef per row: aliases share the dictionary,
        // so encoding under the first matching key is enough.
        bool encoded = false;
        bool sawLong = false;
        bool sawWrongType = false;
        bool alreadyEncoded = false;
        for (const KeyId id : aliasKeys)
        {
            // Phase 1.2: single linear scan per slot — read tag,
            // peek bytes, write `DictRef` in place. Replaces the
            // old IsDictRef + GetValue + SetOrReplaceEnumDictRef triple
            // lookup.
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
                // Numeric / bool / timestamp / monostate slot in a
                // column we expected to be a string enum. Try the
                // next alias before charging the column's health
                // budget.
                sawWrongType = true;
                continue;
            }
            if (mEnumValueMaxLen != 0 && bytes->size() > mEnumValueMaxLen)
            {
                // Long values do not bail; they accrue against the
                // column's health budget (caller demotes when the
                // tolerance ratio is exceeded).
                sawLong = true;
                continue;
            }
            const EnumValueId vid = dict->Insert(*bytes);
            if (vid == INVALID_ENUM_VALUE_ID)
            {
                // Hard dictionary cap (no tolerance — the dict
                // cannot grow further). Caller demotes immediately.
                return false;
            }
            *slot = internal::CompactLogValue::MakeDictRef(vid);
            encoded = true;
            break;
        }
        if (alreadyEncoded)
        {
            // Already counted by a previous batch's `EncodeColumnRange`
            // pass; do not double-count.
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
        // Slot absent on every alias: not counted.
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
    // Phase 1.4: pre-create the canonical dictionary and wire alias
    // keys onto it so the encode hot path skips alias bookkeeping.
    {
        std::optional<KeyId> canonical;
        for (const std::string &key : columns[columnIndex].keys)
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
                        columns[columnIndex].header
                    );
                }
            }
        }
    }

    // The encode walk over all existing rows seeds the column health
    // tracker. A hard dict-cap overflow demotes immediately; the
    // tolerance check below catches a candidate whose un-scanned
    // tail had too many over-cap or wrong-type slots for the column
    // to be enum-shaped after all.
    const std::string headerKey = columns[columnIndex].header;
    EnumColumnHealth &health = mEnumColumnHealth[headerKey];
    if (!EncodeColumnRangeAsEnum(columns[columnIndex], 0U, mData.Lines().size(), health))
    {
        DemoteColumnFromEnum(columnIndex);
        return;
    }
    if (health.ShouldDemote(ENUM_HEALTH_TOLERANCE_RATIO, ENUM_HEALTH_MIN_SAMPLES))
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

    const auto demoteStart = std::chrono::steady_clock::now();
    size_t convertedSlots = 0;

    if (!keyIds.empty())
    {
        // Resolve the dictionary once via the canonical key; aliases
        // share the entry. Bytes from `Resolve` are stable until
        // `Erase` below, and `AppendOwnedBytes` copies them, so it's
        // safe to keep the registry entry alive across the loop.
        const EnumDictionary *dict = mEnumDictionaries.Find(keyIds.front());
        for (auto &line : lines)
        {
            LineSource *source = line.Source();
            const size_t lineId = line.LineId();
            for (const KeyId id : keyIds)
            {
                // Phase 1.2: single lookup per slot, in-place rewrite.
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

    // Demote routes the column to `Type::string`: dictionary overflow
    // (or health-tolerance breach) is a string-cardinality conclusion,
    // not a "we don't know" verdict. The terminal type also stops the
    // candidate pass from re-tracking it (see `IsEnumPassEligible`).
    mConfiguration.SetColumnType(columnIndex, LogConfiguration::Type::string);

    // Drop the column's health entry; it is meaningless once the
    // column is no longer `Type::enumeration`. A re-promotion (only
    // possible via a manual configuration edit) will start a fresh
    // budget.
    mEnumColumnHealth.erase(column.header);

    const auto demoteElapsed =
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - demoteStart);
    if (demoteElapsed.count() > 1000)
    {
        // Telemetry for the demote-cost benchmark; stderr keeps it
        // logger-free.
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
