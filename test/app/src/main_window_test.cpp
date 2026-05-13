#include "filter_editor.hpp"
#include "log_filter_model.hpp"
#include "log_model.hpp"
#include "log_table_view.hpp"
#include "main_window.hpp"
#include "qt_streaming_log_sink.hpp"
#include "row_order_proxy_model.hpp"
#include "streaming_control.hpp"

#include <loglib/enum_dictionary.hpp>
#include <loglib/file_line_source.hpp>
#include <loglib/internal/advanced_parser_options.hpp>
#include <loglib/internal/compact_log_value.hpp>
#include <loglib/internal/log_configuration_glaze_meta.hpp>
#include <loglib/key_index.hpp>
#include <loglib/log_configuration.hpp>
#include <loglib/log_file.hpp>
#include <loglib/log_filter.hpp>
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

#include <QAbstractItemModel>
#include <QAction>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QLineEdit>
#include <QListView>
#include <QMenu>
#include <QMenuBar>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QScrollBar>
#include <QSignalSpy>
#include <QSortFilterProxyModel>
#include <QStandardItem>
#include <QStandardItemModel>
#include <QStatusBar>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QVariant>
#include <QWheelEvent>
#include <QtTest/QtTest>

#include <glaze/glaze.hpp>

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
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

        const loglib::JsonParser parser;
        loglib::JsonParser::ParseStreaming(*fileSourcePtr, *run.model->Sink(), options, advanced);
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
        const std::ofstream stream(mPath.toStdString(), std::ios::binary);
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

// Read-once test-time multiplier so loaded CI runners can scale wait
// budgets without touching every callsite. Mirrors
// `loglib_test::ScaledMs` (which we cannot link directly: `loglib_test`
// is the catch2 fixture target and apptest is QtTest-based). Set via
// `LOGLIB_TEST_TIME_SCALE` (double; defaults to 1.0). Sub-1.0 values
// still floor at 1 ms so a tight 25 ms budget never collapses to 0.
double LoadTestTimeScale() noexcept
{
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4996) // getenv
#endif
    // NOLINTNEXTLINE(concurrency-mt-unsafe): single-threaded test harness; read once per process.
    const char *raw = std::getenv("LOGLIB_TEST_TIME_SCALE");
#ifdef _MSC_VER
#pragma warning(pop)
#endif
    if (raw == nullptr || *raw == '\0')
    {
        return 1.0;
    }
    char *end = nullptr;
    const double parsed = std::strtod(raw, &end);
    if (end == raw || parsed <= 0.0)
    {
        return 1.0;
    }
    return parsed;
}

double TestTimeScale() noexcept
{
    static const double SCALE = LoadTestTimeScale();
    return SCALE;
}

std::chrono::milliseconds ScaledDeadline(std::chrono::milliseconds base) noexcept
{
    const double scaled = static_cast<double>(base.count()) * TestTimeScale();
    const auto rounded = std::llround(scaled);
    return std::chrono::milliseconds(rounded < 1 ? 1 : rounded);
}

