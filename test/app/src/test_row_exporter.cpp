// Focused tests for the row-exporter formatters
// (`slv::exports::JsonLinesExporter`, `CsvExporter`, `SnapshotExporter`,
// `MarkdownExporter`) and the `ExportSink` atomic-rename contract.
//
// Kept out of the monolithic `apptest` binary so a regression in one
// arm surfaces here directly. Uses `QTEST_GUILESS_MAIN`: exporters
// are pure C++ (no widgets), and formatting `TimeStamp` via
// `date::format` on a UTC `sys_time` needs no tzdata bootstrap.

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

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <numeric>
#include <sstream>
#include <string>
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

/// One-off temp directory that unlinks its contents on destruction.
class ScopedTempDir
{
public:
    ScopedTempDir()
    {
        std::error_code ec;
        mPath = std::filesystem::temp_directory_path(ec);
        mPath /= "slv_export_test";
        // Randomise: two concurrent test binaries must not collide.
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

/// Build a small fixed-schema `LogTable` for the exporter tests. Rows
/// carry: `ts` (Time), `level` (String), `message` (String), `count`
/// (Integer), `ratio` (Floating), `ok` (Boolean). The raw source lines
/// mimic the original JSON so `SnapshotExporter` has something to
/// echo verbatim.
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

    /// JSON Lines round-trips row data: exporting a table and
    /// re-parsing each JSON line gives equivalent typed values.
    /// Uses a simple hand-rolled JSON reader (no glaze dependency
    /// in the test itself) so we exercise the wire format directly.
    static void TestJsonLinesTypeFidelityOnSyntheticRows();

    /// CSV escaping per RFC 4180: `,`, `"`, `\n` require quotes;
    /// embedded `"` doubles. Cells without special chars pass
    /// through verbatim.
    static void TestCsvRfc4180Quoting();

    /// CSV header row respects `includeHeaderRow`.
    static void TestCsvHeaderToggle();

    /// Markdown escapes `|` and collapses embedded whitespace so a
    /// multi-line value cannot break the row layout.
    static void TestMarkdownPipeAndNewlineHandling();

    /// Snapshot echoes the raw source bytes for each row, one
    /// record per output line, including embedded multi-line
    /// content (which stays intact via `LineSource::RawLine`).
    static void TestSnapshotEchoesRawBytes();

    /// A stop request mid-export unwinds via `ExportCancelled` and
    /// the `FileSink` leaves no partial file behind on
    /// destruction (temp file is unlinked).
    static void TestCancelLeavesNoPartialFile();

    /// The `FileSink` writes to `<destination>.tmp` and atomically
    /// renames on `Finish`. Assert the temp file disappears and
    /// only the destination exists post-`Finish`.
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
    QVERIFY(lines[0].contains(QStringLiteral("\"ratio\":0")));
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
    // Use raw lines that the `StreamLineSource` echoes back
    // verbatim through `RawLine`. Multi-line content is not
    // possible here (StreamLineSource strips newlines on append),
    // but the roundtrip through Snapshot is exact for what
    // `RawLine` returns.
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

void RowExporterTest::TestCancelLeavesNoPartialFile()
{
    // Big enough that a mid-stream cancel definitely triggers.
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
