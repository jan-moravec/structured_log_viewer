#include "commands.hpp"

#include "ansi.hpp"
#include "cli_filter.hpp"
#include "diagnostics.hpp"
#include "load_table.hpp"
#include "platform.hpp"
#include "session_config.hpp"

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
#include <format>
#include <iomanip>
#include <map>
#include <filesystem>
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

constexpr int kDefaultTextBucketBudget = 60;
constexpr int kDefaultStructuredBucketBudget = 500;
constexpr int kDefaultTerminalWidth = 80;

[[nodiscard]] CommandOutput StatsFail(ExitCode code, std::string_view sentence)
{
    CommandOutput out;
    out.exitCode = static_cast<int>(code);
    out.err = FormatErrorLine("stats", sentence);
    out.err.push_back('\n');
    return out;
}

void AppendJsonString(std::string &out, std::string_view text)
{
    out.push_back('"');
    for (const unsigned char ch : text)
    {
        switch (ch)
        {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (ch < 0x20)
            {
                constexpr char kHex[] = "0123456789abcdef";
                out += "\\u00";
                out.push_back(kHex[ch >> 4]);
                out.push_back(kHex[ch & 0x0f]);
            }
            else
            {
                out.push_back(static_cast<char>(ch));
            }
            break;
        }
    }
    out.push_back('"');
}

[[nodiscard]] std::string FormatPercent(double value)
{
    std::ostringstream out;
    out << std::fixed << std::setprecision(1) << value;
    return out.str();
}

[[nodiscard]] std::string FormatSources(const Options &options)
{
    if (options.sources.empty())
    {
        return "-";
    }
    std::ostringstream out;
    for (std::size_t i = 0; i < options.sources.size(); ++i)
    {
        if (i != 0)
        {
            out << ", ";
        }
        out << options.sources[i].text;
    }
    return out.str();
}

/** @brief Basenames only so JSON stays stable across absolute source paths. */
[[nodiscard]] std::string FormatSourcesJson(const Options &options)
{
    if (options.sources.empty())
    {
        return "-";
    }
    std::ostringstream out;
    for (std::size_t i = 0; i < options.sources.size(); ++i)
    {
        if (i != 0)
        {
            out << ", ";
        }
        out << std::filesystem::path(options.sources[i].text).filename().string();
    }
    return out.str();
}

