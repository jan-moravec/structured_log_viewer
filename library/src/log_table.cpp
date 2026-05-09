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

/// Routes a no-string-bail tracker to a concrete numeric / any type
/// based on `intObservations` / `doubleObservations`. Mirrors the
/// per-batch and finalize flows. See the `LogConfiguration::Type`
/// docblock.
///
/// Note: `intObservations` collapses `Int64` and `UInt64` tags into
/// a single counter; the routing emits `Type::integer` for both.
/// That is fine while the only consumer is the default `"{}"`
/// rendering, which prints either kind correctly. Once numeric sort
/// or range-filter widgets land we will need a dedicated
/// `uintObservations` counter (or a richer terminal type) to
/// distinguish signed from unsigned formatting and overflow rules.
LogConfiguration::Type RouteNoStringBail(size_t intObservations, size_t doubleObservations) noexcept
{
    const bool sawInt = intObservations > 0;
    const bool sawDouble = doubleObservations > 0;
    if (sawInt && sawDouble)
    {
        return LogConfiguration::Type::number;
    }
    if (sawInt)
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

bool LogTable::EnumCandidateTracker::Observe(std::string_view bytes)
{
    ++rowsObserved;
    if (killed)
    {
        return false;
    }
    // Length cap (auto-discovered columns only): an overlong value
    // disqualifies the column on first sight. Long strings rarely
    // repeat enough to amortise the dictionary's heap cost.
    //
    // Single-strike kill is intentional. A column where one stray
    // long value lands in the candidate window is statistically
    // unlikely to be a small fixed enum: even one outlier suggests
    // the field is free-form. The verdict is permanent for the
    // session AND persists into the saved configuration as
    // `Type::string`, so a user who knows better can pin the column
    // to `Type::enumeration` (which ignores the length cap entirely)
    // instead. Loosening to a multi-strike rule would only delay the
    // same conclusion at the cost of more dict / heap traffic.
    if (valueMaxLen != 0 && bytes.size() > valueMaxLen)
    {
        killed = true;
        values = {};
        size = 0;
        return true;
    }
    // Linear membership check over the flat values vector. At
    // `cap=64` the working set fits comfortably in L1 and most
    // observations hit existing values early in the scan.
    for (const std::string &existing : values)
    {
        if (existing == bytes)
        {
            return false;
        }
    }
    if (size >= cap)
    {
        // (cap+1)th distinct value: column is no longer promotable.
        // Drop the buffer; caller flips the column type to
        // `Type::string` so the next batch skips it.
        killed = true;
        values = {};
        size = 0;
        return true;
    }
    values.emplace_back(bytes);
    ++size;
    return true;
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
    RefreshColumnKeyIds();
    // Must run after `mConfiguration.Update(data)` so any newly
    // grown enum columns get a registry entry before the encode pass.
    RefreshSnapshotEnumKeys();
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
        // enum columns over the appended slice. The length cap only
        // applies to the candidate scan (where the tracker still
        // might bail); once a column is promoted to `enumeration`,
        // the dictionary cap is the authoritative growth limit and
        // overflow routes through `DemoteColumnFromEnum`.
        if (column.type == LogConfiguration::Type::enumeration)
        {
            resolveKeys(columnIndex);
            if (resolvedKeys.empty())
            {
                continue;
            }
            if (!EncodeColumnRange(resolvedKeys, oldLineCount, totalRows, 0U))
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

        // Candidate scan. The tracker is keyed on the canonical key
        // string (stable across batches). On bail / kill the column
        // type is flipped to a terminal (`string` / `integer` /
        // `floating` / `number` / `any`), which removes it from
        // `IsEnumPassEligible` for the next batch -- the type itself
        // enforces kill-once-stay-killed.
        const std::string &trackerKey = column.keys.front();
        const bool wellKnown = IsWellKnownEnumKey(trackerKey);
        const size_t promotionMinRows = mIsStreaming
                                            ? STREAM_PROMOTION_MIN_ROWS
                                            : (wellKnown ? ENUM_PROMOTION_MIN_ROWS_WELL_KNOWN : ENUM_PROMOTION_MIN_ROWS);

        resolveKeys(columnIndex);
        if (resolvedKeys.empty())
        {
            continue;
        }

        auto trackerIt = mEnumTrackers.find(trackerKey);
        if (trackerIt == mEnumTrackers.end())
        {
            trackerIt = mEnumTrackers
                            .emplace(trackerKey, EnumCandidateTracker{mEnumValueCap, mEnumValueMaxLen})
                            .first;
        }
        EnumCandidateTracker &tracker = trackerIt->second;

        // Phase 1.5: peek string bytes directly off the slot — skips
        // both the LogValue variant construction and the AsStringView
        // alternative-check on numeric / monostate / DictRef slots.
        //
        // Phase 2.1 sampled scan: once we have enough observations to
        // make a promote decision (size > 0 && >= promotionMinRows),
        // stop scanning rows for this column in this batch.
        //
        // Numeric tag counting lets the no-string bail route the column
        // to `Type::integer` / `Type::floating` / `Type::number` instead
        // of the catch-all `Type::any`. We do a single `FindCompact`
        // walk per (row, key) and dispatch on the returned slot's tag,
        // rather than separate scans for the tag and the string bytes.
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
            if (slot != nullptr)
            {
                std::optional<std::string_view> bytes = line.PeekStringView(*slot);
                if (bytes.has_value())
                {
                    tracker.Observe(*bytes);
                }
                else
                {
                    ++tracker.rowsObserved;
                    if (slot->tag == internal::CompactTag::Int64 || slot->tag == internal::CompactTag::Uint64)
                    {
                        ++tracker.intObservations;
                    }
                    else if (slot->tag == internal::CompactTag::Double)
                    {
                        ++tracker.doubleObservations;
                    }
                }
            }
            else
            {
                ++tracker.rowsObserved;
            }
            if (tracker.killed)
            {
                break;
            }
            // Sampled scan: have we seen enough to decide?
            if (tracker.size > 0 && tracker.size <= mEnumValueCap && tracker.rowsObserved >= promotionMinRows)
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
            // Length cap or dict cap exceeded: column is too varied
            // to enumerate. Route to `string` so the next batch skips
            // it and so save/load preserves the conclusion.
            mConfiguration.SetColumnType(columnIndex, LogConfiguration::Type::string);
            mEnumTrackers.erase(trackerIt);
            continue;
        }

        if (tracker.size > 0 && tracker.size <= mEnumValueCap && tracker.rowsObserved >= promotionMinRows)
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
            const bool noStringSeen = tracker.size == 0;
            const bool highCardinality =
                !mIsStreaming && tracker.size > 0 &&
                static_cast<double>(tracker.size) >
                    ENUM_CARDINALITY_BAIL_RATIO * static_cast<double>(tracker.rowsObserved);
            if (noStringSeen)
            {
                mConfiguration.SetColumnType(
                    columnIndex, RouteNoStringBail(tracker.intObservations, tracker.doubleObservations)
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
        const std::string &trackerKey = column.keys.front();
        auto trackerIt = mEnumTrackers.find(trackerKey);
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
        if (tracker.size > 0 && tracker.size <= mEnumValueCap && tracker.rowsObserved >= 2)
        {
            PromoteColumnToEnum(columnIndex);
            promoted = true;
            continue;
        }
        if (tracker.size == 0 && tracker.rowsObserved > 0)
        {
            mConfiguration.SetColumnType(
                columnIndex, RouteNoStringBail(tracker.intObservations, tracker.doubleObservations)
            );
            continue;
        }
        // Insufficient evidence (e.g. a single string row): leave
        // `Type::unknown`. A future re-load with more rows can still
        // decide.
    }

    mEnumTrackers.clear();
    mIsStreaming = false;
    return promoted;
}

bool LogTable::EncodeColumnRange(std::span<const KeyId> aliasKeys, size_t rowBegin, size_t rowEnd, uint32_t maxLen)
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
        for (const KeyId id : aliasKeys)
        {
            // Phase 1.2: single linear scan per slot — read tag,
            // peek bytes, write `DictRef` in place. Replaces the
            // old IsDictRef + GetValue + SetEnumDictRef triple
            // lookup.
            internal::CompactLogValue *slot = line.FindCompactMutable(id);
            if (slot == nullptr)
            {
                continue;
            }
            if (slot->tag == internal::CompactTag::DictRef)
            {
                break;
            }
            const auto bytes = line.PeekStringView(*slot);
            if (!bytes.has_value())
            {
                continue;
            }
            if (maxLen != 0 && bytes->size() > maxLen)
            {
                // Auto-discovered column exceeded the per-value
                // length cap. Bail to demote — the column is
                // unlikely to be enum-shaped after all.
                return false;
            }
            const EnumValueId vid = dict->Insert(*bytes);
            if (vid == INVALID_ENUM_VALUE_ID)
            {
                return false;
            }
            *slot = internal::CompactLogValue::MakeDictRef(vid);
            break;
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
    if (keyIds.empty())
    {
        return true;
    }
    // No length cap on the encode path: the candidate scan already
    // enforced `mEnumValueMaxLen` via `EnumCandidateTracker::Observe`.
    // Once the column is `Type::enumeration`, the dictionary cap is
    // the authoritative growth limit and overflow routes through
    // `DemoteColumnFromEnum` -> `Type::string`. User-pinned
    // enumerations also benefit -- their long values are honoured.
    return EncodeColumnRange(keyIds, rowBegin, rowEnd, 0U);
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
                mEnumDictionaries.Alias(*canonical, id);
            }
        }
    }

    // The encode walk can still overflow the cap (the tracker only
    // saw a subset of rows); demote immediately if so.
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
    // is a string-cardinality conclusion, not a "we don't know"
    // verdict. The terminal type also stops the candidate pass from
    // re-tracking it (see `IsEnumPassEligible`).
    mConfiguration.SetColumnType(columnIndex, LogConfiguration::Type::string);

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
