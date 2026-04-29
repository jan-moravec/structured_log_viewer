#include "log_filter_model.hpp"
#include "log_model.hpp"
#include "log_table_view.hpp"
#include "main_window.hpp"
#include "qt_streaming_log_sink.hpp"

#include <loglib/internal/parser_options.hpp>
#include <loglib/json_parser.hpp>
#include <loglib/log_configuration.hpp>
#include <loglib/log_file.hpp>
#include <loglib/parser_options.hpp>
#include <loglib/stop_token.hpp>

#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QVariant>
#include <QtTest/QtTest>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
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

// Returns a deterministic JSONL fixture exercising every public LogValue
// alternative the streaming path can produce, plus a few keys that appear
// in some lines but not others (so the column-extension contract is tested
// end to end). Keys are intentionally ordered so the *insertion order*
// (streaming AppendKeys) and the *sorted order* (legacy Update) match —
// otherwise the per-cell comparison would have to be column-name keyed
// rather than column-index keyed.
QStringList MakeParityFixture()
{
    return QStringList{
        // line 1: introduces a, b, c (alphabetic — matches sorted-order from
        // legacy Update and insertion-order from streaming AppendKeys).
        QStringLiteral(R"({"a": "alpha", "b": 1, "c": 3.14})"),
        QStringLiteral(R"({"a": "beta",  "b": 2, "c": 2.71})"),
        // line 3: introduces d (still alphabetic-after-existing).
        QStringLiteral(R"({"a": "gamma", "b": 3, "c": 1.41, "d": true})"),
        QStringLiteral(R"({"a": "delta", "b": 4, "c": 0.0,  "d": false})"),
        QStringLiteral(R"({"a": "eps",   "b": 5})"),
    };
}

// Captures (rowCount, columnCount, header→column map, per-(row, header)
// display value) so the legacy-vs-streaming comparison stays robust against
// configuration column ordering decisions. Using the display role (the same
// one the table view actually paints) means we are validating exactly what
// the user sees, not internal storage details.
struct ModelSnapshot
{
    int rowCount = 0;
    std::vector<std::string> headers;
    std::vector<std::vector<std::string>> cellsByHeaderIndex;
};

