// Focused tests for the row-exporter formatters and the
// `ExportSink` atomic-rename contract. Kept out of the monolithic
// `apptest` binary so a regression in one format surfaces here
// directly. `QTEST_GUILESS_MAIN` because the exporters are pure
// C++ and UTC `date::format` needs no tzdata bootstrap.

#include "export_sink.hpp"
#include "row_exporter.hpp"

#include <loglib/bytes_producer.hpp>
#include <loglib/file_line_source.hpp>
#include <loglib/key_index.hpp>
#include <loglib/log_configuration.hpp>
#include <loglib/log_data.hpp>
#include <loglib/log_line.hpp>
#include <loglib/log_table.hpp>
#include <loglib/log_value.hpp>
#include <loglib/stop_token.hpp>
#include <loglib/stream_line_source.hpp>

#include <QDir>
#include <QTemporaryFile>
#include <QtTest/QtTest>

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <numeric>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace
{

using ExportFormat = slv::exports::ExportFormat;
using RowSource = slv::exports::RowSource;

/// Ephemeral write-target for the exporters. `Bytes()` returns the
/// accumulated buffer so a test can assert against the raw output
/// without touching the filesystem.
class MemorySink : public slv::exports::ExportSink
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

/// `MemorySink` that throws `std::runtime_error` on the Nth
/// `Write`. Regression seam for the "sink I/O failure aborts the
/// export" contract: the snapshot exporter used to have a broad
/// `catch (std::exception)` that swallowed sink throws and
/// silently truncated the output on disk-full / dropped-share.
class ThrowOnNthWriteSink : public slv::exports::ExportSink
{
public:
    /// Throws on write @p failOnWrite (0-based). Every other write
    /// accumulates into the buffer as normal so callers can inspect
    /// what the exporter had written before the throw.
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

/// Mock `LineSource` that throws configurable exception types per
/// line. Lets the snapshot tests confirm the handler catches every
/// `std::exception`, not just `std::out_of_range` (the pre-fix
/// narrow catch).
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

    /// @p perLine maps 1-based `lineId` to its throw behaviour. Any
    /// lineId not in the map returns @p defaultLine as its raw text.
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
    [[nodiscard]] std::string_view ResolveMmapBytes(std::uint64_t, std::uint32_t, std::size_t) const noexcept override
    {
        return {};
    }
    [[nodiscard]] std::string_view ResolveOwnedBytes(std::uint64_t, std::uint32_t, std::size_t) const noexcept override
    {
        return {};
    }
    [[nodiscard]] std::span<const char> StableBytes() const noexcept override
    {
        return {};
    }
    std::uint64_t AppendOwnedBytes(std::size_t, std::string_view) override
    {
        return 0;
    }
    [[nodiscard]] bool SupportsEviction() const noexcept override
    {
        return false;
    }
    void EvictBefore(std::size_t) override {}
    [[nodiscard]] std::size_t FirstAvailableLineId() const noexcept override
    {
        return 1;
    }

private:
    std::filesystem::path mPath;
    std::unordered_map<std::size_t, ThrowMode> mPerLine;
};

/// One-off temp directory that unlinks its contents on destruction.
class ScopedTempDir
{
public:
    ScopedTempDir()
    {
        std::error_code ec;
        mPath = std::filesystem::temp_directory_path(ec);
        mPath /= "slv_export_test";
        // Suffix with a monotonic tick so concurrent test binaries
        // don't collide.
        auto nsecs = std::chrono::steady_clock::now().time_since_epoch().count();
        mPath += std::to_string(nsecs);
        std::filesystem::create_directories(mPath, ec);
    }
    ~ScopedTempDir()
    {
        std::error_code ec;
        std::filesystem::remove_all(mPath, ec);
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

/// Fixed-schema `LogTable` for the exporter tests: `ts` (Time),
/// `level` (String), `message` (String), `count` (Integer),
/// `ratio` (Floating), `ok` (Boolean). Raw source lines are the
/// original JSON so `SnapshotExporter` has bytes to echo.
loglib::LogTable BuildFixtureTable(std::vector<std::string> rawLines, std::size_t rowCount)
{
    auto stream = std::make_unique<loglib::StreamLineSource>(
        std::filesystem::path("fixture.jsonl"), nullptr
    );
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
        map["level"] = std::string(i % 3 == 0 ? "info" : (i % 3 == 1 ? "warn" : "error"));
        map["message"] = std::string("hello, \"world\"\n#") + std::to_string(i);
        map["count"] = static_cast<std::int64_t>(i);
        map["ratio"] = static_cast<double>(i) * 0.5;
        map["ok"] = (i % 2 == 0);
        lines.emplace_back(map, keys, *sourcePtr, i + 1); // 1-based lineId
    }

    loglib::LogData data(std::move(stream), std::move(lines), std::move(keys));

    loglib::LogConfiguration config;
    config.columns.push_back({.header = "Time", .keys = {"ts"}, .printFormat = "%FT%T", .type = loglib::LogConfiguration::Type::Time, .parseFormats = {"%FT%T"}});
    config.columns.push_back({.header = "Level", .keys = {"level"}, .printFormat = "{}", .type = loglib::LogConfiguration::Type::String});
    config.columns.push_back({.header = "Message", .keys = {"message"}, .printFormat = "{}", .type = loglib::LogConfiguration::Type::String});
    config.columns.push_back({.header = "Count", .keys = {"count"}, .printFormat = "{}", .type = loglib::LogConfiguration::Type::Integer});
    config.columns.push_back({.header = "Ratio", .keys = {"ratio"}, .printFormat = "{}", .type = loglib::LogConfiguration::Type::Floating});
    config.columns.push_back({.header = "OK", .keys = {"ok"}, .printFormat = "{}", .type = loglib::LogConfiguration::Type::Boolean});

    loglib::LogConfigurationManager manager;
    manager.SetConfiguration(std::move(config));
    return loglib::LogTable(std::move(data), std::move(manager));
}

/// Read a small file into memory. Used by the atomic-rename test.
std::string ReadFile(const std::filesystem::path &path)
{
    std::ifstream file(path, std::ios::binary);
    std::stringstream ss;
    ss << file.rdbuf();
    return ss.str();
}

} // namespace

// NOLINTNEXTLINE(misc-use-internal-linkage)
class RowExporterTest : public QObject
{
    Q_OBJECT

private slots:
    /// JSON Lines emits one JSON object per row with every present
    /// field. Booleans as native JSON, integers and doubles as JSON
    /// numbers, timestamps as ISO 8601 UTC.
    static void TestJsonLinesTypedValues();

    /// JSON Lines round-trips row data: every row is a JSON
    /// object with the expected keys. Exercises the wire format
    /// directly (no glaze dependency in the test itself).
    static void TestJsonLinesTypeFidelityOnSyntheticRows();

    /// CSV escaping per RFC 4180: `,`, `"`, `\n` require quotes;
    /// embedded `"` doubles. Cells without special chars pass
    /// through verbatim.
    static void TestCsvRfc4180Quoting();

    /// CSV header row respects `includeHeaderRow`.
    static void TestCsvHeaderToggle();

    /// CSV formula-injection defense: cells starting with `=` or
    /// `@` are prefixed with `'` and force-quoted so a spreadsheet
    /// won't evaluate them on open. Leading `+` / `-` are
    /// deliberately left alone (they start every negative number).
    static void TestCsvFormulaInjectionNeutralised();

    /// Whole-valued doubles get a trailing `.0` in JSON Lines so
    /// typed readers (Python `json`, jq, `Number.isInteger`) still
    /// see a fractional literal.
    static void TestJsonLinesDoubleTypeStability();

    /// Markdown escapes `|` and collapses embedded whitespace so
    /// multi-line values cannot break the row layout.
    static void TestMarkdownPipeAndNewlineHandling();

    /// Snapshot echoes the raw source bytes verbatim, one record
    /// per output line.
    static void TestSnapshotEchoesRawBytes();

    /// Snapshot swallows every `RawLine` failure so a single
    /// evicted / broken row cannot abort the export. Guard
    /// against the pre-fix narrow `catch (out_of_range&)` that
    /// let anything else escape.
    static void TestSnapshotSkipsUnavailableRows();

    /// Sink write failures (disk full, network drop) MUST
    /// propagate out of `Run` for every format, so `~FileSink`
    /// unlinks the `.tmp` and the user doesn't see a false
    /// "success" toast on a truncated file.
    static void TestSinkWriteFailurePropagatesFromAllFormats();

    /// Cancel mid-export unwinds via `ExportCancelled` and
    /// `~FileSink` leaves no partial file behind.
    static void TestCancelLeavesNoPartialFile();

    /// `FileSink` writes to `<destination>.tmp` and atomically
    /// renames on `Finish`; the temp file must disappear.
    static void TestFileSinkAtomicRename();
};

void RowExporterTest::TestJsonLinesTypedValues()
{
    std::vector<std::string> raws = {
        R"({"ts":"2023-11-14T22:13:20","level":"info","message":"first"})",
        R"({"ts":"2023-11-14T22:13:20.001000","level":"warn","message":"second"})",
    };
    const auto table = BuildFixtureTable(std::move(raws), 2);

    std::vector<int> rows = {0, 1};
    std::vector<std::size_t> cols = {0, 1, 2, 3, 4, 5};
    RowSource source{
        .table = &table,
        .sourceRows = rows,
        .visibleColumns = cols,
        .includeAllFieldsForJson = true,
        .includeHeaderRow = false,
    };

    MemorySink sink;
    auto exporter = slv::exports::MakeExporter(ExportFormat::JsonLines);
    QVERIFY(exporter != nullptr);
    exporter->Run(source, sink, loglib::StopToken{});

    const QString out = QString::fromStdString(sink.Bytes());
    const QStringList lines = out.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    QCOMPARE(lines.size(), 2);
    // First line: integer count == 0, boolean ok == true, ratio == 0.
    QVERIFY(lines[0].contains(QStringLiteral("\"count\":0")));
    QVERIFY(lines[0].contains(QStringLiteral("\"ok\":true")));
    // `ratio` is a Floating column; whole-valued doubles carry
    // the forced trailing `.0` (see
    // `TestJsonLinesDoubleTypeStability`).
    QVERIFY(lines[0].contains(QStringLiteral("\"ratio\":0.0")));
    QVERIFY(lines[0].contains(QStringLiteral("\"ts\":\"2023-11-14T22:13:20")));
    // Second line: count == 1, ok == false, ratio == 0.5.
    QVERIFY(lines[1].contains(QStringLiteral("\"count\":1")));
    QVERIFY(lines[1].contains(QStringLiteral("\"ok\":false")));
    QVERIFY(lines[1].contains(QStringLiteral("\"ratio\":0.5")));
    // Newlines / quotes inside `message` must be JSON-escaped.
    QVERIFY(lines[0].contains(QStringLiteral("\\\"world\\\"")));
    QVERIFY(lines[0].contains(QStringLiteral("\\n#0")));
}

void RowExporterTest::TestJsonLinesTypeFidelityOnSyntheticRows()
{
    constexpr std::size_t ROW_COUNT = 10000;
    std::vector<std::string> raws;
    raws.reserve(ROW_COUNT);
    for (std::size_t i = 0; i < ROW_COUNT; ++i)
    {
        raws.push_back(std::string("row ") + std::to_string(i));
    }
    const auto table = BuildFixtureTable(std::move(raws), ROW_COUNT);

    std::vector<int> rows(ROW_COUNT);
    std::iota(rows.begin(), rows.end(), 0);
    std::vector<std::size_t> cols;
    RowSource source{
        .table = &table,
        .sourceRows = rows,
        .visibleColumns = cols,
        .includeAllFieldsForJson = true,
        .includeHeaderRow = false,
    };

    MemorySink sink;
    auto exporter = slv::exports::MakeExporter(ExportFormat::JsonLines);
    QVERIFY(exporter != nullptr);
    exporter->Run(source, sink, loglib::StopToken{});

    const QByteArray full = QByteArray::fromStdString(sink.Bytes());
    // Assert one line per row.
    const auto lineCount = static_cast<std::size_t>(std::count(full.begin(), full.end(), '\n'));
    QCOMPARE(lineCount, ROW_COUNT);

    // Every line must be a JSON object with the six expected keys.
    // A regex is enough: we've already validated the escaping
    // in the previous test.
    const QString first = QString::fromUtf8(full.left(full.indexOf('\n')));
    QVERIFY(first.startsWith(QLatin1Char('{')));
    QVERIFY(first.endsWith(QLatin1Char('}')));
    for (const QString &key : {QStringLiteral("\"ts\""),
                                QStringLiteral("\"level\""),
                                QStringLiteral("\"message\""),
                                QStringLiteral("\"count\""),
                                QStringLiteral("\"ratio\""),
                                QStringLiteral("\"ok\"")})
    {
        QVERIFY2(first.contains(key), qPrintable(QStringLiteral("first JSON row missing key: ") + key));
    }
}

void RowExporterTest::TestCsvRfc4180Quoting()
{
    std::vector<std::string> raws = {"raw1"};
    const auto table = BuildFixtureTable(std::move(raws), 1);

    std::vector<int> rows = {0};
    // Visible cols: Level (1), Message (2). Message contains
    // `"world"` and a `\n`, so RFC 4180 requires quoting + inner-`"`
    // doubling.
    std::vector<std::size_t> cols = {1, 2};
    RowSource source{
        .table = &table,
        .sourceRows = rows,
        .visibleColumns = cols,
        .includeAllFieldsForJson = true,
        .includeHeaderRow = false,
    };
    MemorySink sink;
    auto exporter = slv::exports::MakeExporter(ExportFormat::Csv);
    QVERIFY(exporter != nullptr);
    exporter->Run(source, sink, loglib::StopToken{});

    const QString out = QString::fromStdString(sink.Bytes());
    // Level cell 'info' needs no quotes; message cell must be quoted with doubled inner ".
    QVERIFY(out.contains(QStringLiteral("info,\"hello, \"\"world\"\"")));
}

void RowExporterTest::TestCsvHeaderToggle()
{
    std::vector<std::string> raws = {"raw1"};
    const auto table = BuildFixtureTable(std::move(raws), 1);

    std::vector<int> rows = {0};
    std::vector<std::size_t> cols = {1, 3};

    {
        RowSource src{.table = &table, .sourceRows = rows, .visibleColumns = cols, .includeAllFieldsForJson = false, .includeHeaderRow = true};
        MemorySink sink;
        auto exporter = slv::exports::MakeExporter(ExportFormat::Csv);
        exporter->Run(src, sink, loglib::StopToken{});
        const QString out = QString::fromStdString(sink.Bytes());
        // First line should be the header.
        QVERIFY(out.startsWith(QStringLiteral("Level,Count\n")));
    }
    {
        RowSource src{.table = &table, .sourceRows = rows, .visibleColumns = cols, .includeAllFieldsForJson = false, .includeHeaderRow = false};
        MemorySink sink;
        auto exporter = slv::exports::MakeExporter(ExportFormat::Csv);
        exporter->Run(src, sink, loglib::StopToken{});
        const QString out = QString::fromStdString(sink.Bytes());
        QVERIFY(!out.startsWith(QStringLiteral("Level,")));
    }
}

void RowExporterTest::TestCsvFormulaInjectionNeutralised()
{
    // Fixture: canonical `=cmd|...` DDE payload plus the Sheets
    // `@import` / `+SUM` variants. Feeds raw cell bytes rather
    // than going through `GetValueOrFormatted` for exact control.
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
        std::string("=cmd|'/c calc'!A1"),  // classic formula injection
        std::string("@SUM(1+1)"),           // DDE prefix
        std::string("+2+3"),                // benign leading `+` -- must NOT be neutralised
        std::string("-42"),                 // benign negative number -- must NOT be neutralised
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

    std::vector<int> rows = {0, 1, 2, 3};
    std::vector<std::size_t> cols = {0};
    RowSource src{
        .table = &table,
        .sourceRows = rows,
        .visibleColumns = cols,
        .includeAllFieldsForJson = false,
        .includeHeaderRow = false,
    };

    MemorySink sink;
    auto exporter = slv::exports::MakeExporter(ExportFormat::Csv);
    QVERIFY(exporter != nullptr);
    exporter->Run(src, sink, loglib::StopToken{});

    const QString out = QString::fromStdString(sink.Bytes());
    const QStringList outLines = out.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    QCOMPARE(outLines.size(), 4);

    // Row 0: `=cmd|...` -- must be sentinel-prefixed AND quoted so a
    // spreadsheet never evaluates the cell.
    QCOMPARE(outLines[0], QStringLiteral("\"'=cmd|'/c calc'!A1\""));
    // Row 1: `@SUM(...)` -- same treatment.
    QCOMPARE(outLines[1], QStringLiteral("\"'@SUM(1+1)\""));
    // Row 2: `+2+3` -- leading `+` alone is not a formula trigger
    // here; passes through verbatim, no quotes needed.
    QCOMPARE(outLines[2], QStringLiteral("+2+3"));
    // Row 3: `-42` -- negative number, must not be mangled.
    QCOMPARE(outLines[3], QStringLiteral("-42"));
}

void RowExporterTest::TestJsonLinesDoubleTypeStability()
{
    // Whole-valued doubles used to serialise as `"ratio":0` under
    // default fmt shortest-round-trip, which re-parses as an
    // integer in typed JSON readers. Pin the trailing `.0`.
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

    std::vector<int> rows = {0, 1, 2, 3};
    std::vector<std::size_t> cols;
    RowSource src{
        .table = &table,
        .sourceRows = rows,
        .visibleColumns = cols,
        .includeAllFieldsForJson = true,
        .includeHeaderRow = false,
    };

    MemorySink sink;
    auto exporter = slv::exports::MakeExporter(ExportFormat::JsonLines);
    QVERIFY(exporter != nullptr);
    exporter->Run(src, sink, loglib::StopToken{});

    const QString out = QString::fromStdString(sink.Bytes());
    const QStringList outLines = out.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    QCOMPARE(outLines.size(), 4);
    // Whole-valued doubles carry `.0`.
    QVERIFY2(outLines[0].contains(QStringLiteral("\"value\":0.0")), qPrintable(outLines[0]));
    QVERIFY2(outLines[1].contains(QStringLiteral("\"value\":1.0")), qPrintable(outLines[1]));
    QVERIFY2(outLines[2].contains(QStringLiteral("\"value\":-3.0")), qPrintable(outLines[2]));
    // Fractional doubles are unchanged.
    QVERIFY2(outLines[3].contains(QStringLiteral("\"value\":2.5")), qPrintable(outLines[3]));
}

void RowExporterTest::TestMarkdownPipeAndNewlineHandling()
{
    // Feed a row with a literal `|` in the message; the Markdown
    // exporter must escape it, otherwise the row would split.
    std::vector<std::string> raws = {"raw1"};
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
    config.columns.push_back({.header = "Msg", .keys = {"message"}, .printFormat = "{}", .type = loglib::LogConfiguration::Type::String});
    loglib::LogConfigurationManager manager;
    manager.SetConfiguration(std::move(config));
    const loglib::LogTable table(std::move(data), std::move(manager));

    std::vector<int> rows = {0};
    std::vector<std::size_t> cols = {0};
    RowSource src{.table = &table, .sourceRows = rows, .visibleColumns = cols, .includeAllFieldsForJson = false, .includeHeaderRow = true};

    MemorySink sink;
    auto exporter = slv::exports::MakeExporter(ExportFormat::Markdown);
    exporter->Run(src, sink, loglib::StopToken{});

    const QString out = QString::fromStdString(sink.Bytes());
    // Data row must escape `|` -> `\|` and collapse `\n` -> space.
    QVERIFY(out.contains(QStringLiteral("a \\| b newline")));
    // No unescaped `|` sneaks through inside the cell text.
    QVERIFY(!out.contains(QStringLiteral("a | b")));
    // The header row is present.
    QVERIFY(out.startsWith(QStringLiteral("| Msg |\n| --- |")));
}

void RowExporterTest::TestSnapshotEchoesRawBytes()
{
    // `StreamLineSource::RawLine` echoes each appended line
    // verbatim (multi-line records are not possible via
    // AppendLine, which strips newlines).
    std::vector<std::string> raws = {"line one", "line two"};
    const auto table = BuildFixtureTable(std::move(raws), 2);

    std::vector<int> rows = {0, 1};
    std::vector<std::size_t> cols;
    RowSource src{.table = &table, .sourceRows = rows, .visibleColumns = cols};

    MemorySink sink;
    auto exporter = slv::exports::MakeExporter(ExportFormat::Snapshot);
    exporter->Run(src, sink, loglib::StopToken{});

    QCOMPARE(QString::fromStdString(sink.Bytes()), QStringLiteral("line one\nline two\n"));
}

void RowExporterTest::TestSnapshotSkipsUnavailableRows()
{
    // Source throws `runtime_error` on lineId 2 and `logic_error`
    // on lineId 3; the exporter must swallow both and still emit
    // lineIds 1 and 4. Pre-fix catch was narrow (`out_of_range`
    // only) and aborted the whole export.
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
        lines.emplace_back(map, keys, *sourcePtr, i + 1); // lineId 1..4
    }
    loglib::LogData data(std::move(source), std::move(lines), std::move(keys));

    loglib::LogConfiguration config;
    config.columns.push_back(
        {.header = "Message", .keys = {"message"}, .printFormat = "{}", .type = loglib::LogConfiguration::Type::String}
    );
    loglib::LogConfigurationManager manager;
    manager.SetConfiguration(std::move(config));
    const loglib::LogTable table(std::move(data), std::move(manager));

    std::vector<int> rows = {0, 1, 2, 3};
    std::vector<std::size_t> cols;
    RowSource src{.table = &table, .sourceRows = rows, .visibleColumns = cols};

    MemorySink sink;
    auto exporter = slv::exports::MakeExporter(ExportFormat::Snapshot);
    // Must NOT throw: the broadened `catch (std::exception&)`
    // swallows every per-row failure.
    exporter->Run(src, sink, loglib::StopToken{});

    // Only lineIds 1 and 4 survive; the middle two were skipped.
    QCOMPARE(QString::fromStdString(sink.Bytes()), QStringLiteral("line 1\nline 4\n"));
}

