#include <loglib/bytes_producer.hpp>
#include <loglib/exports/export_sink.hpp>
#include <loglib/exports/row_exporter.hpp>
#include <loglib/key_index.hpp>
#include <loglib/line_source.hpp>
#include <loglib/log_configuration.hpp>
#include <loglib/log_data.hpp>
#include <loglib/log_line.hpp>
#include <loglib/log_table.hpp>
#include <loglib/log_value.hpp>
#include <loglib/stop_token.hpp>
#include <loglib/stream_line_source.hpp>

#include <catch2/catch_all.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <numeric>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

using loglib::exports::ExportFormat;
using loglib::exports::RowSource;

namespace
{

class MemorySink : public loglib::exports::ExportSink
{
public:
    void Write(std::string_view bytes) override
    {
        mBuffer.append(bytes.data(), bytes.size());
    }
    void Finish() override
    {
        mFinished = true;
    }
    [[nodiscard]] const std::string &Bytes() const noexcept
    {
        return mBuffer;
    }
    [[nodiscard]] bool Finished() const noexcept
    {
        return mFinished;
    }

private:
    std::string mBuffer;
    bool mFinished = false;
};

class ThrowOnNthWriteSink : public loglib::exports::ExportSink
{
public:
    explicit ThrowOnNthWriteSink(std::size_t failOnWrite) noexcept
        : mFailOnWrite(failOnWrite)
    {
    }

    void Write(std::string_view bytes) override
    {
        if (mWriteCount == mFailOnWrite)
        {
            ++mWriteCount;
            throw std::runtime_error("simulated disk full");
        }
        ++mWriteCount;
        mBuffer.append(bytes.data(), bytes.size());
    }
    void Finish() override
    {
        mFinished = true;
    }
    [[nodiscard]] std::size_t WriteCount() const noexcept
    {
        return mWriteCount;
    }
    [[nodiscard]] const std::string &Bytes() const noexcept
    {
        return mBuffer;
    }
    [[nodiscard]] bool Finished() const noexcept
    {
        return mFinished;
    }

private:
    std::size_t mFailOnWrite;
    std::size_t mWriteCount = 0;
    std::string mBuffer;
    bool mFinished = false;
};

class ThrowingLineSource final : public loglib::LineSource
{
public:
    enum class ThrowMode
    {
        None,
        OutOfRange,
        RuntimeError,
        LogicError,
    };

    ThrowingLineSource(std::filesystem::path displayName, std::unordered_map<std::size_t, ThrowMode> perLine)
        : mPath(std::move(displayName)), mPerLine(std::move(perLine))
    {
    }

    [[nodiscard]] const std::filesystem::path &Path() const noexcept override
    {
        return mPath;
    }
    [[nodiscard]] std::string RawLine(std::size_t lineId) const override
    {
        auto it = mPerLine.find(lineId);
        const ThrowMode mode = (it == mPerLine.end()) ? ThrowMode::None : it->second;
        switch (mode)
        {
        case ThrowMode::None:
            return std::string("line ") + std::to_string(lineId);
        case ThrowMode::OutOfRange:
            throw std::out_of_range("evicted");
        case ThrowMode::RuntimeError:
            throw std::runtime_error("backing store gone");
        case ThrowMode::LogicError:
            throw std::logic_error("bogus lineId");
        }
        return {};
    }
    [[nodiscard]] std::string_view ResolveMmapBytes(
        std::uint64_t /*ownerId*/, std::uint32_t /*offset*/, std::size_t /*length*/
    ) const noexcept override
    {
        return {};
    }
    [[nodiscard]] std::string_view ResolveOwnedBytes(
        std::uint64_t /*ownerId*/, std::uint32_t /*offset*/, std::size_t /*length*/
    ) const noexcept override
    {
        return {};
    }
    [[nodiscard]] std::span<const char> StableBytes() const noexcept override
    {
        return {};
    }
    std::uint64_t AppendOwnedBytes(std::size_t /*lineId*/, std::string_view /*bytes*/) override
    {
        return 0;
    }
    [[nodiscard]] bool SupportsEviction() const noexcept override
    {
        return false;
    }
    void EvictBefore(std::size_t /*lineId*/) override
    {
    }
    [[nodiscard]] std::size_t FirstAvailableLineId() const noexcept override
    {
        return 1;
    }

private:
    std::filesystem::path mPath;
    std::unordered_map<std::size_t, ThrowMode> mPerLine;
};

class ScopedTempDir
{
public:
    ScopedTempDir()
    {
        std::error_code ec;
        mPath = std::filesystem::temp_directory_path(ec);
        mPath /= "slv_export_test";
        auto nsecs = std::chrono::steady_clock::now().time_since_epoch().count();
        mPath += std::to_string(nsecs);
        std::filesystem::create_directories(mPath, ec);
    }
    ~ScopedTempDir() noexcept
    {
        try
        {
            std::error_code ec;
            std::filesystem::remove_all(mPath, ec);
        }
        catch (...) // NOLINT(bugprone-empty-catch)
        {
        }
    }
    ScopedTempDir(const ScopedTempDir &) = delete;
    ScopedTempDir &operator=(const ScopedTempDir &) = delete;
    ScopedTempDir(ScopedTempDir &&) = delete;
    ScopedTempDir &operator=(ScopedTempDir &&) = delete;

