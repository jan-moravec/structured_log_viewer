#include "log_filter_model.hpp"
#include "log_model.hpp"
#include "log_table_view.hpp"
#include "main_window.hpp"
#include "qt_streaming_log_sink.hpp"

#include <loglib/internal/parser_options.hpp>
#include <loglib/json_parser.hpp>
#include <loglib/key_index.hpp>
#include <loglib/log_configuration.hpp>
#include <loglib/log_file.hpp>
#include <loglib/log_value.hpp>
#include <loglib/parser_options.hpp>
#include <loglib/stop_token.hpp>
#include <loglib/stream_log_line.hpp>
#include <loglib/streaming_log_sink.hpp>
#include <loglib/tailing_file_source.hpp>

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
        // Touch the file so `TailingFileSource`'s open() succeeds. Pre-fill
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

    // Drive the same fixture through legacy (`AddData`) and streaming
    // (`BeginStreaming`/`ParseStreaming`/`EndStreaming`) paths and assert
    // byte-equivalent display output. `threads=1` keeps `AppendKeys`
    // insertion order deterministic.
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

        // Run the parse on the test thread; the sink still routes every
        // callback through QueuedConnection, so model-side semantics match
        // the production GUI's worker-thread path.
        loglib::JsonParser parser;
        parser.ParseStreaming(*parseFile, *sink, options, advanced);

        // Drain the queued OnBatch/OnFinished invocations.
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

    // Regression: `Clear()` must emit a compensating `streamingFinished` based
    // on a GUI-thread flag (`mStreamingActive`), not `isRunning()` — the
    // latter races with the queued `OnFinished` and would silently drop the
    // signal, leaving configuration menus disabled.
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

    // Regression: the sink-generation bump must happen in
    // `DropPendingBatches()` (called *after* `waitForFinished()`), not in
    // `RequestStop()`. Otherwise drain-phase queued lambdas pass the
    // mismatch check and run after `Clear()` has destroyed `mLogTable`
    // (use-after-free on dangling `LogFile*` + spurious second `streamingFinished`).
    void testClearDuringStreamingDropsDrainPhaseBatch()
    {
        TempJsonFile fixture(QStringList{QStringLiteral(R"({"a": 1})")});

        LogModel model;
        QSignalSpy finishedSpy(&model, &LogModel::streamingFinished);
        QVERIFY(finishedSpy.isValid());

        auto file = std::make_unique<loglib::LogFile>(fixture.Path().toStdString());
        loglib::LogFile *filePtr = file.get();

        // The parseCallable parks on a CV until the test thread releases it,
        // which happens AFTER the worker has emitted drain-phase
        // `OnBatch` + `OnFinished` (the racy queued lambdas the fix invalidates).
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

        // `Clear()` blocks on `waitForFinished()`; release the worker from a
        // separate thread so `Clear()` can return.
        std::thread releaser([&]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            {
                std::lock_guard lock(releaseMutex);
                released = true;
            }
            releaseCv.notify_all();
        });

        model.Clear();
        releaser.join();

        // Drain the queued lambdas; with the fix both observe a stale
        // generation and short-circuit.
        QCoreApplication::processEvents();

        QCOMPARE(finishedSpy.count(), 1);
        QCOMPARE(finishedSpy.takeFirst().value(0).value<StreamingResult>(), StreamingResult::Cancelled);
        QCOMPARE(model.rowCount(), 0);
    }

    // FIFO eviction at the retention cap (PRD 4.5 / 4.10.3): feed batches of
    // synthetic `StreamLogLine`s directly into `LogModel::AppendBatch`, asserting
    // that:
    //   - `RowCount()` never exceeds the cap once the cap is reached;
    //   - `beginRemoveRows`/`rowsRemoved` fire on the prefix as new lines arrive;
    //   - the surviving rows correspond to the *most recent* lines, not the
    //     oldest ones (FIFO drops oldest).
    void testRetentionCapFifoEviction()
    {
        LogModel model;
        // Fresh sink + null LogFile so the StreamLogLine path takes over —
        // mirrors what `BeginStreaming(unique_ptr<LogSource>)` does for a
        // non-mmap source.
        static_cast<void>(model.BeginStreaming(std::unique_ptr<loglib::LogFile>{}));
        model.SetRetentionCap(100);

        QSignalSpy rowsRemovedSpy(&model, &QAbstractItemModel::rowsRemoved);
        QVERIFY(rowsRemovedSpy.isValid());

        loglib::KeyIndex &keys = model.Sink()->Keys();
        const loglib::KeyId valueKey = keys.GetOrInsert(std::string("value"));
        Q_UNUSED(valueKey);

        const size_t batchSize = 50;
        const size_t totalLines = 500;
        for (size_t batchStart = 0; batchStart < totalLines; batchStart += batchSize)
        {
            loglib::StreamedBatch batch;
            batch.firstLineNumber = batchStart + 1;
            batch.streamLines.reserve(batchSize);
            // First batch must declare `value` as a new key so the column
            // count predictor in `LogTable::PreviewAppend` matches reality.
            if (batchStart == 0)
            {
                batch.newKeys.emplace_back(std::string("value"));
            }
            for (size_t i = 0; i < batchSize; ++i)
            {
                const size_t lineId = batchStart + i + 1;
                std::vector<std::pair<loglib::KeyId, loglib::LogValue>> values;
                values.emplace_back(valueKey, loglib::LogValue{static_cast<int64_t>(lineId)});
                batch.streamLines.emplace_back(
                    std::move(values), keys, loglib::StreamLineReference("synthetic", "synthetic line", lineId)
                );
            }
            model.AppendBatch(std::move(batch));
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

    // Giant-batch collapse (PRD 4.10.3.iii): a batch whose row count alone
    // exceeds the cap must collapse the head of the batch *before* it lands
    // in `LogTable`, so per-batch eviction stays O(cap) and the visible
    // model never breaches the cap.
    void testRetentionCapGiantBatchCollapse()
    {
        LogModel model;
        static_cast<void>(model.BeginStreaming(std::unique_ptr<loglib::LogFile>{}));
        model.SetRetentionCap(1000);

        loglib::KeyIndex &keys = model.Sink()->Keys();
        const loglib::KeyId valueKey = keys.GetOrInsert(std::string("value"));

        loglib::StreamedBatch batch;
        batch.firstLineNumber = 1;
        batch.newKeys.emplace_back(std::string("value"));
        batch.streamLines.reserve(2000);
        for (size_t i = 0; i < 2000; ++i)
        {
            std::vector<std::pair<loglib::KeyId, loglib::LogValue>> values;
            values.emplace_back(valueKey, loglib::LogValue{static_cast<int64_t>(i + 1)});
            batch.streamLines.emplace_back(
                std::move(values), keys, loglib::StreamLineReference("synthetic", "x", i + 1)
            );
        }
        model.AppendBatch(std::move(batch));

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

        static_cast<void>(model.BeginStreaming(std::unique_ptr<loglib::LogFile>{}));
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
            loglib::StreamedBatch batch;
            batch.firstLineNumber = static_cast<size_t>(b * linesPerBatch + 1);
            if (b == 0)
            {
                batch.newKeys.emplace_back(std::string("value"));
            }
            batch.streamLines.reserve(linesPerBatch);
            for (int i = 0; i < linesPerBatch; ++i)
            {
                const size_t lineId = static_cast<size_t>(b * linesPerBatch + i + 1);
                std::vector<std::pair<loglib::KeyId, loglib::LogValue>> values;
                values.emplace_back(valueKey, loglib::LogValue{static_cast<int64_t>(lineId)});
                batch.streamLines.emplace_back(
                    std::move(values), keys, loglib::StreamLineReference("synthetic", "x", lineId)
                );
            }
            // Direct call to OnBatch from this thread mirrors the worker.
            sink->OnBatch(std::move(batch));
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
        // Need a real `LogFile` because `LogFileReference` (and the model's
        // `LogTable::AppendBatch` path that consumes `lines`) hold pointers
        // into the file. The file's contents don't have to match what we
        // synthesise — we never call `LogLine::Data()` / `GetValue()` on
        // them through the table — but they must outlive the test.
        TempJsonFile fixture(QStringList{
            QStringLiteral(R"({"a": 1})"),
            QStringLiteral(R"({"a": 2})"),
            QStringLiteral(R"({"a": 3})"),
        });

        LogModel model;
        QSignalSpy lineCountSpy(&model, &LogModel::lineCountChanged);
        QVERIFY(lineCountSpy.isValid());

        auto file = std::make_unique<loglib::LogFile>(fixture.Path().toStdString());
        loglib::LogFile *filePtr = file.get();
        static_cast<void>(model.BeginStreaming(std::move(file)));

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
                batch.lines.emplace_back(std::move(values), keys, loglib::LogFileReference(*filePtr, lineNumber));
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

    // Pause + cap-shrink interaction (PRD 4.5.5.ii): while paused, lowering
    // the retention cap must trim the paused buffer to `cap - visible`
    // (preserving the visible rows). Verified via PausedLineCount().
    void testPauseCapShrinkTrimsPausedBuffer()
    {
        LogModel model;
        static_cast<void>(model.BeginStreaming(std::unique_ptr<loglib::LogFile>{}));
        QtStreamingLogSink *sink = model.Sink();

        // Seed the visible model with 100 rows by feeding a batch *before* pause.
        loglib::KeyIndex &keys = sink->Keys();
        const loglib::KeyId valueKey = keys.GetOrInsert(std::string("value"));

        auto makeBatch = [&keys, valueKey](size_t firstLineId, size_t count, bool declareNewKey) {
            loglib::StreamedBatch batch;
            batch.firstLineNumber = firstLineId;
            if (declareNewKey)
            {
                batch.newKeys.emplace_back(std::string("value"));
            }
            batch.streamLines.reserve(count);
            for (size_t i = 0; i < count; ++i)
            {
                const size_t lineId = firstLineId + i;
                std::vector<std::pair<loglib::KeyId, loglib::LogValue>> values;
                values.emplace_back(valueKey, loglib::LogValue{static_cast<int64_t>(lineId)});
                batch.streamLines.emplace_back(
                    std::move(values), keys, loglib::StreamLineReference("synthetic", "x", lineId)
                );
            }
            return batch;
        };

        model.SetRetentionCap(1000);
        model.AppendBatch(makeBatch(1, 100, true));
        QCOMPARE(model.rowCount(), 100);

        sink->Pause();
        // 500 paused-buffer rows arrive while paused (visible stays at 100).
        sink->OnBatch(makeBatch(101, 500, false));
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

    // PRD task 6.5: end-to-end Stream Mode smoke test against a temp file
    // tailed by `TailingFileSource`. Drives the same flow `MainWindow::
    // OpenLogStream` does (model.BeginStreaming(unique_ptr<LogSource>)) but
    // without going through the menu so the test stays self-contained.
    //
    // Coverage:
    //   - Pre-fill of 100 lines arrives at the model.
    //   - Appending 50 more lines while running grows the model to 150.
    //   - Pause freezes the visible model at 150 while the worker keeps
    //     ingesting; subsequent appends land in the paused buffer.
    //   - Resume drains the paused buffer in a single coalesced post.
    //
    // The retention cap is set high enough (1000) that the FIFO eviction
    // path stays inactive — that's covered by the dedicated
    // `testRetentionCap*` tests above.
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
        loglib::TailingFileSource::Options sourceOptions;
        sourceOptions.disableNativeWatcher = true; // poll-only for determinism
        sourceOptions.pollInterval = std::chrono::milliseconds(25);
        sourceOptions.rotationDebounce = std::chrono::milliseconds(250);

        auto source = std::make_unique<loglib::TailingFileSource>(
            std::filesystem::path(fixture.Path().toStdString()), /*retentionLines=*/1000, sourceOptions
        );

        loglib::ParserOptions options;
        // Don't pass a configuration; the auto-promote heuristics aren't the
        // focus of this test. The model's `BeginStreaming(LogSource)`
        // overrides `options.stopToken` with the sink's freshly-armed token.
        loglib::StopToken stopToken = model.BeginStreaming(std::move(source), options);
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
        // paused buffer (PRD 4.2.2.v / task 4.1).
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

        // Tear down cleanly. `Clear()` drives the PRD 4.7.2.i teardown
        // sequence (Source::Stop -> sink::RequestStop -> watcher join ->
        // DropPendingBatches) so the test exits without dangling threads.
        model.Clear();
        QCoreApplication::processEvents();

        QVERIFY(!model.IsStreamingActive());
        QCOMPARE(model.rowCount(), 0);
    }

    // PRD 4.2.2.iv regression: when the paused-buffer cap forces the
    // sink to drop its oldest entries, the count of lost lines must be
    // observable so the user knows rows were silently discarded during
    // the pause. This test pauses, feeds enough live-tail batches to
    // overflow the cap, and asserts both that `PausedDropCount()` > 0
    // and that the visible+buffered total stayed within the cap.
    void testPausedDropCountIsObservable()
    {
        LogModel model;
        static_cast<void>(model.BeginStreaming(std::unique_ptr<loglib::LogFile>{}));
        QtStreamingLogSink *sink = model.Sink();
        QVERIFY(sink != nullptr);

        const size_t cap = 100;
        model.SetRetentionCap(cap);

        QCOMPARE(static_cast<qulonglong>(sink->PausedDropCount()), qulonglong(0));

        loglib::KeyIndex &keys = sink->Keys();
        const loglib::KeyId valueKey = keys.GetOrInsert(std::string("value"));

        auto makeBatch = [&keys, valueKey](size_t firstLineId, size_t count, bool declareNewKey) {
            loglib::StreamedBatch batch;
            batch.firstLineNumber = firstLineId;
            if (declareNewKey)
            {
                batch.newKeys.emplace_back(std::string("value"));
            }
            batch.streamLines.reserve(count);
            for (size_t i = 0; i < count; ++i)
            {
                const size_t lineId = firstLineId + i;
                std::vector<std::pair<loglib::KeyId, loglib::LogValue>> values;
                values.emplace_back(valueKey, loglib::LogValue{static_cast<int64_t>(lineId)});
                batch.streamLines.emplace_back(
                    std::move(values), keys, loglib::StreamLineReference("synthetic", "x", lineId)
                );
            }
            return batch;
        };

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
            sink->OnBatch(makeBatch(static_cast<size_t>(b) * batchLines + 1, batchLines, firstBatch));
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

        model.Detach();
        QCoreApplication::processEvents();
    }

    // PRD 4.7.1 regression: `Stop` ends the streaming session but leaves
    // the most-recently-buffered rows visible (the model becomes a
    // static snapshot of what was in memory at stop time). `Detach()`
    // is the API `MainWindow::StopStream` uses; `Clear()` (which fully
    // resets the model) is reserved for the "open a new session" paths.
    void testStopDetachPreservesRows()
    {
        LogModel model;
        QSignalSpy finishedSpy(&model, &LogModel::streamingFinished);
        QVERIFY(finishedSpy.isValid());
        QSignalSpy lineCountSpy(&model, &LogModel::lineCountChanged);
        QVERIFY(lineCountSpy.isValid());

        static_cast<void>(model.BeginStreaming(std::unique_ptr<loglib::LogFile>{}));
        QVERIFY(model.IsStreamingActive());

        // Feed one batch of synthetic stream rows so `Detach` has
        // something to preserve.
        loglib::KeyIndex &keys = model.Sink()->Keys();
        const loglib::KeyId valueKey = keys.GetOrInsert(std::string("value"));

        loglib::StreamedBatch batch;
        batch.firstLineNumber = 1;
        batch.newKeys.emplace_back(std::string("value"));
        const int batchSize = 7;
        batch.streamLines.reserve(batchSize);
        for (int i = 0; i < batchSize; ++i)
        {
            const size_t lineId = static_cast<size_t>(i + 1);
            std::vector<std::pair<loglib::KeyId, loglib::LogValue>> values;
            values.emplace_back(valueKey, loglib::LogValue{static_cast<int64_t>(lineId)});
            batch.streamLines.emplace_back(
                std::move(values), keys, loglib::StreamLineReference("synthetic", "x", lineId)
            );
        }
        model.AppendBatch(std::move(batch));
        QCOMPARE(model.rowCount(), batchSize);

        // Detach: streaming flag flips off, compensating
        // `streamingFinished(Cancelled)` fires, but the table keeps
        // every row (PRD 4.7.1).
        model.Detach();
        QCoreApplication::processEvents();

        QVERIFY(!model.IsStreamingActive());
        QCOMPARE(model.rowCount(), batchSize);
        QCOMPARE(finishedSpy.count(), 1);
        QCOMPARE(finishedSpy.takeFirst().value(0).value<StreamingResult>(), StreamingResult::Cancelled);

        // A second Detach on an already-idle model is a no-op (no
        // spurious `streamingFinished`, no row mutation).
        model.Detach();
        QCOMPARE(finishedSpy.count(), 0);
        QCOMPARE(model.rowCount(), batchSize);

        // Verify the surviving rows are still queryable (the user can
        // still sort / filter / copy as the PRD promises).
        const int valueColumn = ColumnByHeader(model, QStringLiteral("value"));
        QVERIFY(valueColumn >= 0);
        QCOMPARE(
            model.data(model.index(0, valueColumn), LogModelItemDataRole::SortRole).toLongLong(), qint64(1)
        );
        QCOMPARE(
            model.data(model.index(batchSize - 1, valueColumn), LogModelItemDataRole::SortRole).toLongLong(),
            qint64(batchSize)
        );

        // Clear() on the post-Detach state performs the full reset
        // (no streaming active, so no second `streamingFinished`).
        model.Clear();
        QCOMPARE(model.rowCount(), 0);
        QCOMPARE(finishedSpy.count(), 0);
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