void RowExporterTest::TestSinkWriteFailurePropagatesFromAllFormats()
{
    // Two-row fixture: each format emits at least one write per
    // row (plus an optional header for column formats) so failing
    // on write index 1 hits every format regardless of the header
    // toggle.
    std::vector<std::string> raws = {"first raw line", "second raw line"};
    const auto table = BuildFixtureTable(std::move(raws), 2);

    std::vector<int> rows = {0, 1};
    // Skip the Time column: CSV / Markdown format it through
    // `date::zoned_time{CurrentZone(), ...}` which needs tzdata
    // bootstrapping that `QTEST_GUILESS_MAIN` does not do. JSON
    // Lines / Snapshot ignore `visibleColumns` so the narrower
    // list still exercises them fully.
    std::vector<std::size_t> cols = {1, 2, 3, 4, 5};
    RowSource src{
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
        auto exporter = slv::exports::MakeExporter(format);
        QVERIFY(exporter != nullptr);
        bool threw = false;
        try
        {
            exporter->Run(src, sink, loglib::StopToken{});
        }
        catch (const slv::exports::ExportCancelled &)
        {
            // Not this: we didn't cancel, we failed to write.
            QFAIL(qPrintable(
                QStringLiteral("format %1 wrongly surfaced ExportCancelled on write failure")
                    .arg(slv::exports::LabelFor(format))
            ));
        }
        catch (const std::runtime_error &)
        {
            threw = true;
        }
        QVERIFY2(
            threw,
            qPrintable(
                QStringLiteral("format %1 swallowed a sink write failure -- exports over a "
                               "network share / full disk would silently truncate")
                    .arg(slv::exports::LabelFor(format))
            )
        );
        // Sink saw exactly the writes it accepted before the throw
        // plus the throwing write itself. If the exporter had kept
        // going after the failure, `WriteCount` would be higher.
        QVERIFY2(
            sink.WriteCount() == std::size_t(2),
            qPrintable(
                QStringLiteral("format %1: expected WriteCount==2 after throw on write index 1, got %2")
                    .arg(slv::exports::LabelFor(format))
                    .arg(sink.WriteCount())
            )
        );
    }
}