    [[nodiscard]] std::filesystem::path FilePath(std::string_view name) const
    {
        return mPath / std::filesystem::path(std::string(name));
    }

private:
    std::filesystem::path mPath;
};

loglib::LogTable BuildFixtureTable(std::vector<std::string> rawLines, std::size_t rowCount)
{
    auto stream = std::make_unique<loglib::StreamLineSource>(std::filesystem::path("fixture.jsonl"), nullptr);
    for (auto &raw : rawLines)
    {
        stream->AppendLine(std::move(raw), {});
    }
    auto *sourcePtr = stream.get();

    loglib::KeyIndex keys;
    std::vector<loglib::LogLine> lines;
    lines.reserve(rowCount);

    const loglib::TimeStamp t0{std::chrono::microseconds(1700000000000000LL)};
    for (std::size_t i = 0; i < rowCount; ++i)
    {
        loglib::LogMap map;
        map["ts"] = loglib::TimeStamp{t0 + std::chrono::microseconds(static_cast<std::int64_t>(i * 1000))};
        const auto levelSlot = i % 3;
        const char *levelText = "error";
        if (levelSlot == 0)
        {
            levelText = "info";
        }
        else if (levelSlot == 1)
        {
            levelText = "warn";
        }
        map["level"] = std::string(levelText);
        map["message"] = std::string("hello, \"world\"\n#") + std::to_string(i);
        map["count"] = static_cast<std::int64_t>(i);
        map["ratio"] = static_cast<double>(i) * 0.5;
        map["ok"] = (i % 2 == 0);
        lines.emplace_back(map, keys, *sourcePtr, i + 1);
    }

    loglib::LogData data(std::move(stream), std::move(lines), std::move(keys));

    loglib::LogConfiguration config;
    config.columns.push_back(
        {.header = "Time",
         .keys = {"ts"},
         .printFormat = "%FT%T",
         .type = loglib::LogConfiguration::Type::Time,
         .parseFormats = {"%FT%T"}}
    );
    config.columns.push_back(
        {.header = "Level", .keys = {"level"}, .printFormat = "{}", .type = loglib::LogConfiguration::Type::String}
    );
    config.columns.push_back(
        {.header = "Message", .keys = {"message"}, .printFormat = "{}", .type = loglib::LogConfiguration::Type::String}
    );
    config.columns.push_back(
        {.header = "Count", .keys = {"count"}, .printFormat = "{}", .type = loglib::LogConfiguration::Type::Integer}
    );
    config.columns.push_back(
        {.header = "Ratio", .keys = {"ratio"}, .printFormat = "{}", .type = loglib::LogConfiguration::Type::Floating}
    );
    config.columns.push_back(
        {.header = "OK", .keys = {"ok"}, .printFormat = "{}", .type = loglib::LogConfiguration::Type::Boolean}
    );

    loglib::LogConfigurationManager manager;
    manager.SetConfiguration(std::move(config));
    return {std::move(data), std::move(manager)};
}

std::string ReadFile(const std::filesystem::path &path)
{
    const std::ifstream file(path, std::ios::binary);
    std::stringstream ss;
    ss << file.rdbuf();
    return ss.str();
}

std::vector<std::string> SplitNonEmptyLines(std::string_view text)
{
    std::vector<std::string> lines;
    std::size_t start = 0;
    while (start < text.size())
    {
        const auto nl = text.find('\n', start);
        if (nl == std::string_view::npos)
        {
            lines.emplace_back(text.substr(start));
            break;
        }
        if (nl > start)
        {
            lines.emplace_back(text.substr(start, nl - start));
        }
        start = nl + 1;
    }
    return lines;
}

} // namespace