[[nodiscard]] bool IsPresent(const loglib::LogValue &value) noexcept
{
    return !std::holds_alternative<std::monostate>(value);
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

[[nodiscard]] int BucketBudget(const Options &options, bool structuredOutput)
{
    if (options.maxBuckets)
    {
        return static_cast<int>(*options.maxBuckets);
    }
    return structuredOutput ? kDefaultStructuredBucketBudget : kDefaultTextBucketBudget;
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
    const Options &options,
    const LevelSummary &summary,
    const std::string &sources,
    const std::string &spanLine
)
{
    const bool color = ColorActive(options, stdout);
    const int termWidth = TerminalWidth(stdout, kDefaultTerminalWidth);

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
        const double percent = summary.total == 0 ? 0.0 : (100.0 * static_cast<double>(count)) /
                                                             static_cast<double>(summary.total);
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
            maxCount == 0 ? 0 : static_cast<int>((static_cast<double>(count) * barWidth) / static_cast<double>(maxCount));
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
                out << LevelAnsiPrefix(levelForColor) << cells[c] << kAnsiReset;
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

[[nodiscard]] std::string RenderLevelJson(const LevelSummary &summary, const std::string &sources, std::uint64_t rowCount)
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
        const double percent = summary.total == 0 ? 0.0 : (100.0 * static_cast<double>(count)) /
                                                             static_cast<double>(summary.total);
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
        const double percent = summary.total == 0
                                   ? 0.0
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
    const Options &options, bool structuredOutput, const TimeHistogram &histogram
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
    const TimeHistogram &histogram,
    const RuntimeContext &runtime,
    const std::string &sources,
    std::uint64_t rowCount
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
    const Options &options,
    const TimeHistogram &histogram,
    const RuntimeContext &runtime,
    const std::string &sources,
    const std::string &spanLine
)
{
    const bool color = ColorActive(options, stdout);
    const int termWidth = TerminalWidth(stdout, kDefaultTerminalWidth);
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
    out << "bucket: " << loglib::HistogramBucketSizeLabel(histogram.index.BucketSize()) << " ("
        << widthMicros << " us)\n";

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
            maxTotal == 0 ? 0 : static_cast<int>((static_cast<double>(total) * barWidth) / static_cast<double>(maxTotal));
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
                    bar = std::string(LevelAnsiPrefix(level)) + bar + std::string(kAnsiReset);
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

[[nodiscard]] std::string RenderColumnText(
    const ColumnValueStats &stats,
    const Options &options,
    const std::string &sources,
    const std::string &columnName,
    const std::string &spanLine
)
{
    std::vector<std::pair<std::string, std::uint64_t>> ranked(stats.counts.begin(), stats.counts.end());
    std::sort(ranked.begin(), ranked.end(), [](const auto &lhs, const auto &rhs) {
        if (lhs.second != rhs.second)
        {
            return lhs.second > rhs.second;
        }
        return lhs.first < rhs.first;
    });
    if (ranked.size() > options.top)
    {
        ranked.resize(options.top);
    }

    std::vector<std::string> headers = {"value", "count", "percent"};
    std::vector<std::vector<std::string>> rows;
    rows.reserve(ranked.size());
    for (const auto &[value, count] : ranked)
    {
        const double percent = stats.presentRows == 0 ? 0.0
                                                      : (100.0 * static_cast<double>(count)) /
                                                            static_cast<double>(stats.presentRows);
        rows.push_back({value, std::to_string(count), FormatPercent(percent)});
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
    const Options &options,
    const std::string &sources,
    const std::string &columnName,
    std::uint64_t rowCount
)
{
    std::vector<std::pair<std::string, std::uint64_t>> ranked(stats.counts.begin(), stats.counts.end());
    std::sort(ranked.begin(), ranked.end(), [](const auto &lhs, const auto &rhs) {
        if (lhs.second != rhs.second)
        {
            return lhs.second > rhs.second;
        }
        return lhs.first < rhs.first;
    });
    if (ranked.size() > options.top)
    {
        ranked.resize(options.top);
    }

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
        const double percent = stats.presentRows == 0 ? 0.0
                                                      : (100.0 * static_cast<double>(ranked[i].second)) /
                                                            static_cast<double>(stats.presentRows);
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

[[nodiscard]] int StatsExitCode(const Options &options, std::uint64_t sourceRows, std::size_t filteredRows)
{
    if (sourceRows == 0)
    {
        return static_cast<int>(ExitCode::Success);
    }
    if (filteredRows == 0 && !options.filters.empty())
    {
        return static_cast<int>(ExitCode::Empty);
    }
    return static_cast<int>(ExitCode::Success);
}

} // namespace

CommandOutput RunStats(const Options &options, const RuntimeContext &runtime)
{
    CommandOutput out;
    auto [loaded, loadSide] = LoadTable(options, runtime);
    out.err += loadSide.err;
    if (loadSide.exitCode != 0)
    {
        out.exitCode = loadSide.exitCode;
        return out;
    }

    loglib::LogTable &table = loaded.table;
    const std::uint64_t sourceRows = table.RowCount();
    const std::string sources = FormatSources(options);
    const std::string sourcesJson = FormatSourcesJson(options);

    auto session = ApplySessionConfiguration(options, table, runtime, "stats");
    if (!session)
    {
        out.err += session.error().err;
        out.exitCode = session.error().exitCode;
        return out;
    }

    const auto compiled = loglib::CompileExpression(session->expression, table.Configuration().Configuration().columns, &table);
    std::vector<std::size_t> rows = loglib::FilterAcceptedRows(table, compiled);

    out.exitCode = StatsExitCode(options, sourceRows, rows.size());
    if (out.exitCode != 0)
    {
        return out;
    }

    const bool structured = options.outputFormat == "json" || options.outputFormat == "csv";
    const auto &columns = table.Configuration().Configuration().columns;

    if (options.statsBy == "level")
    {
        const int levelColumn = FirstColumnOfType(columns, loglib::ColumnType::Level);
        const LevelSummary summary = SummarizeLevels(table, rows, levelColumn);
        const std::string span = std::format("span: {} row(s)", rows.size());
        if (options.outputFormat == "json")
        {
            out.out = RenderLevelJson(summary, sourcesJson, rows.size());
        }
        else if (options.outputFormat == "csv")
        {
            std::ostringstream csv;
            csv << "by,level\nrowCount," << rows.size() << "\nlevel,count,percent\n";
            for (int value = static_cast<int>(loglib::LogLevel::Trace); value <= static_cast<int>(loglib::LogLevel::Fatal);
                 ++value)
            {
                const auto level = static_cast<loglib::LogLevel>(value);
                const std::uint64_t count = summary.counts[static_cast<std::size_t>(level)];
                if (count == 0)
                {
                    continue;
                }
                const double percent = summary.total == 0 ? 0.0
                                                          : (100.0 * static_cast<double>(count)) /
                                                                static_cast<double>(summary.total);
                csv << loglib::CanonicalLevelName(level) << ',' << count << ',' << FormatPercent(percent) << '\n';
            }
            if (summary.unresolvable > 0)
            {
                const double percent = summary.total == 0 ? 0.0
                                                          : (100.0 * static_cast<double>(summary.unresolvable)) /
                                                                static_cast<double>(summary.total);
                csv << "Unknown," << summary.unresolvable << ',' << FormatPercent(percent) << '\n';
            }
            out.out = csv.str();
        }
        else
        {
            out.out = RenderLevelText(options, summary, sources, span);
        }
        return out;
    }

    if (options.statsBy == "time")
    {
        const int timeIndex = FirstColumnOfType(columns, loglib::ColumnType::Time);
        if (timeIndex < 0)
        {
            return StatsFail(ExitCode::Usage, "this source has no time column for --by time.");
        }
        const std::size_t timeColumn = static_cast<std::size_t>(timeIndex);
        const int levelColumn = FirstColumnOfType(columns, loglib::ColumnType::Level);

        TimeHistogram probe = BuildTimeHistogram(
            table, rows, timeColumn, levelColumn, loglib::HistogramBucketSize::OneMinute
        );
        const loglib::HistogramBucketSize bucketSize = ResolveBucketSize(options, structured, probe);
        TimeHistogram histogram = BuildTimeHistogram(table, rows, timeColumn, levelColumn, bucketSize);

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

        if (options.outputFormat == "json")
        {
            out.out = RenderTimeJson(histogram, runtime, sourcesJson, rows.size());
        }
        else if (options.outputFormat == "csv")
        {
            out.out = RenderTimeCsv(histogram, runtime, rows.size());
        }
        else
        {
            out.out = RenderTimeText(options, histogram, runtime, sources, span);
        }
        return out;
    }

    if (options.statsBy == "column")
    {
        if (!options.column)
        {
            return StatsFail(ExitCode::Usage, "--column is required with --by column.");
        }
        const int columnIndex = FindColumnByName(columns, *options.column);
        if (columnIndex < 0)
        {
            return StatsFail(
                ExitCode::Usage,
                std::format(
                    "unresolvable column '{}'; available columns are {}.", *options.column, AvailableColumnList(columns)
                )
            );
        }
        const ColumnValueStats columnStats =
            SummarizeColumn(table, rows, static_cast<std::size_t>(columnIndex));
        const std::string span = std::format("span: {} row(s)", rows.size());
        const std::string &header = columns[static_cast<std::size_t>(columnIndex)].header;
        if (options.outputFormat == "json")
        {
            out.out = RenderColumnJson(columnStats, options, sourcesJson, header, rows.size());
        }
        else if (options.outputFormat == "csv")
        {
            std::ostringstream csv;
            csv << "by,column\nrowCount," << rows.size() << "\ncolumn," << header
                << "\ncardinality," << columnStats.counts.size() << "\nvalue,count,percent\n";
            std::vector<std::pair<std::string, std::uint64_t>> ranked(
                columnStats.counts.begin(), columnStats.counts.end()
            );
            std::sort(ranked.begin(), ranked.end(), [](const auto &lhs, const auto &rhs) {
                if (lhs.second != rhs.second)
                {
                    return lhs.second > rhs.second;
                }
                return lhs.first < rhs.first;
            });
            if (ranked.size() > options.top)
            {
                ranked.resize(options.top);
            }
            for (const auto &[value, count] : ranked)
            {
                const double percent = columnStats.presentRows == 0
                                           ? 0.0
                                           : (100.0 * static_cast<double>(count)) /
                                                 static_cast<double>(columnStats.presentRows);
                csv << '"' << value << '"' << ',' << count << ',' << FormatPercent(percent) << '\n';
            }
            out.out = csv.str();
        }
        else
        {
            out.out = RenderColumnText(columnStats, options, sources, header, span);
        }
        return out;
    }

    return StatsFail(ExitCode::Usage, std::format("'--by' expects level, time, or column, not '{}'.", options.statsBy));
}

} // namespace slp
