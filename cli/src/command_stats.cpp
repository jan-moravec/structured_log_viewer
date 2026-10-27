#include "commands.hpp"

#include "ansi.hpp"
#include "cli_filter.hpp"
#include "console.hpp"
#include "diagnostics.hpp"
#include "json_report.hpp"
#include "load_table.hpp"
#include "platform.hpp"
#include "session_config.hpp"
#include "text_table.hpp"
#include "text_util.hpp"

#include <loglib/exports/row_exporter.hpp>
#include <loglib/filter_expression.hpp>
#include <loglib/histogram_bucket_index.hpp>
#include <loglib/leaf_rule_compile.hpp>
#include <loglib/log_filter.hpp>
#include <loglib/log_level.hpp>
#include <loglib/log_value.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <format>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace slp
{

// Report structs have external linkage: glaze reflects member names
// through templates that MSVC cannot instantiate for internal types.
namespace reports
{
// NOLINTBEGIN(misc-use-internal-linkage)

/** @brief Rows at one level, with their share of the total in percent (one decimal). */
struct LevelCount
{
    std::string level;
    std::uint64_t count = 0;
    double percent = 0.0;
};

/** @brief `slp stats --by level`. */
struct LevelReport
{
    std::string by = "level";
    std::vector<std::string> sources;
    std::uint64_t rowCount = 0;
    std::uint64_t total = 0;
    std::uint64_t unresolvableLevelCount = 0;
    /** @brief Canonical levels that occur, lowest first, then `Unknown` when any row has no level. */
    std::vector<LevelCount> levels;
};

/** @brief Rows at one level within a time bucket. */
struct BucketLevelCount
{
    std::string level;
    std::uint64_t count = 0;
};

/** @brief One non-empty time bucket. */
struct TimeBucket
{
    std::int64_t bucketStartMicros = 0;
    std::int64_t bucketWidthMicros = 0;
    std::string bucketStartLocal;
    std::uint64_t total = 0;
    std::vector<BucketLevelCount> levels;
};

/** @brief `slp stats --by time`. */
struct TimeReport
{
    std::string by = "time";
    std::vector<std::string> sources;
    std::uint64_t rowCount = 0;
    std::string bucketSize;
    std::int64_t bucketWidthMicros = 0;
    std::vector<TimeBucket> buckets;
};

/** @brief Rows holding one value, with their share of rows that have the column. */
struct ValueCount
{
    std::string value;
    std::uint64_t count = 0;
    double percent = 0.0;
};

/** @brief `slp stats --by column`. */
struct ColumnReport
{
    std::string by = "column";
    std::vector<std::string> sources;
    std::uint64_t rowCount = 0;
    std::string column;
    std::uint64_t cardinality = 0;
    /** @brief The `--top` most frequent values, most frequent first, ties by value. */
    std::vector<ValueCount> top;
};

// NOLINTEND(misc-use-internal-linkage)

} // namespace reports

namespace
{

using reports::BucketLevelCount;
using reports::ColumnReport;
using reports::LevelCount;
using reports::LevelReport;
using reports::TimeBucket;
using reports::TimeReport;
using reports::ValueCount;

constexpr int DEFAULT_TEXT_BUCKET_BUDGET = 60;
constexpr int DEFAULT_STRUCTURED_BUCKET_BUDGET = 500;
constexpr int DEFAULT_TERMINAL_WIDTH = 80;
constexpr std::size_t MIN_BAR_WIDTH = 8;
constexpr std::string_view UNKNOWN_LEVEL = "Unknown";

// ---- helpers ----------------------------------------------------------------

/** @brief @p part as a percentage of @p whole, rounded to one decimal. */
[[nodiscard]] double Percent(std::uint64_t part, std::uint64_t whole) noexcept
{
    constexpr double TENTHS = 10.0;
    if (whole == 0)
    {
        return 0.0;
    }
    return std::round(100.0 * TENTHS * static_cast<double>(part) / static_cast<double>(whole)) / TENTHS;
}

[[nodiscard]] std::string FormatPercent(double value)
{
    return std::format("{:.1f}", value);
}

/** @brief The sources as typed, for text output. */
[[nodiscard]] std::string SourceLine(const InputOptions &input)
{
    if (input.sources.empty())
    {
        return "-";
    }
    std::vector<std::string> texts;
    texts.reserve(input.sources.size());
    for (const SourceSpec &source : input.sources)
    {
        texts.push_back(source.text);
    }
    return JoinComma(texts);
}

/** @brief Source file names only, so JSON stays stable across absolute paths. */
[[nodiscard]] std::vector<std::string> SourceNames(const InputOptions &input)
{
    std::vector<std::string> names;
    names.reserve(input.sources.size());
    for (const SourceSpec &source : input.sources)
    {
        names.push_back(std::filesystem::path(source.text).filename().string());
    }
    if (names.empty())
    {
        names.emplace_back("-");
    }
    return names;
}

[[nodiscard]] std::string LevelName(loglib::LogLevel level)
{
    return level == loglib::LogLevel::Unknown ? std::string(UNKNOWN_LEVEL)
                                              : std::string(loglib::CanonicalLevelName(level));
}

/** @brief ANSI style for a level written by `LevelName`, or empty. */
[[nodiscard]] std::string_view LevelStyle(std::string_view name)
{
    return LevelAnsiPrefix(loglib::ParseLevelName(name).value_or(loglib::LogLevel::Unknown));
}

/**
 * @brief Calls @p visit for each count in @p counts that is non-zero.
 *
 * Canonical levels come first, lowest first; `Unknown` comes last.
 */
template <typename Counts, typename Visit> void ForEachLevel(const Counts &counts, Visit visit)
{
    for (int value = static_cast<int>(loglib::LogLevel::Trace); value <= static_cast<int>(loglib::LogLevel::Fatal);
         ++value)
    {
        const auto level = static_cast<loglib::LogLevel>(value);
        if (const std::uint64_t count = counts[static_cast<std::size_t>(level)]; count != 0)
        {
            visit(level, count);
        }
    }
    if (const std::uint64_t unknown = counts[static_cast<std::size_t>(loglib::LogLevel::Unknown)]; unknown != 0)
    {
        visit(loglib::LogLevel::Unknown, unknown);
    }
}

/** @brief A bar of `#` scaled so @p maximum fills @p width. */
[[nodiscard]] std::string Bar(std::uint64_t value, std::uint64_t maximum, std::size_t width)
{
    if (maximum == 0)
    {
        return {};
    }
    const auto filled = static_cast<std::size_t>(
        static_cast<double>(value) * static_cast<double>(width) / static_cast<double>(maximum)
    );
    // NOLINTNEXTLINE(modernize-return-braced-init-list): braces would select the `initializer_list<char>` constructor.
    return std::string(filled, '#');
}

/** @brief Width left for a bar that starts after @p table's column @p barColumn. */
[[nodiscard]] std::size_t BarWidth(const Console &console, const TextTable &table, std::size_t barColumn)
{
    const auto terminal = static_cast<std::size_t>(TerminalWidth(console.Out(), DEFAULT_TERMINAL_WIDTH));
    const std::size_t used = table.WidthBefore(barColumn);
    return used + MIN_BAR_WIDTH < terminal ? terminal - used : MIN_BAR_WIDTH;
}

[[nodiscard]] std::string CsvLine(std::initializer_list<std::string_view> fields)
{
    std::string line;
    bool first = true;
    for (const std::string_view field : fields)
    {
        if (!first)
        {
            line.push_back(',');
        }
        first = false;
        loglib::exports::AppendCsvField(line, field);
    }
    line.push_back('\n');
    return line;
}

[[nodiscard]] std::optional<loglib::TimeStamp> TimeStampForRow(
    const loglib::LogTable &table, std::size_t row, std::size_t timeColumn
)
{
    const auto epochMicros = loglib::AsEpochMicroseconds(table.GetValue(row, timeColumn));
    if (!epochMicros.has_value())
    {
        return std::nullopt;
    }
    return loglib::TimeStamp{std::chrono::microseconds{*epochMicros}};
}

[[nodiscard]] loglib::LogLevel LevelForRow(const loglib::LogTable &table, std::size_t row, int levelColumn)
{
    if (levelColumn < 0)
    {
        return loglib::LogLevel::Unknown;
    }
    return table.GetLevelForRow(row, static_cast<std::size_t>(levelColumn)).value_or(loglib::LogLevel::Unknown);
}

// ---- --by level -------------------------------------------------------------

[[nodiscard]] LevelReport SummarizeLevels(
    const loglib::LogTable &table, const std::vector<std::size_t> &rows, int levelColumn, const InputOptions &input
)
{
    std::array<std::uint64_t, loglib::CANONICAL_LEVEL_COUNT + 1> counts{};
    for (const std::size_t row : rows)
    {
        ++counts[static_cast<std::size_t>(LevelForRow(table, row, levelColumn))];
    }

    LevelReport report;
    report.sources = SourceNames(input);
    report.rowCount = rows.size();
    report.total = rows.size();
    report.unresolvableLevelCount = counts[static_cast<std::size_t>(loglib::LogLevel::Unknown)];
    ForEachLevel(counts, [&report](loglib::LogLevel level, std::uint64_t count) {
        report.levels.push_back(
            LevelCount{.level = LevelName(level), .count = count, .percent = Percent(count, report.total)}
        );
    });
    return report;
}

[[nodiscard]] std::string RenderLevelText(const Console &console, const LevelReport &report, const InputOptions &input)
{
    const bool color = console.ColorFor(console.Out());
    TextTable table({"level", "count", "percent", "bar"});
    std::uint64_t maximum = 0;
    for (const LevelCount &level : report.levels)
    {
        maximum = std::max(maximum, level.count);
        table.AddRow(
            {level.level, std::to_string(level.count), FormatPercent(level.percent)},
            color ? LevelStyle(level.level) : std::string_view{}
        );
    }
    const std::size_t barWidth = BarWidth(console, table, 3);
    for (std::size_t row = 0; row < report.levels.size(); ++row)
    {
        table.SetCell(row, 3, Bar(report.levels[row].count, maximum, barWidth));
    }

    std::string out =
        std::format("source: {}\nspan: {} row(s)\ntotal: {}", SourceLine(input), report.rowCount, report.total);
    if (report.unresolvableLevelCount > 0)
    {
        out += std::format("  unresolvable: {}", report.unresolvableLevelCount);
    }
    out.push_back('\n');
    return out + table.Render();
}

[[nodiscard]] std::string RenderLevelCsv(const LevelReport &report)
{
    std::string out = std::format("by,level\nrowCount,{}\n", report.rowCount);
    out += CsvLine({"level", "count", "percent"});
    for (const LevelCount &level : report.levels)
    {
        out += CsvLine({level.level, std::to_string(level.count), FormatPercent(level.percent)});
    }
    return out;
}

// ---- --by time --------------------------------------------------------------

struct TimeHistogram
{
    loglib::HistogramBucketIndex index{loglib::HistogramBucketSize::OneMinute};
    std::optional<loglib::TimeStamp> minTs;
    std::optional<loglib::TimeStamp> maxTs;
};

[[nodiscard]] TimeHistogram BuildTimeHistogram(
    const loglib::LogTable &table,
    const std::vector<std::size_t> &rows,
    std::size_t timeColumn,
    int levelColumn,
    loglib::HistogramBucketSize bucketSize
)
{
    TimeHistogram result;
    result.index.SetBucketSize(bucketSize);
    for (const std::size_t row : rows)
    {
        const auto ts = TimeStampForRow(table, row, timeColumn);
        if (!ts.has_value())
        {
            continue;
        }
        if (!result.minTs.has_value() || *ts < *result.minTs)
        {
            result.minTs = *ts;
        }
        if (!result.maxTs.has_value() || *ts > *result.maxTs)
        {
            result.maxTs = *ts;
        }
        result.index.AddRow(*ts, LevelForRow(table, row, levelColumn));
    }
    return result;
}

[[nodiscard]] loglib::HistogramBucketSize ResolveBucketSize(
    const StatsOptions &options, bool structuredOutput, const TimeHistogram &histogram
)
{
    if (options.bucket)
    {
        return *options.bucket;
    }
    if (!histogram.minTs.has_value() || !histogram.maxTs.has_value())
    {
        return loglib::HistogramBucketSize::OneMinute;
    }
    int budget = structuredOutput ? DEFAULT_STRUCTURED_BUCKET_BUDGET : DEFAULT_TEXT_BUCKET_BUDGET;
    if (options.maxBuckets)
    {
        budget = static_cast<int>(*options.maxBuckets);
    }
    return loglib::HistogramBucketIndex::AutoBucketSize(*histogram.minTs, *histogram.maxTs, budget);
}

[[nodiscard]] TimeReport SummarizeTime(
    const TimeHistogram &histogram, const RuntimeContext &runtime, std::uint64_t rowCount, const InputOptions &input
)
{
    TimeReport report;
    report.sources = SourceNames(input);
    report.rowCount = rowCount;
    report.bucketSize = loglib::HistogramBucketSizeLabel(histogram.index.BucketSize());
    report.bucketWidthMicros = histogram.index.BucketWidth().count();
    const auto buckets = histogram.index.Buckets();
    for (std::size_t index = 0; index < buckets.size(); ++index)
    {
        if (buckets[index].Total() == 0)
        {
            continue;
        }
        const loglib::TimeStamp start = histogram.index.BucketStart(index);
        TimeBucket bucket{
            .bucketStartMicros = start.time_since_epoch().count(),
            .bucketWidthMicros = report.bucketWidthMicros,
            .bucketStartLocal = runtime.timeZone.FormatTimeStamp(start),
            .total = buckets[index].Total(),
            .levels = {},
        };
        ForEachLevel(buckets[index].counts, [&bucket](loglib::LogLevel level, std::uint64_t count) {
            bucket.levels.push_back(BucketLevelCount{.level = LevelName(level), .count = count});
        });
        report.buckets.push_back(std::move(bucket));
    }
    return report;
}

/** @brief Style of the most severe canonical level in @p bucket, or empty. */
[[nodiscard]] std::string_view MostSevereStyle(const TimeBucket &bucket)
{
    for (const BucketLevelCount &count : std::views::reverse(bucket.levels))
    {
        if (count.level != UNKNOWN_LEVEL)
        {
            return LevelStyle(count.level);
        }
    }
    return {};
}

[[nodiscard]] std::string RenderTimeText(
    const Console &console,
    const TimeReport &report,
    const TimeHistogram &histogram,
    const RuntimeContext &runtime,
    const InputOptions &input
)
{
    const bool color = console.ColorFor(console.Out());
    TextTable table({"bucketStartLocal", "total", "bar"});
    std::uint64_t maximum = 0;
    for (const TimeBucket &bucket : report.buckets)
    {
        maximum = std::max(maximum, bucket.total);
        table.AddRow({bucket.bucketStartLocal, std::to_string(bucket.total)});
    }
    const std::size_t barWidth = BarWidth(console, table, 2);
    for (std::size_t row = 0; row < report.buckets.size(); ++row)
    {
        std::string bar = Bar(report.buckets[row].total, maximum, barWidth);
        const std::string_view style = color ? MostSevereStyle(report.buckets[row]) : std::string_view{};
        if (!style.empty() && !bar.empty())
        {
            bar.insert(0, style);
            bar.append(ANSI_RESET);
        }
        table.SetCell(row, 2, std::move(bar));
    }

    std::string span = std::format("span: {} row(s)", report.rowCount);
    if (histogram.minTs.has_value() && histogram.maxTs.has_value())
    {
        span = std::format(
            "span: {} .. {} ({} row(s))",
            runtime.timeZone.FormatTimeStamp(*histogram.minTs),
            runtime.timeZone.FormatTimeStamp(*histogram.maxTs),
            report.rowCount
        );
    }
    return std::format(
               "source: {}\n{}\nbucket: {} ({} us)\n",
               SourceLine(input),
               span,
               report.bucketSize,
               report.bucketWidthMicros
           ) +
           table.Render();
}

[[nodiscard]] std::string RenderTimeCsv(const TimeReport &report)
{
    std::string out =
        std::format("by,time\nrowCount,{}\nbucketWidthMicros,{}\n", report.rowCount, report.bucketWidthMicros);
    out += CsvLine({"bucketStartMicros", "bucketWidthMicros", "bucketStartLocal", "total"});
    for (const TimeBucket &bucket : report.buckets)
    {
        out += CsvLine({
            std::to_string(bucket.bucketStartMicros),
            std::to_string(bucket.bucketWidthMicros),
            bucket.bucketStartLocal,
            std::to_string(bucket.total),
        });
    }
    return out;
}

// ---- --by column ------------------------------------------------------------

[[nodiscard]] ColumnReport SummarizeColumn(
    const loglib::LogTable &table,
    const std::vector<std::size_t> &rows,
    std::size_t columnIndex,
    std::uint64_t top,
    const InputOptions &input
)
{
    std::unordered_map<std::string, std::uint64_t> counts;
    std::uint64_t presentRows = 0;
    std::string buffer;
    for (const std::size_t row : rows)
    {
        if (!IsPresent(table.GetValue(row, columnIndex)))
        {
            continue;
        }
        ++presentRows;
        ++counts[std::string(table.GetValueOrFormatted(row, columnIndex, buffer))];
    }

    std::vector<std::pair<std::string, std::uint64_t>> ranked(counts.begin(), counts.end());
    std::ranges::sort(ranked, [](const auto &lhs, const auto &rhs) {
        return lhs.second != rhs.second ? lhs.second > rhs.second : lhs.first < rhs.first;
    });
    if (ranked.size() > top)
    {
        ranked.resize(static_cast<std::size_t>(top));
    }

    ColumnReport report;
    report.sources = SourceNames(input);
    report.rowCount = rows.size();
    report.column = table.Configuration().Configuration().columns[columnIndex].header;
    report.cardinality = counts.size();
    for (auto &[value, count] : ranked)
    {
        report.top.push_back(
            ValueCount{.value = std::move(value), .count = count, .percent = Percent(count, presentRows)}
        );
    }
    return report;
}

[[nodiscard]] std::string RenderColumnText(const ColumnReport &report, const InputOptions &input)
{
    TextTable table({"value", "count", "percent"});
    for (const ValueCount &value : report.top)
    {
        table.AddRow({value.value, std::to_string(value.count), FormatPercent(value.percent)});
    }
    return std::format(
               "source: {}\nspan: {} row(s)\ncolumn: {}\ncardinality: {}\n",
               SourceLine(input),
               report.rowCount,
               report.column,
               report.cardinality
           ) +
           table.Render();
}

[[nodiscard]] std::string RenderColumnCsv(const ColumnReport &report)
{
    std::string out = std::format("by,column\nrowCount,{}\n", report.rowCount);
    out += CsvLine({"column", report.column});
    out += CsvLine({"cardinality", std::to_string(report.cardinality)});
    out += CsvLine({"value", "count", "percent"});
    for (const ValueCount &value : report.top)
    {
        out += CsvLine({value.value, std::to_string(value.count), FormatPercent(value.percent)});
    }
    return out;
}

/** @brief A `--filter` that matches nothing in a non-empty source exits 1. */
[[nodiscard]] ExitCode StatsExitCode(const StatsOptions &options, std::uint64_t sourceRows, std::size_t filteredRows)
{
    if (sourceRows != 0 && filteredRows == 0 && !options.filter.filters.empty())
    {
        return ExitCode::Empty;
    }
    return ExitCode::Success;
}

} // namespace

ExitCode RunStats(
    const StatsOptions &options, const GlobalOptions &global, Console &console, const RuntimeContext &runtime
)
{
    auto loaded = LoadTable(global, options.input, console, runtime);
    if (!loaded)
    {
        return console.Report(loaded.error());
    }

    loglib::LogTable &table = loaded->table;
    auto session = ApplySessionConfiguration(options.filter, ViewOptions{}, table, runtime, StatsOptions::NAME);
    if (!session)
    {
        return console.Report(session.error());
    }

    const auto compiled =
        loglib::CompileExpression(session->expression, table.Configuration().Configuration().columns, &table);
    const std::vector<std::size_t> rows = loglib::FilterAcceptedRows(table, compiled);

    if (const ExitCode exitCode = StatsExitCode(options, table.RowCount(), rows.size()); exitCode != ExitCode::Success)
    {
        return exitCode;
    }

    const auto &columns = table.Configuration().Configuration().columns;
    const int levelColumn = FirstColumnOfType(columns, loglib::ColumnType::Level);

    switch (options.by)
    {
    case StatsAxis::Level:
    {
        const LevelReport report = SummarizeLevels(table, rows, levelColumn, options.input);
        switch (options.outputFormat)
        {
        case ReportFormat::Json:
            return console.Print(WriteJsonReport(report));
        case ReportFormat::Csv:
            return console.Print(RenderLevelCsv(report));
        case ReportFormat::Text:
            return console.Print(RenderLevelText(console, report, options.input));
        }
        return ExitCode::Success;
    }
    case StatsAxis::Time:
    {
        const int timeIndex = FirstColumnOfType(columns, loglib::ColumnType::Time);
        if (timeIndex < 0)
        {
            return console.Report(console.Fail(ExitCode::Usage, "this source has no time column for --by time."));
        }
        const auto timeColumn = static_cast<std::size_t>(timeIndex);
        const bool structured = options.outputFormat != ReportFormat::Text;
        const TimeHistogram probe =
            BuildTimeHistogram(table, rows, timeColumn, levelColumn, loglib::HistogramBucketSize::OneMinute);
        const TimeHistogram histogram =
            BuildTimeHistogram(table, rows, timeColumn, levelColumn, ResolveBucketSize(options, structured, probe));
        const TimeReport report = SummarizeTime(histogram, runtime, rows.size(), options.input);
        switch (options.outputFormat)
        {
        case ReportFormat::Json:
            return console.Print(WriteJsonReport(report));
        case ReportFormat::Csv:
            return console.Print(RenderTimeCsv(report));
        case ReportFormat::Text:
            return console.Print(RenderTimeText(console, report, histogram, runtime, options.input));
        }
        return ExitCode::Success;
    }
    case StatsAxis::Column:
    {
        const std::string name = options.column.value_or(std::string{});
        const int columnIndex = FindColumnByName(columns, name);
        if (columnIndex < 0)
        {
            return console.Report(console.Fail(
                ExitCode::Usage,
                std::format("unresolvable column '{}'; available columns are {}.", name, AvailableColumnList(columns))
            ));
        }
        const ColumnReport report =
            SummarizeColumn(table, rows, static_cast<std::size_t>(columnIndex), options.top, options.input);
        switch (options.outputFormat)
        {
        case ReportFormat::Json:
            return console.Print(WriteJsonReport(report));
        case ReportFormat::Csv:
            return console.Print(RenderColumnCsv(report));
        case ReportFormat::Text:
            return console.Print(RenderColumnText(report, options.input));
        }
        return ExitCode::Success;
    }
    }
    return ExitCode::Success;
}

} // namespace slp