TEST_CASE("exports: JSON Lines emits typed values per row", "[exports][row_exporter]")
{
    std::vector<std::string> raws = {
        R"({"ts":"2023-11-14T22:13:20","level":"info","message":"first"})",
        R"({"ts":"2023-11-14T22:13:20.001000","level":"warn","message":"second"})",
    };
    const auto table = BuildFixtureTable(std::move(raws), 2);

    std::vector<std::size_t> rows = {0, 1};
    std::vector<std::size_t> cols = {0, 1, 2, 3, 4, 5};
    const RowSource source{
        .table = &table,
        .sourceRows = rows,
        .visibleColumns = cols,
        .includeAllFieldsForJson = true,
        .includeHeaderRow = false,
    };

    MemorySink sink;
    auto exporter = loglib::exports::MakeExporter(ExportFormat::JsonLines);
    REQUIRE(exporter != nullptr);
    exporter->Run(source, sink, loglib::StopToken{});

    const auto lines = SplitNonEmptyLines(sink.Bytes());
    REQUIRE(lines.size() == 2);
    REQUIRE(lines[0].contains("\"count\":0"));
    REQUIRE(lines[0].contains("\"ok\":true"));
    REQUIRE(lines[0].contains("\"ratio\":0.0"));
    REQUIRE(lines[0].contains("\"ts\":\"2023-11-14T22:13:20"));
    REQUIRE(lines[1].contains("\"count\":1"));
    REQUIRE(lines[1].contains("\"ok\":false"));
    REQUIRE(lines[1].contains("\"ratio\":0.5"));
    REQUIRE(lines[0].contains("\\\"world\\\""));
    REQUIRE(lines[0].contains("\\n#0"));
}

TEST_CASE("exports: JSON Lines round-trips synthetic rows", "[exports][row_exporter]")
{
    constexpr std::size_t ROW_COUNT = 10000;
    std::vector<std::string> raws;
    raws.reserve(ROW_COUNT);
    for (std::size_t i = 0; i < ROW_COUNT; ++i)
    {
        raws.push_back(std::string("row ") + std::to_string(i));
    }
    const auto table = BuildFixtureTable(std::move(raws), ROW_COUNT);

    std::vector<std::size_t> rows(ROW_COUNT);
    std::iota(rows.begin(), rows.end(), 0);
    std::vector<std::size_t> cols;
    const RowSource source{
        .table = &table,
        .sourceRows = rows,
        .visibleColumns = cols,
        .includeAllFieldsForJson = true,
        .includeHeaderRow = false,
    };

    MemorySink sink;
    auto exporter = loglib::exports::MakeExporter(ExportFormat::JsonLines);
    REQUIRE(exporter != nullptr);
    exporter->Run(source, sink, loglib::StopToken{});

    const auto &full = sink.Bytes();
    const auto lineCount = static_cast<std::size_t>(std::count(full.begin(), full.end(), '\n'));
    REQUIRE(lineCount == ROW_COUNT);

    const auto firstNl = full.find('\n');
    REQUIRE(firstNl != std::string::npos);
    const std::string first = full.substr(0, firstNl);
    REQUIRE(first.front() == '{');
    REQUIRE(first.back() == '}');
    for (const char *key : {"\"ts\"", "\"level\"", "\"message\"", "\"count\"", "\"ratio\"", "\"ok\""})
    {
        REQUIRE(first.contains(key));
    }
}

