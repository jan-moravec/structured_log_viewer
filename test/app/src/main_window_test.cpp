#include "log_filter_model.hpp"
#include "log_model.hpp"
#include "log_table_view.hpp"
#include "main_window.hpp"
#include "qt_streaming_log_sink.hpp"

#include <loglib/json_parser.hpp>
#include <loglib/log_file.hpp>

#include <QSignalSpy>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QtTest/QtTest>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <map>
#include <stop_token>
#include <string>
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

    QString Path() const { return mPath; }

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
        snap.headers.push_back(
            model.headerData(col, Qt::Horizontal, Qt::DisplayRole).toString().toStdString()
        );
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

    // PRD req. S7 / task 5.23 — drive the same fixture through both the legacy
    // synchronous JsonParser::Parse path (LogModel::AddData) and the streaming
    // pipeline (LogModel::BeginStreaming + ParseStreaming + EndStreaming) and
    // assert byte-equivalent display output. Pinned to threads=1 so the order
    // in which Stage B workers race on KeyIndex::GetOrInsert cannot perturb
    // the streaming-side AppendKeys insertion order — that would make the
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
        const std::stop_token stopToken = streamingModel.BeginStreaming(std::move(file));

        QVERIFY(!streamingModel.Table().Data().Files().empty());
        loglib::LogFile *parseFile = streamingModel.Table().Data().Files().front().get();
        QVERIFY(parseFile != nullptr);

        // Stage B parallelism is pinned to 1 so the canonical KeyIndex sees
        // keys in file order (a, b, c, d). The legacy LogConfigurationManager::
        // Update walks SortedKeys() (alphabetic) — both produce the same
        // column order on this fixture only because we authored it that way.
        loglib::JsonParserOptions options;
        options.threads = 1;
        options.stopToken = stopToken;

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
        parser.ParseStreaming(*parseFile, *sink, options);

        // Spin the event loop so the queued OnBatch / OnFinished invocations
        // posted by QtStreamingLogSink during the parse above are drained
        // before the assertions run. QSignalSpy::wait blocks until the signal
        // is emitted (i.e. until LogModel::EndStreaming runs on the GUI side).
        const bool finished = finishedSpy.count() > 0 || finishedSpy.wait(5000);
        QVERIFY2(finished, "streamingFinished must arrive within the timeout");
        QCOMPARE(finishedSpy.count(), 1);
        const QList<QVariant> finishedArgs = finishedSpy.takeFirst();
        QCOMPARE(finishedArgs.value(0).toBool(), false); // not cancelled

        QVERIFY2(streamingModel.StreamingErrors().empty(), "streaming parse must produce no errors on the parity fixture");

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
                    qPrintable(QStringLiteral("streaming model missing header '%1'").arg(QString::fromStdString(header)))
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

private:
    MainWindow *window;
};

QTEST_MAIN(MainWindowTest)
#include "main_window_test.moc"
