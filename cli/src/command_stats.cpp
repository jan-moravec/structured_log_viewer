#include "commands.hpp"

#include "ansi.hpp"
#include "cli_filter.hpp"
#include "console.hpp"
#include "diagnostics.hpp"
#include "load_table.hpp"
#include "platform.hpp"
#include "session_config.hpp"
#include "text_util.hpp"

#include <loglib/filter_expression.hpp>
#include <loglib/histogram_bucket_index.hpp>
#include <loglib/leaf_rule_compile.hpp>
#include <loglib/log_filter.hpp>
#include <loglib/log_level.hpp>
#include <loglib/log_value.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <format>
#include <iomanip>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace slp
{

namespace
{

constexpr int DEFAULT_TEXT_BUCKET_BUDGET = 60;
constexpr int DEFAULT_STRUCTURED_BUCKET_BUDGET = 500;
constexpr int DEFAULT_TERMINAL_WIDTH = 80;

[[nodiscard]] std::string FormatPercent(double value)
{
    std::ostringstream out;
    out << std::fixed << std::setprecision(1) << value;
    return out.str();
}

[[nodiscard]] std::string FormatSources(const InputOptions &input)
{
    if (input.sources.empty())
    {
        return "-";
    }
    std::ostringstream out;
    for (std::size_t i = 0; i < input.sources.size(); ++i)
    {
        if (i != 0)
        {
            out << ", ";
        }
        out << input.sources[i].text;
    }
    return out.str();
}

/** @brief Basenames only so JSON stays stable across absolute source paths. */
[[nodiscard]] std::string FormatSourcesJson(const InputOptions &input)
{
    if (input.sources.empty())
    {
        return "-";
    }
    std::ostringstream out;
    for (std::size_t i = 0; i < input.sources.size(); ++i)
    {
        if (i != 0)
        {
            out << ", ";
        }
        out << std::filesystem::path(input.sources[i].text).filename().string();
    }
    return out.str();
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
    const auto level = table.GetLevelForRow(row, static_cast<std::size_t>(levelColumn));
    return level.value_or(loglib::LogLevel::Unknown);
}

[[nodiscard]] std::optional<loglib::HistogramBucketSize> ParseBucketToken(std::string_view token) noexcept
{
    if (token == "1s")
    {
        return loglib::HistogramBucketSize::OneSecond;
    }
    if (token == "10s")
    {
        return loglib::HistogramBucketSize::TenSeconds;
    }
    if (token == "1m")
    {
        return loglib::HistogramBucketSize::OneMinute;
    }
    if (token == "10m")
    {
        return loglib::HistogramBucketSize::TenMinutes;
    }
    if (token == "1h")
    {
        return loglib::HistogramBucketSize::OneHour;
    }
    if (token == "1d")
    {
        return loglib::HistogramBucketSize::OneDay;
    }
    return std::nullopt;
}

[[nodiscard]] int BucketBudget(const StatsOptions &options, bool structuredOutput)
{
    if (options.maxBuckets)
    {
        return static_cast<int>(*options.maxBuckets);
    }
    return structuredOutput ? DEFAULT_STRUCTURED_BUCKET_BUDGET : DEFAULT_TEXT_BUCKET_BUDGET;
}

struct LevelSummary
{
    std::array<std::uint64_t, loglib::CANONICAL_LEVEL_COUNT + 1> counts{};
    std::uint64_t total = 0;
    std::uint64_t unresolvable = 0;
};

[[nodiscard]] LevelSummary SummarizeLevels(
    const loglib::LogTable &table, const std::vector<std::size_t> &rows, int levelColumn
)
{
    LevelSummary summary;
    for (const std::size_t row : rows)
    {
        const loglib::LogLevel level = LevelForRow(table, row, levelColumn);
        const std::size_t slot = static_cast<std::size_t>(level);
        ++summary.counts[slot];
        ++summary.total;
        if (level == loglib::LogLevel::Unknown)
        {
            ++summary.unresolvable;
        }
    }
    return summary;
}

[[nodiscard]] std::string RenderLevelText(
    const Console &console, const LevelSummary &summary, const std::string &sources, const std::string &spanLine
)
{
    const bool color = console.ColorFor(console.Out());
    const int termWidth = TerminalWidth(console.Out(), DEFAULT_TERMINAL_WIDTH);

    std::vector<std::string> headers = {"level", "count", "percent", "bar"};
    std::vector<std::vector<std::string>> tableRows;
    tableRows.reserve(loglib::CANONICAL_LEVEL_COUNT + 1);

    const auto addRow = [&](loglib::LogLevel level) {
        const std::size_t slot = static_cast<std::size_t>(level);
        const std::uint64_t count = summary.counts[slot];
        if (count == 0 && level != loglib::LogLevel::Unknown)
        {
            return;
        }
        const double percent =
            summary.total == 0 ? 0.0 : (100.0 * static_cast<double>(count)) / static_cast<double>(summary.total);
        std::vector<std::string> row;
        row.emplace_back(std::string(loglib::CanonicalLevelName(level)));
        row.push_back(std::to_string(count));
        row.push_back(FormatPercent(percent));
        row.emplace_back("");
        tableRows.push_back(std::move(row));
    };

    for (int value = static_cast<int>(loglib::LogLevel::Trace); value <= static_cast<int>(loglib::LogLevel::Fatal);
         ++value)
    {
        addRow(static_cast<loglib::LogLevel>(value));
    }
    if (summary.unresolvable > 0)
    {
        addRow(loglib::LogLevel::Unknown);
    }

    std::size_t maxCount = 0;
    for (const auto &row : tableRows)
    {
        maxCount = std::max(maxCount, static_cast<std::size_t>(std::stoull(row[1])));
    }

    std::vector<std::size_t> widths(headers.size(), 0);
    for (std::size_t c = 0; c < headers.size(); ++c)
    {
        widths[c] = headers[c].size();
    }
    for (const auto &row : tableRows)
    {
        for (std::size_t c = 0; c < row.size(); ++c)
        {
            widths[c] = std::max(widths[c], row[c].size());
        }
    }

    const std::size_t fixedWidth = widths[0] + widths[1] + widths[2] + 6 + headers.size() - 1;
    const int barWidth = std::max(8, termWidth - static_cast<int>(fixedWidth));

    for (std::size_t r = 0; r < tableRows.size(); ++r)
    {
        const std::uint64_t count = std::stoull(tableRows[r][1]);
        const int filled =
            maxCount == 0 ? 0
                          : static_cast<int>((static_cast<double>(count) * barWidth) / static_cast<double>(maxCount));
        tableRows[r][3] = std::string(static_cast<std::size_t>(filled), '#');
    }

    std::ostringstream out;
    out << "source: " << sources << '\n';
    out << spanLine << '\n';
    out << "total: " << summary.total;
    if (summary.unresolvable > 0)
    {
        out << "  unresolvable: " << summary.unresolvable;
    }
    out << '\n';

    auto writeRow = [&](const std::vector<std::string> &cells, loglib::LogLevel levelForColor) {
        for (std::size_t c = 0; c < cells.size(); ++c)
        {
            if (c != 0)
            {
                out << "  ";
            }
            if (c == 0 && color)
            {
                out << LevelAnsiPrefix(levelForColor) << cells[c] << ANSI_RESET;
            }
            else
            {
                out << cells[c];
            }
            if (c + 1 < cells.size())
            {
                out << std::string(widths[c] - cells[c].size(), ' ');
            }
        }
        out << '\n';
    };

    writeRow(headers, loglib::LogLevel::Unknown);
    for (const auto &row : tableRows)
    {
        loglib::LogLevel level = loglib::LogLevel::Unknown;
        if (row[0] != "Unknown")
        {
            for (int value = static_cast<int>(loglib::LogLevel::Trace);
                 value <= static_cast<int>(loglib::LogLevel::Fatal);
                 ++value)
            {
                if (row[0] == std::string(loglib::CanonicalLevelName(static_cast<loglib::LogLevel>(value))))
                {
                    level = static_cast<loglib::LogLevel>(value);
                    break;
                }
            }
        }
        writeRow(row, level);
    }
    return out.str();
}

[[nodiscard]] std::string RenderLevelJson(
    const LevelSummary &summary, const std::string &sources, std::uint64_t rowCount
)
{
    std::string out = "{\n  \"by\": \"level\",\n  \"sources\": [";
    AppendJsonString(out, sources);
    out += "],\n  \"rowCount\": ";
    out += std::to_string(rowCount);
    out += ",\n  \"total\": ";
    out += std::to_string(summary.total);
    out += ",\n  \"unresolvableLevelCount\": ";
    out += std::to_string(summary.unresolvable);
    out += ",\n  \"levels\": [\n";

    bool first = true;
    for (int value = static_cast<int>(loglib::LogLevel::Trace); value <= static_cast<int>(loglib::LogLevel::Fatal);
         ++value)
    {
        const auto level = static_cast<loglib::LogLevel>(value);
        const std::uint64_t count = summary.counts[static_cast<std::size_t>(level)];
        if (count == 0)
        {
            continue;
        }
        if (!first)
        {
            out += ",\n";
        }
        first = false;
        const double percent =
            summary.total == 0 ? 0.0 : (100.0 * static_cast<double>(count)) / static_cast<double>(summary.total);
        out += "    {\"level\": ";
        AppendJsonString(out, loglib::CanonicalLevelName(level));
        out += ", \"count\": ";
        out += std::to_string(count);
        out += ", \"percent\": ";
        out += FormatPercent(percent);
        out += "}";
    }
    if (summary.unresolvable > 0)
    {
        if (!first)
        {
            out += ",\n";
        }
        const double percent = summary.total == 0 ? 0.0
                                                  : (100.0 * static_cast<double>(summary.unresolvable)) /
                                                        static_cast<double>(summary.total);
        out += "    {\"level\": ";
        AppendJsonString(out, "Unknown");
        out += ", \"count\": ";
        out += std::to_string(summary.unresolvable);
        out += ", \"percent\": ";
        out += FormatPercent(percent);
        out += "}";
    }
    out += "\n  ]\n}\n";
    return out;
}

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
    if (options.bucket != "auto")
    {
        const auto parsed = ParseBucketToken(options.bucket);
        if (!parsed)
        {
            return loglib::HistogramBucketSize::OneMinute;
        }
        return *parsed;
    }
    if (!histogram.minTs.has_value() || !histogram.maxTs.has_value())
    {
        return loglib::HistogramBucketSize::OneMinute;
    }
    return loglib::HistogramBucketIndex::AutoBucketSize(
        *histogram.minTs, *histogram.maxTs, BucketBudget(options, structuredOutput)
    );
}

void AppendLevelCountsJson(std::string &out, const loglib::LevelBucket &bucket)
{
    out += ", \"levels\": [";
    bool first = true;
    for (int value = static_cast<int>(loglib::LogLevel::Trace); value <= static_cast<int>(loglib::LogLevel::Fatal);
         ++value)
    {
        const auto level = static_cast<loglib::LogLevel>(value);
        const std::uint64_t count = bucket.counts[static_cast<std::size_t>(level)];
        if (count == 0)
        {
            continue;
        }
        if (!first)
        {
            out += ", ";
        }
        first = false;
        out += "{\"level\": ";
        AppendJsonString(out, loglib::CanonicalLevelName(level));
        out += ", \"count\": ";
        out += std::to_string(count);
        out += "}";
    }
    const std::uint64_t unknown = bucket.counts[0];
    if (unknown > 0)
    {
        if (!first)
        {
            out += ", ";
        }
        out += "{\"level\": ";
        AppendJsonString(out, "Unknown");
        out += ", \"count\": ";
        out += std::to_string(unknown);
        out += "}";
    }
    out += "]";
}

[[nodiscard]] std::int64_t EpochMicros(loglib::TimeStamp ts) noexcept
{
    return ts.time_since_epoch().count();
}

[[nodiscard]] std::string RenderTimeJson(
    const TimeHistogram &histogram, const RuntimeContext &runtime, const std::string &sources, std::uint64_t rowCount
)
{
    const auto widthMicros = histogram.index.BucketWidth().count();
    std::string out = "{\n  \"by\": \"time\",\n  \"sources\": [";
    AppendJsonString(out, sources);
    out += "],\n  \"rowCount\": ";
    out += std::to_string(rowCount);
    out += ",\n  \"bucketSize\": ";
    AppendJsonString(out, loglib::HistogramBucketSizeLabel(histogram.index.BucketSize()));
    out += ",\n  \"bucketWidthMicros\": ";
    out += std::to_string(widthMicros);
    out += ",\n  \"buckets\": [\n";

    const auto buckets = histogram.index.Buckets();
    for (std::size_t i = 0; i < buckets.size(); ++i)
    {
        if (buckets[i].Total() == 0)
        {
            continue;
        }
        if (i != 0)
        {
            out += ",\n";
        }
        const loglib::TimeStamp start = histogram.index.BucketStart(i);
        out += "    {\"bucketStartMicros\": ";
        out += std::to_string(EpochMicros(start));
        out += ", \"bucketWidthMicros\": ";
        out += std::to_string(widthMicros);
        out += ", \"bucketStartLocal\": ";
        AppendJsonString(out, runtime.timeZone.FormatTimeStamp(start));
        out += ", \"total\": ";
        out += std::to_string(buckets[i].Total());
        AppendLevelCountsJson(out, buckets[i]);
        out += "}";
    }
    out += "\n  ]\n}\n";
    return out;
}

[[nodiscard]] std::string RenderTimeText(
    const Console &console,
    const TimeHistogram &histogram,
    const RuntimeContext &runtime,
    const std::string &sources,
    const std::string &spanLine
)
{
    const bool color = console.ColorFor(console.Out());
    const int termWidth = TerminalWidth(console.Out(), DEFAULT_TERMINAL_WIDTH);
    const auto widthMicros = histogram.index.BucketWidth().count();

    std::vector<std::string> headers = {"bucketStartLocal", "total", "bar"};
    std::vector<std::vector<std::string>> rows;
    std::uint64_t maxTotal = 0;
    const auto buckets = histogram.index.Buckets();
    for (std::size_t i = 0; i < buckets.size(); ++i)
    {
        const std::uint64_t total = buckets[i].Total();
        if (total == 0)
        {
            continue;
        }
        maxTotal = std::max(maxTotal, total);
        const loglib::TimeStamp start = histogram.index.BucketStart(i);
        rows.push_back({runtime.timeZone.FormatTimeStamp(start), std::to_string(total), ""});
    }

    std::vector<std::size_t> widths(headers.size(), 0);
    for (std::size_t c = 0; c < headers.size(); ++c)
    {
        widths[c] = headers[c].size();
    }
    for (const auto &row : rows)
    {
        for (std::size_t c = 0; c < row.size(); ++c)
        {
            widths[c] = std::max(widths[c], row[c].size());
        }
    }

    const std::size_t fixedWidth = widths[0] + widths[1] + 4 + headers.size() - 1;
    const int barWidth = std::max(8, termWidth - static_cast<int>(fixedWidth));

    std::ostringstream out;
    out << "source: " << sources << '\n';
    out << spanLine << '\n';
    out << "bucket: " << loglib::HistogramBucketSizeLabel(histogram.index.BucketSize()) << " (" << widthMicros
        << " us)\n";

    auto writeCells = [&](const std::vector<std::string> &cells) {
        for (std::size_t c = 0; c < cells.size(); ++c)
        {
            if (c != 0)
            {
                out << "  ";
            }
            out << cells[c];
            if (c + 1 < cells.size())
            {
                out << std::string(widths[c] - cells[c].size(), ' ');
            }
        }
        out << '\n';
    };

    writeCells(headers);
    std::size_t rowIndex = 0;
    for (std::size_t i = 0; i < buckets.size(); ++i)
    {
        const std::uint64_t total = buckets[i].Total();
        if (total == 0)
        {
            continue;
        }
        const int filled =
            maxTotal == 0 ? 0
                          : static_cast<int>((static_cast<double>(total) * barWidth) / static_cast<double>(maxTotal));
        std::string bar(static_cast<std::size_t>(filled), '#');
        if (color)
        {
            for (int value = static_cast<int>(loglib::LogLevel::Fatal);
                 value >= static_cast<int>(loglib::LogLevel::Trace);
                 --value)
            {
                const auto level = static_cast<loglib::LogLevel>(value);
                const std::uint64_t count = buckets[i].counts[static_cast<std::size_t>(level)];
                if (count > 0)
                {
                    bar.insert(0, LevelAnsiPrefix(level));
                    bar.append(ANSI_RESET);
                    break;
                }
            }
        }
        std::vector<std::string> cells = rows[rowIndex];
        cells[2] = std::move(bar);
        writeCells(cells);
        ++rowIndex;
    }
    return out.str();
}

struct ColumnValueStats
{
    std::unordered_map<std::string, std::uint64_t> counts;
    std::uint64_t presentRows = 0;
};

[[nodiscard]] ColumnValueStats SummarizeColumn(
    const loglib::LogTable &table, const std::vector<std::size_t> &rows, std::size_t columnIndex
)
{
    ColumnValueStats stats;
    std::string buffer;
    for (const std::size_t row : rows)
    {
        const loglib::LogValue value = table.GetValue(row, columnIndex);
        if (!IsPresent(value))
        {
            continue;
        }
        ++stats.presentRows;
        const std::string_view text = table.GetValueOrFormatted(row, columnIndex, buffer);
        ++stats.counts[std::string(text)];
    }
    return stats;
}

/** @brief The @p top most frequent values, most frequent first, ties by value. */
[[nodiscard]] std::vector<std::pair<std::string, std::uint64_t>> RankValues(
    const ColumnValueStats &stats, std::uint64_t top
)
{
    std::vector<std::pair<std::string, std::uint64_t>> ranked(stats.counts.begin(), stats.counts.end());
    std::ranges::sort(ranked, [](const auto &lhs, const auto &rhs) {
        if (lhs.second != rhs.second)
        {
            return lhs.second > rhs.second;
        }
        return lhs.first < rhs.first;
    });
    if (ranked.size() > top)
    {
        ranked.resize(static_cast<std::size_t>(top));
    }
    return ranked;
}

[[nodiscard]] double ValuePercent(const ColumnValueStats &stats, std::uint64_t count) noexcept
{
    return stats.presentRows == 0 ? 0.0 : (100.0 * static_cast<double>(count)) / static_cast<double>(stats.presentRows);
}

/** @brief Quotes @p value as one CSV field, doubling embedded quotes. */
[[nodiscard]] std::string CsvField(std::string_view value)
{
    std::string field = "\"";
    for (const char ch : value)
    {
        if (ch == '"')
        {
            field.push_back('"');
        }
        field.push_back(ch);
    }
    field.push_back('"');
    return field;
}

[[nodiscard]] std::string RenderColumnText(
    const ColumnValueStats &stats,
    const StatsOptions &options,
    const std::string &sources,
    const std::string &columnName,
    const std::string &spanLine
)
{
    const auto ranked = RankValues(stats, options.top);

    std::vector<std::string> headers = {"value", "count", "percent"};
    std::vector<std::vector<std::string>> rows;
    rows.reserve(ranked.size());
    for (const auto &[value, count] : ranked)
    {
        rows.push_back({value, std::to_string(count), FormatPercent(ValuePercent(stats, count))});
    }

    std::vector<std::size_t> widths(headers.size(), 0);
    for (std::size_t c = 0; c < headers.size(); ++c)
    {
        widths[c] = headers[c].size();
    }
    for (const auto &row : rows)
    {
        for (std::size_t c = 0; c < row.size(); ++c)
        {
            widths[c] = std::max(widths[c], row[c].size());
        }
    }

    std::ostringstream out;
    out << "source: " << sources << '\n';
    out << spanLine << '\n';
    out << "column: " << columnName << '\n';
    out << "cardinality: " << stats.counts.size() << '\n';

    auto writeRow = [&](const std::vector<std::string> &cells) {
        for (std::size_t c = 0; c < cells.size(); ++c)
        {
            if (c != 0)
            {
                out << "  ";
            }
            out << cells[c];
            if (c + 1 < cells.size())
            {
                out << std::string(widths[c] - cells[c].size(), ' ');
            }
        }
        out << '\n';
    };
    writeRow(headers);
    for (const auto &row : rows)
    {
        writeRow(row);
    }
    return out.str();
}

[[nodiscard]] std::string RenderColumnJson(
    const ColumnValueStats &stats,
    const StatsOptions &options,
    const std::string &sources,
    const std::string &columnName,
    std::uint64_t rowCount
)
{
    const auto ranked = RankValues(stats, options.top);

    std::string out = "{\n  \"by\": \"column\",\n  \"sources\": [";
    AppendJsonString(out, sources);
    out += "],\n  \"rowCount\": ";
    out += std::to_string(rowCount);
    out += ",\n  \"column\": ";
    AppendJsonString(out, columnName);
    out += ",\n  \"cardinality\": ";
    out += std::to_string(stats.counts.size());
    out += ",\n  \"top\": [\n";
    for (std::size_t i = 0; i < ranked.size(); ++i)
    {
        if (i != 0)
        {
            out += ",\n";
        }
        const double percent = ValuePercent(stats, ranked[i].second);
        out += "    {\"value\": ";
        AppendJsonString(out, ranked[i].first);
        out += ", \"count\": ";
        out += std::to_string(ranked[i].second);
        out += ", \"percent\": ";
        out += FormatPercent(percent);
        out += "}";
    }
    out += "\n  ]\n}\n";
    return out;
}

[[nodiscard]] std::string RenderTimeCsv(
    const TimeHistogram &histogram, const RuntimeContext &runtime, std::uint64_t rowCount
)
{
    const auto widthMicros = histogram.index.BucketWidth().count();
    std::ostringstream out;
    out << "by,time\nrowCount," << rowCount << "\nbucketWidthMicros," << widthMicros << "\n";
    out << "bucketStartMicros,bucketWidthMicros,bucketStartLocal,total\n";
    const auto buckets = histogram.index.Buckets();
    for (std::size_t i = 0; i < buckets.size(); ++i)
    {
        if (buckets[i].Total() == 0)
        {
            continue;
        }
        const loglib::TimeStamp start = histogram.index.BucketStart(i);
        out << EpochMicros(start) << ',' << widthMicros << ',';
        out << '"' << runtime.timeZone.FormatTimeStamp(start) << '"' << ',' << buckets[i].Total() << '\n';
    }
    return out.str();
}

[[nodiscard]] std::string RenderColumnCsv(
    const ColumnValueStats &stats, const StatsOptions &options, const std::string &columnName, std::uint64_t rowCount
)
{
    std::ostringstream csv;
    csv << "by,column\nrowCount," << rowCount << "\ncolumn," << columnName << "\ncardinality," << stats.counts.size()
        << "\nvalue,count,percent\n";
    for (const auto &[value, count] : RankValues(stats, options.top))
    {
        csv << CsvField(value) << ',' << count << ',' << FormatPercent(ValuePercent(stats, count)) << '\n';
    }
    return csv.str();
}

[[nodiscard]] std::string RenderLevelCsv(const LevelSummary &summary, std::uint64_t rowCount)
{
    std::ostringstream csv;
    csv << "by,level\nrowCount," << rowCount << "\nlevel,count,percent\n";
    const auto percentOf = [&summary](std::uint64_t count) {
        return summary.total == 0 ? 0.0 : (100.0 * static_cast<double>(count)) / static_cast<double>(summary.total);
    };
    for (int value = static_cast<int>(loglib::LogLevel::Trace); value <= static_cast<int>(loglib::LogLevel::Fatal);
         ++value)
    {
        const auto level = static_cast<loglib::LogLevel>(value);
        const std::uint64_t count = summary.counts[static_cast<std::size_t>(level)];
        if (count != 0)
        {
            csv << loglib::CanonicalLevelName(level) << ',' << count << ',' << FormatPercent(percentOf(count)) << '\n';
        }
    }
    if (summary.unresolvable > 0)
    {
        csv << "Unknown," << summary.unresolvable << ',' << FormatPercent(percentOf(summary.unresolvable)) << '\n';
    }
    return csv.str();
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
    const std::uint64_t sourceRows = table.RowCount();
    const std::string sources = FormatSources(options.input);
    const std::string sourcesJson = FormatSourcesJson(options.input);

    auto session = ApplySessionConfiguration(options.filter, ViewOptions{}, table, runtime, StatsOptions::NAME);
    if (!session)
    {
        return console.Report(session.error());
    }

    const auto compiled =
        loglib::CompileExpression(session->expression, table.Configuration().Configuration().columns, &table);
    const std::vector<std::size_t> rows = loglib::FilterAcceptedRows(table, compiled);

    const ExitCode exitCode = StatsExitCode(options, sourceRows, rows.size());
    if (exitCode != ExitCode::Success)
    {
        return exitCode;
    }

    const bool structured = options.outputFormat != ReportFormat::Text;
    const auto &columns = table.Configuration().Configuration().columns;

    switch (options.by)
    {
    case StatsAxis::Level:
    {
        const int levelColumn = FirstColumnOfType(columns, loglib::ColumnType::Level);
        const LevelSummary summary = SummarizeLevels(table, rows, levelColumn);
        switch (options.outputFormat)
        {
        case ReportFormat::Json:
            console.Print(RenderLevelJson(summary, sourcesJson, rows.size()));
            break;
        case ReportFormat::Csv:
            console.Print(RenderLevelCsv(summary, rows.size()));
            break;
        case ReportFormat::Text:
            console.Print(RenderLevelText(console, summary, sources, std::format("span: {} row(s)", rows.size())));
            break;
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
        const std::size_t timeColumn = static_cast<std::size_t>(timeIndex);
        const int levelColumn = FirstColumnOfType(columns, loglib::ColumnType::Level);

        const TimeHistogram probe =
            BuildTimeHistogram(table, rows, timeColumn, levelColumn, loglib::HistogramBucketSize::OneMinute);
        const loglib::HistogramBucketSize bucketSize = ResolveBucketSize(options, structured, probe);
        const TimeHistogram histogram = BuildTimeHistogram(table, rows, timeColumn, levelColumn, bucketSize);

        std::string span = std::format("span: {} row(s)", rows.size());
        if (histogram.minTs.has_value() && histogram.maxTs.has_value())
        {
            span = std::format(
                "span: {} .. {} ({} row(s))",
                runtime.timeZone.FormatTimeStamp(*histogram.minTs),
                runtime.timeZone.FormatTimeStamp(*histogram.maxTs),
                rows.size()
            );
        }

        switch (options.outputFormat)
        {
        case ReportFormat::Json:
            console.Print(RenderTimeJson(histogram, runtime, sourcesJson, rows.size()));
            break;
        case ReportFormat::Csv:
            console.Print(RenderTimeCsv(histogram, runtime, rows.size()));
            break;
        case ReportFormat::Text:
            console.Print(RenderTimeText(console, histogram, runtime, sources, span));
            break;
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
        const ColumnValueStats columnStats = SummarizeColumn(table, rows, static_cast<std::size_t>(columnIndex));
        const std::string &header = columns[static_cast<std::size_t>(columnIndex)].header;
        switch (options.outputFormat)
        {
        case ReportFormat::Json:
            console.Print(RenderColumnJson(columnStats, options, sourcesJson, header, rows.size()));
            break;
        case ReportFormat::Csv:
            console.Print(RenderColumnCsv(columnStats, options, header, rows.size()));
            break;
        case ReportFormat::Text:
            console.Print(
                RenderColumnText(columnStats, options, sources, header, std::format("span: {} row(s)", rows.size()))
            );
            break;
        }
        return ExitCode::Success;
    }
    }
    return ExitCode::Success;
}

} // namespace slp