TEST_CASE("exports: CSV quoting follows RFC 4180", "[exports][row_exporter]")
{
    std::vector<std::string> raws = {"raw1"};
    const auto table = BuildFixtureTable(std::move(raws), 1);

    std::vector<std::size_t> rows = {0};
    std::vector<std::size_t> cols = {1, 2};
    const RowSource source{
        .table = &table,
        .sourceRows = rows,
        .visibleColumns = cols,
        .includeAllFieldsForJson = true,
        .includeHeaderRow = false,
    };
    MemorySink sink;
    auto exporter = loglib::exports::MakeExporter(ExportFormat::Csv);
    REQUIRE(exporter != nullptr);
    exporter->Run(source, sink, loglib::StopToken{});

    REQUIRE(sink.Bytes().contains("info,\"hello, \"\"world\"\""));
}

TEST_CASE("exports: CSV header row respects includeHeaderRow", "[exports][row_exporter]")
{
    std::vector<std::string> raws = {"raw1"};
    const auto table = BuildFixtureTable(std::move(raws), 1);

    std::vector<std::size_t> rows = {0};
    std::vector<std::size_t> cols = {1, 3};

    {
        const RowSource src{
            .table = &table,
            .sourceRows = rows,
            .visibleColumns = cols,
            .includeAllFieldsForJson = false,
            .includeHeaderRow = true
        };
        MemorySink sink;
        auto exporter = loglib::exports::MakeExporter(ExportFormat::Csv);
        exporter->Run(src, sink, loglib::StopToken{});
        REQUIRE(sink.Bytes().starts_with("Level,Count\n"));
    }
    {
        const RowSource src{
            .table = &table,
            .sourceRows = rows,
            .visibleColumns = cols,
            .includeAllFieldsForJson = false,
            .includeHeaderRow = false
        };
        MemorySink sink;
        auto exporter = loglib::exports::MakeExporter(ExportFormat::Csv);
        exporter->Run(src, sink, loglib::StopToken{});
        REQUIRE_FALSE(sink.Bytes().starts_with("Level,"));
    }
}

TEST_CASE("exports: CSV formula-injection cells are quoted and prefixed", "[exports][row_exporter]")
{
    std::vector<std::string> raws = {"raw1", "raw2", "raw3", "raw4"};
    auto stream = std::make_unique<loglib::StreamLineSource>(std::filesystem::path("fixture.csv"), nullptr);
    for (auto &raw : raws)
    {
        stream->AppendLine(std::move(raw), {});
    }
    auto *sourcePtr = stream.get();

    loglib::KeyIndex keys;
    std::vector<loglib::LogLine> lines;
    const std::array<std::string, 4> payloads = {
        std::string("=cmd|'/c calc'!A1"),
        std::string("@SUM(1+1)"),
        std::string("+2+3"),
        std::string("-42"),
    };
    for (std::size_t i = 0; i < payloads.size(); ++i)
    {
        loglib::LogMap map;
        map["message"] = payloads[i];
        lines.emplace_back(map, keys, *sourcePtr, i + 1);
    }
    loglib::LogData data(std::move(stream), std::move(lines), std::move(keys));

    loglib::LogConfiguration config;
    config.columns.push_back(
        {.header = "Message", .keys = {"message"}, .printFormat = "{}", .type = loglib::LogConfiguration::Type::String}
    );
    loglib::LogConfigurationManager manager;
    manager.SetConfiguration(std::move(config));
    const loglib::LogTable table(std::move(data), std::move(manager));

    std::vector<std::size_t> rows = {0, 1, 2, 3};
    std::vector<std::size_t> cols = {0};
    const RowSource src{
        .table = &table,
        .sourceRows = rows,
        .visibleColumns = cols,
        .includeAllFieldsForJson = false,
        .includeHeaderRow = false,
    };

    MemorySink sink;
    auto exporter = loglib::exports::MakeExporter(ExportFormat::Csv);
    REQUIRE(exporter != nullptr);
    exporter->Run(src, sink, loglib::StopToken{});

    const auto outLines = SplitNonEmptyLines(sink.Bytes());
    REQUIRE(outLines.size() == 4);
    REQUIRE(outLines[0] == "\"'=cmd|'/c calc'!A1\"");
    REQUIRE(outLines[1] == "\"'@SUM(1+1)\"");
    REQUIRE(outLines[2] == "+2+3");
    REQUIRE(outLines[3] == "-42");
}

