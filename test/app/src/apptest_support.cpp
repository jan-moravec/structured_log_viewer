#include "apptest_support.hpp"

#include "log_table_view.hpp"
#include "qt_streaming_log_sink.hpp"

#include <loglib/core/file_line_source.hpp>
#include <loglib/core/stop_token.hpp>
#include <loglib/output/session_bundle.hpp>
#include <loglib/parse/parser_options.hpp>
#include <loglib/parse/parsers/logfmt_parser.hpp>
#include <loglib/time/time_zone_context.hpp>
#include <test_common/wait.hpp>

#include <QCoreApplication>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSettings>
#include <QSignalSpy>
#include <QStandardPaths>

#include <zlib.h>

#include <format>

namespace apptest_support
{

const loglib::TimeZoneContext &TestTimeZone()
{
    static const loglib::TimeZoneContext ZONE = MainWindow::LoadSystemTimeZone();
    return ZONE;
}

void AppTestBase::initTestCase()
{
    // Redirect `QStandardPaths::writableLocation(...)` to a per-user test-only subdirectory so a run never touches the
    // user's real recents profile.
    QStandardPaths::setTestModeEnabled(true);
    // Use a dedicated test identity for QSettings so the recents and open-windows round-trip tests have a writable
    // backing store that does not collide with the real app profile.
    QCoreApplication::setOrganizationName(QStringLiteral("structured-log-viewer-tests"));
    // Each test executable gets its own application name, so executables that run at the same time never share
    // settings or the recents directory.
    QCoreApplication::setApplicationName(QFileInfo(QCoreApplication::applicationFilePath()).completeBaseName());

    // Initialise loglib's timezone database before any test loads a saved configuration containing a time-range
    // filter; mirrors what production `main()` does before constructing the primary window. CMake pins the working
    // directory of every test executable next to the staged `tzdata/` so the app-directory probe always wins.
    QVERIFY2(
        MainWindow::InitializeTimezoneDatabase(),
        "Failed to initialise timezone database; see qCritical above. The staged `tzdata/` directory must live next "
        "to the test binary; run via `ctest --preset <preset>` or invoke the test from its build directory."
    );
}

void AppTestBase::cleanupTestCase()
{
}

void AppTestBase::init()
{
    // Wipe the dedicated test QSettings so recents, open-windows and restore-last values never bleed across tests.
    QSettings().clear();
    // A fresh `ThemeControl` per test mirrors the production `main()` flow: the dependency is constructed after
    // `QApplication` and outlives the window. The themed `MainWindow` overload threads the pointer down into
    // `LogModel`, so theme-aware assertions see real brushes.
    mTheme.reset(new ThemeControl());
    mWindow = new MainWindow(mTheme.data());
}

int AppTestBase::StreamFixtureForColumnTests()
{
    auto *model = mWindow->Model();
    Q_ASSERT(model != nullptr);

    // 200 lines: above `STREAM_PROMOTION_MIN_ROWS` so streaming
    // promotes `level` to enum, but below the static-mode
    // promotion threshold for arbitrary keys.
    const QStringList levels{
        QStringLiteral("info"),
        QStringLiteral("warn"),
        QStringLiteral("error"),
        QStringLiteral("debug"),
    };
    QStringList lines;
    lines.reserve(200);
    for (int i = 0; i < 200; ++i)
    {
        lines.append(QStringLiteral(R"({"category": "%1", "msg": "m%2"})").arg(levels[i % levels.size()]).arg(i));
    }
    const TempJsonFile fixture(lines);
    // Let the parse finish before returning so it doesn't
    // outlive `fixture`'s temp dir.

    QSignalSpy finishedSpy(model, &LogModel::streamingFinished);
    if (!finishedSpy.isValid())
    {
        return -1;
    }
    auto fileSource = std::make_unique<loglib::FileLineSource>(fixture.Path().toStdString());
    loglib::FileLineSource *fileSourcePtr = fileSource.get();
    const loglib::StopToken stopToken = model->BeginStreamingForSyncTest(std::move(fileSource));

    loglib::ParserOptions options;
    options.stopToken = stopToken;
    options.threads = 1;
    const loglib::JsonParser parser;
    parser.Parse(*fileSourcePtr, *model->Sink(), options);
    if (finishedSpy.count() == 0)
    {
        finishedSpy.wait(60000);
    }
    QCoreApplication::processEvents();

    return ColumnByHeader(*model, QStringLiteral("category"));
}

int AppTestBase::StreamFixtureWithTimeColumnForRowMenuTests()
{
    auto *model = mWindow->Model();
    Q_ASSERT(model != nullptr);

    // Use explicit `+00:00` offsets: streaming time-promotion picks
    // them up reliably with only three rows. The `Z` suffix would
    // type the column as Time but leave the value as a raw string.
    const QStringList lines{
        QStringLiteral(R"({"timestamp":"2024-04-28T10:00:01+00:00","msg":"first"})"),
        QStringLiteral(R"({"timestamp":"2024-04-28T10:00:02+00:00","msg":"second"})"),
        QStringLiteral(R"({"timestamp":"2024-04-28T10:00:03+00:00","msg":"third"})"),
    };
    const TempJsonFile fixture(lines);

    QSignalSpy finishedSpy(model, &LogModel::streamingFinished);
    if (!finishedSpy.isValid())
    {
        return -1;
    }
    auto fileSource = std::make_unique<loglib::FileLineSource>(fixture.Path().toStdString());
    loglib::FileLineSource *fileSourcePtr = fileSource.get();
    const loglib::StopToken stopToken = model->BeginStreamingForSyncTest(std::move(fileSource));

    loglib::ParserOptions options;
    options.stopToken = stopToken;
    options.threads = 1;
    const loglib::JsonParser parser;
    parser.Parse(*fileSourcePtr, *model->Sink(), options);
    if (finishedSpy.count() == 0)
    {
        finishedSpy.wait(60000);
    }
    QCoreApplication::processEvents();

    return ColumnByHeader(*model, QStringLiteral("timestamp"));
}

QString AppTestBase::ClickAndReadClipboardWithRetry(
    QClipboard *clipboard, QPushButton *button, const QString &marker, int maxAttempts, int sleepMs
)
{
    QString text;
    for (int attempt = 0; attempt < maxAttempts; ++attempt)
    {
        button->click();
        QCoreApplication::processEvents();
        // Wait up to sleepMs for the clipboard to carry the marker; the next attempt clicks again.
        const bool seen = WaitUntilProcessingEvents(
            [&] {
                text = clipboard->text();
                return marker.isEmpty() ? !text.isEmpty() : text.contains(marker);
            },
            std::chrono::milliseconds(sleepMs)
        );
        if (seen)
        {
            return text;
        }
    }
    return text;
}

QString AppTestBase::InvokeAndReadClipboardWithRetry(
    QClipboard *clipboard, QObject *target, const char *member, const QString &marker, int maxAttempts, int sleepMs
)
{
    QString text;
    for (int attempt = 0; attempt < maxAttempts; ++attempt)
    {
        const bool invoked = QMetaObject::invokeMethod(target, member, Qt::DirectConnection);
        if (!invoked)
        {
            return {};
        }
        QCoreApplication::processEvents();
        // Wait up to sleepMs for the clipboard to carry the marker; the next attempt clicks again.
        const bool seen = WaitUntilProcessingEvents(
            [&] {
                text = clipboard->text();
                return marker.isEmpty() ? !text.isEmpty() : text.contains(marker);
            },
            std::chrono::milliseconds(sleepMs)
        );
        if (seen)
        {
            return text;
        }
    }
    return text;
}

qint64 AppTestBase::NaiveToUtcUs(qint64 wallClockUs)
{
    return TestTimeZone().LocalToUtc(wallClockUs);
}

void AppTestBase::AssertTimestampFixture(StreamingRun &run, qint64 expectedFirstUtcUs, int rowCount)
{
    QCOMPARE(run.finishedCount, 1);
    QCOMPARE(run.cancelled, false);
    QCOMPARE(run.model->rowCount(), rowCount);
    QVERIFY(run.model->StreamingErrors().empty());

    const int tsCol = ColumnByHeader(*run.model, QStringLiteral("timestamp"));
    QVERIFY2(tsCol >= 0, "auto-promoted timestamp column must exist");

    const auto &columns = run.model->Layout().Columns();
    QVERIFY(static_cast<size_t>(tsCol) < columns.size());
    QCOMPARE(columns[static_cast<size_t>(tsCol)].type, loglib::ColumnType::Time);

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

void AppTestBase::cleanup()
{
    delete mWindow;
    mWindow = nullptr;
    // The theme controller must outlive the window during teardown.
    mTheme.reset();
}

void SaveLayoutForTest(const LogModel &model, const std::string &path)
{
    loglib::SaveConfiguration(
        loglib::LogConfiguration{.columns = model.Layout().Columns()}, path, loglib::SaveScope::Full
    );
}

void LoadLayoutForTest(LogModel &model, const std::string &path)
{
    model.MutableLayout().SetColumns(loglib::LoadConfiguration(path).columns);
}

QString WriteBundleFixture(
    const QTemporaryDir &directory,
    const QStringList &lines,
    const std::function<void(loglib::LogConfiguration &)> &configure
)
{
    const TempJsonFile source(lines);
    const loglib::LoadResult loaded = loglib::LoadFile(logapp::QStringToFsPath(source.Path()));
    const loglib::LogTable &table = loaded.table;
    loglib::LogConfiguration configuration{.columns = table.Layout().Columns()};
    configure(configuration);
    for (auto &anchor : configuration.view.anchors)
    {
        if (anchor.locator.empty())
        {
            anchor.locator = source.Path().toStdString();
        }
    }

    const QString path = directory.filePath(QStringLiteral("fixture.slvbundle"));
    loglib::WriteSessionBundle(table, configuration, logapp::QStringToFsPath(path));
    return path;
}

bool PumpUntilStreamingActive(const LogModel *model)
{
    return test_common::WaitUntil(
        [model] {
            QCoreApplication::processEvents();
            return model->IsStreamingActive();
        },
        std::chrono::milliseconds{60000},
        std::chrono::milliseconds{1}
    );
}

QByteArray GzipCompressForTest(const QByteArray &bytes)
{
    z_stream strm{};
    const int initRc = ::deflateInit2(&strm, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY);
    if (initRc != Z_OK)
    {
        qFatal("zlib deflateInit2 must succeed for the test gzip helper (rc=%d)", initRc);
    }
    QByteArray out;
    out.resize(static_cast<qsizetype>(::deflateBound(&strm, static_cast<uLong>(bytes.size()))));
    // zlib's `deflate` takes non-const `next_in`; cast kept for
    // portability across zlib header vintages.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
    strm.next_in = const_cast<Bytef *>(reinterpret_cast<const Bytef *>(bytes.constData()));
    strm.avail_in = static_cast<uInt>(bytes.size());
    strm.next_out = reinterpret_cast<Bytef *>(out.data());
    strm.avail_out = static_cast<uInt>(out.size());
    const int rc = ::deflate(&strm, Z_FINISH);
    if (rc != Z_STREAM_END)
    {
        ::deflateEnd(&strm);
        qFatal("zlib deflate(Z_FINISH) must fully consume the fixture (rc=%d)", rc);
    }
    out.resize(static_cast<qsizetype>(strm.total_out));
    ::deflateEnd(&strm);
    return out;
}

void WriteBinaryForTest(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
    {
        qFatal("test fixture file must be writable: %s", qPrintable(path));
    }
    const qint64 wrote = file.write(bytes);
    if (wrote != static_cast<qint64>(bytes.size()))
    {
        qFatal(
            "short write to test fixture '%s' (%lld of %lld bytes)",
            qPrintable(path),
            static_cast<long long>(wrote),
            static_cast<long long>(bytes.size())
        );
    }
}

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

StreamingRun RunStreaming(const QString &fixturePath, ThemeControl *theme)
{
    StreamingRun run;
    run.model = std::make_unique<LogModel>(nullptr, theme);
    run.model->SetTimeZone(TestTimeZone());
    QSignalSpy finishedSpy(run.model.get(), &LogModel::streamingFinished);

    auto fileSource = std::make_unique<loglib::FileLineSource>(fixturePath.toStdString());
    loglib::FileLineSource *fileSourcePtr = fileSource.get();
    const loglib::StopToken stopToken = run.model->BeginStreamingForSyncTest(std::move(fileSource));

    {
        loglib::ParserOptions options;
        options.stopToken = stopToken;
        options.threads = 1;

        const loglib::JsonParser parser;
        parser.Parse(*fileSourcePtr, *run.model->Sink(), options);
    }

    if (finishedSpy.count() == 0)
    {
        finishedSpy.wait(60000);
    }
    run.finishedCount = finishedSpy.count();
    if (run.finishedCount > 0)
    {
        const auto result = finishedSpy.takeFirst().value(0).value<StreamingResult>();
        run.cancelled = (result == StreamingResult::Cancelled);
    }
    return run;
}

StreamingRun RunStreamingLogfmt(const QString &fixturePath, ThemeControl *theme)
{
    StreamingRun run;
    run.model = std::make_unique<LogModel>(nullptr, theme);
    run.model->SetTimeZone(TestTimeZone());
    QSignalSpy finishedSpy(run.model.get(), &LogModel::streamingFinished);

    auto fileSource = std::make_unique<loglib::FileLineSource>(fixturePath.toStdString());
    loglib::FileLineSource *fileSourcePtr = fileSource.get();
    const loglib::StopToken stopToken = run.model->BeginStreamingForSyncTest(std::move(fileSource));

    {
        loglib::ParserOptions options;
        options.stopToken = stopToken;
        options.threads = 1;

        loglib::LogfmtParser{}.Parse(*fileSourcePtr, *run.model->Sink(), options);
    }

    if (finishedSpy.count() == 0)
    {
        finishedSpy.wait(60000);
    }
    run.finishedCount = finishedSpy.count();
    if (run.finishedCount > 0)
    {
        const auto result = finishedSpy.takeFirst().value(0).value<StreamingResult>();
        run.cancelled = (result == StreamingResult::Cancelled);
    }
    return run;
}

std::unique_ptr<LogModel> RunStaticLoadInModel(const QString &fixturePath, const loglib::LogParser &parser)
{
    auto model = std::make_unique<LogModel>(nullptr, nullptr);
    model->SetTimeZone(TestTimeZone());
    QSignalSpy finishedSpy(model.get(), &LogModel::streamingFinished);

    auto fileSource = std::make_unique<loglib::FileLineSource>(fixturePath.toStdString());
    loglib::FileLineSource *fileSourcePtr = fileSource.get();
    const loglib::StopToken stopToken = model->BeginStreamingForSyncTest(std::move(fileSource));

    loglib::ParserOptions options;
    options.stopToken = stopToken;
    parser.Parse(*fileSourcePtr, *model->Sink(), options);

    if (finishedSpy.count() == 0)
    {
        finishedSpy.wait(60000);
    }
    return model;
}

QString WriteEquivalenceFixture(const QTemporaryDir &directory, const QString &name, const std::string &content)
{
    const QString path = directory.filePath(name);
    std::ofstream stream(path.toStdString(), std::ios::binary);
    stream << content;
    return path;
}

EquivalenceFixtures MakeEquivalenceFixtures()
{
    constexpr int ROWS = 40000;
    EquivalenceFixtures fixtures;
    fixtures.csv = "timestamp,level,service,n,msg\n";
    const char *levels[] = {"info", "warn", "error", "debug"};
    for (int i = 0; i < ROWS; ++i)
    {
        const std::string time =
            "2026-02-03T04:05:" + std::string(i % 60 < 10 ? "0" : "") + std::to_string(i % 60) + "Z";
        const std::string level = levels[i % 4];
        const std::string service = "svc" + std::to_string(i % 7);
        const std::string message = "message number " + std::to_string(i);
        const bool late = i % 5 == 0;
        // A key that first appears after 1000 rows, so its type is decided in a later batch than the others.
        const bool afterThousand = i >= 1500 && i % 3 == 0;

        fixtures.json += std::format(
            "{{\"timestamp\":\"{}\",\"level\":\"{}\",\"service\":\"{}\",\"n\":{},\"msg\":\"{}\"",
            time,
            level,
            service,
            i,
            message
        );
        if (late)
        {
            fixtures.json += std::format(",\"late\":{}", i * 2);
        }
        if (afterThousand)
        {
            fixtures.json += std::format(",\"retry\":{}", i % 11);
        }
        fixtures.json += "}\n";

        fixtures.logfmt +=
            std::format("timestamp={} level={} service={} n={} msg=\"{}\"", time, level, service, i, message);
        if (late)
        {
            fixtures.logfmt += std::format(" late={}", i * 2);
        }
        if (afterThousand)
        {
            fixtures.logfmt += std::format(" retry={}", i % 11);
        }
        fixtures.logfmt += '\n';

        fixtures.csv += std::format("{},{},{},{},{}\n", time, level, service, i, message);
    }
    return fixtures;
}

std::unique_ptr<loglib::LogParser> ThrowingParserFactory()
{
    throw std::runtime_error("factory failed");
}

bool WaitForLineCount(LogModel &model, qsizetype target, std::chrono::milliseconds deadline)
{
    return WaitUntilProcessingEvents([&] { return model.rowCount() >= target; }, deadline);
}

int ColumnByHeader(const LogModel &model, const QString &header)
{
    const std::string needle = header.toStdString();
    const auto &columns = model.Layout().Columns();
    for (size_t col = 0; col < columns.size(); ++col)
    {
        if (columns[col].header == needle)
        {
            return static_cast<int>(col);
        }
    }
    return -1;
}

loglib::StreamLineSource &BeginSyntheticStreamSession(LogModel &model)
{
    auto streamSource = std::make_unique<loglib::StreamLineSource>(std::filesystem::path("synthetic"), nullptr);
    loglib::StreamLineSource *streamPtr = streamSource.get();
    static_cast<void>(model.BeginStreamingForSyncTest(std::move(streamSource)));
    return *streamPtr;
}

loglib::StreamLineSource &BeginSyntheticStaticSession(LogModel &model)
{
    return BeginSyntheticStreamSession(model);
}

loglib::RowBatch MakeSyntheticBatch(
    loglib::StreamLineSource &streamSource, loglib::KeyId valueKey, size_t firstLineId, size_t count, bool declareNewKey
)
{
    loglib::RowBatch batch;
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
        std::vector<loglib::CompactLogValue> compactValues;
        compactValues.push_back(loglib::CompactLogValue::MakeInt64(static_cast<int64_t>(lineId)).WithKey(valueKey));
        batch.lines.emplace_back(std::move(compactValues), streamSource, lineId);
    }
    return batch;
}

QAction *FindActionByObjectName(QMainWindow *window, const QString &name)
{
    return window->findChild<QAction *>(name);
}

} // namespace apptest_support