ModelSnapshot Snapshot(LogModel &model)
{
    ModelSnapshot snap;
    snap.rowCount = model.rowCount();
    const int columnCount = model.columnCount();
    snap.headers.reserve(static_cast<size_t>(columnCount));
    for (int col = 0; col < columnCount; ++col)
    {
        snap.headers.push_back(model.headerData(col, Qt::Horizontal, Qt::DisplayRole).toString().toStdString());
    }

    snap.cellsByHeaderIndex.assign(static_cast<size_t>(snap.rowCount), {});
    for (int row = 0; row < snap.rowCount; ++row)
    {
        snap.cellsByHeaderIndex[static_cast<size_t>(row)].reserve(static_cast<size_t>(columnCount));
        for (int col = 0; col < columnCount; ++col)
        {
            snap.cellsByHeaderIndex[static_cast<size_t>(row)].push_back(
                model.data(model.index(row, col), Qt::DisplayRole).toString().toStdString()
            );
        }
    }
    return snap;
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
    int finishedCount = 0;
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
    const loglib::StopToken stopToken = run.model->BeginStreaming(std::move(file));

    auto &files = run.model->Table().Data().Files();
    if (!files.empty())
    {
        loglib::LogFile *parseFile = files.front().get();

        loglib::ParserOptions options;
        options.stopToken = stopToken;
        loglib::internal::AdvancedParserOptions advanced;
        advanced.threads = 1;

        loglib::JsonParser parser;
        parser.ParseStreaming(*parseFile, *run.model->Sink(), options, advanced);
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

    // Drive the same fixture through both the synchronous `LogParser::Parse`
    // path (`LogModel::AddData`) and the streaming pipeline
    // (`LogModel::BeginStreaming` + `ParseStreaming` + `EndStreaming`) and
    // assert byte-equivalent display output. Pinned to `threads=1` so the
    // order in which workers race on `KeyIndex::GetOrInsert` can't perturb
    // the streaming-side `AppendKeys` insertion order — that would make the
    // legacy (sorted) vs. streaming (insertion) column ordering diverge for
    // reasons unrelated to per-cell parsing parity.
    void testStreamingParityVsLegacy()
    {
        const QStringList fixtureLines = MakeParityFixture();
        TempJsonFile fixture(fixtureLines);

        // ---- Legacy path: synchronous parse, AddData. ----
        loglib::JsonParser legacyParser;
        loglib::ParseResult legacyResult = legacyParser.Parse(fixture.Path().toStdString());
        QVERIFY2(legacyResult.errors.empty(), "legacy parse must produce no errors on the parity fixture");

        LogModel legacyModel;
        legacyModel.AddData(std::move(legacyResult.data));
        const ModelSnapshot legacySnap = Snapshot(legacyModel);
        QCOMPARE(legacySnap.rowCount, fixtureLines.size());

        // ---- Streaming path: BeginStreaming + ParseStreaming + EndStreaming
        // via the QtStreamingLogSink GUI bridge. ----
        LogModel streamingModel;
        QSignalSpy finishedSpy(&streamingModel, &LogModel::streamingFinished);
        QVERIFY(finishedSpy.isValid());

        // BeginStreaming installs the LogFile on the model. The parser must
        // borrow the *same* LogFile (mirroring `MainWindow::OpenJsonStreaming`)
        // so the std::string_view-typed LogValues Stage B emits stay live for
        // the lifetime of the model — opening a second mmap on the parser
        // side would dangle them as soon as the parser thread ran out.
        auto file = std::make_unique<loglib::LogFile>(fixture.Path().toStdString());
        const loglib::StopToken stopToken = streamingModel.BeginStreaming(std::move(file));

        QVERIFY(!streamingModel.Table().Data().Files().empty());
        loglib::LogFile *parseFile = streamingModel.Table().Data().Files().front().get();
        QVERIFY(parseFile != nullptr);

        // Stage B parallelism is pinned to 1 so the canonical KeyIndex sees
        // keys in file order (a, b, c, d). The legacy LogConfigurationManager::
        // Update walks SortedKeys() (alphabetic) — both produce the same
        // column order on this fixture only because we authored it that way.
        loglib::ParserOptions options;
        options.stopToken = stopToken;
        loglib::internal::AdvancedParserOptions advanced;
        advanced.threads = 1;

        QtStreamingLogSink *sink = streamingModel.Sink();
        QVERIFY(sink != nullptr);

        // Run the parse synchronously on the test thread. Even though the
        // production GUI runs ParseStreaming on a worker thread (so OnBatch /
        // OnFinished cross thread boundaries), the QtStreamingLogSink always
        // posts its sink callbacks back through QMetaObject::invokeMethod /
        // Qt::QueuedConnection — meaning the model-side semantics (events
        // queued onto the GUI event loop, draining on the next tick) are
        // identical regardless of which thread the parser ran on. Driving the
        // parse on the test thread keeps the test free of QtConcurrent /
        // std::thread plumbing while still exercising the queued-connection
        // delivery path that production code relies on.
        loglib::JsonParser parser;
        parser.ParseStreaming(*parseFile, *sink, options, advanced);

        // Spin the event loop so the queued OnBatch / OnFinished invocations
        // posted by QtStreamingLogSink during the parse above are drained
        // before the assertions run. QSignalSpy::wait blocks until the signal
        // is emitted (i.e. until LogModel::EndStreaming runs on the GUI side).
        const bool finished = finishedSpy.count() > 0 || finishedSpy.wait(5000);
        QVERIFY2(finished, "streamingFinished must arrive within the timeout");
        QCOMPARE(finishedSpy.count(), 1);
        const QList<QVariant> finishedArgs = finishedSpy.takeFirst();
        QCOMPARE(finishedArgs.value(0).value<StreamingResult>(), StreamingResult::Success);

        QVERIFY2(
            streamingModel.StreamingErrors().empty(), "streaming parse must produce no errors on the parity fixture"
        );

        const ModelSnapshot streamingSnap = Snapshot(streamingModel);

        // ---- Parity assertions. ----
        QCOMPARE(streamingSnap.rowCount, legacySnap.rowCount);

        // Compare headers as a set (the configuration-derivation paths are
        // independent: legacy Update walks SortedKeys, streaming AppendKeys
        // walks the per-batch newKeys slice). Then compare per-cell using
        // header name as the join key, so a column-order skew (which would
        // be a configuration-side bug, not a parsing-side one) is reported
        // distinctly from an actual data mismatch.
        std::vector<std::string> legacyHeadersSorted = legacySnap.headers;
        std::vector<std::string> streamingHeadersSorted = streamingSnap.headers;
        std::sort(legacyHeadersSorted.begin(), legacyHeadersSorted.end());
        std::sort(streamingHeadersSorted.begin(), streamingHeadersSorted.end());
        QCOMPARE(streamingHeadersSorted, legacyHeadersSorted);

        std::map<std::string, int> legacyHeaderIndex;
        for (size_t i = 0; i < legacySnap.headers.size(); ++i)
        {
            legacyHeaderIndex[legacySnap.headers[i]] = static_cast<int>(i);
        }
        std::map<std::string, int> streamingHeaderIndex;
        for (size_t i = 0; i < streamingSnap.headers.size(); ++i)
        {
            streamingHeaderIndex[streamingSnap.headers[i]] = static_cast<int>(i);
        }

        for (int row = 0; row < legacySnap.rowCount; ++row)
        {
            for (const auto &[header, legacyCol] : legacyHeaderIndex)
            {
                const auto it = streamingHeaderIndex.find(header);
                QVERIFY2(
                    it != streamingHeaderIndex.end(),
                    qPrintable(QStringLiteral("streaming model missing header '%1'").arg(QString::fromStdString(header))
                    )
                );
                const int streamingCol = it->second;
                const std::string legacyCell =
                    legacySnap.cellsByHeaderIndex[static_cast<size_t>(row)][static_cast<size_t>(legacyCol)];
                const std::string streamingCell =
                    streamingSnap.cellsByHeaderIndex[static_cast<size_t>(row)][static_cast<size_t>(streamingCol)];
                QVERIFY2(
                    legacyCell == streamingCell,
                    qPrintable(QStringLiteral("row=%1 header='%2' legacy='%3' streaming='%4'")
                                   .arg(row)
                                   .arg(QString::fromStdString(header))
                                   .arg(QString::fromStdString(legacyCell))
                                   .arg(QString::fromStdString(streamingCell)))
                );
            }
        }
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
        QCOMPARE(sortVal(0, colUint).toULongLong(), quint64(18446744073709551610ULL));
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

    // Regression for the Qt model contract violation in
    // `LogModel::AppendBatch`: the original code mutated `mLogTable` *before*
    // calling `beginInsertRows` / `beginInsertColumns`, so any proxy listening
    // on `rowsAboutToBeInserted` / `columnsAboutToBeInserted` (notably
    // `QSortFilterProxyModel`) saw the post-mutation `rowCount()` /
    // `columnCount()` from the source. Qt's contract is begin-before-mutate.
    // The fix splits `LogTable::AppendBatch` into a non-mutating
    // `PreviewAppend` + the original `AppendBatch`, with `LogModel` calling
    // begin → commit → end in the right order. Hook a slot to the proxy's
    // about-to-be-inserted signals and snapshot the *source* model's count
    // there: with the bug it equals the post-mutation count; with the fix it
    // equals the pre-mutation count.
    void testAppendBatchFiresBeginsBeforeMutation()
    {
        const QStringList fixtureLines = MakeParityFixture();
        TempJsonFile fixture(fixtureLines);

        LogModel model;
        LogFilterModel proxy;
        proxy.setSourceModel(&model);

        // Hook the **source** model's `rowsAboutToBeInserted` /
        // `columnsAboutToBeInserted`, not the proxy's: `QSortFilterProxyModel`
        // delays its own `*AboutToBeInserted` until after the source has
        // mutated (it needs the source's post-mutation rows to evaluate the
        // filter), so a snapshot taken from the *proxy*'s slot would always
        // see the post-mutation source count regardless of which order
        // `LogModel::AppendBatch` fires `begin*` vs. the underlying
        // `LogTable::AppendBatch` mutation. Listening on the source's signal
        // makes the test directly assert Qt's begin-before-mutate contract.
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
        const loglib::StopToken stopToken = model.BeginStreaming(std::move(file));

        QVERIFY(!model.Table().Data().Files().empty());
        loglib::LogFile *parseFile = model.Table().Data().Files().front().get();
        QVERIFY(parseFile != nullptr);

        loglib::ParserOptions options;
        options.stopToken = stopToken;
        loglib::internal::AdvancedParserOptions advanced;
        advanced.threads = 1;

        loglib::JsonParser parser;
        parser.ParseStreaming(*parseFile, *model.Sink(), options, advanced);

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

    // Regression: `LogModel::Clear()` used to derive `wasStreaming` from
    // `mStreamingWatcher->isRunning()`, which races with the queued
    // `OnFinished` lambda — the worker function returns (and the watcher
    // flips to "not running") *before* the lambda reaches the GUI thread,
    // so a `Clear()` in that window saw `wasStreaming == false`, the
    // queued `OnFinished` was then dropped by the sink's generation-mismatch
    // check after `RequestStop()` bumped the generation, and no
    // `streamingFinished` ever arrived — leaving the configuration menus
    // permanently disabled until the next streaming parse. Replacing the
    // source with a GUI-thread-only flag (`mStreamingActive`) makes this
    // case race-free; this test covers it deterministically by calling
    // `BeginStreaming` followed immediately by `Clear()` (no parser ever
    // runs, so we cannot rely on `OnFinished` reaching the GUI).
    void testClearAfterBeginStreamingEmitsCompensatingFinished()
    {
        // Use a tiny in-memory file so BeginStreaming has a valid LogFile
        // to install on the model.
        TempJsonFile fixture(QStringList{QStringLiteral(R"({"a": 1})")});

        LogModel model;
        QSignalSpy finishedSpy(&model, &LogModel::streamingFinished);
        QVERIFY(finishedSpy.isValid());

        auto file = std::make_unique<loglib::LogFile>(fixture.Path().toStdString());
        static_cast<void>(model.BeginStreaming(std::move(file)));

        // No parser is started, so `EndStreaming` will never fire on its own.
        // `Clear()` must emit `streamingFinished(true)` itself based on the
        // GUI-thread flag set by `BeginStreaming`.
        model.Clear();

        QCOMPARE(finishedSpy.count(), 1);
        QCOMPARE(finishedSpy.takeFirst().value(0).value<StreamingResult>(), StreamingResult::Cancelled);

        // A second `Clear()` after the first one already cleared the flag
        // must not emit a spurious `streamingFinished` (otherwise repeated
        // user-driven file opens would emit duplicate signals).
        model.Clear();
        QCOMPARE(finishedSpy.count(), 0);
    }

    // Regression: `QtStreamingLogSink::RequestStop()` used to bump the sink
    // generation *before* `LogModel::Clear()` waited for the worker to
    // finish. The worker's drain-phase `OnBatch` / `OnFinished` then
    // captured the bumped generation (the stop_token's request_stop pairs
    // release-acquire with the worker's load), the queued GUI-thread
    // lambdas matched it on dispatch, and they ran `AppendBatch` /
    // `EndStreaming` *after* `Clear()` had already destroyed `mLogTable` —
    // a use-after-free on the dangling `LogFile*` carried inside every
    // batched `LogLine` (observable on the `CopyLine` role) plus a
    // spurious second `streamingFinished(true)`. Triggered by drag-and-
    // drop, multi-file open, or load-configuration while a streaming
    // parse is still running.
    //
    // The fix moves the bump to `DropPendingBatches()`, called by `Clear()`
    // *after* `waitForFinished()` returns. This test reproduces the race
    // deterministically with a manual worker thread that waits for the
    // stop_token to flip (i.e. `Clear()` has called `RequestStop()`),
    // emits one `OnBatch` referencing the planted `LogFile` plus an
    // `OnFinished(true)`, then completes a `QPromise` planted on the
    // model so `waitForFinished()` can unblock. With the fix both queued
    // lambdas observe a stale generation (because `DropPendingBatches`
    // bumped it past the worker's capture) and short-circuit; without
    // the fix the `OnBatch` lambda would push a row whose `LogLine`
    // points at the now-unmapped file, and the `OnFinished` lambda would
    // emit a second `streamingFinished` on top of `Clear`'s compensating
    // one.
    void testClearDuringStreamingDropsDrainPhaseBatch()
    {
        TempJsonFile fixture(QStringList{QStringLiteral(R"({"a": 1})")});

        LogModel model;
        QSignalSpy finishedSpy(&model, &LogModel::streamingFinished);
        QVERIFY(finishedSpy.isValid());

        auto file = std::make_unique<loglib::LogFile>(fixture.Path().toStdString());
        loglib::LogFile *filePtr = file.get();

        // The parseCallable runs on the `QtConcurrent::run` worker that
        // `BeginStreaming(file, parseCallable)` spawns internally. It mimics
        // the original `QPromise`-injected timing by parking on a
        // `std::condition_variable` until the test thread releases it,
        // which happens AFTER the worker has emitted its drain-phase
        // `OnBatch` + `OnFinished` (= the racy queued lambdas the fix is
        // meant to invalidate via the post-`waitForFinished` generation
        // bump in `LogModel::Clear`).
        std::mutex releaseMutex;
        std::condition_variable releaseCv;
        bool released = false;
        QtStreamingLogSink *sinkBeforeBegin = model.Sink();
        QVERIFY(sinkBeforeBegin != nullptr);

        const loglib::StopToken stop = model.BeginStreaming(
            std::move(file),
            [sinkBeforeBegin, filePtr, &releaseMutex, &releaseCv, &released](loglib::StopToken stopToken) {
                while (!stopToken.stop_requested())
                {
                    std::this_thread::yield();
                }

                loglib::KeyIndex &keys = sinkBeforeBegin->Keys();
                const loglib::KeyId keyId = keys.GetOrInsert("a");
                std::vector<std::pair<loglib::KeyId, loglib::LogValue>> values;
                values.emplace_back(keyId, loglib::LogValue(int64_t{1}));
                loglib::LogLine line(std::move(values), keys, loglib::LogFileReference(*filePtr, 1));
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

        // `Clear()` runs `RequestStop()` and then blocks on
        // `mStreamingWatcher->waitForFinished()`. The worker is parked on
        // `releaseCv`, so we have to release it from another thread for
        // `Clear()` to return. Spin a tiny releaser thread that fires the
        // condition variable a fraction after `Clear()` has had a chance
        // to call `RequestStop()`.
        std::thread releaser([&]() {
            // Wait for the worker to clear the stop_requested check + emit.
            // The condition variable is only signalled once the worker has
            // already run through `OnBatch` + `OnFinished`, mirroring the
            // ordering the QPromise-based test guaranteed.
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            {
                std::lock_guard lock(releaseMutex);
                released = true;
            }
            releaseCv.notify_all();
        });

        model.Clear();
        releaser.join();

        // Drain the OnBatch / OnFinished lambdas the worker posted via
        // QMetaObject::invokeMethod / Qt::QueuedConnection. With the fix
        // both observe a stale generation and short-circuit; without it
        // the OnBatch lambda would call AppendBatch on the cleared model
        // (rowCount 0 -> 1, plus a UAF on the now-destroyed `LogFile`
        // when CopyLine is queried) and the OnFinished lambda would emit
        // a duplicate streamingFinished.
        QCoreApplication::processEvents();

        QCOMPARE(finishedSpy.count(), 1);
        QCOMPARE(finishedSpy.takeFirst().value(0).value<StreamingResult>(), StreamingResult::Cancelled);
        QCOMPARE(model.rowCount(), 0);
    }

private:
    // Shared helper for the four ISO/timestamp fixtures: asserts that the
    // `"timestamp"` column is auto-promoted (Type::time) and every row's
    // DisplayRole matches the auto-promoted `%F %H:%M:%S` printFormat. Lives
    // outside `private slots:` so moc doesn't expose it as a test method.
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
            QCOMPARE(static_cast<int>(sortValue.typeId()), static_cast<int>(QMetaType::LongLong));

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