TEST_CASE("exports: JSON Lines keeps a trailing .0 on whole-valued doubles", "[exports][row_exporter]")
{
    std::vector<std::string> raws = {"row 0", "row 1", "row 2", "row 3"};
    auto stream = std::make_unique<loglib::StreamLineSource>(std::filesystem::path("fixture.jsonl"), nullptr);
    for (auto &raw : raws)
    {
        stream->AppendLine(std::move(raw), {});
    }
    auto *sourcePtr = stream.get();

    loglib::KeyIndex keys;
    std::vector<loglib::LogLine> lines;
    const std::array<double, 4> values = {0.0, 1.0, -3.0, 2.5};
    for (std::size_t i = 0; i < values.size(); ++i)
    {
        loglib::LogMap map;
        map["value"] = values[i];
        lines.emplace_back(map, keys, *sourcePtr, i + 1);
    }
    loglib::LogData data(std::move(stream), std::move(lines), std::move(keys));

    loglib::LogConfiguration config;
    config.columns.push_back(
        {.header = "Value", .keys = {"value"}, .printFormat = "{}", .type = loglib::LogConfiguration::Type::Floating}
    );
    loglib::LogConfigurationManager manager;
    manager.SetConfiguration(std::move(config));
    const loglib::LogTable table(std::move(data), std::move(manager));

    std::vector<std::size_t> rows = {0, 1, 2, 3};
    std::vector<std::size_t> cols;
    const RowSource src{
        .table = &table,
        .sourceRows = rows,
        .visibleColumns = cols,
        .includeAllFieldsForJson = true,
        .includeHeaderRow = false,
    };

    MemorySink sink;
    auto exporter = loglib::exports::MakeExporter(ExportFormat::JsonLines);
    REQUIRE(exporter != nullptr);
    exporter->Run(src, sink, loglib::StopToken{});

    const auto outLines = SplitNonEmptyLines(sink.Bytes());
    REQUIRE(outLines.size() == 4);
    REQUIRE(outLines[0].contains("\"value\":0.0"));
    REQUIRE(outLines[1].contains("\"value\":1.0"));
    REQUIRE(outLines[2].contains("\"value\":-3.0"));
    REQUIRE(outLines[3].contains("\"value\":2.5"));
}

TEST_CASE("exports: Markdown escapes pipes and collapses newlines", "[exports][row_exporter]")
{
    const std::vector<std::string> raws = {"raw1"};
    auto stream = std::make_unique<loglib::StreamLineSource>(std::filesystem::path("fixture.md"), nullptr);
    stream->AppendLine("raw1", {});
    auto *sourcePtr = stream.get();

    loglib::KeyIndex keys;
    std::vector<loglib::LogLine> lines;
    loglib::LogMap map;
    map["message"] = std::string("a | b\nnewline");
    lines.emplace_back(map, keys, *sourcePtr, 1);
    loglib::LogData data(std::move(stream), std::move(lines), std::move(keys));

    loglib::LogConfiguration config;
    config.columns.push_back(
        {.header = "Msg", .keys = {"message"}, .printFormat = "{}", .type = loglib::LogConfiguration::Type::String}
    );
    loglib::LogConfigurationManager manager;
    manager.SetConfiguration(std::move(config));
    const loglib::LogTable table(std::move(data), std::move(manager));

    std::vector<std::size_t> rows = {0};
    std::vector<std::size_t> cols = {0};
    const RowSource src{
        .table = &table,
        .sourceRows = rows,
        .visibleColumns = cols,
        .includeAllFieldsForJson = false,
        .includeHeaderRow = true
    };

    MemorySink sink;
    auto exporter = loglib::exports::MakeExporter(ExportFormat::Markdown);
    exporter->Run(src, sink, loglib::StopToken{});

    const auto &out = sink.Bytes();
    REQUIRE(out.contains("a \\| b newline"));
    REQUIRE_FALSE(out.contains("a | b"));
    REQUIRE(out.starts_with("| Msg |\n| --- |"));
}

