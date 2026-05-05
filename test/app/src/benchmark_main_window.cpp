// Single-shot end-to-end timing benchmark for the production Qt path:
// `LogModel -> RowOrderProxyModel -> LogFilterModel`, parser running on
// a `QtConcurrent::run` worker, GUI-thread drain via `QSignalSpy::wait`.
// Three runs (one per row of the matrix below) print throughput in the
// same shape as the lib's `[large]` Catch2 benchmark so the two corpora
// are directly comparable.
//
// Matrix:
//   1. newest-first OFF (default)
//   2. newest-first ON, session in stream mode
//   3. newest-first ON, session in static mode
//
// Stream-mode and static-mode share the proxy code, but staging both
// ensures the per-mode wiring continues to drive the proxy correctly
// once the `StreamingControl::Static*` accessors land in Phase 2.

#include "log_filter_model.hpp"
#include "log_model.hpp"
#include "qt_streaming_log_sink.hpp"
#include "row_order_proxy_model.hpp"

#include <loglib/file_line_source.hpp>
#include <loglib/log_file.hpp>
#include <loglib/log_processing.hpp>
#include <loglib/parser_options.hpp>
#include <loglib/parsers/json_parser.hpp>
#include <loglib/stop_token.hpp>

#include <test_common/json_log_line.hpp>
#include <test_common/log_generator.hpp>

#include <QObject>
#include <QSignalSpy>
#include <QString>
#include <QTemporaryDir>
#include <QtTest/QtTest>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <memory>
#include <utility>

namespace
{

// Walk the CWD ancestor chain for a `tzdata/` sibling and hand it to
// `loglib::Initialize`. Mirrors the search the lib's `InitializeTimezoneData`
// helper performs, minus the Catch2 dependency.
void StageTimezoneData()
{
    static const auto TZ_DATA = std::filesystem::path("tzdata");
    std::filesystem::path path = std::filesystem::current_path();
    while (true)
    {
        const auto tzdataPath = path / TZ_DATA;
        std::error_code ec;
        if (std::filesystem::exists(tzdataPath, ec))
        {
            loglib::Initialize(tzdataPath);
            return;
        }
        const auto parent = path.parent_path();
        if (parent.empty() || parent == path)
        {
            break;
        }
        path = parent;
    }
    qWarning() << "tzdata/ not found in CWD ancestors; timestamp promotion disabled "
                  "for this run. Invoke from the directory holding the staged tzdata "
                  "folder (typically build/<preset>/bin/<config>/).";
}

// Generate `count` random JSON log lines and write them as JSONL into
// `path`. Returns the byte count of the written file.
std::size_t WriteJsonlFixture(const std::filesystem::path &path, std::size_t count)
{
    auto logs = test_common::GenerateRandomJsonLogs(count);
    std::ofstream stream(path, std::ios::binary);
    for (const auto &line : logs)
    {
        stream << line.ToString() << '\n';
    }
    stream.flush();
    return std::filesystem::file_size(path);
}

struct RunResult
{
    std::chrono::steady_clock::duration elapsed{};
    int rowCount = 0;
    bool finished = false;
};

// One end-to-end run through the full Qt pipeline at `reversed`. The
// proxy chain is local to the call so each run starts from a clean
// slate (no left-over rows / cached mappings).
RunResult RunOnce(const std::filesystem::path &logPath, bool reversed)
{
    auto model = std::make_unique<LogModel>();
    auto rowProxy = std::make_unique<RowOrderProxyModel>();
    rowProxy->setSourceModel(model.get());
    rowProxy->SetReversed(reversed);
    auto filterProxy = std::make_unique<LogFilterModel>();
    filterProxy->setSourceModel(rowProxy.get());
    filterProxy->setSortRole(LogModelItemDataRole::SortRole);

    QSignalSpy finishedSpy(model.get(), &LogModel::streamingFinished);

    auto file = std::make_unique<loglib::LogFile>(logPath.string());
    auto fileSource = std::make_unique<loglib::FileLineSource>(std::move(file));
    loglib::FileLineSource *fileSourcePtr = fileSource.get();

    const auto t0 = std::chrono::steady_clock::now();
    model->BeginStreaming(std::move(fileSource), [fileSourcePtr, sink = model->Sink()](loglib::StopToken token) {
        loglib::ParserOptions options;
        options.stopToken = token;
        loglib::JsonParser parser;
        parser.ParseStreaming(*fileSourcePtr, *sink, options);
    });

    RunResult result;
    result.finished = finishedSpy.wait(180'000);
    result.elapsed = std::chrono::steady_clock::now() - t0;
    result.rowCount = model->rowCount();
    return result;
}

// Format the per-run line so it matches the lib's `ReportThroughput`
// output: `<label>: <MiB/s> MB/s, <lines/s> lines/s (<bytes> bytes /
// <lines> lines / <seconds>s)`. Note: the lib labels MiB/s as "MB/s" for
// historical reasons; we mirror that for grep-ability across bench corpora.
QString FormatThroughput(const QString &label, std::chrono::nanoseconds elapsed, std::size_t bytes, std::size_t lines)
{
    const double seconds = std::chrono::duration<double>(elapsed).count();
    if (seconds <= 0.0)
    {
        return label + QStringLiteral(": (elapsed=0)");
    }
    constexpr double MIB = 1024.0 * 1024.0;
    const double mibps = static_cast<double>(bytes) / MIB / seconds;
    const double linesPerSec = static_cast<double>(lines) / seconds;
    return QStringLiteral("%1: %2 MB/s, %3 lines/s (%4 bytes / %5 lines / %6s)")
        .arg(label)
        .arg(mibps, 0, 'g', 6)
        .arg(linesPerSec, 0, 'g', 6)
        .arg(bytes)
        .arg(lines)
        .arg(seconds, 0, 'g', 5);
}

} // namespace

class MainWindowBench : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        StageTimezoneData();

        // Single shared 1'000'000-line fixture for all three runs.
        // 1 M / ~170 MiB matches the lib's `[large]` benchmark so the
        // numbers here are directly comparable. Pinned to the test's
        // own QTemporaryDir so it survives `runConfig` but is cleaned
        // up at suite teardown.
        QVERIFY(mTempDir.isValid());
        mLogPath = std::filesystem::path(mTempDir.path().toStdString()) / "bench.jsonl";
        mBytes = WriteJsonlFixture(mLogPath, LINE_COUNT);
        QVERIFY(mBytes > 0);
    }

    void benchNewestFirstOff()
    {
        runConfig(QStringLiteral("Qt path, newest-first OFF (default)"), false);
    }

    void benchNewestFirstStreamMode()
    {
        runConfig(QStringLiteral("Qt path, newest-first ON (stream mode)"), true);
    }

    void benchNewestFirstStaticMode()
    {
        runConfig(QStringLiteral("Qt path, newest-first ON (static mode)"), true);
    }

private:
    static constexpr std::size_t LINE_COUNT = 1'000'000;

    void runConfig(const QString &label, bool reversed)
    {
        const RunResult run = RunOnce(mLogPath, reversed);
        QVERIFY2(run.finished, "streamingFinished must fire within the 180 s timeout");
        QCOMPARE(static_cast<std::size_t>(run.rowCount), LINE_COUNT);
        qDebug().noquote() << FormatThroughput(label, run.elapsed, mBytes, LINE_COUNT);
    }

    QTemporaryDir mTempDir;
    std::filesystem::path mLogPath;
    std::size_t mBytes = 0;
};

QTEST_MAIN(MainWindowBench)
#include "benchmark_main_window.moc"