// Wait until the model's `lineCountChanged` signal reports at least
// @p target rows, draining the Qt event queue while we wait so the
// QueuedConnection lambdas the parser worker posts actually run. The
// deadline is automatically scaled by `LOGLIB_TEST_TIME_SCALE` so slow
// CI runners can extend it without changing per-call code. Returns
// `true` on success, `false` if the deadline expires (the caller
// usually `QVERIFY2`s the result).
bool WaitForLineCount(LogModel &model, qsizetype target, std::chrono::milliseconds deadline)
{
    const auto scaled = ScaledDeadline(deadline);
    const auto start = std::chrono::steady_clock::now();
    while (model.rowCount() < target)
    {
        if (std::chrono::steady_clock::now() - start > scaled)
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
        batch.newKeys.emplace_back("value");
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

// NOLINTNEXTLINE(misc-use-internal-linkage): `Q_OBJECT` QtTest fixture; moc expects this declaration shape.
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
        mWindow = new MainWindow();
    }

    void cleanup()
    {
        // Called after each test function
        delete mWindow;
        mWindow = nullptr;
    }

    void TestWindowTitle()
    {
        QCOMPARE(mWindow->windowTitle(), QString("Structured Log Viewer"));
    }

    void TestWindowIcon()
    {
        QVERIFY(!mWindow->windowIcon().isNull());
    }

    // Pins the `LogModel::IsSingleLineAsciiTrim` contract that
    // `MainWindow::MakeStringMatcher` relies on. The fast path skips
    // the `ConvertToSingleLineCompactQString` round-trip only when
    // this returns true; misclassifying on either side would
    // silently change filter results.
    void TestIsSingleLineAsciiTrim()
    {
        using L = LogModel;
        // Canonical accepts.
        QVERIFY(L::IsSingleLineAsciiTrim(""));
        QVERIFY(L::IsSingleLineAsciiTrim("hello"));
        QVERIFY(L::IsSingleLineAsciiTrim("hello world"));
        QVERIFY(L::IsSingleLineAsciiTrim("a b c d e f g"));
        QVERIFY(L::IsSingleLineAsciiTrim("INFO"));
        QVERIFY(L::IsSingleLineAsciiTrim("level=info component=auth user=42"));
        QVERIFY(L::IsSingleLineAsciiTrim("!@#$%^&*()_+-=[]{}|;:,.<>/?~`"));

        // Non-ASCII bytes reject (any byte >= 0x80).
        QVERIFY(!L::IsSingleLineAsciiTrim("h\xc3\xa9llo")); // 'é' UTF-8
        QVERIFY(!L::IsSingleLineAsciiTrim("\xe2\x98\x83")); // '☃' UTF-8
        QVERIFY(!L::IsSingleLineAsciiTrim("\xff"));         // raw high byte

        // Leading / trailing whitespace and control bytes reject; the
        // QString pipeline would trim or replace them and a byte
        // compare would diverge.
        QVERIFY(!L::IsSingleLineAsciiTrim(" hello"));
        QVERIFY(!L::IsSingleLineAsciiTrim("hello "));
        QVERIFY(!L::IsSingleLineAsciiTrim("\thello"));
        QVERIFY(!L::IsSingleLineAsciiTrim("hello\n"));
        QVERIFY(!L::IsSingleLineAsciiTrim("hello\r"));

        // Internal whitespace shenanigans reject (`simplified()` would
        // collapse them to a single space).
        QVERIFY(!L::IsSingleLineAsciiTrim("hello  world"));
        QVERIFY(!L::IsSingleLineAsciiTrim("hello\tworld"));
        QVERIFY(!L::IsSingleLineAsciiTrim("hello\nworld"));
        QVERIFY(!L::IsSingleLineAsciiTrim("hello\rworld"));
        QVERIFY(!L::IsSingleLineAsciiTrim("hello\vworld"));
        QVERIFY(!L::IsSingleLineAsciiTrim("hello\fworld"));
        QVERIFY(!L::IsSingleLineAsciiTrim("a\x7f"
                                          "b")); // DEL

        // Contract: accepted input is byte-equal to
        // `ConvertToSingleLineCompactQString(bytes).toUtf8()`. Pin a
        // few cases so future contract drift in either function
        // fails here.
        for (const std::string_view sample : {
                 std::string_view{""},
                 std::string_view{"hello"},
                 std::string_view{"hello world"},
                 std::string_view{"level=info user=42"},
             })
        {
            QVERIFY(L::IsSingleLineAsciiTrim(sample));
            const QString converted = L::ConvertToSingleLineCompactQString(sample);
            const QByteArray convertedBytes = converted.toUtf8();
            const std::string_view convertedView{
                convertedBytes.constData(), static_cast<size_t>(convertedBytes.size())
            };
            QCOMPARE(convertedView, sample);
        }

        // Rejected input must actually change after conversion. The
        // discrimination is conservative: false negatives are
        // acceptable, false positives break the matcher.
        for (const std::string_view sample : {
                 std::string_view{" hello"},
                 std::string_view{"hello "},
                 std::string_view{"hello  world"},
                 std::string_view{"hello\nworld"},
                 std::string_view{"hello\tworld"},
             })
        {
            QVERIFY(!L::IsSingleLineAsciiTrim(sample));
            const QString converted = L::ConvertToSingleLineCompactQString(sample);
            const QByteArray convertedBytes = converted.toUtf8();
            const std::string_view convertedView{
                convertedBytes.constData(), static_cast<size_t>(convertedBytes.size())
            };
            QVERIFY2(
                convertedView != sample,
                qPrintable(QStringLiteral("rejected sample '%1' must differ after "
                                          "conversion to '%2'")
                               .arg(QString::fromUtf8(sample))
                               .arg(converted))
            );
        }
    }

    static void TestFixtureEmpty()
    {
        const FixtureFile fixture(":/fixtures/empty.jsonl");
        const StreamingRun run = RunStreaming(fixture.Path());
        QCOMPARE(run.finishedCount, 1);
        QCOMPARE(run.cancelled, false);
        QCOMPARE(run.model->rowCount(), 0);
        QCOMPARE(run.model->columnCount(), 0);
        QVERIFY(run.model->StreamingErrors().empty());
    }

    static void TestFixtureSingleLine()
    {
        const FixtureFile fixture(":/fixtures/single_line.jsonl");
        const StreamingRun run = RunStreaming(fixture.Path());
        QCOMPARE(run.finishedCount, 1);
        QCOMPARE(run.cancelled, false);
        QCOMPARE(run.model->rowCount(), 1);
        QCOMPARE(run.model->columnCount(), 1);
        QCOMPARE(run.model->headerData(0, Qt::Horizontal, Qt::DisplayRole).toString(), QStringLiteral("message"));
        QCOMPARE(run.model->data(run.model->index(0, 0), Qt::DisplayRole).toString(), QStringLiteral("hello"));
        QVERIFY(run.model->StreamingErrors().empty());
    }

    static void TestFixtureValueTypes()
    {
        const FixtureFile fixture(":/fixtures/value_types.jsonl");
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

    static void TestFixtureIsoTTimestamp()
    {
        const FixtureFile fixture(":/fixtures/iso_t_timestamp.jsonl");
        StreamingRun run = RunStreaming(fixture.Path());
        // 2024-04-28T07:14:30 UTC → 1714288470 seconds since epoch.
        AssertTimestampFixture(run, static_cast<qint64>(1714288470000000), 3);
    }

    static void TestFixtureIsoSpaceTimestamp()
    {
        const FixtureFile fixture(":/fixtures/iso_space_timestamp.jsonl");
        StreamingRun run = RunStreaming(fixture.Path());
        // 2024-04-28 07:14:30 UTC → 1714288470 seconds since epoch.
        AssertTimestampFixture(run, static_cast<qint64>(1714288470000000), 3);
    }

    static void TestFixtureIsoOffsetTimestamp()
    {
        const FixtureFile fixture(":/fixtures/iso_offset_timestamp.jsonl");
        StreamingRun run = RunStreaming(fixture.Path());
        // 2024-04-28T07:14:30+02:00 → 2024-04-28T05:14:30 UTC → 1714281270 seconds.
        AssertTimestampFixture(run, static_cast<qint64>(1714281270000000), 3);
    }

    static void TestFixtureIsoFractionalTimestamp()
    {
        const FixtureFile fixture(":/fixtures/iso_fractional_timestamp.jsonl");
        StreamingRun run = RunStreaming(fixture.Path());
        // 2024-04-28T07:14:30.123 UTC → 1714288470.123 → 1714288470123000 µs.
        AssertTimestampFixture(run, static_cast<qint64>(1714288470123000), 3);

        // Spot-check the µs and 0.5s rows separately so all three fractional
        // widths in the fixture are exercised.
        const int tsCol = ColumnByHeader(*run.model, QStringLiteral("timestamp"));
        const qint64 row1Us = run.model->data(run.model->index(1, tsCol), LogModelItemDataRole::SortRole).toLongLong();
        QCOMPARE(row1Us, qint64(1714288470123456));
        const qint64 row2Us = run.model->data(run.model->index(2, tsCol), LogModelItemDataRole::SortRole).toLongLong();
        QCOMPARE(row2Us, qint64(1714288470500000));
    }

    static void TestFixtureAltTimestampKeys()
    {
        const FixtureFile fixture(":/fixtures/alt_timestamp_keys.jsonl");
        const StreamingRun run = RunStreaming(fixture.Path());
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

        // "time", "ts", "Timestamp" (case-insensitive) all auto-promote to
        // `Type::Time`. The bare "t" alias was dropped to avoid false
        // positives on columns literally named `t`.
        for (const std::string &header : {std::string("time"), std::string("ts"), std::string("Timestamp")})
        {
            const auto *column = findColumn(header);
            QVERIFY2(
                column != nullptr,
                qPrintable(QStringLiteral("column '%1' must exist").arg(QString::fromStdString(header)))
            );
            QCOMPARE(column->type, loglib::LogConfiguration::Type::Time);
        }

        // Each row populated only its own timestamp column; the other two are empty.
        const int colTime = ColumnByHeader(*run.model, QStringLiteral("time"));
        const int colTs = ColumnByHeader(*run.model, QStringLiteral("ts"));
        const int colTimestamp = ColumnByHeader(*run.model, QStringLiteral("Timestamp"));
        QVERIFY(run.model->data(run.model->index(0, colTime), LogModelItemDataRole::SortRole).isValid());
        QVERIFY(!run.model->data(run.model->index(0, colTs), LogModelItemDataRole::SortRole).isValid());
        QVERIFY(!run.model->data(run.model->index(0, colTimestamp), LogModelItemDataRole::SortRole).isValid());
        QVERIFY(run.model->data(run.model->index(1, colTs), LogModelItemDataRole::SortRole).isValid());
        QVERIFY(run.model->data(run.model->index(2, colTimestamp), LogModelItemDataRole::SortRole).isValid());
    }

    static void TestFixtureMixedColumns()
    {
        const FixtureFile fixture(":/fixtures/mixed_columns.jsonl");
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

    static void TestFixtureInvalidLines()
    {
        const FixtureFile fixture(":/fixtures/invalid_lines.jsonl");
        const StreamingRun run = RunStreaming(fixture.Path());
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

    static void TestFixtureMixedTzAndOrder()
    {
        const FixtureFile fixture(":/fixtures/mixed_tz_and_order.jsonl");
        const StreamingRun run = RunStreaming(fixture.Path());
        QCOMPARE(run.finishedCount, 1);
        QCOMPARE(run.cancelled, false);
        QCOMPARE(run.model->rowCount(), 3);
        QVERIFY(run.model->StreamingErrors().empty());

        const int tsCol = ColumnByHeader(*run.model, QStringLiteral("timestamp"));
        const int msgCol = ColumnByHeader(*run.model, QStringLiteral("msg"));
        QVERIFY(tsCol >= 0 && msgCol >= 0);

        const auto &columns = run.model->Configuration().columns;
        QCOMPARE(columns[static_cast<size_t>(tsCol)].type, loglib::LogConfiguration::Type::Time);

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
        // `LogFilterModel` sorts via `loglib::CompareRows` straight
        // off the `LogTable`; bind the model so `CompareRows` can
        // resolve slot types. The SortRole round-trip is no longer
        // wired.
        proxy.setSourceModel(run.model.get());
        proxy.SetLogModel(run.model.get());

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
    static void TestAppendBatchFiresBeginsBeforeMutation()
    {
        const QStringList fixtureLines = MakeParityFixture();
        const TempJsonFile fixture(fixtureLines);

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

        const loglib::JsonParser parser;
        loglib::JsonParser::ParseStreaming(*parseSource, *model.Sink(), options, advanced);

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
        for (const int snapshot : rowCountSnapshotsAtAboutToBeInserted)
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
        for (const int snapshot : columnCountSnapshotsAtAboutToBeInserted)
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
    static void TestResetAfterBeginStreamingEmitsCompensatingFinished()
    {
        // Use a tiny in-memory file so BeginStreaming has a valid LogFile
        // to install on the model.
        const TempJsonFile fixture(QStringList{QStringLiteral(R"({"a": 1})")});

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
    static void TestResetDuringStreamingDropsDrainPhaseBatch()
    {
        const TempJsonFile fixture(QStringList{QStringLiteral(R"({"a": 1})")});

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
            [sinkBeforeBegin, sourcePtr, &releaseMutex, &releaseCv, &released](const loglib::StopToken &stopToken) {
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
                const std::scoped_lock lock(releaseMutex);
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
    static void TestStopAndKeepRowsPreservesDrainPhaseBatch()
    {
        const TempJsonFile fixture(QStringList{QStringLiteral(R"({"a": 1})")});

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
            [sinkBeforeBegin, sourcePtr, &releaseMutex, &releaseCv, &released](const loglib::StopToken &stopToken) {
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
                const std::scoped_lock lock(releaseMutex);
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
    static void TestRetentionCapFifoEviction()
    {
        LogModel model;
        // Install a no-producer `StreamLineSource` so synthetic
        // `LogLine`s constructed below have a valid `LineSource *` to
        // bind to.
        loglib::StreamLineSource &streamSource = BeginSyntheticStreamSession(model);
        model.SetRetentionCap(100);

        const QSignalSpy rowsRemovedSpy(&model, &QAbstractItemModel::rowsRemoved);
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
    static void TestRetentionCapGiantBatchCollapse()
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
    static void TestSinkPauseResumeCoalescesBufferedBatches()
    {
        LogModel model;
        const QSignalSpy lineCountSpy(&model, &LogModel::lineCountChanged);
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
            const auto firstLineId = (static_cast<size_t>(b) * static_cast<size_t>(linesPerBatch)) + size_t{1};
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
    static void TestSinkPauseResumePreservesStaticLineBatches()
    {
        // Need a real `LogFile` because the `FileLineSource` (and the
        // model's `LogTable::AppendBatch` path that consumes `lines`) hold
        // pointers into the file. The file's contents don't have to match
        // what we synthesise -- we never call `LogLine::GetValue()` on them
        // through the table -- but they must outlive the test.
        const TempJsonFile fixture(QStringList{
            QStringLiteral(R"({"a": 1})"),
            QStringLiteral(R"({"a": 2})"),
            QStringLiteral(R"({"a": 3})"),
        });

        LogModel model;
        const QSignalSpy lineCountSpy(&model, &LogModel::lineCountChanged);
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
            batch.firstLineNumber = (static_cast<size_t>(b) * static_cast<size_t>(linesPerBatch)) + size_t{1};
            batch.lines.reserve(linesPerBatch);
            batch.localLineOffsets.reserve(linesPerBatch);
            for (int i = 0; i < linesPerBatch; ++i)
            {
                const auto lineNumber =
                    (static_cast<size_t>(b) * static_cast<size_t>(linesPerBatch)) + static_cast<size_t>(i) + size_t{1};
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
    static void TestPauseCapShrinkTrimsPausedBuffer()
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
    static void TestStreamModeOpensTailFileAndAppends()
    {
        const TempLiveTailFile fixture;

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
        const QSignalSpy lineCountSpy(&model, &LogModel::lineCountChanged);
        QVERIFY(lineCountSpy.isValid());
        const QSignalSpy finishedSpy(&model, &LogModel::streamingFinished);
        QVERIFY(finishedSpy.isValid());

        model.SetRetentionCap(1000);

        // Use a fast poll cadence on the source so the test wall-clock stays
        // small; the production default is 250 ms which would push the test
        // budget over a CI runner's timeout for marginal benefit here.
        loglib::TailingBytesProducer::Options sourceOptions;
        sourceOptions.disableNativeWatcher = true; // poll-only for determinism
        sourceOptions.pollInterval = std::chrono::milliseconds(25);
        sourceOptions.rotationDebounce = std::chrono::milliseconds(250);

        const std::filesystem::path filePath(fixture.Path().toStdString());
        auto source = std::make_unique<loglib::TailingBytesProducer>(filePath, /*retentionLines=*/1000, sourceOptions);
        auto streamSource = std::make_unique<loglib::StreamLineSource>(filePath, std::move(source));

        const loglib::ParserOptions options;
        // Don't pass a configuration; the auto-promote heuristics aren't the
        // focus of this test. The model's `BeginStreaming(StreamLineSource)`
        // overrides `options.stopToken` with the sink's freshly-armed token.
        const loglib::StopToken stopToken = model.BeginStreaming(std::move(streamSource), options);
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
    static void TestStreamModeOpensUdpProducer()
    {
        loglib::UdpServerProducer::Options opts;
        opts.bindAddress = "127.0.0.1"; // ephemeral port
        auto producer = std::make_unique<loglib::UdpServerProducer>(opts);
        const uint16_t port = producer->BoundPort();
        QVERIFY(port != 0);
        const std::string display = producer->DisplayName();

        LogModel model;
        const QSignalSpy lineCountSpy(&model, &LogModel::lineCountChanged);
        QVERIFY(lineCountSpy.isValid());
        model.SetRetentionCap(1000);

        auto streamSource =
            std::make_unique<loglib::StreamLineSource>(std::filesystem::path(display), std::move(producer));
        const loglib::ParserOptions options;
        Q_UNUSED(model.BeginStreaming(std::move(streamSource), options));

        QVERIFY(model.IsStreamingActive());

        // Send 10 datagrams. Each lands as one line in the parser.
        test_common::UdpLogClient client("127.0.0.1", port);
        for (int i = 0; i < 10; ++i)
        {
            client.Send("{\"i\":" + std::to_string(i + 1) + R"(,"phase":"udp"})");
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
    static void TestStreamModeOpensTcpProducer()
    {
        loglib::TcpServerProducer::Options opts;
        opts.bindAddress = "127.0.0.1"; // ephemeral port
        auto producer = std::make_unique<loglib::TcpServerProducer>(opts);
        const uint16_t port = producer->BoundPort();
        QVERIFY(port != 0);
        const std::string display = producer->DisplayName();
        QVERIFY(display.starts_with("tcp://127.0.0.1:"));

        LogModel model;
        const QSignalSpy lineCountSpy(&model, &LogModel::lineCountChanged);
        QVERIFY(lineCountSpy.isValid());
        model.SetRetentionCap(1000);

        auto streamSource =
            std::make_unique<loglib::StreamLineSource>(std::filesystem::path(display), std::move(producer));
        const loglib::ParserOptions options;
        Q_UNUSED(model.BeginStreaming(std::move(streamSource), options));

        QVERIFY(model.IsStreamingActive());

        test_common::TcpLogClient client("127.0.0.1", port);
        for (int i = 0; i < 10; ++i)
        {
            client.Send("{\"i\":" + std::to_string(i + 1) + R"(,"phase":"tcp"})");
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
    static void TestActionOpenNetworkStreamIsExposed()
    {
        // Local variable name avoids the `MainWindowTest::window`
        // member-shadow C4458 warning under MSVC.
        const MainWindow mainWindow;
        const QAction *action = mainWindow.FindUiAction(QStringLiteral("actionOpenNetworkStream"));
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
    static void TestPausedDropCountIsObservable()
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
                streamSource, keys, valueKey, (static_cast<size_t>(b) * batchLines) + 1, batchLines, firstBatch
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

    // Regression: pausing during a live-tail session, feeding N
    // synthetic batches into the paused buffer, then `StopAndKeepRows`
    // must flush every still-buffered row into the visible model so
    // the user sees the final state of the parse rather than losing
    // the last-emitted rows. Asserts that
    // `visibleRows == sumOfBatchedRows - PausedDropCount` so the
    // accounting closes: any row that left the paused buffer either
    // landed in the visible model or was counted in the cap-eviction
    // counter, but never silently disappeared. Counterpart to
    // `testPausedDropCountIsObservable` (which exercises overflow);
    // this one exercises the under-cap normal-stop path.
    static void TestStopAfterPauseFlushesPausedBufferToModel()
    {
        LogModel model;
        loglib::StreamLineSource &streamSource = BeginSyntheticStreamSession(model);
        QtStreamingLogSink *sink = model.Sink();
        QVERIFY(sink != nullptr);

        // Cap chosen high enough to never trigger paused-buffer
        // eviction so this test exercises the no-loss path. Five
        // batches of 50 lines = 250 rows, well under cap.
        const size_t cap = 1000;
        model.SetRetentionCap(cap);
        QCOMPARE(static_cast<qulonglong>(sink->PausedDropCount()), qulonglong(0));

        loglib::KeyIndex &keys = sink->Keys();
        const loglib::KeyId valueKey = keys.GetOrInsert(std::string("value"));

        sink->Pause();
        QVERIFY(sink->IsPaused());

        const size_t batchLines = 50;
        const int batchesPushed = 5;
        const size_t totalLines = batchLines * batchesPushed;
        for (int b = 0; b < batchesPushed; ++b)
        {
            const bool firstBatch = (b == 0);
            sink->OnBatch(MakeSyntheticBatch(
                streamSource, keys, valueKey, (static_cast<size_t>(b) * batchLines) + 1, batchLines, firstBatch
            ));
        }

        QCOMPARE(static_cast<int>(sink->PausedLineCount()), static_cast<int>(totalLines));
        QCOMPARE(model.rowCount(), 0); // still paused, nothing visible

        // StopAndKeepRows must drain the paused buffer into the
        // visible model BEFORE bumping the generation that would
        // short-circuit drain-phase callbacks. The teardown sequence
        // (TeardownStreamingSessionInternal) is responsible for
        // calling `TakePausedBuffer` and feeding it through
        // `AppendBatch` -- this test fails loudly if that step is
        // ever removed.
        model.StopAndKeepRows();
        QCoreApplication::processEvents();

        const qsizetype visible = model.rowCount();
        const auto dropped = static_cast<qulonglong>(sink->PausedDropCount());
        QVERIFY2(
            static_cast<qulonglong>(visible) + dropped == totalLines,
            qPrintable(QStringLiteral("rows accounting failed: visible=%1 + dropped=%2 != totalLines=%3")
                           .arg(visible)
                           .arg(dropped)
                           .arg(static_cast<qulonglong>(totalLines)))
        );
        QCOMPARE(static_cast<int>(visible), static_cast<int>(totalLines));
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
    static void TestPausedDropCountReflectsStaticBatchOverEviction()
    {
        const TempJsonFile fixture(QStringList{
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
            batch.newKeys.emplace_back("value");
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
    static void TestStopAndKeepRowsPreservesRows()
    {
        LogModel model;
        QSignalSpy finishedSpy(&model, &LogModel::streamingFinished);
        QVERIFY(finishedSpy.isValid());
        const QSignalSpy lineCountSpy(&model, &LogModel::lineCountChanged);
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
    static void TestResumeDeliversBufferedBatchSynchronouslyForOrdering()
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
    void TestStreamMenuActionsDisabledWhileIdle()
    {
        QAction *pauseAction = FindActionByObjectName(mWindow, QStringLiteral("actionPauseStream"));
        const QAction *followAction = FindActionByObjectName(mWindow, QStringLiteral("actionFollowTail"));
        const QAction *stopAction = FindActionByObjectName(mWindow, QStringLiteral("actionStopStream"));
        QVERIFY(pauseAction != nullptr);
        QVERIFY(followAction != nullptr);
        QVERIFY(stopAction != nullptr);

        // Idle at startup: no stream has been opened yet, the toolbar is
        // hidden, and the Stream menu actions must be disabled so a
        // user click on the menu cannot flip the checked state.
        // NOLINTBEGIN(clang-analyzer-core.CallAndMessage): false positive; prior `QVERIFY` aborts on null.
        QVERIFY(!pauseAction->isEnabled());
        QVERIFY(!followAction->isEnabled());
        QVERIFY(!stopAction->isEnabled());
        QVERIFY(!pauseAction->isChecked());
        // NOLINTEND(clang-analyzer-core.CallAndMessage)

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
    void TestStaleCheckedPauseClearedOnTeardown()
    {
        QAction *pauseAction = FindActionByObjectName(mWindow, QStringLiteral("actionPauseStream"));
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
        auto *model = mWindow->findChild<LogModel *>();
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
    void TestFollowTailIgnoresProgrammaticScrollbarChanges()
    {
        QAction *followAction = FindActionByObjectName(mWindow, QStringLiteral("actionFollowTail"));
        const auto *tableView = mWindow->findChild<LogTableView *>();
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

        const QSignalSpy awaySpy(tableView, &LogTableView::userScrolledAwayFromTail);
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
    void TestNewestFirstReversesProxyOrder()
    {
        auto *rowOrderProxy = mWindow->findChild<RowOrderProxyModel *>();
        auto *model = mWindow->findChild<LogModel *>();
        QVERIFY(rowOrderProxy != nullptr);
        QVERIFY(model != nullptr);
        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): false positive; prior `QVERIFY` aborts on null.
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
    void TestNewestFirstIncrementalBatchesKeepNewestAtTop()
    {
        auto *rowOrderProxy = mWindow->findChild<RowOrderProxyModel *>();
        auto *model = mWindow->findChild<LogModel *>();
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
    void TestNewestFirstToggleIsOPersistentIndices()
    {
        auto *rowOrderProxy = mWindow->findChild<RowOrderProxyModel *>();
        auto *model = mWindow->findChild<LogModel *>();
        QVERIFY(rowOrderProxy != nullptr);
        QVERIFY(model != nullptr);

        loglib::StreamLineSource &streamSource = BeginSyntheticStreamSession(*model);
        QtStreamingLogSink *sink = model->Sink();
        QVERIFY(sink != nullptr);

        constexpr int K_ROWS = 10'000;
        loglib::KeyIndex &keys = sink->Keys();
        const loglib::KeyId valueKey = keys.GetOrInsert(std::string("value"));
        sink->OnBatch(MakeSyntheticBatch(streamSource, keys, valueKey, 1, K_ROWS, /*declareNewKey=*/true));
        QCoreApplication::processEvents();
        QCOMPARE(model->rowCount(), K_ROWS);

        // Hold a single persistent index from somewhere in the
        // middle of the proxy. Toggling SetReversed must remap *this
        // one* via `changePersistentIndexList`; nothing else.
        const QPersistentModelIndex pinned(rowOrderProxy->index(K_ROWS / 2, 0));
        QVERIFY(pinned.isValid());

        rowOrderProxy->SetReversed(false);
        QVERIFY(!rowOrderProxy->IsReversed());

        QElapsedTimer timer;
        timer.start();
        constexpr int K_TOGGLES = 100;
        for (int i = 0; i < K_TOGGLES; ++i)
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
                           .arg(K_ROWS)
                           .arg(elapsedMs))
        );

        // Sanity check: the persistent index correctly tracks the
        // mapping flip. After the last toggle (kToggles-1=99 -> odd
        // -> reversed=true), the pinned source row (kRows/2 in
        // identity, which is what the constructor saw) should sit at
        // proxy row (kRows - 1 - kRows/2).
        QVERIFY(pinned.isValid());
        QCOMPARE(pinned.row(), K_ROWS - 1 - (K_ROWS / 2));

        rowOrderProxy->SetReversed(false);
        model->EndStreaming(false);
    }

    // Regression for `LogFilterModel`'s persistent-index handling
    // across a *source-side* layout change. Toggling
    // `RowOrderProxyModel::SetReversed` is the production trigger:
    // it flips its mapping (source row K now points at a different
    // entity), emits `layoutChanged`, and the `LogFilterModel` above
    // must remap its own persistent indices to follow the entity,
    // not the bare row number.
    //
    // Pre-fix the snapshot stored an integer source row, which under
    // a no-filter pass-through stayed numerically the same after the
    // flip; the filter's persistent index ended up pointing at the
    // entity that moved into the row -- a silent selection swap. The
    // fix anchors each snapshot on a `QPersistentModelIndex` of the
    // source-mapped index, which the source's own
    // `changePersistentIndexList` remaps to the entity's new row.
    void TestNewestFirstReversalPreservesFilterModelSelection()
    {
        auto *rowOrderProxy = mWindow->findChild<RowOrderProxyModel *>();
        auto *model = mWindow->findChild<LogModel *>();
        auto *filterModel = mWindow->FilterModel();
        QVERIFY(rowOrderProxy != nullptr);
        QVERIFY(model != nullptr);
        QVERIFY(filterModel != nullptr);

        loglib::StreamLineSource &streamSource = BeginSyntheticStreamSession(*model);
        QtStreamingLogSink *sink = model->Sink();
        QVERIFY(sink != nullptr);

        constexpr int K_ROWS = 16;
        loglib::KeyIndex &keys = sink->Keys();
        const loglib::KeyId valueKey = keys.GetOrInsert(std::string("value"));
        sink->OnBatch(MakeSyntheticBatch(streamSource, keys, valueKey, 1, K_ROWS, /*declareNewKey=*/true));
        QCoreApplication::processEvents();
        QCOMPARE(filterModel->rowCount(), K_ROWS);

        const int valueColumn = ColumnByHeader(*model, QStringLiteral("value"));
        QVERIFY(valueColumn >= 0);

        // Pin a persistent index on the filter model (not the
        // RowOrderProxyModel) at proxy row 5. In default
        // (oldest-first) mode that's lineId 6.
        constexpr int K_PINNED_PROXY_ROW = 5;
        const QPersistentModelIndex pinned(filterModel->index(K_PINNED_PROXY_ROW, valueColumn));
        QVERIFY(pinned.isValid());
        const qint64 originalLineId = filterModel->data(pinned, LogModelItemDataRole::SortRole).toLongLong();
        QCOMPARE(originalLineId, qint64(K_PINNED_PROXY_ROW + 1));

        // Flip the source layout via SetReversed. The persistent
        // index must follow the entity (lineId K_PINNED_PROXY_ROW + 1),
        // which after reversal lives at proxy row
        // `K_ROWS - 1 - K_PINNED_PROXY_ROW`.
        rowOrderProxy->SetReversed(true);
        QCoreApplication::processEvents();
        QVERIFY(rowOrderProxy->IsReversed());

        QVERIFY(pinned.isValid());
        const qint64 lineIdAfterFlip = filterModel->data(pinned, LogModelItemDataRole::SortRole).toLongLong();
        QCOMPARE(lineIdAfterFlip, originalLineId);
        QCOMPARE(pinned.row(), K_ROWS - 1 - K_PINNED_PROXY_ROW);

        // Flip back; the persistent index returns to its original row.
        rowOrderProxy->SetReversed(false);
        QCoreApplication::processEvents();
        QVERIFY(pinned.isValid());
        QCOMPARE(pinned.row(), K_PINNED_PROXY_ROW);
        QCOMPARE(filterModel->data(pinned, LogModelItemDataRole::SortRole).toLongLong(), originalLineId);

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
    void TestTailEdgeTopFollowsScrollbarMinimum()
    {
        auto *tableView = mWindow->findChild<LogTableView *>();
        QVERIFY(tableView != nullptr);

        tableView->SetTailEdge(LogTableView::TailEdge::Top);
        QCOMPARE(tableView->GetTailEdge(), LogTableView::TailEdge::Top);

        QScrollBar *scrollBar = tableView->verticalScrollBar();
        QVERIFY(scrollBar != nullptr);
        scrollBar->setRange(0, 1000);
        scrollBar->setValue(scrollBar->minimum());

        // Programmatic value change to the middle: must not flip the
        // toggle (mirrors the bottom-edge programmatic-scroll test).
        const QSignalSpy awaySpy(tableView, &LogTableView::userScrolledAwayFromTail);
        const QSignalSpy toSpy(tableView, &LogTableView::userScrolledToTail);
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
    void TestFollowNewestDisengagesOnScrollbarAction()
    {
        QAction *followAction = FindActionByObjectName(mWindow, QStringLiteral("actionFollowTail"));
        const auto *tableView = mWindow->findChild<LogTableView *>();
        QVERIFY(followAction != nullptr);
        QVERIFY(tableView != nullptr);

        followAction->setEnabled(true);
        followAction->setChecked(true);
        QVERIFY(followAction->isChecked());

        QScrollBar *scrollBar = tableView->verticalScrollBar();
        QVERIFY(scrollBar != nullptr);
        scrollBar->setRange(0, 1000);
        scrollBar->setValue(scrollBar->maximum());

        const QSignalSpy awaySpy(tableView, &LogTableView::userScrolledAwayFromTail);
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
    void TestNewestFirstPreservesReadingPositionAcrossBatches()
    {
        auto *rowOrderProxy = mWindow->findChild<RowOrderProxyModel *>();
        auto *tableView = mWindow->findChild<LogTableView *>();
        auto *model = mWindow->findChild<LogModel *>();
        QAction *followAction = FindActionByObjectName(mWindow, QStringLiteral("actionFollowTail"));
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
    void TestAlternatingRowColoursDisabledInNewestFirstMode()
    {
        const auto *tableView = mWindow->findChild<LogTableView *>();
        auto *model = mWindow->findChild<LogModel *>();
        QVERIFY(tableView != nullptr);
        QVERIFY(model != nullptr);

        const bool originalNewestFirst = StreamingControl::IsNewestFirst();
        auto restoreNewestFirst = qScopeGuard([this, originalNewestFirst]() {
            StreamingControl::SetNewestFirst(originalNewestFirst);
            mWindow->SetSessionModeForTest(MainWindow::TestSessionMode::Idle);
        });

        // Stage a stream-mode session so `ApplyDisplayOrder` consults
        // `IsNewestFirst()` (and not `IsStaticNewestFirst()`).
        static_cast<void>(BeginSyntheticStreamSession(*model));
        mWindow->SetSessionModeForTest(MainWindow::TestSessionMode::LiveTail);

        // Default-mode baseline: alternation is on so users still get
        // the lighter/darker reading aid while reading static logs or
        // a bottom-tail stream.
        StreamingControl::SetNewestFirst(false);
        mWindow->ApplyDisplayOrder();
        QVERIFY2(tableView->alternatingRowColors(), "default bottom-tail mode should keep alternating row colours on");

        // Newest-first flips the toggle off — see the comment in
        // `ApplyDisplayOrder` for the rationale.
        StreamingControl::SetNewestFirst(true);
        mWindow->ApplyDisplayOrder();
        QVERIFY2(
            !tableView->alternatingRowColors(),
            "newest-first mode should disable alternating row colours to avoid the "
            "row-parity flicker on every incoming batch"
        );

        // Toggling back restores the reading aid (no-op for users who
        // never enabled newest-first, but covers the "I tried it,
        // didn't like it, switched back" path).
        StreamingControl::SetNewestFirst(false);
        mWindow->ApplyDisplayOrder();
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
    void TestStaticNewestFirstReversesProxyOrder()
    {
        const auto *rowOrderProxy = mWindow->findChild<RowOrderProxyModel *>();
        const auto *tableView = mWindow->findChild<LogTableView *>();
        auto *model = mWindow->findChild<LogModel *>();
        QVERIFY(rowOrderProxy != nullptr);
        QVERIFY(tableView != nullptr);
        QVERIFY(model != nullptr);

        const bool originalStream = StreamingControl::IsNewestFirst();
        const bool originalStatic = StreamingControl::IsStaticNewestFirst();
        auto restore = qScopeGuard([this, originalStream, originalStatic]() {
            StreamingControl::SetNewestFirst(originalStream);
            StreamingControl::SetStaticNewestFirst(originalStatic);
            mWindow->SetSessionModeForTest(MainWindow::TestSessionMode::Idle);
        });

        BeginSyntheticStaticSession(*model);
        mWindow->SetSessionModeForTest(MainWindow::TestSessionMode::Static);

        // Stream-mode flag has no effect on a static session: with the
        // stream flag ON and the static flag OFF, the proxy must stay
        // in the identity orientation.
        StreamingControl::SetNewestFirst(true);
        StreamingControl::SetStaticNewestFirst(false);
        mWindow->ApplyDisplayOrder();
        QVERIFY2(!rowOrderProxy->IsReversed(), "static session must ignore the stream-mode newest-first flag");
        QVERIFY(tableView->alternatingRowColors());

        // Flipping the static-mode flag drives the same proxy reversal
        // as the stream-mode flag does for live-tail sessions.
        StreamingControl::SetStaticNewestFirst(true);
        mWindow->ApplyDisplayOrder();
        QVERIFY(rowOrderProxy->IsReversed());
        QVERIFY(!tableView->alternatingRowColors());

        StreamingControl::SetStaticNewestFirst(false);
        mWindow->ApplyDisplayOrder();
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
    void TestStaticSessionDoesNotFollowNewestRows()
    {
        auto *tableView = mWindow->findChild<LogTableView *>();
        auto *model = mWindow->findChild<LogModel *>();
        const QAction *followAction = FindActionByObjectName(mWindow, QStringLiteral("actionFollowTail"));
        QVERIFY(tableView != nullptr);
        QVERIFY(model != nullptr);
        QVERIFY(followAction != nullptr);

        auto restore = qScopeGuard([this]() { mWindow->SetSessionModeForTest(MainWindow::TestSessionMode::Idle); });

        // Static session, with `actionFollowTail` left in its startup
        // baseline (checked). Production users never see this toggle
        // in static mode (the toolbar is hidden, the action is
        // disabled), but the *value* persists across sessions and is
        // what the buggy auto-scroll path was reading.
        loglib::StreamLineSource &streamSource = BeginSyntheticStaticSession(*model);
        mWindow->SetSessionModeForTest(MainWindow::TestSessionMode::Static);
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
    void TestStaticSessionDoesNotReArmFollowOnScrollToTail()
    {
        auto *tableView = mWindow->findChild<LogTableView *>();
        auto *model = mWindow->findChild<LogModel *>();
        QAction *followAction = FindActionByObjectName(mWindow, QStringLiteral("actionFollowTail"));
        QVERIFY(tableView != nullptr);
        QVERIFY(model != nullptr);
        QVERIFY(followAction != nullptr);

        auto restore = qScopeGuard([this]() { mWindow->SetSessionModeForTest(MainWindow::TestSessionMode::Idle); });

        BeginSyntheticStaticSession(*model);
        mWindow->SetSessionModeForTest(MainWindow::TestSessionMode::Static);

        // Force the action *unchecked* so the re-arm path has a
        // visible state transition to attempt.
        followAction->setChecked(false);
        QVERIFY(!followAction->isChecked());

        const QSignalSpy toSpy(tableView, &LogTableView::userScrolledToTail);
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
    void TestNewestFirstFollowsSessionModeOnTransition()
    {
        const auto *rowOrderProxy = mWindow->findChild<RowOrderProxyModel *>();
        auto *model = mWindow->findChild<LogModel *>();
        QVERIFY(rowOrderProxy != nullptr);
        QVERIFY(model != nullptr);

        const bool originalStream = StreamingControl::IsNewestFirst();
        const bool originalStatic = StreamingControl::IsStaticNewestFirst();
        auto restore = qScopeGuard([this, originalStream, originalStatic]() {
            StreamingControl::SetNewestFirst(originalStream);
            StreamingControl::SetStaticNewestFirst(originalStatic);
            mWindow->SetSessionModeForTest(MainWindow::TestSessionMode::Idle);
        });

        // Both ON: every session orientation should land reversed.
        StreamingControl::SetNewestFirst(true);
        StreamingControl::SetStaticNewestFirst(true);

        BeginSyntheticStaticSession(*model);
        mWindow->SetSessionModeForTest(MainWindow::TestSessionMode::Static);
        mWindow->ApplyDisplayOrder();
        QVERIFY(rowOrderProxy->IsReversed());

        // End the static session and start a stream one; the proxy
        // must stay reversed because the stream-mode flag is also ON.
        model->EndStreaming(false);
        static_cast<void>(BeginSyntheticStreamSession(*model));
        mWindow->SetSessionModeForTest(MainWindow::TestSessionMode::LiveTail);
        mWindow->ApplyDisplayOrder();
        QVERIFY(rowOrderProxy->IsReversed());

        // Asymmetric: only the static-mode flag is ON. The proxy must
        // de-reverse for the stream session (still active) and only
        // re-reverse once we transition back to a static session.
        StreamingControl::SetNewestFirst(false);
        mWindow->ApplyDisplayOrder();
        QVERIFY2(
            !rowOrderProxy->IsReversed(), "stream session must follow the stream-mode flag, not the static-mode one"
        );

        model->EndStreaming(false);
        BeginSyntheticStaticSession(*model);
        mWindow->SetSessionModeForTest(MainWindow::TestSessionMode::Static);
        mWindow->ApplyDisplayOrder();
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
    static void TestBoundedQueueBlocksWorkerWhenGuiFallsBehind()
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
    static void TestStopWhileWorkerBlockedOnBoundedQueue()
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
    static void TestNoBatchLossUnderBackPressure()
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

    // Picker UI: stream a fixture whose `level` column auto-promotes to
    // `Type::Enumeration`, open `FilterEditor` against it, and verify
    // that typing into the search box prunes the visible item count.
    void TestFilterEditorPickerSearchFiltersVisibleCount()
    {
        // Past `STREAM_PROMOTION_MIN_ROWS` (2) and under the 64-value cap.
        const QStringList levels{
            QStringLiteral("info"),
            QStringLiteral("warn"),
            QStringLiteral("error"),
            QStringLiteral("debug"),
            QStringLiteral("trace"),
        };
        QStringList lines;
        lines.reserve(300);
        for (int i = 0; i < 300; ++i)
        {
            lines.append(QStringLiteral(R"({"level": "%1"})").arg(levels[i % levels.size()]));
        }
        const TempJsonFile fixture(lines);

        const StreamingRun run = RunStreaming(fixture.Path());
        QCOMPARE(run.finishedCount, 1);
        QCOMPARE(run.cancelled, false);
        QVERIFY(run.model->StreamingErrors().empty());

        const int levelCol = ColumnByHeader(*run.model, QStringLiteral("level"));
        QVERIFY2(levelCol >= 0, "auto-promoted level column must exist");
        const auto &columns = run.model->Configuration().columns;
        QVERIFY(static_cast<size_t>(levelCol) < columns.size());
        QCOMPARE(columns[static_cast<size_t>(levelCol)].type, loglib::LogConfiguration::Type::Enumeration);

        FilterEditor editor(*run.model, QStringLiteral("test-filter"));
        editor.Load(levelCol, QStringList{});

        // Reach the picker through the explicit accessor: on the Linux
        // runner with Qt 6.8 + offscreen QPA, `findChildren<QListView*>`
        // strands these widgets the same way it strands `QAction`s in
        // the `MainWindow` `.ui` (see `MainWindow::FindUiAction`).
        const QListView *picker = editor.EnumPickerView();
        const QSortFilterProxyModel *proxy = editor.EnumPickerProxy();
        QVERIFY2(picker != nullptr, "FilterEditor must expose its enum picker QListView");
        QVERIFY2(proxy != nullptr, "picker must wrap a QSortFilterProxyModel");
        QCOMPARE(proxy->rowCount(), 5);

        QLineEdit *searchBox = editor.EnumSearchEdit();
        QVERIFY2(searchBox != nullptr, "FilterEditor must expose the picker search QLineEdit");

        searchBox->setText(QStringLiteral("err"));
        QCoreApplication::processEvents();
        QCOMPARE(proxy->rowCount(), 1);
        QCOMPARE(proxy->data(proxy->index(0, 0), Qt::DisplayRole).toString(), QStringLiteral("error"));

        searchBox->setText(QStringLiteral("e"));
        QCoreApplication::processEvents();
        // error / debug / trace contain 'e'; info and warn do not.
        QCOMPARE(proxy->rowCount(), 3);

        searchBox->clear();
        QCoreApplication::processEvents();
        QCOMPARE(proxy->rowCount(), 5);
    }

    // A saved string filter against a now-enum column must be dropped at
    // `AddFilter` time, not silently retained as a type-mismatched rule.
    void TestSavedStringFilterDroppedOnNowEnumColumn()
    {
        // 300 rows is well past the stream-mode promotion threshold.
        const QStringList levels{QStringLiteral("info"), QStringLiteral("warn"), QStringLiteral("error")};
        QStringList lines;
        lines.reserve(300);
        for (int i = 0; i < 300; ++i)
        {
            lines.append(QStringLiteral(R"({"level": "%1"})").arg(levels[i % levels.size()]));
        }
        const TempJsonFile fixture(lines);

        // Drive streaming on the live MainWindow so wiring matches production.
        auto *model = mWindow->findChild<LogModel *>();
        QVERIFY2(model != nullptr, "MainWindow must own a LogModel");

        QSignalSpy finishedSpy(model, &LogModel::streamingFinished);
        QVERIFY(finishedSpy.isValid());

        auto file = std::make_unique<loglib::LogFile>(fixture.Path().toStdString());
        auto fileSource = std::make_unique<loglib::FileLineSource>(std::move(file));
        loglib::FileLineSource *fileSourcePtr = fileSource.get();
        const loglib::StopToken stopToken = model->BeginStreamingForSyncTest(std::move(fileSource));

        loglib::ParserOptions options;
        options.stopToken = stopToken;
        loglib::internal::AdvancedParserOptions advanced;
        advanced.threads = 1;
        const loglib::JsonParser parser;
        loglib::JsonParser::ParseStreaming(*fileSourcePtr, *model->Sink(), options, advanced);

        const bool finished = finishedSpy.count() > 0 || finishedSpy.wait(5000);
        QVERIFY2(finished, "streamingFinished must arrive within the timeout");
        QCOMPARE(model->rowCount(), 300);

        const int levelCol = ColumnByHeader(*model, QStringLiteral("level"));
        QVERIFY2(levelCol >= 0, "level column must exist");
        const auto &columns = model->Configuration().columns;
        QCOMPARE(columns[static_cast<size_t>(levelCol)].type, loglib::LogConfiguration::Type::Enumeration);

        // Find the Filters-menu action whose data matches `filterId`.
        // Reach the menu via `MainWindow::FiltersMenu()` rather than
        // `findChild`: the Linux Qt 6.8 + offscreen-QPA traversal bug
        // strands `findChild<QMenu*>("menuFilters")` the same way it
        // strands `QAction` lookups (see `MainWindow::FindUiAction`).
        const auto findFilterMenuAction = [&](const QString &filterId) -> QAction * {
            const auto *menu = mWindow->FiltersMenu();
            if (menu == nullptr)
            {
                return nullptr;
            }
            for (QAction *action : menu->actions())
            {
                if (action->data().toString() == filterId)
                {
                    return action;
                }
            }
            return nullptr;
        };

        // Stage 1: install a string rule (simulates a saved pre-promotion filter).
        const QString filterId = QStringLiteral("saved-string");
        QVERIFY2(
            QMetaObject::invokeMethod(
                mWindow,
                "FilterSubmitted",
                Qt::DirectConnection,
                Q_ARG(QString, filterId),
                Q_ARG(int, levelCol),
                Q_ARG(QString, QStringLiteral("info")),
                Q_ARG(int, static_cast<int>(loglib::LogConfiguration::LogFilter::Match::Exactly))
            ),
            "FilterSubmitted slot must be invocable via meta-object"
        );
        QCoreApplication::processEvents();
        QVERIFY2(findFilterMenuAction(filterId) != nullptr, "active filter must have a menu entry before the drop");

        // 3 levels across 300 rows -> ~100 `info` rows pass. Reach the
        // proxy via `MainWindow::FilterModel()`: `findChildren<
        // QSortFilterProxyModel*>()` is similarly stranded on the Linux
        // offscreen runner.
        const LogFilterModel *filterModel = mWindow->FilterModel();
        QVERIFY2(filterModel != nullptr, "MainWindow must own a LogFilterModel proxy");
        const int filteredRowCount = filterModel->rowCount();
        QVERIFY2(
            filteredRowCount > 0 && filteredRowCount < 300,
            qPrintable(QStringLiteral("active filter must trim row count; got %1").arg(filteredRowCount))
        );

        // Stage 2: replay the saved string filter against the now-enum
        // column. The type-mismatch guard must drop the active rule.
        loglib::LogConfiguration::LogFilter savedFilter;
        savedFilter.type = loglib::LogConfiguration::LogFilter::Type::String;
        savedFilter.row = levelCol;
        savedFilter.filterString = std::string("info");
        savedFilter.matchType = loglib::LogConfiguration::LogFilter::Match::Exactly;

        mWindow->statusBar()->clearMessage();
        // `AddFilter` is private; invoke via the meta-object system.
        const bool invoked = QMetaObject::invokeMethod(
            mWindow,
            "AddFilter",
            Qt::DirectConnection,
            Q_ARG(QString, filterId),
            Q_ARG(std::optional<loglib::LogConfiguration::LogFilter>, savedFilter)
        );
        QVERIFY2(invoked, "AddFilter slot must be invocable via meta-object");
        QCoreApplication::processEvents();

        const QString message = mWindow->statusBar()->currentMessage();
        QVERIFY2(
            message.contains(QStringLiteral("was removed because the column type changed"), Qt::CaseInsensitive),
            qPrintable(QStringLiteral("expected status-bar drop message; got '%1'").arg(message))
        );

        // Active rule and menu entry are gone.
        QVERIFY2(findFilterMenuAction(filterId) == nullptr, "menu entry for the dropped filter must be removed");
        // Unfiltered total: no active rule for `filterId`.
        QCOMPARE(filterModel->rowCount(), 300);

        // Close the empty editor the drop path opens.
        for (QWidget *widget : QApplication::topLevelWidgets())
        {
            if (auto *editor = qobject_cast<FilterEditor *>(widget))
            {
                editor->close();
                editor->deleteLater();
            }
        }
        QCoreApplication::processEvents();

        model->EndStreaming(false);
    }

    // The bitset fast path and the string fallback must accept the same
    // rows on an enum-encoded column.
    void TestEnumFilterRuleFastPathMatchesFallbackPath()
    {
        // 4 levels rotated; selecting 2 gives a 50% pass rate.
        const QStringList levels{
            QStringLiteral("info"),
            QStringLiteral("warn"),
            QStringLiteral("error"),
            QStringLiteral("debug"),
        };
        constexpr int FIXTURE_LINES = 320;
        QStringList lines;
        lines.reserve(FIXTURE_LINES);
        for (int i = 0; i < FIXTURE_LINES; ++i)
        {
            lines.append(QStringLiteral(R"({"level": "%1", "n": %2})").arg(levels[i % levels.size()]).arg(i));
        }
        const TempJsonFile fixture(lines);

        const StreamingRun run = RunStreaming(fixture.Path());
        QCOMPARE(run.finishedCount, 1);
        QCOMPARE(run.cancelled, false);
        QVERIFY(run.model->StreamingErrors().empty());

        const int levelCol = ColumnByHeader(*run.model, QStringLiteral("level"));
        QVERIFY2(levelCol >= 0, "level column must exist");
        const auto &columns = run.model->Configuration().columns;
        QCOMPARE(columns[static_cast<size_t>(levelCol)].type, loglib::LogConfiguration::Type::Enumeration);

        const loglib::EnumDictionary *dictionary = nullptr;
        QVERIFY(!columns[static_cast<size_t>(levelCol)].keys.empty());
        const loglib::KeyId canonicalKeyId =
            run.model->Table().Keys().Find(columns[static_cast<size_t>(levelCol)].keys.front());
        QVERIFY(canonicalKeyId != loglib::INVALID_KEY_ID);
        dictionary = run.model->Table().EnumDictionaries().Find(canonicalKeyId);
        QVERIFY2(dictionary != nullptr, "the enum-encoded column must have a dictionary");

        const std::vector<std::string> selectedHolders = {"info", "warn"};
        std::vector<std::string_view> selectedViews;
        selectedViews.reserve(selectedHolders.size());
        for (const auto &v : selectedHolders)
        {
            selectedViews.emplace_back(v);
        }
        const loglib::EnumRowPredicate fastRule(static_cast<size_t>(levelCol), selectedViews, dictionary);
        const loglib::EnumRowPredicate fallbackRule(
            static_cast<size_t>(levelCol), selectedViews, /*dictionary=*/nullptr
        );

        const int rows = run.model->rowCount();
        QCOMPARE(rows, FIXTURE_LINES);

        int matchedRows = 0;
        const loglib::LogTable &table = run.model->Table();
        for (int row = 0; row < rows; ++row)
        {
            const auto r = static_cast<size_t>(row);
            const bool fast = fastRule.MatchesRow(table, r);
            const bool fallback = fallbackRule.MatchesRow(table, r);

            QVERIFY2(
                fast == fallback,
                qPrintable(QStringLiteral("row %1: fast=%2 vs fallback=%3").arg(row).arg(fast).arg(fallback))
            );
            if (fast)
            {
                ++matchedRows;
            }
        }

        QCOMPARE(matchedRows, FIXTURE_LINES / 2);

        // Every enum-encoded row exposes `EnumValueRole`.
        for (int row = 0; row < rows; ++row)
        {
            const QVariant enumId =
                run.model->data(run.model->index(row, levelCol), LogModelItemDataRole::EnumValueRole);
            QVERIFY2(
                enumId.isValid(),
                qPrintable(QStringLiteral("row %1: EnumValueRole must be valid post-promotion").arg(row))
            );
        }
    }

    // Regression: `LogModel` must emit `enumColumnsChanged` on
    // demote (registry entry erased), not only on dict growth. The
    // post-batch column type is `String`, so the back-fill branch
    // can't detect it -- only the registry-shape loop can.
    void TestEnumColumnsChangedFiresOnDemote()
    {
        auto *model = mWindow->findChild<LogModel *>();
        QVERIFY2(model != nullptr, "MainWindow must own a LogModel");

        // Two batches needed: batch 1 promotes, batch 2 overflows the
        // cap. The streaming flush would collapse small fixtures, so
        // we feed `AppendBatch` directly.
        const TempJsonFile emptyFixture(QStringList{});
        auto file = std::make_unique<loglib::LogFile>(emptyFixture.Path().toStdString());
        auto fileSource = std::make_unique<loglib::FileLineSource>(std::move(file));
        loglib::FileLineSource *sourcePtr = fileSource.get();
        (void)model->BeginStreamingForSyncTest(std::move(fileSource));

        // Tiny cap so the 3rd distinct value demotes to `Type::String`.
        // Set after `BeginStreamingForSyncTest` (which resets the table).
        constexpr uint16_t TEST_CAP = 2;
        model->Table().SetEnumValueCap(TEST_CAP);

        QSignalSpy enumChangedSpy(model, &LogModel::enumColumnsChanged);
        QVERIFY(enumChangedSpy.isValid());

        loglib::KeyIndex &keys = model->Table().Keys();
        const auto makeLine = [&](const std::string &value) {
            std::vector<std::pair<loglib::KeyId, loglib::LogValue>> values;
            values.emplace_back(keys.GetOrInsert("level"), loglib::LogValue(value));
            return loglib::LogLine{std::move(values), keys, *sourcePtr, 0};
        };

        // Batch 1: 2x "info" promotes `level` (well-known key, stream
        // threshold = 2). Dict size 0 -> 1.
        {
            loglib::StreamedBatch batch;
            batch.firstLineNumber = 1;
            batch.newKeys.emplace_back("level");
            batch.lines.push_back(makeLine("info"));
            batch.lines.push_back(makeLine("info"));
            model->AppendBatch(std::move(batch));
        }

        const int levelCol = ColumnByHeader(*model, QStringLiteral("level"));
        QVERIFY2(levelCol >= 0, "level column must exist after promoting batch");
        QCOMPARE(
            model->Configuration().columns[static_cast<size_t>(levelCol)].type,
            loglib::LogConfiguration::Type::Enumeration
        );
        const loglib::KeyId levelKey = keys.Find("level");
        QVERIFY(levelKey != loglib::INVALID_KEY_ID);
        const loglib::EnumDictionary *dict = model->Table().EnumDictionaries().Find(levelKey);
        QVERIFY2(dict != nullptr, "promotion must create a dictionary entry");
        QCOMPARE(static_cast<int>(dict->Size()), 1);

        // Promotion drives `enumColumnsChanged` via the back-fill
        // loop. The reason must be `Promoted` so receivers skip the
        // rank-cache flush (the cache has no entry for this key yet).
        QVERIFY2(enumChangedSpy.count() >= 1, "enumColumnsChanged must fire on promotion");
        bool sawPromotion = false;
        for (const auto &args : enumChangedSpy)
        {
            if (args.at(0).value<EnumColumnsChangeReason>() == EnumColumnsChangeReason::Promoted)
            {
                sawPromotion = true;
                break;
            }
        }
        QVERIFY2(sawPromotion, "promotion batch must emit at least one `Promoted` reason");
        enumChangedSpy.clear();

        // Batch 2: "warn" fits the cap; "error" is the cap+1th value,
        // demoting to `String` and erasing the registry entry. The
        // back-fill enum-type check sees `String` post-batch, so the
        // registry-shape path is the only signal site.
        {
            loglib::StreamedBatch batch;
            batch.firstLineNumber = 3;
            batch.lines.push_back(makeLine("warn"));
            batch.lines.push_back(makeLine("error"));
            model->AppendBatch(std::move(batch));
        }

        QCOMPARE(
            model->Configuration().columns[static_cast<size_t>(levelCol)].type, loglib::LogConfiguration::Type::String
        );
        QVERIFY2(
            model->Table().EnumDictionaries().Find(levelKey) == nullptr, "demotion must erase the dictionary entry"
        );

        QVERIFY2(
            enumChangedSpy.count() >= 1,
            qPrintable(QStringLiteral("expected enumColumnsChanged on demote (registry erase); got %1")
                           .arg(enumChangedSpy.count()))
        );
        bool sawDemote = false;
        for (const auto &args : enumChangedSpy)
        {
            if (args.at(0).value<EnumColumnsChangeReason>() == EnumColumnsChangeReason::Demoted)
            {
                sawDemote = true;
                break;
            }
        }
        QVERIFY2(sawDemote, "demote batch must emit at least one `Demoted` reason");

        model->EndStreaming(false);
    }

    // Regression: a `Grew` `enumColumnsChanged` must NOT flush the
    // `EnumDictRank` cache through `MainWindow`'s slot. `EnumRankFor`
    // self-heals on the next sort via its `DictSize()` check, so
    // flushing on every batch would re-rebuild ranks per batch and
    // dominate the wall-clock of a busy stream.
    void TestEnumRankCacheSurvivesGrowthSignalThroughMainWindow()
    {
        auto *model = mWindow->findChild<LogModel *>();
        QVERIFY2(model != nullptr, "MainWindow must own a LogModel");
        auto *filterModel = mWindow->FilterModel();
        QVERIFY2(filterModel != nullptr, "MainWindow must own a LogFilterModel");

        const TempJsonFile emptyFixture(QStringList{});
        auto file = std::make_unique<loglib::LogFile>(emptyFixture.Path().toStdString());
        auto fileSource = std::make_unique<loglib::FileLineSource>(std::move(file));
        loglib::FileLineSource *sourcePtr = fileSource.get();
        (void)model->BeginStreamingForSyncTest(std::move(fileSource));

        loglib::KeyIndex &keys = model->Table().Keys();
        const auto makeLine = [&](const std::string &value) {
            std::vector<std::pair<loglib::KeyId, loglib::LogValue>> values;
            values.emplace_back(keys.GetOrInsert("level"), loglib::LogValue(value));
            return loglib::LogLine{std::move(values), keys, *sourcePtr, 0};
        };

        // Batch 1 promotes "level" (well-known key; stream threshold = 2).
        {
            loglib::StreamedBatch batch;
            batch.firstLineNumber = 1;
            batch.newKeys.emplace_back("level");
            batch.lines.push_back(makeLine("info"));
            batch.lines.push_back(makeLine("warn"));
            model->AppendBatch(std::move(batch));
        }
        const int levelCol = ColumnByHeader(*model, QStringLiteral("level"));
        QVERIFY2(levelCol >= 0, "level column must exist after promotion");

        // Warm the rank cache by sorting on the enum column.
        filterModel->sort(levelCol, Qt::AscendingOrder);
        QCoreApplication::processEvents();
        QCOMPARE(filterModel->EnumRankCacheSizeForTest(), std::size_t{1});

        // Batch 2 grows the dictionary (new value "error"). Pre-fix
        // the slot called `InvalidateEnumRanks()` on every emit and
        // the cache dropped to 0 entries; post-fix `Grew` is a no-op
        // for the cache so the entry stays.
        {
            loglib::StreamedBatch batch;
            batch.firstLineNumber = 3;
            batch.lines.push_back(makeLine("error"));
            batch.lines.push_back(makeLine("info"));
            model->AppendBatch(std::move(batch));
        }
        QCoreApplication::processEvents();
        QCOMPARE(filterModel->EnumRankCacheSizeForTest(), std::size_t{1});

        model->EndStreaming(false);
    }

    // Regression: a column auto-promoted to `Type::Enumeration` and
    // demoted back to `Type::String` inside the same batch (encode
    // overflows the dict cap) must still emit `Demoted` so consumers
    // can rebuild any rank-cache entry or filter rule that aliased
    // the transient dictionary. Pre-fix the `enumDictSizesBefore`
    // detector only saw columns already enum at snapshot time and
    // missed this transition.
    void TestEnumDemotedSignalFiresOnSameBatchPromoteThenDemote()
    {
        auto *model = mWindow->findChild<LogModel *>();
        QVERIFY2(model != nullptr, "MainWindow must own a LogModel");

        // Tiny dictionary cap: the promotion encode pass will trip it
        // as soon as it sees a third distinct value.
        model->Table().SetEnumValueCap(2);

        const TempJsonFile emptyFixture(QStringList{});
        auto file = std::make_unique<loglib::LogFile>(emptyFixture.Path().toStdString());
        auto fileSource = std::make_unique<loglib::FileLineSource>(std::move(file));
        loglib::FileLineSource *sourcePtr = fileSource.get();
        (void)model->BeginStreamingForSyncTest(std::move(fileSource));

        const QSignalSpy enumChangedSpy(model, &LogModel::enumColumnsChanged);
        QVERIFY(enumChangedSpy.isValid());

        loglib::KeyIndex &keys = model->Table().Keys();
        const auto makeLine = [&](const std::string &value) {
            std::vector<std::pair<loglib::KeyId, loglib::LogValue>> values;
            values.emplace_back(keys.GetOrInsert("level"), loglib::LogValue(value));
            return loglib::LogLine{std::move(values), keys, *sourcePtr, 0};
        };

        // One batch, five rows: the candidate scan early-bails after
        // two "info" rows so well-known-key promotion fires. The
        // `PromoteColumnToEnum` encode pass then trips the cap on the
        // third distinct value, demoting back to `Type::String` --
        // all inside a single `AppendBatch`.
        {
            loglib::StreamedBatch batch;
            batch.firstLineNumber = 1;
            batch.newKeys.emplace_back("level");
            batch.lines.push_back(makeLine("info"));
            batch.lines.push_back(makeLine("info"));
            batch.lines.push_back(makeLine("warn"));
            batch.lines.push_back(makeLine("error"));
            batch.lines.push_back(makeLine("fatal"));
            model->AppendBatch(std::move(batch));
        }
        QCoreApplication::processEvents();

        const int levelCol = ColumnByHeader(*model, QStringLiteral("level"));
        QVERIFY2(levelCol >= 0, "level column must exist post-batch");
        QCOMPARE(
            model->Configuration().columns[static_cast<size_t>(levelCol)].type, loglib::LogConfiguration::Type::String
        );
        const loglib::KeyId levelKey = model->Table().Keys().Find("level");
        QVERIFY2(
            model->Table().EnumDictionaries().Find(levelKey) == nullptr,
            "transient dictionary must have been erased by the in-batch demote"
        );

        bool sawDemote = false;
        for (const auto &args : enumChangedSpy)
        {
            if (args.at(0).value<EnumColumnsChangeReason>() == EnumColumnsChangeReason::Demoted)
            {
                sawDemote = true;
                break;
            }
        }
        QVERIFY2(sawDemote, "same-batch promote+demote must still emit a Demoted enumColumnsChanged");

        model->EndStreaming(false);
    }

    // Regression: after a demote, `MainWindow::enumColumnsChanged`
    // must rebuild every active enum filter so the predicate
    // falls back to its string-set path. A cached
    // "fully-resolved" snapshot would let a stale armed predicate
    // hide every row (post-demote `GetEnumValueId` returns nullopt).
    void TestEnumFilterRebuiltAfterDemote()
    {
        auto *model = mWindow->findChild<LogModel *>();
        QVERIFY2(model != nullptr, "MainWindow must own a LogModel");

        // Two-batch path (mirrors `TestEnumColumnsChangedFiresOnDemote`):
        // batch 2 must trip the cap, so we feed `AppendBatch` directly.
        const TempJsonFile emptyFixture(QStringList{});
        auto file = std::make_unique<loglib::LogFile>(emptyFixture.Path().toStdString());
        auto fileSource = std::make_unique<loglib::FileLineSource>(std::move(file));
        loglib::FileLineSource *sourcePtr = fileSource.get();
        (void)model->BeginStreamingForSyncTest(std::move(fileSource));

        constexpr uint16_t TEST_CAP = 2;
        model->Table().SetEnumValueCap(TEST_CAP);

        loglib::KeyIndex &keys = model->Table().Keys();
        const auto makeLine = [&](const std::string &value) {
            std::vector<std::pair<loglib::KeyId, loglib::LogValue>> values;
            values.emplace_back(keys.GetOrInsert("level"), loglib::LogValue(value));
            return loglib::LogLine{std::move(values), keys, *sourcePtr, 0};
        };

        // Batch 1: two "info" rows promote `level`. Dict size 1;
        // predicate built against it observes `mAllResolved == true`.
        {
            loglib::StreamedBatch batch;
            batch.firstLineNumber = 1;
            batch.newKeys.emplace_back("level");
            batch.lines.push_back(makeLine("info"));
            batch.lines.push_back(makeLine("info"));
            model->AppendBatch(std::move(batch));
        }

        const int levelCol = ColumnByHeader(*model, QStringLiteral("level"));
        QVERIFY2(levelCol >= 0, "level column must exist after promotion");
        QCOMPARE(
            model->Configuration().columns[static_cast<size_t>(levelCol)].type,
            loglib::LogConfiguration::Type::Enumeration
        );

        // Install an enum filter selecting the one resolved value via
        // `FilterEnumSubmitted` (the FilterEditor slot).
        const QString filterId = QStringLiteral("post-demote-rebuild");
        QVERIFY2(
            QMetaObject::invokeMethod(
                mWindow,
                "FilterEnumSubmitted",
                Qt::DirectConnection,
                Q_ARG(QString, filterId),
                Q_ARG(int, levelCol),
                Q_ARG(QStringList, QStringList{QStringLiteral("info")})
            ),
            "FilterEnumSubmitted slot must be invocable via meta-object"
        );
        QCoreApplication::processEvents();

        const LogFilterModel *filterModel = mWindow->FilterModel();
        QVERIFY2(filterModel != nullptr, "MainWindow must own a LogFilterModel proxy");
        QCOMPARE(filterModel->rowCount(), 2); // both info rows visible

        // Batch 2: "warn" fits, "error" overflows -> demote to String.
        // `Find` returns nullptr, `EnumFilterFullyResolved` is false,
        // and the slot rebuilds the predicate.
        {
            loglib::StreamedBatch batch;
            batch.firstLineNumber = 3;
            batch.lines.push_back(makeLine("warn"));
            batch.lines.push_back(makeLine("error"));
            model->AppendBatch(std::move(batch));
        }
        QCoreApplication::processEvents();

        QCOMPARE(
            model->Configuration().columns[static_cast<size_t>(levelCol)].type, loglib::LogConfiguration::Type::String
        );

        // Rebuilt predicate has no dictionary, so the string-set
        // fallback runs against the demoted column's string slots:
        // both "info" rows match; "warn" / "error" do not.
        QCOMPARE(filterModel->rowCount(), 2);

        // Confirm the visible rows are the "info" ones.
        const int levelColInProxy = levelCol; // proxy preserves column layout
        for (int i = 0; i < filterModel->rowCount(); ++i)
        {
            const QModelIndex idx = filterModel->index(i, levelColInProxy);
            QCOMPARE(idx.data(Qt::DisplayRole).toString(), QStringLiteral("info"));
        }

        model->EndStreaming(false);
    }

    // Regression: the `EnumDictRank` cache is keyed by canonical
    // `KeyId`, not column index, so a column reorder no longer
    // requires a `columnsMoved` invalidation hook -- the cached rank
    // stays attached to the same logical column.
    void TestEnumRankCacheSurvivesColumnsMoved()
    {
        auto *model = mWindow->findChild<LogModel *>();
        QVERIFY2(model != nullptr, "MainWindow must own a LogModel");

        // Two-column fixture where "level" promotes to enum (one
        // batch -- 200 lines is below the streaming threshold).
        const QStringList levels{
            QStringLiteral("info"), QStringLiteral("warn"), QStringLiteral("error"), QStringLiteral("debug")
        };
        QStringList lines;
        lines.reserve(200);
        for (int i = 0; i < 200; ++i)
        {
            lines.append(QStringLiteral(R"({"level": "%1", "msg": "m%2"})").arg(levels[i % levels.size()]).arg(i));
        }
        const TempJsonFile fixture(lines);

        QSignalSpy finishedSpy(model, &LogModel::streamingFinished);
        QVERIFY(finishedSpy.isValid());

        auto file = std::make_unique<loglib::LogFile>(fixture.Path().toStdString());
        auto fileSource = std::make_unique<loglib::FileLineSource>(std::move(file));
        loglib::FileLineSource *fileSourcePtr = fileSource.get();
        const loglib::StopToken stopToken = model->BeginStreamingForSyncTest(std::move(fileSource));

        loglib::ParserOptions options;
        options.stopToken = stopToken;
        loglib::internal::AdvancedParserOptions advanced;
        advanced.threads = 1;
        const loglib::JsonParser parser;
        loglib::JsonParser::ParseStreaming(*fileSourcePtr, *model->Sink(), options, advanced);
        QVERIFY2(finishedSpy.count() > 0 || finishedSpy.wait(5000), "streamingFinished must arrive");

        const int levelCol = ColumnByHeader(*model, QStringLiteral("level"));
        QVERIFY2(levelCol >= 0, "level column must exist");
        QCOMPARE(
            model->Configuration().columns[static_cast<size_t>(levelCol)].type,
            loglib::LogConfiguration::Type::Enumeration
        );

        LogFilterModel *filterModel = mWindow->FilterModel();
        QVERIFY2(filterModel != nullptr, "MainWindow must own a LogFilterModel proxy");

        // Warm the rank cache: sort by the enum column.
        filterModel->sort(levelCol, Qt::AscendingOrder);
        QCoreApplication::processEvents();
        const std::size_t cacheSizeBefore = filterModel->EnumRankCacheSizeForTest();
        QVERIFY2(cacheSizeBefore >= std::size_t{1}, "sorting by an enum column must populate the rank cache");

        // Reorder columns. Pre-fix a `columnsMoved` hook dropped the
        // cache; post-fix it survives (KeyId-keyed).
        QVERIFY2(model->columnCount() >= 2, "fixture must have at least two columns");
        const QSignalSpy columnsMovedSpy(model, &QAbstractItemModel::columnsMoved);
        // Also pin the proxy-level forwarding contract: pre-fix
        // `LogFilterModel` swallowed the `columnsMoved` pair (only
        // re-emitted `headerDataChanged`), which left downstream
        // views with stale column metadata when a streaming session
        // late-promoted a `Type::Time` column. Post-fix the proxy
        // forwards begin/endMoveColumns through to the view.
        const QSignalSpy filterColumnsAboutToBeMovedSpy(filterModel, &QAbstractItemModel::columnsAboutToBeMoved);
        const QSignalSpy filterColumnsMovedSpy(filterModel, &QAbstractItemModel::columnsMoved);
        QVERIFY(columnsMovedSpy.isValid());
        QVERIFY(filterColumnsAboutToBeMovedSpy.isValid());
        QVERIFY(filterColumnsMovedSpy.isValid());
        const int src = (levelCol == 0) ? 1 : 0;
        const int dest = (levelCol == 0) ? 0 : 1;
        QVERIFY2(model->MoveColumnForTest(src, dest), "MoveColumnForTest must succeed");
        QCoreApplication::processEvents();
        QVERIFY2(columnsMovedSpy.count() >= 1, "MoveColumnForTest must emit columnsMoved");
        QVERIFY2(
            filterColumnsAboutToBeMovedSpy.count() >= 1,
            "LogFilterModel must forward columnsAboutToBeMoved from its source"
        );
        QVERIFY2(filterColumnsMovedSpy.count() >= 1, "LogFilterModel must forward columnsMoved from its source");

        // Cache entry survives the reorder (KeyId-keyed).
        QCOMPARE(filterModel->EnumRankCacheSizeForTest(), cacheSizeBefore);

        // Re-sort against the column's new position. The proxy
        // tracks the sort column in source-coords, so a reorder
        // shifts what that index points at -- we re-issue `sort()`.
        // The lookup hits the cache (KeyId match), no rebuild.
        const int levelColAfter = ColumnByHeader(*model, QStringLiteral("level"));
        QVERIFY2(levelColAfter >= 0, "level column must still exist after the move");
        filterModel->sort(levelColAfter, Qt::AscendingOrder);
        QCoreApplication::processEvents();
        QCOMPARE(filterModel->EnumRankCacheSizeForTest(), cacheSizeBefore);

        QStringList sortedLevelsAfter;
        sortedLevelsAfter.reserve(filterModel->rowCount());
        for (int i = 0; i < filterModel->rowCount(); ++i)
        {
            sortedLevelsAfter.append(filterModel->index(i, levelColAfter).data(Qt::DisplayRole).toString());
        }
        // Ascending alphabetical: debug < error < info < warn (50 of each).
        QCOMPARE(sortedLevelsAfter.front(), QStringLiteral("debug"));
        QCOMPARE(sortedLevelsAfter.back(), QStringLiteral("warn"));
        for (int i = 1; i < sortedLevelsAfter.size(); ++i)
        {
            QVERIFY2(
                sortedLevelsAfter[i - 1] <= sortedLevelsAfter[i],
                qPrintable(QStringLiteral("rows out of order: %1 > %2 at index %3")
                               .arg(sortedLevelsAfter[i - 1])
                               .arg(sortedLevelsAfter[i])
                               .arg(i))
            );
        }

        model->EndStreaming(false);
    }

    // Regression: `EnumRankFor` self-heals when the live dictionary
    // has grown or its address changed since the cached rank was
    // built. Production also has a `MainWindow` invalidation tick;
    // this test wires the proxy directly (no `MainWindow`) so the
    // self-heal is the only defence.
    void TestEnumRankCacheSelfHealsOnDictionaryGrowth()
    {
        LogModel model;
        LogFilterModel filterModel;
        filterModel.setSourceModel(&model);
        filterModel.SetLogModel(&model);

        // Direct `AppendBatch` driving. Stream mode needs
        // `BeginStreaming` first; an empty fixture satisfies it.
        const TempJsonFile emptyFixture(QStringList{});
        auto file = std::make_unique<loglib::LogFile>(emptyFixture.Path().toStdString());
        auto fileSource = std::make_unique<loglib::FileLineSource>(std::move(file));
        loglib::FileLineSource *sourcePtr = fileSource.get();
        (void)model.BeginStreamingForSyncTest(std::move(fileSource));

        loglib::KeyIndex &keys = model.Table().Keys();
        const auto makeLine = [&](const std::string &value) {
            std::vector<std::pair<loglib::KeyId, loglib::LogValue>> values;
            values.emplace_back(keys.GetOrInsert("level"), loglib::LogValue(value));
            return loglib::LogLine{std::move(values), keys, *sourcePtr, 0};
        };

        // Batch 1: dict_v1 = {"info" -> 0, "warn" -> 1}.
        {
            loglib::StreamedBatch batch;
            batch.firstLineNumber = 1;
            batch.newKeys.emplace_back("level");
            batch.lines.push_back(makeLine("info"));
            batch.lines.push_back(makeLine("warn"));
            model.AppendBatch(std::move(batch));
        }

        const int levelCol = ColumnByHeader(model, QStringLiteral("level"));
        QVERIFY2(levelCol >= 0, "level column must exist after promotion");
        QCOMPARE(
            model.Configuration().columns[static_cast<size_t>(levelCol)].type,
            loglib::LogConfiguration::Type::Enumeration
        );

        // Warm the cache.
        filterModel.sort(levelCol, Qt::AscendingOrder);
        QVERIFY2(filterModel.EnumRankCacheSizeForTest() == std::size_t{1}, "first sort must populate the rank cache");

        // Snapshot pre-growth order. Without the self-heal, ids past
        // the cached bitset all collide at the past-the-end rank.
        QStringList orderedBeforeGrowth;
        orderedBeforeGrowth.reserve(filterModel.rowCount());
        for (int i = 0; i < filterModel.rowCount(); ++i)
        {
            orderedBeforeGrowth.append(filterModel.index(i, levelCol).data(Qt::DisplayRole).toString());
        }
        QCOMPARE(orderedBeforeGrowth, (QStringList{QStringLiteral("info"), QStringLiteral("warn")}));

        // Batch 2: dictionary grows from 2 to 4 entries; cached rank
        // is now too small.
        {
            loglib::StreamedBatch batch;
            batch.firstLineNumber = 3;
            batch.lines.push_back(makeLine("error"));
            batch.lines.push_back(makeLine("debug"));
            model.AppendBatch(std::move(batch));
        }
        const loglib::KeyId levelKey = keys.Find("level");
        QVERIFY(levelKey != loglib::INVALID_KEY_ID);
        const loglib::EnumDictionary *dict = model.Table().EnumDictionaries().Find(levelKey);
        QVERIFY2(dict != nullptr, "level dictionary must still exist after batch 2");
        QCOMPARE(static_cast<int>(dict->Size()), 4);

        // Re-sort. `rank.DictSize() < live.Size()` trips and
        // `EnumRankFor` rebuilds; the cache stays at one row (same
        // `KeyId`) but its `mIdToRank` now covers all 4 ids.
        filterModel.sort(levelCol, Qt::AscendingOrder);
        QCOMPARE(filterModel.EnumRankCacheSizeForTest(), std::size_t{1});

        QStringList orderedAfterGrowth;
        orderedAfterGrowth.reserve(filterModel.rowCount());
        for (int i = 0; i < filterModel.rowCount(); ++i)
        {
            orderedAfterGrowth.append(filterModel.index(i, levelCol).data(Qt::DisplayRole).toString());
        }
        // Alphabetical: debug < error < info < warn. Without the
        // self-heal, "debug" / "error" would tie at past-the-end and
        // appear at the tail in source order.
        QCOMPARE(
            orderedAfterGrowth,
            (QStringList{
                QStringLiteral("debug"), QStringLiteral("error"), QStringLiteral("info"), QStringLiteral("warn")
            })
        );

        model.EndStreaming(false);
    }

    // Regression: `setSourceModel` must wipe filter state before the
    // base re-filter, otherwise a predicate baked against the old
    // chain's dictionary aliases unrelated bytes in the new chain
    // (and the stale `mLogModel` rejects every row).
    void TestSetSourceModelClearsFilterState()
    {
        // Two independent fixtures with distinct dictionaries.
        LogModel modelA;
        LogModel modelB;
        const auto installSession = [&](LogModel &model, const std::string &fixtureName) {
            const TempJsonFile emptyFixture(QStringList{});
            auto file = std::make_unique<loglib::LogFile>(emptyFixture.Path().toStdString());
            auto fileSource = std::make_unique<loglib::FileLineSource>(std::move(file));
            loglib::FileLineSource *sourcePtr = fileSource.get();
            (void)model.BeginStreamingForSyncTest(std::move(fileSource));

            loglib::KeyIndex &keys = model.Table().Keys();
            const auto makeLine = [&](const std::string &value) {
                std::vector<std::pair<loglib::KeyId, loglib::LogValue>> values;
                values.emplace_back(keys.GetOrInsert("level"), loglib::LogValue(value));
                return loglib::LogLine{std::move(values), keys, *sourcePtr, 0};
            };

            loglib::StreamedBatch batch;
            batch.firstLineNumber = 1;
            batch.newKeys.emplace_back("level");
            // Same shape on both sides; dictionary instances differ.
            batch.lines.push_back(makeLine("info"));
            batch.lines.push_back(makeLine("warn"));
            batch.lines.push_back(makeLine("info"));
            batch.lines.push_back(makeLine("warn"));
            model.AppendBatch(std::move(batch));
            Q_UNUSED(fixtureName);
        };
        installSession(modelA, "A");
        installSession(modelB, "B");
        QCOMPARE(modelA.rowCount(), 4);
        QCOMPARE(modelB.rowCount(), 4);

        LogFilterModel proxy;
        proxy.setSourceModel(&modelA);
        proxy.SetLogModel(&modelA);

        const int levelColA = ColumnByHeader(modelA, QStringLiteral("level"));
        QVERIFY(levelColA >= 0);
        const loglib::KeyId keyA =
            modelA.Table().Keys().Find(modelA.Configuration().columns[static_cast<size_t>(levelColA)].keys.front());
        const loglib::EnumDictionary *dictA = modelA.Table().EnumDictionaries().Find(keyA);
        QVERIFY(dictA != nullptr);

        // Filter on `A`: keep only "info" rows.
        {
            const std::vector<std::string> selected = {"info"};
            std::vector<std::string_view> selectedViews;
            selectedViews.reserve(selected.size());
            for (const auto &v : selected)
            {
                selectedViews.emplace_back(v);
            }
            std::vector<loglib::RowPredicate> rules;
            rules.emplace_back(
                std::in_place_type<loglib::EnumRowPredicate>,
                static_cast<size_t>(levelColA),
                std::span<const std::string_view>(selectedViews),
                dictA
            );
            proxy.SetFilterRules(std::move(rules));
        }
        QCOMPARE(proxy.rowCount(), 2);
        // Warm the cache so we can assert it's cleared on swap.
        proxy.sort(levelColA, Qt::AscendingOrder);
        QCOMPARE(proxy.EnumRankCacheSizeForTest(), std::size_t{1});

        // Swap to `modelB` without re-issuing `SetLogModel`. Filter
        // state must be wiped; the empty rule list defaults to
        // accepting every `B` row (safe "not yet rewired" default).
        proxy.setSourceModel(&modelB);
        QCOMPARE(proxy.EnumRankCacheSizeForTest(), std::size_t{0});
        QCOMPARE(proxy.rowCount(), modelB.rowCount());

        // Re-wire for `B`. Survivors come from `B`'s rows (2 "info").
        proxy.SetLogModel(&modelB);
        const int levelColB = ColumnByHeader(modelB, QStringLiteral("level"));
        QVERIFY(levelColB >= 0);
        const loglib::KeyId keyB =
            modelB.Table().Keys().Find(modelB.Configuration().columns[static_cast<size_t>(levelColB)].keys.front());
        const loglib::EnumDictionary *dictB = modelB.Table().EnumDictionaries().Find(keyB);
        QVERIFY(dictB != nullptr);
        QVERIFY2(dictB != dictA, "fixtures must have distinct dictionary instances");

        const std::vector<std::string> selected = {"info"};
        std::vector<std::string_view> selectedViews;
        selectedViews.reserve(selected.size());
        for (const auto &v : selected)
        {
            selectedViews.emplace_back(v);
        }
        std::vector<loglib::RowPredicate> rulesB;
        rulesB.emplace_back(
            std::in_place_type<loglib::EnumRowPredicate>,
            static_cast<size_t>(levelColB),
            std::span<const std::string_view>(selectedViews),
            dictB
        );
        proxy.SetFilterRules(std::move(rulesB));
        QCOMPARE(proxy.rowCount(), 2);

        modelA.EndStreaming(false);
        modelB.EndStreaming(false);
    }

#ifdef QT_NO_DEBUG
    // Regression (release-only): with rules installed but
    // `mLogModel` null, `filterAcceptsRow` rejects every row instead
    // of silently accepting all. Debug builds trip `Q_ASSERT_X` and
    // never reach this branch.
    void TestFilterAcceptsRowRejectsAllWithoutLogModel()
    {
        LogModel model;
        {
            const TempJsonFile emptyFixture(QStringList{});
            auto file = std::make_unique<loglib::LogFile>(emptyFixture.Path().toStdString());
            auto fileSource = std::make_unique<loglib::FileLineSource>(std::move(file));
            loglib::FileLineSource *sourcePtr = fileSource.get();
            (void)model.BeginStreamingForSyncTest(std::move(fileSource));

            loglib::KeyIndex &keys = model.Table().Keys();
            const auto makeLine = [&](const std::string &value) {
                std::vector<std::pair<loglib::KeyId, loglib::LogValue>> values;
                values.emplace_back(keys.GetOrInsert("level"), loglib::LogValue(value));
                return loglib::LogLine{std::move(values), keys, *sourcePtr, 0};
            };
            loglib::StreamedBatch batch;
            batch.firstLineNumber = 1;
            batch.newKeys.emplace_back("level");
            batch.lines.push_back(makeLine("info"));
            batch.lines.push_back(makeLine("warn"));
            model.AppendBatch(std::move(batch));
        }
        QCOMPARE(model.rowCount(), 2);

        LogFilterModel proxy;
        proxy.setSourceModel(&model);
        // Skip `SetLogModel` on purpose: rule installation in this
        // state must be rejected at evaluation time.
        const int levelCol = ColumnByHeader(model, QStringLiteral("level"));
        QVERIFY(levelCol >= 0);
        const std::vector<std::string> selected = {"info"};
        std::vector<std::string_view> selectedViews;
        selectedViews.reserve(selected.size());
        for (const auto &v : selected)
        {
            selectedViews.emplace_back(v);
        }
        std::vector<loglib::RowPredicate> rules;
        rules.emplace_back(
            std::in_place_type<loglib::EnumRowPredicate>,
            static_cast<size_t>(levelCol),
            std::span<const std::string_view>(selectedViews),
            /*dictionary=*/nullptr
        );
        proxy.SetFilterRules(std::move(rules));

        QCOMPARE(proxy.rowCount(), 0);

        model.EndStreaming(false);
    }
#endif

    // Regression: invalid regex submitted via `FilterSubmitted` is
    // rejected up front (status-bar error, existing filters kept)
    // instead of silently hiding every row.
    void TestRegexFilterSubmissionRejectsInvalid()
    {
        // Fixture: a few rows so we can verify "no filter applied".
        const TempJsonFile fixture(MakeParityFixture());

        auto *model = mWindow->findChild<LogModel *>();
        QVERIFY2(model != nullptr, "MainWindow must own a LogModel");

        QSignalSpy finishedSpy(model, &LogModel::streamingFinished);
        QVERIFY(finishedSpy.isValid());

        auto file = std::make_unique<loglib::LogFile>(fixture.Path().toStdString());
        auto fileSource = std::make_unique<loglib::FileLineSource>(std::move(file));
        loglib::FileLineSource *fileSourcePtr = fileSource.get();
        const loglib::StopToken stopToken = model->BeginStreamingForSyncTest(std::move(fileSource));

        loglib::ParserOptions options;
        options.stopToken = stopToken;
        loglib::internal::AdvancedParserOptions advanced;
        advanced.threads = 1;
        const loglib::JsonParser parser;
        loglib::JsonParser::ParseStreaming(*fileSourcePtr, *model->Sink(), options, advanced);
        QVERIFY2(finishedSpy.count() > 0 || finishedSpy.wait(5000), "streamingFinished must arrive");
        const int totalRows = model->rowCount();
        QVERIFY2(totalRows > 0, "fixture must produce at least one row");

        const LogFilterModel *filterModel = mWindow->FilterModel();
        QVERIFY2(filterModel != nullptr, "MainWindow must own a LogFilterModel proxy");

        mWindow->statusBar()->clearMessage();

        // `*[invalid` is a syntactically-broken regex (quantifier with
        // no operand, unbalanced bracket). `QRegularExpression(pattern).isValid()`
        // returns false; pre-fix this was passed straight into the
        // matcher.
        const QString filterId = QStringLiteral("invalid-regex");
        const int column = 0;
        QVERIFY2(
            QMetaObject::invokeMethod(
                mWindow,
                "FilterSubmitted",
                Qt::DirectConnection,
                Q_ARG(QString, filterId),
                Q_ARG(int, column),
                Q_ARG(QString, QStringLiteral("*[invalid")),
                Q_ARG(int, static_cast<int>(loglib::LogConfiguration::LogFilter::Match::RegularExpression))
            ),
            "FilterSubmitted slot must be invocable via meta-object"
        );
        QCoreApplication::processEvents();

        // The rule must be rejected: every row stays visible.
        QCOMPARE(filterModel->rowCount(), totalRows);

        const QString msg = mWindow->statusBar()->currentMessage();
        QVERIFY2(
            msg.contains(QStringLiteral("Invalid regular expression"), Qt::CaseInsensitive),
            qPrintable(QStringLiteral("status bar must explain the rejection; got '%1'").arg(msg))
        );

        // No filter menu entry should have been created.
        const auto filterActions = mWindow->findChildren<QAction *>();
        for (const QAction *action : filterActions)
        {
            QVERIFY2(
                action->data().toString() != filterId,
                qPrintable(QStringLiteral("filter '%1' must not appear in the menu after rejection").arg(filterId))
            );
        }

        model->EndStreaming(false);
    }

    // Regression: string filters now match the user-visible (one-line,
    // simplified) text. Pre-fix the matcher saw raw bytes with
    // embedded `\n`, so a `Contains` typed as the displayed text
    // ("line1 line2") silently rejected the row. All four match
    // modes flowed through the same broken path.
    void TestStringFilterMatchesDisplayedTextForMultilineValues()
    {
        // Two rows: one matches, one doesn't, so we know the filter
        // is doing work (not just accepting everything).
        const QStringList lines{
            QStringLiteral(R"({"msg": "line1\nline2"})"),
            QStringLiteral(R"({"msg": "other"})"),
        };
        const TempJsonFile fixture(lines);

        auto *model = mWindow->findChild<LogModel *>();
        QVERIFY2(model != nullptr, "MainWindow must own a LogModel");

        QSignalSpy finishedSpy(model, &LogModel::streamingFinished);
        QVERIFY(finishedSpy.isValid());

        auto file = std::make_unique<loglib::LogFile>(fixture.Path().toStdString());
        auto fileSource = std::make_unique<loglib::FileLineSource>(std::move(file));
        loglib::FileLineSource *fileSourcePtr = fileSource.get();
        const loglib::StopToken stopToken = model->BeginStreamingForSyncTest(std::move(fileSource));

        loglib::ParserOptions options;
        options.stopToken = stopToken;
        loglib::internal::AdvancedParserOptions advanced;
        advanced.threads = 1;
        const loglib::JsonParser parser;
        loglib::JsonParser::ParseStreaming(*fileSourcePtr, *model->Sink(), options, advanced);
        QVERIFY2(finishedSpy.count() > 0 || finishedSpy.wait(5000), "streamingFinished must arrive");
        QCOMPARE(model->rowCount(), 2);

        const int msgCol = ColumnByHeader(*model, QStringLiteral("msg"));
        QVERIFY2(msgCol >= 0, "msg column must exist");

        // Pin the bug's precondition: stored value still has the
        // real newline. `SortRole` would already simplify it, so we
        // pull raw bytes via `LogTable::GetValue`.
        const loglib::LogValue raw = model->Table().GetValue(0, static_cast<size_t>(msgCol));
        QVERIFY2(
            std::holds_alternative<std::string_view>(raw) || std::holds_alternative<std::string>(raw),
            "fixture row must store the msg as a string slot so the matcher hits the string branch"
        );
        const std::string_view rawView = std::holds_alternative<std::string_view>(raw)
                                             ? std::get<std::string_view>(raw)
                                             : std::string_view(std::get<std::string>(raw));
        QVERIFY2(rawView.contains('\n'), "fixture must keep the embedded newline so the bug's precondition holds");

        // Displayed text is the simplified one-line form.
        const QString displayed = model->index(0, msgCol).data(Qt::DisplayRole).toString();
        QCOMPARE(displayed, QStringLiteral("line1 line2"));

        const LogFilterModel *filterModel = mWindow->FilterModel();
        QVERIFY2(filterModel != nullptr, "MainWindow must own a LogFilterModel proxy");

        const auto submitFilter =
            [&](const QString &filterId, const QString &pattern, loglib::LogConfiguration::LogFilter::Match match) {
                QVERIFY2(
                    QMetaObject::invokeMethod(
                        mWindow,
                        "FilterSubmitted",
                        Qt::DirectConnection,
                        Q_ARG(QString, filterId),
                        Q_ARG(int, msgCol),
                        Q_ARG(QString, pattern),
                        Q_ARG(int, static_cast<int>(match))
                    ),
                    "FilterSubmitted slot must be invocable via meta-object"
                );
                QCoreApplication::processEvents();
            };

        const auto clearFilter = [&](const QString &filterId) {
            QVERIFY2(
                QMetaObject::invokeMethod(mWindow, "ClearFilter", Qt::DirectConnection, Q_ARG(QString, filterId)),
                "ClearFilter slot must be invocable via meta-object"
            );
            QCoreApplication::processEvents();
            QCOMPARE(filterModel->rowCount(), 2);
        };

        // Contains: needle matches the displayed (simplified) text.
        submitFilter(
            QStringLiteral("contains-displayed"),
            QStringLiteral("line1 line2"),
            loglib::LogConfiguration::LogFilter::Match::Contains
        );
        QCOMPARE(filterModel->rowCount(), 1);
        clearFilter(QStringLiteral("contains-displayed"));

        // Exactly: full simplified value.
        submitFilter(
            QStringLiteral("exact-displayed"),
            QStringLiteral("line1 line2"),
            loglib::LogConfiguration::LogFilter::Match::Exactly
        );
        QCOMPARE(filterModel->rowCount(), 1);
        clearFilter(QStringLiteral("exact-displayed"));

        // RegularExpression: `^line1 line2$` only spans the simplified
        // form -- `^...$` without `MultilineOption` won't span a `\n`.
        submitFilter(
            QStringLiteral("regex-displayed"),
            QStringLiteral("^line1 line2$"),
            loglib::LogConfiguration::LogFilter::Match::RegularExpression
        );
        QCOMPARE(filterModel->rowCount(), 1);
        clearFilter(QStringLiteral("regex-displayed"));

        // Wildcard: Qt-flavoured `line1 *2` only matches the
        // simplified single-line form.
        submitFilter(
            QStringLiteral("wildcard-displayed"),
            QStringLiteral("line1 *2"),
            loglib::LogConfiguration::LogFilter::Match::Wildcard
        );
        QCOMPARE(filterModel->rowCount(), 1);
        clearFilter(QStringLiteral("wildcard-displayed"));

        model->EndStreaming(false);
    }

    // Regression: `LogFilterModel::MatchRow` formats cells via
    // `LogTable::GetFormattedValue` + `ConvertToSingleLineCompactQString`,
    // skipping the `data()` -> `QVariant<QString>` round-trip.
    // Match results must stay identical to the slow path.
    void TestFindUsesFormattedValueFastPath()
    {
        const QStringList levels{
            QStringLiteral("info"), QStringLiteral("warn"), QStringLiteral("error"), QStringLiteral("debug")
        };
        QStringList lines;
        lines.reserve(40);
        for (int i = 0; i < 40; ++i)
        {
            lines.append(QStringLiteral(R"({"level": "%1", "msg": "row%2"})").arg(levels[i % levels.size()]).arg(i));
        }
        const TempJsonFile fixture(lines);

        auto *model = mWindow->findChild<LogModel *>();
        QVERIFY2(model != nullptr, "MainWindow must own a LogModel");

        QSignalSpy finishedSpy(model, &LogModel::streamingFinished);
        QVERIFY(finishedSpy.isValid());

        auto file = std::make_unique<loglib::LogFile>(fixture.Path().toStdString());
        auto fileSource = std::make_unique<loglib::FileLineSource>(std::move(file));
        loglib::FileLineSource *fileSourcePtr = fileSource.get();
        const loglib::StopToken stopToken = model->BeginStreamingForSyncTest(std::move(fileSource));

        loglib::ParserOptions options;
        options.stopToken = stopToken;
        loglib::internal::AdvancedParserOptions advanced;
        advanced.threads = 1;
        const loglib::JsonParser parser;
        loglib::JsonParser::ParseStreaming(*fileSourcePtr, *model->Sink(), options, advanced);
        QVERIFY2(finishedSpy.count() > 0 || finishedSpy.wait(5000), "streamingFinished must arrive");
        QCOMPARE(model->rowCount(), 40);

        const LogFilterModel *filterModel = mWindow->FilterModel();
        QVERIFY2(filterModel != nullptr, "MainWindow must own a LogFilterModel proxy");

        // Walk the table the same way Find does (every column,
        // `MatchContains | MatchWrap`) to compute expected `info` rows.
        const int rowCount = filterModel->rowCount();
        const int columnCount = filterModel->columnCount();
        QVERIFY(rowCount > 0 && columnCount > 0);

        QList<int> expectedRows;
        for (int row = 0; row < rowCount; ++row)
        {
            for (int col = 0; col < columnCount; ++col)
            {
                const QString cell = filterModel->index(row, col).data(Qt::DisplayRole).toString();
                if (cell.contains(QStringLiteral("info")))
                {
                    expectedRows.append(row);
                    break;
                }
            }
        }
        QVERIFY2(!expectedRows.isEmpty(), "fixture must contain at least one 'info' match");

        // Drive `MatchRow` with `Qt::DisplayRole` (fast-path branch).
        // Results must match the `data()`-walking baseline.
        const QModelIndex start = filterModel->index(0, 0);
        const QList<QModelIndex> hits = filterModel->MatchRow(
            start,
            Qt::DisplayRole,
            QStringLiteral("info"),
            LogFilterModel::UNLIMITED_HITS,
            Qt::MatchContains | Qt::MatchWrap,
            true,
            0
        );

        QList<int> actualRows;
        actualRows.reserve(hits.size());
        for (const QModelIndex &idx : hits)
        {
            actualRows.append(idx.row());
        }
        std::ranges::sort(actualRows);

        QList<int> expectedSorted = expectedRows;
        std::ranges::sort(expectedSorted);
        QCOMPARE(actualRows, expectedSorted);

        model->EndStreaming(false);
    }

    // Regression: `FindRecords` previously routed the proxy
    // `currentIndex()` through `mModel->index(...)`, mixing proxy
    // and source coords. The bug was latent (MatchRow only reads
    // `.row()` / `.parent()`), so this test pins the production
    // wiring under an active filter.
    void TestFindRespectsActiveFilter()
    {
        // 12 rows alternating info/warn -> filtered to 6 visible info rows.
        const QStringList levels{QStringLiteral("info"), QStringLiteral("warn")};
        constexpr int FIXTURE_LINES = 12;
        QStringList lines;
        lines.reserve(FIXTURE_LINES);
        for (int i = 0; i < FIXTURE_LINES; ++i)
        {
            lines.append(QStringLiteral(R"({"level": "%1", "msg": "row%2"})").arg(levels[i % levels.size()]).arg(i));
        }
        const TempJsonFile fixture(lines);

        auto *model = mWindow->findChild<LogModel *>();
        QVERIFY2(model != nullptr, "MainWindow must own a LogModel");

        QSignalSpy finishedSpy(model, &LogModel::streamingFinished);
        QVERIFY(finishedSpy.isValid());

        auto file = std::make_unique<loglib::LogFile>(fixture.Path().toStdString());
        auto fileSource = std::make_unique<loglib::FileLineSource>(std::move(file));
        loglib::FileLineSource *fileSourcePtr = fileSource.get();
        const loglib::StopToken stopToken = model->BeginStreamingForSyncTest(std::move(fileSource));

        loglib::ParserOptions options;
        options.stopToken = stopToken;
        loglib::internal::AdvancedParserOptions advanced;
        advanced.threads = 1;
        const loglib::JsonParser parser;
        loglib::JsonParser::ParseStreaming(*fileSourcePtr, *model->Sink(), options, advanced);
        QVERIFY2(finishedSpy.count() > 0 || finishedSpy.wait(5000), "streamingFinished must arrive");
        QCOMPARE(model->rowCount(), FIXTURE_LINES);

        const int levelCol = ColumnByHeader(*model, QStringLiteral("level"));
        QVERIFY2(levelCol >= 0, "level column must exist");

        // Install an enum filter selecting only "info" -> 6 visible rows.
        const QString filterId = QStringLiteral("find-with-filter");
        QVERIFY2(
            QMetaObject::invokeMethod(
                mWindow,
                "FilterEnumSubmitted",
                Qt::DirectConnection,
                Q_ARG(QString, filterId),
                Q_ARG(int, levelCol),
                Q_ARG(QStringList, QStringList{QStringLiteral("info")})
            ),
            "FilterEnumSubmitted slot must be invocable via meta-object"
        );
        QCoreApplication::processEvents();

        LogFilterModel *filterModel = mWindow->FilterModel();
        QVERIFY2(filterModel != nullptr, "MainWindow must own a LogFilterModel proxy");
        QCOMPARE(filterModel->rowCount(), FIXTURE_LINES / 2);

        // The table view's selection model is what `Find` reads
        // to pick the start index.
        auto *tableView = mWindow->findChild<LogTableView *>();
        QVERIFY2(tableView != nullptr, "MainWindow must own a LogTableView");
        QCOMPARE(tableView->model(), filterModel);

        // Current index at proxy row 1 -> first forward hit
        // (skipping the selected row) lands at proxy row 2.
        const int startProxyRow = 1;
        const QModelIndex startProxyIdx = filterModel->index(startProxyRow, 0);
        QVERIFY(startProxyIdx.isValid());
        tableView->selectionModel()->setCurrentIndex(
            startProxyIdx, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows
        );
        QVERIFY(tableView->selectionModel()->isRowSelected(startProxyRow));

        // `MatchRow` always rebuilds through `this->index(...)`, so the
        // strongest invariant we can pin is "result row < proxy
        // rowCount and the cell matches the needle".
        QVERIFY(model->rowCount() > filterModel->rowCount());

        // Drive `FindRecords` (next=true, no wildcard / regex).
        QVERIFY2(
            QMetaObject::invokeMethod(
                mWindow,
                "FindRecords",
                Qt::DirectConnection,
                Q_ARG(QString, QStringLiteral("info")),
                Q_ARG(bool, true),
                Q_ARG(bool, false),
                Q_ARG(bool, false)
            ),
            "FindRecords slot must be invocable via meta-object"
        );
        QCoreApplication::processEvents();

        // Selection moved to the next visible info row.
        const QModelIndex selected = tableView->selectionModel()->currentIndex();
        QVERIFY2(selected.isValid(), "FindRecords must move the current index to a match");
        QCOMPARE(selected.model(), filterModel);
        QVERIFY2(
            selected.row() >= 0 && selected.row() < filterModel->rowCount(),
            qPrintable(QStringLiteral("selected row %1 must be inside the filtered proxy [0, %2)")
                           .arg(selected.row())
                           .arg(filterModel->rowCount()))
        );
        QVERIFY2(
            selected.row() != startProxyRow, "next-match must advance past the selected start row when `skipFirstN==1`"
        );

        const QString cellText = filterModel->index(selected.row(), levelCol).data(Qt::DisplayRole).toString();
        QCOMPARE(cellText, QStringLiteral("info"));

        // Cross-check: filter is active and Find stayed inside it.
        for (int r = 0; r < filterModel->rowCount(); ++r)
        {
            const QString v = filterModel->index(r, levelCol).data(Qt::DisplayRole).toString();
            QCOMPARE(v, QStringLiteral("info"));
        }

        model->EndStreaming(false);
    }

    // An enum column with an empty dictionary must show the placeholder
    // and disable OK so a "hide everything" filter cannot be submitted.
    void TestFilterEditorEmptyEnumPickerDisablesOk()
    {
        // Configure `level` as enum without streaming so the dict stays empty.
        auto *model = mWindow->findChild<LogModel *>();
        QVERIFY2(model != nullptr, "MainWindow must own a LogModel");

        const QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString cfgPath = dir.filePath("empty_enum.json");
        // Save through the manager so the JSON matches the live schema,
        // then load it back into the model.
        {
            loglib::LogConfigurationManager scratch;
            scratch.AppendKeys({"level"});
            scratch.SetColumnType(0, loglib::LogConfiguration::Type::Enumeration);
            scratch.Save(cfgPath.toStdString());
        }
        model->ConfigurationManager().Load(cfgPath.toStdString());

        QCOMPARE(model->Configuration().columns.size(), static_cast<size_t>(1));
        QCOMPARE(model->Configuration().columns[0].type, loglib::LogConfiguration::Type::Enumeration);

        const FilterEditor editor(*model, QStringLiteral("test-empty-enum"));
        // `UpdateSelectedColumn(0)` settles the OK / placeholder state.

        // Reach widgets via the explicit accessors: see the comment on
        // `MainWindow::FindUiAction` for why `findChildren<...>` is
        // unreliable on the Linux runner with Qt 6.8 + offscreen QPA.
        const QPushButton *okButton = editor.OkButton();
        QVERIFY2(okButton != nullptr, "FilterEditor must expose an OK button");
        // clang-analyzer does not model `QVERIFY2`'s test-aborting behaviour,
        // so it still considers `okButton` potentially null on the next line.
        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage)
        QVERIFY2(!okButton->isEnabled(), "OK must be disabled when the picker dictionary is empty");

        const QLabel *placeholder = editor.EnumEmptyPlaceholder();
        QVERIFY2(placeholder != nullptr, "FilterEditor must expose the empty-picker placeholder");
        QVERIFY2(
            placeholder->text().contains(QStringLiteral("No values observed"), Qt::CaseInsensitive),
            "placeholder text must explain why the picker is empty"
        );
        // Use `!isHidden()`: `isVisible()` requires shown ancestors.
        QVERIFY2(!placeholder->isHidden(), "placeholder must not be explicitly hidden when the picker is empty");
    }

    // A saved enum filter with empty `filterValues` is "hide everything";
    // drop it at AddFilter time rather than opening a doomed editor.
    void TestSavedEmptyEnumFilterIsDropped()
    {
        // Promote `level` so the filter is type-compatible.
        const QStringList levels{
            QStringLiteral("info"), QStringLiteral("warn"), QStringLiteral("error"), QStringLiteral("debug")
        };
        QStringList lines;
        lines.reserve(320);
        for (int i = 0; i < 320; ++i)
        {
            lines.append(QStringLiteral(R"({"level": "%1"})").arg(levels[i % levels.size()]));
        }
        const TempJsonFile fixture(lines);

        auto *model = mWindow->findChild<LogModel *>();
        QVERIFY2(model != nullptr, "MainWindow must own a LogModel");

        QSignalSpy finishedSpy(model, &LogModel::streamingFinished);
        QVERIFY(finishedSpy.isValid());

        auto file = std::make_unique<loglib::LogFile>(fixture.Path().toStdString());
        auto fileSource = std::make_unique<loglib::FileLineSource>(std::move(file));
        loglib::FileLineSource *fileSourcePtr = fileSource.get();
        const loglib::StopToken stopToken = model->BeginStreamingForSyncTest(std::move(fileSource));

        loglib::ParserOptions options;
        options.stopToken = stopToken;
        loglib::internal::AdvancedParserOptions advanced;
        advanced.threads = 1;
        const loglib::JsonParser parser;
        loglib::JsonParser::ParseStreaming(*fileSourcePtr, *model->Sink(), options, advanced);

        const bool finished = finishedSpy.count() > 0 || finishedSpy.wait(5000);
        QVERIFY2(finished, "streamingFinished must arrive within the timeout");
        QCOMPARE(model->rowCount(), 320);

        const int levelCol = ColumnByHeader(*model, QStringLiteral("level"));
        QVERIFY2(levelCol >= 0, "level column must exist");
        QCOMPARE(
            model->Configuration().columns[static_cast<size_t>(levelCol)].type,
            loglib::LogConfiguration::Type::Enumeration
        );

        loglib::LogConfiguration::LogFilter savedFilter;
        savedFilter.type = loglib::LogConfiguration::LogFilter::Type::Enumeration;
        savedFilter.row = levelCol;

        mWindow->statusBar()->clearMessage();
        const QString filterId = QStringLiteral("saved-empty-enum");
        QVERIFY2(
            QMetaObject::invokeMethod(
                mWindow,
                "AddFilter",
                Qt::DirectConnection,
                Q_ARG(QString, filterId),
                Q_ARG(std::optional<loglib::LogConfiguration::LogFilter>, savedFilter)
            ),
            "AddFilter slot must be invocable via meta-object"
        );
        QCoreApplication::processEvents();

        const QString message = mWindow->statusBar()->currentMessage();
        QVERIFY2(
            message.contains(QStringLiteral("had no values selected"), Qt::CaseInsensitive),
            qPrintable(QStringLiteral("expected status-bar 'no values selected' message; got '%1'").arg(message))
        );

        // Unfiltered total: the empty enum filter never became active.
        // Reach the proxy and menu via the explicit accessors: see
        // `MainWindow::FindUiAction` for the offscreen-QPA traversal
        // bug that strands `findChild`/`findChildren` here.
        const LogFilterModel *filterModel = mWindow->FilterModel();
        QVERIFY(filterModel != nullptr);
        QCOMPARE(filterModel->rowCount(), 320);

        const auto *menu = mWindow->FiltersMenu();
        QVERIFY(menu != nullptr);
        for (const QAction *action : menu->actions())
        {
            QVERIFY2(action->data().toString() != filterId, "dropped enum filter must not have a menu entry");
        }

        // No editor opens; filter is dropped before construction.
        for (QWidget *widget : QApplication::topLevelWidgets())
        {
            if (auto *editor = qobject_cast<FilterEditor *>(widget))
            {
                editor->close();
                editor->deleteLater();
            }
        }
        QCoreApplication::processEvents();

        model->EndStreaming(false);
    }

    // `enumColumnsChanged` must fire when a column auto-promotes mid-stream.
    void TestEnumColumnsChangedFiresOnPromotion()
    {
        // 4 levels across 320 rows; well past the promotion threshold.
        const QStringList levels{
            QStringLiteral("info"), QStringLiteral("warn"), QStringLiteral("error"), QStringLiteral("debug")
        };
        QStringList lines;
        lines.reserve(320);
        for (int i = 0; i < 320; ++i)
        {
            lines.append(QStringLiteral(R"({"level": "%1"})").arg(levels[i % levels.size()]));
        }
        const TempJsonFile fixture(lines);

        auto *model = mWindow->findChild<LogModel *>();
        QVERIFY2(model != nullptr, "MainWindow must own a LogModel");

        const QSignalSpy enumSpy(model, &LogModel::enumColumnsChanged);
        QVERIFY(enumSpy.isValid());
        QSignalSpy finishedSpy(model, &LogModel::streamingFinished);
        QVERIFY(finishedSpy.isValid());

        auto file = std::make_unique<loglib::LogFile>(fixture.Path().toStdString());
        auto fileSource = std::make_unique<loglib::FileLineSource>(std::move(file));
        loglib::FileLineSource *fileSourcePtr = fileSource.get();
        const loglib::StopToken stopToken = model->BeginStreamingForSyncTest(std::move(fileSource));

        loglib::ParserOptions options;
        options.stopToken = stopToken;
        loglib::internal::AdvancedParserOptions advanced;
        advanced.threads = 1;
        const loglib::JsonParser parser;
        loglib::JsonParser::ParseStreaming(*fileSourcePtr, *model->Sink(), options, advanced);

        const bool finished = finishedSpy.count() > 0 || finishedSpy.wait(5000);
        QVERIFY2(finished, "streamingFinished must arrive within the timeout");
        QCOMPARE(model->rowCount(), 320);

        const int levelCol = ColumnByHeader(*model, QStringLiteral("level"));
        QVERIFY2(levelCol >= 0, "level column must exist");
        const auto &columns = model->Configuration().columns;
        QCOMPARE(columns[static_cast<size_t>(levelCol)].type, loglib::LogConfiguration::Type::Enumeration);

        // Don't pin an exact count: dictionary growth may re-fire.
        QVERIFY2(
            enumSpy.count() >= 1,
            qPrintable(QStringLiteral("enumColumnsChanged should have fired at least once; got %1").arg(enumSpy.count())
            )
        );

        model->EndStreaming(false);
    }

    // Regression: a streaming batch through `LogFilterModel` with no
    // active sort and no filter must emit a single bracketed
    // `rowsInserted` pair covering the whole accepted range, not one
    // pair per source row. Pre-fix `OnSourceRowsInserted` bracketed
    // each row individually and rebuilt the reverse index outside
    // the bracket (O(n) signal pairs + O(n)-staleness window on
    // `mSourceRowToProxyRow`). Post-fix coalesces into one bracket
    // and rebuilds the reverse index inside it.
    void TestStreamingBatchEmitsSingleRowsInsertedBracket()
    {
        auto *model = mWindow->findChild<LogModel *>();
        QVERIFY2(model != nullptr, "MainWindow must own a LogModel");
        auto *filterModel = mWindow->FilterModel();
        QVERIFY2(filterModel != nullptr, "MainWindow must own a LogFilterModel");

        // Empty fixture, driven by direct `AppendBatch` calls so the
        // batch boundary is well-defined and the only `rowsInserted`
        // emission on the proxy is the one under test.
        const TempJsonFile emptyFixture(QStringList{});
        auto file = std::make_unique<loglib::LogFile>(emptyFixture.Path().toStdString());
        auto fileSource = std::make_unique<loglib::FileLineSource>(std::move(file));
        loglib::FileLineSource *sourcePtr = fileSource.get();
        (void)model->BeginStreamingForSyncTest(std::move(fileSource));

        const QSignalSpy aboutToInsertSpy(filterModel, &QAbstractItemModel::rowsAboutToBeInserted);
        QSignalSpy insertedSpy(filterModel, &QAbstractItemModel::rowsInserted);
        QVERIFY(aboutToInsertSpy.isValid());
        QVERIFY(insertedSpy.isValid());

        constexpr int BATCH_SIZE = 100;
        loglib::KeyIndex &keys = model->Table().Keys();
        {
            loglib::StreamedBatch batch;
            batch.firstLineNumber = 1;
            batch.newKeys.emplace_back("msg");
            batch.lines.reserve(BATCH_SIZE);
            for (int i = 0; i < BATCH_SIZE; ++i)
            {
                std::vector<std::pair<loglib::KeyId, loglib::LogValue>> values;
                values.emplace_back(keys.GetOrInsert("msg"), loglib::LogValue(std::string("msg-") + std::to_string(i)));
                batch.lines.emplace_back(std::move(values), keys, *sourcePtr, static_cast<size_t>(i));
            }
            model->AppendBatch(std::move(batch));
        }
        QCoreApplication::processEvents();

        QCOMPARE(filterModel->rowCount(), BATCH_SIZE);

        // One bulk bracket. Pre-fix this would have been BATCH_SIZE pairs.
        QCOMPARE(aboutToInsertSpy.count(), 1);
        QCOMPARE(insertedSpy.count(), 1);

        const auto args = insertedSpy.takeFirst();
        QCOMPARE(args.at(1).toInt(), 0);
        QCOMPARE(args.at(2).toInt(), BATCH_SIZE - 1);

        // Reverse index must be consistent post-bracket: a
        // `mapToSource` -> `mapFromSource` round-trip pins both
        // directions of the proxy after the bulk insert.
        for (int proxyRow = 0; proxyRow < BATCH_SIZE; ++proxyRow)
        {
            const QModelIndex proxyIdx = filterModel->index(proxyRow, 0);
            const QModelIndex sourceIdx = filterModel->mapToSource(proxyIdx);
            QVERIFY2(
                sourceIdx.isValid(),
                qPrintable(QStringLiteral("proxy row %1 must map to a valid source index").arg(proxyRow))
            );
            const QModelIndex roundTrip = filterModel->mapFromSource(sourceIdx);
            QVERIFY2(
                roundTrip.isValid() && roundTrip.row() == proxyRow,
                qPrintable(QStringLiteral("proxy row %1 must round-trip through mapFromSource (got %2)")
                               .arg(proxyRow)
                               .arg(roundTrip.isValid() ? roundTrip.row() : -1))
            );
        }

        model->EndStreaming(false);
    }

    // Regression: an enum filter installed while the column is still
    // `Type::Unknown` builds with `dictionary = nullptr` and runs the
    // slow string-set fallback. On auto-promote,
    // `MainWindow::enumColumnsChanged(Promoted)` must rebuild the
    // predicate so it picks up the bitset hot path. Pre-fix the slot
    // gated the rebuild on `!EnumFilterFullyResolved` -- which goes
    // true the instant the new dictionary contains every selected
    // value -- so no rebuild ever fired. The observable is a
    // `LogFilterModel::layoutChanged` from `UpdateFilters` after the
    // `Promoted` signal.
    void TestNumericRangeFilterFiltersToBoundedRange()
    {
        // Stream into `mWindow`'s own model so the live filter pipeline
        // applies. `RunStreaming` builds a detached model and is not
        // wired to `mWindow->FilterModel()`.
        constexpr int FIXTURE_LINES = 100;
        QStringList lines;
        lines.reserve(FIXTURE_LINES);
        for (int i = 0; i < FIXTURE_LINES; ++i)
        {
            lines.append(QStringLiteral(R"({"value": %1})").arg(i));
        }
        const TempJsonFile fixture(lines);

        auto *model = mWindow->findChild<LogModel *>();
        QVERIFY2(model != nullptr, "MainWindow must own a LogModel");

        QSignalSpy finishedSpy(model, &LogModel::streamingFinished);
        QVERIFY(finishedSpy.isValid());

        auto file = std::make_unique<loglib::LogFile>(fixture.Path().toStdString());
        auto fileSource = std::make_unique<loglib::FileLineSource>(std::move(file));
        loglib::FileLineSource *fileSourcePtr = fileSource.get();
        const loglib::StopToken stopToken = model->BeginStreamingForSyncTest(std::move(fileSource));

        loglib::ParserOptions options;
        options.stopToken = stopToken;
        loglib::internal::AdvancedParserOptions advanced;
        advanced.threads = 1;
        const loglib::JsonParser parser;
        loglib::JsonParser::ParseStreaming(*fileSourcePtr, *model->Sink(), options, advanced);
        QVERIFY2(finishedSpy.count() > 0 || finishedSpy.wait(5000), "streamingFinished must arrive");
        QCOMPARE(model->rowCount(), FIXTURE_LINES);

        const int valueCol = ColumnByHeader(*model, QStringLiteral("value"));
        QVERIFY2(valueCol >= 0, "value column must exist");
        const auto &columns = model->Configuration().columns;
        QCOMPARE(columns[static_cast<size_t>(valueCol)].type, loglib::LogConfiguration::Type::Integer);

        const QString filterId = QStringLiteral("numeric-range");
        QVERIFY2(
            QMetaObject::invokeMethod(
                mWindow,
                "FilterNumericRangeSubmitted",
                Qt::DirectConnection,
                Q_ARG(QString, filterId),
                Q_ARG(int, valueCol),
                Q_ARG(std::optional<double>, std::optional<double>{20.0}),
                Q_ARG(std::optional<double>, std::optional<double>{40.0})
            ),
            "FilterNumericRangeSubmitted slot must be invocable via meta-object"
        );
        QCoreApplication::processEvents();

        const LogFilterModel *filterModel = mWindow->FilterModel();
        QVERIFY(filterModel != nullptr);
        QCOMPARE(filterModel->rowCount(), 21); // 20..40 inclusive

        model->EndStreaming(false);
    }

    void TestNumericRangeFilterUnboundedSideAcceptsTail()
    {
        constexpr int FIXTURE_LINES = 50;
        QStringList lines;
        lines.reserve(FIXTURE_LINES);
        for (int i = 0; i < FIXTURE_LINES; ++i)
        {
            lines.append(QStringLiteral(R"({"value": %1})").arg(i));
        }
        const TempJsonFile fixture(lines);

        auto *model = mWindow->findChild<LogModel *>();
        QVERIFY2(model != nullptr, "MainWindow must own a LogModel");

        QSignalSpy finishedSpy(model, &LogModel::streamingFinished);
        QVERIFY(finishedSpy.isValid());

        auto file = std::make_unique<loglib::LogFile>(fixture.Path().toStdString());
        auto fileSource = std::make_unique<loglib::FileLineSource>(std::move(file));
        loglib::FileLineSource *fileSourcePtr = fileSource.get();
        const loglib::StopToken stopToken = model->BeginStreamingForSyncTest(std::move(fileSource));

        loglib::ParserOptions options;
        options.stopToken = stopToken;
        loglib::internal::AdvancedParserOptions advanced;
        advanced.threads = 1;
        const loglib::JsonParser parser;
        loglib::JsonParser::ParseStreaming(*fileSourcePtr, *model->Sink(), options, advanced);
        QVERIFY2(finishedSpy.count() > 0 || finishedSpy.wait(5000), "streamingFinished must arrive");

        const int valueCol = ColumnByHeader(*model, QStringLiteral("value"));
        QVERIFY2(valueCol >= 0, "value column must exist");

        // Min=30, unbounded max -> 30..49 inclusive (20 rows).
        const QString filterId = QStringLiteral("numeric-range-unbounded");
        QVERIFY2(
            QMetaObject::invokeMethod(
                mWindow,
                "FilterNumericRangeSubmitted",
                Qt::DirectConnection,
                Q_ARG(QString, filterId),
                Q_ARG(int, valueCol),
                Q_ARG(std::optional<double>, std::optional<double>{30.0}),
                Q_ARG(std::optional<double>, std::optional<double>{})
            ),
            "FilterNumericRangeSubmitted slot must be invocable via meta-object"
        );
        QCoreApplication::processEvents();

        const LogFilterModel *filterModel = mWindow->FilterModel();
        QVERIFY(filterModel != nullptr);
        QCOMPARE(filterModel->rowCount(), 20);

        model->EndStreaming(false);
    }

    void TestBooleanFilterFiltersByPickedSide()
    {
        constexpr int FIXTURE_LINES = 80;
        QStringList lines;
        lines.reserve(FIXTURE_LINES);
        for (int i = 0; i < FIXTURE_LINES; ++i)
        {
            lines.append(QStringLiteral(R"({"flag": %1})").arg(i % 2 == 0 ? "true" : "false"));
        }
        const TempJsonFile fixture(lines);

        auto *model = mWindow->findChild<LogModel *>();
        QVERIFY2(model != nullptr, "MainWindow must own a LogModel");

        QSignalSpy finishedSpy(model, &LogModel::streamingFinished);
        QVERIFY(finishedSpy.isValid());

        auto file = std::make_unique<loglib::LogFile>(fixture.Path().toStdString());
        auto fileSource = std::make_unique<loglib::FileLineSource>(std::move(file));
        loglib::FileLineSource *fileSourcePtr = fileSource.get();
        const loglib::StopToken stopToken = model->BeginStreamingForSyncTest(std::move(fileSource));

        loglib::ParserOptions options;
        options.stopToken = stopToken;
        loglib::internal::AdvancedParserOptions advanced;
        advanced.threads = 1;
        const loglib::JsonParser parser;
        loglib::JsonParser::ParseStreaming(*fileSourcePtr, *model->Sink(), options, advanced);
        QVERIFY2(finishedSpy.count() > 0 || finishedSpy.wait(5000), "streamingFinished must arrive");

        const int flagCol = ColumnByHeader(*model, QStringLiteral("flag"));
        QVERIFY2(flagCol >= 0, "flag column must exist");
        const auto &columns = model->Configuration().columns;
        QCOMPARE(columns[static_cast<size_t>(flagCol)].type, loglib::LogConfiguration::Type::Boolean);

        const QString filterId = QStringLiteral("boolean-true-only");
        QVERIFY2(
            QMetaObject::invokeMethod(
                mWindow,
                "FilterBooleanSubmitted",
                Qt::DirectConnection,
                Q_ARG(QString, filterId),
                Q_ARG(int, flagCol),
                Q_ARG(bool, true),
                Q_ARG(bool, false)
            ),
            "FilterBooleanSubmitted slot must be invocable via meta-object"
        );
        QCoreApplication::processEvents();

        const LogFilterModel *filterModel = mWindow->FilterModel();
        QVERIFY(filterModel != nullptr);
        QCOMPARE(filterModel->rowCount(), FIXTURE_LINES / 2); // half are true

        model->EndStreaming(false);
    }

    // Numeric-range filter survives a serialise+reload round trip: the
    // saved JSON is reloaded into a fresh `LogConfiguration`, the
    // round-tripped `LogFilter` is replayed through `AddFilter`, the
    // type-match check accepts it, and the editor opens preloaded with
    // the saved bounds.
    void TestSavedNumericRangeFilterRoundTrips()
    {
        constexpr int FIXTURE_LINES = 100;
        QStringList lines;
        lines.reserve(FIXTURE_LINES);
        for (int i = 0; i < FIXTURE_LINES; ++i)
        {
            lines.append(QStringLiteral(R"({"value": %1})").arg(i));
        }
        const TempJsonFile fixture(lines);

        auto *model = mWindow->findChild<LogModel *>();
        QVERIFY2(model != nullptr, "MainWindow must own a LogModel");

        QSignalSpy finishedSpy(model, &LogModel::streamingFinished);
        QVERIFY(finishedSpy.isValid());

        auto file = std::make_unique<loglib::LogFile>(fixture.Path().toStdString());
        auto fileSource = std::make_unique<loglib::FileLineSource>(std::move(file));
        loglib::FileLineSource *fileSourcePtr = fileSource.get();
        const loglib::StopToken stopToken = model->BeginStreamingForSyncTest(std::move(fileSource));

        loglib::ParserOptions options;
        options.stopToken = stopToken;
        loglib::internal::AdvancedParserOptions advanced;
        advanced.threads = 1;
        const loglib::JsonParser parser;
        loglib::JsonParser::ParseStreaming(*fileSourcePtr, *model->Sink(), options, advanced);
        QVERIFY2(finishedSpy.count() > 0 || finishedSpy.wait(5000), "streamingFinished must arrive");

        const int valueCol = ColumnByHeader(*model, QStringLiteral("value"));
        QVERIFY2(valueCol >= 0, "value column must exist");

        // Serialise the saved filter and round-trip it through glaze to
        // confirm the wire-format keys survive (mirrors the lib-side
        // tests for `Type::Number`).
        loglib::LogConfiguration cfg;
        cfg.filters.emplace_back();
        cfg.filters[0].type = loglib::LogConfiguration::LogFilter::Type::Number;
        cfg.filters[0].row = valueCol;
        cfg.filters[0].filterMinValue = 10.0;
        cfg.filters[0].filterMaxValue = 19.0;

        std::string json;
        QVERIFY(!glz::write_json(cfg, json));

        loglib::LogConfiguration loaded;
        QVERIFY(!glz::read_json(loaded, json));
        QCOMPARE(loaded.filters.size(), static_cast<size_t>(1));
        QCOMPARE(loaded.filters[0].type, loglib::LogConfiguration::LogFilter::Type::Number);
        QVERIFY(loaded.filters[0].filterMinValue.has_value());
        QVERIFY(loaded.filters[0].filterMaxValue.has_value());

        // Replay the round-tripped filter through `AddFilter`. The
        // type-match check accepts (Number vs Integer column) and the
        // editor opens preloaded with the bounds.
        mWindow->statusBar()->clearMessage();
        const QString filterId = QStringLiteral("saved-numeric-roundtrip");
        QVERIFY2(
            QMetaObject::invokeMethod(
                mWindow,
                "AddFilter",
                Qt::DirectConnection,
                Q_ARG(QString, filterId),
                Q_ARG(std::optional<loglib::LogConfiguration::LogFilter>, loaded.filters[0])
            ),
            "AddFilter slot must be invocable via meta-object"
        );
        QCoreApplication::processEvents();

        // The saved filter survived the type-match check (no drop msg).
        const QString msg = mWindow->statusBar()->currentMessage();
        QVERIFY2(
            !msg.contains(QStringLiteral("was removed"), Qt::CaseInsensitive),
            qPrintable(QStringLiteral("expected no drop message; got '%1'").arg(msg))
        );

        // A `FilterEditor` opens with the saved bounds. Confirm by
        // probing the top-level editor's getters.
        FilterEditor *editor = nullptr;
        for (QWidget *widget : QApplication::topLevelWidgets())
        {
            if (auto *e = qobject_cast<FilterEditor *>(widget))
            {
                editor = e;
                break;
            }
        }
        QVERIFY2(editor != nullptr, "FilterEditor must open for a Number-typed saved filter");
        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage)
        const auto editorMin = editor->GetNumericRangeMin();
        const auto editorMax = editor->GetNumericRangeMax();
        QVERIFY(editorMin.has_value());
        QVERIFY(editorMax.has_value());
        QCOMPARE(*editorMin, 10.0);
        QCOMPARE(*editorMax, 19.0);

        editor->close();
        editor->deleteLater();
        QCoreApplication::processEvents();

        model->EndStreaming(false);
    }

    // Saved numeric-range filter against a column whose type changes
    // underneath is dropped at `AddFilter` time, mirroring the
    // string-filter-on-now-enum drop behaviour.
    void TestSavedNumericRangeFilterDroppedOnTypeMismatch()
    {
        // `level` promotes to enum after streaming, so a saved
        // numeric-range filter on the same row mismatches the type.
        const QStringList levels{QStringLiteral("info"), QStringLiteral("warn"), QStringLiteral("error")};
        QStringList lines;
        lines.reserve(300);
        for (int i = 0; i < 300; ++i)
        {
            lines.append(QStringLiteral(R"({"level": "%1"})").arg(levels[i % levels.size()]));
        }
        const TempJsonFile fixture(lines);

        auto *model = mWindow->findChild<LogModel *>();
        QVERIFY2(model != nullptr, "MainWindow must own a LogModel");

        QSignalSpy finishedSpy(model, &LogModel::streamingFinished);
        QVERIFY(finishedSpy.isValid());

        auto file = std::make_unique<loglib::LogFile>(fixture.Path().toStdString());
        auto fileSource = std::make_unique<loglib::FileLineSource>(std::move(file));
        loglib::FileLineSource *fileSourcePtr = fileSource.get();
        const loglib::StopToken stopToken = model->BeginStreamingForSyncTest(std::move(fileSource));

        loglib::ParserOptions options;
        options.stopToken = stopToken;
        loglib::internal::AdvancedParserOptions advanced;
        advanced.threads = 1;
        const loglib::JsonParser parser;
        loglib::JsonParser::ParseStreaming(*fileSourcePtr, *model->Sink(), options, advanced);
        QVERIFY2(finishedSpy.count() > 0 || finishedSpy.wait(5000), "streamingFinished must arrive");

        const int levelCol = ColumnByHeader(*model, QStringLiteral("level"));
        QVERIFY2(levelCol >= 0, "level column must exist");
        QCOMPARE(
            model->Configuration().columns[static_cast<size_t>(levelCol)].type,
            loglib::LogConfiguration::Type::Enumeration
        );

        loglib::LogConfiguration::LogFilter savedFilter;
        savedFilter.type = loglib::LogConfiguration::LogFilter::Type::Number;
        savedFilter.row = levelCol;
        savedFilter.filterMinValue = 0.0;
        savedFilter.filterMaxValue = 1.0;

        mWindow->statusBar()->clearMessage();
        const QString filterId = QStringLiteral("saved-numeric-mismatch");
        QVERIFY2(
            QMetaObject::invokeMethod(
                mWindow,
                "AddFilter",
                Qt::DirectConnection,
                Q_ARG(QString, filterId),
                Q_ARG(std::optional<loglib::LogConfiguration::LogFilter>, savedFilter)
            ),
            "AddFilter slot must be invocable via meta-object"
        );
        QCoreApplication::processEvents();

        const QString message = mWindow->statusBar()->currentMessage();
        QVERIFY2(
            message.contains(QStringLiteral("was removed because the column type changed"), Qt::CaseInsensitive),
            qPrintable(QStringLiteral("expected status-bar drop message; got '%1'").arg(message))
        );

        // Filter was dropped, model is unfiltered.
        const LogFilterModel *filterModel = mWindow->FilterModel();
        QVERIFY(filterModel != nullptr);
        QCOMPARE(filterModel->rowCount(), 300);

        // Close any drop editor (saved-filter drop opens an empty one).
        for (QWidget *widget : QApplication::topLevelWidgets())
        {
            if (auto *editor = qobject_cast<FilterEditor *>(widget))
            {
                editor->close();
                editor->deleteLater();
            }
        }
        QCoreApplication::processEvents();

        model->EndStreaming(false);
    }

    // Saved `Type::String` filter against a column that auto-detected
    // to `Type::Boolean` should be dropped at `AddFilter` time, the
    // same way it would for an enum-promoted column. Regression cover
    // for the type-match check after `Boolean` was added.
    void TestSavedStringFilterDroppedOnNowBooleanColumn()
    {
        QStringList lines;
        lines.reserve(80);
        for (int i = 0; i < 80; ++i)
        {
            lines.append(QStringLiteral(R"({"flag": %1})").arg(i % 2 == 0 ? "true" : "false"));
        }
        const TempJsonFile fixture(lines);

        auto *model = mWindow->findChild<LogModel *>();
        QVERIFY2(model != nullptr, "MainWindow must own a LogModel");

        QSignalSpy finishedSpy(model, &LogModel::streamingFinished);
        QVERIFY(finishedSpy.isValid());

        auto file = std::make_unique<loglib::LogFile>(fixture.Path().toStdString());
        auto fileSource = std::make_unique<loglib::FileLineSource>(std::move(file));
        loglib::FileLineSource *fileSourcePtr = fileSource.get();
        const loglib::StopToken stopToken = model->BeginStreamingForSyncTest(std::move(fileSource));

        loglib::ParserOptions options;
        options.stopToken = stopToken;
        loglib::internal::AdvancedParserOptions advanced;
        advanced.threads = 1;
        const loglib::JsonParser parser;
        loglib::JsonParser::ParseStreaming(*fileSourcePtr, *model->Sink(), options, advanced);
        QVERIFY2(finishedSpy.count() > 0 || finishedSpy.wait(5000), "streamingFinished must arrive");

        const int flagCol = ColumnByHeader(*model, QStringLiteral("flag"));
        QVERIFY2(flagCol >= 0, "flag column must exist");
        QCOMPARE(
            model->Configuration().columns[static_cast<size_t>(flagCol)].type,
            loglib::LogConfiguration::Type::Boolean
        );

        loglib::LogConfiguration::LogFilter savedFilter;
        savedFilter.type = loglib::LogConfiguration::LogFilter::Type::String;
        savedFilter.row = flagCol;
        savedFilter.filterString = "true";
        savedFilter.matchType = loglib::LogConfiguration::LogFilter::Match::Exactly;

        mWindow->statusBar()->clearMessage();
        const QString filterId = QStringLiteral("saved-string-on-bool");
        QVERIFY2(
            QMetaObject::invokeMethod(
                mWindow,
                "AddFilter",
                Qt::DirectConnection,
                Q_ARG(QString, filterId),
                Q_ARG(std::optional<loglib::LogConfiguration::LogFilter>, savedFilter)
            ),
            "AddFilter slot must be invocable via meta-object"
        );
        QCoreApplication::processEvents();

        const QString message = mWindow->statusBar()->currentMessage();
        QVERIFY2(
            message.contains(QStringLiteral("was removed because the column type changed"), Qt::CaseInsensitive),
            qPrintable(QStringLiteral("expected status-bar drop message; got '%1'").arg(message))
        );

        const LogFilterModel *filterModel = mWindow->FilterModel();
        QVERIFY(filterModel != nullptr);
        QCOMPARE(filterModel->rowCount(), 80);

        for (QWidget *widget : QApplication::topLevelWidgets())
        {
            if (auto *editor = qobject_cast<FilterEditor *>(widget))
            {
                editor->close();
                editor->deleteLater();
            }
        }
        QCoreApplication::processEvents();

        model->EndStreaming(false);
    }

    // Saved `Type::String` filter against a numeric column should drop
    // at `AddFilter` time. Same shape as the now-Boolean / now-Enum
    // variants but covers the numeric type-match branch.
    void TestSavedStringFilterDroppedOnNowNumericColumn()
    {
        QStringList lines;
        lines.reserve(100);
        for (int i = 0; i < 100; ++i)
        {
            lines.append(QStringLiteral(R"({"value": %1})").arg(i));
        }
        const TempJsonFile fixture(lines);

        auto *model = mWindow->findChild<LogModel *>();
        QVERIFY2(model != nullptr, "MainWindow must own a LogModel");

        QSignalSpy finishedSpy(model, &LogModel::streamingFinished);
        QVERIFY(finishedSpy.isValid());

        auto file = std::make_unique<loglib::LogFile>(fixture.Path().toStdString());
        auto fileSource = std::make_unique<loglib::FileLineSource>(std::move(file));
        loglib::FileLineSource *fileSourcePtr = fileSource.get();
        const loglib::StopToken stopToken = model->BeginStreamingForSyncTest(std::move(fileSource));

        loglib::ParserOptions options;
        options.stopToken = stopToken;
        loglib::internal::AdvancedParserOptions advanced;
        advanced.threads = 1;
        const loglib::JsonParser parser;
        loglib::JsonParser::ParseStreaming(*fileSourcePtr, *model->Sink(), options, advanced);
        QVERIFY2(finishedSpy.count() > 0 || finishedSpy.wait(5000), "streamingFinished must arrive");

        const int valueCol = ColumnByHeader(*model, QStringLiteral("value"));
        QVERIFY2(valueCol >= 0, "value column must exist");
        QCOMPARE(
            model->Configuration().columns[static_cast<size_t>(valueCol)].type,
            loglib::LogConfiguration::Type::Integer
        );

        loglib::LogConfiguration::LogFilter savedFilter;
        savedFilter.type = loglib::LogConfiguration::LogFilter::Type::String;
        savedFilter.row = valueCol;
        savedFilter.filterString = "42";
        savedFilter.matchType = loglib::LogConfiguration::LogFilter::Match::Exactly;

        mWindow->statusBar()->clearMessage();
        const QString filterId = QStringLiteral("saved-string-on-numeric");
        QVERIFY2(
            QMetaObject::invokeMethod(
                mWindow,
                "AddFilter",
                Qt::DirectConnection,
                Q_ARG(QString, filterId),
                Q_ARG(std::optional<loglib::LogConfiguration::LogFilter>, savedFilter)
            ),
            "AddFilter slot must be invocable via meta-object"
        );
        QCoreApplication::processEvents();

        const QString message = mWindow->statusBar()->currentMessage();
        QVERIFY2(
            message.contains(QStringLiteral("was removed because the column type changed"), Qt::CaseInsensitive),
            qPrintable(QStringLiteral("expected status-bar drop message; got '%1'").arg(message))
        );

        const LogFilterModel *filterModel = mWindow->FilterModel();
        QVERIFY(filterModel != nullptr);
        QCOMPARE(filterModel->rowCount(), 100);

        for (QWidget *widget : QApplication::topLevelWidgets())
        {
            if (auto *editor = qobject_cast<FilterEditor *>(widget))
            {
                editor->close();
                editor->deleteLater();
            }
        }
        QCoreApplication::processEvents();

        model->EndStreaming(false);
    }

    // Hand-edited Boolean filter with mixed-case `"True"` / `"FALSE"`
    // still restores correctly. `DecodeBooleanFilterSides` is
    // case-insensitive on the read path so a config touched by a
    // human (or a non-canonical writer) keeps working; the canonical
    // submit path still writes lowercase.
    void TestSavedBooleanFilterCaseInsensitiveRestore()
    {
        QStringList lines;
        lines.reserve(60);
        for (int i = 0; i < 60; ++i)
        {
            lines.append(QStringLiteral(R"({"flag": %1})").arg(i % 2 == 0 ? "true" : "false"));
        }
        const TempJsonFile fixture(lines);

        auto *model = mWindow->findChild<LogModel *>();
        QVERIFY2(model != nullptr, "MainWindow must own a LogModel");

        QSignalSpy finishedSpy(model, &LogModel::streamingFinished);
        QVERIFY(finishedSpy.isValid());

        auto file = std::make_unique<loglib::LogFile>(fixture.Path().toStdString());
        auto fileSource = std::make_unique<loglib::FileLineSource>(std::move(file));
        loglib::FileLineSource *fileSourcePtr = fileSource.get();
        const loglib::StopToken stopToken = model->BeginStreamingForSyncTest(std::move(fileSource));

        loglib::ParserOptions options;
        options.stopToken = stopToken;
        loglib::internal::AdvancedParserOptions advanced;
        advanced.threads = 1;
        const loglib::JsonParser parser;
        loglib::JsonParser::ParseStreaming(*fileSourcePtr, *model->Sink(), options, advanced);
        QVERIFY2(finishedSpy.count() > 0 || finishedSpy.wait(5000), "streamingFinished must arrive");

        const int flagCol = ColumnByHeader(*model, QStringLiteral("flag"));
        QVERIFY2(flagCol >= 0, "flag column must exist");
        QCOMPARE(
            model->Configuration().columns[static_cast<size_t>(flagCol)].type,
            loglib::LogConfiguration::Type::Boolean
        );

        loglib::LogConfiguration::LogFilter savedFilter;
        savedFilter.type = loglib::LogConfiguration::LogFilter::Type::Boolean;
        savedFilter.row = flagCol;
        // Non-canonical casing: lower path must still recognise both.
        savedFilter.filterValues = {"True", "FALSE"};

        mWindow->statusBar()->clearMessage();
        const QString filterId = QStringLiteral("saved-bool-mixed-case");
        QVERIFY2(
            QMetaObject::invokeMethod(
                mWindow,
                "AddFilter",
                Qt::DirectConnection,
                Q_ARG(QString, filterId),
                Q_ARG(std::optional<loglib::LogConfiguration::LogFilter>, savedFilter)
            ),
            "AddFilter slot must be invocable via meta-object"
        );
        QCoreApplication::processEvents();

        // Editor opens preloaded with both sides checked.
        FilterEditor *editor = nullptr;
        for (QWidget *widget : QApplication::topLevelWidgets())
        {
            if (auto *e = qobject_cast<FilterEditor *>(widget))
            {
                editor = e;
                break;
            }
        }
        QVERIFY2(editor != nullptr, "FilterEditor must open for a Boolean-typed saved filter");
        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage)
        QVERIFY(editor->GetBooleanIncludeTrue());
        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage)
        QVERIFY(editor->GetBooleanIncludeFalse());

        editor->close();
        editor->deleteLater();
        QCoreApplication::processEvents();

        model->EndStreaming(false);
    }

    void TestEnumFilterUpgradesToFastPathOnColumnPromotion()
    {
        auto *model = mWindow->findChild<LogModel *>();
        QVERIFY2(model != nullptr, "MainWindow must own a LogModel");
        auto *filterModel = mWindow->FilterModel();
        QVERIFY2(filterModel != nullptr, "MainWindow must own a LogFilterModel");

        const TempJsonFile emptyFixture(QStringList{});
        auto file = std::make_unique<loglib::LogFile>(emptyFixture.Path().toStdString());
        auto fileSource = std::make_unique<loglib::FileLineSource>(std::move(file));
        loglib::FileLineSource *sourcePtr = fileSource.get();
        (void)model->BeginStreamingForSyncTest(std::move(fileSource));

        loglib::KeyIndex &keys = model->Table().Keys();
        const auto makeLine = [&](const std::string &value) {
            std::vector<std::pair<loglib::KeyId, loglib::LogValue>> values;
            values.emplace_back(keys.GetOrInsert("mycol"), loglib::LogValue(value));
            return loglib::LogLine{std::move(values), keys, *sourcePtr, 0};
        };

        // Batch 1: one row keeps `mycol` below
        // STREAM_PROMOTION_MIN_ROWS (== 2), so the column stays
        // `Type::Unknown` and `ResolveEnumDictionary` returns nullptr
        // at filter-install time.
        {
            loglib::StreamedBatch batch;
            batch.firstLineNumber = 1;
            batch.newKeys.emplace_back("mycol");
            batch.lines.push_back(makeLine("alpha"));
            model->AppendBatch(std::move(batch));
        }
        QCoreApplication::processEvents();

        const int col = ColumnByHeader(*model, QStringLiteral("mycol"));
        QVERIFY2(col >= 0, "mycol column must exist after the first batch");
        QCOMPARE(
            model->Configuration().columns[static_cast<size_t>(col)].type, loglib::LogConfiguration::Type::Unknown
        );

        // Install an enum filter while the column is still Unknown:
        // predicate built with `dictionary = nullptr`, only the slow
        // string-set fallback path is available.
        const QString filterId = QStringLiteral("upgrade-on-promote");
        QVERIFY2(
            QMetaObject::invokeMethod(
                mWindow,
                "FilterEnumSubmitted",
                Qt::DirectConnection,
                Q_ARG(QString, filterId),
                Q_ARG(int, col),
                Q_ARG(QStringList, QStringList{QStringLiteral("alpha")})
            ),
            "FilterEnumSubmitted slot must be invocable via meta-object"
        );
        QCoreApplication::processEvents();
        QCOMPARE(filterModel->rowCount(), 1); // matched via the string-set fallback.

        // Spy after install so only post-install rebuilds count. The
        // only `layoutChanged` path on `LogFilterModel` is
        // `RebuildAcceptedRows` (via `SetFilterRules` / sort), so
        // any post-promote emission means `UpdateFilters` re-armed
        // the predicate.
        const QSignalSpy layoutChangedSpy(filterModel, &QAbstractItemModel::layoutChanged);
        QVERIFY(layoutChangedSpy.isValid());
        const QSignalSpy enumChangedSpy(model, &LogModel::enumColumnsChanged);
        QVERIFY(enumChangedSpy.isValid());

        // Batch 2: a second row pushes `presenceCount` to 2 and
        // trips streaming promotion. The slot must rebuild the
        // predicate to upgrade it to the bitset path.
        {
            loglib::StreamedBatch batch;
            batch.firstLineNumber = 2;
            batch.lines.push_back(makeLine("alpha"));
            model->AppendBatch(std::move(batch));
        }
        QCoreApplication::processEvents();

        QCOMPARE(
            model->Configuration().columns[static_cast<size_t>(col)].type, loglib::LogConfiguration::Type::Enumeration
        );
        bool sawPromoted = false;
        for (const auto &args : enumChangedSpy)
        {
            if (args.at(0).value<EnumColumnsChangeReason>() == EnumColumnsChangeReason::Promoted)
            {
                sawPromoted = true;
                break;
            }
        }
        QVERIFY2(sawPromoted, "promotion batch must emit a `Promoted` reason");

        QVERIFY2(
            layoutChangedSpy.count() >= 1,
            qPrintable(QStringLiteral("filter must be rebuilt on Promoted to upgrade to the fast path; "
                                      "expected layoutChanged >= 1, got %1")
                           .arg(layoutChangedSpy.count()))
        );

        // Sanity: the rebuilt fast-path predicate still matches both rows.
        QCOMPARE(filterModel->rowCount(), 2);

        model->EndStreaming(false);
    }

    // Regression: `enumColumnsChanged(Promoted, columnIndex)` only
    // triggers a `MainWindow` filter rebuild when an enum filter
    // actually targets the promoted column. With per-column scoping
    // a promotion on column B should NOT invalidate the filter on
    // column A. Pre-fix the slot rebuilt on any enum filter,
    // regardless of which column changed.
    void TestEnumPromotedOnUnrelatedColumnDoesNotRebuildFilters()
    {
        auto *model = mWindow->findChild<LogModel *>();
        QVERIFY2(model != nullptr, "MainWindow must own a LogModel");
        auto *filterModel = mWindow->FilterModel();
        QVERIFY2(filterModel != nullptr, "MainWindow must own a LogFilterModel");

        const TempJsonFile emptyFixture(QStringList{});
        auto file = std::make_unique<loglib::LogFile>(emptyFixture.Path().toStdString());
        auto fileSource = std::make_unique<loglib::FileLineSource>(std::move(file));
        loglib::FileLineSource *sourcePtr = fileSource.get();
        (void)model->BeginStreamingForSyncTest(std::move(fileSource));

        loglib::KeyIndex &keys = model->Table().Keys();
        const auto makeColARow = [&](const std::string &value) {
            std::vector<std::pair<loglib::KeyId, loglib::LogValue>> values;
            values.emplace_back(keys.GetOrInsert("colA"), loglib::LogValue(value));
            return loglib::LogLine{std::move(values), keys, *sourcePtr, 0};
        };
        const auto makeColBRow = [&](const std::string &value) {
            std::vector<std::pair<loglib::KeyId, loglib::LogValue>> values;
            values.emplace_back(keys.GetOrInsert("colB"), loglib::LogValue(value));
            return loglib::LogLine{std::move(values), keys, *sourcePtr, 0};
        };

        // Batch 1: promote colA to Enumeration via the streaming
        // threshold (`presenceCount >= 2`). colB doesn't appear so
        // it stays `Type::Unknown` until batch 2 introduces it.
        {
            loglib::StreamedBatch batch;
            batch.firstLineNumber = 1;
            batch.newKeys.emplace_back("colA");
            batch.lines.push_back(makeColARow("alpha"));
            batch.lines.push_back(makeColARow("alpha"));
            model->AppendBatch(std::move(batch));
        }
        QCoreApplication::processEvents();

        const int colA = ColumnByHeader(*model, QStringLiteral("colA"));
        QVERIFY2(colA >= 0, "colA must exist after batch 1");
        QCOMPARE(
            model->Configuration().columns[static_cast<size_t>(colA)].type, loglib::LogConfiguration::Type::Enumeration
        );

        // Install an enum filter on colA only. Predicate is built
        // with the live dictionary so it's already on the fast path.
        const QString filterId = QStringLiteral("scoped-promote-test");
        QVERIFY2(
            QMetaObject::invokeMethod(
                mWindow,
                "FilterEnumSubmitted",
                Qt::DirectConnection,
                Q_ARG(QString, filterId),
                Q_ARG(int, colA),
                Q_ARG(QStringList, QStringList{QStringLiteral("alpha")})
            ),
            "FilterEnumSubmitted slot must be invocable via meta-object"
        );
        QCoreApplication::processEvents();

        // Spy after install so only post-install rebuilds count.
        const QSignalSpy layoutChangedSpy(filterModel, &QAbstractItemModel::layoutChanged);
        QVERIFY(layoutChangedSpy.isValid());

        // Batch 2: introduce colB with 2 same-value rows so it
        // promotes via the streaming threshold. colA does not
        // appear in this batch so its dictionary is untouched.
        {
            loglib::StreamedBatch batch;
            batch.firstLineNumber = 3;
            batch.newKeys.emplace_back("colB");
            batch.lines.push_back(makeColBRow("x"));
            batch.lines.push_back(makeColBRow("x"));
            model->AppendBatch(std::move(batch));
        }
        QCoreApplication::processEvents();

        const int colB = ColumnByHeader(*model, QStringLiteral("colB"));
        QVERIFY2(colB >= 0, "colB must exist after batch 2");
        QCOMPARE(
            model->Configuration().columns[static_cast<size_t>(colB)].type, loglib::LogConfiguration::Type::Enumeration
        );
        QVERIFY2(colA != colB, "colA and colB must be distinct columns for the scoping test");

        // The filter on colA must NOT have been rebuilt. The only
        // `layoutChanged` source on `LogFilterModel` is
        // `RebuildAcceptedRows` (via `SetFilterRules`), so a count
        // of 0 confirms the slot short-circuited.
        QVERIFY2(
            layoutChangedSpy.count() == 0,
            qPrintable(
                QStringLiteral(
                    "filter on colA must not be rebuilt when an unrelated column promotes; got layoutChanged count %1"
                )
                    .arg(layoutChangedSpy.count())
            )
        );

        model->EndStreaming(false);
    }

    // Regression: a hand-edited config with `+inf` / `-inf` numeric
    // bounds opens the editor in a non-submittable state because
    // `OnOkClicked` rejects non-finite tokens and `QDoubleValidator`
    // doesn't strip them on `setText`. `Load()` coerces non-finite
    // bounds to "unbounded" so the dialog opens submittable and the
    // user can adjust as needed.
    void TestSavedNumericRangeFilterCoercesNonFiniteToUnbounded()
    {
        auto *model = mWindow->findChild<LogModel *>();
        QVERIFY2(model != nullptr, "MainWindow must own a LogModel");

        // Tiny fixture: just need a numeric column to exist so
        // `FilterEditor::UpdateSelectedColumn` switches to
        // `PAGE_NUMERIC`.
        QStringList lines;
        lines.reserve(64);
        for (int i = 0; i < 64; ++i)
        {
            lines.append(QStringLiteral(R"({"value": %1.5})").arg(i));
        }
        const TempJsonFile fixture(lines);

        QSignalSpy finishedSpy(model, &LogModel::streamingFinished);
        QVERIFY(finishedSpy.isValid());

        auto file = std::make_unique<loglib::LogFile>(fixture.Path().toStdString());
        auto fileSource = std::make_unique<loglib::FileLineSource>(std::move(file));
        loglib::FileLineSource *fileSourcePtr = fileSource.get();
        const loglib::StopToken stopToken = model->BeginStreamingForSyncTest(std::move(fileSource));

        loglib::ParserOptions options;
        options.stopToken = stopToken;
        loglib::internal::AdvancedParserOptions advanced;
        advanced.threads = 1;
        const loglib::JsonParser parser;
        loglib::JsonParser::ParseStreaming(*fileSourcePtr, *model->Sink(), options, advanced);
        QVERIFY2(finishedSpy.count() > 0 || finishedSpy.wait(5000), "streamingFinished must arrive");

        const int valueCol = ColumnByHeader(*model, QStringLiteral("value"));
        QVERIFY2(valueCol >= 0, "value column must exist");

        FilterEditor editor(*model, QStringLiteral("inf-coerce"));
        editor.Load(
            valueCol,
            std::optional<double>(std::numeric_limits<double>::infinity()),
            std::optional<double>(-std::numeric_limits<double>::infinity())
        );

        QVERIFY2(
            !editor.GetNumericRangeMin().has_value(),
            "+inf min must coerce to unbounded so the dialog opens submittable"
        );
        QVERIFY2(
            !editor.GetNumericRangeMax().has_value(),
            "-inf max must coerce to unbounded so the dialog opens submittable"
        );

        // NaN should also coerce.
        editor.Load(valueCol, std::optional<double>(std::nan("")), std::optional<double>(std::nan("")));
        QVERIFY2(!editor.GetNumericRangeMin().has_value(), "NaN min must coerce to unbounded");
        QVERIFY2(!editor.GetNumericRangeMax().has_value(), "NaN max must coerce to unbounded");

        model->EndStreaming(false);
    }

    // Regression: the inverted-range status-bar message uses the
    // canonical `QLocale::c() + max_digits10` formatter so the user
    // sees the exact bounds they typed. Pre-fix the message used
    // `arg(double)` with default precision 6 and silently truncated
    // distinct bounds like `12345.6789` and `12345.6790` into the
    // same string.
    void TestNumericRangeRejectionMessagePreservesPrecision()
    {
        auto *model = mWindow->findChild<LogModel *>();
        QVERIFY2(model != nullptr, "MainWindow must own a LogModel");

        QStringList lines;
        lines.reserve(64);
        for (int i = 0; i < 64; ++i)
        {
            lines.append(QStringLiteral(R"({"value": %1.5})").arg(i));
        }
        const TempJsonFile fixture(lines);

        QSignalSpy finishedSpy(model, &LogModel::streamingFinished);
        QVERIFY(finishedSpy.isValid());

        auto file = std::make_unique<loglib::LogFile>(fixture.Path().toStdString());
        auto fileSource = std::make_unique<loglib::FileLineSource>(std::move(file));
        loglib::FileLineSource *fileSourcePtr = fileSource.get();
        const loglib::StopToken stopToken = model->BeginStreamingForSyncTest(std::move(fileSource));

        loglib::ParserOptions options;
        options.stopToken = stopToken;
        loglib::internal::AdvancedParserOptions advanced;
        advanced.threads = 1;
        const loglib::JsonParser parser;
        loglib::JsonParser::ParseStreaming(*fileSourcePtr, *model->Sink(), options, advanced);
        QVERIFY2(finishedSpy.count() > 0 || finishedSpy.wait(5000), "streamingFinished must arrive");

        const int valueCol = ColumnByHeader(*model, QStringLiteral("value"));
        QVERIFY2(valueCol >= 0, "value column must exist");

        // Use exactly-representable doubles whose `g` precision-6
        // rendering collides (`"1e+06"` for both, since the
        // exponent equals the precision and trailing zeros are
        // stripped) but whose max_digits10 rendering is distinct.
        // 1000000 + 1/128 and 1000000 + 1/256 are both exact in
        // IEEE 754 double precision.
        constexpr double MIN_VALUE = 1000000.0078125;     // = 1e6 + 1/128
        constexpr double MAX_VALUE = 1000000.00390625;    // = 1e6 + 1/256, < MIN_VALUE

        mWindow->statusBar()->clearMessage();
        const QString filterId = QStringLiteral("inverted-precision");
        QVERIFY2(
            QMetaObject::invokeMethod(
                mWindow,
                "FilterNumericRangeSubmitted",
                Qt::DirectConnection,
                Q_ARG(QString, filterId),
                Q_ARG(int, valueCol),
                Q_ARG(std::optional<double>, std::optional<double>(MIN_VALUE)),
                Q_ARG(std::optional<double>, std::optional<double>(MAX_VALUE))
            ),
            "FilterNumericRangeSubmitted slot must be invocable via meta-object"
        );
        QCoreApplication::processEvents();

        const QString message = mWindow->statusBar()->currentMessage();
        QVERIFY2(
            message.contains(QStringLiteral("1000000.0078125")),
            qPrintable(QStringLiteral("rejection message must preserve full precision of min; got '%1'").arg(message))
        );
        QVERIFY2(
            message.contains(QStringLiteral("1000000.00390625")),
            qPrintable(QStringLiteral("rejection message must preserve full precision of max; got '%1'").arg(message))
        );

        model->EndStreaming(false);
    }

    // Regression: a Boolean filter with both sides selected renders
    // its menu title in canonical `"true, false"` order. The
    // submit slot already writes them in this order, but
    // `AddLogFilter` now derives the title via
    // `DecodeBooleanFilterSides` so a future code path that
    // installs a saved filter with non-canonical order (e.g. a
    // hand-edited `"filterValues": ["false", "true"]` payload)
    // still produces a stable title that matches the editor's
    // checkbox order.
    void TestBooleanFilterMenuTitleCanonicalOrder()
    {
        QStringList lines;
        lines.reserve(60);
        for (int i = 0; i < 60; ++i)
        {
            lines.append(QStringLiteral(R"({"flag": %1})").arg(i % 2 == 0 ? "true" : "false"));
        }
        const TempJsonFile fixture(lines);

        auto *model = mWindow->findChild<LogModel *>();
        QVERIFY2(model != nullptr, "MainWindow must own a LogModel");

        QSignalSpy finishedSpy(model, &LogModel::streamingFinished);
        QVERIFY(finishedSpy.isValid());

        auto file = std::make_unique<loglib::LogFile>(fixture.Path().toStdString());
        auto fileSource = std::make_unique<loglib::FileLineSource>(std::move(file));
        loglib::FileLineSource *fileSourcePtr = fileSource.get();
        const loglib::StopToken stopToken = model->BeginStreamingForSyncTest(std::move(fileSource));

        loglib::ParserOptions options;
        options.stopToken = stopToken;
        loglib::internal::AdvancedParserOptions advanced;
        advanced.threads = 1;
        const loglib::JsonParser parser;
        loglib::JsonParser::ParseStreaming(*fileSourcePtr, *model->Sink(), options, advanced);
        QVERIFY2(finishedSpy.count() > 0 || finishedSpy.wait(5000), "streamingFinished must arrive");

        const int flagCol = ColumnByHeader(*model, QStringLiteral("flag"));
        QVERIFY2(flagCol >= 0, "flag column must exist");
        QCOMPARE(
            model->Configuration().columns[static_cast<size_t>(flagCol)].type,
            loglib::LogConfiguration::Type::Boolean
        );

        const QString filterId = QStringLiteral("bool-canonical-title");
        QVERIFY2(
            QMetaObject::invokeMethod(
                mWindow,
                "FilterBooleanSubmitted",
                Qt::DirectConnection,
                Q_ARG(QString, filterId),
                Q_ARG(int, flagCol),
                Q_ARG(bool, true),
                Q_ARG(bool, true)
            ),
            "FilterBooleanSubmitted slot must be invocable via meta-object"
        );
        QCoreApplication::processEvents();

        const auto findFilterMenuAction = [&](const QString &id) -> QAction * {
            const auto *menu = mWindow->FiltersMenu();
            if (menu == nullptr)
            {
                return nullptr;
            }
            for (QAction *action : menu->actions())
            {
                if (action->data().toString() == id)
                {
                    return action;
                }
            }
            return nullptr;
        };

        QAction *action = findFilterMenuAction(filterId);
        QVERIFY2(action != nullptr, "Boolean filter must have a menu entry");
        QCOMPARE(action->text(), QStringLiteral("true, false"));

        // Single-side title sanity check: "false only" must not
        // accidentally inherit "true, false" from a stale code path.
        const QString falseOnlyId = QStringLiteral("bool-canonical-title-false");
        QVERIFY2(
            QMetaObject::invokeMethod(
                mWindow,
                "FilterBooleanSubmitted",
                Qt::DirectConnection,
                Q_ARG(QString, falseOnlyId),
                Q_ARG(int, flagCol),
                Q_ARG(bool, false),
                Q_ARG(bool, true)
            ),
            "FilterBooleanSubmitted slot must be invocable via meta-object"
        );
        QCoreApplication::processEvents();
        QAction *falseAction = findFilterMenuAction(falseOnlyId);
        QVERIFY2(falseAction != nullptr, "false-only Boolean filter must have a menu entry");
        QCOMPARE(falseAction->text(), QStringLiteral("false"));

        model->EndStreaming(false);
    }

    // Shared helper for the ISO/timestamp fixtures. Outside `private slots:`
    // so moc doesn't expose it as a test method.
    static void AssertTimestampFixture(StreamingRun &run, qint64 expectedFirstUtcUs, int rowCount)
    {
        QCOMPARE(run.finishedCount, 1);
        QCOMPARE(run.cancelled, false);
        QCOMPARE(run.model->rowCount(), rowCount);
        QVERIFY(run.model->StreamingErrors().empty());

        const int tsCol = ColumnByHeader(*run.model, QStringLiteral("timestamp"));
        QVERIFY2(tsCol >= 0, "auto-promoted timestamp column must exist");

        const auto &columns = run.model->Configuration().columns;
        QVERIFY(static_cast<size_t>(tsCol) < columns.size());
        QCOMPARE(columns[static_cast<size_t>(tsCol)].type, loglib::LogConfiguration::Type::Time);

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

private:
    MainWindow *mWindow{};
};

QTEST_MAIN(MainWindowTest)
#include "main_window_test.moc"