TEST_CASE("exports: Snapshot echoes raw source bytes", "[exports][row_exporter]")
{
    std::vector<std::string> raws = {"line one", "line two"};
    const auto table = BuildFixtureTable(std::move(raws), 2);

    std::vector<std::size_t> rows = {0, 1};
    std::vector<std::size_t> cols;
    const RowSource src{.table = &table, .sourceRows = rows, .visibleColumns = cols};

    MemorySink sink;
    auto exporter = loglib::exports::MakeExporter(ExportFormat::Snapshot);
    exporter->Run(src, sink, loglib::StopToken{});

    REQUIRE(sink.Bytes() == "line one\nline two\n");
}

TEST_CASE("exports: Snapshot skips unavailable source rows", "[exports][row_exporter]")
{
    std::unordered_map<std::size_t, ThrowingLineSource::ThrowMode> throwSpec = {
        {2, ThrowingLineSource::ThrowMode::RuntimeError},
        {3, ThrowingLineSource::ThrowMode::LogicError},
    };
    auto source = std::make_unique<ThrowingLineSource>(std::filesystem::path("throwing.log"), std::move(throwSpec));
    auto *sourcePtr = source.get();

    loglib::KeyIndex keys;
    std::vector<loglib::LogLine> lines;
    for (std::size_t i = 0; i < 4; ++i)
    {
        loglib::LogMap map;
        map["message"] = std::string("payload ") + std::to_string(i);
        lines.emplace_back(map, keys, *sourcePtr, i + 1);
    }
    loglib::LogData data(std::move(source), std::move(lines), std::move(keys));

    loglib::LogConfiguration config;
    config.columns.push_back(
        {.header = "Message", .keys = {"message"}, .printFormat = "{}", .type = loglib::LogConfiguration::Type::String}
    );
    loglib::LogConfigurationManager manager;
    manager.SetConfiguration(std::move(config));
    const loglib::LogTable table(std::move(data), std::move(manager));

    std::vector<std::size_t> rows = {0, 1, 2, 3};
    std::vector<std::size_t> cols;
    const RowSource src{.table = &table, .sourceRows = rows, .visibleColumns = cols};

    MemorySink sink;
    auto exporter = loglib::exports::MakeExporter(ExportFormat::Snapshot);
    exporter->Run(src, sink, loglib::StopToken{});

    REQUIRE(sink.Bytes() == "line 1\nline 4\n");
}

TEST_CASE("exports: sink write failures propagate from every format", "[exports][row_exporter]")
{
    std::vector<std::string> raws = {"first raw line", "second raw line"};
    const auto table = BuildFixtureTable(std::move(raws), 2);

    std::vector<std::size_t> rows = {0, 1};
    std::vector<std::size_t> cols = {1, 2, 3, 4, 5};
    const RowSource src{
        .table = &table,
        .sourceRows = rows,
        .visibleColumns = cols,
        .includeAllFieldsForJson = false,
        .includeHeaderRow = true,
    };

    const std::array<ExportFormat, 4> formats = {
        ExportFormat::JsonLines,
        ExportFormat::Csv,
        ExportFormat::Snapshot,
        ExportFormat::Markdown,
    };
    for (const ExportFormat format : formats)
    {
        ThrowOnNthWriteSink sink(/*failOnWrite=*/1);
        auto exporter = loglib::exports::MakeExporter(format);
        REQUIRE(exporter != nullptr);
        bool threw = false;
        try
        {
            exporter->Run(src, sink, loglib::StopToken{});
        }
        catch (const loglib::exports::ExportCancelled &)
        {
            FAIL("format " << loglib::exports::LabelFor(format) << " wrongly surfaced ExportCancelled");
        }
        catch (const std::runtime_error &)
        {
            threw = true;
        }
        REQUIRE(threw);
        REQUIRE(sink.WriteCount() == std::size_t(2));
    }
}