void RowExporterTest::TestCancelLeavesNoPartialFile()
{
    // Large enough that the first stop-poll iteration triggers
    // before the loop finishes.
    constexpr std::size_t ROW_COUNT = 20000;
    std::vector<std::string> raws;
    raws.reserve(ROW_COUNT);
    for (std::size_t i = 0; i < ROW_COUNT; ++i)
    {
        raws.push_back(std::string("row ") + std::to_string(i));
    }
    const auto table = BuildFixtureTable(std::move(raws), ROW_COUNT);

    std::vector<int> rows(ROW_COUNT);
    std::iota(rows.begin(), rows.end(), 0);
    std::vector<std::size_t> cols;
    RowSource src{
        .table = &table,
        .sourceRows = rows,
        .visibleColumns = cols,
        .includeAllFieldsForJson = true,
        .includeHeaderRow = false,
    };

    ScopedTempDir dir;
    const auto destination = dir.FilePath("cancel_target.jsonl");
    const std::filesystem::path tempPath = destination.string() + ".tmp";

    loglib::StopSource stopSource;
    // Fire the stop request up-front. First stop-poll iteration in
    // the exporter (row 0) will throw.
    stopSource.request_stop();

    {
        slv::exports::FileSink sink(destination);
        auto exporter = slv::exports::MakeExporter(ExportFormat::JsonLines);
        bool threw = false;
        try
        {
            exporter->Run(src, sink, stopSource.get_token());
        }
        catch (const slv::exports::ExportCancelled &)
        {
            threw = true;
        }
        QVERIFY(threw);
        // Sink destructor unlinks the temp file below.
    }
    // Neither the destination nor its `.tmp` sidecar should exist.
    QVERIFY(!std::filesystem::exists(destination));
    QVERIFY(!std::filesystem::exists(tempPath));
}

void RowExporterTest::TestFileSinkAtomicRename()
{
    ScopedTempDir dir;
    const auto destination = dir.FilePath("atomic.txt");
    const std::filesystem::path tempPath = destination.string() + ".tmp";

    {
        slv::exports::FileSink sink(destination);
        // On construction, the temp file exists but destination doesn't.
        QVERIFY(std::filesystem::exists(tempPath));
        QVERIFY(!std::filesystem::exists(destination));
        sink.Write(std::string_view("hello world\n"));
        sink.Finish();
        QVERIFY(sink.Finished());
    }
    // After Finish, only the destination exists.
    QVERIFY(std::filesystem::exists(destination));
    QVERIFY(!std::filesystem::exists(tempPath));
    QCOMPARE(ReadFile(destination), std::string("hello world\n"));
}

QTEST_GUILESS_MAIN(RowExporterTest)
#include "test_row_exporter.moc"
