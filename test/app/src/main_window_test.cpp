#include "log_filter_model.hpp"
#include "log_model.hpp"
#include "log_table_view.hpp"
#include "main_window.hpp"
#include "qt_streaming_log_sink.hpp"
#include "row_order_proxy_model.hpp"
#include "streaming_control.hpp"

#include <loglib/file_line_source.hpp>
#include <loglib/internal/advanced_parser_options.hpp>
#include <loglib/internal/compact_log_value.hpp>
#include <loglib/key_index.hpp>
#include <loglib/log_configuration.hpp>
#include <loglib/log_file.hpp>
#include <loglib/log_line.hpp>
#include <loglib/log_parse_sink.hpp>
#include <loglib/log_value.hpp>
#include <loglib/parser_options.hpp>
#include <loglib/parsers/json_parser.hpp>
#include <loglib/stop_token.hpp>
#include <loglib/stream_line_source.hpp>
#include <loglib/tailing_bytes_producer.hpp>
#include <loglib/tcp_server_producer.hpp>
#include <loglib/udp_server_producer.hpp>

#include <test_common/network_log_client.hpp>

#include <QAction>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QMenu>
#include <QMenuBar>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QScrollBar>
#include <QSignalSpy>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QVariant>
#include <QWheelEvent>
#include <QtTest/QtTest>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace
{

// Tiny RAII helper that writes a JSONL fixture into a QTemporaryDir and
// removes it on destruction. Kept inside this TU to avoid pulling in the
// library tests' TestJsonLogFile helper (which lives in test/lib and would
// add a build-graph dependency from apptest onto loglib's catch2 fixtures).
class TempJsonFile
{
public:
    explicit TempJsonFile(const QStringList &lines)
    {
        QVERIFY2(mDir.isValid(), "QTemporaryDir creation must succeed");
        mPath = mDir.filePath("parity.json");
        std::ofstream stream(mPath.toStdString(), std::ios::binary);
        QVERIFY2(stream.is_open(), "fixture file must be openable");
        for (const QString &line : lines)
        {
            stream << line.toStdString() << '\n';
        }
    }

    QString Path() const
    {
        return mPath;
    }

private:
    QTemporaryDir mDir;
    QString mPath;
};

// Returns a deterministic JSONL fixture used by the
// `*AboutToBeInserted` regression test below. Keys are intentionally
// alphabetic so the parser's per-batch insertion order matches a
// sorted-order column layout, which keeps the assertion shape simple.
QStringList MakeParityFixture()
{
    return QStringList{
        QStringLiteral(R"({"a": "alpha", "b": 1, "c": 3.14})"),
        QStringLiteral(R"({"a": "beta",  "b": 2, "c": 2.71})"),
        QStringLiteral(R"({"a": "gamma", "b": 3, "c": 1.41, "d": true})"),
        QStringLiteral(R"({"a": "delta", "b": 4, "c": 0.0,  "d": false})"),
        QStringLiteral(R"({"a": "eps",   "b": 5})"),
    };
}

// Extracts a Qt resource (e.g. ":/fixtures/empty.jsonl") into a unique
// QTemporaryDir so the parser can mmap it. The directory and the extracted
// copy live for the FixtureFile lifetime.
class FixtureFile
{
public:
    explicit FixtureFile(const QString &resourcePath)
    {
        QVERIFY2(mDir.isValid(), "QTemporaryDir creation must succeed");
        QFile in(resourcePath);
        QVERIFY2(
            in.open(QFile::ReadOnly),
            qPrintable(QStringLiteral("embedded fixture %1 must be readable").arg(resourcePath))
        );
        mPath = mDir.filePath(QFileInfo(resourcePath).fileName());
        QFile out(mPath);
        QVERIFY2(
            out.open(QFile::WriteOnly),
            qPrintable(QStringLiteral("fixture extraction file %1 must be writable").arg(mPath))
        );
        out.write(in.readAll());
    }

    QString Path() const
    {
        return mPath;
    }

private:
    QTemporaryDir mDir;
    QString mPath;
};

// Result of running one fixture through the streaming pipeline. `cancelled`
// is true iff the streaming pipeline reported `StreamingResult::Cancelled`
// (the externally-stopped path); `Success` and `Failed` both produce
// `cancelled == false`. Tests that need to distinguish the latter two
// should read the captured `StreamingResult` directly.
struct StreamingRun
{
    std::unique_ptr<LogModel> model;
    qsizetype finishedCount = 0;
    bool cancelled = false;
};

// Drives one fixture through `LogModel::BeginStreaming` +
// `JsonParser::ParseStreaming` + `QtStreamingLogSink`, mirroring
// `MainWindow::OpenJsonStreaming` but on the calling thread. Pinned to
// `threads=1` so per-batch newKeys / cell ordering is deterministic.
StreamingRun RunStreaming(const QString &fixturePath)
{
    StreamingRun run;
    run.model = std::make_unique<LogModel>();
    QSignalSpy finishedSpy(run.model.get(), &LogModel::streamingFinished);

    auto file = std::make_unique<loglib::LogFile>(fixturePath.toStdString());
    auto fileSource = std::make_unique<loglib::FileLineSource>(std::move(file));
    loglib::FileLineSource *fileSourcePtr = fileSource.get();
    const loglib::StopToken stopToken = run.model->BeginStreamingForSyncTest(std::move(fileSource));

    {
        loglib::ParserOptions options;
        options.stopToken = stopToken;
        loglib::internal::AdvancedParserOptions advanced;
        advanced.threads = 1;

        loglib::JsonParser parser;
        parser.ParseStreaming(*fileSourcePtr, *run.model->Sink(), options, advanced);
    }

    if (finishedSpy.count() == 0)
    {
        finishedSpy.wait(5000);
    }
    run.finishedCount = finishedSpy.count();
    if (run.finishedCount > 0)
    {
        const auto result = finishedSpy.takeFirst().value(0).value<StreamingResult>();
        run.cancelled = (result == StreamingResult::Cancelled);
    }
    return run;
}

// RAII helper for the live-tail smoke test: a temp directory that lives
// for the lifetime of the test, plus an `Append` helper that opens, writes,
// and flushes a string to a file in a single call. We don't share the
// `TempJsonFile` helper because it pre-writes a fixed line list at
// construction; the live-tail flow needs to *grow* the file across the
// test's runtime so the parser actually exercises its tailing path
// (rather than draining a static fixture).
class TempLiveTailFile
{
public:
    TempLiveTailFile()
    {
        QVERIFY2(mDir.isValid(), "QTemporaryDir creation must succeed");
        mPath = mDir.filePath("live.jsonl");
        // Touch the file so `TailingBytesProducer`'s open() succeeds. Pre-fill
        // walks the existing content (zero bytes here) before tailing.
        std::ofstream stream(mPath.toStdString(), std::ios::binary);
        QVERIFY2(stream.is_open(), "live-tail fixture file must be openable");
    }

    QString Path() const
    {
        return mPath;
    }

    /// Append @p bytes and flush. The producer side of the live-tail flow.
    void Append(const std::string &bytes) const
    {
        std::ofstream stream(mPath.toStdString(), std::ios::binary | std::ios::app);
        QVERIFY2(stream.is_open(), "live-tail append must succeed");
        stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        stream.flush();
    }

private:
    QTemporaryDir mDir;
    QString mPath;
};

// Wait until the model's `lineCountChanged` signal reports at least
// @p target rows, draining the Qt event queue while we wait so the
// QueuedConnection lambdas the parser worker posts actually run. Returns
// `true` on success, `false` if the deadline expires (the caller usually
// `QVERIFY2`s the result).
bool WaitForLineCount(LogModel &model, qsizetype target, std::chrono::milliseconds deadline)
{
    const auto start = std::chrono::steady_clock::now();
    while (model.rowCount() < target)
    {
        if (std::chrono::steady_clock::now() - start > deadline)
        {
            return false;
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 25);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return true;
}

// Returns the column index whose header equals @p header, or -1 if none.
int ColumnByHeader(const LogModel &model, const QString &header)
{
    const int columnCount = model.columnCount();
    for (int col = 0; col < columnCount; ++col)
    {
        if (model.headerData(col, Qt::Horizontal, Qt::DisplayRole).toString() == header)
        {
            return col;
        }
    }
    return -1;
}

// Arms @p model for a live-tail-style synthetic-batch session by
// installing a no-producer `StreamLineSource` so subsequent
// `LogLine`s constructed for synthetic batches have a valid
// `LineSource *` to point at. Returns the installed source by
// reference for callers that publish raw bytes via `AppendLine`.
loglib::StreamLineSource &BeginSyntheticStreamSession(LogModel &model)
{
    auto streamSource = std::make_unique<loglib::StreamLineSource>(std::filesystem::path("synthetic"), nullptr);
    loglib::StreamLineSource *streamPtr = streamSource.get();
    static_cast<void>(model.BeginStreamingForSyncTest(std::move(streamSource)));
    return *streamPtr;
}

// Static-mode counterpart of `BeginSyntheticStreamSession`. Installs a
// synthetic `StreamLineSource` (the `LineSource` flavour is irrelevant
// for proxy-orientation tests) but without any rows; callers are
// expected to push synthetic batches via `OnBatch` if they need rows.
// Returns the installed source by reference for callers that publish
// raw bytes via `AppendLine`.
loglib::StreamLineSource &BeginSyntheticStaticSession(LogModel &model)
{
    return BeginSyntheticStreamSession(model);
}

// Builds one synthetic streaming batch carrying @p count `LogLine`
// rows with line ids `[firstLineId, firstLineId + count)`, each
// holding a single int64 value field equal to its line id under
// @p valueKey. Each row's raw bytes are also published into
// @p streamSource so `RawLine(lineId)` round-trips. Set
// @p declareNewKey on the first batch in a sequence so
// `LogTable::PreviewAppend`'s column predictor matches reality.
loglib::StreamedBatch MakeSyntheticBatch(
    loglib::StreamLineSource &streamSource,
    loglib::KeyIndex &keys,
    loglib::KeyId valueKey,
    size_t firstLineId,
    size_t count,
    bool declareNewKey
)
{
    loglib::StreamedBatch batch;
    batch.firstLineNumber = firstLineId;
    if (declareNewKey)
    {
        batch.newKeys.emplace_back(std::string("value"));
    }
    batch.lines.reserve(count);
    for (size_t i = 0; i < count; ++i)
    {
        const size_t lineId = firstLineId + i;
        const size_t publishedId = streamSource.AppendLine("synthetic line " + std::to_string(lineId), std::string{});
        Q_ASSERT(publishedId == lineId);
        Q_UNUSED(publishedId);
        std::vector<std::pair<loglib::KeyId, loglib::internal::CompactLogValue>> compactValues;
        compactValues.emplace_back(
            valueKey, loglib::internal::CompactLogValue::MakeInt64(static_cast<int64_t>(lineId))
        );
        batch.lines.emplace_back(std::move(compactValues), keys, streamSource, lineId);
    }
    return batch;
}

// Locate a UI-file-declared `QAction` by `objectName`.
//
// The primary path goes through `MainWindow::FindUiAction`, which
// forwards directly to the `ui->actionXxx` pointer and bypasses the
// QObject-tree lookup altogether. This is necessary because on the
// GitHub-hosted Linux runner with Qt 6.8 + the offscreen QPA plugin,
// `QObject::findChild<QAction*>` — and, in fact, every form of
// QObject-tree traversal we tried (findChildren<QAction*>, walking
// findChildren<QWidget*>() and inspecting each widget's actions(),
// and even `QMainWindow::actions()`) — returns null for actions
// declared inside `<widget class="QMainWindow">` even though the
// `ui->actionXxx` pointer is valid and wired into menus / toolbars.
// Windows and macOS with the same Qt build are unaffected.
//
// The `findChild` fallback remains as a safety net for QMainWindow
// subclasses we don't know about (the helper is generic and might
// be reused in other test TUs).
QAction *FindActionByObjectName(QMainWindow *window, const QString &name)
{
    if (auto *mainWindow = qobject_cast<MainWindow *>(window))
    {
        if (QAction *uiAction = mainWindow->FindUiAction(name))
        {
            return uiAction;
        }
    }
    return window->findChild<QAction *>(name);
}

} // namespace

class MainWindowTest : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        // Called before the first test function
        qDebug() << "Starting MainWindow tests";
    }

    void cleanupTestCase()
    {
        // Called after the last test function
        qDebug() << "MainWindow tests complete";
    }

    void init()
    {
        // Called before each test function
        window = new MainWindow();
    }

    void cleanup()
    {
        // Called after each test function
        delete window;
        window = nullptr;
    }

    void testWindowTitle()
    {
        QCOMPARE(window->windowTitle(), QString("Structured Log Viewer"));
    }

    void testWindowIcon()
    {
        QVERIFY(!window->windowIcon().isNull());
    }

    void testFixture_Empty()
    {
        FixtureFile fixture(":/fixtures/empty.jsonl");
        StreamingRun run = RunStreaming(fixture.Path());
        QCOMPARE(run.finishedCount, 1);
        QCOMPARE(run.cancelled, false);
        QCOMPARE(run.model->rowCount(), 0);
        QCOMPARE(run.model->columnCount(), 0);
        QVERIFY(run.model->StreamingErrors().empty());
    }

    void testFixture_SingleLine()
    {
        FixtureFile fixture(":/fixtures/single_line.jsonl");
        StreamingRun run = RunStreaming(fixture.Path());
        QCOMPARE(run.finishedCount, 1);
        QCOMPARE(run.cancelled, false);
        QCOMPARE(run.model->rowCount(), 1);
        QCOMPARE(run.model->columnCount(), 1);
        QCOMPARE(run.model->headerData(0, Qt::Horizontal, Qt::DisplayRole).toString(), QStringLiteral("message"));
        QCOMPARE(run.model->data(run.model->index(0, 0), Qt::DisplayRole).toString(), QStringLiteral("hello"));
        QVERIFY(run.model->StreamingErrors().empty());
    }

    void testFixture_ValueTypes()
    {
        FixtureFile fixture(":/fixtures/value_types.jsonl");
        StreamingRun run = RunStreaming(fixture.Path());
        QCOMPARE(run.finishedCount, 1);
        QCOMPARE(run.cancelled, false);
        QCOMPARE(run.model->rowCount(), 3);
        QVERIFY(run.model->StreamingErrors().empty());

        // Look up columns by header so the test is robust against the streaming
        // path's insertion-order column layout.
        const int colStr = ColumnByHeader(*run.model, QStringLiteral("str"));
        const int colInt = ColumnByHeader(*run.model, QStringLiteral("int"));
        const int colUint = ColumnByHeader(*run.model, QStringLiteral("uint"));
        const int colDbl = ColumnByHeader(*run.model, QStringLiteral("dbl"));
        const int colFlag = ColumnByHeader(*run.model, QStringLiteral("flag"));
        const int colNul = ColumnByHeader(*run.model, QStringLiteral("nul"));
        const int colObj = ColumnByHeader(*run.model, QStringLiteral("obj"));
        const int colArr = ColumnByHeader(*run.model, QStringLiteral("arr"));
        QVERIFY(colStr >= 0 && colInt >= 0 && colUint >= 0 && colDbl >= 0);
        QVERIFY(colFlag >= 0 && colNul >= 0 && colObj >= 0 && colArr >= 0);

        const auto sortVal = [&](int row, int col) {
            return run.model->data(run.model->index(row, col), LogModelItemDataRole::SortRole);
        };
        const auto displayVal = [&](int row, int col) {
            return run.model->data(run.model->index(row, col), Qt::DisplayRole).toString();
        };

        // Row 0 (line 1): comprehensive values that pin the cached simdjson type
        // for each key (especially "uint" → unsigned_integer via 18446744073709551610).
        QCOMPARE(sortVal(0, colStr).toString(), QStringLiteral("alpha"));
        QCOMPARE(sortVal(0, colInt).toLongLong(), qint64(-7));
        QCOMPARE(sortVal(0, colUint).toULongLong(), 18446744073709551610ULL);
        QCOMPARE(sortVal(0, colDbl).toDouble(), 3.14);
        QCOMPARE(sortVal(0, colFlag).toBool(), true);
        QVERIFY(!sortVal(0, colNul).isValid());
        QCOMPARE(displayVal(0, colObj), QStringLiteral("{\"k\":\"v\"}"));
        QCOMPARE(displayVal(0, colArr), QStringLiteral("[1,2,3]"));

        // Row 1 (line 2): edge values (0 / 0.0 / false / empty containers).
        QCOMPARE(sortVal(1, colStr).toString(), QStringLiteral("beta"));
        QCOMPARE(sortVal(1, colInt).toLongLong(), qint64(0));
        QCOMPARE(sortVal(1, colDbl).toDouble(), 0.0);
        QCOMPARE(sortVal(1, colFlag).toBool(), false);
        QVERIFY(!sortVal(1, colNul).isValid());
        QCOMPARE(displayVal(1, colObj), QStringLiteral("{}"));
        QCOMPARE(displayVal(1, colArr), QStringLiteral("[]"));

        // Row 2 (line 3): negative double + nested object/array stay compacted
        // through `LogModel::ConvertToSingleLineCompactQString`.
        QCOMPARE(sortVal(2, colStr).toString(), QStringLiteral("gamma"));
        QCOMPARE(sortVal(2, colInt).toLongLong(), qint64(42));
        QCOMPARE(sortVal(2, colDbl).toDouble(), -2.5);
        QCOMPARE(sortVal(2, colFlag).toBool(), true);
        QVERIFY(!sortVal(2, colNul).isValid());
        QCOMPARE(displayVal(2, colObj), QStringLiteral("{\"nested\":{\"deep\":1}}"));
        QCOMPARE(displayVal(2, colArr), QStringLiteral("[\"x\",\"y\"]"));
    }

    void testFixture_IsoTTimestamp()
    {
        FixtureFile fixture(":/fixtures/iso_t_timestamp.jsonl");
        StreamingRun run = RunStreaming(fixture.Path());
        // 2024-04-28T07:14:30 UTC → 1714288470 seconds since epoch.
        AssertTimestampFixture(run, qint64(1714288470000000), 3);
    }

    void testFixture_IsoSpaceTimestamp()
    {
        FixtureFile fixture(":/fixtures/iso_space_timestamp.jsonl");
        StreamingRun run = RunStreaming(fixture.Path());
        // 2024-04-28 07:14:30 UTC → 1714288470 seconds since epoch.
        AssertTimestampFixture(run, qint64(1714288470000000), 3);
    }

    void testFixture_IsoOffsetTimestamp()
    {
        FixtureFile fixture(":/fixtures/iso_offset_timestamp.jsonl");
        StreamingRun run = RunStreaming(fixture.Path());
        // 2024-04-28T07:14:30+02:00 → 2024-04-28T05:14:30 UTC → 1714281270 seconds.
        AssertTimestampFixture(run, qint64(1714281270000000), 3);
    }

    void testFixture_IsoFractionalTimestamp()
    {
        FixtureFile fixture(":/fixtures/iso_fractional_timestamp.jsonl");
        StreamingRun run = RunStreaming(fixture.Path());
        // 2024-04-28T07:14:30.123 UTC → 1714288470.123 → 1714288470123000 µs.
        AssertTimestampFixture(run, qint64(1714288470123000), 3);

        // Spot-check the µs and 0.5s rows separately so all three fractional
        // widths in the fixture are exercised.
        const int tsCol = ColumnByHeader(*run.model, QStringLiteral("timestamp"));
        const qint64 row1Us = run.model->data(run.model->index(1, tsCol), LogModelItemDataRole::SortRole).toLongLong();
        QCOMPARE(row1Us, qint64(1714288470123456));
        const qint64 row2Us = run.model->data(run.model->index(2, tsCol), LogModelItemDataRole::SortRole).toLongLong();
        QCOMPARE(row2Us, qint64(1714288470500000));
    }

    void testFixture_AltTimestampKeys()
    {
        FixtureFile fixture(":/fixtures/alt_timestamp_keys.jsonl");
        StreamingRun run = RunStreaming(fixture.Path());
        QCOMPARE(run.finishedCount, 1);
        QCOMPARE(run.cancelled, false);
        QCOMPARE(run.model->rowCount(), 3);
        QVERIFY(run.model->StreamingErrors().empty());

        const auto &columns = run.model->Configuration().columns;
        const auto findColumn = [&](const std::string &header) -> const loglib::LogConfiguration::Column * {
            for (const auto &c : columns)
            {
                if (c.header == header)
                {
                    return &c;
                }
            }
            return nullptr;
        };

        // All three IsTimestampKey-recognised keys ("time", "t", "Timestamp",
        // case-insensitive) must auto-promote to Type::time.
        for (const std::string &header : {std::string("time"), std::string("t"), std::string("Timestamp")})
        {
            const auto *column = findColumn(header);
            QVERIFY2(
                column != nullptr,
                qPrintable(QStringLiteral("column '%1' must exist").arg(QString::fromStdString(header)))
            );
            QCOMPARE(column->type, loglib::LogConfiguration::Type::time);
        }

        // Each row populated only its own timestamp column; the other two are empty.
        const int colTime = ColumnByHeader(*run.model, QStringLiteral("time"));
        const int colT = ColumnByHeader(*run.model, QStringLiteral("t"));
        const int colTimestamp = ColumnByHeader(*run.model, QStringLiteral("Timestamp"));
        QVERIFY(run.model->data(run.model->index(0, colTime), LogModelItemDataRole::SortRole).isValid());
        QVERIFY(!run.model->data(run.model->index(0, colT), LogModelItemDataRole::SortRole).isValid());
        QVERIFY(!run.model->data(run.model->index(0, colTimestamp), LogModelItemDataRole::SortRole).isValid());
        QVERIFY(run.model->data(run.model->index(1, colT), LogModelItemDataRole::SortRole).isValid());
        QVERIFY(run.model->data(run.model->index(2, colTimestamp), LogModelItemDataRole::SortRole).isValid());
    }

    void testFixture_MixedColumns()
    {
        FixtureFile fixture(":/fixtures/mixed_columns.jsonl");
        StreamingRun run = RunStreaming(fixture.Path());
        QCOMPARE(run.finishedCount, 1);
        QCOMPARE(run.cancelled, false);
        QCOMPARE(run.model->rowCount(), 3);
        QVERIFY(run.model->StreamingErrors().empty());

        const int colA = ColumnByHeader(*run.model, QStringLiteral("a"));
        const int colB = ColumnByHeader(*run.model, QStringLiteral("b"));
        const int colC = ColumnByHeader(*run.model, QStringLiteral("c"));
        QVERIFY(colA >= 0 && colB >= 0 && colC >= 0);
        QCOMPARE(run.model->columnCount(), 3);

        const auto displayVal = [&](int row, int col) {
            return run.model->data(run.model->index(row, col), Qt::DisplayRole).toString();
        };

        // Row 0: {a, b}; c is missing → empty.
        QCOMPARE(displayVal(0, colA), QStringLiteral("a1"));
        QCOMPARE(displayVal(0, colB), QStringLiteral("b1"));
        QVERIFY(displayVal(0, colC).isEmpty());

        // Row 1: {a, c}; b is missing.
        QCOMPARE(displayVal(1, colA), QStringLiteral("a2"));
        QVERIFY(displayVal(1, colB).isEmpty());
        QCOMPARE(displayVal(1, colC), QStringLiteral("c2"));

        // Row 2: {b, c}; a is missing.
        QVERIFY(displayVal(2, colA).isEmpty());
        QCOMPARE(displayVal(2, colB), QStringLiteral("b3"));
        QCOMPARE(displayVal(2, colC), QStringLiteral("c3"));
    }

    void testFixture_InvalidLines()
    {
        FixtureFile fixture(":/fixtures/invalid_lines.jsonl");
        StreamingRun run = RunStreaming(fixture.Path());
        QCOMPARE(run.finishedCount, 1);
        QCOMPARE(run.cancelled, false);
        QCOMPARE(run.model->rowCount(), 2);

        const auto &errors = run.model->StreamingErrors();
        QCOMPARE(static_cast<int>(errors.size()), 1);
        const QString message = QString::fromStdString(errors.front());
        QVERIFY2(
            message.contains(QStringLiteral("line"), Qt::CaseInsensitive),
            qPrintable(QStringLiteral("error '%1' must reference a line number").arg(message))
        );

        const int colA = ColumnByHeader(*run.model, QStringLiteral("a"));
        QVERIFY(colA >= 0);
        QCOMPARE(run.model->data(run.model->index(0, colA), Qt::DisplayRole).toString(), QStringLiteral("valid_first"));
        QCOMPARE(run.model->data(run.model->index(1, colA), Qt::DisplayRole).toString(), QStringLiteral("valid_third"));
    }

    void testFixture_MixedTzAndOrder()
    {
        FixtureFile fixture(":/fixtures/mixed_tz_and_order.jsonl");
        StreamingRun run = RunStreaming(fixture.Path());
        QCOMPARE(run.finishedCount, 1);
        QCOMPARE(run.cancelled, false);
        QCOMPARE(run.model->rowCount(), 3);
        QVERIFY(run.model->StreamingErrors().empty());

        const int tsCol = ColumnByHeader(*run.model, QStringLiteral("timestamp"));
        const int msgCol = ColumnByHeader(*run.model, QStringLiteral("msg"));
        QVERIFY(tsCol >= 0 && msgCol >= 0);

        const auto &columns = run.model->Configuration().columns;
        QCOMPARE(columns[static_cast<size_t>(tsCol)].type, loglib::LogConfiguration::Type::time);

        // (1) Loading: rows preserve file order (msg column).
        QCOMPARE(run.model->data(run.model->index(0, msgCol), Qt::DisplayRole).toString(), QStringLiteral("line1"));
        QCOMPARE(run.model->data(run.model->index(1, msgCol), Qt::DisplayRole).toString(), QStringLiteral("line2"));
        QCOMPARE(run.model->data(run.model->index(2, msgCol), Qt::DisplayRole).toString(), QStringLiteral("line3"));

        // (2) UTC conversion: each offset must be normalised to UTC microseconds.
        // line1 = 2024-04-28T10:00:00+02:00 → 2024-04-28T08:00:00 UTC = 1714291200 s.
        // line2 = 2024-04-28T05:00:00-05:00 → 2024-04-28T10:00:00 UTC = 1714298400 s.
        // line3 = 2024-04-28T07:30:00+00:00 → 2024-04-28T07:30:00 UTC = 1714289400 s.
        const qint64 line1Us = run.model->data(run.model->index(0, tsCol), LogModelItemDataRole::SortRole).toLongLong();
        const qint64 line2Us = run.model->data(run.model->index(1, tsCol), LogModelItemDataRole::SortRole).toLongLong();
        const qint64 line3Us = run.model->data(run.model->index(2, tsCol), LogModelItemDataRole::SortRole).toLongLong();
        QCOMPARE(line1Us, qint64(1714291200000000));
        QCOMPARE(line2Us, qint64(1714298400000000));
        QCOMPARE(line3Us, qint64(1714289400000000));

        // (3) Sorting via the production LogFilterModel proxy. SortRole returns
        // qint64 microseconds, so the proxy's chronological order must be
        // line3 < line1 < line2 ascending and the mirror descending.
        LogFilterModel proxy;
        proxy.setSortRole(LogModelItemDataRole::SortRole);
        proxy.setSourceModel(run.model.get());

        proxy.sort(tsCol, Qt::AscendingOrder);
        QCOMPARE(proxy.mapToSource(proxy.index(0, 0)).row(), 2); // line3 (07:30 UTC)
        QCOMPARE(proxy.mapToSource(proxy.index(1, 0)).row(), 0); // line1 (08:00 UTC)
        QCOMPARE(proxy.mapToSource(proxy.index(2, 0)).row(), 1); // line2 (10:00 UTC)

        proxy.sort(tsCol, Qt::DescendingOrder);
        QCOMPARE(proxy.mapToSource(proxy.index(0, 0)).row(), 1);
        QCOMPARE(proxy.mapToSource(proxy.index(1, 0)).row(), 0);
        QCOMPARE(proxy.mapToSource(proxy.index(2, 0)).row(), 2);
    }

    // Regression: `LogModel::AppendBatch` must fire `beginInsertRows` /
    // `beginInsertColumns` *before* `LogTable::AppendBatch` mutates, so
    // proxies see the pre-mutation source count in `*AboutToBeInserted`.
    void testAppendBatchFiresBeginsBeforeMutation()
    {
        const QStringList fixtureLines = MakeParityFixture();
        TempJsonFile fixture(fixtureLines);

        LogModel model;
        LogFilterModel proxy;
        proxy.setSourceModel(&model);

        // Hook the source model's signals, not the proxy's: the proxy delays
        // its own `*AboutToBeInserted` until after the source has mutated.
        std::vector<int> rowCountSnapshotsAtAboutToBeInserted;
        std::vector<int> columnCountSnapshotsAtAboutToBeInserted;
        QObject::connect(
            &model,
            &QAbstractItemModel::rowsAboutToBeInserted,
            &model,
            [&](const QModelIndex &, int, int) { rowCountSnapshotsAtAboutToBeInserted.push_back(model.rowCount()); }
        );
        QObject::connect(
            &model,
            &QAbstractItemModel::columnsAboutToBeInserted,
            &model,
            [&](const QModelIndex &, int, int) {
                columnCountSnapshotsAtAboutToBeInserted.push_back(model.columnCount());
            }
        );

        QSignalSpy finishedSpy(&model, &LogModel::streamingFinished);
        QVERIFY(finishedSpy.isValid());

        auto file = std::make_unique<loglib::LogFile>(fixture.Path().toStdString());
        auto fileSource = std::make_unique<loglib::FileLineSource>(std::move(file));
        loglib::FileLineSource *parseSource = fileSource.get();
        const loglib::StopToken stopToken = model.BeginStreamingForSyncTest(std::move(fileSource));

        QVERIFY(!model.Table().Data().Sources().empty());

        loglib::ParserOptions options;
        options.stopToken = stopToken;
        loglib::internal::AdvancedParserOptions advanced;
        advanced.threads = 1;

        loglib::JsonParser parser;
        parser.ParseStreaming(*parseSource, *model.Sink(), options, advanced);

        const bool finished = finishedSpy.count() > 0 || finishedSpy.wait(5000);
        QVERIFY2(finished, "streamingFinished must arrive within the timeout");

        // The fixture grows the model at least once; capture the running
        // pre-snapshot counts and assert each `*AboutToBeInserted` slot saw
        // the count *before* that growth, not after.
        QVERIFY2(!rowCountSnapshotsAtAboutToBeInserted.empty(), "at least one rowsAboutToBeInserted must have fired");
        QVERIFY2(
            !columnCountSnapshotsAtAboutToBeInserted.empty(), "at least one columnsAboutToBeInserted must have fired"
        );

        int expectedRowCount = 0;
        for (int snapshot : rowCountSnapshotsAtAboutToBeInserted)
        {
            QVERIFY2(
                snapshot == expectedRowCount,
                qPrintable(QStringLiteral("rowsAboutToBeInserted snapshot %1 must equal pre-mutation row count %2")
                               .arg(snapshot)
                               .arg(expectedRowCount))
            );
            ++expectedRowCount; // the test fixture is small; growth is monotonic
        }
        // Final post-batch row count matches the fixture line count (the last
        // batch grew the model from N-1 to N, and the snapshot before it
        // showed N-1).
        QCOMPARE(model.rowCount(), fixtureLines.size());

        int expectedColumnCount = 0;
        for (int snapshot : columnCountSnapshotsAtAboutToBeInserted)
        {
            QVERIFY2(
                snapshot >= expectedColumnCount,
                qPrintable(QStringLiteral("columnsAboutToBeInserted snapshot %1 must be at least the previous %2")
                               .arg(snapshot)
                               .arg(expectedColumnCount))
            );
            QVERIFY2(
                snapshot < model.columnCount(),
                qPrintable(QStringLiteral("columnsAboutToBeInserted snapshot %1 must be < final column count %2")
                               .arg(snapshot)
                               .arg(model.columnCount()))
            );
            expectedColumnCount = snapshot;
        }
    }

    // Regression: `Reset()` must emit a compensating `streamingFinished` based
    // on a GUI-thread flag (`mStreamingActive`), not `isRunning()` — the
    // latter races with the queued `OnFinished` and would silently drop the
    // signal, leaving configuration menus disabled.
    void testResetAfterBeginStreamingEmitsCompensatingFinished()
    {
        // Use a tiny in-memory file so BeginStreaming has a valid LogFile
        // to install on the model.
        TempJsonFile fixture(QStringList{QStringLiteral(R"({"a": 1})")});

        LogModel model;
        QSignalSpy finishedSpy(&model, &LogModel::streamingFinished);
        QVERIFY(finishedSpy.isValid());

        auto file = std::make_unique<loglib::LogFile>(fixture.Path().toStdString());
        auto fileSource = std::make_unique<loglib::FileLineSource>(std::move(file));
        static_cast<void>(model.BeginStreamingForSyncTest(std::move(fileSource)));

        // No parser is started, so `EndStreaming` will never fire on its own.
        // `Reset()` must emit `streamingFinished(true)` itself based on the
        // GUI-thread flag set by `BeginStreaming`.
        model.Reset();

        QCOMPARE(finishedSpy.count(), 1);
        QCOMPARE(finishedSpy.takeFirst().value(0).value<StreamingResult>(), StreamingResult::Cancelled);

        // A second `Reset()` after the first one already cleared the flag
        // must not emit a spurious `streamingFinished` (otherwise repeated
        // user-driven file opens would emit duplicate signals).
        model.Reset();
        QCOMPARE(finishedSpy.count(), 0);
    }

    // Regression: the sink-generation bump must happen in
    // `DropPendingBatches()` (called *after* `waitForFinished()`), not in
    // `RequestStop()`. Otherwise drain-phase queued lambdas pass the
    // mismatch check and run after `Reset()` has destroyed `mLogTable`
    // (use-after-free on dangling `LogFile*` + spurious second `streamingFinished`).
    void testResetDuringStreamingDropsDrainPhaseBatch()
    {
        TempJsonFile fixture(QStringList{QStringLiteral(R"({"a": 1})")});

        LogModel model;
        QSignalSpy finishedSpy(&model, &LogModel::streamingFinished);
        QVERIFY(finishedSpy.isValid());

        auto file = std::make_unique<loglib::LogFile>(fixture.Path().toStdString());
        auto fileSource = std::make_unique<loglib::FileLineSource>(std::move(file));
        loglib::FileLineSource *sourcePtr = fileSource.get();

        // The parseCallable parks on a CV until the test thread releases it,
        // which happens AFTER the worker has emitted drain-phase
        // `OnBatch` + `OnFinished` (the racy queued lambdas the fix invalidates).
        std::mutex releaseMutex;
        std::condition_variable releaseCv;
        bool released = false;
        QtStreamingLogSink *sinkBeforeBegin = model.Sink();
        QVERIFY(sinkBeforeBegin != nullptr);

        const loglib::StopToken stop = model.BeginStreaming(
            std::move(fileSource),
            [sinkBeforeBegin, sourcePtr, &releaseMutex, &releaseCv, &released](loglib::StopToken stopToken) {
                while (!stopToken.stop_requested())
                {
                    std::this_thread::yield();
                }

                loglib::KeyIndex &keys = sinkBeforeBegin->Keys();
                const loglib::KeyId keyId = keys.GetOrInsert("a");
                std::vector<std::pair<loglib::KeyId, loglib::LogValue>> values;
                values.emplace_back(keyId, loglib::LogValue(int64_t{1}));
                loglib::LogLine line(std::move(values), keys, *sourcePtr, 1);
                loglib::StreamedBatch batch;
                batch.lines.push_back(std::move(line));
                batch.firstLineNumber = 1;
                sinkBeforeBegin->OnBatch(std::move(batch));
                sinkBeforeBegin->OnFinished(true);

                std::unique_lock lock(releaseMutex);
                releaseCv.wait(lock, [&]() { return released; });
            }
        );
        static_cast<void>(stop);

        // `Reset()` blocks on `waitForFinished()`; release the worker from a
        // separate thread so `Reset()` can return.
        std::thread releaser([&]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            {
                std::lock_guard lock(releaseMutex);
                released = true;
            }
            releaseCv.notify_all();
        });

        model.Reset();
        releaser.join();

        // Drain the queued lambdas; with the fix both observe a stale
        // generation and short-circuit.
        QCoreApplication::processEvents();

        QCOMPARE(finishedSpy.count(), 1);
        QCOMPARE(finishedSpy.takeFirst().value(0).value<StreamingResult>(), StreamingResult::Cancelled);
        QCOMPARE(model.rowCount(), 0);
    }

    // Regression for C.4.1: `StopAndKeepRows()` is the API that
    // `MainWindow::StopStream` invokes; it must preserve every row the
    // worker has already produced — including the drain-phase `OnBatch`
    // emitted between `RequestStop()` and `waitForFinished()` returning.
    // The bug was that `DropPendingBatches()` bumped the generation
    // before the GUI thread ran the queued lambdas, so the drain-phase
    // batch was silently discarded. The fix drains queued sink events
    // between the join and the generation bump on the StopAndKeepRows path
    // *only* (Reset keeps dropping them — the table is reset anyway,
    // see `testResetDuringStreamingDropsDrainPhaseBatch`).
    //
    // Test shape mirrors `testResetDuringStreamingDropsDrainPhaseBatch`
    // but swaps `Reset()` for `StopAndKeepRows()` and inverts the row-count
    // assertion (the row must survive) and the signal-count assertion
    // (the drained `OnFinished` lambda emits the *only* terminal
    // signal — no compensating duplicate from the teardown helper).
    void testStopAndKeepRowsPreservesDrainPhaseBatch()
    {
        TempJsonFile fixture(QStringList{QStringLiteral(R"({"a": 1})")});

        LogModel model;
        QSignalSpy finishedSpy(&model, &LogModel::streamingFinished);
        QVERIFY(finishedSpy.isValid());

        auto file = std::make_unique<loglib::LogFile>(fixture.Path().toStdString());
        auto fileSource = std::make_unique<loglib::FileLineSource>(std::move(file));
        loglib::FileLineSource *sourcePtr = fileSource.get();

        std::mutex releaseMutex;
        std::condition_variable releaseCv;
        bool released = false;
        QtStreamingLogSink *sinkBeforeBegin = model.Sink();
        QVERIFY(sinkBeforeBegin != nullptr);

        const loglib::StopToken stop = model.BeginStreaming(
            std::move(fileSource),
            [sinkBeforeBegin, sourcePtr, &releaseMutex, &releaseCv, &released](loglib::StopToken stopToken) {
                while (!stopToken.stop_requested())
                {
                    std::this_thread::yield();
                }

                loglib::KeyIndex &keys = sinkBeforeBegin->Keys();
                const loglib::KeyId keyId = keys.GetOrInsert("a");
                std::vector<std::pair<loglib::KeyId, loglib::LogValue>> values;
                values.emplace_back(keyId, loglib::LogValue(int64_t{1}));
                loglib::LogLine line(std::move(values), keys, *sourcePtr, 1);
                loglib::StreamedBatch batch;
                batch.lines.push_back(std::move(line));
                batch.firstLineNumber = 1;
                sinkBeforeBegin->OnBatch(std::move(batch));
                sinkBeforeBegin->OnFinished(true);

                std::unique_lock lock(releaseMutex);
                releaseCv.wait(lock, [&]() { return released; });
            }
        );
        static_cast<void>(stop);

        std::thread releaser([&]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            {
                std::lock_guard lock(releaseMutex);
                released = true;
            }
            releaseCv.notify_all();
        });

        model.StopAndKeepRows();
        releaser.join();

        // The drain at Step 3.4 of `TeardownStreamingSessionInternal` has
        // already delivered the queued `OnBatch` (so the row is in the
        // model) *and* the queued `OnFinished` (so `streamingFinished`
        // has already been emitted via `EndStreaming`). A follow-up
        // `processEvents()` is still safe (any other Qt-posted work
        // should drain too) and matches the surrounding tests' style;
        // it must not change the observed row count or signal count.
        QCoreApplication::processEvents();

        QVERIFY(!model.IsStreamingActive());
        QCOMPARE(model.rowCount(), 1);
        QCOMPARE(finishedSpy.count(), 1);
        QCOMPARE(finishedSpy.takeFirst().value(0).value<StreamingResult>(), StreamingResult::Cancelled);
    }

    // FIFO eviction at the retention cap: feed batches of
    // synthetic `LogLine`s directly into `LogModel::AppendBatch`, asserting
    // that:
    //   - `RowCount()` never exceeds the cap once the cap is reached;
    //   - `beginRemoveRows`/`rowsRemoved` fire on the prefix as new lines arrive;
    //   - the surviving rows correspond to the *most recent* lines, not the
    //     oldest ones (FIFO drops oldest).
    void testRetentionCapFifoEviction()
    {
        LogModel model;
        // Install a no-producer `StreamLineSource` so synthetic
        // `LogLine`s constructed below have a valid `LineSource *` to
        // bind to.
        loglib::StreamLineSource &streamSource = BeginSyntheticStreamSession(model);
        model.SetRetentionCap(100);

        QSignalSpy rowsRemovedSpy(&model, &QAbstractItemModel::rowsRemoved);
        QVERIFY(rowsRemovedSpy.isValid());

        loglib::KeyIndex &keys = model.Sink()->Keys();
        const loglib::KeyId valueKey = keys.GetOrInsert(std::string("value"));

        const size_t batchSize = 50;
        const size_t totalLines = 500;
        for (size_t batchStart = 0; batchStart < totalLines; batchStart += batchSize)
        {
            const bool declareNewKey = (batchStart == 0);
            model.AppendBatch(MakeSyntheticBatch(streamSource, keys, valueKey, batchStart + 1, batchSize, declareNewKey)
            );
        }

        QCOMPARE(model.rowCount(), 100);
        // 500 lines arrived, cap is 100 → 400 lines were evicted across
        // however many batches breached the cap.
        QVERIFY(rowsRemovedSpy.count() > 0);

        // The first surviving row's `value` field must be the (500-100+1)-th
        // line we fed (1-indexed), i.e. lineId 401.
        const auto firstValue =
            model.data(model.index(0, ColumnByHeader(model, QStringLiteral("value"))), LogModelItemDataRole::SortRole);
        QCOMPARE(firstValue.toLongLong(), qint64(401));
        const auto lastValue = model.data(
            model.index(model.rowCount() - 1, ColumnByHeader(model, QStringLiteral("value"))),
            LogModelItemDataRole::SortRole
        );
        QCOMPARE(lastValue.toLongLong(), qint64(500));

        // Reset the streaming flag so model destructors don't emit a
        // compensating cancelled signal that the test isn't watching for.
        model.EndStreaming(false);
    }

    // Giant-batch collapse: a batch whose row count alone
    // exceeds the cap must collapse the head of the batch *before* it lands
    // in `LogTable`, so per-batch eviction stays O(cap) and the visible
    // model never breaches the cap.
    void testRetentionCapGiantBatchCollapse()
    {
        LogModel model;
        loglib::StreamLineSource &streamSource = BeginSyntheticStreamSession(model);
        model.SetRetentionCap(1000);

        loglib::KeyIndex &keys = model.Sink()->Keys();
        const loglib::KeyId valueKey = keys.GetOrInsert(std::string("value"));

        // Build the giant 2000-line batch. The model's giant-batch
        // collapse drops the head of `batch.lines` before forwarding
        // to `LogTable::AppendBatch`.
        model.AppendBatch(
            MakeSyntheticBatch(streamSource, keys, valueKey, /*firstLineId=*/1, /*count=*/2000, /*declareNewKey=*/true)
        );

        // Visible rows must equal the cap exactly (the head of the batch
        // was collapsed pre-AppendBatch, so the whole batch never landed).
        QCOMPARE(model.rowCount(), 1000);
        const int valueColumn = ColumnByHeader(model, QStringLiteral("value"));
        QVERIFY(valueColumn >= 0);
        // Surviving rows are lines 1001..2000 in input order.
        QCOMPARE(model.data(model.index(0, valueColumn), LogModelItemDataRole::SortRole).toLongLong(), qint64(1001));
        QCOMPARE(model.data(model.index(999, valueColumn), LogModelItemDataRole::SortRole).toLongLong(), qint64(2000));

        model.EndStreaming(false);
    }

    // Sink Pause/Resume: while paused, `OnBatch` redirects into the paused
    // buffer instead of posting per-batch QueuedConnection lambdas; on
    // Resume the buffer is coalesced into a single batch and posted to
    // `LogModel::AppendBatch`.
    void testSinkPauseResumeCoalescesBufferedBatches()
    {
        LogModel model;
        QSignalSpy lineCountSpy(&model, &LogModel::lineCountChanged);
        QVERIFY(lineCountSpy.isValid());

        loglib::StreamLineSource &streamSource = BeginSyntheticStreamSession(model);
        QtStreamingLogSink *sink = model.Sink();
        QVERIFY(sink != nullptr);
        QVERIFY(sink->IsActive());

        loglib::KeyIndex &keys = sink->Keys();
        const loglib::KeyId valueKey = keys.GetOrInsert(std::string("value"));

        sink->Pause();
        QVERIFY(sink->IsPaused());

        // Push three batches into the sink while paused — they must NOT
        // reach the model (no `lineCountChanged` ticks observed).
        const int batchesWhilePaused = 3;
        const int linesPerBatch = 5;
        for (int b = 0; b < batchesWhilePaused; ++b)
        {
            const size_t firstLineId = static_cast<size_t>(b * linesPerBatch + 1);
            const bool declareNewKey = (b == 0);
            // Direct call to OnBatch from this thread mirrors the worker.
            sink->OnBatch(MakeSyntheticBatch(
                streamSource, keys, valueKey, firstLineId, static_cast<size_t>(linesPerBatch), declareNewKey
            ));
        }

        // Drain the event loop just to be sure no QueuedConnection lambdas
        // slipped through.
        QCoreApplication::processEvents();
        QCOMPARE(model.rowCount(), 0);
        QCOMPARE(static_cast<int>(sink->PausedLineCount()), batchesWhilePaused * linesPerBatch);

        // Resume coalesces the three batches into a single batch and posts
        // it via QueuedConnection; `processEvents` runs the lambda.
        sink->Resume();
        QVERIFY(!sink->IsPaused());
        QCoreApplication::processEvents();

        QCOMPARE(model.rowCount(), batchesWhilePaused * linesPerBatch);
        QCOMPARE(static_cast<int>(sink->PausedLineCount()), 0);

        model.EndStreaming(false);
    }

    // Regression: pausing a *static-streaming* session (the **Stream** menu
    // is reachable from both paths because `TogglePauseStream` only gates
    // on `IsStreamingActive()`) must not silently drop the buffered rows
    // on Resume. The bug was `CoalesceLocked` only moving `streamLines` /
    // `errors` / `newKeys`, dropping the `lines` and `localLineOffsets`
    // payload that the static path emits, and `PausedLineCountLocked`
    // only summing `streamLines.size()` (so the status-bar `K buffered`
    // would read 0 even with rows queued).
    void testSinkPauseResumePreservesStaticLineBatches()
    {
        // Need a real `LogFile` because the `FileLineSource` (and the
        // model's `LogTable::AppendBatch` path that consumes `lines`) hold
        // pointers into the file. The file's contents don't have to match
        // what we synthesise -- we never call `LogLine::GetValue()` on them
        // through the table -- but they must outlive the test.
        TempJsonFile fixture(QStringList{
            QStringLiteral(R"({"a": 1})"),
            QStringLiteral(R"({"a": 2})"),
            QStringLiteral(R"({"a": 3})"),
        });

        LogModel model;
        QSignalSpy lineCountSpy(&model, &LogModel::lineCountChanged);
        QVERIFY(lineCountSpy.isValid());

        auto file = std::make_unique<loglib::LogFile>(fixture.Path().toStdString());
        auto fileSource = std::make_unique<loglib::FileLineSource>(std::move(file));
        loglib::FileLineSource *sourcePtr = fileSource.get();
        static_cast<void>(model.BeginStreamingForSyncTest(std::move(fileSource)));

        QtStreamingLogSink *sink = model.Sink();
        QVERIFY(sink != nullptr);
        QVERIFY(sink->IsActive());

        loglib::KeyIndex &keys = sink->Keys();
        const loglib::KeyId keyId = keys.GetOrInsert(std::string("a"));

        sink->Pause();
        QVERIFY(sink->IsPaused());

        // Push three static-path batches (`lines` + paired
        // `localLineOffsets`) into the sink while paused.
        const int batchesWhilePaused = 3;
        const int linesPerBatch = 4;
        size_t cursorOffset = 0;
        for (int b = 0; b < batchesWhilePaused; ++b)
        {
            loglib::StreamedBatch batch;
            batch.firstLineNumber = static_cast<size_t>(b * linesPerBatch + 1);
            batch.lines.reserve(linesPerBatch);
            batch.localLineOffsets.reserve(linesPerBatch);
            for (int i = 0; i < linesPerBatch; ++i)
            {
                const size_t lineNumber = static_cast<size_t>(b * linesPerBatch + i + 1);
                std::vector<std::pair<loglib::KeyId, loglib::LogValue>> values;
                values.emplace_back(keyId, loglib::LogValue(static_cast<int64_t>(lineNumber)));
                batch.lines.emplace_back(std::move(values), keys, *sourcePtr, lineNumber);
                cursorOffset += 16;
                batch.localLineOffsets.push_back(static_cast<uint64_t>(cursorOffset));
            }
            sink->OnBatch(std::move(batch));
        }

        QCoreApplication::processEvents();
        // No queued lambdas fired through (we're paused), and the buffered
        // count must reflect the `lines` payload — not 0 as it would have
        // with the pre-fix `PausedLineCountLocked`.
        QCOMPARE(model.rowCount(), 0);
        QCOMPARE(static_cast<int>(sink->PausedLineCount()), batchesWhilePaused * linesPerBatch);

        // Resume coalesces all three batches and posts once. Pre-fix this
        // dropped every row because `CoalesceLocked` ignored `lines` /
        // `localLineOffsets`; post-fix the model gains the full count.
        sink->Resume();
        QVERIFY(!sink->IsPaused());
        QCoreApplication::processEvents();

        QCOMPARE(model.rowCount(), batchesWhilePaused * linesPerBatch);
        QCOMPARE(static_cast<int>(sink->PausedLineCount()), 0);

        model.EndStreaming(false);
    }

    // Pause + cap-shrink interaction: while paused, lowering
    // the retention cap must trim the paused buffer to `cap - visible`
    // (preserving the visible rows). Verified via PausedLineCount().
    void testPauseCapShrinkTrimsPausedBuffer()
    {
        LogModel model;
        loglib::StreamLineSource &streamSource = BeginSyntheticStreamSession(model);
        QtStreamingLogSink *sink = model.Sink();

        loglib::KeyIndex &keys = sink->Keys();
        const loglib::KeyId valueKey = keys.GetOrInsert(std::string("value"));

        model.SetRetentionCap(1000);
        model.AppendBatch(MakeSyntheticBatch(streamSource, keys, valueKey, 1, 100, /*declareNewKey=*/true));
        QCOMPARE(model.rowCount(), 100);

        sink->Pause();
        // 500 paused-buffer rows arrive while paused (visible stays at 100).
        sink->OnBatch(MakeSyntheticBatch(streamSource, keys, valueKey, 101, 500, /*declareNewKey=*/false));
        QCOMPARE(model.rowCount(), 100);
        QCOMPARE(static_cast<int>(sink->PausedLineCount()), 500);

        // Lower cap to (visible + 100). Visible rows must stay; paused buffer
        // must trim down to 100 so visible+buffered <= cap.
        model.SetRetentionCap(100 + 100);
        QCOMPARE(model.rowCount(), 100);
        QCOMPARE(static_cast<int>(sink->PausedLineCount()), 100);

        // Resume drains the trimmed buffer, FIFO-evicting the oldest visible
        // rows so the post-Resume visible count stays within the cap.
        sink->Resume();
        QCoreApplication::processEvents();
        QCOMPARE(model.rowCount(), 200);

        model.EndStreaming(false);
    }

    // End-to-end Stream Mode smoke test against a temp file tailed by
    // `TailingBytesProducer`. Mirrors `MainWindow::OpenLogStream`'s
    // flow but stays self-contained. Covers pre-fill, live append,
    // pause-freezes-view-with-buffer-keeps-growing, and resume-drains.
    // Retention cap is high (1000) so FIFO eviction stays inactive
    // here — the dedicated `testRetentionCap*` tests cover that.
    void testStreamModeOpensTailFileAndAppends()
    {
        TempLiveTailFile fixture;

        // Pre-fill: 100 lines on disk before the source opens.
        {
            std::string blob;
            for (int i = 0; i < 100; ++i)
            {
                blob += "{\"i\":" + std::to_string(i + 1) + ",\"phase\":\"prefill\"}\n";
            }
            fixture.Append(blob);
        }

        LogModel model;
        QSignalSpy lineCountSpy(&model, &LogModel::lineCountChanged);
        QVERIFY(lineCountSpy.isValid());
        QSignalSpy finishedSpy(&model, &LogModel::streamingFinished);
        QVERIFY(finishedSpy.isValid());

        model.SetRetentionCap(1000);

        // Use a fast poll cadence on the source so the test wall-clock stays
        // small; the production default is 250 ms which would push the test
        // budget over a CI runner's timeout for marginal benefit here.
        loglib::TailingBytesProducer::Options sourceOptions;
        sourceOptions.disableNativeWatcher = true; // poll-only for determinism
        sourceOptions.pollInterval = std::chrono::milliseconds(25);
        sourceOptions.rotationDebounce = std::chrono::milliseconds(250);

        std::filesystem::path filePath(fixture.Path().toStdString());
        auto source = std::make_unique<loglib::TailingBytesProducer>(filePath, /*retentionLines=*/1000, sourceOptions);
        auto streamSource = std::make_unique<loglib::StreamLineSource>(filePath, std::move(source));

        loglib::ParserOptions options;
        // Don't pass a configuration; the auto-promote heuristics aren't the
        // focus of this test. The model's `BeginStreaming(StreamLineSource)`
        // overrides `options.stopToken` with the sink's freshly-armed token.
        loglib::StopToken stopToken = model.BeginStreaming(std::move(streamSource), options);
        Q_UNUSED(stopToken);

        QVERIFY(model.IsStreamingActive());
        QVERIFY2(WaitForLineCount(model, 100, std::chrono::seconds(5)), "pre-fill must arrive within 5 s");
        QCOMPARE(model.rowCount(), 100);

        // Append 50 more lines while the stream is running.
        {
            std::string blob;
            for (int i = 0; i < 50; ++i)
            {
                blob += "{\"i\":" + std::to_string(101 + i) + ",\"phase\":\"running\"}\n";
            }
            fixture.Append(blob);
        }
        QVERIFY2(WaitForLineCount(model, 150, std::chrono::seconds(5)), "running-phase appends must arrive within 5 s");
        QCOMPARE(model.rowCount(), 150);

        // Pause: visible model freezes; subsequent appends land in the
        // paused buffer.
        QtStreamingLogSink *sink = model.Sink();
        QVERIFY(sink != nullptr);
        sink->Pause();
        QVERIFY(sink->IsPaused());

        {
            std::string blob;
            for (int i = 0; i < 50; ++i)
            {
                blob += "{\"i\":" + std::to_string(151 + i) + ",\"phase\":\"paused\"}\n";
            }
            fixture.Append(blob);
        }

        // Wait for the worker to drain the appended bytes into the paused
        // buffer. We can't `WaitForLineCount` here because visible row
        // count must stay frozen at 150 — the buffer is the side channel.
        // Drain Qt events too in case any lambdas slip through (they
        // shouldn't while paused).
        const auto pauseDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (sink->PausedLineCount() < 50 && std::chrono::steady_clock::now() < pauseDeadline)
        {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 25);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        QCOMPARE(model.rowCount(), 150);
        QCOMPARE(static_cast<int>(sink->PausedLineCount()), 50);

        // Resume coalesces the paused buffer into a single batch and posts
        // it once. The post-Resume row count is the cap-bounded sum.
        sink->Resume();
        QVERIFY(!sink->IsPaused());
        QVERIFY2(
            WaitForLineCount(model, 200, std::chrono::seconds(5)), "Resume must coalesce paused buffer within 5 s"
        );
        QCOMPARE(model.rowCount(), 200);
        QCOMPARE(static_cast<int>(sink->PausedLineCount()), 0);

        // Tear down cleanly. `Reset()` drives the teardown
        // sequence (Source::Stop -> sink::RequestStop -> watcher join ->
        // DropPendingBatches) so the test exits without dangling threads.
        model.Reset();
        QCoreApplication::processEvents();

        QVERIFY(!model.IsStreamingActive());
        QCOMPARE(model.rowCount(), 0);
    }

    // End-to-end smoke test for the UDP network-stream open path.
    // Mirrors `testStreamModeOpensTailFileAndAppends` but the byte
    // producer is a `UdpServerProducer` driven by a synchronous
    // `UdpLogClient` from `test_common`. We do not pop the
    // `NetworkStreamDialog` (modal -> would hang offscreen QPA);
    // instead we wire the producer directly into the model the same
    // way `MainWindow::OpenNetworkStream` does.
    void testStreamModeOpensUdpProducer()
    {
        loglib::UdpServerProducer::Options opts;
        opts.bindAddress = "127.0.0.1"; // ephemeral port
        auto producer = std::make_unique<loglib::UdpServerProducer>(opts);
        const uint16_t port = producer->BoundPort();
        QVERIFY(port != 0);
        const std::string display = producer->DisplayName();

        LogModel model;
        QSignalSpy lineCountSpy(&model, &LogModel::lineCountChanged);
        QVERIFY(lineCountSpy.isValid());
        model.SetRetentionCap(1000);

        auto streamSource =
            std::make_unique<loglib::StreamLineSource>(std::filesystem::path(display), std::move(producer));
        loglib::ParserOptions options;
        Q_UNUSED(model.BeginStreaming(std::move(streamSource), options));

        QVERIFY(model.IsStreamingActive());

        // Send 10 datagrams. Each lands as one line in the parser.
        test_common::UdpLogClient client("127.0.0.1", port);
        for (int i = 0; i < 10; ++i)
        {
            client.Send("{\"i\":" + std::to_string(i + 1) + ",\"phase\":\"udp\"}");
        }

        QVERIFY2(WaitForLineCount(model, 10, std::chrono::seconds(5)), "10 UDP lines must arrive within 5 s");
        QCOMPARE(model.rowCount(), 10);

        model.Reset();
        QCoreApplication::processEvents();
        QVERIFY(!model.IsStreamingActive());
    }

    // End-to-end smoke test for the TCP network-stream open path.
    // Single client, plaintext (TLS coverage lives in the lib-tree
    // `test_tcp_server_producer_tls.cpp` so the apptest does not need
    // to build OpenSSL into its link line when LOGLIB_NETWORK_TLS is
    // off on a developer build).
    void testStreamModeOpensTcpProducer()
    {
        loglib::TcpServerProducer::Options opts;
        opts.bindAddress = "127.0.0.1"; // ephemeral port
        auto producer = std::make_unique<loglib::TcpServerProducer>(opts);
        const uint16_t port = producer->BoundPort();
        QVERIFY(port != 0);
        const std::string display = producer->DisplayName();
        QVERIFY(display.starts_with("tcp://127.0.0.1:"));

        LogModel model;
        QSignalSpy lineCountSpy(&model, &LogModel::lineCountChanged);
        QVERIFY(lineCountSpy.isValid());
        model.SetRetentionCap(1000);

        auto streamSource =
            std::make_unique<loglib::StreamLineSource>(std::filesystem::path(display), std::move(producer));
        loglib::ParserOptions options;
        Q_UNUSED(model.BeginStreaming(std::move(streamSource), options));

        QVERIFY(model.IsStreamingActive());

        test_common::TcpLogClient client("127.0.0.1", port);
        for (int i = 0; i < 10; ++i)
        {
            client.Send("{\"i\":" + std::to_string(i + 1) + ",\"phase\":\"tcp\"}");
        }

        QVERIFY2(WaitForLineCount(model, 10, std::chrono::seconds(5)), "10 TCP lines must arrive within 5 s");
        QCOMPARE(model.rowCount(), 10);

        client.Close();
        model.Reset();
        QCoreApplication::processEvents();
        QVERIFY(!model.IsStreamingActive());
    }

    // The `actionOpenNetworkStream` UI entry must be reachable via
    // `FindUiAction` so apptest harnesses can locate it without going
    // through the QObject tree (the workaround documented in
    // `MainWindow::FindUiAction`).
    void testActionOpenNetworkStreamIsExposed()
    {
        // Local variable name avoids the `MainWindowTest::window`
        // member-shadow C4458 warning under MSVC.
        MainWindow mainWindow;
        QAction *action = mainWindow.FindUiAction(QStringLiteral("actionOpenNetworkStream"));
        QVERIFY2(action != nullptr, "actionOpenNetworkStream must be reachable via FindUiAction");
        // Sanity-check the keyboard accelerator the production UI ships.
        QCOMPARE(action->shortcut(), QKeySequence(QStringLiteral("Ctrl+Shift+N")));
    }

    //  regression: when the paused-buffer cap forces the
    // sink to drop its oldest entries, the count of lost lines must be
    // observable so the user knows rows were silently discarded during
    // the pause. This test pauses, feeds enough live-tail batches to
    // overflow the cap, and asserts both that `PausedDropCount()` > 0
    // and that the visible+buffered total stayed within the cap.
    void testPausedDropCountIsObservable()
    {
        LogModel model;
        loglib::StreamLineSource &streamSource = BeginSyntheticStreamSession(model);
        QtStreamingLogSink *sink = model.Sink();
        QVERIFY(sink != nullptr);

        const size_t cap = 100;
        model.SetRetentionCap(cap);

        QCOMPARE(static_cast<qulonglong>(sink->PausedDropCount()), qulonglong(0));

        loglib::KeyIndex &keys = sink->Keys();
        const loglib::KeyId valueKey = keys.GetOrInsert(std::string("value"));

        sink->Pause();
        QVERIFY(sink->IsPaused());

        // Push three 100-line batches while paused. The second and
        // third will each tip the buffer over the cap, evicting the
        // oldest 100 lines per batch. After the third batch the paused
        // buffer holds the most-recent 100 lines and the counter should
        // reflect ~200 evictions.
        const size_t batchLines = 100;
        const int batchesPushed = 3;
        for (int b = 0; b < batchesPushed; ++b)
        {
            const bool firstBatch = (b == 0);
            sink->OnBatch(MakeSyntheticBatch(
                streamSource, keys, valueKey, static_cast<size_t>(b) * batchLines + 1, batchLines, firstBatch
            ));
        }

        QCOMPARE(static_cast<int>(sink->PausedLineCount()), static_cast<int>(cap));
        // Each overflow batch evicted 100 lines; two of three batches
        // overflowed → at least 200 lines dropped. (Exactly 200 in
        // practice; assert >= to avoid pinning the exact count against
        // future eviction-policy tweaks.)
        QVERIFY2(
            sink->PausedDropCount() >= batchLines * (batchesPushed - 1),
            qPrintable(QStringLiteral("PausedDropCount=%1 must be >= %2")
                           .arg(static_cast<qulonglong>(sink->PausedDropCount()))
                           .arg(static_cast<qulonglong>(batchLines * (batchesPushed - 1))))
        );

        // Resume and Detach: the counter survives Resume (it's a
        // session-scoped history of drops). Only `Arm()` /
        // `DropPendingBatches()` reset it.
        const uint64_t droppedBeforeResume = sink->PausedDropCount();
        sink->Resume();
        QCoreApplication::processEvents();
        QCOMPARE(sink->PausedDropCount(), droppedBeforeResume);

        model.StopAndKeepRows();
        QCoreApplication::processEvents();
    }

    // Regression for the `linesDropped` snapshot bug.
    // When the head of `mPausedBatches` is a *static-content* batch
    // (`lines` + `localLineOffsets`, reachable when **Pause** is invoked
    // during a static-streaming parse from the **Stream** menu), the
    // eviction loop in `OnBatch` evicts the whole batch atomically even
    // when its row count exceeds the requested `toDrop` — the
    // `localLineOffsets` array can be longer than `lines`, so a partial
    // prefix trim would break the `LogTable::AppendBatch` invariant. The
    // pre-fix counter snapshotted `toDrop` up front, under-reporting the
    // overshoot to the status-bar "dropped while paused" indicator. The
    // fix accumulates the *actual* lines evicted as the loop runs.
    void testPausedDropCountReflectsStaticBatchOverEviction()
    {
        TempJsonFile fixture(QStringList{
            QStringLiteral(R"({"a": 1})"),
            QStringLiteral(R"({"a": 2})"),
            QStringLiteral(R"({"a": 3})"),
        });

        LogModel model;
        auto file = std::make_unique<loglib::LogFile>(fixture.Path().toStdString());
        auto fileSource = std::make_unique<loglib::FileLineSource>(std::move(file));
        loglib::FileLineSource *sourcePtr = fileSource.get();
        static_cast<void>(model.BeginStreamingForSyncTest(std::move(fileSource)));

        QtStreamingLogSink *sink = model.Sink();
        QVERIFY(sink != nullptr);

        const size_t cap = 100;
        model.SetRetentionCap(cap);
        QCOMPARE(static_cast<qulonglong>(sink->PausedDropCount()), qulonglong(0));

        loglib::KeyIndex &keys = sink->Keys();
        const loglib::KeyId valueKey = keys.GetOrInsert(std::string("value"));

        sink->Pause();
        QVERIFY(sink->IsPaused());

        // Step 1 — fill the paused buffer to the cap with a single
        // *static-content* batch (`lines` + `localLineOffsets`).
        const size_t staticBatchRows = cap; // 100
        {
            loglib::StreamedBatch batch;
            batch.firstLineNumber = 1;
            batch.newKeys.emplace_back(std::string("value"));
            batch.lines.reserve(staticBatchRows);
            batch.localLineOffsets.reserve(staticBatchRows);
            for (size_t i = 0; i < staticBatchRows; ++i)
            {
                const size_t lineNumber = i + 1;
                std::vector<std::pair<loglib::KeyId, loglib::LogValue>> values;
                values.emplace_back(valueKey, loglib::LogValue{static_cast<int64_t>(lineNumber)});
                batch.lines.emplace_back(std::move(values), keys, *sourcePtr, lineNumber);
                batch.localLineOffsets.push_back(lineNumber * 16);
            }
            sink->OnBatch(std::move(batch));
        }
        QCOMPARE(static_cast<int>(sink->PausedLineCount()), static_cast<int>(staticBatchRows));
        QCOMPARE(static_cast<qulonglong>(sink->PausedDropCount()), qulonglong(0));

        // Step 2 — push a small live-tail batch that overflows the cap by
        // a fraction of the static head. Pre-fix `toDrop = 30` was used
        // as the drop count; in reality the entire 100-row static head
        // is evicted (atomic on `hasStaticContent`), and the counter
        // must reflect that overshoot. Build the live-tail rows as
        // `LogLine`s referencing a no-producer `StreamLineSource` we
        // wire up directly (not via the model — the model is mid-static-
        // session here).
        const size_t overflowRows = 30;
        loglib::StreamLineSource liveTailSource(std::filesystem::path("synthetic"), nullptr);
        {
            loglib::StreamedBatch batch;
            batch.firstLineNumber = staticBatchRows + 1;
            batch.lines.reserve(overflowRows);
            for (size_t i = 0; i < overflowRows; ++i)
            {
                const size_t lineId = staticBatchRows + 1 + i;
                static_cast<void>(liveTailSource.AppendLine("synthetic", std::string{}));
                std::vector<std::pair<loglib::KeyId, loglib::internal::CompactLogValue>> compactValues;
                compactValues.emplace_back(
                    valueKey, loglib::internal::CompactLogValue::MakeInt64(static_cast<int64_t>(lineId))
                );
                batch.lines.emplace_back(std::move(compactValues), keys, liveTailSource, lineId);
            }
            sink->OnBatch(std::move(batch));
        }

        // Whole 100-row static head was evicted; only the 30-row live-tail
        // batch survives in the buffer. Pre-fix the counter would report
        // 30 (the snapshot of `toDrop`); post-fix it reports the actual
        // 100 lines that left the buffer.
        QCOMPARE(static_cast<int>(sink->PausedLineCount()), static_cast<int>(overflowRows));
        QCOMPARE(static_cast<qulonglong>(sink->PausedDropCount()), static_cast<qulonglong>(staticBatchRows));

        sink->Resume();
        QCoreApplication::processEvents();
        model.StopAndKeepRows();
        QCoreApplication::processEvents();
    }

    //  regression: `Stop` ends the streaming session but leaves
    // the most-recently-buffered rows visible (the model becomes a
    // static snapshot of what was in memory at stop time). `StopAndKeepRows()`
    // is the API `MainWindow::StopStream` uses; `Reset()` (which fully
    // resets the model) is reserved for the "open a new session" paths.
    void testStopAndKeepRowsPreservesRows()
    {
        LogModel model;
        QSignalSpy finishedSpy(&model, &LogModel::streamingFinished);
        QVERIFY(finishedSpy.isValid());
        QSignalSpy lineCountSpy(&model, &LogModel::lineCountChanged);
        QVERIFY(lineCountSpy.isValid());

        loglib::StreamLineSource &streamSource = BeginSyntheticStreamSession(model);
        QVERIFY(model.IsStreamingActive());

        // Feed one batch of synthetic stream rows so `StopAndKeepRows` has
        // something to preserve.
        loglib::KeyIndex &keys = model.Sink()->Keys();
        const loglib::KeyId valueKey = keys.GetOrInsert(std::string("value"));

        const int batchSize = 7;
        model.AppendBatch(MakeSyntheticBatch(
            streamSource, keys, valueKey, /*firstLineId=*/1, static_cast<size_t>(batchSize), /*declareNewKey=*/true
        ));
        QCOMPARE(model.rowCount(), batchSize);

        // StopAndKeepRows: streaming flag flips off, compensating
        // `streamingFinished(Cancelled)` fires, but the table keeps
        // every row.
        model.StopAndKeepRows();
        QCoreApplication::processEvents();

        QVERIFY(!model.IsStreamingActive());
        QCOMPARE(model.rowCount(), batchSize);
        QCOMPARE(finishedSpy.count(), 1);
        QCOMPARE(finishedSpy.takeFirst().value(0).value<StreamingResult>(), StreamingResult::Cancelled);

        // A second StopAndKeepRows on an already-idle model is a no-op (no
        // spurious `streamingFinished`, no row mutation).
        model.StopAndKeepRows();
        QCOMPARE(finishedSpy.count(), 0);
        QCOMPARE(model.rowCount(), batchSize);

        // Verify the surviving rows are still queryable (the user can
        // still sort / filter / copy as the ).
        const int valueColumn = ColumnByHeader(model, QStringLiteral("value"));
        QVERIFY(valueColumn >= 0);
        QCOMPARE(model.data(model.index(0, valueColumn), LogModelItemDataRole::SortRole).toLongLong(), qint64(1));
        QCOMPARE(
            model.data(model.index(batchSize - 1, valueColumn), LogModelItemDataRole::SortRole).toLongLong(),
            qint64(batchSize)
        );

        // Reset() on the post-StopAndKeepRows state performs the full reset
        // (no streaming active, so no second `streamingFinished`).
        model.Reset();
        QCOMPARE(model.rowCount(), 0);
        QCOMPARE(finishedSpy.count(), 0);
    }

    // Ordering regression: `Resume()` must deliver the coalesced paused
    // buffer **before** any subsequent `OnBatch` (running on the worker
    // thread, observing the now-cleared `mPaused`) can land in the model.
    //
    // The pre-fix `Resume()` cleared `mPaused` synchronously and then
    // posted the coalesced buffer via `Qt::QueuedConnection`. Between
    // those two steps a worker `OnBatch` could see `mPaused == false`,
    // skip the buffer, and post its own newer batch — also via
    // `QueuedConnection`. The Qt event queue is FIFO, but because both
    // posts happen from different threads ordering depends entirely on
    // wall-clock arrival, so the newer batch could be processed first.
    //
    // The synchronous-Resume contract this test pins down is the
    // strongest: after `Resume()` returns, the paused-buffer rows are
    // already in the model — so any later `OnBatch` (queued or direct)
    // necessarily lands after them. We don't try to win the worker race
    // here; the synchronous contract removes the race entirely.
    void testResumeDeliversBufferedBatchSynchronouslyForOrdering()
    {
        LogModel model;
        loglib::StreamLineSource &streamSource = BeginSyntheticStreamSession(model);
        QtStreamingLogSink *sink = model.Sink();
        QVERIFY(sink != nullptr);

        loglib::KeyIndex &keys = sink->Keys();
        const loglib::KeyId valueKey = keys.GetOrInsert(std::string("value"));

        sink->Pause();
        QVERIFY(sink->IsPaused());

        const size_t pausedCount = 8;
        sink->OnBatch(MakeSyntheticBatch(streamSource, keys, valueKey, 1, pausedCount, /*declareNewKey=*/true));
        QCOMPARE(sink->PausedLineCount(), pausedCount);
        QCOMPARE(model.rowCount(), 0);

        // Synchronous-Resume contract: the coalesced paused buffer must
        // already be in the model the instant `Resume()` returns. Pre-
        // fix this required `processEvents()` to run the queued lambda;
        // post-fix it's `mModel->AppendBatch(...)` called inline.
        sink->Resume();
        QVERIFY(!sink->IsPaused());
        QCOMPARE(static_cast<size_t>(model.rowCount()), pausedCount);
        QCOMPARE(sink->PausedLineCount(), size_t{0});

        // Drive a follow-up `OnBatch` directly (mirroring a worker that
        // fires immediately after `mPaused` was cleared) — its rows must
        // append *after* the paused-buffer rows because Resume already
        // delivered them. With the pre-fix code a queued post-Resume
        // batch could arrive at the model first if it happened to land
        // in the Qt queue before the Resume lambda; this test would not
        // trigger that exact race, but the synchronous contract makes
        // the inversion impossible by construction. We assert the
        // resulting row order matches the input order.
        const size_t followupCount = 4;
        sink->OnBatch(
            MakeSyntheticBatch(streamSource, keys, valueKey, pausedCount + 1, followupCount, /*declareNewKey=*/false)
        );
        QCoreApplication::processEvents();
        QCOMPARE(static_cast<size_t>(model.rowCount()), pausedCount + followupCount);

        const int valueColumn = ColumnByHeader(model, QStringLiteral("value"));
        QVERIFY(valueColumn >= 0);
        // Row 0 must be the first paused-buffer line (lineId == 1) —
        // not the post-Resume batch's first line (lineId == 9).
        QCOMPARE(model.data(model.index(0, valueColumn), LogModelItemDataRole::SortRole).toLongLong(), qint64(1));
        // Last row is the post-Resume batch's last line.
        QCOMPARE(
            model
                .data(
                    model.index(static_cast<int>(pausedCount + followupCount) - 1, valueColumn),
                    LogModelItemDataRole::SortRole
                )
                .toLongLong(),
            static_cast<qint64>(pausedCount + followupCount)
        );

        model.EndStreaming(false);
    }

    // Bug 2 regression: `actionPauseStream` is checkable and lives in
    // both the toolbar and the **Stream** menu. The menu stays reachable
    // even with the toolbar hidden. Without an explicit disable while
    // idle, a menu click would flip the action's checked state before
    // `TogglePauseStream`'s `IsStreamingActive()` early-return runs;
    // the toggle then survives into the next session — and the teardown
    // path *cannot* unstick it because `LogModel::Reset()` only emits
    // `streamingFinished` when the prior session was still active.
    //
    // The fix disables the three Stream actions whenever the toolbar
    // is hidden, and force-unchecks `actionPauseStream` defensively so
    // a stuck checked state from any other path can never persist.
    void testStreamMenuActionsDisabledWhileIdle()
    {
        QAction *pauseAction = FindActionByObjectName(window, QStringLiteral("actionPauseStream"));
        QAction *followAction = FindActionByObjectName(window, QStringLiteral("actionFollowTail"));
        QAction *stopAction = FindActionByObjectName(window, QStringLiteral("actionStopStream"));
        QVERIFY(pauseAction != nullptr);
        QVERIFY(followAction != nullptr);
        QVERIFY(stopAction != nullptr);

        // Idle at startup: no stream has been opened yet, the toolbar is
        // hidden, and the Stream menu actions must be disabled so a
        // user click on the menu cannot flip the checked state.
        QVERIFY(!pauseAction->isEnabled());
        QVERIFY(!followAction->isEnabled());
        QVERIFY(!stopAction->isEnabled());
        QVERIFY(!pauseAction->isChecked());

        // `trigger()` ignores disabled actions (Qt won't deliver the
        // `triggered` / `toggled` signals on a disabled action), so even
        // a programmatic activation while idle leaves the checked state
        // alone. This is the user-facing guarantee.
        pauseAction->trigger();
        QVERIFY(!pauseAction->isChecked());
    }

    // Companion to `testStreamMenuActionsDisabledWhileIdle`: even if
    // some external path manages to flip `actionPauseStream` checked
    // (a future caller that programmatically re-enables and checks
    // the action; a test that pokes the QAction directly), the
    // `streamingFinished` slot must clear that stale state at the
    // next session boundary. This validates that my new disable-gate
    // in `UpdateStreamToolbarVisibility` did not interfere with the
    // existing Pause-toggle reset in the slot.
    void testStaleCheckedPauseClearedOnTeardown()
    {
        QAction *pauseAction = FindActionByObjectName(window, QStringLiteral("actionPauseStream"));
        QVERIFY(pauseAction != nullptr);

        // Simulate the bug condition: forcibly enable the action and
        // set it checked while idle (matching the Stream-menu click
        // that the pre-fix code allowed).
        pauseAction->setEnabled(true);
        pauseAction->setChecked(true);
        QVERIFY(pauseAction->isChecked());

        // Drive a streaming session and tear it down. `StopAndKeepRows()` emits
        // a compensating `streamingFinished(Cancelled)` whose slot
        // (in `MainWindow`) resets the Pause toggle and refreshes the
        // toolbar gating. This is the existing reset path that must
        // keep working alongside the new disabled-while-idle invariant.
        LogModel *model = window->findChild<LogModel *>();
        QVERIFY(model != nullptr);

        static_cast<void>(model->BeginStreamingForSyncTest(std::unique_ptr<loglib::LineSource>{}));
        model->StopAndKeepRows();
        QCoreApplication::processEvents();

        // After teardown the action is back to its idle baseline:
        // unchecked (cleared by the `streamingFinished` slot) and
        // disabled (by `UpdateStreamToolbarVisibility` since the
        // toolbar is no longer visible). The next legitimate session
        // will re-enable it from a clean baseline.
        QVERIFY(!pauseAction->isChecked());
        QVERIFY(!pauseAction->isEnabled());
    }

    // Regression: hovering over table cells (and other Qt-internal,
    // layout-driven scrollbar updates) must not auto-disengage the
    // **Follow newest** toggle. Pre-fix, `LogTableView` connected the
    // verticalScrollBar's `valueChanged` signal directly to the
    // edge-trigger lambda — so any programmatic value change (e.g. an
    // `endInsertRows` clamp, a hover/repaint-induced scroll adjustment,
    // a viewport-size update) flipped `mAtTailEdge` from true to false
    // and emitted `userScrolledAwayFromTail`, which the `MainWindow`
    // then dutifully translated into "uncheck Follow newest" — even
    // though the user never scrolled.
    //
    // The fix gates `userScrolledAwayFromTail` /
    // `userScrolledToTail` emissions on a user-input depth counter
    // that's only non-zero during real wheel / keyboard / scrollbar-
    // mouse events. A bare programmatic `setValue` to the middle of
    // the range — the closest in-process simulation of a hover-
    // triggered internal scroll update — must therefore leave the
    // toggle alone.
    void testFollowTailIgnoresProgrammaticScrollbarChanges()
    {
        QAction *followAction = FindActionByObjectName(window, QStringLiteral("actionFollowTail"));
        LogTableView *tableView = window->findChild<LogTableView *>();
        QVERIFY(followAction != nullptr);
        QVERIFY(tableView != nullptr);

        // Enable + check the toggle from the simulated streaming-active
        // baseline (the production path enables it when a stream opens
        // via `UpdateStreamToolbarVisibility`). We bypass the gate here
        // because the test only needs the toggle's checked state to
        // observe the scrollbar→toggle wiring in isolation.
        followAction->setEnabled(true);
        followAction->setChecked(true);
        QVERIFY(followAction->isChecked());

        // Force the scrollbar into a valid range so `setValue` can push
        // the slider somewhere genuinely "not at bottom". Without this
        // setup the maximum stays at 0 and the at-bottom check is
        // trivially true regardless of value.
        QScrollBar *scrollBar = tableView->verticalScrollBar();
        QVERIFY(scrollBar != nullptr);
        scrollBar->setRange(0, 1000);
        scrollBar->setValue(scrollBar->maximum());

        QSignalSpy awaySpy(tableView, &LogTableView::userScrolledAwayFromTail);
        QVERIFY(awaySpy.isValid());

        // Programmatic value change to the middle of the range — the
        // in-process equivalent of a layout-driven scroll adjustment
        // that Qt fires on hover / repaint. Pre-fix this flipped
        // `actionFollowTail` to unchecked.
        scrollBar->setValue(scrollBar->maximum() / 2);
        QCoreApplication::processEvents();

        QCOMPARE(awaySpy.count(), 0);
        QVERIFY(followAction->isChecked());
    }

    // The **Show newest lines first** preference flips the
    // `RowOrderProxyModel` into reversed mode: the highest source
    // row index (the most-recently-appended streamed line) lands at
    // proxy row 0 and the oldest at the bottom of the visible model.
    // Drives the proxy directly (no Preferences dialog round-trip)
    // because the dialog's Ok handler is just a thin wrapper around
    // `RowOrderProxyModel::SetReversed` plus the persisted
    // `streaming/newestFirst` setting — exercising the proxy here
    // covers the contract that the GUI relies on.
    void testNewestFirstReversesProxyOrder()
    {
        RowOrderProxyModel *rowOrderProxy = window->findChild<RowOrderProxyModel *>();
        LogModel *model = window->findChild<LogModel *>();
        QVERIFY(rowOrderProxy != nullptr);
        QVERIFY(model != nullptr);
        QVERIFY(!rowOrderProxy->IsReversed());

        // Drive a tiny streaming session with three rows so the proxy
        // has something to reorder. `valueKey` plus a per-line integer
        // gives us a deterministic identifier we can read back via
        // `data(SortRole)` to assert the visible row order.
        loglib::StreamLineSource &streamSource = BeginSyntheticStreamSession(*model);
        QtStreamingLogSink *sink = model->Sink();
        QVERIFY(sink != nullptr);

        loglib::KeyIndex &keys = sink->Keys();
        const loglib::KeyId valueKey = keys.GetOrInsert(std::string("value"));
        sink->OnBatch(MakeSyntheticBatch(streamSource, keys, valueKey, 1, 3, /*declareNewKey=*/true));
        QCoreApplication::processEvents();
        QCOMPARE(model->rowCount(), 3);

        const int valueColumn = ColumnByHeader(*model, QStringLiteral("value"));
        QVERIFY(valueColumn >= 0);

        // Default (oldest-first) order: proxy row 0 == source row 0
        // == lineId 1, proxy row 2 == source row 2 == lineId 3.
        QCOMPARE(
            rowOrderProxy->data(rowOrderProxy->index(0, valueColumn), LogModelItemDataRole::SortRole).toLongLong(),
            qint64(1)
        );
        QCOMPARE(
            rowOrderProxy->data(rowOrderProxy->index(2, valueColumn), LogModelItemDataRole::SortRole).toLongLong(),
            qint64(3)
        );

        rowOrderProxy->SetReversed(true);
        QVERIFY(rowOrderProxy->IsReversed());

        // Reversed order: proxy row 0 carries the *most-recently-
        // appended* line (lineId 3), and proxy row 2 the oldest
        // (lineId 1). The source model (`model`) is untouched — only
        // the proxy mapping flips, so the underlying append-order
        // contract continues to hold.
        QCOMPARE(
            rowOrderProxy->data(rowOrderProxy->index(0, valueColumn), LogModelItemDataRole::SortRole).toLongLong(),
            qint64(3)
        );
        QCOMPARE(
            rowOrderProxy->data(rowOrderProxy->index(2, valueColumn), LogModelItemDataRole::SortRole).toLongLong(),
            qint64(1)
        );

        // Toggling back leaves the source order intact and restores
        // the identity mapping (idempotency contract for
        // `SetReversed`).
        rowOrderProxy->SetReversed(false);
        QVERIFY(!rowOrderProxy->IsReversed());
        QCOMPARE(
            rowOrderProxy->data(rowOrderProxy->index(0, valueColumn), LogModelItemDataRole::SortRole).toLongLong(),
            qint64(1)
        );

        model->EndStreaming(false);
    }

    // Regression for incremental streaming: `QSortFilterProxyModel`
    // does not reliably keep descending-by-insertion-order stable
    // across successive `rowsInserted` unless we queue an explicit
    // `sort()` after each structural change. **Show newest lines
    // first** enables reversed mode before any data (matching session
    // startup); the second batch must land with its newest line at
    // proxy row 0, not stuck under the first batch.
    void testNewestFirstIncrementalBatchesKeepNewestAtTop()
    {
        RowOrderProxyModel *rowOrderProxy = window->findChild<RowOrderProxyModel *>();
        LogModel *model = window->findChild<LogModel *>();
        QVERIFY(rowOrderProxy != nullptr);
        QVERIFY(model != nullptr);

        loglib::StreamLineSource &streamSource = BeginSyntheticStreamSession(*model);
        QtStreamingLogSink *sink = model->Sink();
        QVERIFY(sink != nullptr);

        rowOrderProxy->SetReversed(true);
        QVERIFY(rowOrderProxy->IsReversed());

        loglib::KeyIndex &keys = sink->Keys();
        const loglib::KeyId valueKey = keys.GetOrInsert(std::string("value"));

        sink->OnBatch(MakeSyntheticBatch(streamSource, keys, valueKey, 1, 3, /*declareNewKey=*/true));
        QCoreApplication::processEvents();
        QCOMPARE(model->rowCount(), 3);

        const int valueColumn = ColumnByHeader(*model, QStringLiteral("value"));
        QVERIFY(valueColumn >= 0);

        QCOMPARE(
            rowOrderProxy->data(rowOrderProxy->index(0, valueColumn), LogModelItemDataRole::SortRole).toLongLong(),
            qint64(3)
        );

        sink->OnBatch(MakeSyntheticBatch(streamSource, keys, valueKey, 4, 2, /*declareNewKey=*/false));
        QCoreApplication::processEvents();
        QCOMPARE(model->rowCount(), 5);

        QCOMPARE(
            rowOrderProxy->data(rowOrderProxy->index(0, valueColumn), LogModelItemDataRole::SortRole).toLongLong(),
            qint64(5)
        );

        model->EndStreaming(false);
    }

    // Guard test: toggling `RowOrderProxyModel::SetReversed` must be
    // O(persistent indices), not O(rows). Regression here would mean
    // we accidentally re-introduced a `QSortFilterProxyModel`-style
    // full mapping rebuild — which is the exact O(N²) trap Phase 3
    // was written to fix.
    //
    // Method: install a single persistent index, fill the model with
    // 10'000 rows (one order of magnitude past anything Qt's painter
    // visits per frame), toggle 100 times, and assert the wall-time
    // stays well under a budget that a full per-row rebuild would
    // blow. Set deliberately loose to absorb CI noise; the failing
    // implementation routinely lands at multiple seconds, while the
    // current one lands in single-digit ms.
    void testNewestFirstToggleIsOPersistentIndices()
    {
        RowOrderProxyModel *rowOrderProxy = window->findChild<RowOrderProxyModel *>();
        LogModel *model = window->findChild<LogModel *>();
        QVERIFY(rowOrderProxy != nullptr);
        QVERIFY(model != nullptr);

        loglib::StreamLineSource &streamSource = BeginSyntheticStreamSession(*model);
        QtStreamingLogSink *sink = model->Sink();
        QVERIFY(sink != nullptr);

        constexpr int kRows = 10'000;
        loglib::KeyIndex &keys = sink->Keys();
        const loglib::KeyId valueKey = keys.GetOrInsert(std::string("value"));
        sink->OnBatch(MakeSyntheticBatch(streamSource, keys, valueKey, 1, kRows, /*declareNewKey=*/true));
        QCoreApplication::processEvents();
        QCOMPARE(model->rowCount(), kRows);

        // Hold a single persistent index from somewhere in the
        // middle of the proxy. Toggling SetReversed must remap *this
        // one* via `changePersistentIndexList`; nothing else.
        const QPersistentModelIndex pinned(rowOrderProxy->index(kRows / 2, 0));
        QVERIFY(pinned.isValid());

        rowOrderProxy->SetReversed(false);
        QVERIFY(!rowOrderProxy->IsReversed());

        QElapsedTimer timer;
        timer.start();
        constexpr int kToggles = 100;
        for (int i = 0; i < kToggles; ++i)
        {
            rowOrderProxy->SetReversed(i % 2 == 1);
        }
        const qint64 elapsedMs = timer.elapsed();

        // Budget: 200 ms for 100 toggles over 10'000 rows. The
        // QSortFilterProxyModel-based predecessor in newest-first
        // mode was O(N log N) per toggle and timed in seconds for
        // this row count.
        QVERIFY2(
            elapsedMs < 200,
            qPrintable(QStringLiteral("RowOrderProxyModel::SetReversed must be O(persistent indices), "
                                      "not O(rows); 100 toggles over %1 rows took %2 ms (budget 200 ms)")
                           .arg(kRows)
                           .arg(elapsedMs))
        );

        // Sanity check: the persistent index correctly tracks the
        // mapping flip. After the last toggle (kToggles-1=99 -> odd
        // -> reversed=true), the pinned source row (kRows/2 in
        // identity, which is what the constructor saw) should sit at
        // proxy row (kRows - 1 - kRows/2).
        QVERIFY(pinned.isValid());
        QCOMPARE(pinned.row(), kRows - 1 - kRows / 2);

        rowOrderProxy->SetReversed(false);
        model->EndStreaming(false);
    }

    // Companion to `testNewestFirstReversesProxyOrder`: the user-input
    // gate on the `LogTableView` must follow the configured tail edge
    // when **Show newest lines first** is enabled. Specifically:
    //
    //   - With `TailEdge::Top`, "at tail" is `value <= minimum` (the
    //     newest row sits at proxy row 0, which is at the top of the
    //     scroll viewport).
    //   - A user scrolling down (away from the top) must edge-trigger
    //     `userScrolledAwayFromTail` so `MainWindow` disengages
    //     Follow newest.
    //   - A user scrolling back to the top must edge-trigger
    //     `userScrolledToTail` so Follow newest re-engages.
    //
    // The companion `testFollowTailIgnoresProgrammaticScrollbarChanges`
    // already covers the bottom edge; this test pins down the top edge
    // contract introduced by the newest-first feature.
    void testTailEdgeTopFollowsScrollbarMinimum()
    {
        LogTableView *tableView = window->findChild<LogTableView *>();
        QVERIFY(tableView != nullptr);

        tableView->SetTailEdge(LogTableView::TailEdge::Top);
        QCOMPARE(tableView->GetTailEdge(), LogTableView::TailEdge::Top);

        QScrollBar *scrollBar = tableView->verticalScrollBar();
        QVERIFY(scrollBar != nullptr);
        scrollBar->setRange(0, 1000);
        scrollBar->setValue(scrollBar->minimum());

        // Programmatic value change to the middle: must not flip the
        // toggle (mirrors the bottom-edge programmatic-scroll test).
        QSignalSpy awaySpy(tableView, &LogTableView::userScrolledAwayFromTail);
        QSignalSpy toSpy(tableView, &LogTableView::userScrolledToTail);
        QVERIFY(awaySpy.isValid());
        QVERIFY(toSpy.isValid());

        scrollBar->setValue(scrollBar->maximum() / 2);
        QCoreApplication::processEvents();
        QCOMPARE(awaySpy.count(), 0);
        QCOMPARE(toSpy.count(), 0);

        // Reset the view to "at tail" (top) so the next user-initiated
        // event can flip the edge tracking. We bypass the user-input
        // gate by re-seeding via `SetTailEdge`, which is the production
        // path the `MainWindow` takes when `ApplyDisplayOrder`
        // re-orients the view.
        scrollBar->setValue(scrollBar->minimum());
        QCoreApplication::processEvents();
        tableView->SetTailEdge(LogTableView::TailEdge::Top);

        // Simulate a real user wheel-scroll down. `wheelEvent` on the
        // viewport sets `mNextValueChangeIsUser`, so the resulting
        // `valueChanged` *is* attributed to the user — and because
        // the tail edge is Top, leaving the minimum must emit
        // `userScrolledAwayFromTail`.
        QWheelEvent wheelDown(
            QPointF(10, 10), // position in viewport
            tableView->viewport()->mapToGlobal(QPointF(10, 10)),
            QPoint(0, -120), // pixelDelta (downward)
            QPoint(0, -120), // angleDelta (downward)
            Qt::NoButton,
            Qt::NoModifier,
            Qt::NoScrollPhase,
            false
        );
        QCoreApplication::sendEvent(tableView->viewport(), &wheelDown);
        QCoreApplication::processEvents();

        QCOMPARE(awaySpy.count(), 1);
        QCOMPARE(toSpy.count(), 0);
    }

    // Regression for the user-reported "Follow newest never auto-
    // disables when I scroll manually" bug: the previous
    // `installEventFilter`-based user-input gate ran *before* the
    // scrollbar processed the event, so direct interactions with the
    // scrollbar widget (drag, arrow / track click, Page Up / Down,
    // Home / End) saw `mUserInteractionDepth` flip back to zero
    // before `valueChanged` actually fired — and the edge transition
    // was therefore treated as programmatic, not user-initiated.
    //
    // The fix replaces the filter with a connection to
    // `QAbstractSlider::actionTriggered`, which fires synchronously
    // *before* the slider value is updated. `triggerAction(...)` is
    // the public API the same internal handlers (mouse press on the
    // arrows, key press on the slider) ultimately call, so driving
    // it directly here is the closest in-process equivalent of the
    // user clicking the scrollbar.
    void testFollowNewestDisengagesOnScrollbarAction()
    {
        QAction *followAction = FindActionByObjectName(window, QStringLiteral("actionFollowTail"));
        LogTableView *tableView = window->findChild<LogTableView *>();
        QVERIFY(followAction != nullptr);
        QVERIFY(tableView != nullptr);

        followAction->setEnabled(true);
        followAction->setChecked(true);
        QVERIFY(followAction->isChecked());

        QScrollBar *scrollBar = tableView->verticalScrollBar();
        QVERIFY(scrollBar != nullptr);
        scrollBar->setRange(0, 1000);
        scrollBar->setValue(scrollBar->maximum());

        QSignalSpy awaySpy(tableView, &LogTableView::userScrolledAwayFromTail);
        QVERIFY(awaySpy.isValid());

        // `triggerAction(SliderToMinimum)` mirrors a Home keypress on
        // the scrollbar: the slider emits `actionTriggered`, our slot
        // sets `mNextValueChangeIsUser`, then the action proper
        // updates the slider value and fires `valueChanged` — which
        // now sees the user flag and emits `userScrolledAwayFromTail`.
        scrollBar->triggerAction(QAbstractSlider::SliderToMinimum);
        QCoreApplication::processEvents();

        QCOMPARE(awaySpy.count(), 1);
        QVERIFY(!followAction->isChecked());
    }

    // Companion to `testFollowNewestDisengagesOnScrollbarAction`:
    // **Show newest lines first** mode preserves the user's reading
    // position when new lines arrive. Without preservation each
    // incoming batch grows the proxy's row count above the user's
    // current viewport row, the scrollbar value stays where it was,
    // and the visible content visually shifts down by the new rows'
    // height. With preservation the view records the topmost visible
    // row + its pixel offset before the structural change and
    // re-aligns the scrollbar after, so the row stays at the same
    // pixel position from the user's perspective (chat-app pattern).
    void testNewestFirstPreservesReadingPositionAcrossBatches()
    {
        RowOrderProxyModel *rowOrderProxy = window->findChild<RowOrderProxyModel *>();
        LogTableView *tableView = window->findChild<LogTableView *>();
        LogModel *model = window->findChild<LogModel *>();
        QAction *followAction = FindActionByObjectName(window, QStringLiteral("actionFollowTail"));
        QVERIFY(rowOrderProxy != nullptr);
        QVERIFY(tableView != nullptr);
        QVERIFY(model != nullptr);
        QVERIFY(followAction != nullptr);

        // The preservation hook is the **Follow newest OFF, user is
        // reading** path. In production a real scroll-away would
        // disengage Follow newest via `userScrolledAwayFromTail`; the
        // test bypasses that path by writing the scrollbar value
        // programmatically below, so we have to uncheck the action
        // up front to match the same end state.
        followAction->setEnabled(true);
        followAction->setChecked(false);

        // Force a known viewport size so `visualRect` / `indexAt`
        // produce meaningful pixel coordinates and the scrollbar
        // reaches a non-zero range below.
        tableView->resize(400, 200);
        tableView->show();
        QCoreApplication::processEvents();

        // Reverse the proxy and orient the table view's tail edge so
        // the preservation hook (which only fires for `TailEdge::Top`)
        // is in the right configuration. Mirrors what
        // `MainWindow::ApplyDisplayOrder` does in production.
        rowOrderProxy->SetReversed(true);
        tableView->SetTailEdge(LogTableView::TailEdge::Top);

        loglib::StreamLineSource &streamSource = BeginSyntheticStreamSession(*model);
        QtStreamingLogSink *sink = model->Sink();
        QVERIFY(sink != nullptr);

        loglib::KeyIndex &keys = sink->Keys();
        const loglib::KeyId valueKey = keys.GetOrInsert(std::string("value"));

        // Seed the model with enough rows that the viewport is
        // genuinely scrollable.
        sink->OnBatch(MakeSyntheticBatch(streamSource, keys, valueKey, 1, 50, /*declareNewKey=*/true));
        QCoreApplication::processEvents();
        QCOMPARE(model->rowCount(), 50);

        QScrollBar *vbar = tableView->verticalScrollBar();
        QVERIFY(vbar != nullptr);
        if (vbar->maximum() == 0)
        {
            QSKIP("offscreen layout did not produce a scrollable viewport for the seeded rows");
        }

        // Scroll to the middle so the view is *not* at the
        // configured tail edge — preservation only kicks in here.
        const int midValue = vbar->maximum() / 2;
        vbar->setValue(midValue);
        QCoreApplication::processEvents();
        QVERIFY(vbar->value() > 0);

        // Capture the topmost visible row (mapped through both
        // proxies down to the source model) and the scrollbar value
        // at the start of the structural change.
        const QModelIndex topProxyBefore = tableView->indexAt(QPoint(0, 1));
        QVERIFY2(topProxyBefore.isValid(), "indexAt(0,1) must land inside a row");
        const int valueBefore = vbar->value();

        // Push another batch. In reversed mode the new rows land at
        // the visual top of the proxy; without preservation the
        // scrollbar value stays at `valueBefore` and the visible
        // content slides down by the new rows' total height. With
        // preservation the scrollbar advances by that same amount so
        // the previously-visible content stays put.
        sink->OnBatch(MakeSyntheticBatch(streamSource, keys, valueKey, 51, 5, /*declareNewKey=*/false));
        QCoreApplication::processEvents();
        QCOMPARE(model->rowCount(), 55);

        const int valueAfter = vbar->value();
        QVERIFY2(
            valueAfter > valueBefore,
            qPrintable(QStringLiteral("scrollbar value should advance to compensate for the new top rows; "
                                      "before=%1 after=%2")
                           .arg(valueBefore)
                           .arg(valueAfter))
        );

        model->EndStreaming(false);
    }

    // Regression: the alternating-row colours used to flip on every
    // newest-first batch arrival because Qt's stock `alternateRowColors`
    // is keyed off the visual row index and a top-insert shifts every
    // existing row's parity. We initially tried to pin the parity to
    // the source row in a custom delegate, but the CSS-based table
    // stylesheet (`alternate-background-color`) bypassed the
    // delegate's `QStyleOptionViewItem::Alternate` override and the
    // rows kept flickering. The accepted fallback is to disable
    // alternating rows entirely while newest-first is active and let
    // the table render with a single base tone there; the default
    // bottom-tail mode keeps the visual reading aid because rows
    // append at the bottom (visual parity is stable).
    //
    // This test asserts the toggle in `MainWindow::ApplyDisplayOrder`
    // mirrors the persisted `StreamingControl::IsNewestFirst()` value
    // both ways while a **stream-mode** session is active. We stage the
    // session mode via `BeginSyntheticStreamSession`, poke the
    // preference, fire the apply path, and read back
    // `QTableView::alternatingRowColors`. The companion
    // `testAlternatingRowColoursDisabledInStaticNewestFirstMode`
    // asserts the same contract for static-mode sessions.
    void testAlternatingRowColoursDisabledInNewestFirstMode()
    {
        LogTableView *tableView = window->findChild<LogTableView *>();
        LogModel *model = window->findChild<LogModel *>();
        QVERIFY(tableView != nullptr);
        QVERIFY(model != nullptr);

        const bool originalNewestFirst = StreamingControl::IsNewestFirst();
        auto restoreNewestFirst = qScopeGuard([this, originalNewestFirst]() {
            StreamingControl::SetNewestFirst(originalNewestFirst);
            window->SetSessionModeForTest(MainWindow::TestSessionMode::Idle);
        });

        // Stage a stream-mode session so `ApplyDisplayOrder` consults
        // `IsNewestFirst()` (and not `IsStaticNewestFirst()`).
        static_cast<void>(BeginSyntheticStreamSession(*model));
        window->SetSessionModeForTest(MainWindow::TestSessionMode::LiveTail);

        // Default-mode baseline: alternation is on so users still get
        // the lighter/darker reading aid while reading static logs or
        // a bottom-tail stream.
        StreamingControl::SetNewestFirst(false);
        window->ApplyDisplayOrder();
        QVERIFY2(tableView->alternatingRowColors(), "default bottom-tail mode should keep alternating row colours on");

        // Newest-first flips the toggle off — see the comment in
        // `ApplyDisplayOrder` for the rationale.
        StreamingControl::SetNewestFirst(true);
        window->ApplyDisplayOrder();
        QVERIFY2(
            !tableView->alternatingRowColors(),
            "newest-first mode should disable alternating row colours to avoid the "
            "row-parity flicker on every incoming batch"
        );

        // Toggling back restores the reading aid (no-op for users who
        // never enabled newest-first, but covers the "I tried it,
        // didn't like it, switched back" path).
        StreamingControl::SetNewestFirst(false);
        window->ApplyDisplayOrder();
        QVERIFY2(
            tableView->alternatingRowColors(), "switching newest-first off should re-enable alternating row colours"
        );

        model->EndStreaming(false);
    }

    // Static-mode parallel of `testAlternatingRowColoursDisabledInNewestFirstMode`:
    // when the active session is static the apply path must read the
    // **static** preference (`StreamingControl::IsStaticNewestFirst`),
    // not the stream-mode one. Toggling only the stream-mode flag must
    // be a no-op while a static session is active.
    void testStaticNewestFirstReversesProxyOrder()
    {
        RowOrderProxyModel *rowOrderProxy = window->findChild<RowOrderProxyModel *>();
        LogTableView *tableView = window->findChild<LogTableView *>();
        LogModel *model = window->findChild<LogModel *>();
        QVERIFY(rowOrderProxy != nullptr);
        QVERIFY(tableView != nullptr);
        QVERIFY(model != nullptr);

        const bool originalStream = StreamingControl::IsNewestFirst();
        const bool originalStatic = StreamingControl::IsStaticNewestFirst();
        auto restore = qScopeGuard([this, originalStream, originalStatic]() {
            StreamingControl::SetNewestFirst(originalStream);
            StreamingControl::SetStaticNewestFirst(originalStatic);
            window->SetSessionModeForTest(MainWindow::TestSessionMode::Idle);
        });

        BeginSyntheticStaticSession(*model);
        window->SetSessionModeForTest(MainWindow::TestSessionMode::Static);

        // Stream-mode flag has no effect on a static session: with the
        // stream flag ON and the static flag OFF, the proxy must stay
        // in the identity orientation.
        StreamingControl::SetNewestFirst(true);
        StreamingControl::SetStaticNewestFirst(false);
        window->ApplyDisplayOrder();
        QVERIFY2(!rowOrderProxy->IsReversed(), "static session must ignore the stream-mode newest-first flag");
        QVERIFY(tableView->alternatingRowColors());

        // Flipping the static-mode flag drives the same proxy reversal
        // as the stream-mode flag does for live-tail sessions.
        StreamingControl::SetStaticNewestFirst(true);
        window->ApplyDisplayOrder();
        QVERIFY(rowOrderProxy->IsReversed());
        QVERIFY(!tableView->alternatingRowColors());

        StreamingControl::SetStaticNewestFirst(false);
        window->ApplyDisplayOrder();
        QVERIFY(!rowOrderProxy->IsReversed());
        QVERIFY(tableView->alternatingRowColors());

        model->EndStreaming(false);
    }

    // Regression for the user-reported "static file mode keeps
    // following the newest data" bug: while a large static file is
    // being parsed, every batch fires `LogModel::lineCountChanged`,
    // which in turn called `ScrollToNewestRowIfFollowing()` whenever
    // `mModel->IsStreamingActive()` -- a flag that's true for *both*
    // live-tail and static sessions. With `actionFollowTail` defaulting
    // to checked at startup (and on every `streamingFinished` reset),
    // a static session would silently chase the newest parsed row and
    // yank the viewport away from wherever the user had scrolled.
    //
    // The fix narrows the gate from `IsStreamingActive()` to
    // `IsLiveTailSession()` (in both `lineCountChanged` and
    // `userScrolledToTail`), and adds a defensive early-return inside
    // `ScrollToNewestRowIfFollowing()` itself so any future caller
    // of the method honours the same contract.
    //
    // The test simulates a static-mode batch arrival after the user
    // has scrolled away from the bottom and asserts the scrollbar
    // value does not change.
    void testStaticSessionDoesNotFollowNewestRows()
    {
        LogTableView *tableView = window->findChild<LogTableView *>();
        LogModel *model = window->findChild<LogModel *>();
        QAction *followAction = FindActionByObjectName(window, QStringLiteral("actionFollowTail"));
        QVERIFY(tableView != nullptr);
        QVERIFY(model != nullptr);
        QVERIFY(followAction != nullptr);

        auto restore = qScopeGuard([this]() { window->SetSessionModeForTest(MainWindow::TestSessionMode::Idle); });

        // Static session, with `actionFollowTail` left in its startup
        // baseline (checked). Production users never see this toggle
        // in static mode (the toolbar is hidden, the action is
        // disabled), but the *value* persists across sessions and is
        // what the buggy auto-scroll path was reading.
        loglib::StreamLineSource &streamSource = BeginSyntheticStaticSession(*model);
        window->SetSessionModeForTest(MainWindow::TestSessionMode::Static);
        QVERIFY(followAction->isChecked());

        QtStreamingLogSink *sink = model->Sink();
        QVERIFY(sink != nullptr);
        loglib::KeyIndex &keys = sink->Keys();
        const loglib::KeyId valueKey = keys.GetOrInsert(std::string("value"));

        // Force a known viewport size so the seeded rows actually
        // produce a scrollable range.
        tableView->resize(400, 200);
        tableView->show();
        QCoreApplication::processEvents();

        sink->OnBatch(MakeSyntheticBatch(streamSource, keys, valueKey, 1, 200, /*declareNewKey=*/true));
        QCoreApplication::processEvents();
        QCOMPARE(model->rowCount(), 200);

        QScrollBar *vbar = tableView->verticalScrollBar();
        QVERIFY(vbar != nullptr);
        if (vbar->maximum() == 0)
        {
            QSKIP("offscreen layout did not produce a scrollable viewport for the seeded rows");
        }

        // Park the user mid-file. A live-tail session would re-yank
        // the viewport to the bottom on the next batch (see
        // `testFollowNewestDisengagesOnScrollbarAction` for the
        // production scroll-away behaviour); a static session must
        // leave the value alone.
        const int parkedValue = vbar->maximum() / 2;
        vbar->setValue(parkedValue);
        QCoreApplication::processEvents();
        QCOMPARE(vbar->value(), parkedValue);

        // Push another batch: this fires `lineCountChanged`, which
        // pre-fix would have called `ScrollToNewestRowIfFollowing`
        // and snapped the viewport to the bottom.
        sink->OnBatch(MakeSyntheticBatch(streamSource, keys, valueKey, 201, 200, /*declareNewKey=*/false));
        QCoreApplication::processEvents();
        QCOMPARE(model->rowCount(), 400);

        // Post-fix: the scrollbar's range can grow because rows were
        // appended, but the *value* must not have been programmatic-
        // ally snapped to the new maximum. Allow at most a small
        // delta from any layout / viewport recalc that genuinely
        // affected the parked row's pixel position.
        const int valueAfter = vbar->value();
        QVERIFY2(
            qAbs(valueAfter - parkedValue) <= 4,
            qPrintable(QStringLiteral("static-mode batch must not move the scrollbar; "
                                      "parked=%1 after=%2 max=%3")
                           .arg(parkedValue)
                           .arg(valueAfter)
                           .arg(vbar->maximum()))
        );
        QVERIFY2(
            valueAfter < vbar->maximum(),
            qPrintable(QStringLiteral("static-mode batch must not snap the scrollbar to the bottom; "
                                      "after=%1 max=%2")
                           .arg(valueAfter)
                           .arg(vbar->maximum()))
        );

        model->EndStreaming(false);
    }

    // Companion to `testStaticSessionDoesNotFollowNewestRows`: the
    // user-scroll-to-tail signal must not silently re-arm
    // `actionFollowTail` while a static session is active. Pre-fix
    // the slot re-engaged the toggle whenever
    // `mModel->IsStreamingActive()`, which is true for static
    // sessions too -- so a user who scrolled to the bottom of a
    // partially-parsed static file would have the next incoming
    // batch yank the viewport away.
    void testStaticSessionDoesNotReArmFollowOnScrollToTail()
    {
        LogTableView *tableView = window->findChild<LogTableView *>();
        LogModel *model = window->findChild<LogModel *>();
        QAction *followAction = FindActionByObjectName(window, QStringLiteral("actionFollowTail"));
        QVERIFY(tableView != nullptr);
        QVERIFY(model != nullptr);
        QVERIFY(followAction != nullptr);

        auto restore = qScopeGuard([this]() { window->SetSessionModeForTest(MainWindow::TestSessionMode::Idle); });

        BeginSyntheticStaticSession(*model);
        window->SetSessionModeForTest(MainWindow::TestSessionMode::Static);

        // Force the action *unchecked* so the re-arm path has a
        // visible state transition to attempt.
        followAction->setChecked(false);
        QVERIFY(!followAction->isChecked());

        QSignalSpy toSpy(tableView, &LogTableView::userScrolledToTail);
        QVERIFY(toSpy.isValid());

        // Synthesise the same `userScrolledToTail` emission the
        // production path produces when the user drags the scrollbar
        // to the bottom edge. Going through `Q_EMIT` here is the
        // narrowest possible exercise of the slot we just narrowed.
        Q_EMIT tableView->userScrolledToTail();
        QCoreApplication::processEvents();

        QCOMPARE(toSpy.count(), 1);
        QVERIFY2(!followAction->isChecked(), "scroll-to-tail in static mode must not re-arm the Follow newest toggle");

        model->EndStreaming(false);
    }

    // Mode-transition guard: with both flags ON, switching from a
    // static session to a stream session must keep the proxy reversed
    // because both modes' preferences agree. Then with only the
    // *static* flag ON, the proxy must reverse on a static session and
    // de-reverse on a stream session, proving the mode dispatch in
    // `ApplyDisplayOrder` actually picks per-mode (and isn't reading
    // either flag's value unconditionally).
    void testNewestFirstFollowsSessionModeOnTransition()
    {
        RowOrderProxyModel *rowOrderProxy = window->findChild<RowOrderProxyModel *>();
        LogModel *model = window->findChild<LogModel *>();
        QVERIFY(rowOrderProxy != nullptr);
        QVERIFY(model != nullptr);

        const bool originalStream = StreamingControl::IsNewestFirst();
        const bool originalStatic = StreamingControl::IsStaticNewestFirst();
        auto restore = qScopeGuard([this, originalStream, originalStatic]() {
            StreamingControl::SetNewestFirst(originalStream);
            StreamingControl::SetStaticNewestFirst(originalStatic);
            window->SetSessionModeForTest(MainWindow::TestSessionMode::Idle);
        });

        // Both ON: every session orientation should land reversed.
        StreamingControl::SetNewestFirst(true);
        StreamingControl::SetStaticNewestFirst(true);

        BeginSyntheticStaticSession(*model);
        window->SetSessionModeForTest(MainWindow::TestSessionMode::Static);
        window->ApplyDisplayOrder();
        QVERIFY(rowOrderProxy->IsReversed());

        // End the static session and start a stream one; the proxy
        // must stay reversed because the stream-mode flag is also ON.
        model->EndStreaming(false);
        static_cast<void>(BeginSyntheticStreamSession(*model));
        window->SetSessionModeForTest(MainWindow::TestSessionMode::LiveTail);
        window->ApplyDisplayOrder();
        QVERIFY(rowOrderProxy->IsReversed());

        // Asymmetric: only the static-mode flag is ON. The proxy must
        // de-reverse for the stream session (still active) and only
        // re-reverse once we transition back to a static session.
        StreamingControl::SetNewestFirst(false);
        window->ApplyDisplayOrder();
        QVERIFY2(
            !rowOrderProxy->IsReversed(), "stream session must follow the stream-mode flag, not the static-mode one"
        );

        model->EndStreaming(false);
        BeginSyntheticStaticSession(*model);
        window->SetSessionModeForTest(MainWindow::TestSessionMode::Static);
        window->ApplyDisplayOrder();
        QVERIFY2(
            rowOrderProxy->IsReversed(),
            "static session must follow the static-mode flag even when the stream-mode flag is OFF"
        );

        model->EndStreaming(false);
    }

    // Back-pressure: when the GUI thread cannot keep up, the worker
    // posting batches via `OnBatch` must block on the bounded queue
    // once it fills. This test fills the queue without spinning the
    // event loop, samples that the worker is parked on the (cap+1)-th
    // call, then drains via `processEvents` and confirms every batch
    // eventually lands.
    void testBoundedQueueBlocksWorkerWhenGuiFallsBehind()
    {
        constexpr std::size_t CAPACITY = 4;
        constexpr int TOTAL_BATCHES = static_cast<int>(CAPACITY) + 5;
        LogModel model(nullptr, CAPACITY);

        loglib::StreamLineSource &streamSource = BeginSyntheticStreamSession(model);
        QtStreamingLogSink *sink = model.Sink();
        QVERIFY(sink != nullptr);
        QCOMPARE(sink->PendingCapacity(), CAPACITY);

        loglib::KeyIndex &keys = sink->Keys();
        const loglib::KeyId valueKey = keys.GetOrInsert(std::string("value"));

        std::atomic<int> enqueuedCount{0};
        std::thread worker([&]() {
            for (int i = 0; i < TOTAL_BATCHES; ++i)
            {
                sink->OnBatch(MakeSyntheticBatch(
                    streamSource, keys, valueKey, static_cast<size_t>(i) + 1, 1, /*declareNewKey=*/i == 0
                ));
                enqueuedCount.fetch_add(1, std::memory_order_release);
            }
        });

        // Wait for the worker to fill the queue. We deliberately do
        // NOT spin the event loop here -- the drain lambda must stay
        // queued so the worker has nowhere to push the (cap+1)-th batch.
        const auto fillDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
        while (enqueuedCount.load(std::memory_order_acquire) < static_cast<int>(CAPACITY))
        {
            QVERIFY(std::chrono::steady_clock::now() < fillDeadline);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        // Producer must remain parked: count stays at cap for a
        // sustained sample window.
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        QCOMPARE(enqueuedCount.load(std::memory_order_acquire), static_cast<int>(CAPACITY));

        // Now spin the event loop so the drain lambda runs, pulls
        // batches out, wakes the worker, and the rest go through.
        const auto drainDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (enqueuedCount.load(std::memory_order_acquire) < TOTAL_BATCHES)
        {
            QVERIFY(std::chrono::steady_clock::now() < drainDeadline);
            QCoreApplication::processEvents();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        worker.join();

        // Final drain so any trailing posted lambda runs.
        QCoreApplication::processEvents();
        QCOMPARE(model.rowCount(), TOTAL_BATCHES);

        model.EndStreaming(false);
    }

    // Stop integration: when the worker is parked inside the bounded
    // queue and the GUI calls `Reset`, `RequestStop -> NotifyStop` must
    // wake the worker immediately (no polling). Without the explicit
    // `NotifyStop` hook the worker would deadlock against
    // `mStreamingWatcher->waitForFinished()`.
    void testStopWhileWorkerBlockedOnBoundedQueue()
    {
        constexpr std::size_t CAPACITY = 1;
        LogModel model(nullptr, CAPACITY);

        loglib::StreamLineSource &streamSource = BeginSyntheticStreamSession(model);
        QtStreamingLogSink *sink = model.Sink();
        QVERIFY(sink != nullptr);

        loglib::KeyIndex &keys = sink->Keys();
        const loglib::KeyId valueKey = keys.GetOrInsert(std::string("value"));

        // Pre-fill the queue so the next enqueue blocks.
        sink->OnBatch(MakeSyntheticBatch(streamSource, keys, valueKey, 1, 1, /*declareNewKey=*/true));

        std::atomic<bool> entered{false};
        std::atomic<bool> finished{false};
        std::thread worker([&]() {
            entered.store(true, std::memory_order_release);
            sink->OnBatch(MakeSyntheticBatch(streamSource, keys, valueKey, 2, 1, /*declareNewKey=*/false));
            finished.store(true, std::memory_order_release);
        });

        const auto enterDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
        while (!entered.load(std::memory_order_acquire))
        {
            QVERIFY(std::chrono::steady_clock::now() < enterDeadline);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        // Give the worker a moment to actually park inside the queue's
        // condition variable.
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        QVERIFY(!finished.load(std::memory_order_acquire));

        // Reset must wake the parked worker via NotifyStop and return
        // well inside this deadline. Without NotifyStop the
        // `waitForFinished` step would still complete (no QFuture is
        // associated with the synthetic worker), but the worker
        // thread's `WaitEnqueue` would never return -- our `worker.join()`
        // below would deadlock.
        const auto resetStart = std::chrono::steady_clock::now();
        model.Reset();
        const auto resetElapsed = std::chrono::steady_clock::now() - resetStart;
        QVERIFY2(
            resetElapsed < std::chrono::seconds(1),
            qPrintable(QStringLiteral("Reset took %1 ms; expected < 1000 ms")
                           .arg(std::chrono::duration_cast<std::chrono::milliseconds>(resetElapsed).count()))
        );

        worker.join();
        QVERIFY(finished.load(std::memory_order_acquire));
    }

    // Throughput / FIFO: drive many small batches through the bounded
    // queue with a small capacity and a draining GUI; every batch must
    // arrive in order. Guards against drain-side dropouts and against
    // the lazy `mDrainScheduled` flag missing a re-arm window.
    void testNoBatchLossUnderBackPressure()
    {
        constexpr std::size_t CAPACITY = 8;
        constexpr int TOTAL_BATCHES = 200;
        LogModel model(nullptr, CAPACITY);

        loglib::StreamLineSource &streamSource = BeginSyntheticStreamSession(model);
        QtStreamingLogSink *sink = model.Sink();
        QVERIFY(sink != nullptr);

        loglib::KeyIndex &keys = sink->Keys();
        const loglib::KeyId valueKey = keys.GetOrInsert(std::string("value"));

        std::atomic<bool> producerDone{false};
        std::thread worker([&]() {
            for (int i = 0; i < TOTAL_BATCHES; ++i)
            {
                sink->OnBatch(MakeSyntheticBatch(
                    streamSource, keys, valueKey, static_cast<size_t>(i) + 1, 1, /*declareNewKey=*/i == 0
                ));
            }
            producerDone.store(true, std::memory_order_release);
        });

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (model.rowCount() < TOTAL_BATCHES)
        {
            QVERIFY(std::chrono::steady_clock::now() < deadline);
            QCoreApplication::processEvents();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        worker.join();
        QVERIFY(producerDone.load(std::memory_order_acquire));

        QCOMPARE(model.rowCount(), TOTAL_BATCHES);
        // FIFO: with newest-first OFF (default) the source-row at
        // position K must be the K-th appended row.
        for (int row = 0; row < TOTAL_BATCHES; ++row)
        {
            const QVariant insertionOrder = model.data(model.index(row, 0), LogModelItemDataRole::InsertionOrderRole);
            QCOMPARE(insertionOrder.toInt(), row);
        }

        model.EndStreaming(false);
    }

private:
    // Shared helper for the ISO/timestamp fixtures. Outside `private slots:`
    // so moc doesn't expose it as a test method.
    void AssertTimestampFixture(StreamingRun &run, qint64 expectedFirstUtcUs, int rowCount)
    {
        QCOMPARE(run.finishedCount, 1);
        QCOMPARE(run.cancelled, false);
        QCOMPARE(run.model->rowCount(), rowCount);
        QVERIFY(run.model->StreamingErrors().empty());

        const int tsCol = ColumnByHeader(*run.model, QStringLiteral("timestamp"));
        QVERIFY2(tsCol >= 0, "auto-promoted timestamp column must exist");

        const auto &columns = run.model->Configuration().columns;
        QVERIFY(static_cast<size_t>(tsCol) < columns.size());
        QCOMPARE(columns[static_cast<size_t>(tsCol)].type, loglib::LogConfiguration::Type::time);

        // `FormatLogValue` rounds to milliseconds before formatting, so the
        // date library always emits a `.fff` fractional suffix even with the
        // `%F %H:%M:%S` printFormat — accept the optional fraction here.
        const QRegularExpression dateTimeRe(QStringLiteral("^\\d{4}-\\d{2}-\\d{2} \\d{2}:\\d{2}:\\d{2}(\\.\\d{3})?$"));
        for (int row = 0; row < rowCount; ++row)
        {
            const QVariant sortValue = run.model->data(run.model->index(row, tsCol), LogModelItemDataRole::SortRole);
            QVERIFY2(sortValue.isValid(), qPrintable(QStringLiteral("row %1 timestamp must be promoted").arg(row)));
            QCOMPARE(sortValue.typeId(), static_cast<int>(QMetaType::LongLong));

            const QString display = run.model->data(run.model->index(row, tsCol), Qt::DisplayRole).toString();
            QVERIFY2(
                dateTimeRe.match(display).hasMatch(),
                qPrintable(QStringLiteral("row %1 display '%2' must match %F %H:%M:%S").arg(row).arg(display))
            );
        }

        const qint64 firstUs = run.model->data(run.model->index(0, tsCol), LogModelItemDataRole::SortRole).toLongLong();
        QCOMPARE(firstUs, expectedFirstUtcUs);
    }

    MainWindow *window;
};

QTEST_MAIN(MainWindowTest)
#include "main_window_test.moc"