TEST_CASE("exports: cancel mid-export leaves no partial file", "[exports][row_exporter]")
{
    constexpr std::size_t ROW_COUNT = 20000;
    std::vector<std::string> raws;
    raws.reserve(ROW_COUNT);
    for (std::size_t i = 0; i < ROW_COUNT; ++i)
    {
        raws.push_back(std::string("row ") + std::to_string(i));
    }
    const auto table = BuildFixtureTable(std::move(raws), ROW_COUNT);

    std::vector<std::size_t> rows(ROW_COUNT);
    std::iota(rows.begin(), rows.end(), 0);
    std::vector<std::size_t> cols;
    const RowSource src{
        .table = &table,
        .sourceRows = rows,
        .visibleColumns = cols,
        .includeAllFieldsForJson = true,
        .includeHeaderRow = false,
    };

    const ScopedTempDir dir;
    const auto destination = dir.FilePath("cancel_target.jsonl");
    const std::filesystem::path tempPath = destination.string() + ".tmp";

    loglib::StopSource stopSource;
    stopSource.request_stop();

    {
        loglib::exports::FileSink sink(destination);
        auto exporter = loglib::exports::MakeExporter(ExportFormat::JsonLines);
        bool threw = false;
        try
        {
            exporter->Run(src, sink, stopSource.get_token());
        }
        catch (const loglib::exports::ExportCancelled &)
        {
            threw = true;
        }
        REQUIRE(threw);
    }
    REQUIRE_FALSE(std::filesystem::exists(destination));
    REQUIRE_FALSE(std::filesystem::exists(tempPath));
}

TEST_CASE("exports: FileSink atomically renames the temp file on Finish", "[exports][row_exporter]")
{
    const ScopedTempDir dir;
    const auto destination = dir.FilePath("atomic.txt");
    const std::filesystem::path tempPath = destination.string() + ".tmp";

    {
        loglib::exports::FileSink sink(destination);
        REQUIRE(std::filesystem::exists(tempPath));
        REQUIRE_FALSE(std::filesystem::exists(destination));
        sink.Write(std::string_view("hello world\n"));
        sink.Finish();
        REQUIRE(sink.Finished());
    }
    REQUIRE(std::filesystem::exists(destination));
    REQUIRE_FALSE(std::filesystem::exists(tempPath));
    REQUIRE(ReadFile(destination) == std::string("hello world\n"));
}

TEST_CASE("exports: ExtensionFor and LabelFor cover every format", "[exports]")
{
    REQUIRE(std::string_view(loglib::exports::ExtensionFor(ExportFormat::JsonLines)) == "jsonl");
    REQUIRE(std::string_view(loglib::exports::ExtensionFor(ExportFormat::Csv)) == "csv");
    REQUIRE(std::string_view(loglib::exports::ExtensionFor(ExportFormat::Snapshot)) == "log");
    REQUIRE(std::string_view(loglib::exports::ExtensionFor(ExportFormat::Markdown)) == "md");
    REQUIRE(std::string_view(loglib::exports::LabelFor(ExportFormat::JsonLines)) == "JSON Lines");
    REQUIRE(std::string_view(loglib::exports::LabelFor(ExportFormat::Csv)) == "CSV");
    REQUIRE(std::string_view(loglib::exports::LabelFor(ExportFormat::Snapshot)) == "Source snapshot");
    REQUIRE(std::string_view(loglib::exports::LabelFor(ExportFormat::Markdown)) == "Markdown table");
}
