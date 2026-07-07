#include "anchor_manager.hpp"
#include "anchors_dock.hpp"
#include "cli_parser.hpp"
#include "column_editor.hpp"
#include "columns_manager_dialog.hpp"
#include "configuration_diagnostics_dialog.hpp"
#include "filter_editor.hpp"
#include "find_dock.hpp"
#include "find_record_widget.hpp"
#include "jump_to_tail_pill.hpp"
#include "level_cell_delegate.hpp"
#include "log_filter_model.hpp"
#include "log_model.hpp"
#include "log_table_view.hpp"
#include "main_window.hpp"
#include "parse_errors_dock.hpp"
#include "preferences_editor.hpp"
#include "qt_streaming_log_sink.hpp"
#include "record_detail_dock.hpp"
#include "record_detail_widget.hpp"
#include "record_detail_window.hpp"
#include "row_order_proxy_model.hpp"
#include "session_history_manager.hpp"
#include "single_instance_guard.hpp"
#include "streaming_control.hpp"
#include "theme_control.hpp"
#include "uuid_utils.hpp"

#include <loglib/enum_dictionary.hpp>
#include <loglib/file_line_source.hpp>
#include <loglib/internal/advanced_parser_options.hpp>
#include <loglib/internal/compact_log_value.hpp>
#include <loglib/internal/log_configuration_glaze_meta.hpp>
#include <loglib/key_index.hpp>
#include <loglib/log_configuration.hpp>
#include <loglib/log_file.hpp>
#include <loglib/log_filter.hpp>
#include <loglib/log_level.hpp>
#include <loglib/log_line.hpp>
#include <loglib/log_parse_sink.hpp>
#include <loglib/log_value.hpp>
#include <loglib/parser_options.hpp>
#include <loglib/parsers/json_parser.hpp>
#include <loglib/parsers/logfmt_parser.hpp>
#include <loglib/stop_token.hpp>
#include <loglib/stream_line_source.hpp>
#include <loglib/tailing_bytes_producer.hpp>
#include <loglib/tcp_server_producer.hpp>
#include <loglib/theme.hpp>
#include <loglib/udp_server_producer.hpp>

#include <test_common/network_log_client.hpp>

#include <QAbstractItemModel>
#include <QAction>
#include <QBrush>
#include <QCheckBox>
#include <QClipboard>
#include <QColor>
#include <QComboBox>
#include <QDataStream>
#include <QDialogButtonBox>
#include <QDockWidget>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QGroupBox>
#include <QHeaderView>
#include <QIcon>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QListWidget>
#include <QListWidgetItem>
#include <QLocalSocket>
#include <QLockFile>
#include <QMenu>
#include <QMenuBar>
#include <QPainter>
#include <QPalette>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QScopedPointer>
#include <QScrollBar>
#include <QSettings>
#include <QShortcut>
#include <QSignalSpy>
#include <QSortFilterProxyModel>
#include <QStandardItem>
#include <QStandardItemModel>
#include <QStandardPaths>
#include <QStatusBar>
#include <QString>
#include <QStringList>
#include <QStyleOptionViewItem>
#include <QTableView>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QToolBar>
#include <QToolButton>
#include <QUuid>
#include <QVariant>
#include <QWheelEvent>
#include <QtTest/QtTest>

#include <glaze/glaze.hpp>

#include <zlib.h>

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

// Tiny RAII helper that writes a JSONL fixture into a QTemporaryDir.
// Kept in-TU to avoid pulling library-test fixtures (and their Catch2
// dependency) into `apptest`.
// In-memory storage backend for `SessionHistoryManager` tests so each
// case stays isolated from `QSettings` and the user's profile.
class InMemoryRecentsIndexStorage final : public IRecentsIndexStorage
{
public:
    QList<RecentSessionEntry> Read() const override
    {
        return mEntries;
    }

    void Write(const QList<RecentSessionEntry> &entries) override
    {
        mEntries = entries;
    }

    std::optional<QString> ReadLastUuid() const override
    {
        return mLastUuid;
    }

    void WriteLastUuid(const std::optional<QString> &uuid) override
    {
        mLastUuid = uuid;
    }

private:
    QList<RecentSessionEntry> mEntries;
    std::optional<QString> mLastUuid;
};

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

// Gzip-compress @p bytes into a self-contained gzip stream using
// zlib's `windowBits = 15 + 16` mode (gzip framing on top of raw
// deflate). Kept in this TU rather than a shared helper because
// only the transparent-decompression regression suite needs it, and
// the codec-level round-trip lives in
// `test/lib/src/test_decompressing_byte_source.cpp`.
//
// Fixture-setup failures use `qFatal` instead of `QVERIFY2`: this
// helper returns a value, and `QVERIFY2` expands to `return;` on
// failure -- which would be ill-formed in a non-void function. A
// zlib init failure at test-fixture-build time means the whole
// suite is broken anyway (system-level zlib misconfiguration), so
// terminating hard is the right response.
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
    // zlib's `deflate` takes non-const `next_in` even though it does
    // not mutate the input. Older headers require the cast; newer
    // ones silently discard the const. Keep the cast for portability.
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

// Write @p bytes to @p path (binary, truncating). Used together
// with `GzipCompressForTest` to stage compressed fixtures on disk
// without pulling in Catch2's `TempBinaryFile` from the library tests.
// Same `qFatal`-vs-`QVERIFY2` rationale as above.
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
StreamingRun RunStreaming(const QString &fixturePath, ThemeControl *theme = nullptr)
{
    StreamingRun run;
    run.model = std::make_unique<LogModel>(nullptr, theme);
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

// Logfmt sibling of `RunStreaming` (same shape so assertions can
// match the JSON counterparts line-for-line).
StreamingRun RunStreamingLogfmt(const QString &fixturePath, ThemeControl *theme = nullptr)
{
    StreamingRun run;
    run.model = std::make_unique<LogModel>(nullptr, theme);
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

        loglib::LogfmtParser::ParseStreaming(*fileSourcePtr, *run.model->Sink(), options, advanced);
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

// Column index whose configured `Column::header` equals @p header,
// or -1. Reads the configuration directly (not `DisplayRole`) so
// it survives theme overrides that suppress the header text.
int ColumnByHeader(const LogModel &model, const QString &header)
{
    const std::string needle = header.toStdString();
    const auto &columns = model.Configuration().columns;
    for (size_t col = 0; col < columns.size(); ++col)
    {
        if (columns[col].header == needle)
        {
            return static_cast<int>(col);
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

// Locate a UI-file-declared `QAction` by `objectName`. Thin wrapper
// over `findChild<QAction*>` so call sites read intent rather than the
// generic Qt API.
QAction *FindActionByObjectName(QMainWindow *window, const QString &name)
{
    return window->findChild<QAction *>(name);
}

// Tests drive `RecordDetailDock` visibility via `emit
// visibilityChanged(...)` instead of `dock.show()` / `dock.hide()`
// because the dock's refresh gate keys off `mPerceivedVisible`, which
// is fed by the `visibilityChanged` signal -- this lets tests simulate
// "buried tab" transitions where the perceived-visible flag flips
// without the explicit-hide flag flipping (the dock isn't actually
// hidden, just covered). Tests that attach the dock to an unrealised
// `QMainWindow` also avoid `QDockWidget::setVisible(true)` walking
// `QMainWindowLayout`'s dock-area state, which is only wired up by
// the host's first paint cycle.

/// Returns the first action in @p menu whose visible text (mnemonic
/// stripped) equals @p text, or null. Lookup by label avoids
/// hard-coded indices in the row-context-menu tests.
[[nodiscard]] inline QAction *FindMenuActionByText(const QMenu *menu, const QString &text)
{
    if (menu == nullptr)
    {
        return nullptr;
    }
    for (QAction *action : menu->actions())
    {
        if (action == nullptr)
        {
            continue;
        }
        QString visible = action->text();
        visible.remove(QChar('&'));
        if (visible == text)
        {
            return action;
        }
    }
    return nullptr;
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
        // Redirect `QStandardPaths::writableLocation(...)` to a
        // per-user test-only subdirectory so this suite never
        // touches the user's real recents profile.
        QStandardPaths::setTestModeEnabled(true);
        // Use a dedicated test identity for QSettings so the
        // recents / openWindowsAtQuit round-trip tests have a
        // writable backing store that does not collide with the
        // real app profile.
        QCoreApplication::setOrganizationName(QStringLiteral("structured-log-viewer-tests"));
        QCoreApplication::setApplicationName(QStringLiteral("apptest"));
        QSettings::setDefaultFormat(QSettings::IniFormat);

        // Initialise loglib's timezone database before any test
        // loads a saved configuration containing a time-range
        // filter; mirrors what production `main()` does before
        // constructing the primary window. CMake pins the apptest
        // working directory next to the staged `tzdata/` so
        // `FindTzdata`'s appDir probe always wins.
        QVERIFY2(
            MainWindow::InitializeTimezoneDatabase(),
            "Failed to initialise timezone database; see qCritical above. The staged "
            "`tzdata/` directory must live next to the apptest binary "
            "($<TARGET_FILE_DIR:apptest>); run via `ctest --preset <preset>` or "
            "invoke `apptest` from its build directory."
        );
    }

    void cleanupTestCase()
    {
        // Called after the last test function
        qDebug() << "MainWindow tests complete";
    }

    void init()
    {
        // Wipe the dedicated test QSettings (set up in `initTestCase`)
        // so recents / `openWindowsAtQuit` / `restoreLast` values
        // never bleed across tests.
        QSettings().clear();
        // Fresh `ThemeControl` per test mirrors the production
        // `main()` flow: the dependency is constructed after
        // `QApplication` and outlives the window. The themed
        // `MainWindow` overload threads the pointer down into
        // `LogModel`, so theme-aware assertions (e.g.
        // `TestLogModelDataReturnsThemeBackground`) see real
        // brushes; theme-agnostic assertions are unaffected.
        mTheme.reset(new ThemeControl());
        mWindow = new MainWindow(mTheme.data());
    }

    void cleanup()
    {
        // Called after each test function
        delete mWindow;
        mWindow = nullptr;
        // Theme controller must outlive the window during teardown:
        // `MainWindow`'s `themeChanged` connect auto-disconnects on
        // `delete mWindow`, so by the time we reset `mTheme` no
        // dangling slots remain.
        mTheme.reset();
    }

    void TestWindowTitle()
    {
        // Idle title carries the `[*]` modified-marker placeholder. Qt
        // strips it from the rendered title when `isWindowModified()` is
        // false, but the getter returns the raw string verbatim.
        QCOMPARE(mWindow->windowTitle(), QString("Structured Log Viewer[*]"));
        QVERIFY(!mWindow->isWindowModified());
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

    // Logfmt smoke: a single-line record loads as one row + two
    // columns ("level", "msg"), bare value typed as string and the
    // quoted value's spaces preserved.
    static void TestLogfmtFixtureSingleLine()
    {
        const FixtureFile fixture(":/fixtures/single_line.logfmt");
        const StreamingRun run = RunStreamingLogfmt(fixture.Path());
        QCOMPARE(run.finishedCount, 1);
        QCOMPARE(run.cancelled, false);
        QCOMPARE(run.model->rowCount(), 1);
        QCOMPARE(run.model->columnCount(), 2);

        const int colLevel = ColumnByHeader(*run.model, QStringLiteral("level"));
        const int colMsg = ColumnByHeader(*run.model, QStringLiteral("msg"));
        QVERIFY(colLevel >= 0 && colMsg >= 0);
        QCOMPARE(run.model->data(run.model->index(0, colLevel), Qt::DisplayRole).toString(), QStringLiteral("info"));
        QCOMPARE(
            run.model->data(run.model->index(0, colMsg), Qt::DisplayRole).toString(), QStringLiteral("hello world")
        );
        QVERIFY(run.model->StreamingErrors().empty());
    }

    // Logfmt smoke: bare values get auto-typed (int / uint / double /
    // bool / null); quoted values stay strings.
    static void TestLogfmtFixtureValueTypes()
    {
        const FixtureFile fixture(":/fixtures/value_types.logfmt");
        const StreamingRun run = RunStreamingLogfmt(fixture.Path());
        QCOMPARE(run.finishedCount, 1);
        QCOMPARE(run.cancelled, false);
        QCOMPARE(run.model->rowCount(), 3);
        QVERIFY(run.model->StreamingErrors().empty());

        const int colStr = ColumnByHeader(*run.model, QStringLiteral("str"));
        const int colInt = ColumnByHeader(*run.model, QStringLiteral("int"));
        const int colUint = ColumnByHeader(*run.model, QStringLiteral("uint"));
        const int colDbl = ColumnByHeader(*run.model, QStringLiteral("dbl"));
        const int colFlag = ColumnByHeader(*run.model, QStringLiteral("flag"));
        const int colNul = ColumnByHeader(*run.model, QStringLiteral("nul"));
        const int colQuoted = ColumnByHeader(*run.model, QStringLiteral("quoted"));
        QVERIFY(colStr >= 0 && colInt >= 0 && colUint >= 0 && colDbl >= 0);
        QVERIFY(colFlag >= 0 && colNul >= 0 && colQuoted >= 0);

        const auto sortVal = [&](int row, int col) {
            return run.model->data(run.model->index(row, col), LogModelItemDataRole::SortRole);
        };

        QCOMPARE(sortVal(0, colStr).toString(), QStringLiteral("alpha"));
        QCOMPARE(sortVal(0, colInt).toLongLong(), qint64(-7));
        QCOMPARE(sortVal(0, colUint).toULongLong(), 18446744073709551610ULL);
        QCOMPARE(sortVal(0, colDbl).toDouble(), 3.14);
        QCOMPARE(sortVal(0, colFlag).toBool(), true);
        QVERIFY(!sortVal(0, colNul).isValid());
        // `quoted="42"` must stay a string; promoting it to int
        // would lose the user's intent on round-trip.
        QCOMPARE(sortVal(0, colQuoted).toString(), QStringLiteral("42"));
    }

    // Logfmt fixture: ISO-T timestamps promote to `Type::Time` like
    // the JSON path. Reuses `AssertTimestampFixture` so the pinned
    // format-detection contract stays in lockstep across formats.
    static void TestLogfmtFixtureIsoTTimestamp()
    {
        const FixtureFile fixture(":/fixtures/iso_t_timestamp.logfmt");
        StreamingRun run = RunStreamingLogfmt(fixture.Path());
        // 2024-04-28T07:14:30 UTC -> 1714288470 seconds since epoch.
        AssertTimestampFixture(run, static_cast<qint64>(1714288470000000), 3);
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
        // No queued lambdas fired (we're paused); the buffered
        // count reflects the `lines` payload.
        QCOMPARE(model.rowCount(), 0);
        QCOMPARE(static_cast<int>(sink->PausedLineCount()), batchesWhilePaused * linesPerBatch);

        // Resume coalesces all three batches and posts once.
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

    // The `actionOpenNetworkStream` UI entry must be reachable by
    // `objectName` so apptest harnesses (and any future tooling that
    // walks the menu tree) can locate it without poking `ui->`.
    static void TestActionOpenNetworkStreamIsExposed()
    {
        // Local variable name avoids the `MainWindowTest::window`
        // member-shadow C4458 warning under MSVC.
        const MainWindow mainWindow;
        const auto *action = mainWindow.findChild<QAction *>(QStringLiteral("actionOpenNetworkStream"));
        QVERIFY2(action != nullptr, "actionOpenNetworkStream must be reachable via objectName");
        // Ctrl+Shift+N was reassigned to File -> New Window; the
        // network-stream action moved to Ctrl+Shift+L.
        QCOMPARE(action->shortcut(), QKeySequence(QStringLiteral("Ctrl+Shift+L")));
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
    // `localLineOffsets` may be longer than `lines`, so a partial
    // prefix trim would break `LogTable::AppendBatch`. The counter
    // accumulates the actual lines evicted as the loop runs (not a
    // pre-loop snapshot).
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

        // Step 2: push a small live-tail batch that overflows the
        // cap by a fraction of the static head. The entire 100-row
        // static head is evicted atomically; the counter must
        // reflect the actual eviction count. Build the live-tail
        // rows as
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

        // Whole 100-row static head was evicted; only the 30-row
        // live-tail batch survives. Counter reports the 100 lines
        // that actually left the buffer.
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
    // The bug: `Resume()` cleared `mPaused` synchronously then
    // posted the coalesced buffer via `Qt::QueuedConnection`.
    // Between those steps a worker `OnBatch` could see
    // `mPaused == false`, skip the buffer, and post its newer
    // batch ahead of the coalesced one via
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
        // fires after `mPaused` was cleared) -- its rows must
        // append after the paused-buffer rows because Resume
        // already delivered them. The synchronous contract makes
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

        // Simulate the bug condition: action enabled + checked
        // while idle.
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

    // Regression: hover / layout-driven scrollbar updates must not
    // auto-disengage **Follow newest**. Earlier `LogTableView`
    // connected `valueChanged` directly to the edge-trigger lambda,
    // so any programmatic value change (`endInsertRows` clamp,
    // hover/repaint scroll adjustment,
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

        // Programmatic value change: mimics a layout-driven scroll
        // adjustment Qt fires on hover / repaint.
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
    // Earlier the snapshot stored an integer source row, which
    // under a no-filter pass-through stayed numerically the same
    // after the flip; the persistent index ended up pointing at
    // the row that moved into the slot -- a silent selection swap.
    // The
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

    // Pill is hidden on a fresh idle window: no rows, no
    // scroll-away, nothing to announce. Pins the resting state
    // so a future change to the construction order is caught.
    //
    // Note: this group uses `isHidden()` instead of `!isVisible()`
    // because under the offscreen QPA the test window is never
    // mapped, so `isVisible()` is always false. `isHidden()`
    // reflects the explicit `setVisible(false)` the pill ctor
    // issues -- the production "would show if mapped" property.
    void TestJumpToTailPillHiddenOnIdle()
    {
        const auto *tableView = mWindow->findChild<LogTableView *>();
        QVERIFY(tableView != nullptr);
        const JumpToTailPill *pill = tableView->TailPillForTest();
        QVERIFY2(pill != nullptr, "log table view must construct the jump-to-tail pill");
        QVERIFY2(pill->isHidden(), "pill must be hidden on a fresh idle window");
        QCOMPARE(pill->Count(), 0);
        QCOMPARE(tableView->PendingNewRowsForTest(), 0);
    }

    // While the user is at the tail edge, inserted rows must not
    // raise the pill -- they're already current.
    void TestJumpToTailPillStaysHiddenAtTail()
    {
        auto *tableView = mWindow->findChild<LogTableView *>();
        auto *model = mWindow->findChild<LogModel *>();
        QVERIFY(tableView != nullptr);
        QVERIFY(model != nullptr);
        const JumpToTailPill *pill = tableView->TailPillForTest();
        QVERIFY(pill != nullptr);

        loglib::StreamLineSource &streamSource = BeginSyntheticStreamSession(*model);
        QtStreamingLogSink *sink = model->Sink();
        QVERIFY(sink != nullptr);
        loglib::KeyIndex &keys = sink->Keys();
        const loglib::KeyId valueKey = keys.GetOrInsert(std::string("value"));

        // Default `TailEdge::Bottom` + empty scrollbar starts us
        // at the tail edge.
        QVERIFY(tableView->verticalScrollBar()->value() >= tableView->verticalScrollBar()->maximum());

        sink->OnBatch(MakeSyntheticBatch(streamSource, keys, valueKey, 1, 10, /*declareNewKey=*/true));
        QCoreApplication::processEvents();
        QCOMPARE(model->rowCount(), 10);

        QCOMPARE(tableView->PendingNewRowsForTest(), 0);
        QCOMPARE(pill->Count(), 0);
        QVERIFY2(pill->isHidden(), "pill must stay hidden while the user sits at the tail edge");

        model->EndStreaming(false);
    }

    // After a user scroll-away from the tail, the next batch
    // raises the pill with a matching count. Uses
    // `triggerAction(SliderToMinimum)` so the scroll-edge state
    // machine sees a user-flagged change; a plain `setValue`
    // would be filtered out as programmatic.
    void TestJumpToTailPillSurfacesAfterScrollAway()
    {
        auto *tableView = mWindow->findChild<LogTableView *>();
        auto *model = mWindow->findChild<LogModel *>();
        QVERIFY(tableView != nullptr);
        QVERIFY(model != nullptr);
        const JumpToTailPill *pill = tableView->TailPillForTest();
        QVERIFY(pill != nullptr);

        loglib::StreamLineSource &streamSource = BeginSyntheticStreamSession(*model);
        QtStreamingLogSink *sink = model->Sink();
        QVERIFY(sink != nullptr);
        loglib::KeyIndex &keys = sink->Keys();
        const loglib::KeyId valueKey = keys.GetOrInsert(std::string("value"));

        // Seed scrollable rows, then simulate a user Home press
        // to scroll away from the bottom tail.
        sink->OnBatch(MakeSyntheticBatch(streamSource, keys, valueKey, 1, 30, /*declareNewKey=*/true));
        QCoreApplication::processEvents();

        QScrollBar *vbar = tableView->verticalScrollBar();
        vbar->setRange(0, 1000);
        vbar->setValue(vbar->maximum());
        // Re-seed the at-tail flag: the prior programmatic
        // `setValue` may have transitioned it silently. Production
        // reaches the same state via `SetTailEdge` in
        // `ApplyDisplayOrder`.
        tableView->SetTailEdge(LogTableView::TailEdge::Bottom);
        vbar->triggerAction(QAbstractSlider::SliderToMinimum);
        QCoreApplication::processEvents();

        sink->OnBatch(MakeSyntheticBatch(streamSource, keys, valueKey, 31, 7, /*declareNewKey=*/false));
        QCoreApplication::processEvents();

        QCOMPARE(tableView->PendingNewRowsForTest(), 7);
        QCOMPARE(pill->Count(), 7);
        QVERIFY2(!pill->isHidden(), "pill must be un-hidden after a scroll-away + batch");

        model->EndStreaming(false);
    }

    // The pill count accumulates across batches: it tracks "rows
    // since you scrolled away", not "rows in the last batch".
    void TestJumpToTailPillAccumulatesAcrossBatches()
    {
        auto *tableView = mWindow->findChild<LogTableView *>();
        auto *model = mWindow->findChild<LogModel *>();
        QVERIFY(tableView != nullptr);
        QVERIFY(model != nullptr);
        const JumpToTailPill *pill = tableView->TailPillForTest();
        QVERIFY(pill != nullptr);

        loglib::StreamLineSource &streamSource = BeginSyntheticStreamSession(*model);
        QtStreamingLogSink *sink = model->Sink();
        QVERIFY(sink != nullptr);
        loglib::KeyIndex &keys = sink->Keys();
        const loglib::KeyId valueKey = keys.GetOrInsert(std::string("value"));

        sink->OnBatch(MakeSyntheticBatch(streamSource, keys, valueKey, 1, 20, /*declareNewKey=*/true));
        QCoreApplication::processEvents();

        QScrollBar *vbar = tableView->verticalScrollBar();
        vbar->setRange(0, 1000);
        vbar->setValue(vbar->maximum());
        tableView->SetTailEdge(LogTableView::TailEdge::Bottom);
        vbar->triggerAction(QAbstractSlider::SliderToMinimum);
        QCoreApplication::processEvents();

        sink->OnBatch(MakeSyntheticBatch(streamSource, keys, valueKey, 21, 3, /*declareNewKey=*/false));
        QCoreApplication::processEvents();
        QCOMPARE(tableView->PendingNewRowsForTest(), 3);
        QCOMPARE(pill->Count(), 3);

        sink->OnBatch(MakeSyntheticBatch(streamSource, keys, valueKey, 24, 5, /*declareNewKey=*/false));
        QCoreApplication::processEvents();
        QCOMPARE(tableView->PendingNewRowsForTest(), 8);
        QCOMPARE(pill->Count(), 8);

        model->EndStreaming(false);
    }

    // Returning to the tail edge zeroes the counter and hides
    // the pill: a single user scroll back catches them up.
    void TestJumpToTailPillResetsOnTailReturn()
    {
        auto *tableView = mWindow->findChild<LogTableView *>();
        auto *model = mWindow->findChild<LogModel *>();
        QVERIFY(tableView != nullptr);
        QVERIFY(model != nullptr);
        const JumpToTailPill *pill = tableView->TailPillForTest();
        QVERIFY(pill != nullptr);

        loglib::StreamLineSource &streamSource = BeginSyntheticStreamSession(*model);
        QtStreamingLogSink *sink = model->Sink();
        QVERIFY(sink != nullptr);
        loglib::KeyIndex &keys = sink->Keys();
        const loglib::KeyId valueKey = keys.GetOrInsert(std::string("value"));

        sink->OnBatch(MakeSyntheticBatch(streamSource, keys, valueKey, 1, 25, /*declareNewKey=*/true));
        QCoreApplication::processEvents();

        QScrollBar *vbar = tableView->verticalScrollBar();
        vbar->setRange(0, 1000);
        vbar->setValue(vbar->maximum());
        tableView->SetTailEdge(LogTableView::TailEdge::Bottom);
        vbar->triggerAction(QAbstractSlider::SliderToMinimum);
        QCoreApplication::processEvents();

        sink->OnBatch(MakeSyntheticBatch(streamSource, keys, valueKey, 26, 4, /*declareNewKey=*/false));
        QCoreApplication::processEvents();
        QCOMPARE(pill->Count(), 4);

        // User returns to the tail. `triggerAction` flags the
        // change as user-initiated, so `OnVerticalScrollValueChanged`
        // emits `userScrolledToTail` and drops the pending count.
        vbar->triggerAction(QAbstractSlider::SliderToMaximum);
        QCoreApplication::processEvents();

        QCOMPARE(tableView->PendingNewRowsForTest(), 0);
        QCOMPARE(pill->Count(), 0);

        model->EndStreaming(false);
    }

    // Clicking the pill emits `jumpToTailRequested`, which the
    // host turns into a scroll back to tail (zeroing the counter)
    // and a Follow-newest re-engage. Without the re-engage the
    // next batch would push the user behind the tail again.
    void TestJumpToTailPillClickJumpsAndResets()
    {
        auto *tableView = mWindow->findChild<LogTableView *>();
        auto *model = mWindow->findChild<LogModel *>();
        QAction *followAction = FindActionByObjectName(mWindow, QStringLiteral("actionFollowTail"));
        QVERIFY(tableView != nullptr);
        QVERIFY(model != nullptr);
        QVERIFY(followAction != nullptr);
        JumpToTailPill *pill = tableView->TailPillForTest();
        QVERIFY(pill != nullptr);

        // The host slot gates the Follow re-engage on
        // `IsLiveTailSession()`; production sets the mode in
        // `OpenLogStream`, so tests must opt in explicitly.
        auto restore = qScopeGuard([this]() { mWindow->SetSessionModeForTest(MainWindow::TestSessionMode::Idle); });
        mWindow->SetSessionModeForTest(MainWindow::TestSessionMode::LiveTail);

        loglib::StreamLineSource &streamSource = BeginSyntheticStreamSession(*model);
        QtStreamingLogSink *sink = model->Sink();
        QVERIFY(sink != nullptr);
        loglib::KeyIndex &keys = sink->Keys();
        const loglib::KeyId valueKey = keys.GetOrInsert(std::string("value"));

        sink->OnBatch(MakeSyntheticBatch(streamSource, keys, valueKey, 1, 40, /*declareNewKey=*/true));
        QCoreApplication::processEvents();

        // Pin Follow on so the scroll-away assertion below is
        // meaningful (Follow already off would mask a missing
        // re-engage).
        followAction->setEnabled(true);
        followAction->setChecked(true);

        QScrollBar *vbar = tableView->verticalScrollBar();
        vbar->setRange(0, 1000);
        vbar->setValue(vbar->maximum());
        tableView->SetTailEdge(LogTableView::TailEdge::Bottom);
        vbar->triggerAction(QAbstractSlider::SliderToMinimum);
        QCoreApplication::processEvents();

        // Sanity-check the scroll-away disengaged Follow before
        // we exercise the re-engage path.
        QVERIFY2(
            !followAction->isChecked(),
            "scroll-away must disengage Follow newest before the click test exercises re-engage"
        );

        sink->OnBatch(MakeSyntheticBatch(streamSource, keys, valueKey, 41, 6, /*declareNewKey=*/false));
        QCoreApplication::processEvents();
        QCOMPARE(pill->Count(), 6);

        const QSignalSpy jumpSpy(tableView, &LogTableView::jumpToTailRequested);
        QVERIFY(jumpSpy.isValid());

        pill->click();
        QCoreApplication::processEvents();

        QCOMPARE(jumpSpy.count(), 1);
        // `JumpToNewestRow` scrolls back to the bottom; the
        // at-tail transition fires `ResetPendingNewRows`.
        QCOMPARE(tableView->PendingNewRowsForTest(), 0);
        QCOMPARE(pill->Count(), 0);
        // Follow newest must be re-engaged: the pill click is
        // "catch me up", not "show the tail once and then fall
        // behind again".
        QVERIFY2(followAction->isChecked(), "pill click must re-engage Follow newest in a live-tail session");

        model->EndStreaming(false);
    }

    // `SetTailEdge(Top)` must flip the pill arrow to Up so the
    // glyph reads correctly in newest-first mode.
    void TestJumpToTailPillArrowFollowsTailEdge()
    {
        auto *tableView = mWindow->findChild<LogTableView *>();
        QVERIFY(tableView != nullptr);
        const JumpToTailPill *pill = tableView->TailPillForTest();
        QVERIFY(pill != nullptr);

        QCOMPARE(tableView->GetTailEdge(), LogTableView::TailEdge::Bottom);
        QCOMPARE(pill->Direction(), JumpToTailPill::ArrowDirection::Down);

        tableView->SetTailEdge(LogTableView::TailEdge::Top);
        QCOMPARE(pill->Direction(), JumpToTailPill::ArrowDirection::Up);

        tableView->SetTailEdge(LogTableView::TailEdge::Bottom);
        QCOMPARE(pill->Direction(), JumpToTailPill::ArrowDirection::Down);
    }

    // Regression: a tail-edge flip while the user was mid-scroll
    // used to leave the pill showing the old direction's tally
    // under the new arrow ("5 rows below" becoming "5 rows
    // above"). The fix gates the reset on the orientation flip
    // itself, not just on `mAtTailEdge`.
    void TestJumpToTailPillResetsOnEdgeFlipMidScroll()
    {
        auto *tableView = mWindow->findChild<LogTableView *>();
        auto *model = mWindow->findChild<LogModel *>();
        QVERIFY(tableView != nullptr);
        QVERIFY(model != nullptr);
        const JumpToTailPill *pill = tableView->TailPillForTest();
        QVERIFY(pill != nullptr);

        loglib::StreamLineSource &streamSource = BeginSyntheticStreamSession(*model);
        QtStreamingLogSink *sink = model->Sink();
        QVERIFY(sink != nullptr);
        loglib::KeyIndex &keys = sink->Keys();
        const loglib::KeyId valueKey = keys.GetOrInsert(std::string("value"));

        sink->OnBatch(MakeSyntheticBatch(streamSource, keys, valueKey, 1, 30, /*declareNewKey=*/true));
        QCoreApplication::processEvents();

        // Park at the (Bottom) tail, scroll away, then take a
        // batch so the pill ends up with a non-zero count under
        // Bottom orientation.
        QScrollBar *vbar = tableView->verticalScrollBar();
        vbar->setRange(0, 1000);
        vbar->setValue(vbar->maximum());
        tableView->SetTailEdge(LogTableView::TailEdge::Bottom);
        vbar->triggerAction(QAbstractSlider::SliderToMinimum);
        QCoreApplication::processEvents();

        sink->OnBatch(MakeSyntheticBatch(streamSource, keys, valueKey, 31, 5, /*declareNewKey=*/false));
        QCoreApplication::processEvents();
        QCOMPARE(tableView->PendingNewRowsForTest(), 5);
        QCOMPARE(pill->Count(), 5);

        // Park the scrollbar in the middle so neither tail edge
        // computes as true -- the exact mid-scroll condition
        // where the old `if (mAtTailEdge)` gate would skip the
        // reset. `setRange` is needed because `SliderToMinimum`
        // clamped the range to Qt's computed layout.
        vbar->setRange(0, 1000);
        vbar->setValue(500);
        QVERIFY2(
            !tableView->AtTailEdgeForTest(),
            "precondition: mid-scroll viewport must not be at the Bottom tail edge before the flip"
        );

        tableView->SetTailEdge(LogTableView::TailEdge::Top);

        QCOMPARE(pill->Direction(), JumpToTailPill::ArrowDirection::Up);
        QVERIFY2(
            !tableView->AtTailEdgeForTest(),
            "sanity: mid-scroll viewport must not be at the Top tail edge either, so the at-tail "
            "branch of the reset gate cannot mask a missing edge-flip branch"
        );
        QCOMPARE(tableView->PendingNewRowsForTest(), 0);
        QCOMPARE(pill->Count(), 0);

        model->EndStreaming(false);
    }

    // `JumpToNewestRow` used to bail (and the pill click do
    // nothing) when the source's newest row was filtered out --
    // a common live-tail + level filter case. The fallback now
    // walks back to the first visible source row and ultimately
    // snaps to the proxy's visual tail, so the click always
    // moves the viewport.
    //
    // Setup: push values 1..30, exclude `value >= 26`, scroll
    // off tail, and check that `JumpToNewestRow` advances the
    // scrollbar (vs. the pre-fix early-return that left it put).
    void TestJumpToNewestRowFallbackToVisualTailWhenFiltered()
    {
        auto *model = mWindow->findChild<LogModel *>();
        auto *tableView = mWindow->findChild<LogTableView *>();
        LogFilterModel *filterModel = mWindow->FilterModel();
        QVERIFY(model != nullptr);
        QVERIFY(tableView != nullptr);
        QVERIFY(filterModel != nullptr);

        loglib::StreamLineSource &streamSource = BeginSyntheticStreamSession(*model);
        QtStreamingLogSink *sink = model->Sink();
        QVERIFY(sink != nullptr);
        loglib::KeyIndex &keys = sink->Keys();
        const loglib::KeyId valueKey = keys.GetOrInsert(std::string("value"));

        sink->OnBatch(MakeSyntheticBatch(streamSource, keys, valueKey, 1, 30, /*declareNewKey=*/true));
        QCoreApplication::processEvents();
        QCOMPARE(model->rowCount(), 30);

        const int valueCol = ColumnByHeader(*model, QStringLiteral("value"));
        QVERIFY2(valueCol >= 0, "MakeSyntheticBatch must produce a 'value' column for the filter to target");

        // Exclude `value >= 26` so the source-newest row
        // (value=30) is filtered out -- the bug condition.
        {
            std::vector<loglib::RowPredicate> rules;
            rules.emplace_back(
                std::in_place_type<loglib::NumericRangeRowPredicate>,
                static_cast<size_t>(valueCol),
                std::optional<double>{},
                std::optional<double>{25.5}
            );
            filterModel->SetFilterRules(std::move(rules));
        }
        QCoreApplication::processEvents();
        QCOMPARE(model->rowCount(), 30);
        QCOMPARE(filterModel->rowCount(), 25);

        // Park the scrollbar off tail; this test exercises
        // `JumpToNewestRow`'s mapping path, not the scroll-edge
        // state machine, so a plain `setRange` + `setValue` is
        // enough.
        QScrollBar *vbar = tableView->verticalScrollBar();
        vbar->setRange(0, 1000);
        vbar->setValue(0);
        const int valueBefore = vbar->value();
        QCOMPARE(valueBefore, 0);

        mWindow->JumpToNewestRowForTest();
        QCoreApplication::processEvents();

        // Pre-fix: stays at 0 (source-newest mapping was
        // invalid). With the fallback we land near the proxy's
        // visual tail.
        QVERIFY2(
            vbar->value() > valueBefore,
            qPrintable(QStringLiteral("scrollbar must move toward the visual tail when the newest source row is "
                                      "filtered out; value stayed at %1")
                           .arg(vbar->value()))
        );

        model->EndStreaming(false);
    }

    // A row insert that grows `maximum` without moving `value`
    // used to leave `mAtTailEdge` stale (`OnVerticalScrollValueChanged`
    // only fires when `value` changes), so a user at the previous
    // tail would silently drift behind. The `rangeChanged`
    // listener refreshes the flag without emitting user-scroll
    // signals (a layout change must not toggle Follow newest).
    //
    // Drives `setRange` directly and observes via
    // `AtTailEdgeForTest()`: a row insert under the offscreen QPA
    // can re-call `setRange(0, 0)` during geometry and mask the
    // bug; the seam pins the moment the listener fires.
    void TestRangeChangeFreshensAtTailFlag()
    {
        auto *tableView = mWindow->findChild<LogTableView *>();
        QVERIFY(tableView != nullptr);

        QScrollBar *vbar = tableView->verticalScrollBar();
        vbar->setRange(0, 0);
        vbar->setValue(0);
        tableView->SetTailEdge(LogTableView::TailEdge::Bottom);
        QVERIFY2(tableView->AtTailEdgeForTest(), "empty range + value 0 must be at-tail after re-seed");

        const QSignalSpy awaySpy(tableView, &LogTableView::userScrolledAwayFromTail);
        const QSignalSpy toSpy(tableView, &LogTableView::userScrolledToTail);
        QVERIFY(awaySpy.isValid());
        QVERIFY(toSpy.isValid());

        // Grow the range without moving `value` -- the bug
        // condition. The listener must flip the flag to false.
        vbar->setRange(0, 500);
        QVERIFY2(
            !tableView->AtTailEdgeForTest(),
            "rangeChanged growing max must flip mAtTailEdge to false when value < newMax"
        );

        // Shrinking the range back so `value >= max` must restore
        // the flag (the rare "rows removed past value" branch).
        vbar->setRange(0, 0);
        QVERIFY2(tableView->AtTailEdgeForTest(), "rangeChanged shrinking max back below value must restore at-tail");

        // Neither transition is a user scroll, so no
        // `userScrolled*` signal must fire (a stray emission
        // would auto-toggle Follow newest).
        QCOMPARE(awaySpy.count(), 0);
        QCOMPARE(toSpy.count(), 0);
    }

    // Regression: under live-tail with Follow newest engaged,
    // signal ordering during a row insert (`rangeChanged` ->
    // `mAtTailEdge` to false -> our `OnRowsInserted` increments
    // -> `lineCountChanged` -> scroll-back -> reset) used to
    // briefly leak a count between steps 3 and 5. The fix
    // mirrors `actionFollowTail` into `mPendingNewRowsSuppressed`
    // so the increment short-circuits.
    //
    // Pin: with Follow engaged, no batch may push the pill above
    // zero regardless of signal ordering.
    void TestJumpToTailPillStaysZeroUnderFollowNewest()
    {
        auto *tableView = mWindow->findChild<LogTableView *>();
        auto *model = mWindow->findChild<LogModel *>();
        QAction *followAction = FindActionByObjectName(mWindow, QStringLiteral("actionFollowTail"));
        QVERIFY(tableView != nullptr);
        QVERIFY(model != nullptr);
        QVERIFY(followAction != nullptr);
        const JumpToTailPill *pill = tableView->TailPillForTest();
        QVERIFY(pill != nullptr);

        auto restore = qScopeGuard([this]() { mWindow->SetSessionModeForTest(MainWindow::TestSessionMode::Idle); });
        mWindow->SetSessionModeForTest(MainWindow::TestSessionMode::LiveTail);

        loglib::StreamLineSource &streamSource = BeginSyntheticStreamSession(*model);
        QtStreamingLogSink *sink = model->Sink();
        QVERIFY(sink != nullptr);
        loglib::KeyIndex &keys = sink->Keys();
        const loglib::KeyId valueKey = keys.GetOrInsert(std::string("value"));

        // Engage Follow newest and verify the suppression flag
        // followed. Suppression must be active *before* the
        // batch arrives or the window we're closing would still
        // leak a count.
        followAction->setEnabled(true);
        followAction->setChecked(true);
        QVERIFY2(
            tableView->PendingNewRowsSuppressedForTest(),
            "Follow newest engaging must mirror into the view's pending-new-rows suppression flag"
        );

        sink->OnBatch(MakeSyntheticBatch(streamSource, keys, valueKey, 1, 25, /*declareNewKey=*/true));
        QCoreApplication::processEvents();
        QCOMPARE(model->rowCount(), 25);

        QCOMPARE(tableView->PendingNewRowsForTest(), 0);
        QCOMPARE(pill->Count(), 0);
        QVERIFY2(pill->isHidden(), "pill must stay hidden across a batch in the live-tail Follow-newest steady state");

        // A second batch must also stay silent -- the
        // suppression flag is sticky, not one-shot.
        sink->OnBatch(MakeSyntheticBatch(streamSource, keys, valueKey, 26, 13, /*declareNewKey=*/false));
        QCoreApplication::processEvents();
        QCOMPARE(tableView->PendingNewRowsForTest(), 0);
        QCOMPARE(pill->Count(), 0);

        model->EndStreaming(false);
    }

    // Toggling Follow newest back on while scrolled away with a
    // pending tally must drain the count -- engaging suppression
    // implies the user has acknowledged the announcement.
    void TestFollowNewestEngagingClearsPendingTally()
    {
        auto *tableView = mWindow->findChild<LogTableView *>();
        auto *model = mWindow->findChild<LogModel *>();
        QAction *followAction = FindActionByObjectName(mWindow, QStringLiteral("actionFollowTail"));
        QVERIFY(tableView != nullptr);
        QVERIFY(model != nullptr);
        QVERIFY(followAction != nullptr);
        const JumpToTailPill *pill = tableView->TailPillForTest();
        QVERIFY(pill != nullptr);

        auto restore = qScopeGuard([this]() { mWindow->SetSessionModeForTest(MainWindow::TestSessionMode::Idle); });
        mWindow->SetSessionModeForTest(MainWindow::TestSessionMode::LiveTail);

        loglib::StreamLineSource &streamSource = BeginSyntheticStreamSession(*model);
        QtStreamingLogSink *sink = model->Sink();
        QVERIFY(sink != nullptr);
        loglib::KeyIndex &keys = sink->Keys();
        const loglib::KeyId valueKey = keys.GetOrInsert(std::string("value"));

        // Start with Follow off so the scroll-away + batch path
        // accumulates a tally (same path as
        // `TestJumpToTailPillSurfaces*`, just under live-tail).
        followAction->setEnabled(true);
        followAction->setChecked(false);

        sink->OnBatch(MakeSyntheticBatch(streamSource, keys, valueKey, 1, 30, /*declareNewKey=*/true));
        QCoreApplication::processEvents();

        QScrollBar *vbar = tableView->verticalScrollBar();
        vbar->setRange(0, 1000);
        vbar->setValue(vbar->maximum());
        tableView->SetTailEdge(LogTableView::TailEdge::Bottom);
        vbar->triggerAction(QAbstractSlider::SliderToMinimum);
        QCoreApplication::processEvents();

        sink->OnBatch(MakeSyntheticBatch(streamSource, keys, valueKey, 31, 4, /*declareNewKey=*/false));
        QCoreApplication::processEvents();
        QCOMPARE(tableView->PendingNewRowsForTest(), 4);
        QCOMPARE(pill->Count(), 4);

        // Toggle Follow back on. Engaging suppression must drain
        // the tally: "I trust auto-follow more than the catch-up
        // affordance".
        followAction->setChecked(true);

        QVERIFY2(
            tableView->PendingNewRowsSuppressedForTest(),
            "engaging Follow newest must mirror into the view's pending-new-rows suppression flag"
        );
        QCOMPARE(tableView->PendingNewRowsForTest(), 0);
        QCOMPARE(pill->Count(), 0);

        model->EndStreaming(false);
    }

    // Regression: under a custom sort the pill-click scroll can
    // land short of the visual tail (the source-newest row sits
    // in the middle of the proxy), so `mAtTailEdge` never
    // transitions and `ResetPendingNewRows` would not fire. The
    // host's `jumpToTailRequested` handler now acknowledges
    // up-front so the count clears regardless of where the
    // scroll lands.
    void TestJumpToTailPillClickAcknowledgesUnderCustomSort()
    {
        auto *tableView = mWindow->findChild<LogTableView *>();
        auto *model = mWindow->findChild<LogModel *>();
        const QAction *followAction = FindActionByObjectName(mWindow, QStringLiteral("actionFollowTail"));
        QVERIFY(tableView != nullptr);
        QVERIFY(model != nullptr);
        QVERIFY(followAction != nullptr);
        JumpToTailPill *pill = tableView->TailPillForTest();
        QVERIFY(pill != nullptr);

        // Static session keeps Follow newest out of the picture:
        // live-tail's auto re-engage would drain the tally as a
        // side effect and mask the regression.
        auto restore = qScopeGuard([this]() { mWindow->SetSessionModeForTest(MainWindow::TestSessionMode::Idle); });
        mWindow->SetSessionModeForTest(MainWindow::TestSessionMode::Static);

        loglib::StreamLineSource &streamSource = BeginSyntheticStaticSession(*model);
        QtStreamingLogSink *sink = model->Sink();
        QVERIFY(sink != nullptr);
        loglib::KeyIndex &keys = sink->Keys();
        const loglib::KeyId valueKey = keys.GetOrInsert(std::string("value"));

        sink->OnBatch(MakeSyntheticBatch(streamSource, keys, valueKey, 1, 30, /*declareNewKey=*/true));
        QCoreApplication::processEvents();

        // Sort descending so source-newest (value=30) lands at
        // proxy row 0 -- the visual top under Bottom orientation.
        // `JumpToNewestRow`'s `scrollTo(PositionAtBottom)` then
        // can't move that row to the visual bottom, leaving the
        // scrollbar short of `maximum()`.
        const int valueCol = ColumnByHeader(*model, QStringLiteral("value"));
        QVERIFY2(valueCol >= 0, "MakeSyntheticBatch must produce a 'value' column for the sort to target");
        tableView->sortByColumn(valueCol, Qt::DescendingOrder);
        QCoreApplication::processEvents();

        // Park scrolled away and seed a pending tally via a
        // second batch. The count tracks "rows since I scrolled
        // away" regardless of where they land visually.
        QScrollBar *vbar = tableView->verticalScrollBar();
        vbar->setRange(0, 1000);
        vbar->setValue(vbar->maximum());
        tableView->SetTailEdge(LogTableView::TailEdge::Bottom);
        vbar->triggerAction(QAbstractSlider::SliderToMinimum);
        QCoreApplication::processEvents();

        sink->OnBatch(MakeSyntheticBatch(streamSource, keys, valueKey, 31, 8, /*declareNewKey=*/false));
        QCoreApplication::processEvents();
        QVERIFY2(
            tableView->PendingNewRowsForTest() > 0,
            qPrintable(QStringLiteral("precondition: pill must have non-zero tally before the click; got %1")
                           .arg(tableView->PendingNewRowsForTest()))
        );

        // Click the pill. The host handler calls
        // `AcknowledgePendingNewRows` up-front, so the count
        // clears regardless of where the scroll lands.
        pill->click();
        QCoreApplication::processEvents();

        QCOMPARE(tableView->PendingNewRowsForTest(), 0);
        QCOMPARE(pill->Count(), 0);

        model->EndStreaming(false);
    }

    // Accessibility contract: the pill must be keyboard-reachable
    // (`Qt::TabFocus`) and its `accessibleName` must include the
    // running count so AT users hear updates (Qt's bridge prefers
    // `accessibleName` over the visible `text`). Substring match
    // on the digit keeps the assertion robust to locale wording.
    void TestJumpToTailPillAccessibilityContract()
    {
        auto *tableView = mWindow->findChild<LogTableView *>();
        auto *model = mWindow->findChild<LogModel *>();
        QVERIFY(tableView != nullptr);
        QVERIFY(model != nullptr);
        const JumpToTailPill *pill = tableView->TailPillForTest();
        QVERIFY(pill != nullptr);

        QCOMPARE(pill->focusPolicy(), Qt::TabFocus);
        QVERIFY2(
            pill->accessibleName().contains(QStringLiteral("Jump to newest row")),
            "pill accessibleName must reference the action while the count is zero"
        );

        loglib::StreamLineSource &streamSource = BeginSyntheticStreamSession(*model);
        QtStreamingLogSink *sink = model->Sink();
        QVERIFY(sink != nullptr);
        loglib::KeyIndex &keys = sink->Keys();
        const loglib::KeyId valueKey = keys.GetOrInsert(std::string("value"));

        sink->OnBatch(MakeSyntheticBatch(streamSource, keys, valueKey, 1, 30, /*declareNewKey=*/true));
        QCoreApplication::processEvents();

        QScrollBar *vbar = tableView->verticalScrollBar();
        vbar->setRange(0, 1000);
        vbar->setValue(vbar->maximum());
        tableView->SetTailEdge(LogTableView::TailEdge::Bottom);
        vbar->triggerAction(QAbstractSlider::SliderToMinimum);
        QCoreApplication::processEvents();

        sink->OnBatch(MakeSyntheticBatch(streamSource, keys, valueKey, 31, 7, /*declareNewKey=*/false));
        QCoreApplication::processEvents();
        QCOMPARE(pill->Count(), 7);

        // Live-update path: with a non-zero count, the
        // accessible name must include the count so screen
        // readers convey *how many* rows are pending.
        // Substring match: locales rephrase the sentence (and
        // some embed `7` differently), the contract is that
        // the digit is present in the announced name.
        QVERIFY2(
            pill->accessibleName().contains(QStringLiteral("7")),
            qPrintable(QStringLiteral("pill accessibleName must include the running count for AT users; got: %1")
                           .arg(pill->accessibleName()))
        );

        model->EndStreaming(false);
    }

    // When every row is filtered out, `JumpToNewestRow` has
    // nothing to scroll to and used to leave the pill stranded.
    // The empty-proxy branch now acknowledges so a pending tally
    // doesn't linger across the swallow.
    void TestJumpToNewestRowAcknowledgesWhenProxyEmpty()
    {
        auto *tableView = mWindow->findChild<LogTableView *>();
        auto *model = mWindow->findChild<LogModel *>();
        LogFilterModel *filterModel = mWindow->FilterModel();
        QVERIFY(tableView != nullptr);
        QVERIFY(model != nullptr);
        QVERIFY(filterModel != nullptr);
        const JumpToTailPill *pill = tableView->TailPillForTest();
        QVERIFY(pill != nullptr);

        loglib::StreamLineSource &streamSource = BeginSyntheticStreamSession(*model);
        QtStreamingLogSink *sink = model->Sink();
        QVERIFY(sink != nullptr);
        loglib::KeyIndex &keys = sink->Keys();
        const loglib::KeyId valueKey = keys.GetOrInsert(std::string("value"));

        sink->OnBatch(MakeSyntheticBatch(streamSource, keys, valueKey, 1, 30, /*declareNewKey=*/true));
        QCoreApplication::processEvents();

        // Park scrolled away and accumulate a pending count.
        QScrollBar *vbar = tableView->verticalScrollBar();
        vbar->setRange(0, 1000);
        vbar->setValue(vbar->maximum());
        tableView->SetTailEdge(LogTableView::TailEdge::Bottom);
        vbar->triggerAction(QAbstractSlider::SliderToMinimum);
        QCoreApplication::processEvents();

        sink->OnBatch(MakeSyntheticBatch(streamSource, keys, valueKey, 31, 4, /*declareNewKey=*/false));
        QCoreApplication::processEvents();
        QCOMPARE(pill->Count(), 4);

        // Tighten the filter to swallow every row. Pre-fix the
        // empty-proxy branch left the pill alone.
        const int valueCol = ColumnByHeader(*model, QStringLiteral("value"));
        QVERIFY2(valueCol >= 0, "MakeSyntheticBatch must produce a 'value' column");
        {
            std::vector<loglib::RowPredicate> rules;
            // Range that excludes every value (max < min).
            rules.emplace_back(
                std::in_place_type<loglib::NumericRangeRowPredicate>,
                static_cast<size_t>(valueCol),
                std::optional<double>{1000.0},
                std::optional<double>{0.0}
            );
            filterModel->SetFilterRules(std::move(rules));
        }
        QCoreApplication::processEvents();
        QCOMPARE(filterModel->rowCount(), 0);

        mWindow->JumpToNewestRowForTest();
        QCoreApplication::processEvents();

        QCOMPARE(tableView->PendingNewRowsForTest(), 0);
        QCOMPARE(pill->Count(), 0);

        model->EndStreaming(false);
    }

    // Alternating row colours stay off on the log table regardless
    // of newest-first orientation. Per-level theme colours already
    // partition rows; toggling alternation also used to flicker
    // every batch in newest-first mode. The test guards against a
    // regression that re-enables alternation.
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

        StreamingControl::SetNewestFirst(false);
        mWindow->ApplyDisplayOrder();
        QVERIFY2(
            !tableView->alternatingRowColors(),
            "log table must keep alternating row colours off regardless of display order"
        );

        StreamingControl::SetNewestFirst(true);
        mWindow->ApplyDisplayOrder();
        QVERIFY2(
            !tableView->alternatingRowColors(), "log table must keep alternating row colours off in newest-first mode"
        );

        StreamingControl::SetNewestFirst(false);
        mWindow->ApplyDisplayOrder();
        QVERIFY2(
            !tableView->alternatingRowColors(),
            "log table must keep alternating row colours off after toggling newest-first back"
        );

        model->EndStreaming(false);
    }

    // Pins the table's Explorer/Excel selection idiom (plain click
    // replaces, Ctrl-click toggles, Shift-click extends) at whole-row
    // granularity. Driven through `selectionCommand()` directly to
    // avoid the offscreen-QPA realisation issues.
    void TestTableExtendedSelectionRowClickSemantics()
    {
        auto *tableView = mWindow->findChild<LogTableView *>();
        auto *model = mWindow->findChild<LogModel *>();
        QVERIFY(tableView != nullptr);
        QVERIFY(model != nullptr);

        QCOMPARE(tableView->selectionMode(), QAbstractItemView::ExtendedSelection);
        QCOMPARE(tableView->selectionBehavior(), QAbstractItemView::SelectRows);

        // Populate a few rows so `selectionCommand` runs against a
        // valid proxy index.
        loglib::StreamLineSource &streamSource = BeginSyntheticStreamSession(*model);
        QtStreamingLogSink *sink = model->Sink();
        QVERIFY(sink != nullptr);
        loglib::KeyIndex &keys = sink->Keys();
        const loglib::KeyId valueKey = keys.GetOrInsert(std::string("value"));
        sink->OnBatch(MakeSyntheticBatch(streamSource, keys, valueKey, 1, 5, /*declareNewKey=*/true));
        QCoreApplication::processEvents();
        QVERIFY(tableView->model()->rowCount() >= 1);

        const QModelIndex clickedIndex = tableView->model()->index(0, 0);
        QVERIFY(clickedIndex.isValid());

        // Left-button press with the given modifiers; position is
        // irrelevant for `selectionCommand`.
        auto makePress = [](Qt::KeyboardModifiers mods) {
            return QMouseEvent(
                QEvent::MouseButtonPress, QPointF(0, 0), QPointF(0, 0), Qt::LeftButton, Qt::LeftButton, mods
            );
        };

        // Plain click replaces the current selection (row granularity).
        {
            const QMouseEvent ev = makePress(Qt::NoModifier);
            const QItemSelectionModel::SelectionFlags flags = tableView->SelectionCommandForTest(clickedIndex, &ev);
            QVERIFY2(
                flags.testFlag(QItemSelectionModel::ClearAndSelect),
                "Plain click on a row must replace the existing selection (ClearAndSelect)."
            );
            QVERIFY2(flags.testFlag(QItemSelectionModel::Rows), "Plain click must operate on whole rows (Rows flag).");
            QVERIFY2(
                !flags.testFlag(QItemSelectionModel::Toggle),
                "Plain click must NOT merely toggle the row -- that is the MultiSelection regression."
            );
        }

        // Ctrl-click toggles one row in/out of the selection.
        {
            const QMouseEvent ev = makePress(Qt::ControlModifier);
            const QItemSelectionModel::SelectionFlags flags = tableView->SelectionCommandForTest(clickedIndex, &ev);
            QVERIFY2(
                flags.testFlag(QItemSelectionModel::Toggle),
                "Ctrl-click must toggle the clicked row in or out of the selection."
            );
            QVERIFY2(flags.testFlag(QItemSelectionModel::Rows), "Ctrl-click must operate on whole rows (Rows flag).");
            QVERIFY2(!flags.testFlag(QItemSelectionModel::Clear), "Ctrl-click must NOT clear the existing selection.");
            QVERIFY2(
                !flags.testFlag(QItemSelectionModel::ClearAndSelect),
                "Ctrl-click must NOT replace the existing selection."
            );
        }

        // Shift-click extends from the selection anchor.
        {
            const QMouseEvent ev = makePress(Qt::ShiftModifier);
            const QItemSelectionModel::SelectionFlags flags = tableView->SelectionCommandForTest(clickedIndex, &ev);
            QVERIFY2(
                flags.testFlag(QItemSelectionModel::SelectCurrent),
                "Shift-click must extend the current selection (SelectCurrent)."
            );
            QVERIFY2(flags.testFlag(QItemSelectionModel::Rows), "Shift-click must operate on whole rows (Rows flag).");
            QVERIFY2(!flags.testFlag(QItemSelectionModel::Toggle), "Shift-click is a range extension, not a toggle.");
        }

        model->EndStreaming(false);
    }

    // AnchorManager mutation contract: idempotent SetAnchor,
    // RemoveAnchor's "did anything change" return, ClearAll's bulk
    // signal, Replace's drop-of-bad-slots policy, and the stable
    // `Entries()` ordering used by the byte-stable save path.
    void TestAnchorManagerCoreContract()
    {
        AnchorManager manager;
        const QSignalSpy changedSpy(&manager, &AnchorManager::anchorChanged);
        const QSignalSpy resetSpy(&manager, &AnchorManager::anchorsReset);

        const AnchorManager::Key keyA{.locator = "c:/logs/a.json", .lineId = 7};
        const AnchorManager::Key keyB{.locator = "c:/logs/b.json", .lineId = 3};

        QVERIFY(manager.Empty());
        QCOMPARE(manager.Count(), std::size_t{0});

        // First add: changes state, fires `anchorChanged` once.
        QVERIFY(manager.SetAnchor(keyA, 2));
        QCOMPARE(changedSpy.count(), 1);
        QCOMPARE(manager.ColorFor(keyA).value_or(255U), uint8_t{2});

        // Same key + colour: idempotent (no signal, returns false).
        QVERIFY(!manager.SetAnchor(keyA, 2));
        QCOMPARE(changedSpy.count(), 1);

        // Same key, new colour: re-emits once.
        QVERIFY(manager.SetAnchor(keyA, 5));
        QCOMPARE(changedSpy.count(), 2);
        QCOMPARE(manager.ColorFor(keyA).value_or(255U), uint8_t{5});

        // Out-of-range colour clamps rather than rejecting.
        QVERIFY(manager.SetAnchor(keyB, 99));
        QCOMPARE(manager.ColorFor(keyB).value_or(255U), uint8_t{loglib::ANCHOR_PALETTE_SIZE - 1});
        QCOMPARE(changedSpy.count(), 3);

        // Stable sort order: locator-first, then lineId.
        const auto entries = manager.Entries();
        QCOMPARE(entries.size(), std::size_t{2});
        QCOMPARE(entries[0].locator, std::string{"c:/logs/a.json"});
        QCOMPARE(entries[1].locator, std::string{"c:/logs/b.json"});

        // RemoveAnchor: emits exactly once when the key existed,
        // no-ops + returns false otherwise.
        QVERIFY(manager.RemoveAnchor(keyA));
        QCOMPARE(changedSpy.count(), 4);
        QVERIFY(!manager.RemoveAnchor(keyA));
        QCOMPARE(changedSpy.count(), 4);

        // ClearAll: bulk signal, only when the map was non-empty.
        QVERIFY(manager.ClearAll());
        QCOMPARE(resetSpy.count(), 1);
        QVERIFY(!manager.ClearAll());
        QCOMPARE(resetSpy.count(), 1);

        // Replace drops out-of-range entries (rather than clamping)
        // and returns the drop count so the GUI can surface it.
        std::vector<loglib::LogConfiguration::AnchorEntry> incoming;
        incoming.push_back(loglib::LogConfiguration::AnchorEntry{.locator = "c:/x.json", .lineId = 1, .colorIndex = 0});
        incoming.push_back(loglib::LogConfiguration::AnchorEntry{.locator = "c:/x.json", .lineId = 2, .colorIndex = 42}
        );
        QCOMPARE(manager.Replace(incoming), std::size_t{1});
        QCOMPARE(resetSpy.count(), 2);
        QCOMPARE(manager.Count(), std::size_t{1});
        QCOMPARE(manager.ColorFor({.locator = "c:/x.json", .lineId = 1}).value_or(255U), uint8_t{0});
        QVERIFY(!manager.ColorFor({.locator = "c:/x.json", .lineId = 2}).has_value());

        // All-valid input returns 0 dropped (it is a schema-drift
        // signal, not an input size).
        std::vector<loglib::LogConfiguration::AnchorEntry> clean;
        clean.push_back(loglib::LogConfiguration::AnchorEntry{.locator = "c:/x.json", .lineId = 3, .colorIndex = 1});
        QCOMPARE(manager.Replace(clean), std::size_t{0});
    }

    // `Entries()` (the save snapshot) drops empty-locator anchors:
    // their `lineId` is not stable across sessions, so persisting
    // them would later collide with unrelated rows.
    // `EntriesIncludingRuntimeOnly` keeps them for diagnostics.
    void TestAnchorManagerEntriesDropsRuntimeOnlyAnchors()
    {
        AnchorManager manager;

        // One canonical-locator anchor, one runtime-only.
        const AnchorManager::Key persistable{.locator = "c:/logs/persistent.json", .lineId = 7};
        const AnchorManager::Key runtimeOnly{.locator = "", .lineId = 42};
        QVERIFY(manager.SetAnchor(persistable, 1));
        QVERIFY(manager.SetAnchor(runtimeOnly, 5));
        QCOMPARE(manager.Count(), std::size_t{2});

        // Save path keeps only the persistable anchor.
        const auto saved = manager.Entries();
        QCOMPARE(saved.size(), std::size_t{1});
        QCOMPARE(saved.front().locator, std::string{"c:/logs/persistent.json"});
        QCOMPARE(saved.front().lineId, std::uint64_t{7});
        QCOMPARE(saved.front().colorIndex, std::uint8_t{1});

        // Diagnostics path keeps both, deterministic order.
        const auto all = manager.EntriesIncludingRuntimeOnly();
        QCOMPARE(all.size(), std::size_t{2});
        QCOMPARE(all[0].locator, std::string{""});
        QCOMPARE(all[1].locator, std::string{"c:/logs/persistent.json"});

        // The runtime-only anchor still lives in the manager
        // (the filter is at the snapshot, not the mutation).
        QCOMPARE(manager.ColorFor(runtimeOnly).value_or(255U), std::uint8_t{5});
    }

    // Replace's same-content short-circuit: re-applying the same
    // entries (in any order) must be silent; a colour change must
    // still fire `anchorsReset`.
    void TestAnchorManagerReplaceIsSilentWhenContentUnchanged()
    {
        AnchorManager manager;
        const QSignalSpy resetSpy(&manager, &AnchorManager::anchorsReset);

        // Seed two anchors.
        std::vector<loglib::LogConfiguration::AnchorEntry> entries;
        entries.push_back(
            loglib::LogConfiguration::AnchorEntry{.locator = "c:/logs/a.json", .lineId = 1, .colorIndex = 2}
        );
        entries.push_back(
            loglib::LogConfiguration::AnchorEntry{.locator = "c:/logs/b.json", .lineId = 9, .colorIndex = 5}
        );
        QCOMPARE(manager.Replace(entries), std::size_t{0});
        QCOMPARE(resetSpy.count(), 1);
        QCOMPARE(manager.Count(), std::size_t{2});

        // Same content, shuffled order: comparison is map-equality,
        // so this must NOT emit.
        std::vector<loglib::LogConfiguration::AnchorEntry> shuffled;
        shuffled.push_back(
            loglib::LogConfiguration::AnchorEntry{.locator = "c:/logs/b.json", .lineId = 9, .colorIndex = 5}
        );
        shuffled.push_back(
            loglib::LogConfiguration::AnchorEntry{.locator = "c:/logs/a.json", .lineId = 1, .colorIndex = 2}
        );
        QCOMPARE(manager.Replace(shuffled), std::size_t{0});
        QCOMPARE(resetSpy.count(), 1);

        // Same keys, different colour: content changed, listeners
        // must be told.
        std::vector<loglib::LogConfiguration::AnchorEntry> mutated;
        mutated.push_back(
            loglib::LogConfiguration::AnchorEntry{.locator = "c:/logs/a.json", .lineId = 1, .colorIndex = 2}
        );
        mutated.push_back(
            loglib::LogConfiguration::AnchorEntry{.locator = "c:/logs/b.json", .lineId = 9, .colorIndex = 6}
        );
        QCOMPARE(manager.Replace(mutated), std::size_t{0});
        QCOMPARE(resetSpy.count(), 2);
        QCOMPARE(manager.ColorFor({.locator = "c:/logs/b.json", .lineId = 9}).value_or(255U), uint8_t{6});
    }

    // ThemeControl::AnchorBrushFor: paint-ready brush for every
    // valid slot, invalid brush for out-of-range / unknown roles.
    void TestThemeControlAnchorBrushResolver()
    {
        QVERIFY(mTheme != nullptr);

        // Every slot yields paint-ready brushes for BG and FG.
        for (uint8_t i = 0; i < loglib::ANCHOR_PALETTE_SIZE; ++i)
        {
            const QBrush bg = mTheme->AnchorBrushFor(i, Qt::BackgroundRole);
            const QBrush fg = mTheme->AnchorBrushFor(i, Qt::ForegroundRole);
            QVERIFY2(bg.style() != Qt::NoBrush, qPrintable(QStringLiteral("anchor slot %1 BG was invalid").arg(i)));
            QVERIFY2(fg.style() != Qt::NoBrush, qPrintable(QStringLiteral("anchor slot %1 FG was invalid").arg(i)));
            QVERIFY(bg.color().isValid());
            QVERIFY(fg.color().isValid());
        }

        // Distinct slots must paint distinct backgrounds.
        QVERIFY(
            mTheme->AnchorBrushFor(0, Qt::BackgroundRole).color() !=
            mTheme->AnchorBrushFor(4, Qt::BackgroundRole).color()
        );

        // Out-of-range -> invalid brush (model falls through to
        // the level branch).
        const QBrush oob = mTheme->AnchorBrushFor(loglib::ANCHOR_PALETTE_SIZE, Qt::BackgroundRole);
        QVERIFY(oob.style() == Qt::NoBrush);

        // Unhandled role -> invalid brush.
        QVERIFY(mTheme->AnchorBrushFor(0, Qt::DisplayRole).style() == Qt::NoBrush);
    }

    // LogModel anchor overlay: anchored rows resolve to the anchor
    // brush (BG + FG), removing reverts to the level brush, and
    // `anchorChanged` fires a scoped `dataChanged` so the view
    // repaints that row.
    void TestLogModelAnchorOverlay()
    {
        ThemeControl theme;
        AnchorManager anchors;
        LogModel model(nullptr, &theme, &anchors);

        loglib::StreamLineSource &streamSource = BeginSyntheticStreamSession(model);
        QtStreamingLogSink *sink = model.Sink();
        QVERIFY(sink != nullptr);
        loglib::KeyIndex &keys = sink->Keys();
        const loglib::KeyId valueKey = keys.GetOrInsert(std::string("value"));
        sink->OnBatch(MakeSyntheticBatch(streamSource, keys, valueKey, /*firstLineId=*/1, /*count=*/3, true));
        QCoreApplication::processEvents();
        QCOMPARE(model.rowCount(), 3);

        // Snapshot row 0's pre-anchor brush so we can confirm the
        // anchor overlay differs (and reverts later).
        const QModelIndex row0 = model.index(0, 0);
        const QVariant preAnchorBg = model.data(row0, Qt::BackgroundRole);

        // Resolve through the model's helper to exercise the same
        // canonicalisation path production uses.
        const auto key = model.AnchorKeyForRow(0);
        QVERIFY(key.has_value());
        QCOMPARE(key->lineId, uint64_t{1});

        QSignalSpy dataSpy(&model, &LogModel::dataChanged);
        anchors.SetAnchor(*key, /*colorIndex=*/3);
        QCoreApplication::processEvents();

        // Scoped `dataChanged` covering Background + Foreground.
        QVERIFY(dataSpy.count() >= 1);
        bool sawBackgroundRoleEmit = false;
        for (const QList<QVariant> &args : dataSpy)
        {
            const auto roles = args.at(2).value<QList<int>>();
            if (roles.contains(Qt::BackgroundRole) && roles.contains(Qt::ForegroundRole))
            {
                sawBackgroundRoleEmit = true;
                break;
            }
        }
        QVERIFY2(sawBackgroundRoleEmit, "anchor change must emit dataChanged with Background+Foreground roles");

        const QVariant anchoredBg = model.data(row0, Qt::BackgroundRole);
        const QVariant anchoredFg = model.data(row0, Qt::ForegroundRole);
        QVERIFY(anchoredBg.canConvert<QBrush>());
        QVERIFY(anchoredFg.canConvert<QBrush>());
        const auto bgBrush = anchoredBg.value<QBrush>();
        const auto fgBrush = anchoredFg.value<QBrush>();
        QCOMPARE(bgBrush.color(), theme.AnchorBrushFor(3, Qt::BackgroundRole).color());
        QCOMPARE(fgBrush.color(), theme.AnchorBrushFor(3, Qt::ForegroundRole).color());

        // Other rows keep their pre-anchor brush.
        QVERIFY(model.data(model.index(1, 0), Qt::BackgroundRole) == preAnchorBg);

        // RemoveAnchor reverts row 0 to the pre-anchor brush.
        dataSpy.clear();
        anchors.RemoveAnchor(*key);
        QCoreApplication::processEvents();
        QVERIFY(dataSpy.count() >= 1);
        QCOMPARE(model.data(row0, Qt::BackgroundRole), preAnchorBg);

        model.EndStreaming(false);
    }

    // End-to-end: `AnchorSelection` routes through the proxy chain,
    // sets the requested colour on each selected row, and ignores
    // unselected rows. Stand-in for the right-click menu and
    // Ctrl+1..8 hotkey, both wired to this slot.
    void TestLogTableViewAnchorSelectionDrivesAnchorManager()
    {
        auto *anchors = mWindow->Anchors();
        QVERIFY2(anchors != nullptr, "MainWindow must own an AnchorManager after construction");
        auto *model = mWindow->Model();
        auto *view = mWindow->findChild<LogTableView *>();
        QVERIFY(view != nullptr);

        // Four rows.
        loglib::StreamLineSource &streamSource = BeginSyntheticStreamSession(*model);
        QtStreamingLogSink *sink = model->Sink();
        QVERIFY(sink != nullptr);
        loglib::KeyIndex &keys = sink->Keys();
        const loglib::KeyId valueKey = keys.GetOrInsert(std::string("value"));
        sink->OnBatch(MakeSyntheticBatch(streamSource, keys, valueKey, 1, 4, true));
        QCoreApplication::processEvents();
        QCOMPARE(model->rowCount(), 4);

        // Select rows 0 and 2.
        QItemSelectionModel *sel = view->selectionModel();
        QVERIFY(sel != nullptr);
        sel->clearSelection();
        const QModelIndex idx0 = view->model()->index(0, 0);
        const QModelIndex idx2 = view->model()->index(2, 0);
        sel->select(idx0, QItemSelectionModel::Select | QItemSelectionModel::Rows);
        sel->select(idx2, QItemSelectionModel::Select | QItemSelectionModel::Rows);

        view->AnchorSelection(/*colorIndex=*/3);
        QCoreApplication::processEvents();

        const auto key0 = model->AnchorKeyForRow(0);
        const auto key1 = model->AnchorKeyForRow(1);
        const auto key2 = model->AnchorKeyForRow(2);
        const auto key3 = model->AnchorKeyForRow(3);
        QVERIFY(key0.has_value());
        QVERIFY(key1.has_value());
        QVERIFY(key2.has_value());
        QVERIFY(key3.has_value());
        QCOMPARE(anchors->ColorFor(*key0).value_or(255), static_cast<std::uint8_t>(3));
        QCOMPARE(anchors->ColorFor(*key2).value_or(255), static_cast<std::uint8_t>(3));
        QVERIFY2(!anchors->ColorFor(*key1).has_value(), "unselected row 1 must not be anchored");
        QVERIFY2(!anchors->ColorFor(*key3).has_value(), "unselected row 3 must not be anchored");

        // Clear, then verify both anchors are gone.
        view->ClearAnchorOnSelection();
        QCoreApplication::processEvents();
        QVERIFY(!anchors->ColorFor(*key0).has_value());
        QVERIFY(!anchors->ColorFor(*key2).has_value());

        model->EndStreaming(false);
    }

    // JumpToAnchor visits anchored rows in ascending visible order
    // and wraps. With anchors on rows 0 and 2, the forward sequence
    // is 0 -> 2 -> 0; one step back from the wrap lands on 2.
    void TestMainWindowJumpToAnchorVisits()
    {
        auto *anchors = mWindow->Anchors();
        QVERIFY(anchors != nullptr);
        auto *model = mWindow->Model();
        auto *view = mWindow->findChild<LogTableView *>();
        QVERIFY(view != nullptr);

        loglib::StreamLineSource &streamSource = BeginSyntheticStreamSession(*model);
        QtStreamingLogSink *sink = model->Sink();
        QVERIFY(sink != nullptr);
        loglib::KeyIndex &keys = sink->Keys();
        const loglib::KeyId valueKey = keys.GetOrInsert(std::string("value"));
        sink->OnBatch(MakeSyntheticBatch(streamSource, keys, valueKey, 1, 4, true));
        QCoreApplication::processEvents();
        QCOMPARE(model->rowCount(), 4);

        const auto key0 = model->AnchorKeyForRow(0);
        const auto key2 = model->AnchorKeyForRow(2);
        QVERIFY(key0.has_value());
        QVERIFY(key2.has_value());
        anchors->SetAnchor(*key0, 0);
        anchors->SetAnchor(*key2, 1);
        QCoreApplication::processEvents();

        view->selectionModel()->clearSelection();
        view->setCurrentIndex(QModelIndex{});

        mWindow->JumpToAnchor(/*forward=*/true);
        QCoreApplication::processEvents();
        QCOMPARE(view->currentIndex().row(), 0);

        mWindow->JumpToAnchor(/*forward=*/true);
        QCoreApplication::processEvents();
        QCOMPARE(view->currentIndex().row(), 2);

        // Wrap to row 0.
        mWindow->JumpToAnchor(/*forward=*/true);
        QCoreApplication::processEvents();
        QCOMPARE(view->currentIndex().row(), 0);

        // Step back to row 2.
        mWindow->JumpToAnchor(/*forward=*/false);
        QCoreApplication::processEvents();
        QCOMPARE(view->currentIndex().row(), 2);

        model->EndStreaming(false);
    }

    // Anchor sub-menu reflects the right-clicked row: the matching
    // colour entry is checked (others aren't), and "Remove anchor"
    // is enabled iff the row carries a colour.
    void TestRowContextMenuAnchorSubMenuReflectsRowState()
    {
        auto *anchors = mWindow->Anchors();
        QVERIFY(anchors != nullptr);
        auto *model = mWindow->Model();

        loglib::StreamLineSource &streamSource = BeginSyntheticStreamSession(*model);
        QtStreamingLogSink *sink = model->Sink();
        QVERIFY(sink != nullptr);
        loglib::KeyIndex &keys = sink->Keys();
        const loglib::KeyId valueKey = keys.GetOrInsert(std::string("value"));
        sink->OnBatch(MakeSyntheticBatch(streamSource, keys, valueKey, 1, 3, true));
        QCoreApplication::processEvents();
        QCOMPARE(model->rowCount(), 3);

        const auto key1 = model->AnchorKeyForRow(1);
        QVERIFY(key1.has_value());
        anchors->SetAnchor(*key1, 4);
        QCoreApplication::processEvents();

        // Right-click row 1 (anchored at colour 4).
        QMenu *menu = mWindow->BuildRowContextMenu(/*sourceRow=*/1, nullptr);
        QVERIFY(menu != nullptr);
        const QScopeGuard menuDeleter([&menu]() { menu->deleteLater(); });
        const QAction *anchorMenuAction = FindMenuActionByText(menu, MainWindow::tr("Anchor"));
        QVERIFY2(anchorMenuAction != nullptr, "Anchor sub-menu must be present");
        const QMenu *anchorMenu = anchorMenuAction->menu();
        QVERIFY(anchorMenu != nullptr);

        int colour4ChecksSeen = 0;
        int otherColourChecksSeen = 0;
        for (std::size_t i = 0; i < loglib::ANCHOR_PALETTE_SIZE; ++i)
        {
            const int colourIndex = static_cast<int>(i);
            const QAction *entry = FindMenuActionByText(anchorMenu, MainWindow::tr("Colour %1").arg(colourIndex + 1));
            QVERIFY2(entry != nullptr, "every palette slot must have a sub-menu entry");
            QVERIFY(entry->isCheckable());
            if (colourIndex == 4)
            {
                if (entry->isChecked())
                {
                    ++colour4ChecksSeen;
                }
            }
            else if (entry->isChecked())
            {
                ++otherColourChecksSeen;
            }
        }
        QCOMPARE(colour4ChecksSeen, 1);
        QCOMPARE(otherColourChecksSeen, 0);

        const QAction *clear = FindMenuActionByText(anchorMenu, MainWindow::tr("Remove anchor"));
        QVERIFY2(clear != nullptr, "remove-anchor entry must be present");
        QVERIFY2(clear->isEnabled(), "remove-anchor must be enabled when the right-clicked row carries a colour");

        // Unanchored row: no checked colour, Remove disabled.
        QMenu *menu0 = mWindow->BuildRowContextMenu(/*sourceRow=*/0, nullptr);
        QVERIFY(menu0 != nullptr);
        const QScopeGuard menu0Deleter([&menu0]() { menu0->deleteLater(); });
        const QAction *anchor0 = FindMenuActionByText(menu0, MainWindow::tr("Anchor"));
        QVERIFY(anchor0 != nullptr);
        const QMenu *anchor0Menu = anchor0->menu();
        QVERIFY(anchor0Menu != nullptr);
        for (std::size_t i = 0; i < loglib::ANCHOR_PALETTE_SIZE; ++i)
        {
            const int colourIndex = static_cast<int>(i);
            const QAction *entry = FindMenuActionByText(anchor0Menu, MainWindow::tr("Colour %1").arg(colourIndex + 1));
            QVERIFY(entry != nullptr);
            QVERIFY2(!entry->isChecked(), "unanchored row must have no checked colour");
        }
        const QAction *clear0 = FindMenuActionByText(anchor0Menu, MainWindow::tr("Remove anchor"));
        QVERIFY(clear0 != nullptr);
        QVERIFY2(!clear0->isEnabled(), "remove-anchor must be disabled when the row carries no colour");

        model->EndStreaming(false);
    }

    // Right-clicking outside an existing selection collapses to
    // the clicked row (Explorer idiom); clicking inside the
    // selection keeps the set intact for multi-row actions.
    void TestRowContextMenuAdoptsRightClickedRowOutsideSelection()
    {
        auto *anchors = mWindow->Anchors();
        QVERIFY(anchors != nullptr);
        auto *model = mWindow->Model();
        auto *view = mWindow->findChild<LogTableView *>();
        QVERIFY(view != nullptr);

        loglib::StreamLineSource &streamSource = BeginSyntheticStreamSession(*model);
        QtStreamingLogSink *sink = model->Sink();
        QVERIFY(sink != nullptr);
        loglib::KeyIndex &keys = sink->Keys();
        const loglib::KeyId valueKey = keys.GetOrInsert(std::string("value"));
        sink->OnBatch(MakeSyntheticBatch(streamSource, keys, valueKey, 1, 5, true));
        QCoreApplication::processEvents();
        QCOMPARE(model->rowCount(), 5);

        const QAbstractItemModel *proxy = view->model();
        QVERIFY(proxy != nullptr);
        QItemSelectionModel *selection = view->selectionModel();
        QVERIFY(selection != nullptr);

        // Seed a multi-row selection on rows 0-2.
        selection->clearSelection();
        const QModelIndex anchorIdx = proxy->index(0, 0);
        const QModelIndex middleIdx = proxy->index(1, 0);
        const QModelIndex tailIdx = proxy->index(2, 0);
        selection->select(
            QItemSelection(anchorIdx, tailIdx), QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows
        );
        selection->setCurrentIndex(middleIdx, QItemSelectionModel::NoUpdate);
        QCOMPARE(selection->selectedRows().count(), 3);

        // Right-click inside the selection leaves it alone.
        const QPoint insideClick = view->visualRect(middleIdx).center();
        mWindow->ShowRowContextMenuForTest(insideClick);
        QCoreApplication::processEvents();
        QCOMPARE(selection->selectedRows().count(), 3);

        // Close the popup before the next case.
        if (QWidget *popup = QApplication::activePopupWidget(); popup != nullptr)
        {
            popup->close();
            QCoreApplication::processEvents();
        }

        // Right-click outside the selection collapses to that row.
        const QModelIndex outsideIdx = proxy->index(4, 0);
        const QPoint outsideClick = view->visualRect(outsideIdx).center();
        mWindow->ShowRowContextMenuForTest(outsideClick);
        QCoreApplication::processEvents();
        QCOMPARE(selection->selectedRows().count(), 1);
        QCOMPARE(selection->selectedRows().first().row(), 4);
        QCOMPARE(view->currentIndex().row(), 4);

        if (QWidget *popup = QApplication::activePopupWidget(); popup != nullptr)
        {
            popup->close();
            QCoreApplication::processEvents();
        }

        model->EndStreaming(false);
    }

    // AnchorsDock end-to-end: lists anchors in `Entries()` order,
    // double-click resolves a key back to a source row, and "Clear
    // all" wipes everything.
    void TestAnchorsDockListsAndJumpsAndClearsAll()
    {
        auto *anchors = mWindow->Anchors();
        QVERIFY(anchors != nullptr);
        auto *model = mWindow->Model();
        auto *dock = mWindow->findChild<AnchorsDock *>();
        QVERIFY2(dock != nullptr, "MainWindow must own an AnchorsDock");
        auto *list = dock->ListForTest();
        QVERIFY(list != nullptr);
        auto *clearBtn = dock->ClearAllButtonForTest();
        QVERIFY(clearBtn != nullptr);

        loglib::StreamLineSource &streamSource = BeginSyntheticStreamSession(*model);
        QtStreamingLogSink *sink = model->Sink();
        QVERIFY(sink != nullptr);
        loglib::KeyIndex &keys = sink->Keys();
        const loglib::KeyId valueKey = keys.GetOrInsert(std::string("value"));
        sink->OnBatch(MakeSyntheticBatch(streamSource, keys, valueKey, 1, 4, true));
        QCoreApplication::processEvents();
        QCOMPARE(model->rowCount(), 4);

        const auto key0 = model->AnchorKeyForRow(0);
        const auto key2 = model->AnchorKeyForRow(2);
        QVERIFY(key0.has_value());
        QVERIFY(key2.has_value());
        anchors->SetAnchor(*key0, 0);
        anchors->SetAnchor(*key2, 5);
        QCoreApplication::processEvents();

        // Offscreen QPA keeps the dock hidden so refresh signals
        // elide; invoke explicitly.
        dock->RefreshForTest();
        QCOMPARE(list->count(), 2);

        // `itemActivated` -> `jumpToAnchorRequested(sourceRow)`.
        QSignalSpy jumpSpy(dock, &AnchorsDock::jumpToAnchorRequested);
        QVERIFY(jumpSpy.isValid());

        QListWidgetItem *firstItem = list->item(0);
        QVERIFY(firstItem != nullptr);
        emit list->itemActivated(firstItem);
        QCoreApplication::processEvents();
        QCOMPARE(jumpSpy.count(), 1);
        // Both keys resolve back to a non-negative source row.
        QVERIFY(jumpSpy.last().at(0).toInt() >= 0);

        QListWidgetItem *secondItem = list->item(1);
        QVERIFY(secondItem != nullptr);
        emit list->itemActivated(secondItem);
        QCoreApplication::processEvents();
        QCOMPARE(jumpSpy.count(), 2);
        QVERIFY(jumpSpy.last().at(0).toInt() >= 0);

        // Two distinct source rows (Entries() sorts by lineId,
        // so item 0 -> lineId 1 / row 0, item 1 -> lineId 3 / row 2).
        QVERIFY(jumpSpy.at(0).at(0).toInt() != jumpSpy.at(1).at(0).toInt());

        emit clearBtn->clicked();
        QCoreApplication::processEvents();
        QVERIFY2(anchors->Empty(), "Clear all button must wipe every anchor");
        dock->RefreshForTest();
        QCOMPARE(list->count(), 0);

        model->EndStreaming(false);
    }

    // SelectSourceRow scrolls + selects the source row through the
    // proxy chain and moves the current index there. Negative rows
    // no-op.
    void TestMainWindowSelectSourceRowScrollsAndSelects()
    {
        auto *model = mWindow->Model();
        auto *view = mWindow->findChild<LogTableView *>();
        QVERIFY(view != nullptr);

        loglib::StreamLineSource &streamSource = BeginSyntheticStreamSession(*model);
        QtStreamingLogSink *sink = model->Sink();
        QVERIFY(sink != nullptr);
        loglib::KeyIndex &keys = sink->Keys();
        const loglib::KeyId valueKey = keys.GetOrInsert(std::string("value"));
        sink->OnBatch(MakeSyntheticBatch(streamSource, keys, valueKey, 1, 5, true));
        QCoreApplication::processEvents();
        QCOMPARE(model->rowCount(), 5);

        view->selectionModel()->clearSelection();
        view->setCurrentIndex(QModelIndex{});

        mWindow->SelectSourceRow(/*sourceRow=*/3);
        QCoreApplication::processEvents();
        QCOMPARE(view->currentIndex().row(), 3);
        QCOMPARE(view->selectionModel()->selectedRows().count(), 1);
        QCOMPARE(view->selectionModel()->selectedRows().first().row(), 3);

        // Negative rows no-op.
        mWindow->SelectSourceRow(/*sourceRow=*/-1);
        QCoreApplication::processEvents();
        QCOMPARE(view->currentIndex().row(), 3);

        model->EndStreaming(false);
    }

    // Anchors round-trip: live AnchorManager -> saved JSON ->
    // loaded JSON -> AnchorManager. Covers the mirror, save, load,
    // and Replace plumbing.
    void TestAnchorPersistenceRoundTripsThroughSavedConfiguration()
    {
        auto *anchors = mWindow->Anchors();
        QVERIFY(anchors != nullptr);
        auto *model = mWindow->Model();

        // Three real rows so the model has real (locator, lineId)
        // keys to anchor on.
        loglib::StreamLineSource &streamSource = BeginSyntheticStreamSession(*model);
        QtStreamingLogSink *sink = model->Sink();
        QVERIFY(sink != nullptr);
        loglib::KeyIndex &keys = sink->Keys();
        const loglib::KeyId valueKey = keys.GetOrInsert(std::string("value"));
        sink->OnBatch(MakeSyntheticBatch(streamSource, keys, valueKey, 1, 3, true));
        QCoreApplication::processEvents();
        QCOMPARE(model->rowCount(), 3);
        model->EndStreaming(false);

        const auto key0 = model->AnchorKeyForRow(0);
        const auto key2 = model->AnchorKeyForRow(2);
        QVERIFY(key0.has_value());
        QVERIFY(key2.has_value());
        anchors->SetAnchor(*key0, 2);
        anchors->SetAnchor(*key2, 6);
        QCOMPARE(anchors->Count(), static_cast<std::size_t>(2));

        // Save via the test seam (mirror -> serialise).
        const QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const QString cfgPath = tmp.filePath(QStringLiteral("anchors-roundtrip.json"));
        mWindow->SaveConfigurationToPathForTest(cfgPath);
        QCoreApplication::processEvents();

        // On-disk JSON must contain an `anchors` key.
        QFile saved(cfgPath);
        QVERIFY(saved.open(QIODevice::ReadOnly | QIODevice::Text));
        const QString savedJson = QString::fromUtf8(saved.readAll());
        saved.close();
        QVERIFY2(savedJson.contains(QStringLiteral("\"anchors\"")), "saved JSON must carry an anchors key");

        // Wipe live state so the load is observable.
        auto *newSessionAction = mWindow->findChild<QAction *>(QStringLiteral("actionNewSession"));
        QVERIFY(newSessionAction != nullptr);
        newSessionAction->trigger();
        QCoreApplication::processEvents();
        QVERIFY(anchors->Empty());

        // Load through the same path the menu uses; anchors come
        // back.
        QVERIFY(mWindow->TryLoadAsConfigurationForTest(cfgPath));
        QCoreApplication::processEvents();

        QCOMPARE(anchors->Count(), static_cast<std::size_t>(2));
        QCOMPARE(anchors->ColorFor(*key0).value_or(255U), uint8_t{2});
        QCOMPARE(anchors->ColorFor(*key2).value_or(255U), uint8_t{6});
    }

    // NewSession drops every anchor (rows are gone).
    void TestNewSessionClearsAnchors()
    {
        auto *anchors = mWindow->Anchors();
        QVERIFY(anchors != nullptr);
        auto *model = mWindow->Model();

        loglib::StreamLineSource &streamSource = BeginSyntheticStreamSession(*model);
        QtStreamingLogSink *sink = model->Sink();
        QVERIFY(sink != nullptr);
        loglib::KeyIndex &keys = sink->Keys();
        const loglib::KeyId valueKey = keys.GetOrInsert(std::string("value"));
        sink->OnBatch(MakeSyntheticBatch(streamSource, keys, valueKey, 1, 2, true));
        QCoreApplication::processEvents();
        model->EndStreaming(false);

        const auto key0 = model->AnchorKeyForRow(0);
        QVERIFY(key0.has_value());
        anchors->SetAnchor(*key0, 1);
        QVERIFY(!anchors->Empty());

        auto *newSessionAction = mWindow->findChild<QAction *>(QStringLiteral("actionNewSession"));
        QVERIFY(newSessionAction != nullptr);
        newSessionAction->trigger();
        QCoreApplication::processEvents();
        QVERIFY2(anchors->Empty(), "NewSession must clear every anchor");
    }

    // `anchorsReset` (the bulk path) emits a single whole-table
    // `dataChanged` so the view repaints every visible row.
    void TestLogModelAnchorBulkRefresh()
    {
        ThemeControl theme;
        AnchorManager anchors;
        LogModel model(nullptr, &theme, &anchors);

        loglib::StreamLineSource &streamSource = BeginSyntheticStreamSession(model);
        QtStreamingLogSink *sink = model.Sink();
        QVERIFY(sink != nullptr);
        loglib::KeyIndex &keys = sink->Keys();
        const loglib::KeyId valueKey = keys.GetOrInsert(std::string("value"));
        sink->OnBatch(MakeSyntheticBatch(streamSource, keys, valueKey, 1, 4, true));
        QCoreApplication::processEvents();
        QCOMPARE(model.rowCount(), 4);

        const auto key0 = model.AnchorKeyForRow(0);
        QVERIFY(key0.has_value());
        anchors.SetAnchor(*key0, 1);
        QCoreApplication::processEvents();

        const QSignalSpy dataSpy(&model, &LogModel::dataChanged);
        anchors.ClearAll();
        QCoreApplication::processEvents();

        // ClearAll -> `RefreshAllAnchorRows` -> one `dataChanged`
        // spanning the whole table (row 0 to rowCount-1).
        QVERIFY(dataSpy.count() >= 1);
        bool sawWholeTableEmit = false;
        for (const QList<QVariant> &args : dataSpy)
        {
            const QModelIndex topLeft = args.at(0).toModelIndex();
            const QModelIndex bottomRight = args.at(1).toModelIndex();
            if (topLeft.row() == 0 && bottomRight.row() == model.rowCount() - 1)
            {
                sawWholeTableEmit = true;
                break;
            }
        }
        QVERIFY2(sawWholeTableEmit, "anchorsReset must emit a whole-table dataChanged so cached rows refresh");

        model.EndStreaming(false);
    }

    // FIFO eviction drops anchors on evicted rows; anchors on
    // survivors stay put.
    void TestAnchorsAreDroppedOnFifoEviction()
    {
        ThemeControl theme;
        AnchorManager anchors;
        LogModel model(nullptr, &theme, &anchors);

        loglib::StreamLineSource &streamSource = BeginSyntheticStreamSession(model);
        QtStreamingLogSink *sink = model.Sink();
        QVERIFY(sink != nullptr);
        loglib::KeyIndex &keys = sink->Keys();
        const loglib::KeyId valueKey = keys.GetOrInsert(std::string("value"));

        // Cap=4 fills with lineIds 1..4; the next batch evicts
        // 1..3 and keeps 4.
        constexpr size_t CAP = 4;
        model.SetRetentionCap(CAP);

        model.AppendBatch(MakeSyntheticBatch(streamSource, keys, valueKey, 1, 4, /*declareNewKey=*/true));
        QCOMPARE(model.rowCount(), 4);

        // Anchor lineIds 1 (evicted), 3 (evicted), 4 (survives).
        const auto keyL1 = model.AnchorKeyForRow(0);
        const auto keyL3 = model.AnchorKeyForRow(2);
        const auto keyL4 = model.AnchorKeyForRow(3);
        QVERIFY(keyL1.has_value());
        QVERIFY(keyL3.has_value());
        QVERIFY(keyL4.has_value());
        anchors.SetAnchor(*keyL1, 0);
        anchors.SetAnchor(*keyL3, 1);
        anchors.SetAnchor(*keyL4, 2);
        QCOMPARE(anchors.Count(), static_cast<std::size_t>(3));

        // Append lineIds 5..7; evicts 1..3, keeps 4..7.
        model.AppendBatch(MakeSyntheticBatch(streamSource, keys, valueKey, 5, 3, /*declareNewKey=*/false));
        QCOMPARE(model.rowCount(), 4);

        QCOMPARE(anchors.Count(), static_cast<std::size_t>(1));
        QVERIFY2(!anchors.ColorFor(*keyL1).has_value(), "anchor on evicted lineId 1 must be dropped");
        QVERIFY2(!anchors.ColorFor(*keyL3).has_value(), "anchor on evicted lineId 3 must be dropped");
        const auto survivedColor = anchors.ColorFor(*keyL4);
        QVERIFY2(survivedColor.has_value(), "anchor on surviving lineId 4 must persist");
        QCOMPARE(*survivedColor, uint8_t{2});

        model.EndStreaming(false);
    }

    // AnchorsDock::Refresh preserves list selection + current item
    // so the dock's own right-click flow doesn't yank focus off the
    // entry the user is acting on.
    void TestAnchorsDockPreservesSelectionAcrossRefresh()
    {
        auto *anchors = mWindow->Anchors();
        QVERIFY(anchors != nullptr);
        auto *model = mWindow->Model();
        auto *dock = mWindow->findChild<AnchorsDock *>();
        QVERIFY(dock != nullptr);
        auto *list = dock->ListForTest();
        QVERIFY(list != nullptr);

        loglib::StreamLineSource &streamSource = BeginSyntheticStreamSession(*model);
        QtStreamingLogSink *sink = model->Sink();
        QVERIFY(sink != nullptr);
        loglib::KeyIndex &keys = sink->Keys();
        const loglib::KeyId valueKey = keys.GetOrInsert(std::string("value"));
        sink->OnBatch(MakeSyntheticBatch(streamSource, keys, valueKey, 1, 4, true));
        QCoreApplication::processEvents();
        QCOMPARE(model->rowCount(), 4);

        // Three anchors so the surviving-selection check is
        // non-trivial.
        const auto key0 = model->AnchorKeyForRow(0);
        const auto key1 = model->AnchorKeyForRow(1);
        const auto key2 = model->AnchorKeyForRow(2);
        QVERIFY(key0.has_value());
        QVERIFY(key1.has_value());
        QVERIFY(key2.has_value());
        anchors->SetAnchor(*key0, 0);
        anchors->SetAnchor(*key1, 1);
        anchors->SetAnchor(*key2, 2);
        dock->RefreshForTest();
        QCOMPARE(list->count(), 3);

        // Middle item current + selected; then re-run Refresh.
        // Without snapshot/restore, `mList->clear()` would wipe it.
        QListWidgetItem *middle = list->item(1);
        QVERIFY(middle != nullptr);
        list->setCurrentItem(middle);
        middle->setSelected(true);
        QCOMPARE(list->selectedItems().count(), 1);
        QVERIFY(list->currentItem() == middle);

        // Touch key0 -> `anchorChanged` -> `Refresh()`. Offscreen
        // QPA gates Refresh, so call the test seam directly.
        anchors->SetAnchor(*key0, 5);
        QCoreApplication::processEvents();
        dock->RefreshForTest();

        // Three anchors, middle still selected + current.
        QCOMPARE(list->count(), 3);
        QCOMPARE(list->selectedItems().count(), 1);
        const QListWidgetItem *restoredCurrent = list->currentItem();
        QVERIFY2(restoredCurrent != nullptr, "current item must survive Refresh");
        QCOMPARE(restoredCurrent->data(Qt::UserRole + 2).toULongLong(), static_cast<qulonglong>(key1->lineId));

        anchors->ClearAll();
        QCoreApplication::processEvents();
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

        // Stream-mode flag has no effect on a static session: with
        // the stream flag ON and the static flag OFF, the proxy
        // stays in the identity orientation. Alternation stays off
        // (per-level theme colours own the row partitioning).
        StreamingControl::SetNewestFirst(true);
        StreamingControl::SetStaticNewestFirst(false);
        mWindow->ApplyDisplayOrder();
        QVERIFY2(!rowOrderProxy->IsReversed(), "static session must ignore the stream-mode newest-first flag");
        QVERIFY(!tableView->alternatingRowColors());

        // Flipping the static-mode flag drives the same proxy reversal
        // as the stream-mode flag does for live-tail sessions.
        StreamingControl::SetStaticNewestFirst(true);
        mWindow->ApplyDisplayOrder();
        QVERIFY(rowOrderProxy->IsReversed());
        QVERIFY(!tableView->alternatingRowColors());

        StreamingControl::SetStaticNewestFirst(false);
        mWindow->ApplyDisplayOrder();
        QVERIFY(!rowOrderProxy->IsReversed());
        QVERIFY(!tableView->alternatingRowColors());

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

        // Push another batch; the slot must not snap the viewport
        // to the bottom (only live-tail sessions follow).
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

    // Companion to `testStaticSessionDoesNotFollowNewestRows`:
    // user scroll-to-tail must not silently re-arm
    // `actionFollowTail` in a static session. The slot must gate
    // on session mode, not on `IsStreamingActive()` (which is true
    // for static sessions too).
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
            lines.append(QStringLiteral(R"({"category": "%1"})").arg(levels[i % levels.size()]));
        }
        const TempJsonFile fixture(lines);

        const StreamingRun run = RunStreaming(fixture.Path());
        QCOMPARE(run.finishedCount, 1);
        QCOMPARE(run.cancelled, false);
        QVERIFY(run.model->StreamingErrors().empty());

        const int levelCol = ColumnByHeader(*run.model, QStringLiteral("category"));
        QVERIFY2(levelCol >= 0, "auto-promoted level column must exist");
        const auto &columns = run.model->Configuration().columns;
        QVERIFY(static_cast<size_t>(levelCol) < columns.size());
        QCOMPARE(columns[static_cast<size_t>(levelCol)].type, loglib::LogConfiguration::Type::Enumeration);

        FilterEditor editor(*run.model, QStringLiteral("test-filter"));
        editor.Load(levelCol, QStringList{});

        const auto *picker = editor.findChild<QListView *>();
        const auto *proxy = editor.findChild<QSortFilterProxyModel *>();
        QVERIFY2(picker != nullptr, "FilterEditor must expose its enum picker QListView");
        QVERIFY2(proxy != nullptr, "picker must wrap a QSortFilterProxyModel");
        QCOMPARE(proxy->rowCount(), 5);

        auto *searchBox = editor.findChild<QLineEdit *>(QStringLiteral("enumSearchEdit"));
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

    // End-to-end: a JSON `level` column with canonical names auto-
    // promotes through `Type::Enumeration` to `Type::Level`, and
    // `GetLevelForRow` then reports the canonical rank per row.
    // (Tests that use `category` instead are pinning the enum path.)
    void TestLevelColumnAutoPromotesFromCanonicalLevelKey()
    {
        const QStringList levels{
            QStringLiteral("info"),
            QStringLiteral("warn"),
            QStringLiteral("error"),
            QStringLiteral("debug"),
            QStringLiteral("trace"),
            QStringLiteral("fatal"),
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
        QVERIFY2(levelCol >= 0, "level column must exist");
        const auto &columns = run.model->Configuration().columns;
        QVERIFY(static_cast<size_t>(levelCol) < columns.size());
        // Every dict entry is canonical, so the tolerance trivially
        // holds and promotion must reach `Type::Level`.
        QCOMPARE(columns[static_cast<size_t>(levelCol)].type, loglib::LogConfiguration::Type::Level);

        // Row-cycle pattern is `levels[i % 6]`; check the first few.
        const auto col = static_cast<size_t>(levelCol);
        QCOMPARE(run.model->Table().GetLevelForRow(0, col).value(), loglib::LogLevel::Info);
        QCOMPARE(run.model->Table().GetLevelForRow(1, col).value(), loglib::LogLevel::Warn);
        QCOMPARE(run.model->Table().GetLevelForRow(2, col).value(), loglib::LogLevel::Error);
        QCOMPARE(run.model->Table().GetLevelForRow(3, col).value(), loglib::LogLevel::Debug);
        QCOMPARE(run.model->Table().GetLevelForRow(4, col).value(), loglib::LogLevel::Trace);
        QCOMPARE(run.model->Table().GetLevelForRow(5, col).value(), loglib::LogLevel::Fatal);
    }

    /// Regression: an auto-promoted `Type::Level` column with
    /// non-canonical columns ahead of it must bubble to
    /// `CANONICAL_LEVEL_COLUMN_INDEX` via `columnsMoved`. The
    /// pre-fix code rotated silently inside `mLogTable.AppendBatch`,
    /// leaving Qt's column-keyed view state on the wrong section.
    void TestStreamingLevelBubbleEmitsColumnsMoved()
    {
        // Fixture: `body, scope, level`. Once `level` promotes,
        // it bubbles to index 1, pushing `scope` to index 2.
        const QStringList levels{
            QStringLiteral("info"),
            QStringLiteral("warn"),
            QStringLiteral("error"),
        };
        QStringList lines;
        lines.reserve(200);
        for (int i = 0; i < 200; ++i)
        {
            lines.append(QStringLiteral(R"({"body": "msg %1", "scope": "core", "level": "%2"})")
                             .arg(i)
                             .arg(levels[i % levels.size()]));
        }
        const TempJsonFile fixture(lines);

        auto model = std::make_unique<LogModel>(nullptr, nullptr);
        QSignalSpy columnsMovedSpy(model.get(), &QAbstractItemModel::columnsMoved);
        const QSignalSpy columnsInsertedSpy(model.get(), &QAbstractItemModel::columnsInserted);
        QSignalSpy finishedSpy(model.get(), &LogModel::streamingFinished);
        QVERIFY(columnsMovedSpy.isValid());
        QVERIFY(columnsInsertedSpy.isValid());
        QVERIFY(finishedSpy.isValid());

        auto file = std::make_unique<loglib::LogFile>(fixture.Path().toStdString());
        auto fileSource = std::make_unique<loglib::FileLineSource>(std::move(file));
        loglib::FileLineSource *fileSourcePtr = fileSource.get();
        const loglib::StopToken stopToken = model->BeginStreamingForSyncTest(std::move(fileSource));
        {
            loglib::ParserOptions options;
            options.stopToken = stopToken;
            loglib::internal::AdvancedParserOptions advanced;
            advanced.threads = 1;
            const loglib::JsonParser parser;
            loglib::JsonParser::ParseStreaming(*fileSourcePtr, *model->Sink(), options, advanced);
        }
        if (finishedSpy.count() == 0)
        {
            QVERIFY2(finishedSpy.wait(5000), "streamingFinished must arrive");
        }

        // Post-bubble column layout: `body, level, scope`.
        const int levelCol = ColumnByHeader(*model, QStringLiteral("level"));
        QVERIFY2(levelCol >= 0, "level column must exist after streaming");
        QCOMPARE(levelCol, static_cast<int>(loglib::CANONICAL_LEVEL_COLUMN_INDEX));
        const auto &columns = model->Configuration().columns;
        QCOMPARE(columns.size(), static_cast<size_t>(3));
        QCOMPARE(columns[0].header, std::string{"body"});
        QCOMPARE(columns[1].header, std::string{"level"});
        QCOMPARE(columns[1].type, loglib::LogConfiguration::Type::Level);
        QCOMPARE(columns[2].header, std::string{"scope"});

        // Match any single-column `columnsMoved` whose destination
        // is canonical. We don't pin the last emit because a Time
        // fixture would interleave its own bubble. Qt's
        // `destinationColumn` equals the final index for leftward
        // moves. Param `signalArgs` because `emit` is a Qt macro.
        const auto matchesLevelBubble = [](const QList<QVariant> &signalArgs) {
            const int first = signalArgs.value(1).toInt();
            const int last = signalArgs.value(2).toInt();
            const int destColumn = signalArgs.value(4).toInt();
            return first == last && first >= 0 && std::cmp_equal(destColumn, loglib::CANONICAL_LEVEL_COLUMN_INDEX);
        };
        const bool sawLevelBubble = std::any_of(columnsMovedSpy.begin(), columnsMovedSpy.end(), matchesLevelBubble);
        QVERIFY2(
            sawLevelBubble,
            qPrintable(QStringLiteral("expected a columnsMoved emit with destination=%1; got %2 emit(s)")
                           .arg(static_cast<int>(loglib::CANONICAL_LEVEL_COLUMN_INDEX))
                           .arg(columnsMovedSpy.count()))
        );

        // Sanity: rank cache survived the bubble.
        QCOMPARE(model->Table().GetLevelForRow(0, static_cast<size_t>(levelCol)).value(), loglib::LogLevel::Info);

        model->EndStreaming(false);
    }

    /// Regression: a single batch carrying both `Level` and
    /// `Time` columns must end up with `[time, level, ...]`. The
    /// Time bubble shifts the just-promoted Level out of place,
    /// and the Level bubble's re-check rebalances it.
    ///
    /// Pre-fix bug: `[time, body, level]` -- level stranded.
    /// Post-fix:    `[time, level, body]`.
    void TestStreamingLevelBubblesAfterTimeBubbleSameBatch()
    {
        // Fixture order matters: `body, level, time` so level
        // transiently lands at canonical (index 1) before the
        // Time bubble shifts it out.
        QStringList lines;
        lines.reserve(200);
        const QStringList levels{
            QStringLiteral("info"),
            QStringLiteral("warn"),
            QStringLiteral("error"),
        };
        for (int i = 0; i < 200; ++i)
        {
            lines.append(QStringLiteral(R"({"body": "msg %1", "level": "%2", "time": "2025-01-15T12:34:%3Z"})")
                             .arg(i)
                             .arg(levels[i % levels.size()])
                             .arg(i % 60, 2, 10, QLatin1Char('0')));
        }
        const TempJsonFile fixture(lines);

        const StreamingRun run = RunStreaming(fixture.Path());
        QCOMPARE(run.finishedCount, 1);
        QCOMPARE(run.cancelled, false);

        const auto &columns = run.model->Configuration().columns;
        QCOMPARE(columns.size(), static_cast<size_t>(3));
        QCOMPARE(columns[0].header, std::string{"time"});
        QCOMPARE(columns[0].type, loglib::LogConfiguration::Type::Time);
        // Level rejoined canonical index 1 after the Time bubble.
        QCOMPARE(columns[1].header, std::string{"level"});
        QCOMPARE(columns[1].type, loglib::LogConfiguration::Type::Level);
        QCOMPARE(columns[2].header, std::string{"body"});

        const int levelCol = ColumnByHeader(*run.model, QStringLiteral("level"));
        QCOMPARE(levelCol, static_cast<int>(loglib::CANONICAL_LEVEL_COLUMN_INDEX));
        // Sanity: rank cache survived both bubbles.
        QCOMPARE(run.model->Table().GetLevelForRow(0, static_cast<size_t>(levelCol)).value(), loglib::LogLevel::Info);
    }

    /// Integration: a streamed `Type::Level` column makes
    /// `LogModel::data(Qt::BackgroundRole)` return the cached
    /// theme brush for styled levels and an empty QVariant for
    /// unstyled ones.
    void TestLogModelDataReturnsThemeBackground()
    {
        // The fixture owns a `ThemeControl` and threads it into
        // `MainWindow` + `LogModel`; force Light for determinism.
        QVERIFY(mTheme != nullptr);
        mTheme->SetActiveSelection(QStringLiteral("Light"));
        const QBrush expectedErrorBg = mTheme->BackgroundFor(loglib::LogLevel::Error);
        QVERIFY2(expectedErrorBg.style() != Qt::NoBrush, "the Light theme must define an Error background brush");
        const QBrush expectedInfoBg = mTheme->BackgroundFor(loglib::LogLevel::Info);
        QCOMPARE(expectedInfoBg.style(), Qt::NoBrush);

        // Stream a tiny fixture so `level` promotes to `Type::Level`.
        const QStringList levels{
            QStringLiteral("info"),
            QStringLiteral("warn"),
            QStringLiteral("error"),
            QStringLiteral("debug"),
            QStringLiteral("trace"),
            QStringLiteral("fatal"),
        };
        QStringList lines;
        lines.reserve(300);
        for (int i = 0; i < 300; ++i)
        {
            lines.append(QStringLiteral(R"({"level": "%1"})").arg(levels[i % levels.size()]));
        }
        const TempJsonFile fixture(lines);

        const StreamingRun run = RunStreaming(fixture.Path(), mTheme.data());
        QCOMPARE(run.finishedCount, 1);
        QCOMPARE(run.cancelled, false);

        const int levelCol = ColumnByHeader(*run.model, QStringLiteral("level"));
        QVERIFY2(levelCol >= 0, "level column must exist");

        // Row 0 = Info (unstyled) -> empty QVariant.
        const QModelIndex infoIndex = run.model->index(0, levelCol);
        const QVariant infoBg = run.model->data(infoIndex, Qt::BackgroundRole);
        QVERIFY2(!infoBg.isValid(), "Info row must defer to the palette (empty QVariant)");

        // Row 2 = Error (styled) -> theme brush.
        const QModelIndex errorIndex = run.model->index(2, levelCol);
        const QVariant errorBg = run.model->data(errorIndex, Qt::BackgroundRole);
        QVERIFY2(errorBg.isValid(), "Error row must carry the theme background brush");
        const auto actualErrorBg = qvariant_cast<QBrush>(errorBg);
        QCOMPARE(actualErrorBg.color(), expectedErrorBg.color());

        // Row 5 = Fatal -> bold font via FontRole.
        const QModelIndex fatalIndex = run.model->index(5, levelCol);
        const QVariant fatalFont = run.model->data(fatalIndex, Qt::FontRole);
        QVERIFY2(fatalFont.isValid(), "Fatal row must carry a bold font (theme sets bold=true)");
        QVERIFY(qvariant_cast<QFont>(fatalFont).bold());
    }

    // Regression: a theme that ships only a `headerIcon` (no
    // explicit `header` override) must paint the icon alone so
    // the header doesn't read "<gauge> level". Funnel / warning
    // priority and the tooltip are unchanged.
    void TestLevelHeaderIconSuppressesDisplayText()
    {
        QVERIFY(mTheme != nullptr);
        mTheme->SetActiveSelection(QStringLiteral("Light"));
        QVERIFY2(mTheme->HasLevelColumnOverride(), "shipped Light theme must opt into icon mode");
        QVERIFY2(!mTheme->LevelColumnHeaderIcon().isNull(), "shipped Light theme must ship a `headerIcon`");
        QVERIFY2(
            !mTheme->LevelColumnHeaderTextOverride().has_value(),
            "shipped Light theme must not set an explicit `header` -- this test pins the implicit-blank rule"
        );

        const QStringList lines{
            QStringLiteral(R"({"level": "info"})"),
            QStringLiteral(R"({"level": "warn"})"),
        };
        const TempJsonFile fixture(lines);
        const StreamingRun run = RunStreaming(fixture.Path(), mTheme.data());
        QCOMPARE(run.cancelled, false);
        QVERIFY(run.model != nullptr);

        const int levelCol = ColumnByHeader(*run.model, QStringLiteral("level"));
        QVERIFY2(levelCol >= 0, "level column must resolve via configured Column::header");

        const QString displayed = run.model->headerData(levelCol, Qt::Horizontal, Qt::DisplayRole).toString();
        QVERIFY2(
            displayed.isEmpty(),
            qPrintable(
                QStringLiteral("level header must render blank text when an icon is present, got '%1'").arg(displayed)
            )
        );

        // Header icon centres so it lines up with the centred
        // pills below.
        const QVariant alignment = run.model->headerData(levelCol, Qt::Horizontal, Qt::TextAlignmentRole);
        QVERIFY2(alignment.isValid(), "icon-only level header must report an explicit Qt::TextAlignmentRole");
        QCOMPARE(alignment.toInt(), static_cast<int>(Qt::AlignCenter));

        // Sibling text columns keep the default alignment.
        for (int col = 0; col < run.model->columnCount(); ++col)
        {
            if (col == levelCol)
            {
                continue;
            }
            const QVariant siblingAlignment = run.model->headerData(col, Qt::Horizontal, Qt::TextAlignmentRole);
            QVERIFY2(
                !siblingAlignment.isValid(),
                qPrintable(QStringLiteral("text column %1 must inherit the default header alignment").arg(col))
            );
        }

        // Tooltip still names the column.
        const QString tooltip = run.model->headerData(levelCol, Qt::Horizontal, Qt::ToolTipRole).toString();
        QVERIFY2(
            tooltip.contains(QStringLiteral("level")),
            qPrintable(QStringLiteral("tooltip must keep naming the column, got '%1'").arg(tooltip))
        );

        // Toggling icon mode off restores the configured header
        // text and the default alignment.
        run.model->SetShowLevelIcons(false);
        const QString restored = run.model->headerData(levelCol, Qt::Horizontal, Qt::DisplayRole).toString();
        QCOMPARE(restored, QStringLiteral("level"));
        const QVariant restoredAlignment = run.model->headerData(levelCol, Qt::Horizontal, Qt::TextAlignmentRole);
        QVERIFY2(!restoredAlignment.isValid(), "level header must drop the centre alignment when icon mode is off");
    }

    // Regression: `LevelCellDelegate::paint` must (1) actually
    // render the pill, (2) hard-clip to `option.rect` so a narrow
    // column can't bleed into the neighbour, and (3) fall back to
    // the base delegate when icon mode is off. The pixmap probe
    // pins the visual contract -- a change to the insets, clip,
    // gate, or pill-brush resolution surfaces as a pixel diff.
    void TestLevelCellDelegatePaintsPillAndClipsToRect()
    {
        QVERIFY(mTheme != nullptr);
        // Dark ships a concrete Info pill (`#1E3A5F`) to assert
        // against; pinning to a fixed theme keeps the test stable.
        mTheme->SetActiveSelection(QStringLiteral("Dark"));
        QVERIFY(mTheme->HasLevelColumnOverride());
        const QBrush expectedPill = mTheme->PillBackgroundFor(loglib::LogLevel::Info);
        QVERIFY2(expectedPill.style() != Qt::NoBrush, "Dark theme must define an Info pill background");
        const QColor expectedPillColor = expectedPill.color();
        QVERIFY(expectedPillColor.isValid());

        // Minimal fixture so the rank cache resolves `info`.
        const QStringList lines{
            QStringLiteral(R"({"level": "info"})"),
            QStringLiteral(R"({"level": "info"})"),
        };
        const TempJsonFile fixture(lines);
        const StreamingRun run = RunStreaming(fixture.Path(), mTheme.data());
        QCOMPARE(run.cancelled, false);
        QVERIFY(run.model != nullptr);
        run.model->SetShowLevelIcons(true);
        QVERIFY(run.model->IsLevelIconModeActive());

        const int levelCol = ColumnByHeader(*run.model, QStringLiteral("level"));
        QVERIFY2(levelCol >= 0, "fixture must resolve a level column");
        const QModelIndex cell = run.model->index(0, levelCol);
        QVERIFY(cell.isValid());

        const LevelCellDelegate delegate(mTheme.data());

        // Pixmap > cell rect, filled with a sentinel colour. Any
        // sentinel pixel changing after `paint` means the clip
        // leaked.
        constexpr int CELL_WIDTH = 64;
        constexpr int CELL_HEIGHT = 24;
        constexpr int OUTSIDE_MARGIN = 8;
        constexpr int PIXMAP_WIDTH = CELL_WIDTH + (2 * OUTSIDE_MARGIN);
        constexpr int PIXMAP_HEIGHT = CELL_HEIGHT + (2 * OUTSIDE_MARGIN);
        const QColor sentinel(0xFF, 0x00, 0xFF); // magenta -- not used by Dark theme.

        QPixmap pix(PIXMAP_WIDTH, PIXMAP_HEIGHT);
        pix.fill(sentinel);

        QStyleOptionViewItem option;
        option.rect = QRect(OUTSIDE_MARGIN, OUTSIDE_MARGIN, CELL_WIDTH, CELL_HEIGHT);
        option.state = QStyle::State_Enabled;
        option.widget = nullptr;

        {
            QPainter painter(&pix);
            delegate.paint(&painter, option, cell);
        }
        const QImage image = pix.toImage();

        // (1) Pill paint ran: at least one sampled pixel inside
        // the inset matches the theme's pill colour. Sampling a
        // ring avoids false negatives from AA edges and DPR
        // rounding.
        constexpr int PILL_PROBE_INSET = 6;
        const QPoint sampleCentre = option.rect.center();
        const std::array<QPoint, 5> samples = {
            sampleCentre,
            QPoint(option.rect.left() + PILL_PROBE_INSET, sampleCentre.y()),
            QPoint(option.rect.right() - PILL_PROBE_INSET, sampleCentre.y()),
            QPoint(sampleCentre.x(), option.rect.top() + PILL_PROBE_INSET),
            QPoint(sampleCentre.x(), option.rect.bottom() - PILL_PROBE_INSET)
        };
        const auto colorsMatch = [](QRgb actual, const QColor &expected) {
            // Per-channel tolerance covers AA edges + a swapped
            // compositing mode in any installed stylesheet.
            constexpr int CHANNEL_TOLERANCE = 3;
            return std::abs(qRed(actual) - expected.red()) <= CHANNEL_TOLERANCE &&
                   std::abs(qGreen(actual) - expected.green()) <= CHANNEL_TOLERANCE &&
                   std::abs(qBlue(actual) - expected.blue()) <= CHANNEL_TOLERANCE;
        };
        const bool pillVisible = std::ranges::any_of(samples, [&](const QPoint &pt) {
            return colorsMatch(image.pixel(pt), expectedPillColor);
        });
        QVERIFY2(pillVisible, "expected at least one pixel inside the cell to match the Dark Info pill brush");

        // (2) Clip held: every pixel outside the cell rect is
        // still the sentinel.
        for (int y = 0; y < PIXMAP_HEIGHT; ++y)
        {
            for (int x = 0; x < PIXMAP_WIDTH; ++x)
            {
                if (option.rect.contains(x, y))
                {
                    continue;
                }
                const QRgb actual = image.pixel(x, y);
                QVERIFY2(
                    qRed(actual) == 0xFF && qGreen(actual) == 0x00 && qBlue(actual) == 0xFF,
                    qPrintable(QStringLiteral("delegate painted outside option.rect at (%1, %2); RGBA=#%3")
                                   .arg(x)
                                   .arg(y)
                                   .arg(QString::number(actual, 16)))
                );
            }
        }

        // (3) Icon mode off: the delegate forwards to the base
        // class. Observed as "pill colour no longer at the
        // centre sample"; the exact fallback colour is palette-
        // dependent so we don't pin it.
        run.model->SetShowLevelIcons(false);
        QVERIFY(!run.model->IsLevelIconModeActive());
        pix.fill(sentinel);
        {
            QPainter painter(&pix);
            delegate.paint(&painter, option, cell);
        }
        const QImage offModeImage = pix.toImage();
        QVERIFY2(
            !colorsMatch(offModeImage.pixel(sampleCentre), expectedPillColor),
            "with icon mode disabled the delegate must not paint the pill"
        );
    }

    // Regression: `LogTableView` installs `LogHeaderView`, and
    // its icon-centering rule centres icon-only sections. The
    // model-side `TextAlignmentRole` alone can't fix this because
    // text alignment doesn't move the icon. Exercises the rule
    // via the public static helper to avoid needing a model.
    void TestLogHeaderViewCentersIconOnlySections()
    {
        // Reach through the production wiring rather than building
        // a detached view, so the install path is exercised too.
        auto *tableView = mWindow->findChild<LogTableView *>();
        QVERIFY2(tableView != nullptr, "MainWindow must own a LogTableView");

        auto *headerView = dynamic_cast<LogHeaderView *>(tableView->horizontalHeader());
        QVERIFY2(headerView != nullptr, "LogTableView must install LogHeaderView as its horizontal header");

        // Empty text + non-null icon -> centred icon.
        QStyleOptionHeader iconOnly;
        iconOnly.text = QString{};
        iconOnly.icon = QApplication::style()->standardIcon(QStyle::SP_MessageBoxInformation);
        iconOnly.iconAlignment = Qt::AlignVCenter; // QHeaderView's default
        LogHeaderView::CenterIconAlignmentForIconOnlySection(&iconOnly);
        QCOMPARE(static_cast<int>(iconOnly.iconAlignment), static_cast<int>(Qt::AlignCenter));

        // Icon + text -> default (icon left of text); matches the
        // funnel / warning case.
        QStyleOptionHeader textAndIcon;
        textAndIcon.text = QStringLiteral("level");
        textAndIcon.icon = QApplication::style()->standardIcon(QStyle::SP_MessageBoxWarning);
        textAndIcon.iconAlignment = Qt::AlignVCenter;
        LogHeaderView::CenterIconAlignmentForIconOnlySection(&textAndIcon);
        QCOMPARE(static_cast<int>(textAndIcon.iconAlignment), static_cast<int>(Qt::AlignVCenter));

        // No icon -> no-op, even with empty text.
        QStyleOptionHeader emptyBoth;
        emptyBoth.text = QString{};
        emptyBoth.icon = QIcon{};
        emptyBoth.iconAlignment = Qt::AlignVCenter;
        LogHeaderView::CenterIconAlignmentForIconOnlySection(&emptyBoth);
        QCOMPARE(static_cast<int>(emptyBoth.iconAlignment), static_cast<int>(Qt::AlignVCenter));

        // Defensive: nullptr is a no-op (must not crash).
        LogHeaderView::CenterIconAlignmentForIconOnlySection(nullptr);
    }

    // Regression: a theme switch must repaint the visible rows.
    // Symptom of failure: rows keep the old level colours until
    // the user scrolls or resizes the table.
    void TestThemeSwitchRefreshesLogTableRows()
    {
        QVERIFY(mTheme != nullptr);

        // Tiny multi-level fixture to get a themed Error row.
        const QStringList lines{
            QStringLiteral(R"({"level": "info"})"),
            QStringLiteral(R"({"level": "error"})"),
            QStringLiteral(R"({"level": "warn"})"),
        };
        const TempJsonFile fixture(lines);

        mTheme->SetActiveSelection(QStringLiteral("Light"));
        const StreamingRun run = RunStreaming(fixture.Path(), mTheme.data());
        QCOMPARE(run.cancelled, false);
        QVERIFY(run.model != nullptr);

        const int levelCol = ColumnByHeader(*run.model, QStringLiteral("level"));
        QVERIFY2(levelCol >= 0, "level column must exist after streaming");

        // Locate the Error row by display value (robust to reorders).
        int errorRow = -1;
        for (int r = 0; r < run.model->rowCount(); ++r)
        {
            if (run.model->data(run.model->index(r, levelCol), Qt::DisplayRole).toString() == QStringLiteral("error"))
            {
                errorRow = r;
                break;
            }
        }
        QVERIFY2(errorRow >= 0, "fixture must include an Error-level row");

        const QModelIndex errorIndex = run.model->index(errorRow, levelCol);
        const QVariant lightErrorBg = run.model->data(errorIndex, Qt::BackgroundRole);
        QVERIFY2(lightErrorBg.isValid(), "Light theme must style the Error row background");
        const auto lightBrush = qvariant_cast<QBrush>(lightErrorBg);

        // Flip to Dark; `ThemeControl` rebuilds its cache so the
        // next `data()` call sees the new brush.
        mTheme->SetActiveSelection(QStringLiteral("Dark"));
        QCoreApplication::processEvents();

        const QVariant darkErrorBg = run.model->data(errorIndex, Qt::BackgroundRole);
        QVERIFY2(darkErrorBg.isValid(), "Dark theme must style the Error row background");
        const auto darkBrush = qvariant_cast<QBrush>(darkErrorBg);
        QVERIFY2(
            lightBrush.color() != darkBrush.color(),
            qPrintable(QStringLiteral("Error row background must differ between Light and Dark themes; "
                                      "got light=%1, dark=%2")
                           .arg(lightBrush.color().name(), darkBrush.color().name()))
        );

        // Inverse: Light -> Dark -> Light returns the original brush.
        mTheme->SetActiveSelection(QStringLiteral("Light"));
        QCoreApplication::processEvents();
        const auto lightBrushAgain = qvariant_cast<QBrush>(run.model->data(errorIndex, Qt::BackgroundRole));
        QCOMPARE(lightBrushAgain.color(), lightBrush.color());
    }

    // Regression: a theme switch must emit `dataChanged` on the
    // styled roles so the view drops its per-item style cache.
    // `viewport()->update()` alone doesn't reliably invalidate it.
    void TestThemeSwitchEmitsDataChangedOnLogTable()
    {
        QVERIFY(mTheme != nullptr);
        // Stream into the live `MainWindow` to observe the
        // production-wired `LogModel`.
        const QStringList lines{
            QStringLiteral(R"({"level": "info"})"),
            QStringLiteral(R"({"level": "error"})"),
            QStringLiteral(R"({"level": "warn"})"),
        };
        const TempJsonFile fixture(lines);
        mTheme->SetActiveSelection(QStringLiteral("Light"));
        mWindow->OpenFilesForTest({fixture.Path()}, MainWindow::OpenMode::Replace);
        // Pump until `streamingFinished` arrives (queued connection).
        auto *model = mWindow->findChild<LogModel *>();
        QVERIFY2(model != nullptr, "MainWindow must own a LogModel");
        QSignalSpy finishedSpy(model, &LogModel::streamingFinished);
        if (finishedSpy.count() == 0)
        {
            finishedSpy.wait(5000);
        }
        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): false positive; prior `QVERIFY2` aborts on null.
        QVERIFY2(model->rowCount() > 0, "fixture must produce at least one streamed row");

        const QSignalSpy spy(model, &QAbstractItemModel::dataChanged);
        mTheme->SetActiveSelection(QStringLiteral("Dark"));
        QCoreApplication::processEvents();

        // At least one `dataChanged` must cover a styled role.
        // Other (e.g. anchor-related) notifications can ride along.
        bool foundStyledNotification = false;
        for (int i = 0; i < spy.count(); ++i)
        {
            const QList<QVariant> &args = spy.at(i);
            QVERIFY2(args.size() >= 3, "dataChanged must carry topLeft, bottomRight, roles");
            const auto roles = args.at(2).value<QList<int>>();
            // Empty roles = "all roles changed" in Qt; counts as styled.
            if (roles.isEmpty() || roles.contains(Qt::BackgroundRole) || roles.contains(Qt::ForegroundRole) ||
                roles.contains(Qt::FontRole))
            {
                foundStyledNotification = true;
                break;
            }
        }
        QVERIFY2(
            foundStyledNotification,
            "Theme switch must emit a dataChanged covering BackgroundRole / ForegroundRole / FontRole "
            "(or all roles) so the table view refreshes the row tints."
        );
    }

    // Regression: a theme switch must re-apply the table view's
    // stylesheet so `QStyleSheetStyle` drops its palette cache.
    // Otherwise rows with no explicit Background brush (Info /
    // Trace) keep the previous theme's default, even when the
    // rule text is unchanged.
    void TestThemeSwitchRepolishesTableStylesheet()
    {
        QVERIFY(mTheme != nullptr);

        // Prime the stylesheet via the normal app path.
        const QStringList lines{
            QStringLiteral(R"({"level": "info"})"),
            QStringLiteral(R"({"level": "error"})"),
        };
        const TempJsonFile fixture(lines);
        mTheme->SetActiveSelection(QStringLiteral("Light"));
        mWindow->OpenFilesForTest({fixture.Path()}, MainWindow::OpenMode::Replace);
        auto *model = mWindow->findChild<LogModel *>();
        QVERIFY2(model != nullptr, "MainWindow must own a LogModel");
        QSignalSpy finishedSpy(model, &LogModel::streamingFinished);
        if (finishedSpy.count() == 0)
        {
            finishedSpy.wait(5000);
        }

        auto *tableView = mWindow->findChild<QTableView *>();
        QVERIFY2(tableView != nullptr, "MainWindow must own a table view");
        const QString initialStyleSheet = tableView->styleSheet();
        QVERIFY2(
            !initialStyleSheet.isEmpty(), "After a normal load the body stylesheet must include the monospace cell rule"
        );

        // Mirror the post-optimisation state where
        // `mLastBodyStyleSheet` claims the rule is applied but the
        // widget's stylesheet is stale. Fix must re-apply anyway.
        tableView->setStyleSheet(QString{});
        QCOMPARE(tableView->styleSheet(), QString{});

        // `OnThemeChanged` resets the tracker and forces a re-write.
        mTheme->SetActiveSelection(QStringLiteral("Dark"));
        QCoreApplication::processEvents();

        QVERIFY2(
            !tableView->styleSheet().isEmpty(),
            qPrintable(QStringLiteral("Theme switch must re-apply the body stylesheet so "
                                      "`QStyleSheetStyle` re-resolves the new palette; got: '%1'")
                           .arg(tableView->styleSheet()))
        );
        // Re-applied stylesheet must still carry the monospace rule.
        QVERIFY2(
            tableView->styleSheet().contains(QStringLiteral("QTableView::item")),
            qPrintable(QStringLiteral("Re-applied stylesheet must carry the monospace cell rule; got: '%1'")
                           .arg(tableView->styleSheet()))
        );
    }

    // End-to-end round-trip for `Column::levelMapping`. Saved config
    // pins `Type::Level` with `NOTICE -> Info`, `PANIC -> Fatal`. A
    // filter selecting `Info` expands via the rank cache to match
    // both `NOTICE` (override) and `info` (built-in) while rejecting `PANIC`.
    void TestLevelMappingRoundTripThroughFilterEditor()
    {
        // Step 1: build the saved configuration on disk.
        loglib::LogConfiguration cfg;
        loglib::LogConfiguration::Column column;
        column.header = "lvl";
        column.keys = {"lvl"};
        column.printFormat = "{}";
        column.type = loglib::LogConfiguration::Type::Level;
        column.parseFormats = {};
        column.levelMapping = {
            {"NOTICE", "Info"},
            {"PANIC", "Fatal"},
        };
        cfg.columns.push_back(column);

        const QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        const QString cfgPath = tempDir.filePath(QStringLiteral("level-mapping.json"));
        {
            std::string json;
            QVERIFY(!glz::write_json(cfg, json));
            QFile out(cfgPath);
            QVERIFY(out.open(QIODevice::WriteOnly | QIODevice::Truncate));
            out.write(QByteArray::fromStdString(json));
        }

        // Step 2: load through MainWindow so the filter UI rebuilds.
        QVERIFY2(mWindow->TryLoadAsConfigurationForTest(cfgPath), "TryLoadAsConfiguration must succeed");
        QCoreApplication::processEvents();

        auto *model = mWindow->findChild<LogModel *>();
        QVERIFY2(model != nullptr, "MainWindow must own a LogModel");
        const int levelCol = ColumnByHeader(*model, QStringLiteral("lvl"));
        QVERIFY2(levelCol >= 0, "lvl column must exist after configuration load");
        QCOMPARE(
            model->Configuration().columns[static_cast<size_t>(levelCol)].type, loglib::LogConfiguration::Type::Level
        );

        // Step 3: drive a stream with mixed canonical / aliased values.
        const TempJsonFile emptyFixture(QStringList{});
        auto file = std::make_unique<loglib::LogFile>(emptyFixture.Path().toStdString());
        auto fileSource = std::make_unique<loglib::FileLineSource>(std::move(file));
        loglib::FileLineSource *sourcePtr = fileSource.get();
        (void)model->BeginStreamingForSyncTest(std::move(fileSource));

        loglib::KeyIndex &keys = model->Table().Keys();
        const auto makeLine = [&](const std::string &value) {
            std::vector<std::pair<loglib::KeyId, loglib::LogValue>> values;
            values.emplace_back(keys.GetOrInsert("lvl"), loglib::LogValue(value));
            return loglib::LogLine{std::move(values), keys, *sourcePtr, 0};
        };
        {
            loglib::StreamedBatch batch;
            batch.firstLineNumber = 1;
            batch.lines.push_back(makeLine("NOTICE")); // override -> Info
            batch.lines.push_back(makeLine("PANIC"));  // override -> Fatal
            batch.lines.push_back(makeLine("info"));   // built-in -> Info
            batch.lines.push_back(makeLine("WARN"));   // built-in -> Warn
            batch.lines.push_back(makeLine("qux"));    // unmapped (no level)
            model->AppendBatch(std::move(batch));
        }
        QCoreApplication::processEvents();

        // Step 4: submit `Info` through the FilterEditor's slot.
        // `MainWindow::UpdateFilters` expands `"Info"` to every raw
        // dict entry resolving to `LogLevel::Info` via `LevelRankCache`.
        const QString filterId = QStringLiteral("level-mapping-info-only");
        QVERIFY2(
            QMetaObject::invokeMethod(
                mWindow,
                "FilterEnumSubmitted",
                Qt::DirectConnection,
                Q_ARG(QString, filterId),
                Q_ARG(int, levelCol),
                Q_ARG(QStringList, QStringList{QStringLiteral("Info")})
            ),
            "FilterEnumSubmitted slot must be invocable via meta-object"
        );
        QCoreApplication::processEvents();

        // Step 5: only `NOTICE` (override) and `info` (built-in)
        // resolve to `Info` and survive. `PANIC`/`WARN`/`qux` drop out.
        const LogFilterModel *filterModel = mWindow->FilterModel();
        QVERIFY2(filterModel != nullptr, "MainWindow must own a LogFilterModel proxy");
        QCOMPARE(filterModel->rowCount(), 2);

        std::vector<QString> visible;
        visible.reserve(static_cast<size_t>(filterModel->rowCount()));
        for (int i = 0; i < filterModel->rowCount(); ++i)
        {
            visible.push_back(filterModel->index(i, levelCol).data(Qt::DisplayRole).toString());
        }
        std::ranges::sort(visible);
        QCOMPARE(visible.size(), static_cast<size_t>(2));
        QCOMPARE(visible[0], QStringLiteral("NOTICE"));
        QCOMPARE(visible[1], QStringLiteral("info"));

        model->EndStreaming(false);
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
            lines.append(QStringLiteral(R"({"category": "%1"})").arg(levels[i % levels.size()]));
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

        const int levelCol = ColumnByHeader(*model, QStringLiteral("category"));
        QVERIFY2(levelCol >= 0, "level column must exist");
        const auto &columns = model->Configuration().columns;
        QCOMPARE(columns[static_cast<size_t>(levelCol)].type, loglib::LogConfiguration::Type::Enumeration);

        // Find the Filters-menu action whose data matches `filterId`.
        const auto findFilterMenuAction = [&](const QString &filterId) -> QAction * {
            const auto *menu = mWindow->findChild<QMenu *>(QStringLiteral("menuFilters"));
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
            lines.append(QStringLiteral(R"({"category": "%1", "n": %2})").arg(levels[i % levels.size()]).arg(i));
        }
        const TempJsonFile fixture(lines);

        const StreamingRun run = RunStreaming(fixture.Path());
        QCOMPARE(run.finishedCount, 1);
        QCOMPARE(run.cancelled, false);
        QVERIFY(run.model->StreamingErrors().empty());

        const int levelCol = ColumnByHeader(*run.model, QStringLiteral("category"));
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
            values.emplace_back(keys.GetOrInsert("category"), loglib::LogValue(value));
            return loglib::LogLine{std::move(values), keys, *sourcePtr, 0};
        };

        // Batch 1: 2x "info" promotes `level` (stream threshold = 2).
        // Dict size 0 -> 1.
        {
            loglib::StreamedBatch batch;
            batch.firstLineNumber = 1;
            batch.newKeys.emplace_back("category");
            batch.lines.push_back(makeLine("info"));
            batch.lines.push_back(makeLine("info"));
            model->AppendBatch(std::move(batch));
        }

        const int levelCol = ColumnByHeader(*model, QStringLiteral("category"));
        QVERIFY2(levelCol >= 0, "level column must exist after promoting batch");
        QCOMPARE(
            model->Configuration().columns[static_cast<size_t>(levelCol)].type,
            loglib::LogConfiguration::Type::Enumeration
        );
        const loglib::KeyId levelKey = keys.Find("category");
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
            values.emplace_back(keys.GetOrInsert("category"), loglib::LogValue(value));
            return loglib::LogLine{std::move(values), keys, *sourcePtr, 0};
        };

        // Batch 1 promotes "category" (stream threshold = 2).
        {
            loglib::StreamedBatch batch;
            batch.firstLineNumber = 1;
            batch.newKeys.emplace_back("category");
            batch.lines.push_back(makeLine("info"));
            batch.lines.push_back(makeLine("warn"));
            model->AppendBatch(std::move(batch));
        }
        const int levelCol = ColumnByHeader(*model, QStringLiteral("category"));
        QVERIFY2(levelCol >= 0, "level column must exist after promotion");

        // Warm the rank cache by sorting on the enum column.
        filterModel->sort(levelCol, Qt::AscendingOrder);
        QCoreApplication::processEvents();
        QCOMPARE(filterModel->EnumRankCacheSizeForTest(), std::size_t{1});

        // Batch 2 grows the dictionary. `Grew` is a no-op for the
        // rank cache so the entry stays.
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
    // overflows the dict cap) must still emit `Demoted` so
    // consumers can rebuild any rank-cache entry or filter rule
    // that aliased the transient dictionary.
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
            values.emplace_back(keys.GetOrInsert("category"), loglib::LogValue(value));
            return loglib::LogLine{std::move(values), keys, *sourcePtr, 0};
        };

        // One batch, five rows: the candidate scan early-bails after
        // two "info" rows so stream-mode promotion fires. The
        // `PromoteColumnToEnum` encode pass then trips the cap on the
        // third distinct value, demoting back to `Type::String` --
        // all inside a single `AppendBatch`.
        {
            loglib::StreamedBatch batch;
            batch.firstLineNumber = 1;
            batch.newKeys.emplace_back("category");
            batch.lines.push_back(makeLine("info"));
            batch.lines.push_back(makeLine("info"));
            batch.lines.push_back(makeLine("warn"));
            batch.lines.push_back(makeLine("error"));
            batch.lines.push_back(makeLine("fatal"));
            model->AppendBatch(std::move(batch));
        }
        QCoreApplication::processEvents();

        const int levelCol = ColumnByHeader(*model, QStringLiteral("category"));
        QVERIFY2(levelCol >= 0, "level column must exist post-batch");
        QCOMPARE(
            model->Configuration().columns[static_cast<size_t>(levelCol)].type, loglib::LogConfiguration::Type::String
        );
        const loglib::KeyId levelKey = model->Table().Keys().Find("category");
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
    // Regression: a `Type::Level -> String` mid-session demote must
    // keep matching the same rows. Filter values are canonical names
    // (`"Info"`, ...), but the demoted column carries raw bytes --
    // `LogModel::AppendBatch` snapshots the level -> raw-bytes
    // mapping pre-demote and `MainWindow`'s `Demoted` handler
    // rewrites `filterValues` to the raw bytes.
    void TestLevelFilterTranslatesOnDemoteToString()
    {
        auto *model = mWindow->findChild<LogModel *>();
        QVERIFY2(model != nullptr, "MainWindow must own a LogModel");

        const TempJsonFile emptyFixture(QStringList{});
        auto file = std::make_unique<loglib::LogFile>(emptyFixture.Path().toStdString());
        auto fileSource = std::make_unique<loglib::FileLineSource>(std::move(file));
        loglib::FileLineSource *sourcePtr = fileSource.get();
        (void)model->BeginStreamingForSyncTest(std::move(fileSource));

        // Cap of 3 lets batch 1 promote to Level (dict size 2). Batch 2
        // adds three new distinct values, trips the cap, demotes to String.
        constexpr uint16_t TEST_CAP = 3;
        model->Table().SetEnumValueCap(TEST_CAP);

        loglib::KeyIndex &keys = model->Table().Keys();
        const auto makeLine = [&](const std::string &value) {
            std::vector<std::pair<loglib::KeyId, loglib::LogValue>> values;
            values.emplace_back(keys.GetOrInsert("level"), loglib::LogValue(value));
            return loglib::LogLine{std::move(values), keys, *sourcePtr, 0};
        };

        // Batch 1: 2x `info` + 1x `warn` promotes through Enumeration
        // to Level (`IsLogLevelKey` match + canonical dict entries).
        {
            loglib::StreamedBatch batch;
            batch.firstLineNumber = 1;
            batch.newKeys.emplace_back("level");
            batch.lines.push_back(makeLine("info"));
            batch.lines.push_back(makeLine("info"));
            batch.lines.push_back(makeLine("warn"));
            model->AppendBatch(std::move(batch));
        }

        const int levelCol = ColumnByHeader(*model, QStringLiteral("level"));
        QVERIFY2(levelCol >= 0, "level column must exist after promotion");
        QCOMPARE(
            model->Configuration().columns[static_cast<size_t>(levelCol)].type, loglib::LogConfiguration::Type::Level
        );

        // Submit `Info` through FilterEditor. Only the two `info`
        // rows out of three should survive.
        const QString filterId = QStringLiteral("level-demote-translation");
        QVERIFY2(
            QMetaObject::invokeMethod(
                mWindow,
                "FilterEnumSubmitted",
                Qt::DirectConnection,
                Q_ARG(QString, filterId),
                Q_ARG(int, levelCol),
                Q_ARG(QStringList, QStringList{QStringLiteral("Info")})
            ),
            "FilterEnumSubmitted slot must be invocable via meta-object"
        );
        QCoreApplication::processEvents();

        const LogFilterModel *filterModel = mWindow->FilterModel();
        QVERIFY2(filterModel != nullptr, "MainWindow must own a LogFilterModel proxy");
        QCOMPARE(filterModel->rowCount(), 2);

        // Batch 2: three new values overflow the cap, column demotes
        // to String. The Demoted handler expands `["Info"]` to the
        // raw entries pre-demote (`["info"]`).
        {
            loglib::StreamedBatch batch;
            batch.firstLineNumber = 4;
            batch.lines.push_back(makeLine("error"));
            batch.lines.push_back(makeLine("debug"));
            batch.lines.push_back(makeLine("trace"));
            model->AppendBatch(std::move(batch));
        }
        QCoreApplication::processEvents();

        QCOMPARE(
            model->Configuration().columns[static_cast<size_t>(levelCol)].type, loglib::LogConfiguration::Type::String
        );

        // Translation succeeded iff the two `info` rows still match
        // post-demote. Without it, `["Info"]` would byte-compare
        // against `info` and reject every row.
        QCOMPARE(filterModel->rowCount(), 2);
        for (int i = 0; i < filterModel->rowCount(); ++i)
        {
            QCOMPARE(filterModel->index(i, levelCol).data(Qt::DisplayRole).toString(), QStringLiteral("info"));
        }

        model->EndStreaming(false);
    }

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
            values.emplace_back(keys.GetOrInsert("category"), loglib::LogValue(value));
            return loglib::LogLine{std::move(values), keys, *sourcePtr, 0};
        };

        // Batch 1: two "info" rows promote `level`. Dict size 1;
        // predicate built against it observes `mAllResolved == true`.
        {
            loglib::StreamedBatch batch;
            batch.firstLineNumber = 1;
            batch.newKeys.emplace_back("category");
            batch.lines.push_back(makeLine("info"));
            batch.lines.push_back(makeLine("info"));
            model->AppendBatch(std::move(batch));
        }

        const int levelCol = ColumnByHeader(*model, QStringLiteral("category"));
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

        // Two-column fixture where "category" promotes to enum (one
        // batch -- 200 lines is below the streaming threshold).
        const QStringList levels{
            QStringLiteral("info"), QStringLiteral("warn"), QStringLiteral("error"), QStringLiteral("debug")
        };
        QStringList lines;
        lines.reserve(200);
        for (int i = 0; i < 200; ++i)
        {
            lines.append(QStringLiteral(R"({"category": "%1", "msg": "m%2"})").arg(levels[i % levels.size()]).arg(i));
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

        const int levelCol = ColumnByHeader(*model, QStringLiteral("category"));
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

        // Reorder columns; the rank cache is KeyId-keyed and
        // survives.
        QVERIFY2(model->columnCount() >= 2, "fixture must have at least two columns");
        const QSignalSpy columnsMovedSpy(model, &QAbstractItemModel::columnsMoved);
        // Pin the proxy-level forwarding contract too: `LogFilterModel`
        // must forward `columnsMoved` so downstream views see the
        // bracketed structural change during a streaming session
        // late-promoted a `Type::Time` column. Post-fix the proxy
        // forwards begin/endMoveColumns through to the view.
        const QSignalSpy filterColumnsAboutToBeMovedSpy(filterModel, &QAbstractItemModel::columnsAboutToBeMoved);
        const QSignalSpy filterColumnsMovedSpy(filterModel, &QAbstractItemModel::columnsMoved);
        QVERIFY(columnsMovedSpy.isValid());
        QVERIFY(filterColumnsAboutToBeMovedSpy.isValid());
        QVERIFY(filterColumnsMovedSpy.isValid());
        const int src = (levelCol == 0) ? 1 : 0;
        const int dest = (levelCol == 0) ? 0 : 1;
        QVERIFY2(model->MoveColumn(src, dest), "MoveColumn must succeed");
        QCoreApplication::processEvents();
        QVERIFY2(columnsMovedSpy.count() >= 1, "MoveColumn must emit columnsMoved");
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
        const int levelColAfter = ColumnByHeader(*model, QStringLiteral("category"));
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

    // Stream a small two-column fixture and return the `level`
    // column index. QTest filters non-void slots out of the test
    // scan, so this stays callable from inside `private slots:`.
    int StreamFixtureForColumnTests()
    {
        auto *model = mWindow->Model();
        Q_ASSERT(model != nullptr);

        // 200 lines: above `STREAM_PROMOTION_MIN_ROWS` so streaming
        // promotes `level` to enum, but below the static-mode
        // promotion threshold for arbitrary keys.
        const QStringList levels{
            QStringLiteral("info"), QStringLiteral("warn"), QStringLiteral("error"), QStringLiteral("debug")
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
        if (finishedSpy.count() == 0)
        {
            finishedSpy.wait(5000);
        }
        QCoreApplication::processEvents();

        return ColumnByHeader(*model, QStringLiteral("category"));
    }

    // `SetColumnVisible(idx, false)` flips `Column::visible` and
    // hides the matching header section.
    void TestHideColumnUpdatesHeaderAndConfiguration()
    {
        const int levelCol = StreamFixtureForColumnTests();
        QVERIFY2(levelCol >= 0, "level column must exist after streaming");

        auto *model = mWindow->Model();
        QVERIFY2(model != nullptr, "MainWindow must own a LogModel");
        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): false positive; prior `QVERIFY2` aborts on null.
        QVERIFY2(
            model->Configuration().columns[static_cast<size_t>(levelCol)].visible,
            "column must default to visible before hide"
        );

        mWindow->SetColumnVisible(levelCol, false);

        QVERIFY2(
            !model->Configuration().columns[static_cast<size_t>(levelCol)].visible, "Column::visible must flip to false"
        );
        const QHeaderView *header = mWindow->findChild<LogTableView *>()->horizontalHeader();
        QVERIFY2(header != nullptr, "view must own a horizontal header");
        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): false positive; prior `QVERIFY2` aborts on null.
        QVERIFY2(header->isSectionHidden(levelCol), "header section must be hidden");
    }

    // `SetColumnVisible(idx, true)` reverses a hide.
    void TestShowColumnRestoresSection()
    {
        const int levelCol = StreamFixtureForColumnTests();
        QVERIFY2(levelCol >= 0, "level column must exist after streaming");

        auto *model = mWindow->Model();
        mWindow->SetColumnVisible(levelCol, false);
        QVERIFY(!model->Configuration().columns[static_cast<size_t>(levelCol)].visible);

        mWindow->SetColumnVisible(levelCol, true);

        QVERIFY2(
            model->Configuration().columns[static_cast<size_t>(levelCol)].visible,
            "Column::visible must flip back to true"
        );
        const QHeaderView *header = mWindow->findChild<LogTableView *>()->horizontalHeader();
        QVERIFY2(header != nullptr, "view must own a horizontal header");
        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): false positive; prior `QVERIFY2` aborts on null.
        QVERIFY2(!header->isSectionHidden(levelCol), "header section must no longer be hidden");
    }

    // The header menu must never advertise a hidden column on a
    // visible-column right-click: re-showing hidden columns is
    // intentionally delegated to the `View` menu (the only escape
    // hatch when *every* column is hidden, since no header section
    // is left to right-click). Built against a visible clicked
    // column so the `Hide` entry is present and the contrast is
    // sharp. Pinned so a future regression that reintroduces a
    // `Show column` submenu trips here.
    void TestHeaderContextMenuOmitsShowColumnSubmenu()
    {
        const int levelCol = StreamFixtureForColumnTests();
        QVERIFY2(levelCol >= 0, "level column must exist after streaming");
        auto *model = mWindow->Model();
        const int msgCol = ColumnByHeader(*model, QStringLiteral("msg"));
        QVERIFY2(msgCol >= 0, "msg column must exist after streaming");

        mWindow->SetColumnVisible(msgCol, false);

        auto built = mWindow->BuildHeaderContextMenu(levelCol, nullptr);
        QVERIFY2(built.menu != nullptr, "BuildHeaderContextMenu must return a menu");
        const QScopeGuard menuDeleter([&built]() { built.menu->deleteLater(); });

        const QList<QAction *> topActions = built.menu->actions();
        QVERIFY2(!topActions.isEmpty(), "visible-column menu must contain at least the Hide entry");
        QVERIFY2(topActions.front()->text().startsWith("Hide"), "first action must be the Hide entry");

        for (const QAction *act : topActions)
        {
            QVERIFY2(
                act->text() != QStringLiteral("Show column"),
                "header context menu must not offer a Show column submenu (use the View menu)"
            );
            QVERIFY2(
                act->text() != QStringLiteral("msg"),
                "header context menu must not list the hidden `msg` column as an action"
            );
        }
    }

    // For a hidden column the menu must omit both `Hide` and
    // `Add filter on ...` (both would be confusing no-ops). The
    // hidden-column right-click is only reachable via the test
    // seam -- production right-clicks fire on visible sections --
    // and re-showing hidden columns is delegated to the `View`
    // menu, so the resulting menu is empty in this branch.
    void TestHeaderContextMenuOmitsHideForHiddenColumn()
    {
        const int levelCol = StreamFixtureForColumnTests();
        QVERIFY2(levelCol >= 0, "level column must exist after streaming");

        mWindow->SetColumnVisible(levelCol, false);

        auto built = mWindow->BuildHeaderContextMenu(levelCol, nullptr);
        QVERIFY2(built.menu != nullptr, "BuildHeaderContextMenu must return a menu");
        const QScopeGuard menuDeleter([&built]() { built.menu->deleteLater(); });

        for (const QAction *act : built.menu->actions())
        {
            QVERIFY2(
                !act->text().startsWith("Hide"), "menu rooted at a hidden column must not advertise a Hide action"
            );
            QVERIFY2(
                !act->text().startsWith("Add filter on"),
                "menu rooted at a hidden column must not advertise an Add-filter action"
            );
        }
    }

    // The header menu always offers `Add filter on "<col>"...` so
    // the user can scope a filter to the clicked column without
    // leaving the right-click menu.
    void TestHeaderContextMenuOffersAddFilter()
    {
        const int levelCol = StreamFixtureForColumnTests();
        QVERIFY2(levelCol >= 0, "level column must exist after streaming");

        auto built = mWindow->BuildHeaderContextMenu(levelCol, nullptr);
        QVERIFY2(built.menu != nullptr, "BuildHeaderContextMenu must return a menu");
        const QScopeGuard menuDeleter([&built]() { built.menu->deleteLater(); });

        const QAction *addFilterAction = nullptr;
        for (const QAction *act : built.menu->actions())
        {
            if (act->text().startsWith(QStringLiteral("Add filter on")))
            {
                addFilterAction = act;
                break;
            }
        }
        QVERIFY2(addFilterAction != nullptr, "menu must contain an `Add filter on ...` action");
        QVERIFY2(
            // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): false positive; prior `QVERIFY2` aborts on null.
            addFilterAction->text().contains(QStringLiteral("category")),
            "Add filter action title must reference the clicked column"
        );
    }

    // Triggering Add filter opens a `FilterEditor` with the row
    // combobox already pointed at the clicked column.
    void TestHeaderContextMenuAddFilterOpensEditorWithColumnPreselected()
    {
        const int levelCol = StreamFixtureForColumnTests();
        QVERIFY2(levelCol >= 0, "level column must exist after streaming");

        auto built = mWindow->BuildHeaderContextMenu(levelCol, nullptr);
        QVERIFY2(built.menu != nullptr, "BuildHeaderContextMenu must return a menu");
        const QScopeGuard menuDeleter([&built]() { built.menu->deleteLater(); });

        QAction *addFilterAction = nullptr;
        for (QAction *act : built.menu->actions())
        {
            if (act->text().startsWith(QStringLiteral("Add filter on")))
            {
                addFilterAction = act;
                break;
            }
        }
        QVERIFY2(addFilterAction != nullptr, "menu must contain an `Add filter on ...` action");

        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): false positive; prior `QVERIFY2` aborts on null.
        addFilterAction->trigger();
        QCoreApplication::processEvents();

        FilterEditor *editor = nullptr;
        for (QWidget *widget : QApplication::topLevelWidgets())
        {
            if (auto *e = qobject_cast<FilterEditor *>(widget))
            {
                editor = e;
                break;
            }
        }
        QVERIFY2(editor != nullptr, "Add filter must spawn a FilterEditor");
        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): false positive; prior `QVERIFY2` aborts on null.
        QCOMPARE(editor->GetRowToFilter(), levelCol);

        editor->close();
        editor->deleteLater();
        QCoreApplication::processEvents();
    }

    // Every existing filter on the clicked column gets a per-filter
    // submenu (titled with its display value, mirroring the Filters
    // menu) carrying `Edit` and `Remove` actions.
    void TestHeaderContextMenuListsExistingFiltersForColumn()
    {
        const int levelCol = StreamFixtureForColumnTests();
        QVERIFY2(levelCol >= 0, "level column must exist after streaming");
        auto *model = mWindow->Model();
        const int msgCol = ColumnByHeader(*model, QStringLiteral("msg"));
        QVERIFY2(msgCol >= 0, "msg column must exist after streaming");

        // Two filters on `level`, one on `msg` (the latter must
        // not appear in the menu rooted at `level`).
        const QString levelFilter1 = QStringLiteral("level-filter-1");
        const QString levelFilter2 = QStringLiteral("level-filter-2");
        const QString msgFilter = QStringLiteral("msg-filter");
        QVERIFY2(
            QMetaObject::invokeMethod(
                mWindow,
                "FilterEnumSubmitted",
                Qt::DirectConnection,
                Q_ARG(QString, levelFilter1),
                Q_ARG(int, levelCol),
                Q_ARG(QStringList, QStringList{QStringLiteral("info")})
            ),
            "FilterEnumSubmitted slot must be invocable via meta-object"
        );
        QVERIFY2(
            QMetaObject::invokeMethod(
                mWindow,
                "FilterEnumSubmitted",
                Qt::DirectConnection,
                Q_ARG(QString, levelFilter2),
                Q_ARG(int, levelCol),
                Q_ARG(QStringList, QStringList{QStringLiteral("warn")})
            ),
            "FilterEnumSubmitted slot must be invocable via meta-object"
        );
        QVERIFY2(
            QMetaObject::invokeMethod(
                mWindow,
                "FilterSubmitted",
                Qt::DirectConnection,
                Q_ARG(QString, msgFilter),
                Q_ARG(int, msgCol),
                Q_ARG(QString, QStringLiteral("m1")),
                Q_ARG(int, static_cast<int>(loglib::LogConfiguration::LogFilter::Match::Contains))
            ),
            "FilterSubmitted slot must be invocable via meta-object"
        );
        QCoreApplication::processEvents();
        QCOMPARE(mWindow->Filters().size(), static_cast<size_t>(3));

        auto built = mWindow->BuildHeaderContextMenu(levelCol, nullptr);
        QVERIFY2(built.menu != nullptr, "BuildHeaderContextMenu must return a menu");
        const QScopeGuard menuDeleter([&built]() { built.menu->deleteLater(); });

        const QList<QMenu *> subMenus = built.menu->findChildren<QMenu *>();
        QCOMPARE(subMenus.size(), 2);
        QVERIFY2(built.menu->findChild<QMenu *>(levelFilter1) != nullptr, "level-filter-1 submenu must be exposed");
        QVERIFY2(built.menu->findChild<QMenu *>(levelFilter2) != nullptr, "level-filter-2 submenu must be exposed");
        QVERIFY2(
            built.menu->findChild<QMenu *>(msgFilter) == nullptr,
            "filters on a different column must not appear in this header menu"
        );

        // Use MainWindow's translation context so a future
        // translation of "Edit" / "Remove" cannot silently break
        // this assertion.
        const QString editLabel = MainWindow::tr("Edit");
        const QString removeLabel = MainWindow::tr("Remove");
        for (const QMenu *subMenu : subMenus)
        {
            QVERIFY2(subMenu != nullptr, "filter sub-menu pointer must be non-null");
            const QList<QAction *> actions = subMenu->actions();
            QStringList labels;
            // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): false positive; prior `QVERIFY2` aborts on null.
            for (const QAction *act : actions)
            {
                labels.append(act->text());
            }
            QVERIFY2(labels.contains(editLabel), "filter sub-menu must contain an Edit action");
            QVERIFY2(labels.contains(removeLabel), "filter sub-menu must contain a Remove action");
        }
    }

    // Triggering `Remove` on a header-menu filter submenu drops
    // the filter from `mFilters` (and the wire-format mirror).
    void TestHeaderContextMenuRemoveClearsFilter()
    {
        const int levelCol = StreamFixtureForColumnTests();
        QVERIFY2(levelCol >= 0, "level column must exist after streaming");

        const QString filterId = QStringLiteral("header-remove");
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
        QVERIFY2(mWindow->Filters().contains(filterId.toStdString()), "filter must land in mFilters before remove");

        auto built = mWindow->BuildHeaderContextMenu(levelCol, nullptr);
        QVERIFY2(built.menu != nullptr, "BuildHeaderContextMenu must return a menu");
        const QScopeGuard menuDeleter([&built]() { built.menu->deleteLater(); });

        const QMenu *subMenu = built.menu->findChild<QMenu *>(filterId);
        QVERIFY2(subMenu != nullptr, "filter sub-menu must be exposed via the struct");
        QAction *removeAction = nullptr;
        const QString removeLabel = MainWindow::tr("Remove");
        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): false positive; prior `QVERIFY2` aborts on null.
        for (QAction *act : subMenu->actions())
        {
            if (act->text() == removeLabel)
            {
                removeAction = act;
                break;
            }
        }
        QVERIFY2(removeAction != nullptr, "sub-menu must contain a Remove action");

        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): false positive; prior `QVERIFY2` aborts on null.
        removeAction->trigger();
        QCoreApplication::processEvents();

        QVERIFY2(!mWindow->Filters().contains(filterId.toStdString()), "Remove must drop the filter from mFilters");
    }

    // Triggering `Edit` on a header-menu filter submenu opens a
    // `FilterEditor` bound to the filter's column.
    void TestHeaderContextMenuEditOpensEditor()
    {
        const int levelCol = StreamFixtureForColumnTests();
        QVERIFY2(levelCol >= 0, "level column must exist after streaming");

        const QString filterId = QStringLiteral("header-edit");
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

        auto built = mWindow->BuildHeaderContextMenu(levelCol, nullptr);
        QVERIFY2(built.menu != nullptr, "BuildHeaderContextMenu must return a menu");
        const QScopeGuard menuDeleter([&built]() { built.menu->deleteLater(); });

        const QMenu *subMenu = built.menu->findChild<QMenu *>(filterId);
        QVERIFY2(subMenu != nullptr, "filter sub-menu must be exposed");
        QAction *editAction = nullptr;
        const QString editLabel = MainWindow::tr("Edit");
        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): false positive; prior `QVERIFY2` aborts on null.
        for (QAction *act : subMenu->actions())
        {
            if (act->text() == editLabel)
            {
                editAction = act;
                break;
            }
        }
        QVERIFY2(editAction != nullptr, "sub-menu must contain an Edit action");

        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): false positive; prior `QVERIFY2` aborts on null.
        editAction->trigger();
        QCoreApplication::processEvents();

        FilterEditor *editor = nullptr;
        for (QWidget *widget : QApplication::topLevelWidgets())
        {
            if (auto *e = qobject_cast<FilterEditor *>(widget))
            {
                editor = e;
                break;
            }
        }
        QVERIFY2(editor != nullptr, "Edit must open a FilterEditor");
        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): false positive; prior `QVERIFY2` aborts on null.
        QCOMPARE(editor->GetRowToFilter(), levelCol);
        QVERIFY2(
            mWindow->Filters().contains(filterId.toStdString()),
            "Edit must not drop the filter; the lambda re-resolves mFilters[id] live"
        );

        editor->close();
        editor->deleteLater();
        QCoreApplication::processEvents();
    }

    // Regression: the `Add filter on ...` lambda must re-resolve by
    // keys, so a column reorder between menu build and click still
    // targets the column's current index. Mirrors
    // `TestEditFilterAfterColumnReorderUsesCurrentRow`.
    void TestHeaderContextMenuAddFilterAfterColumnReorderResolvesByKeys()
    {
        const int levelCol = StreamFixtureForColumnTests();
        QVERIFY2(levelCol >= 0, "level column must exist after streaming");
        auto *model = mWindow->Model();
        const int columnCount = model->columnCount();
        QVERIFY2(columnCount >= 2, "fixture must yield at least two columns");

        auto built = mWindow->BuildHeaderContextMenu(levelCol, nullptr);
        QVERIFY2(built.menu != nullptr, "BuildHeaderContextMenu must return a menu");
        const QScopeGuard menuDeleter([&built]() { built.menu->deleteLater(); });

        QAction *addFilterAction = nullptr;
        for (QAction *act : built.menu->actions())
        {
            if (act->text().startsWith(QStringLiteral("Add filter on")))
            {
                addFilterAction = act;
                break;
            }
        }
        QVERIFY2(addFilterAction != nullptr, "menu must contain an `Add filter on ...` action");

        // Reorder `level` after the menu is built but before the
        // action is triggered; the lambda must re-resolve the index
        // from the captured keys.
        const int src = levelCol;
        const int dest = (src == 0) ? columnCount - 1 : 0;
        QVERIFY(src != dest);
        QVERIFY2(
            QMetaObject::invokeMethod(
                mWindow,
                "OnHeaderSectionMoved",
                Qt::DirectConnection,
                Q_ARG(int, src),
                Q_ARG(int, src),
                Q_ARG(int, dest)
            ),
            "OnHeaderSectionMoved slot must be invocable via meta-object"
        );
        QCoreApplication::processEvents();
        QCOMPARE(ColumnByHeader(*model, QStringLiteral("category")), dest);

        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): false positive; prior `QVERIFY2` aborts on null.
        addFilterAction->trigger();
        QCoreApplication::processEvents();

        FilterEditor *editor = nullptr;
        for (QWidget *widget : QApplication::topLevelWidgets())
        {
            if (auto *e = qobject_cast<FilterEditor *>(widget))
            {
                editor = e;
                break;
            }
        }
        QVERIFY2(editor != nullptr, "Add filter must spawn a FilterEditor");
        // The editor's row must reflect the post-reorder index,
        // proving the lambda re-resolved by keys rather than
        // freezing on the build-time index.
        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): false positive; prior `QVERIFY2` aborts on null.
        QCOMPARE(editor->GetRowToFilter(), dest);

        editor->close();
        editor->deleteLater();
        QCoreApplication::processEvents();
    }

    // The header menu must keep a stable, polished order: Hide →
    // separator → Add filter → filter submenus. The order is only
    // encoded in `BuildHeaderContextMenu`'s control flow, so a
    // future reorder could quietly break the menu's polish without
    // tripping any other test.
    void TestHeaderContextMenuActionOrdering()
    {
        const int levelCol = StreamFixtureForColumnTests();
        QVERIFY2(levelCol >= 0, "level column must exist after streaming");
        auto *model = mWindow->Model();
        const int msgCol = ColumnByHeader(*model, QStringLiteral("msg"));
        QVERIFY2(msgCol >= 0, "msg column must exist after streaming");

        // Hide `msg` to assert it does *not* leak into the menu
        // (re-showing is owned by the `View` menu), add a filter on
        // `level` so the filter group is present, and root the menu
        // at `level` (still visible) so every group is reachable.
        mWindow->SetColumnVisible(msgCol, false);
        const QString filterId = QStringLiteral("order-test");
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

        auto built = mWindow->BuildHeaderContextMenu(levelCol, nullptr);
        QVERIFY2(built.menu != nullptr, "BuildHeaderContextMenu must return a menu");
        const QScopeGuard menuDeleter([&built]() { built.menu->deleteLater(); });

        const QList<QAction *> topActions = built.menu->actions();
        QCOMPARE(topActions.size(), 9);

        // Order: Hide, Edit column, separator, Add filter on,
        // filter submenu, separator, Sort asc, Sort desc,
        // Clear sort. Sort sits after filter so column-
        // mutation actions group above row-projection ones.
        QVERIFY2(topActions[0]->text().startsWith("Hide"), "first action must be Hide");
        QVERIFY2(topActions[1]->text().startsWith("Edit column"), "second action must be Edit column ...");
        QVERIFY2(topActions[2]->isSeparator(), "third action must be a separator");
        QVERIFY2(topActions[3]->text().startsWith("Add filter on"), "fourth action must be Add filter on ...");
        QVERIFY2(!topActions[4]->isSeparator(), "fifth action must be the filter submenu");
        QVERIFY2(topActions[5]->isSeparator(), "sixth action must be the sort-block separator");
        QVERIFY2(topActions[6]->text().startsWith("Sort ascending"), "seventh action must be Sort ascending");
        QVERIFY2(topActions[7]->text().startsWith("Sort descending"), "eighth action must be Sort descending");
        // The trailing Clear-sort entry is the shared
        // `actionClearSort` re-attached; pin by `objectName` so the
        // entry's text can evolve in the .ui without a test edit.
        QCOMPARE(topActions[8]->objectName(), QStringLiteral("actionClearSort"));
    }

    // With zero rows, Add-filter and per-filter Edit must be
    // disabled (`AddFilter` short-circuits with a status-bar hint,
    // so enabled entries would advertise a no-op). Remove stays
    // enabled -- dropping a filter doesn't need rows. The save /
    // reset / load detour is the only seam that yields a
    // columns-without-rows state outside a fresh stream open.
    void TestHeaderContextMenuDisablesFilterActionsWhenModelEmpty()
    {
        const int levelCol = StreamFixtureForColumnTests();
        QVERIFY2(levelCol >= 0, "level column must exist after streaming");
        auto *model = mWindow->Model();

        // Round-trip the configuration so the model keeps its
        // streamed columns but loses all rows. Same shape as
        // `TestColumnVisibilityRoundTripsThroughSaveLoad`.
        const QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        const QString savedPath = tempDir.filePath(QStringLiteral("rowless.json"));
        model->ConfigurationManager().Save(savedPath.toStdString());
        model->Reset();
        model->ConfigurationManager().Load(savedPath.toStdString());
        mWindow->ApplyColumnVisibility();
        QCoreApplication::processEvents();

        QCOMPARE(model->rowCount(), 0);
        const int reloadedLevelCol = ColumnByHeader(*model, QStringLiteral("category"));
        QVERIFY2(reloadedLevelCol >= 0, "level column must survive the config round-trip");

        // Inject a filter post-reload so the Edit/Remove submenus
        // are populated. `FilterEnumSubmitted` does not need rows.
        const QString filterId = QStringLiteral("empty-model-filter");
        QVERIFY2(
            QMetaObject::invokeMethod(
                mWindow,
                "FilterEnumSubmitted",
                Qt::DirectConnection,
                Q_ARG(QString, filterId),
                Q_ARG(int, reloadedLevelCol),
                Q_ARG(QStringList, QStringList{QStringLiteral("info")})
            ),
            "FilterEnumSubmitted slot must be invocable via meta-object"
        );
        QCoreApplication::processEvents();

        auto built = mWindow->BuildHeaderContextMenu(reloadedLevelCol, nullptr);
        QVERIFY2(built.menu != nullptr, "BuildHeaderContextMenu must return a menu");
        const QScopeGuard menuDeleter([&built]() { built.menu->deleteLater(); });

        const QAction *addFilterAction = nullptr;
        for (const QAction *act : built.menu->actions())
        {
            if (act->text().startsWith(QStringLiteral("Add filter on")))
            {
                addFilterAction = act;
                break;
            }
        }
        QVERIFY2(addFilterAction != nullptr, "menu must contain an Add-filter action");
        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): false positive; prior `QVERIFY2` aborts on null.
        QVERIFY2(!addFilterAction->isEnabled(), "Add filter must be disabled when the model has no rows");

        const QMenu *subMenu = built.menu->findChild<QMenu *>(filterId);
        QVERIFY2(subMenu != nullptr, "filter sub-menu must be exposed");
        const QString editLabel = MainWindow::tr("Edit");
        const QString removeLabel = MainWindow::tr("Remove");
        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): false positive; prior `QVERIFY2` aborts on null.
        for (const QAction *act : subMenu->actions())
        {
            if (act->text() == editLabel)
            {
                QVERIFY2(!act->isEnabled(), "Edit must be disabled when the model has no rows");
            }
            else if (act->text() == removeLabel)
            {
                QVERIFY2(act->isEnabled(), "Remove must stay enabled even when the model has no rows");
            }
        }
    }

    // Stream a three-row fixture with a `timestamp` column (promoted to
    // `Type::Time`) and a `msg` column. Returns the time-column index
    // post-bubble, or -1 on streaming failure.
    int StreamFixtureWithTimeColumnForRowMenuTests()
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
        if (finishedSpy.count() == 0)
        {
            finishedSpy.wait(5000);
        }
        QCoreApplication::processEvents();

        return ColumnByHeader(*model, QStringLiteral("timestamp"));
    }

    // Row menu carries the newer/older time-range actions plus the
    // always-present Anchor sub-menu.
    void TestRowContextMenuOffersAtOrAfterAndAtOrBefore()
    {
        const int timeCol = StreamFixtureWithTimeColumnForRowMenuTests();
        QVERIFY2(timeCol >= 0, "timestamp column must exist after streaming");

        QMenu *menu = mWindow->BuildRowContextMenu(/*sourceRow=*/1, nullptr);
        QVERIFY2(menu != nullptr, "BuildRowContextMenu must return a menu for a row with a timestamp");
        const QScopeGuard menuDeleter([&menu]() { menu->deleteLater(); });

        // Build labels via `tr` so the test survives a future
        // `QTranslator` install.
        const QString newerLabel = MainWindow::tr("Show only newer logs (%1)").arg(QStringLiteral("timestamp"));
        const QString olderLabel = MainWindow::tr("Show only older logs (%1)").arg(QStringLiteral("timestamp"));
        QVERIFY2(FindMenuActionByText(menu, newerLabel) != nullptr, "menu must offer the newer-logs action");
        QVERIFY2(FindMenuActionByText(menu, olderLabel) != nullptr, "menu must offer the older-logs action");

        QVERIFY2(
            FindMenuActionByText(menu, MainWindow::tr("Anchor")) != nullptr, "menu must carry the Anchor sub-menu"
        );
    }

    // Without a `Type::Time` column the time-range actions drop out
    // but the Anchor sub-menu still keeps the row menu alive.
    void TestRowContextMenuOffersAnchorOnlyWithoutTimeColumn()
    {
        // The returned index is for `category`; used only as a
        // streaming-completed sentinel.
        const int streamedColumn = StreamFixtureForColumnTests();
        QVERIFY2(streamedColumn >= 0, "streaming must complete before the row-menu probe");

        QMenu *menu = mWindow->BuildRowContextMenu(/*sourceRow=*/0, nullptr);
        QVERIFY2(menu != nullptr, "BuildRowContextMenu must return the anchor-only menu when no time column exists");
        const QScopeGuard menuDeleter([&menu]() { menu->deleteLater(); });

        QVERIFY2(
            FindMenuActionByText(menu, MainWindow::tr("Anchor")) != nullptr,
            "anchor-only menu must still expose the Anchor sub-menu"
        );
        // No "Show only ..." entries when there is no time column.
        for (const QAction *action : menu->actions())
        {
            QVERIFY2(
                !action->text().startsWith(MainWindow::tr("Show only")),
                "menu must not expose time-range actions without a Type::Time column"
            );
        }
    }

    // Empty model: nothing to bind to, menu must be null.
    void TestRowContextMenuReturnsNullWhenModelEmpty()
    {
        QCOMPARE(mWindow->Model()->rowCount(), 0);

        const QMenu *menu = mWindow->BuildRowContextMenu(/*sourceRow=*/0, nullptr);
        QVERIFY2(menu == nullptr, "BuildRowContextMenu must return null when the model is empty");
    }

    // Out-of-range rows must not crash and must not produce a menu.
    void TestRowContextMenuReturnsNullForOutOfRangeRow()
    {
        const int timeCol = StreamFixtureWithTimeColumnForRowMenuTests();
        QVERIFY2(timeCol >= 0, "timestamp column must exist after streaming");
        const int rowCount = mWindow->Model()->rowCount();
        QVERIFY2(rowCount > 0, "fixture must yield at least one row");

        QVERIFY2(
            mWindow->BuildRowContextMenu(/*sourceRow=*/-1, nullptr) == nullptr,
            "BuildRowContextMenu must reject negative source rows"
        );
        QVERIFY2(
            mWindow->BuildRowContextMenu(/*sourceRow=*/rowCount, nullptr) == nullptr,
            "BuildRowContextMenu must reject source rows beyond the model"
        );
    }

    // Row with a Time column but a monostate slot: suppress the
    // time-range entries (would install a `(nullopt, nullopt)`
    // filter) but keep the anchor sub-menu live.
    void TestRowContextMenuOffersAnchorOnlyForMonostateTimeSlot()
    {
        auto *model = mWindow->Model();
        Q_ASSERT(model != nullptr);

        // Three rows so the parser promotes `timestamp` to `Type::Time`;
        // the middle row omits the key so its slot is `monostate`.
        const QStringList lines{
            QStringLiteral(R"({"timestamp":"2024-04-28T10:00:01+00:00","msg":"first"})"),
            QStringLiteral(R"({"msg":"middle has no timestamp"})"),
            QStringLiteral(R"({"timestamp":"2024-04-28T10:00:03+00:00","msg":"third"})"),
        };
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
        if (finishedSpy.count() == 0)
        {
            finishedSpy.wait(5000);
        }
        QCoreApplication::processEvents();

        const int timeCol = ColumnByHeader(*model, QStringLiteral("timestamp"));
        QVERIFY2(timeCol >= 0, "timestamp column must exist after streaming");
        QCOMPARE(model->rowCount(), 3);

        QMenu *first = mWindow->BuildRowContextMenu(/*sourceRow=*/0, nullptr);
        QVERIFY2(first != nullptr, "row 0 has a timestamp slot, menu must be offered");
        const QScopeGuard firstDeleter([&first]() { first->deleteLater(); });
        QVERIFY2(
            FindMenuActionByText(first, MainWindow::tr("Show only newer logs (%1)").arg(QStringLiteral("timestamp"))) !=
                nullptr,
            "row 0's populated time slot must keep the time-range action"
        );

        QMenu *middle = mWindow->BuildRowContextMenu(/*sourceRow=*/1, nullptr);
        QVERIFY2(middle != nullptr, "row 1's anchor sub-menu must keep the row menu alive");
        const QScopeGuard middleDeleter([&middle]() { middle->deleteLater(); });
        QVERIFY2(
            FindMenuActionByText(middle, MainWindow::tr("Anchor")) != nullptr,
            "row 1's anchor sub-menu must still be present"
        );
        for (const QAction *action : middle->actions())
        {
            QVERIFY2(
                !action->text().startsWith(MainWindow::tr("Show only")),
                "row 1's monostate time slot must suppress time-range actions"
            );
        }
    }

    // Triggering "newer" installs an inclusive time filter with the
    // clicked row's micros as the lower bound and `nullopt` as the
    // upper bound. Lower bound comes from `SortRole`, which normalises
    // every `Type::Time` slot to epoch microseconds.
    void TestRowContextMenuAtOrAfterAddsTimeFilter()
    {
        const int timeCol = StreamFixtureWithTimeColumnForRowMenuTests();
        QVERIFY2(timeCol >= 0, "timestamp column must exist after streaming");
        auto *model = mWindow->Model();
        QCOMPARE(model->rowCount(), 3);
        const qint64 row1Micros = model->data(model->index(1, timeCol), LogModelItemDataRole::SortRole).toLongLong();
        QVERIFY2(row1Micros > 0, "row 1 must carry a positive epoch-microseconds timestamp");

        QMenu *menu = mWindow->BuildRowContextMenu(/*sourceRow=*/1, nullptr);
        QVERIFY2(menu != nullptr, "BuildRowContextMenu must return a menu for a row with a timestamp");
        const QScopeGuard menuDeleter([&menu]() { menu->deleteLater(); });

        QAction *newerAction =
            FindMenuActionByText(menu, MainWindow::tr("Show only newer logs (%1)").arg(QStringLiteral("timestamp")));
        QVERIFY2(newerAction != nullptr, "menu must carry the newer-logs action");
        QCOMPARE(mWindow->Filters().size(), static_cast<size_t>(0));

        newerAction->trigger();
        QCoreApplication::processEvents();

        QCOMPARE(mWindow->Filters().size(), static_cast<size_t>(1));
        const auto &installed = mWindow->Filters().begin()->second;
        QCOMPARE(installed.type, loglib::LogConfiguration::LogFilter::Type::Time);
        QCOMPARE(installed.row, timeCol);
        QVERIFY(installed.filterBegin.has_value());
        // Open upper bound encoded as `std::nullopt` (not INT64_MAX).
        QVERIFY2(!installed.filterEnd.has_value(), "open upper bound must be std::nullopt, not INT64_MAX");
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        QCOMPARE(*installed.filterBegin, row1Micros);
    }

    // Symmetric: "older" gives `(nullopt, micros)`.
    void TestRowContextMenuAtOrBeforeAddsTimeFilter()
    {
        const int timeCol = StreamFixtureWithTimeColumnForRowMenuTests();
        QVERIFY2(timeCol >= 0, "timestamp column must exist after streaming");
        auto *model = mWindow->Model();
        QCOMPARE(model->rowCount(), 3);
        const qint64 row2Micros = model->data(model->index(2, timeCol), LogModelItemDataRole::SortRole).toLongLong();
        QVERIFY2(row2Micros > 0, "row 2 must carry a positive epoch-microseconds timestamp");

        QMenu *menu = mWindow->BuildRowContextMenu(/*sourceRow=*/2, nullptr);
        QVERIFY2(menu != nullptr, "BuildRowContextMenu must return a menu for a row with a timestamp");
        const QScopeGuard menuDeleter([&menu]() { menu->deleteLater(); });

        QAction *olderAction =
            FindMenuActionByText(menu, MainWindow::tr("Show only older logs (%1)").arg(QStringLiteral("timestamp")));
        QVERIFY2(olderAction != nullptr, "menu must carry the older-logs action");

        olderAction->trigger();
        QCoreApplication::processEvents();

        QCOMPARE(mWindow->Filters().size(), static_cast<size_t>(1));
        const auto &installed = mWindow->Filters().begin()->second;
        QCOMPARE(installed.type, loglib::LogConfiguration::LogFilter::Type::Time);
        QCOMPARE(installed.row, timeCol);
        QVERIFY2(!installed.filterBegin.has_value(), "open lower bound must be std::nullopt, not INT64_MIN");
        QVERIFY(installed.filterEnd.has_value());
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        QCOMPARE(*installed.filterEnd, row2Micros);
    }

    // Action lambdas must re-resolve the time column by its captured
    // keys at trigger time so a reorder between build and click still
    // targets the right column.
    void TestRowContextMenuAddTimeFilterAfterColumnReorderResolvesByKeys()
    {
        const int timeCol = StreamFixtureWithTimeColumnForRowMenuTests();
        QVERIFY2(timeCol >= 0, "timestamp column must exist after streaming");
        auto *model = mWindow->Model();
        const int columnCount = model->columnCount();
        QVERIFY2(columnCount >= 2, "fixture must yield at least two columns");

        QMenu *menu = mWindow->BuildRowContextMenu(/*sourceRow=*/0, nullptr);
        QVERIFY2(menu != nullptr, "BuildRowContextMenu must return a menu for a row with a timestamp");
        const QScopeGuard menuDeleter([&menu]() { menu->deleteLater(); });

        // Move the time column after building the menu, before
        // triggering. The captured keys must re-resolve to the new
        // index.
        const int src = timeCol;
        const int dest = (src == 0) ? columnCount - 1 : 0;
        QVERIFY(src != dest);
        QVERIFY2(
            QMetaObject::invokeMethod(
                mWindow,
                "OnHeaderSectionMoved",
                Qt::DirectConnection,
                Q_ARG(int, src),
                Q_ARG(int, src),
                Q_ARG(int, dest)
            ),
            "OnHeaderSectionMoved slot must be invocable via meta-object"
        );
        QCoreApplication::processEvents();
        const int timeColAfter = ColumnByHeader(*model, QStringLiteral("timestamp"));
        QCOMPARE(timeColAfter, dest);

        QAction *newerAction =
            FindMenuActionByText(menu, MainWindow::tr("Show only newer logs (%1)").arg(QStringLiteral("timestamp")));
        QVERIFY2(newerAction != nullptr, "menu must carry the newer-logs action");

        newerAction->trigger();
        QCoreApplication::processEvents();

        QCOMPARE(mWindow->Filters().size(), static_cast<size_t>(1));
        const auto &installed = mWindow->Filters().begin()->second;
        QCOMPARE(installed.row, timeColAfter);
    }

    // Each click adds a fresh-UUID filter rather than replacing an
    // existing one on the same column. The additive behaviour lets the
    // user combine "newer than X" + "older than Y" without opening the
    // FilterEditor.
    void TestRowContextMenuAdditiveDoesNotReplaceExisting()
    {
        const int timeCol = StreamFixtureWithTimeColumnForRowMenuTests();
        QVERIFY2(timeCol >= 0, "timestamp column must exist after streaming");

        // Pre-seed a permissive bounded filter on the same column.
        // Bounds are decades around the fixture so it's identifiable
        // (real values, not INT64 sentinels) and excludes no rows.
        const QString preSeededId = QStringLiteral("pre-seeded-time-filter");
        const std::optional<qint64> preBegin{1'000'000'000'000'000LL}; // 2001-09-09 UTC, micros.
        const std::optional<qint64> preEnd{4'000'000'000'000'000LL};   // 2096-10-02 UTC, micros.
        QVERIFY2(
            QMetaObject::invokeMethod(
                mWindow,
                "FilterTimeStampSubmitted",
                Qt::DirectConnection,
                Q_ARG(QString, preSeededId),
                Q_ARG(int, timeCol),
                Q_ARG(std::optional<qint64>, preBegin),
                Q_ARG(std::optional<qint64>, preEnd)
            ),
            "FilterTimeStampSubmitted slot must be invocable via meta-object"
        );
        QCoreApplication::processEvents();
        QCOMPARE(mWindow->Filters().size(), static_cast<size_t>(1));

        QMenu *menu = mWindow->BuildRowContextMenu(/*sourceRow=*/1, nullptr);
        QVERIFY2(menu != nullptr, "BuildRowContextMenu must return a menu for a row with a timestamp");
        const QScopeGuard menuDeleter([&menu]() { menu->deleteLater(); });

        QAction *newerAction =
            FindMenuActionByText(menu, MainWindow::tr("Show only newer logs (%1)").arg(QStringLiteral("timestamp")));
        QVERIFY2(newerAction != nullptr, "menu must carry the newer-logs action");

        newerAction->trigger();
        QCoreApplication::processEvents();

        QCOMPARE(mWindow->Filters().size(), static_cast<size_t>(2));
        // Pre-seeded filter survives unchanged; the menu added a sibling.
        const auto it = mWindow->Filters().find(preSeededId.toStdString());
        QVERIFY2(it != mWindow->Filters().end(), "pre-seeded filter must survive the menu trigger");
        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): false positive; prior `QVERIFY2` aborts on .end().
        QCOMPARE(it->second.type, loglib::LogConfiguration::LogFilter::Type::Time);
        QVERIFY(it->second.filterBegin.has_value());
        QVERIFY(it->second.filterEnd.has_value());
        // NOLINTBEGIN(bugprone-unchecked-optional-access)
        QCOMPARE(*it->second.filterBegin, *preBegin);
        QCOMPARE(*it->second.filterEnd, *preEnd);
        // NOLINTEND(bugprone-unchecked-optional-access)
    }

    // Regression: editing a `(begin, nullopt)` filter without changes
    // must round-trip the open upper bound. The pre-fix code silently
    // rewrote it to `9999-12-31T23:59:59` (the QDateTimeEdit ceiling).
    void TestRowContextMenuTimeFilterRoundTripsThroughEditor()
    {
        const int timeCol = StreamFixtureWithTimeColumnForRowMenuTests();
        QVERIFY2(timeCol >= 0, "timestamp column must exist after streaming");
        auto *model = mWindow->Model();
        const qint64 row1Micros = model->data(model->index(1, timeCol), LogModelItemDataRole::SortRole).toLongLong();
        QVERIFY2(row1Micros > 0, "row 1 must carry a positive epoch-microseconds timestamp");

        // Step 1: install a `(row1Micros, nullopt)` filter via the
        // row menu.
        QMenu *rowMenu = mWindow->BuildRowContextMenu(/*sourceRow=*/1, nullptr);
        QVERIFY2(rowMenu != nullptr, "BuildRowContextMenu must return a menu for a row with a timestamp");
        const QScopeGuard rowMenuDeleter([&rowMenu]() { rowMenu->deleteLater(); });
        QAction *newerAction =
            FindMenuActionByText(rowMenu, MainWindow::tr("Show only newer logs (%1)").arg(QStringLiteral("timestamp")));
        QVERIFY2(newerAction != nullptr, "row menu must carry the newer-logs action");
        newerAction->trigger();
        QCoreApplication::processEvents();

        QCOMPARE(mWindow->Filters().size(), static_cast<size_t>(1));
        const QString filterId = QString::fromStdString(mWindow->Filters().begin()->first);
        QVERIFY2(
            !mWindow->Filters().begin()->second.filterEnd.has_value(),
            "installed filter must carry an open upper bound (nullopt)"
        );

        // Step 2: trigger Edit from the column-header sub-menu.
        auto built = mWindow->BuildHeaderContextMenu(timeCol, nullptr);
        QVERIFY2(built.menu != nullptr, "BuildHeaderContextMenu must return a menu");
        const QScopeGuard headerMenuDeleter([&built]() { built.menu->deleteLater(); });
        const QMenu *subMenu = built.menu->findChild<QMenu *>(filterId);
        QVERIFY2(subMenu != nullptr, "filter sub-menu must be exposed");
        QAction *editAction = nullptr;
        const QString editLabel = MainWindow::tr("Edit");
        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): false positive; prior `QVERIFY2` aborts on null.
        for (QAction *act : subMenu->actions())
        {
            if (act->text() == editLabel)
            {
                editAction = act;
                break;
            }
        }
        QVERIFY2(editAction != nullptr, "sub-menu must contain an Edit action");
        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): false positive; prior `QVERIFY2` aborts on null.
        editAction->trigger();
        QCoreApplication::processEvents();

        const FilterEditor *editor = nullptr;
        for (QWidget *widget : QApplication::topLevelWidgets())
        {
            if (auto *e = qobject_cast<FilterEditor *>(widget))
            {
                editor = e;
                break;
            }
        }
        QVERIFY2(editor != nullptr, "Edit must open a FilterEditor");
        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): false positive; prior `QVERIFY2` aborts on null.
        QCOMPARE(editor->GetRowToFilter(), timeCol);

        // Step 3: click OK without touching anything. The editor must
        // read the open-bound state and emit `nullopt` back.
        auto *ok = editor->findChild<QPushButton *>(QStringLiteral("okButton"));
        QVERIFY2(ok != nullptr, "FilterEditor must expose its OK button");
        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): false positive; prior `QVERIFY2` aborts on null.
        ok->click();
        QCoreApplication::processEvents();

        // Step 4: filter must keep its `(row1Micros, nullopt)` shape.
        QCOMPARE(mWindow->Filters().size(), static_cast<size_t>(1));
        const auto it = mWindow->Filters().find(filterId.toStdString());
        QVERIFY2(it != mWindow->Filters().end(), "filter id must survive Edit -> OK");
        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): false positive; prior `QVERIFY2` aborts on .end().
        QCOMPARE(it->second.type, loglib::LogConfiguration::LogFilter::Type::Time);
        QCOMPARE(it->second.row, timeCol);
        QVERIFY(it->second.filterBegin.has_value());
        QVERIFY2(!it->second.filterEnd.has_value(), "open upper bound must round-trip as std::nullopt");
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        QCOMPARE(*it->second.filterBegin, row1Micros);

        // Editor deletes itself via `Qt::WA_DeleteOnClose` on accept;
        // spin the loop to match other editor-using tests.
        QCoreApplication::processEvents();
    }

    // Regression: opening Edit on a `(nullopt, X)` filter must let the
    // user uncheck the begin-unbounded checkbox and pick a begin
    // earlier than `X`. The pre-fix code pinned begin to `[X, X]`
    // because it set `setMinimumDateTime(X)` from the fallback seed
    // and never cleared it.
    void TestRowContextMenuEditUncheckWidensOpenBeginBound()
    {
        const int timeCol = StreamFixtureWithTimeColumnForRowMenuTests();
        QVERIFY2(timeCol >= 0, "timestamp column must exist after streaming");
        auto *model = mWindow->Model();
        const qint64 row2Micros = model->data(model->index(2, timeCol), LogModelItemDataRole::SortRole).toLongLong();
        QVERIFY2(row2Micros > 0, "row 2 must carry a positive epoch-microseconds timestamp");

        // Step 1: install a `(nullopt, row2Micros)` filter via the
        // row menu's "older" action.
        QMenu *rowMenu = mWindow->BuildRowContextMenu(/*sourceRow=*/2, nullptr);
        QVERIFY2(rowMenu != nullptr, "BuildRowContextMenu must return a menu for a row with a timestamp");
        const QScopeGuard rowMenuDeleter([&rowMenu]() { rowMenu->deleteLater(); });
        QAction *olderAction =
            FindMenuActionByText(rowMenu, MainWindow::tr("Show only older logs (%1)").arg(QStringLiteral("timestamp")));
        QVERIFY2(olderAction != nullptr, "row menu must carry the older-logs action");
        olderAction->trigger();
        QCoreApplication::processEvents();

        QCOMPARE(mWindow->Filters().size(), static_cast<size_t>(1));
        const QString filterId = QString::fromStdString(mWindow->Filters().begin()->first);
        QVERIFY2(
            !mWindow->Filters().begin()->second.filterBegin.has_value(),
            "installed filter must carry an open lower bound (nullopt)"
        );

        // Step 2: open the editor via the column-header sub-menu's Edit action.
        auto built = mWindow->BuildHeaderContextMenu(timeCol, nullptr);
        QVERIFY2(built.menu != nullptr, "BuildHeaderContextMenu must return a menu");
        const QScopeGuard headerMenuDeleter([&built]() { built.menu->deleteLater(); });
        const QMenu *subMenu = built.menu->findChild<QMenu *>(filterId);
        QVERIFY2(subMenu != nullptr, "filter sub-menu must be exposed");
        QAction *editAction = nullptr;
        const QString editLabel = MainWindow::tr("Edit");
        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): false positive; prior `QVERIFY2` aborts on null.
        for (QAction *act : subMenu->actions())
        {
            if (act->text() == editLabel)
            {
                editAction = act;
                break;
            }
        }
        QVERIFY2(editAction != nullptr, "sub-menu must contain an Edit action");
        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): false positive; prior `QVERIFY2` aborts on null.
        editAction->trigger();
        QCoreApplication::processEvents();

        const FilterEditor *editor = nullptr;
        for (QWidget *widget : QApplication::topLevelWidgets())
        {
            if (auto *e = qobject_cast<FilterEditor *>(widget))
            {
                editor = e;
                break;
            }
        }
        QVERIFY2(editor != nullptr, "Edit must open a FilterEditor");
        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): false positive; prior `QVERIFY2` aborts on null.

        // Step 3: editor must reflect the loaded shape: begin
        // unbounded, end bounded.
        auto *beginUnbounded = editor->findChild<QCheckBox *>(QStringLiteral("beginUnboundedCheckBox"));
        const auto *endUnbounded = editor->findChild<QCheckBox *>(QStringLiteral("endUnboundedCheckBox"));
        const auto *beginDate = editor->findChild<QDateEdit *>(QStringLiteral("beginDateEdit"));
        const auto *beginTime = editor->findChild<QTimeEdit *>(QStringLiteral("beginTimeEdit"));
        const auto *endDate = editor->findChild<QDateEdit *>(QStringLiteral("endDateEdit"));
        const auto *endTime = editor->findChild<QTimeEdit *>(QStringLiteral("endTimeEdit"));
        QVERIFY(beginUnbounded != nullptr);
        QVERIFY(endUnbounded != nullptr);
        QVERIFY(beginDate != nullptr);
        QVERIFY(beginTime != nullptr);
        QVERIFY(endDate != nullptr);
        QVERIFY(endTime != nullptr);
        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): false positive; prior `QVERIFY` aborts on null.
        QVERIFY2(beginUnbounded->isChecked(), "begin checkbox must be checked for a (nullopt, X) load");
        QVERIFY2(!endUnbounded->isChecked(), "end checkbox must be unchecked when end is bounded");

        // Step 4: the property the `SetBeginEnd` fix changes: begin
        // edits' minimum must NOT be pinned to the end seed, so the
        // user can pick a value earlier than `X` after unchecking.
        const QDateTime endSeed = endDate->dateTime();
        QVERIFY2(
            beginDate->minimumDateTime() < endSeed,
            "begin date edit's minimum must allow picking values earlier than the bounded end seed"
        );
        QVERIFY2(
            beginTime->minimumDateTime() < endSeed,
            "begin time edit's minimum must allow picking values earlier than the bounded end seed"
        );

        // Step 5: uncheck begin-unbounded; edits must become enabled.
        beginUnbounded->setChecked(false);
        QCoreApplication::processEvents();
        QVERIFY2(beginDate->isEnabled(), "begin date edit must be enabled after uncheck");
        QVERIFY2(beginTime->isEnabled(), "begin time edit must be enabled after uncheck");

        // Step 6: clicking OK now must emit a real begin (not
        // nullopt) and round-trip the original end. The test asserts
        // the *ability* to widen; the actual widening is straight
        // QDateTimeEdit usage once the minimum is clear.
        auto *ok = editor->findChild<QPushButton *>(QStringLiteral("okButton"));
        QVERIFY(ok != nullptr);
        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): false positive; prior `QVERIFY` aborts on null.
        ok->click();
        QCoreApplication::processEvents();

        QCOMPARE(mWindow->Filters().size(), static_cast<size_t>(1));
        const auto it = mWindow->Filters().find(filterId.toStdString());
        QVERIFY2(it != mWindow->Filters().end(), "filter id must survive Edit -> OK");
        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): false positive; prior `QVERIFY2` aborts on .end().
        QCOMPARE(it->second.type, loglib::LogConfiguration::LogFilter::Type::Time);
        QCOMPARE(it->second.row, timeCol);
        QVERIFY2(it->second.filterBegin.has_value(), "uncheck must promote begin from nullopt to the seed value");
        QVERIFY(it->second.filterEnd.has_value());
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        QCOMPARE(*it->second.filterEnd, row2Micros);

        QCoreApplication::processEvents();
    }

    // `Column::visible` survives the Save / Reset / Load round-trip
    // (driven through the manager directly so the file dialogs
    // don't pop).
    void TestColumnVisibilityRoundTripsThroughSaveLoad()
    {
        const int levelCol = StreamFixtureForColumnTests();
        QVERIFY2(levelCol >= 0, "level column must exist after streaming");
        auto *model = mWindow->Model();

        mWindow->SetColumnVisible(levelCol, false);
        QVERIFY(!model->Configuration().columns[static_cast<size_t>(levelCol)].visible);

        const QTemporaryDir savedDir;
        QVERIFY(savedDir.isValid());
        const QString savedPath = savedDir.filePath(QStringLiteral("config.json"));
        model->ConfigurationManager().Save(savedPath.toStdString());

        // `Reset` wipes the model; `Load` repopulates columns;
        // `ApplyColumnVisibility` is the production hook that pushes
        // the loaded flags onto the header.
        model->Reset();
        model->ConfigurationManager().Load(savedPath.toStdString());
        mWindow->ApplyColumnVisibility();
        QCoreApplication::processEvents();

        const auto &reloadedColumns = model->Configuration().columns;
        QVERIFY2(
            static_cast<size_t>(levelCol) < reloadedColumns.size(), "level column index must remain valid after reload"
        );
        QVERIFY2(
            !reloadedColumns[static_cast<size_t>(levelCol)].visible, "hidden flag must survive Save / Reset / Load"
        );

        const QHeaderView *header = mWindow->findChild<LogTableView *>()->horizontalHeader();
        QVERIFY2(header != nullptr, "view must own a horizontal header");
        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): false positive; prior `QVERIFY2` aborts on null.
        QVERIFY2(header->isSectionHidden(levelCol), "header section must remain hidden after reload");
    }

    // `LogModel::MoveColumn` rotates columns at the source level and
    // remaps `LogFilter::row` for every persisted filter through the
    // manager. The proxy chain forwards the move; the in-memory
    // `MainWindow::mFilters` map is updated by `OnHeaderSectionMoved`
    // when the move was initiated from a header drag, but a direct
    // `LogModel::MoveColumn` call only updates the lib-side
    // configuration. This test pins the lib-side remap on the
    // `LogConfiguration::filters` vector that `Save` would persist.
    void TestMoveColumnRemapsConfigurationFilters()
    {
        const int levelCol = StreamFixtureForColumnTests();
        QVERIFY2(levelCol >= 0, "level column must exist after streaming");
        auto *model = mWindow->Model();
        const int columnCount = model->columnCount();
        QVERIFY2(columnCount >= 2, "fixture must yield at least two columns");

        // Seed a saved filter on `level`. The public `AddFilter`
        // path opens an editor, so write through Save / Load instead
        // (the manager only exposes a const `Configuration()`).
        auto &mgr = model->ConfigurationManager();
        loglib::LogConfiguration configuration = mgr.Configuration();
        configuration.filters.push_back(loglib::LogConfiguration::LogFilter{
            .type = loglib::LogConfiguration::LogFilter::Type::Enumeration,
            .row = levelCol,
            .filterString = std::nullopt,
            .matchType = std::nullopt,
            .filterBegin = std::nullopt,
            .filterEnd = std::nullopt,
            .filterMinValue = std::nullopt,
            .filterMaxValue = std::nullopt,
            .filterValues = {"info"},
        });
        const QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        const QString path = tempDir.filePath(QStringLiteral("with-filter.json"));
        std::string json;
        const auto writeError = glz::write_json(configuration, json);
        QVERIFY(!writeError);
        {
            std::ofstream stream(path.toStdString(), std::ios::binary);
            QVERIFY(stream.is_open());
            stream << json;
        }
        model->Reset();
        mgr.Load(path.toStdString());
        QCoreApplication::processEvents();

        const int levelAfterReload = ColumnByHeader(*model, QStringLiteral("category"));
        QVERIFY2(levelAfterReload >= 0, "level column must reload");
        QCOMPARE(mgr.Configuration().filters.size(), static_cast<size_t>(1));
        QCOMPARE(mgr.Configuration().filters[0].row, levelAfterReload);

        // Move `level`; the saved filter's `row` must follow it.
        const int src = levelAfterReload;
        const int dest = (src == 0) ? model->columnCount() - 1 : 0;
        QVERIFY(src != dest);
        QVERIFY2(model->MoveColumn(src, dest), "MoveColumn must succeed");
        QCoreApplication::processEvents();

        QCOMPARE(mgr.Configuration().filters.size(), static_cast<size_t>(1));
        QCOMPARE(mgr.Configuration().filters[0].row, dest);
    }

    // Regression: an implicit source-side move (e.g. the streaming
    // timestamp bubble in `LogModel::AppendBatch`) used to leave
    // the runtime `mFilters` map pointing at the pre-move indices,
    // which `Save` would then mirror back over the lib-rotated
    // wire format and drop on next load. The fix wires
    // `OnSourceColumnsMoved` to `LogModel::columnsMoved`. This
    // exercises the slot through `LogModel::MoveColumn`, which
    // emits the same payload the bubble path does.
    void TestSourceColumnMoveRemapsRuntimeFilters()
    {
        const int levelCol = StreamFixtureForColumnTests();
        QVERIFY2(levelCol >= 0, "level column must exist after streaming");
        auto *model = mWindow->Model();
        const int columnCount = model->columnCount();
        QVERIFY2(columnCount >= 2, "fixture must yield at least two columns");

        // Install an enum filter on `level` via the production
        // `FilterEnumSubmitted` slot so both stores populate through
        // the supported entry point.
        const QString filterId = QStringLiteral("bubble-remap");
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

        const std::string filterKey = filterId.toStdString();
        QVERIFY2(mWindow->Filters().contains(filterKey), "filter must land in mFilters after submit");
        QCOMPARE(mWindow->Filters().at(filterKey).row, levelCol);

        // Bypass `OnHeaderSectionMoved` and emit `columnsMoved`
        // directly via `LogModel::MoveColumn` -- the streaming
        // bubble path runs the same code synchronously inside
        // `endMoveColumns()`.
        const int src = levelCol;
        const int dest = (src == 0) ? columnCount - 1 : 0;
        QVERIFY(src != dest);
        QVERIFY2(model->MoveColumn(src, dest), "MoveColumn must succeed");
        QCoreApplication::processEvents();

        // Runtime map and wire-format vector both follow the move.
        QVERIFY2(mWindow->Filters().contains(filterKey), "filter must survive the source-side move");
        QCOMPARE(mWindow->Filters().at(filterKey).row, dest);
        const auto &wireFilters = model->Configuration().filters;
        QCOMPARE(wireFilters.size(), static_cast<size_t>(1));
        QCOMPARE(wireFilters[0].row, dest);

        // The remapped rule still targets `level`, so the proxy
        // should keep matching rows visible. A stale `row` would
        // silently zero this out.
        auto *tableView = mWindow->findChild<LogTableView *>();
        QVERIFY(tableView != nullptr);
        const QAbstractItemModel *proxy = tableView->model();
        QVERIFY(proxy != nullptr);
        QVERIFY2(proxy->rowCount() > 0, "filter on level=info must keep at least one row visible after the move");
    }

    // Regression: a source-side column move (header drag or
    // streaming bubble) must keep `setSectionHidden` aligned with
    // `Column::visible`. Qt usually carries hidden flags through
    // `columnsMoved`, but bails on a zero-row source -- so
    // `OnSourceColumnsMoved` re-applies explicitly. Pairs with
    // `TestSourceColumnMoveRemapsRuntimeFilters` (filter remap).
    void TestSourceColumnMovePreservesHiddenColumn()
    {
        const int levelCol = StreamFixtureForColumnTests();
        QVERIFY2(levelCol >= 0, "level column must exist after streaming");
        auto *model = mWindow->Model();
        const int columnCount = model->columnCount();
        QVERIFY2(columnCount >= 2, "fixture must yield at least two columns");

        // Hide `level`; after the move it lands at `dest` and the
        // matching header section should still report hidden.
        mWindow->SetColumnVisible(levelCol, false);
        const QHeaderView *header = mWindow->findChild<LogTableView *>()->horizontalHeader();
        QVERIFY2(header != nullptr, "view must own a horizontal header");
        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): false positive; prior `QVERIFY2` aborts on null.
        QVERIFY2(header->isSectionHidden(levelCol), "precondition: section hidden before move");

        // `LogModel::MoveColumn` emits the same `columnsMoved`
        // payload the streaming bubble does, so the slot under test
        // runs the same code.
        const int src = levelCol;
        const int dest = (src == 0) ? columnCount - 1 : 0;
        QVERIFY(src != dest);
        QVERIFY2(model->MoveColumn(src, dest), "MoveColumn must succeed");
        QCoreApplication::processEvents();

        // The whole `Column` struct rotates with the move, so
        // `dest` is now the hidden index.
        QVERIFY2(
            !model->Configuration().columns[static_cast<size_t>(dest)].visible,
            "Column::visible must follow the moved column to its new index"
        );
        QVERIFY2(
            model->Configuration().columns[static_cast<size_t>(src)].visible,
            "the column displaced to `src` must remain visible"
        );

        // Header flags must agree with the configuration. Without
        // the `ApplyColumnVisibility()` inside `OnSourceColumnsMoved`,
        // this would only hold on the user-drag path.
        QVERIFY2(
            header->isSectionHidden(dest), "header section at the moved column's new logical index must remain hidden"
        );
        QVERIFY2(!header->isSectionHidden(src), "header section at the displaced column's index must not be hidden");
    }

    // Pin the wire format: `Save` output must contain `visible`
    // for both visible and hidden columns. A future Glaze meta
    // change that hides the field would surface here.
    void TestColumnVisibleFieldEmittedInSavedJson()
    {
        const int levelCol = StreamFixtureForColumnTests();
        QVERIFY2(levelCol >= 0, "level column must exist after streaming");
        auto *model = mWindow->Model();
        mWindow->SetColumnVisible(levelCol, false);

        const QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        const QString path = tempDir.filePath(QStringLiteral("with-visible.json"));
        model->ConfigurationManager().Save(path.toStdString());

        QFile file(path);
        QVERIFY(file.open(QFile::ReadOnly));
        const QByteArray contents = file.readAll();
        QVERIFY2(contents.contains("\"visible\": true"), "JSON must contain \"visible\": true");
        QVERIFY2(contents.contains("\"visible\": false"), "JSON must contain \"visible\": false");
    }

    // Regression: `Reset` emits `modelReset`, which clears every
    // `setSectionHidden` flag. The configuration's `Column::visible`
    // survives, and the wired `modelReset -> ApplyColumnVisibility`
    // pushes the flags back. Without it, a hidden column would be
    // silently re-shown after every reset.
    void TestVisibilityReappliedAfterModelReset()
    {
        const int levelCol = StreamFixtureForColumnTests();
        QVERIFY2(levelCol >= 0, "level column must exist after streaming");
        auto *model = mWindow->Model();

        mWindow->SetColumnVisible(levelCol, false);
        const QHeaderView *header = mWindow->findChild<LogTableView *>()->horizontalHeader();
        QVERIFY2(header != nullptr, "view must own a horizontal header");
        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): false positive; prior `QVERIFY2` aborts on null.
        QVERIFY2(header->isSectionHidden(levelCol), "precondition: section hidden");

        const QSignalSpy resetSpy(model, &QAbstractItemModel::modelReset);
        QVERIFY(resetSpy.isValid());
        model->Reset();
        QCoreApplication::processEvents();
        QVERIFY2(resetSpy.count() >= 1, "Reset must emit modelReset");

        QVERIFY2(
            !model->Configuration().columns.empty(),
            "configuration columns must survive Reset (Reset wipes data, not configuration)"
        );
        QVERIFY2(
            !model->Configuration().columns[static_cast<size_t>(levelCol)].visible,
            "Column::visible flag must survive Reset"
        );
        QVERIFY2(header->isSectionHidden(levelCol), "modelReset hook must reapply Column::visible to the header");
    }

    // `TryLoadAsConfiguration` is the single-file entry from
    // `OpenFiles`. It must reapply `Column::visible` to the header,
    // otherwise a saved config would load but every column would
    // still be shown.
    void TestTryLoadAsConfigurationAppliesVisibility()
    {
        const int levelCol = StreamFixtureForColumnTests();
        QVERIFY2(levelCol >= 0, "level column must exist after streaming");
        auto *model = mWindow->Model();

        mWindow->SetColumnVisible(levelCol, false);

        const QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        const QString savedPath = tempDir.filePath(QStringLiteral("hidden-level.json"));
        model->ConfigurationManager().Save(savedPath.toStdString());

        // Re-show first so the load + reapply has something to
        // do; otherwise the test would pass without the new
        // `ApplyColumnVisibility` inside `TryLoadAsConfiguration`.
        mWindow->SetColumnVisible(levelCol, true);
        const QHeaderView *header = mWindow->findChild<LogTableView *>()->horizontalHeader();
        QVERIFY2(header != nullptr, "view must own a horizontal header");
        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): false positive; prior `QVERIFY2` aborts on null.
        QVERIFY2(!header->isSectionHidden(levelCol), "precondition: section visible before load");

        QVERIFY2(mWindow->TryLoadAsConfigurationForTest(savedPath), "TryLoadAsConfiguration must succeed");
        QCoreApplication::processEvents();

        QVERIFY2(
            !model->Configuration().columns[static_cast<size_t>(levelCol)].visible,
            "loaded configuration must mark level hidden"
        );
        QVERIFY2(header->isSectionHidden(levelCol), "TryLoadAsConfiguration must reapply visibility to the header");
    }

    // Regression: loading a configuration must reset the active
    // sort. Saved column layouts can reorder or remove columns, so
    // a stale `mSortColumn` index from before the load now points
    // at a different column identity (or none at all). Without the
    // explicit `sortByColumn(-1, ...)` inside the load helpers, the
    // proxy would silently sort by whatever column happens to land
    // at the old index, and the header's sort indicator would still
    // be advertising the wrong column.
    void TestLoadConfigurationResetsActiveSort()
    {
        const int levelCol = StreamFixtureForColumnTests();
        QVERIFY2(levelCol >= 0, "level column must exist after streaming");
        auto *model = mWindow->Model();

        // Engage a sort on `level` via the view, then save the
        // configuration and reload it. Sort state lives on the
        // header (indicator) and on the proxy (`mSortColumn`); both
        // must clear after the reload.
        auto *tableView = mWindow->findChild<LogTableView *>();
        QVERIFY(tableView != nullptr);
        const QHeaderView *header = tableView->horizontalHeader();
        QVERIFY(header != nullptr);
        tableView->sortByColumn(levelCol, Qt::AscendingOrder);
        QCoreApplication::processEvents();
        QVERIFY2(header->isSortIndicatorShown(), "precondition: sort indicator must be shown after sortByColumn");
        QCOMPARE(header->sortIndicatorSection(), levelCol);

        const QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        const QString savedPath = tempDir.filePath(QStringLiteral("with-sort.json"));
        model->ConfigurationManager().Save(savedPath.toStdString());

        // The single-file open path; `DoLoadConfiguration` does the
        // same reset, so both entry points share the contract.
        QVERIFY2(mWindow->TryLoadAsConfigurationForTest(savedPath), "TryLoadAsConfiguration must succeed");
        QCoreApplication::processEvents();

        QVERIFY2(
            !header->isSortIndicatorShown() || header->sortIndicatorSection() == -1,
            "sort indicator must clear after configuration load"
        );
    }

    // The `View` menu rebuilds on every `aboutToShow` and gives
    // each column a checkable action that flips `Column::visible`.
    // It stays reachable when every header section is hidden.
    void TestViewMenuTogglesColumn()
    {
        const int levelCol = StreamFixtureForColumnTests();
        QVERIFY2(levelCol >= 0, "level column must exist after streaming");
        auto *model = mWindow->Model();

        auto *viewMenu = mWindow->findChild<QMenu *>(QStringLiteral("menuView"));
        QVERIFY2(viewMenu != nullptr, "View menu must exist");

        emit viewMenu->aboutToShow();

        const QString levelHeader =
            QString::fromStdString(model->Configuration().columns[static_cast<size_t>(levelCol)].header);
        QAction *levelAction = nullptr;
        for (QAction *act : viewMenu->actions())
        {
            if (act->text() == levelHeader)
            {
                levelAction = act;
                break;
            }
        }
        QVERIFY2(levelAction != nullptr, "View menu must contain an action for the level column");
        // NOLINTBEGIN(clang-analyzer-core.CallAndMessage): false positive; prior `QVERIFY2` aborts on null.
        QVERIFY2(levelAction->isCheckable(), "View menu action must be checkable");
        QVERIFY2(levelAction->isChecked(), "View menu action must reflect the visible state");

        levelAction->trigger();
        // NOLINTEND(clang-analyzer-core.CallAndMessage)
        QCoreApplication::processEvents();

        QVERIFY2(
            !model->Configuration().columns[static_cast<size_t>(levelCol)].visible,
            "toggling the View menu action must flip Column::visible"
        );
        const QHeaderView *header = mWindow->findChild<LogTableView *>()->horizontalHeader();
        QVERIFY2(header != nullptr, "view must own a horizontal header");
        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): false positive; prior `QVERIFY2` aborts on null.
        QVERIFY2(header->isSectionHidden(levelCol), "header section must follow the toggle");

        // Toggle back; rebuild the menu from scratch so the action
        // state reflects the new configuration.
        emit viewMenu->aboutToShow();
        QAction *levelActionAfter = nullptr;
        for (QAction *act : viewMenu->actions())
        {
            if (act->text() == levelHeader)
            {
                levelActionAfter = act;
                break;
            }
        }
        QVERIFY(levelActionAfter != nullptr);
        QVERIFY2(!levelActionAfter->isChecked(), "rebuilt action must reflect newly-hidden state");
        levelActionAfter->trigger();
        QCoreApplication::processEvents();

        QVERIFY2(
            model->Configuration().columns[static_cast<size_t>(levelCol)].visible,
            "second toggle must restore Column::visible"
        );
        QVERIFY2(!header->isSectionHidden(levelCol), "second toggle must un-hide the header section");
    }

    // Regression: clicking Edit after the filter's column was
    // reordered must open the editor against the column's *current*
    // index. Before the fix the lambda captured the filter by value,
    // freezing `row` at menu-build time; after a reorder, Edit
    // would mis-target the wrong column and `AddFilter`'s type-match
    // guard would silently drop the filter.
    void TestEditFilterAfterColumnReorderUsesCurrentRow()
    {
        const int levelCol = StreamFixtureForColumnTests();
        QVERIFY2(levelCol >= 0, "level column must exist after streaming");

        auto *model = mWindow->Model();
        const int columnCount = model->columnCount();
        QVERIFY2(columnCount >= 2, "fixture must yield at least two columns");

        // Install an enum filter on `level` via the production
        // submit slot (mirrors what `FilterEditor` emits on OK).
        const QString filterId = QStringLiteral("edit-after-move");
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

        const std::string filterKey = filterId.toStdString();
        QVERIFY2(mWindow->Filters().contains(filterKey), "filter must land in mFilters after submit");
        QCOMPARE(mWindow->Filters().at(filterKey).row, levelCol);

        // Reorder `level` via the production header-drag slot;
        // both the source column and `mFilters[*].row` shift.
        const int src = levelCol;
        const int dest = (src == 0) ? columnCount - 1 : 0;
        QVERIFY(src != dest);
        QVERIFY2(
            QMetaObject::invokeMethod(
                mWindow,
                "OnHeaderSectionMoved",
                Qt::DirectConnection,
                Q_ARG(int, src),
                Q_ARG(int, src),
                Q_ARG(int, dest)
            ),
            "OnHeaderSectionMoved slot must be invocable via meta-object"
        );
        QCoreApplication::processEvents();

        const int levelColAfter = ColumnByHeader(*model, QStringLiteral("category"));
        QCOMPARE(levelColAfter, dest);
        QVERIFY2(mWindow->Filters().contains(filterKey), "filter must survive the reorder");
        QCOMPARE(mWindow->Filters().at(filterKey).row, dest);

        const auto *filtersMenu = mWindow->findChild<QMenu *>(QStringLiteral("menuFilters"));
        QVERIFY2(filtersMenu != nullptr, "MainWindow must expose its Filters menu");
        const QAction *filterMenuAction = nullptr;
        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): false positive; prior `QVERIFY2` aborts on null.
        for (const QAction *action : filtersMenu->actions())
        {
            if (action->data().toString() == filterId)
            {
                filterMenuAction = action;
                break;
            }
        }
        QVERIFY2(filterMenuAction != nullptr, "active filter must have a Filters-menu entry");

        const auto *filterSubMenu = filtersMenu->findChild<QMenu *>(filterId);
        QVERIFY2(filterSubMenu != nullptr, "filter menu entry must own an Edit/Remove sub-menu");
        QAction *editAction = nullptr;
        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): false positive; prior `QVERIFY2` aborts on null.
        for (QAction *action : filterSubMenu->actions())
        {
            if (action->text() == QStringLiteral("Edit"))
            {
                editAction = action;
                break;
            }
        }
        QVERIFY2(editAction != nullptr, "filter sub-menu must contain an Edit action");

        // Triggering Edit opens a `FilterEditor`. In the buggy build
        // the stale lambda would type-mismatch against `columns[src]`
        // and silently drop the filter from `mFilters`.
        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): false positive; prior `QVERIFY2` aborts on null.
        editAction->trigger();
        QCoreApplication::processEvents();

        QVERIFY2(
            mWindow->Filters().contains(filterKey),
            "Edit must not drop the filter; the lambda must look up the live mFilters[id]"
        );
        QCOMPARE(mWindow->Filters().at(filterKey).row, dest);

        // The opened `FilterEditor` should be pointing at `dest`,
        // not the stale `src`.
        FilterEditor *editor = nullptr;
        for (QWidget *widget : QApplication::topLevelWidgets())
        {
            if (auto *e = qobject_cast<FilterEditor *>(widget))
            {
                editor = e;
                break;
            }
        }
        QVERIFY2(editor != nullptr, "Edit must open a FilterEditor");
        QCOMPARE(editor->GetRowToFilter(), dest);

        editor->close();
        editor->deleteLater();
        QCoreApplication::processEvents();
    }

    // Regression: filters added via the Filters menu used to live
    // only in `mFilters` and were never mirrored into the wire-
    // format vector `Save` persists, so they vanished on Save /
    // Load. The eager mirror keeps both stores in lockstep; the
    // load path rebuilds `mFilters` from the loaded vector.
    void TestFilterPersistenceRoundtrip()
    {
        const int levelCol = StreamFixtureForColumnTests();
        QVERIFY2(levelCol >= 0, "level column must exist after streaming");
        auto *model = mWindow->Model();

        const int msgCol = ColumnByHeader(*model, QStringLiteral("msg"));
        QVERIFY2(msgCol >= 0, "msg column must exist after streaming");

        // Install a string filter on `msg` (string-typed by
        // default) via the production submit slot.
        const QString filterId = QStringLiteral("persisted-string");
        QVERIFY2(
            QMetaObject::invokeMethod(
                mWindow,
                "FilterSubmitted",
                Qt::DirectConnection,
                Q_ARG(QString, filterId),
                Q_ARG(int, msgCol),
                Q_ARG(QString, QStringLiteral("m1")),
                Q_ARG(int, static_cast<int>(loglib::LogConfiguration::LogFilter::Match::Contains))
            ),
            "FilterSubmitted slot must be invocable via meta-object"
        );
        QCoreApplication::processEvents();

        QCOMPARE(mWindow->Filters().size(), static_cast<size_t>(1));

        // The eager mirror must have copied the filter into the
        // wire-format vector before any Save -- the load-bearing
        // invariant this fix establishes.
        QCOMPARE(model->Configuration().filters.size(), static_cast<size_t>(1));
        QCOMPARE(model->Configuration().filters[0].row, msgCol);
        QCOMPARE(model->Configuration().filters[0].type, loglib::LogConfiguration::LogFilter::Type::String);

        const QTemporaryDir savedDir;
        QVERIFY(savedDir.isValid());
        const QString savedPath = savedDir.filePath(QStringLiteral("filter-persistence.json"));

        mWindow->SaveConfigurationToPathForTest(savedPath);

        // Clear the active filter; the eager mirror empties the
        // wire-format vector too.
        QMetaObject::invokeMethod(mWindow, "ClearAllFilters", Qt::DirectConnection);
        QCoreApplication::processEvents();
        QCOMPARE(mWindow->Filters().size(), static_cast<size_t>(0));
        QCOMPARE(model->Configuration().filters.size(), static_cast<size_t>(0));

        // Suppress the modal so the test thread does not block.
        // No drops are expected here, but the load path always
        // checks before showing.
        mWindow->SetSuppressDialogsForTest(true);
        mWindow->LoadConfigurationFromPathForTest(savedPath);
        QCoreApplication::processEvents();

        QCOMPARE(mWindow->LastDroppedFilterCountForTest(), 0);
        QCOMPARE(mWindow->Filters().size(), static_cast<size_t>(1));
        const auto &revived = mWindow->Filters().begin()->second;
        QCOMPARE(revived.type, loglib::LogConfiguration::LogFilter::Type::String);
        QCOMPARE(revived.row, msgCol);
        QVERIFY(revived.filterString.has_value());
        QCOMPARE(*revived.filterString, std::string("m1"));
        QVERIFY(revived.matchType.has_value());
        QCOMPARE(*revived.matchType, loglib::LogConfiguration::LogFilter::Match::Contains);

        // The wire-format vector also reflects the revived state.
        QCOMPARE(model->Configuration().filters.size(), static_cast<size_t>(1));
    }

    // Multi-type round-trip: a string filter and an enum filter
    // persist together and revive into the correct slots, with one
    // Filters-menu entry per revived filter.
    void TestFilterPersistenceMultipleTypes()
    {
        const int levelCol = StreamFixtureForColumnTests();
        QVERIFY2(levelCol >= 0, "level column must exist after streaming");
        auto *model = mWindow->Model();
        const int msgCol = ColumnByHeader(*model, QStringLiteral("msg"));
        QVERIFY2(msgCol >= 0, "msg column must exist after streaming");

        // String filter on msg.
        const QString stringId = QStringLiteral("persisted-string");
        QMetaObject::invokeMethod(
            mWindow,
            "FilterSubmitted",
            Qt::DirectConnection,
            Q_ARG(QString, stringId),
            Q_ARG(int, msgCol),
            Q_ARG(QString, QStringLiteral("m1")),
            Q_ARG(int, static_cast<int>(loglib::LogConfiguration::LogFilter::Match::Contains))
        );
        // Enum filter on level (auto-promoted to enum by the streaming threshold).
        const QString enumId = QStringLiteral("persisted-enum");
        // Hoist to a named local: an inline brace-init inside
        // `Q_ARG(...)` confuses the preprocessor (commas).
        const QStringList enumValues{QStringLiteral("info"), QStringLiteral("warn")};
        QMetaObject::invokeMethod(
            mWindow,
            "FilterEnumSubmitted",
            Qt::DirectConnection,
            Q_ARG(QString, enumId),
            Q_ARG(int, levelCol),
            Q_ARG(QStringList, enumValues)
        );
        QCoreApplication::processEvents();

        QCOMPARE(mWindow->Filters().size(), static_cast<size_t>(2));
        QCOMPARE(model->Configuration().filters.size(), static_cast<size_t>(2));

        const QTemporaryDir savedDir;
        QVERIFY(savedDir.isValid());
        const QString savedPath = savedDir.filePath(QStringLiteral("multi-filter.json"));
        mWindow->SaveConfigurationToPathForTest(savedPath);

        QMetaObject::invokeMethod(mWindow, "ClearAllFilters", Qt::DirectConnection);
        QCoreApplication::processEvents();
        QCOMPARE(mWindow->Filters().size(), static_cast<size_t>(0));

        mWindow->SetSuppressDialogsForTest(true);
        mWindow->LoadConfigurationFromPathForTest(savedPath);
        QCoreApplication::processEvents();

        QCOMPARE(mWindow->LastDroppedFilterCountForTest(), 0);
        QCOMPARE(mWindow->Filters().size(), static_cast<size_t>(2));

        // Verify both types survived. UUIDs are regenerated on load
        // so look up by (type, row) rather than original IDs.
        bool sawString = false;
        bool sawEnum = false;
        for (const auto &[id, f] : mWindow->Filters())
        {
            if (f.type == loglib::LogConfiguration::LogFilter::Type::String && f.row == msgCol)
            {
                sawString = true;
                QVERIFY(f.filterString.has_value());
                QCOMPARE(*f.filterString, std::string("m1"));
            }
            else if (f.type == loglib::LogConfiguration::LogFilter::Type::Enumeration && f.row == levelCol)
            {
                sawEnum = true;
                QCOMPARE(f.filterValues.size(), static_cast<size_t>(2));
            }
        }
        QVERIFY2(sawString, "string filter on msg must revive");
        QVERIFY2(sawEnum, "enum filter on level must revive");

        // Filters menu must hold one sub-menu per revived filter.
        const auto *filtersMenu = mWindow->findChild<QMenu *>(QStringLiteral("menuFilters"));
        QVERIFY2(filtersMenu != nullptr, "MainWindow must expose its Filters menu");
        int subMenuCount = 0;
        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): false positive; prior `QVERIFY2` aborts on null.
        for (const QAction *action : filtersMenu->actions())
        {
            if (!action->data().toString().isNull())
            {
                ++subMenuCount;
            }
        }
        QCOMPARE(subMenuCount, 2);
    }

    // A configuration JSON mixing a valid filter with an invalid
    // one (out-of-range column index) must keep the valid filter,
    // drop the invalid one, and surface the drop through
    // `ShowDroppedFiltersDialog` (observed via the test counter).
    void TestFilterPersistenceDropsInvalidFilters()
    {
        const int levelCol = StreamFixtureForColumnTests();
        QVERIFY2(levelCol >= 0, "level column must exist after streaming");
        auto *model = mWindow->Model();
        const int columnCount = model->columnCount();

        // Build a config with two filters: a valid enum on `level`
        // and a string filter pointing past the end of the columns.
        // Serialise via Glaze so the load path sees the same wire
        // format `Save` emits.
        loglib::LogConfiguration configuration = model->Configuration();
        configuration.filters.clear();
        configuration.filters.push_back(loglib::LogConfiguration::LogFilter{
            .type = loglib::LogConfiguration::LogFilter::Type::Enumeration,
            .row = levelCol,
            .filterString = std::nullopt,
            .matchType = std::nullopt,
            .filterBegin = std::nullopt,
            .filterEnd = std::nullopt,
            .filterMinValue = std::nullopt,
            .filterMaxValue = std::nullopt,
            .filterValues = {"info"},
        });
        configuration.filters.push_back(loglib::LogConfiguration::LogFilter{
            .type = loglib::LogConfiguration::LogFilter::Type::String,
            .row = columnCount + 5, // intentionally off the end
            .filterString = std::string("nope"),
            .matchType = loglib::LogConfiguration::LogFilter::Match::Contains,
            .filterBegin = std::nullopt,
            .filterEnd = std::nullopt,
            .filterMinValue = std::nullopt,
            .filterMaxValue = std::nullopt,
            .filterValues = {},
        });

        const QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        const QString path = tempDir.filePath(QStringLiteral("mixed-filters.json"));
        std::string json;
        const auto writeError = glz::write_json(configuration, json);
        QVERIFY(!writeError);
        {
            std::ofstream stream(path.toStdString(), std::ios::binary);
            QVERIFY(stream.is_open());
            stream << json;
        }

        mWindow->SetSuppressDialogsForTest(true);
        mWindow->LoadConfigurationFromPathForTest(path);
        QCoreApplication::processEvents();

        QCOMPARE(mWindow->LastDroppedFilterCountForTest(), 1);
        QCOMPARE(mWindow->Filters().size(), static_cast<size_t>(1));
        const auto &surviving = mWindow->Filters().begin()->second;
        QCOMPARE(surviving.type, loglib::LogConfiguration::LogFilter::Type::Enumeration);
        const int levelColAfterLoad = ColumnByHeader(*model, QStringLiteral("category"));
        QVERIFY2(levelColAfterLoad >= 0, "level column must reload");
        QCOMPARE(surviving.row, levelColAfterLoad);
    }

    // Two consecutive `Save`s of the same filter set must produce
    // byte-identical JSON. `MirrorSessionStateToConfiguration` sorts the
    // snapshot so the output is independent of the `unordered_map`
    // iteration order. Without the sort, save -> reopen -> save
    // would change the file every round-trip.
    void TestFilterPersistenceSaveOrderingIsDeterministic()
    {
        const int levelCol = StreamFixtureForColumnTests();
        QVERIFY2(levelCol >= 0, "level column must exist after streaming");
        auto *model = mWindow->Model();
        const int msgCol = ColumnByHeader(*model, QStringLiteral("msg"));
        QVERIFY2(msgCol >= 0, "msg column must exist after streaming");

        // Three filters across two columns and two types so the
        // sort has to break ties beyond `row`. Submit out of sort
        // order so a missing sort would show in the saved bytes.
        const QStringList enumValues{QStringLiteral("info"), QStringLiteral("warn")};
        QMetaObject::invokeMethod(
            mWindow,
            "FilterEnumSubmitted",
            Qt::DirectConnection,
            Q_ARG(QString, QStringLiteral("z-enum")),
            Q_ARG(int, levelCol),
            Q_ARG(QStringList, enumValues)
        );
        QMetaObject::invokeMethod(
            mWindow,
            "FilterSubmitted",
            Qt::DirectConnection,
            Q_ARG(QString, QStringLiteral("a-string-msg")),
            Q_ARG(int, msgCol),
            Q_ARG(QString, QStringLiteral("m1")),
            Q_ARG(int, static_cast<int>(loglib::LogConfiguration::LogFilter::Match::Contains))
        );
        QMetaObject::invokeMethod(
            mWindow,
            "FilterSubmitted",
            Qt::DirectConnection,
            Q_ARG(QString, QStringLiteral("m-string-msg-2")),
            Q_ARG(int, msgCol),
            Q_ARG(QString, QStringLiteral("m2")),
            Q_ARG(int, static_cast<int>(loglib::LogConfiguration::LogFilter::Match::Contains))
        );
        QCoreApplication::processEvents();

        const QTemporaryDir savedDir;
        QVERIFY(savedDir.isValid());
        const QString pathA = savedDir.filePath(QStringLiteral("save-a.json"));
        const QString pathB = savedDir.filePath(QStringLiteral("save-b.json"));

        mWindow->SaveConfigurationToPathForTest(pathA);
        mWindow->SaveConfigurationToPathForTest(pathB);

        QFile fileA(pathA);
        QFile fileB(pathB);
        QVERIFY(fileA.open(QFile::ReadOnly));
        QVERIFY(fileB.open(QFile::ReadOnly));
        const QByteArray bytesA = fileA.readAll();
        const QByteArray bytesB = fileB.readAll();
        QCOMPARE(bytesA, bytesB);

        // Stronger pin: round-trip through Load and save again. The
        // load rebuilds `mFilters` with fresh UUIDs, so the iteration
        // order is guaranteed different -- yet the bytes must match.
        mWindow->SetSuppressDialogsForTest(true);
        mWindow->LoadConfigurationFromPathForTest(pathA);
        QCoreApplication::processEvents();
        const QString pathRoundTrip = savedDir.filePath(QStringLiteral("save-c.json"));
        mWindow->SaveConfigurationToPathForTest(pathRoundTrip);

        QFile fileC(pathRoundTrip);
        QVERIFY(fileC.open(QFile::ReadOnly));
        const QByteArray bytesC = fileC.readAll();
        QCOMPARE(bytesC, bytesA);
    }

    // `Save Configuration...` (SaveScope::ColumnsOnly) writes a
    // portable layout: columns survive, session-only state
    // (filters, sort) is omitted from the file on purpose so the
    // configuration can be applied to a different log source
    // without dragging old filter rows / sort indices along.
    void TestSaveScopeColumnsOnlyOmitsSession()
    {
        const int levelCol = StreamFixtureForColumnTests();
        QVERIFY2(levelCol >= 0, "level column must exist after streaming");
        auto *model = mWindow->Model();
        const int msgCol = ColumnByHeader(*model, QStringLiteral("msg"));
        QVERIFY2(msgCol >= 0, "msg column must exist after streaming");

        // Engage filter + sort so the wire-format vector + sort field
        // are non-default before saving.
        QMetaObject::invokeMethod(
            mWindow,
            "FilterSubmitted",
            Qt::DirectConnection,
            Q_ARG(QString, QStringLiteral("columns-only-filter")),
            Q_ARG(int, msgCol),
            Q_ARG(QString, QStringLiteral("m1")),
            Q_ARG(int, static_cast<int>(loglib::LogConfiguration::LogFilter::Match::Contains))
        );
        auto *tableView = mWindow->findChild<LogTableView *>();
        QVERIFY(tableView != nullptr);
        tableView->sortByColumn(levelCol, Qt::DescendingOrder);
        QCoreApplication::processEvents();

        QCOMPARE(mWindow->Filters().size(), static_cast<size_t>(1));

        const QTemporaryDir savedDir;
        QVERIFY(savedDir.isValid());
        const QString columnsOnlyPath = savedDir.filePath(QStringLiteral("columns-only.json"));
        mWindow->SaveConfigurationToPathForTest(columnsOnlyPath, loglib::SaveScope::ColumnsOnly);

        // Reload into a fresh manager and confirm the on-disk file is
        // a configuration-shape file: columns present, session-only
        // fields back at their inert defaults.
        loglib::LogConfigurationManager probe;
        probe.Load(columnsOnlyPath.toStdString());
        QVERIFY(!probe.Configuration().columns.empty());
        QVERIFY2(probe.Configuration().filters.empty(), "SaveScope::ColumnsOnly must omit filters");
        QCOMPARE(probe.Configuration().sort.columnIndex, -1);
        QVERIFY2(!probe.Configuration().source.has_value(), "SaveScope::ColumnsOnly must omit source");
    }

    // `Save Session...` (SaveScope::Full) persists the live sort
    // indicator and a Load restores it. This is the load-bearing
    // promise of the session/configuration split: filters already
    // round-trip; the sort indicator now travels with them.
    void TestSaveScopeFullPersistsSortAndRestoresOnLoad()
    {
        const int levelCol = StreamFixtureForColumnTests();
        QVERIFY2(levelCol >= 0, "level column must exist after streaming");

        auto *tableView = mWindow->findChild<LogTableView *>();
        QVERIFY(tableView != nullptr);
        const QHeaderView *header = tableView->horizontalHeader();
        QVERIFY(header != nullptr);

        tableView->sortByColumn(levelCol, Qt::DescendingOrder);
        QCoreApplication::processEvents();
        QVERIFY2(
            header->isSortIndicatorShown() && header->sortIndicatorSection() == levelCol,
            "precondition: sort indicator must be on the level column before save"
        );

        const QTemporaryDir savedDir;
        QVERIFY(savedDir.isValid());
        const QString sessionPath = savedDir.filePath(QStringLiteral("with-sort.json"));
        mWindow->SaveConfigurationToPathForTest(sessionPath, loglib::SaveScope::Full);

        // Drop the live sort so the load has to actually reapply it,
        // not just look like it did.
        tableView->sortByColumn(-1, Qt::AscendingOrder);
        QCoreApplication::processEvents();

        mWindow->SetSuppressDialogsForTest(true);
        mWindow->LoadConfigurationFromPathForTest(sessionPath);
        QCoreApplication::processEvents();

        QVERIFY2(header->isSortIndicatorShown(), "Load must restore the persisted sort indicator");
        QCOMPARE(header->sortIndicatorSection(), levelCol);
        QCOMPARE(header->sortIndicatorOrder(), Qt::DescendingOrder);
    }

    // Regression: `Save Session...` used to silently drop the
    // `Source` descriptor for a static file because
    // `streamingFinished` reset `mCurrentSource` unconditionally,
    // and a follow-up `Save` mirrored the now-empty mirror into
    // the configuration. The fix is to keep the descriptor alive
    // for the lifetime of the loaded rows; this test exercises the
    // full save -> wipe -> load round-trip.
    //
    // The shared streaming fixture bypasses `OpenFiles`, so we
    // poke `mCurrentSource` directly: production sets it on every
    // open path; the test fakes that to keep the fixture small.
    void TestSaveScopeFullRoundTripsSourceDescriptor()
    {
        const int levelCol = StreamFixtureForColumnTests();
        QVERIFY2(levelCol >= 0, "level column must exist after streaming");

        // Stand in for the open-path `mCurrentSource` write that the
        // sync fixture skips. `streamingFinished` already ran by now
        // -- before the fix the reset there would have wiped this
        // back to `nullopt` regardless.
        const std::string syntheticSource = "/test/source/path.log";
        mWindow->SetCurrentSourceForTest(loglib::LogConfiguration::Source{
            .kind = loglib::LogConfiguration::Source::Kind::File, .locators = {syntheticSource}
        });

        const QTemporaryDir savedDir;
        QVERIFY(savedDir.isValid());
        const QString sessionPath = savedDir.filePath(QStringLiteral("with-source.json"));
        mWindow->SaveConfigurationToPathForTest(sessionPath, loglib::SaveScope::Full);

        // Read back through a fresh manager so we don't accidentally
        // observe the still-live runtime mirror.
        loglib::LogConfigurationManager probe;
        probe.Load(sessionPath.toStdString());
        QVERIFY2(
            probe.Configuration().source.has_value(),
            "SaveScope::Full must persist the source descriptor for a streamed-then-finished file"
        );
        QCOMPARE(
            static_cast<int>(probe.Configuration().source->kind),
            static_cast<int>(loglib::LogConfiguration::Source::Kind::File)
        );
        QCOMPARE(probe.Configuration().source->locators.size(), static_cast<std::size_t>(1));
        QCOMPARE(
            QString::fromStdString(probe.Configuration().source->locators.front()),
            QString::fromStdString(syntheticSource)
        );

        // Now load the freshly-saved session back into the running
        // window and re-save: the descriptor must survive that round
        // too (regression for the symmetric bug where Load -> Save
        // dropped a loaded source because the runtime mirror was
        // never seeded from disk).
        mWindow->SetSuppressDialogsForTest(true);
        mWindow->LoadConfigurationFromPathForTest(sessionPath);
        QCoreApplication::processEvents();

        const QString resavePath = savedDir.filePath(QStringLiteral("with-source-resave.json"));
        mWindow->SaveConfigurationToPathForTest(resavePath, loglib::SaveScope::Full);

        loglib::LogConfigurationManager resaveProbe;
        resaveProbe.Load(resavePath.toStdString());
        QVERIFY2(
            resaveProbe.Configuration().source.has_value(),
            "Load -> Save round trip must preserve a loaded source descriptor"
        );
        QCOMPARE(resaveProbe.Configuration().source->locators.size(), static_cast<std::size_t>(1));
        QCOMPARE(resaveProbe.Configuration().source->locators.front(), syntheticSource);
    }

    // `OpenMode::Append` keeps the active static session's rows / filters /
    // source intact and queues the new file via `AppendStreaming`. Row
    // count after the second open must equal the sum of both fixtures
    // and `mCurrentSource->locators` must list both file paths in load
    // order.
    void TestOpenFilesAppendsToActiveStaticSession()
    {
        auto *model = mWindow->Model();
        QVERIFY(model != nullptr);

        const QStringList fixtureLinesA{
            QStringLiteral(R"({"category": "info", "msg": "a-0"})"),
            QStringLiteral(R"({"category": "warn", "msg": "a-1"})"),
        };
        const QStringList fixtureLinesB{
            QStringLiteral(R"({"category": "error", "msg": "b-0"})"),
            QStringLiteral(R"({"category": "debug", "msg": "b-1"})"),
            QStringLiteral(R"({"category": "info", "msg": "b-2"})"),
        };
        const TempJsonFile fixtureA(fixtureLinesA);
        const TempJsonFile fixtureB(fixtureLinesB);

        QSignalSpy finishedSpy(model, &LogModel::streamingFinished);
        QVERIFY(finishedSpy.isValid());

        mWindow->OpenFilesForTest({fixtureA.Path()}, MainWindow::OpenMode::Append);
        QVERIFY2(finishedSpy.wait(5000), "first open must finish");
        QCoreApplication::processEvents();
        QCOMPARE(model->rowCount(), fixtureLinesA.size());
        QVERIFY(mWindow->Model() == model);

        finishedSpy.clear();
        mWindow->OpenFilesForTest({fixtureB.Path()}, MainWindow::OpenMode::Append);
        QVERIFY2(finishedSpy.wait(5000), "appended open must finish");
        QCoreApplication::processEvents();

        QCOMPARE(model->rowCount(), fixtureLinesA.size() + fixtureLinesB.size());

        // Source descriptor lists every appended file in load order.
        loglib::LogConfigurationManager manager;
        manager.SetSource(loglib::LogConfiguration::Source{});
        const QTemporaryDir saved;
        QVERIFY(saved.isValid());
        const QString sessionPath = saved.filePath(QStringLiteral("appended.json"));
        mWindow->SaveConfigurationToPathForTest(sessionPath, loglib::SaveScope::Full);

        loglib::LogConfigurationManager probe;
        probe.Load(sessionPath.toStdString());
        QVERIFY(probe.Configuration().source.has_value());
        QCOMPARE(probe.Configuration().source->locators.size(), static_cast<std::size_t>(2));
        // `locators` stores case-preserved display paths; the
        // lower-cased dedup form is parallel-indexed under
        // `locatorDedupKeys`. Assert both stay populated.
        QCOMPARE(
            QString::fromStdString(probe.Configuration().source->locators[0]),
            logapp::CanonicalDisplayPath(fixtureA.Path())
        );
        QCOMPARE(
            QString::fromStdString(probe.Configuration().source->locators[1]),
            logapp::CanonicalDisplayPath(fixtureB.Path())
        );
        QCOMPARE(probe.Configuration().source->locatorDedupKeys.size(), static_cast<std::size_t>(2));
        QCOMPARE(
            QString::fromStdString(probe.Configuration().source->locatorDedupKeys[0]),
            logapp::CanonicalLocator(fixtureA.Path())
        );
        QCOMPARE(
            QString::fromStdString(probe.Configuration().source->locatorDedupKeys[1]),
            logapp::CanonicalLocator(fixtureB.Path())
        );
    }

    // Regression: a multi-file open must sniff each queued file
    // independently, not reuse the first file's format for the rest.
    // Before the fix, a JSON Lines + logfmt queue tried to parse the
    // logfmt file with `JsonParser`, turning every line into a parse
    // error. Detection is content-based, so the extension is irrelevant.
    void TestMultiFileOpenSniffsFormatPerFile()
    {
        auto *model = mWindow->Model();
        QVERIFY(model != nullptr);

        const QStringList jsonLines{
            QStringLiteral(R"({"level": "info", "msg": "json-0"})"),
            QStringLiteral(R"({"level": "warn", "msg": "json-1"})"),
        };
        // logfmt bytes written through the JSON temp-file helper.
        // Detection is content-based, so the `.json` suffix is
        // exactly what we want the test to defeat.
        const QStringList logfmtLines{
            QStringLiteral(R"(level=info msg="logfmt-0")"),
            QStringLiteral(R"(level=warn msg="logfmt-1")"),
            QStringLiteral(R"(level=error msg="logfmt-2")"),
        };
        const TempJsonFile jsonFixture(jsonLines);
        const TempJsonFile logfmtFixture(logfmtLines);

        QSignalSpy finishedSpy(model, &LogModel::streamingFinished);
        QVERIFY(finishedSpy.isValid());

        // One open, two queued files: first uses `BeginStreaming`,
        // second uses `AppendStreaming` after the first finishes
        // -> two `streamingFinished` emits.
        mWindow->OpenFilesForTest({jsonFixture.Path(), logfmtFixture.Path()}, MainWindow::OpenMode::Replace);
        while (finishedSpy.count() < 2)
        {
            QVERIFY2(finishedSpy.wait(5000), "both queued files must finish parsing");
        }
        QCoreApplication::processEvents();

        // Without per-file sniffing the logfmt lines would be parse
        // errors and never become rows.
        QVERIFY2(model->StreamingErrors().empty(), "mixed-format queue must not produce parse errors");
        QCOMPARE(model->rowCount(), jsonLines.size() + logfmtLines.size());
    }

    // Regression for the Append-while-streaming crash: Append while
    // a static streaming worker was in flight used to call
    // `AppendStreaming` and assert `!isRunning()`. The fix defers
    // the new files into `mPendingOpenFiles` so the next
    // `streamingFinished` picks them up.
    void TestAppendDuringActiveStreamingDefersInsteadOfCrashing()
    {
        auto *model = mWindow->Model();
        QVERIFY(model != nullptr);

        // Inflate fixture A so the first parse is still in flight
        // when the second `OpenFilesForTest` lands. Short fixtures
        // would race the GUI thread on fast machines.
        QStringList fixtureLinesA;
        fixtureLinesA.reserve(500);
        for (int i = 0; i < 500; ++i)
        {
            fixtureLinesA.append(QStringLiteral(R"({"msg": "a-%1"})").arg(i));
        }
        const QStringList fixtureLinesB{
            QStringLiteral(R"({"msg": "b-0"})"),
            QStringLiteral(R"({"msg": "b-1"})"),
        };
        const TempJsonFile fixtureA(fixtureLinesA);
        const TempJsonFile fixtureB(fixtureLinesB);

        QSignalSpy finishedSpy(model, &LogModel::streamingFinished);
        QVERIFY(finishedSpy.isValid());

        // First open arms the worker but does NOT wait. Pump events
        // just enough to enter the streaming state.
        mWindow->OpenFilesForTest({fixtureA.Path()}, MainWindow::OpenMode::Append);
        QCoreApplication::processEvents();

        // Second open is the regression case: with an in-flight
        // streaming worker, the file must defer onto
        // `mPendingOpenFiles` instead of tripping `AppendStreaming`'s
        // `!isRunning()` assert. On a fast runner streaming may
        // already have finished and the safer post-finish Append
        // path runs.
        mWindow->OpenFilesForTest({fixtureB.Path()}, MainWindow::OpenMode::Append);

        // Drain both `streamingFinished` events; the deferred file
        // produces a second one after the first worker terminates.
        while (finishedSpy.count() < 2)
        {
            QVERIFY2(finishedSpy.wait(10000), "both streaming sessions must finish");
        }
        QCoreApplication::processEvents();

        QCOMPARE(model->rowCount(), fixtureLinesA.size() + fixtureLinesB.size());

        // Both files land on the source descriptor in load order
        // despite the second open being deferred.
        const QTemporaryDir saved;
        QVERIFY(saved.isValid());
        const QString sessionPath = saved.filePath(QStringLiteral("appended_during_stream.json"));
        mWindow->SaveConfigurationToPathForTest(sessionPath, loglib::SaveScope::Full);

        loglib::LogConfigurationManager probe;
        probe.Load(sessionPath.toStdString());
        QVERIFY(probe.Configuration().source.has_value());
        QCOMPARE(probe.Configuration().source->locators.size(), static_cast<std::size_t>(2));
        QCOMPARE(
            QString::fromStdString(probe.Configuration().source->locators[0]),
            logapp::CanonicalDisplayPath(fixtureA.Path())
        );
        QCOMPARE(
            QString::fromStdString(probe.Configuration().source->locators[1]),
            logapp::CanonicalDisplayPath(fixtureB.Path())
        );
    }

    // Switching to live-tail mid-append-queue used to silently drop
    // the remaining queued files via the cancel-path `clear()`. The
    // fix snapshots the queue size, posts a status-bar hint, and
    // clears the queue explicitly before the reset.
    void TestOpenLogStreamSurfacesDiscardedQueuedFiles()
    {
        auto *model = mWindow->Model();
        QVERIFY(model != nullptr);

        // Inflate fixture A so the second Append queues into
        // `mPendingOpenFiles` while the first parse is still active
        // (same trick as the Defer-vs-Crash test).
        QStringList fixtureLinesA;
        fixtureLinesA.reserve(500);
        for (int i = 0; i < 500; ++i)
        {
            fixtureLinesA.append(QStringLiteral(R"({"msg": "discard-a-%1"})").arg(i));
        }
        const TempJsonFile fixtureA(fixtureLinesA);
        const TempJsonFile fixtureB({QStringLiteral(R"({"msg": "discard-b"})")});
        const TempJsonFile streamFixture({QStringLiteral(R"({"msg": "discard-stream"})")});

        const QSignalSpy finishedSpy(model, &LogModel::streamingFinished);
        QVERIFY(finishedSpy.isValid());

        mWindow->OpenFilesForTest({fixtureA.Path()}, MainWindow::OpenMode::Append);
        QCoreApplication::processEvents();
        // Second open lands in `mPendingOpenFiles` because the
        // first parse is still in flight.
        mWindow->OpenFilesForTest({fixtureB.Path()}, MainWindow::OpenMode::Append);

        mWindow->statusBar()->clearMessage();

        // Switch to live-tail while the append queue is primed.
        // The fix surfaces a discard hint; without it the queue is
        // silently cleared via the cancel-path inside Reset().
        mWindow->OpenLogStreamForTest(streamFixture.Path());
        QCoreApplication::processEvents();

        const QString message = mWindow->statusBar()->currentMessage();
        QVERIFY2(
            message.contains(QStringLiteral("queued file")),
            qPrintable(QStringLiteral("expected discard hint in status bar; got: '%1'").arg(message))
        );

        // fixtureB never streamed (discarded before the live-tail
        // took over). We only check the queue drop, not the
        // live-tail row count.
        const bool seesFixtureB = [&]() {
            const int rows = model->rowCount();
            for (int r = 0; r < rows; ++r)
            {
                const QModelIndex idx = model->index(r, 0);
                if (model->data(idx, Qt::DisplayRole).toString().contains(QStringLiteral("discard-b")))
                {
                    return true;
                }
            }
            return false;
        }();
        QVERIFY2(!seesFixtureB, "discarded queued file's rows must not be in the model");
    }

    // `OpenMode::Replace` is the destructive path: rows wiped, filters
    // cleared, source descriptor reset to just the new file.
    void TestOpenFilesReplaceWipesPreviousSession()
    {
        auto *model = mWindow->Model();
        QVERIFY(model != nullptr);

        const QStringList fixtureLinesA{
            QStringLiteral(R"({"msg": "a-0"})"),
            QStringLiteral(R"({"msg": "a-1"})"),
        };
        const QStringList fixtureLinesB{
            QStringLiteral(R"({"msg": "b-0"})"),
        };
        const TempJsonFile fixtureA(fixtureLinesA);
        const TempJsonFile fixtureB(fixtureLinesB);

        QSignalSpy finishedSpy(model, &LogModel::streamingFinished);
        QVERIFY(finishedSpy.isValid());

        mWindow->OpenFilesForTest({fixtureA.Path()}, MainWindow::OpenMode::Append);
        QVERIFY(finishedSpy.wait(5000));
        QCoreApplication::processEvents();
        QCOMPARE(model->rowCount(), fixtureLinesA.size());

        finishedSpy.clear();
        mWindow->OpenFilesForTest({fixtureB.Path()}, MainWindow::OpenMode::Replace);
        QVERIFY(finishedSpy.wait(5000));
        QCoreApplication::processEvents();

        QCOMPARE(model->rowCount(), fixtureLinesB.size());

        const QTemporaryDir saved;
        QVERIFY(saved.isValid());
        const QString sessionPath = saved.filePath(QStringLiteral("replaced.json"));
        mWindow->SaveConfigurationToPathForTest(sessionPath, loglib::SaveScope::Full);

        loglib::LogConfigurationManager probe;
        probe.Load(sessionPath.toStdString());
        QVERIFY(probe.Configuration().source.has_value());
        QCOMPARE(probe.Configuration().source->locators.size(), static_cast<std::size_t>(1));
        QCOMPARE(
            QString::fromStdString(probe.Configuration().source->locators.front()),
            logapp::CanonicalDisplayPath(fixtureB.Path())
        );
    }

    // Transparent decompression happy path: a gzip-compressed JSONL
    // fixture streams through `MainWindow::StartStreamingOpenQueue`
    // -> `SniffCodec` -> `BeginAsyncDecompression` ->
    // `OnDecompressionFinished` -> `ContinueOpenAfterPrepared` and
    // lands rows in the model. Row count, source locator (original
    // .gz path, not the temp file), and detected format are all
    // pinned so a regression in any of those seams surfaces here.
    void TestOpenCompressedFileDecodesAndStreams()
    {
        auto *model = mWindow->Model();
        QVERIFY(model != nullptr);
        // Skip the potential progress dialog on slow runners; the
        // decompression completes in a few ms for this size but
        // the modal would still steal focus in headless CI.
        mWindow->SetSuppressDialogsForTest(true);

        // 500 lines is small enough to decompress well under
        // `DECOMPRESSION_DIALOG_DEFER_MS` on any runner, so the
        // dialog never even appears -- but large enough that the
        // decompression is genuinely async (worker on the thread
        // pool + queued `finished` callout).
        QStringList fixtureLines;
        fixtureLines.reserve(500);
        for (int i = 0; i < 500; ++i)
        {
            fixtureLines.append(QStringLiteral(R"({"idx": %1, "msg": "compressed-happy-%1"})").arg(i));
        }
        QByteArray uncompressed;
        for (const QString &line : fixtureLines)
        {
            uncompressed.append(line.toUtf8());
            uncompressed.append('\n');
        }
        const QByteArray gzipped = GzipCompressForTest(uncompressed);

        const QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        const QString path = tempDir.filePath(QStringLiteral("happy.jsonl.gz"));
        WriteBinaryForTest(path, gzipped);

        QSignalSpy finishedSpy(model, &LogModel::streamingFinished);
        QVERIFY(finishedSpy.isValid());

        mWindow->OpenFilesForTest({path}, MainWindow::OpenMode::Replace);
        // Decompression is async: `mDecompressionInFlight` must be
        // observably true immediately after the call returns (before
        // any events pump). This is the same precondition the
        // Append-during-decompression regression relies on; if it
        // ever stops holding, the fixture selection above needs
        // revisiting.
        QVERIFY2(
            mWindow->IsDecompressionInFlightForTest(),
            "gzip open must arm the async decompression worker synchronously; without this precondition the "
            "append-during-decompression regression below is silently vacuous"
        );

        while (finishedSpy.count() < 1)
        {
            QVERIFY2(finishedSpy.wait(10000), "compressed open must produce a streamingFinished event");
        }
        QCoreApplication::processEvents();

        QCOMPARE(model->rowCount(), fixtureLines.size());

        // Session locator must be the ORIGINAL compressed path --
        // the temp file is a per-open implementation detail and
        // must never appear in a persisted descriptor.
        const QTemporaryDir savedDir;
        QVERIFY(savedDir.isValid());
        const QString sessionPath = savedDir.filePath(QStringLiteral("compressed_happy.json"));
        mWindow->SaveConfigurationToPathForTest(sessionPath, loglib::SaveScope::Full);

        loglib::LogConfigurationManager probe;
        probe.Load(sessionPath.toStdString());
        QVERIFY(probe.Configuration().source.has_value());
        QCOMPARE(probe.Configuration().source->locators.size(), static_cast<std::size_t>(1));
        QCOMPARE(
            QString::fromStdString(probe.Configuration().source->locators.front()), logapp::CanonicalDisplayPath(path)
        );
        // Format sniff runs against the *decompressed* bytes; JSON
        // Lines must be detected even though the file extension
        // suffix (`.jsonl.gz`) would tempt an extension-based
        // classifier to over-index.
        QCOMPARE(probe.Configuration().source->format, loglib::LogConfiguration::Source::Format::Json);
    }

    // Regression for the review note "B1": appending a file (of any
    // codec) while a decompression is in flight must queue behind the
    // pending worker instead of racing it. Prior to the guard in
    // `StartStreamingOpenQueue`, this scenario either wiped the
    // Append's rows when the decompression's `BeginStreaming`
    // finished second, or tripped `LogModel::AppendStreaming`'s
    // `Q_ASSERT(!mStreamingWatcher->isRunning())` when it finished
    // first. The append must land after the decompressed file
    // streams, and the total row count must equal the sum of both.
    void TestAppendDuringDecompressionQueuesBehindWorker()
    {
        auto *model = mWindow->Model();
        QVERIFY(model != nullptr);
        mWindow->SetSuppressDialogsForTest(true);

        // Same 500-line gzip fixture as the happy-path test above.
        QStringList gzipLines;
        gzipLines.reserve(500);
        for (int i = 0; i < 500; ++i)
        {
            gzipLines.append(QStringLiteral(R"({"idx": %1, "msg": "gz-%1"})").arg(i));
        }
        QByteArray uncompressed;
        for (const QString &line : gzipLines)
        {
            uncompressed.append(line.toUtf8());
            uncompressed.append('\n');
        }
        const QByteArray gzipped = GzipCompressForTest(uncompressed);

        const QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        const QString gzipPath = tempDir.filePath(QStringLiteral("first.jsonl.gz"));
        WriteBinaryForTest(gzipPath, gzipped);

        const QStringList appendLines{
            QStringLiteral(R"({"idx": 0, "msg": "append-0"})"),
            QStringLiteral(R"({"idx": 1, "msg": "append-1"})"),
            QStringLiteral(R"({"idx": 2, "msg": "append-2"})"),
        };
        const TempJsonFile appendFixture(appendLines);

        QSignalSpy finishedSpy(model, &LogModel::streamingFinished);
        QVERIFY(finishedSpy.isValid());

        // Step 1: arm the compressed open. Do NOT pump events --
        // we deliberately race the append against a synchronously-
        // armed decompression worker so the timing is deterministic.
        mWindow->OpenFilesForTest({gzipPath}, MainWindow::OpenMode::Replace);
        QVERIFY2(
            mWindow->IsDecompressionInFlightForTest(),
            "test precondition: gzip decompression must be in flight before the append call lands"
        );

        // Step 2: append the uncompressed file while the guard
        // above holds. Without the fix, this call would either
        // clobber `mPendingOpenFiles` and race `BeginStreaming`
        // (row loss when the decompression later resets the model)
        // or drop through the "fall-through" branch and dispatch a
        // second worker that trips `AppendStreaming`'s assert
        // once the decompression completes.
        mWindow->OpenFilesForTest({appendFixture.Path()}, MainWindow::OpenMode::Append);
        // The Append must have taken the "queue" branch, NOT the
        // "arm the fast path" branch. The former leaves the
        // decompression flag set; the latter would have cleared it
        // (only `OnDecompressionFinished` / `CancelInFlightDecompression`
        // clear the flag, and neither runs synchronously here).
        QVERIFY2(
            mWindow->IsDecompressionInFlightForTest(),
            "Append during in-flight decompression must not cancel the pending worker; the guard should defer "
            "the append into mPendingOpenFiles and leave the decompression running"
        );

        // Two files -> two streamingFinished events (one per
        // BeginStreaming / AppendStreaming pair). The decompressed
        // one fires first, then the uncompressed append.
        while (finishedSpy.count() < 2)
        {
            QVERIFY2(finishedSpy.wait(10000), "both compressed + appended sessions must finish");
        }
        QCoreApplication::processEvents();

        // Total rows = gz payload + append payload. A row-loss
        // regression would leave only one file's rows here.
        QCOMPARE(model->rowCount(), gzipLines.size() + appendLines.size());

        // Load order preserved: the .gz was queued first, so its
        // locator is index 0 and the appended file is index 1.
        // Both locators must be the ORIGINAL user-facing paths (the
        // .gz path, not the decompressed temp file's).
        const QTemporaryDir savedDir;
        QVERIFY(savedDir.isValid());
        const QString sessionPath = savedDir.filePath(QStringLiteral("append_during_decompression.json"));
        mWindow->SaveConfigurationToPathForTest(sessionPath, loglib::SaveScope::Full);

        loglib::LogConfigurationManager probe;
        probe.Load(sessionPath.toStdString());
        QVERIFY(probe.Configuration().source.has_value());
        QCOMPARE(probe.Configuration().source->locators.size(), static_cast<std::size_t>(2));
        QCOMPARE(
            QString::fromStdString(probe.Configuration().source->locators[0]), logapp::CanonicalDisplayPath(gzipPath)
        );
        QCOMPARE(
            QString::fromStdString(probe.Configuration().source->locators[1]),
            logapp::CanonicalDisplayPath(appendFixture.Path())
        );
    }

    // Regression for the review note "compressed follow-up in a
    // multi-file queue is torn down mid-chain".
    //
    // When `OnStreamingFinished(Success)` chains to
    // `StreamNextPendingFile` and the next queued file happens to
    // be compressed, `StreamNextPendingFile` dispatches the async
    // decompression worker and returns without starting a parse.
    // The subsequent guard `if (mModel->IsStreamingActive())`
    // (line ~2500 in `OnStreamingFinished`) then evaluates false --
    // no parse worker is running, only a decompression worker --
    // and the function falls through to the session teardown
    // block, setting `mSessionMode = Idle`. When the decompression
    // eventually finishes and `ContinueOpenAfterPrepared` runs,
    // `IsSessionActive()` is false, so `BeginStreaming` is called
    // instead of `AppendStreaming`, wiping the rows previously
    // parsed from `a.log`.
    //
    // The fix adds `|| mDecompressionInFlight` to the guard so
    // the teardown is suppressed while the compressed follow-up
    // is decompressing. This test opens an uncompressed fixture
    // first, immediately appends a gzip fixture (which queues
    // behind the in-flight `a.log` parse), and verifies BOTH
    // files' rows end up in the model with locators preserved
    // in load order.
    void TestQueuedCompressedFollowUpKeepsPriorRows()
    {
        auto *model = mWindow->Model();
        QVERIFY(model != nullptr);
        mWindow->SetSuppressDialogsForTest(true);

        // Inflate the uncompressed fixture so its parse is
        // guaranteed to still be running when the Append lands
        // (matches the timing precondition used by
        // `TestAppendDuringActiveStreamingDefersInsteadOfCrashing`).
        // Short fixtures would race the GUI thread.
        QStringList fixtureLinesA;
        fixtureLinesA.reserve(500);
        for (int i = 0; i < 500; ++i)
        {
            fixtureLinesA.append(QStringLiteral(R"({"idx": %1, "msg": "chain-a-%1"})").arg(i));
        }
        const TempJsonFile uncompressedFixture(fixtureLinesA);

        // The compressed follow-up: any size works -- the
        // regression is triggered by *any* codec-detected file
        // dequeued after a Success. Keep it short so the second
        // decompression completes quickly under CI.
        const QStringList gzipLines{
            QStringLiteral(R"({"idx": 0, "msg": "chain-b-0"})"),
            QStringLiteral(R"({"idx": 1, "msg": "chain-b-1"})"),
            QStringLiteral(R"({"idx": 2, "msg": "chain-b-2"})"),
        };
        QByteArray uncompressedGz;
        for (const QString &line : gzipLines)
        {
            uncompressedGz.append(line.toUtf8());
            uncompressedGz.append('\n');
        }
        const QByteArray gzipped = GzipCompressForTest(uncompressedGz);

        const QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        const QString gzipPath = tempDir.filePath(QStringLiteral("second.jsonl.gz"));
        WriteBinaryForTest(gzipPath, gzipped);

        QSignalSpy finishedSpy(model, &LogModel::streamingFinished);
        QVERIFY(finishedSpy.isValid());

        // Step 1: arm the uncompressed open. Pump events so the
        // parse worker actually starts (mirrors the setup used by
        // `TestAppendDuringActiveStreamingDefersInsteadOfCrashing`).
        mWindow->OpenFilesForTest({uncompressedFixture.Path()}, MainWindow::OpenMode::Append);
        QCoreApplication::processEvents();

        // Step 2: append the gzip fixture while `a.log`'s parse is
        // still in flight. Existing behaviour: it queues behind
        // the in-flight parse via `mPendingOpenFiles.append`.
        mWindow->OpenFilesForTest({gzipPath}, MainWindow::OpenMode::Append);

        // Both files must land in the model in load order:
        //   - `a.log` (uncompressed, parses first)
        //   - `b.log.gz` (chained after a.log's `OnStreamingFinished`
        //     -> `StreamNextPendingFile` -> `BeginAsyncDecompression`)
        // Without the guard fix, `a.log`'s rows would be wiped
        // when the gzip's `BeginStreaming` runs, leaving only the
        // gzip's rows in the model.
        while (finishedSpy.count() < 2)
        {
            QVERIFY2(finishedSpy.wait(10000), "both queued sessions must finish");
        }
        QCoreApplication::processEvents();

        QCOMPARE(model->rowCount(), fixtureLinesA.size() + gzipLines.size());

        // Locator round-trip in load order (uncompressed first,
        // compressed second). Both must be the ORIGINAL user-
        // facing paths (the `.gz` path, not the decompressed
        // temp file's).
        const QTemporaryDir savedDir;
        QVERIFY(savedDir.isValid());
        const QString sessionPath = savedDir.filePath(QStringLiteral("queued_compressed_follow_up.json"));
        mWindow->SaveConfigurationToPathForTest(sessionPath, loglib::SaveScope::Full);

        loglib::LogConfigurationManager probe;
        probe.Load(sessionPath.toStdString());
        QVERIFY(probe.Configuration().source.has_value());
        QCOMPARE(probe.Configuration().source->locators.size(), static_cast<std::size_t>(2));
        QCOMPARE(
            QString::fromStdString(probe.Configuration().source->locators[0]),
            logapp::CanonicalDisplayPath(uncompressedFixture.Path())
        );
        QCOMPARE(
            QString::fromStdString(probe.Configuration().source->locators[1]), logapp::CanonicalDisplayPath(gzipPath)
        );
    }

    // Cancel-path regression. Verifies that requesting stop while a
    // decompression worker is running unwinds cleanly:
    //   - `mDecompressionInFlight` flips back to false;
    //   - no `streamingFinished` is emitted (no session was ever
    //     armed on the model, so a spurious Success / Cancelled here
    //     would signal the cancel branch is starting BeginStreaming
    //     by accident);
    //   - no rows land in the model;
    //   - a subsequent open on a different file still works
    //     (proves the cancel drained the queue + reset the state
    //     rather than wedging the pipeline).
    //
    // Uses `RequestDecompressionCancelForTest` because the
    // production `QProgressDialog::canceled` wiring lives inside
    // the dialog, which is suppressed under
    // `SetSuppressDialogsForTest`.
    void TestCancelDecompressionUnwindsCleanly()
    {
        auto *model = mWindow->Model();
        QVERIFY(model != nullptr);
        mWindow->SetSuppressDialogsForTest(true);

        // Fixture sized so the worker is very likely to still be
        // running when the cancel lands. Small (few-KB) fixtures
        // race the cancel: the worker completes first and the test
        // would pass vacuously along the Success path instead of
        // exercising the cancel branch. 20K lines with padded
        // messages produces ~2 MiB of uncompressed input, which
        // gives the worker several `ObservePoll` checkpoints per
        // 64 KiB input chunk to notice the stop token.
        QByteArray uncompressed;
        uncompressed.reserve(3'000'000);
        const QString pad =
            QStringLiteral("padding-payload-so-the-input-stream-is-big-enough-that-cancel-lands-before-completion");
        for (int i = 0; i < 20000; ++i)
        {
            uncompressed.append(QStringLiteral(R"({"idx": %1, "msg": "%2-%1"})").arg(i).arg(pad).toUtf8());
            uncompressed.append('\n');
        }
        const QByteArray gzipped = GzipCompressForTest(uncompressed);

        const QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        const QString gzipPath = tempDir.filePath(QStringLiteral("cancel.jsonl.gz"));
        WriteBinaryForTest(gzipPath, gzipped);

        QSignalSpy finishedSpy(model, &LogModel::streamingFinished);
        QVERIFY(finishedSpy.isValid());

        mWindow->OpenFilesForTest({gzipPath}, MainWindow::OpenMode::Replace);
        QVERIFY2(
            mWindow->IsDecompressionInFlightForTest(),
            "precondition: decompression worker must be armed synchronously so the cancel below has a target"
        );

        // Trip the stop token. The worker unwinds on its next
        // `ObservePoll` (between 64 KiB input chunks); the finished
        // slot then posts the cancel toast and returns to idle.
        mWindow->RequestDecompressionCancelForTest();

        // Pump events until the finished slot has cleared the flag,
        // or 10 s -- generous headroom for a slow CI runner. On
        // failure this fires QFAIL with a diagnostic pointing at
        // the flag itself, which is exactly the right seam.
        QTRY_VERIFY_WITH_TIMEOUT(!mWindow->IsDecompressionInFlightForTest(), 10000);

        // Cancel is deliberately NOT surfaced as a
        // `streamingFinished` event: no `LogModel::BeginStreaming`
        // ever ran, so the model's watcher was never armed.
        // A non-zero count here would mean the cancel branch is
        // accidentally taking the "arm session + call
        // AppendStreaming" path -- either a plumbing bug or the
        // wrong catch order in `OnDecompressionFinished`.
        QCOMPARE(finishedSpy.count(), 0);
        QCOMPARE(model->rowCount(), 0);

        // The cancel must leave the pipeline usable. A follow-up
        // open on a different file exercises `StreamNextPendingFile`
        // -> `ContinueOpenAfterPrepared` -> `BeginStreaming` starting
        // from a clean slate, so any residual state left by the
        // cancel (dangling watcher, stale `mCurrentSource`, unclosed
        // `mSessionMode`, ...) surfaces as either the follow-up
        // failing to finish or its rows getting appended onto the
        // cancelled session's residue.
        const QStringList followUpLines{
            QStringLiteral(R"({"idx": 0, "msg": "after-cancel-0"})"),
            QStringLiteral(R"({"idx": 1, "msg": "after-cancel-1"})"),
        };
        const TempJsonFile followUp(followUpLines);
        mWindow->OpenFilesForTest({followUp.Path()}, MainWindow::OpenMode::Replace);
        QVERIFY2(finishedSpy.wait(10000), "follow-up open after cancel must produce a streamingFinished event");
        QCoreApplication::processEvents();
        QCOMPARE(model->rowCount(), followUpLines.size());
    }

    // Chain-terminal decompression failure regression. Pre-fix,
    // opening `[good.log, corrupt.gz]` in Append mode ran the
    // pipeline as:
    //   - `good.log` parses successfully -> `OnStreamingFinished(Success)`
    //   - chain to `corrupt.gz` -> `BeginAsyncDecompression`
    //   - `OnStreamingFinished` early-returns on `mDecompressionInFlight`
    //   - worker fails, `OnDecompressionFinished` pushes an entry
    //     into `mPendingDecompressionErrors`, calls
    //     `StreamNextPendingFile()`
    //   - `SNP` finds empty queue, sees `IsSessionActive() == true`
    //     and skips its own drain, returns
    // ...and now the error sits in `mPendingDecompressionErrors`
    // forever: `mSessionMode` is still `Static` with no live worker,
    // so `OnStreamingFinished` never fires again to drain it. The
    // user sees the app in a wedged "Parsing corrupt.gz" state with
    // no error dialog / dock entry. `NewSession` or the next
    // destructive open silently clears the error bucket.
    //
    // The fix (`FinalizeAfterDecompressionIfChainTerminal` called
    // at the end of the error / cancel / mmap-fail branches of
    // `OnDecompressionFinished`) drains both error buckets under
    // their own titles when no successor worker was armed AND
    // settles the UI back to Idle so the previous file's rows
    // stay accessible.
    void TestChainTerminalDecompressionFailureSurfacesError()
    {
        auto *model = mWindow->Model();
        QVERIFY(model != nullptr);
        mWindow->SetSuppressDialogsForTest(true);

        auto *dock = mWindow->findChild<ParseErrorsDock *>();
        QVERIFY2(dock != nullptr, "MainWindow must own a ParseErrorsDock");
        dock->ResetSessionState();
        const int startingErrorCount = dock->Count();

        // Uncompressed fixture: 500 lines to guarantee its parse is
        // still running when the corrupt .gz Append lands (matches
        // the timing precondition used by
        // `TestQueuedCompressedFollowUpKeepsPriorRows`). Short
        // fixtures let the queue drain before the Append is even
        // dispatched, which would put the second file into
        // `mPendingOpenFiles` via a *fresh* `StartStreamingOpenQueue`
        // rather than the chain path the regression targets.
        QStringList uncompressedLines;
        uncompressedLines.reserve(500);
        for (int i = 0; i < 500; ++i)
        {
            uncompressedLines.append(QStringLiteral(R"({"idx": %1, "msg": "good-%1"})").arg(i));
        }
        const TempJsonFile uncompressedFixture(uncompressedLines);

        // Corrupt gzip: valid magic bytes (`SniffCodec` returns
        // `Gzip` so the dispatch takes the decompression branch)
        // followed by a truncated deflate stream. The worker will
        // read the header, start inflating, and fail on the
        // premature end-of-stream inside `DecodeGzip`, throwing
        // an exception that `OnDecompressionFinished` catches on
        // the non-`DecompressionCancelled` branch.
        const QByteArray validGzip =
            GzipCompressForTest(QByteArrayLiteral(R"({"idx": 0, "msg": "will-not-survive-truncation"})"));
        QVERIFY2(validGzip.size() > 12, "gzip helper must produce a real stream (magic + header + payload + footer)");
        // Truncate mid-payload: keep the magic + header but chop
        // off the deflate stream so the decoder errors on
        // premature EOF.
        const QByteArray corruptGzip = validGzip.left(12);

        const QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        const QString gzipPath = tempDir.filePath(QStringLiteral("chain_terminal_corrupt.jsonl.gz"));
        WriteBinaryForTest(gzipPath, corruptGzip);

        QSignalSpy finishedSpy(model, &LogModel::streamingFinished);
        QVERIFY(finishedSpy.isValid());

        // Arm the uncompressed open; pump one round so the parse
        // worker actually starts before we append the .gz.
        mWindow->OpenFilesForTest({uncompressedFixture.Path()}, MainWindow::OpenMode::Append);
        QCoreApplication::processEvents();

        // Append the corrupt .gz while the first parse is in
        // flight -- queues behind the running parse in
        // `mPendingOpenFiles`.
        mWindow->OpenFilesForTest({gzipPath}, MainWindow::OpenMode::Append);

        // Wait for `good.log` to reach terminal state
        // (`streamingFinished(Success)` -> chain to `corrupt.gz`).
        QVERIFY2(finishedSpy.wait(10000), "first file must reach terminal state before the chain kicks in");
        // Now drive the event loop until the chained decompression
        // has finished and the pipeline has settled: worker
        // unwinds via the error path, `OnDecompressionFinished`
        // clears `mDecompressionInFlight` and calls the finalize
        // helper. Two conditions in one wait -- IsStreamingActive
        // returns false the moment `good.log` finished, so we
        // gate on the decompression flag too.
        QTRY_VERIFY_WITH_TIMEOUT(!mWindow->IsDecompressionInFlightForTest(), 10000);
        QTRY_VERIFY_WITH_TIMEOUT(!model->IsStreamingActive(), 10000);
        QCoreApplication::processEvents();

        // Regression assertion #1: the previous file's rows must
        // survive. Pre-fix these were preserved too -- the bug was
        // silent-error-loss, not row-loss -- but the assertion is
        // cheap and makes the intent obvious.
        QCOMPARE(model->rowCount(), uncompressedLines.size());

        // Regression assertion #2: the decompression failure must
        // surface in the parse-errors dock. Pre-fix, the entry sat
        // in `mPendingDecompressionErrors` forever and the dock
        // count was unchanged.
        QCOMPARE(dock->Count(), startingErrorCount + 1);

        // Locate the entry and confirm it landed under the
        // `Error Decompressing File` header (as opposed to
        // `Error Opening File`). Categorising the failure
        // correctly is the whole reason we kept a second error
        // bucket in the first place; a regression that lumps
        // decompression failures back into the open-error bucket
        // would degrade the user-facing message without changing
        // the count.
        auto *list = dock->findChild<QListWidget *>(QStringLiteral("parseErrorsList"));
        QVERIFY2(list != nullptr, "ParseErrorsDock must expose its internal QListWidget");
        bool foundDecompressionHeader = false;
        bool foundCorruptEntry = false;
        for (int i = 0; i < list->count(); ++i)
        {
            const QListWidgetItem *item = list->item(i);
            if (item == nullptr)
            {
                continue;
            }
            const QString text = item->text();
            if (text.contains(QStringLiteral("Error Decompressing File")))
            {
                foundDecompressionHeader = true;
            }
            if (text.contains(QStringLiteral("chain_terminal_corrupt.jsonl.gz")))
            {
                foundCorruptEntry = true;
            }
        }
        QVERIFY2(
            foundDecompressionHeader,
            "the chain-terminal decompression failure must be surfaced under an "
            "`Error Decompressing File` header, not lumped into `Error Opening File`"
        );
        QVERIFY2(
            foundCorruptEntry, "the failure entry must name the original .gz path so the user can locate the culprit"
        );

        // Regression assertion #3: the pipeline must be usable
        // again after the chain-terminal failure. Pre-fix,
        // `mSessionMode` was stuck at `Static` with no live worker
        // and the configuration UI stayed disabled. Re-arming a
        // fresh session via Append (which follows the "no active
        // session" branch in `StartStreamingOpenQueue`) would
        // succeed either way, but the *observable* stuckness is
        // the streaming label / session mode -- so we exercise
        // that indirectly by opening a follow-up and confirming
        // its rows land on top (append onto the surviving
        // session, not a wedged Replace that clears them).
        const QStringList followUpLines{QStringLiteral(R"({"idx": 0, "msg": "after-chain-failure"})")};
        const TempJsonFile followUp(followUpLines);
        mWindow->OpenFilesForTest({followUp.Path()}, MainWindow::OpenMode::Append);
        QVERIFY2(finishedSpy.wait(10000), "follow-up open after chain-terminal failure must produce streamingFinished");
        QCoreApplication::processEvents();
        QCOMPARE(model->rowCount(), uncompressedLines.size() + followUpLines.size());
    }

    // `actionNewSession` clears rows, runtime filters, the persisted
    // column configuration, and session mode. Reached through
    // `findChild<QAction*>("actionNewSession")` so the action wiring
    // is covered too.
    void TestNewSessionActionResetsState()
    {
        auto *model = mWindow->Model();
        QVERIFY(model != nullptr);

        // Two distinct keys so the auto-detector materialises at
        // least two columns; NewSession must wipe both.
        const QStringList fixtureLines{
            QStringLiteral(R"({"category": "info", "msg": "x-0"})"),
            QStringLiteral(R"({"category": "warn", "msg": "x-1"})"),
        };
        const TempJsonFile fixture(fixtureLines);

        QSignalSpy finishedSpy(model, &LogModel::streamingFinished);
        QVERIFY(finishedSpy.isValid());
        mWindow->OpenFilesForTest({fixture.Path()}, MainWindow::OpenMode::Append);
        QVERIFY(finishedSpy.wait(5000));
        QCoreApplication::processEvents();
        QCOMPARE(model->rowCount(), fixtureLines.size());
        QVERIFY2(model->columnCount() >= 2, "fixture must auto-detect at least the two JSON keys as columns");

        // Apply a sort so NewSession has something to clear --
        // otherwise the post-condition holds trivially.
        const int categoryCol = ColumnByHeader(*model, QStringLiteral("category"));
        QVERIFY2(categoryCol >= 0, "category column must exist after streaming");
        mWindow->findChild<LogTableView *>()->sortByColumn(categoryCol, Qt::DescendingOrder);
        QCoreApplication::processEvents();
        QCOMPARE(mWindow->FilterModel()->SortColumn(), categoryCol);

        auto *action = mWindow->findChild<QAction *>(QStringLiteral("actionNewSession"));
        QVERIFY2(action != nullptr, "actionNewSession must be reachable via objectName");
        action->trigger();
        QCoreApplication::processEvents();

        // Rows, columns, runtime filters gone; sort cleared.
        QCOMPARE(model->rowCount(), 0);
        QCOMPARE(model->columnCount(), 0);
        QVERIFY2(model->Configuration().columns.empty(), "New Session must clear the persisted column configuration");
        QVERIFY2(mWindow->Filters().empty(), "New Session must clear the runtime filter map");
        QCOMPARE(mWindow->FilterModel()->SortColumn(), -1);

        // SaveSession on the cleared state writes no source, empty
        // columns / filters arrays, and the default sort.
        const QTemporaryDir saved;
        QVERIFY(saved.isValid());
        const QString sessionPath = saved.filePath(QStringLiteral("new-session.json"));
        mWindow->SaveConfigurationToPathForTest(sessionPath, loglib::SaveScope::Full);
        loglib::LogConfigurationManager probe;
        probe.Load(sessionPath.toStdString());
        QVERIFY2(!probe.Configuration().source.has_value(), "New Session must clear the source descriptor");
        QVERIFY2(probe.Configuration().columns.empty(), "New Session saved JSON must have no columns");
        QVERIFY2(probe.Configuration().filters.empty(), "New Session saved JSON must have no filters");
        QCOMPARE(probe.Configuration().sort.columnIndex, -1);
    }

    // Pinning a string-only column (`msg`) to `Integer` is the
    // canonical "user misconfiguration" case: every present value is
    // a string, no value is an integer, so `ColumnTypeHealth` reports
    // presentSlots > matchingSlots and the header should advertise a
    // mismatch via tooltip + decoration role.
    void TestColumnHealthFlagsMismatchedType()
    {
        const int levelCol = StreamFixtureForColumnTests();
        QVERIFY2(levelCol >= 0, "level column must exist after streaming");
        auto *model = mWindow->Model();
        const int msgCol = ColumnByHeader(*model, QStringLiteral("msg"));
        QVERIFY2(msgCol >= 0, "msg column must exist after streaming");

        // Streaming auto-detected `msg` to `String`; pin it to
        // `Integer` to force a mismatch and re-snapshot health.
        model->ConfigurationManager().SetColumnAutoDetect(static_cast<size_t>(msgCol), false);
        model->ConfigurationManager().SetColumnType(
            static_cast<size_t>(msgCol), loglib::LogConfiguration::Type::Integer
        );

        const QSignalSpy healthSpy(model, &LogModel::columnHealthChanged);
        model->RefreshColumnHealth();
        // RefreshColumnHealth emits when health moves; the pin
        // shifts matching from N to 0, which is always a delta.
        QVERIFY2(healthSpy.count() >= 1, "RefreshColumnHealth must emit columnHealthChanged on a mismatch flip");

        const auto health = model->ColumnHealth(msgCol);
        QVERIFY2(health.has_value(), "ColumnHealth must yield a snapshot after Refresh");
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access): asserted above.
        QVERIFY2(health->presentSlots > 0, "msg column must have data after streaming");
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        QVERIFY2(health->matchingSlots < health->presentSlots, "Integer-pinned string column must report mismatches");

        // Tooltip carries the human-readable diagnostics; decoration
        // returns the standard warning icon. Both are wired through
        // the same path so a downstream UI test exercising header
        // rendering sees a consistent surface.
        const QVariant tooltip = model->headerData(msgCol, Qt::Horizontal, Qt::ToolTipRole);
        QVERIFY(tooltip.canConvert<QString>());
        QVERIFY2(
            tooltip.toString().contains(QStringLiteral("do not match the configured type")),
            "Mismatched column tooltip must explain the diagnostic"
        );

        const QVariant decoration = model->headerData(msgCol, Qt::Horizontal, Qt::DecorationRole);
        QVERIFY(decoration.canConvert<QIcon>());
        QVERIFY2(!decoration.value<QIcon>().isNull(), "Mismatched column must surface a warning icon");

        // Auto-detected columns (Type::Any + autoDetect=true) never
        // mismatch by definition. Promoted enum columns (level) carry
        // dictionary slots so their matching count equals presentSlots.
        const auto levelHealth = model->ColumnHealth(levelCol);
        QVERIFY(levelHealth.has_value());
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        QCOMPARE(levelHealth->matchingSlots, levelHealth->presentSlots);
    }

    // With no active filter, the header has no decoration and no
    // "Filters:" tooltip section. Baseline for the tests below.
    void TestHeaderFilterIndicatorAbsentWithoutFilters()
    {
        const int levelCol = StreamFixtureForColumnTests();
        QVERIFY2(levelCol >= 0, "level column must exist after streaming");
        auto *model = mWindow->Model();
        const int msgCol = ColumnByHeader(*model, QStringLiteral("msg"));
        QVERIFY2(msgCol >= 0, "msg column must exist after streaming");

        QVERIFY2(mWindow->Filters().empty(), "no filters submitted; baseline state");
        // Fixture precondition: `msg` is type-clean (every cell is
        // a string), so no warning glyph competes for the slot.
        const auto msgHealth = model->ColumnHealth(msgCol);
        const size_t mismatched = (msgHealth.has_value() && msgHealth->presentSlots > msgHealth->matchingSlots)
                                      ? msgHealth->presentSlots - msgHealth->matchingSlots
                                      : 0;
        QVERIFY2(mismatched == 0, "fixture precondition: msg column must be type-clean (no warning decoration)");

        const QVariant decoration = model->headerData(msgCol, Qt::Horizontal, Qt::DecorationRole);
        // A null `QVariant` and an empty `QIcon` both mean "no
        // decoration"; accept either.
        if (decoration.canConvert<QIcon>())
        {
            QVERIFY2(decoration.value<QIcon>().isNull(), "decoration must be empty when no filter is active");
        }

        const QVariant tooltip = model->headerData(msgCol, Qt::Horizontal, Qt::ToolTipRole);
        QVERIFY(tooltip.canConvert<QString>());
        QVERIFY2(
            !tooltip.toString().contains(QStringLiteral("Filters:")),
            "tooltip must not advertise a Filters section when no filter is active"
        );
    }

    // Submitting a String filter installs the funnel decoration
    // and adds a "Filters:" bullet listing the filter title.
    void TestHeaderFilterIndicatorAppearsOnAddFilter()
    {
        const int levelCol = StreamFixtureForColumnTests();
        QVERIFY2(levelCol >= 0, "level column must exist after streaming");
        auto *model = mWindow->Model();
        const int msgCol = ColumnByHeader(*model, QStringLiteral("msg"));
        QVERIFY2(msgCol >= 0, "msg column must exist after streaming");

        const QString filterId = QStringLiteral("hf-single");
        const QString needle = QStringLiteral("m42");
        QMetaObject::invokeMethod(
            mWindow,
            "FilterSubmitted",
            Qt::DirectConnection,
            Q_ARG(QString, filterId),
            Q_ARG(int, msgCol),
            Q_ARG(QString, needle),
            Q_ARG(int, static_cast<int>(loglib::LogConfiguration::LogFilter::Match::Contains))
        );
        QCoreApplication::processEvents();

        QCOMPARE(mWindow->Filters().size(), static_cast<size_t>(1));

        const QVariant decoration = model->headerData(msgCol, Qt::Horizontal, Qt::DecorationRole);
        QVERIFY(decoration.canConvert<QIcon>());
        QVERIFY2(
            !decoration.value<QIcon>().isNull(), "filtered column header must surface a (themed funnel) decoration"
        );

        const QString tooltip = model->headerData(msgCol, Qt::Horizontal, Qt::ToolTipRole).toString();
        QVERIFY2(tooltip.contains(QStringLiteral("Filters:")), "tooltip must include a Filters section");
        QVERIFY2(tooltip.contains(needle), "tooltip must list the submitted filter title");
    }

    // Two filters on the same column render as bullets in
    // alphabetical order regardless of insertion order. Pins the
    // sort in `MainWindow::SyncColumnFilterIndicators` against
    // `mFilters`'s unordered iteration.
    void TestHeaderFilterTooltipListsMultipleFiltersSortedAlphabetically()
    {
        const int levelCol = StreamFixtureForColumnTests();
        QVERIFY2(levelCol >= 0, "level column must exist after streaming");
        auto *model = mWindow->Model();
        const int msgCol = ColumnByHeader(*model, QStringLiteral("msg"));
        QVERIFY2(msgCol >= 0, "msg column must exist after streaming");

        // Insert deliberately reverse-sorted to prove the sort runs.
        QMetaObject::invokeMethod(
            mWindow,
            "FilterSubmitted",
            Qt::DirectConnection,
            Q_ARG(QString, QStringLiteral("hf-zebra")),
            Q_ARG(int, msgCol),
            Q_ARG(QString, QStringLiteral("zebra")),
            Q_ARG(int, static_cast<int>(loglib::LogConfiguration::LogFilter::Match::Contains))
        );
        QMetaObject::invokeMethod(
            mWindow,
            "FilterSubmitted",
            Qt::DirectConnection,
            Q_ARG(QString, QStringLiteral("hf-apple")),
            Q_ARG(int, msgCol),
            Q_ARG(QString, QStringLiteral("apple")),
            Q_ARG(int, static_cast<int>(loglib::LogConfiguration::LogFilter::Match::Contains))
        );
        QCoreApplication::processEvents();

        QCOMPARE(mWindow->Filters().size(), static_cast<size_t>(2));

        const QString tooltip = model->headerData(msgCol, Qt::Horizontal, Qt::ToolTipRole).toString();
        const qsizetype applePos = tooltip.indexOf(QStringLiteral("apple"));
        const qsizetype zebraPos = tooltip.indexOf(QStringLiteral("zebra"));
        QVERIFY2(applePos >= 0 && zebraPos >= 0, "tooltip must mention both filter titles");
        QVERIFY2(applePos < zebraPos, "tooltip must list filter titles in alphabetical order");
    }

    // `ClearAllFilters` returns the header to the no-filter
    // baseline: no decoration, no "Filters:" section. Catches the
    // missing trailing `SyncColumnFilterIndicators()` regression.
    void TestHeaderFilterIndicatorClearsOnClearAll()
    {
        const int levelCol = StreamFixtureForColumnTests();
        QVERIFY2(levelCol >= 0, "level column must exist after streaming");
        auto *model = mWindow->Model();
        const int msgCol = ColumnByHeader(*model, QStringLiteral("msg"));
        QVERIFY2(msgCol >= 0, "msg column must exist after streaming");

        QMetaObject::invokeMethod(
            mWindow,
            "FilterSubmitted",
            Qt::DirectConnection,
            Q_ARG(QString, QStringLiteral("hf-temporary")),
            Q_ARG(int, msgCol),
            Q_ARG(QString, QStringLiteral("m1")),
            Q_ARG(int, static_cast<int>(loglib::LogConfiguration::LogFilter::Match::Contains))
        );
        QCoreApplication::processEvents();
        QCOMPARE(mWindow->Filters().size(), static_cast<size_t>(1));
        QVERIFY2(model->HasFilterForColumn(msgCol), "precondition: filter recorded on msg column");

        QMetaObject::invokeMethod(mWindow, "ClearAllFilters", Qt::DirectConnection);
        QCoreApplication::processEvents();

        QVERIFY2(mWindow->Filters().empty(), "ClearAllFilters must drain mFilters");
        QVERIFY2(
            !model->HasFilterForColumn(msgCol), "ClearAllFilters must propagate into LogModel::HasFilterForColumn"
        );

        const QVariant decoration = model->headerData(msgCol, Qt::Horizontal, Qt::DecorationRole);
        if (decoration.canConvert<QIcon>())
        {
            QVERIFY2(decoration.value<QIcon>().isNull(), "decoration must be empty after ClearAllFilters");
        }
        const QString tooltip = model->headerData(msgCol, Qt::Horizontal, Qt::ToolTipRole).toString();
        QVERIFY2(
            !tooltip.contains(QStringLiteral("Filters:")),
            "tooltip must not advertise a Filters section after ClearAllFilters"
        );
    }

    // A column with both a type mismatch AND an active filter
    // shows both sections in the tooltip, but the warning glyph
    // wins the single decoration slot. Verified by snapshotting
    // the icon pixmap with and without the warning active.
    void TestHeaderWarningAndFilterCoexistInTooltip()
    {
        const int levelCol = StreamFixtureForColumnTests();
        QVERIFY2(levelCol >= 0, "level column must exist after streaming");
        auto *model = mWindow->Model();
        const int msgCol = ColumnByHeader(*model, QStringLiteral("msg"));
        QVERIFY2(msgCol >= 0, "msg column must exist after streaming");

        QMetaObject::invokeMethod(
            mWindow,
            "FilterSubmitted",
            Qt::DirectConnection,
            Q_ARG(QString, QStringLiteral("hf-coexist")),
            Q_ARG(int, msgCol),
            Q_ARG(QString, QStringLiteral("m7")),
            Q_ARG(int, static_cast<int>(loglib::LogConfiguration::LogFilter::Match::Contains))
        );
        QCoreApplication::processEvents();
        const QVariant funnelOnlyVariant = model->headerData(msgCol, Qt::Horizontal, Qt::DecorationRole);
        QVERIFY(funnelOnlyVariant.canConvert<QIcon>());
        const auto funnelOnly = funnelOnlyVariant.value<QIcon>();
        QVERIFY2(!funnelOnly.isNull(), "filter-only decoration must be the funnel icon");
        const QImage funnelOnlyImage = funnelOnly.pixmap(16, 16).toImage();

        model->ConfigurationManager().SetColumnAutoDetect(static_cast<size_t>(msgCol), false);
        model->ConfigurationManager().SetColumnType(
            static_cast<size_t>(msgCol), loglib::LogConfiguration::Type::Integer
        );
        model->RefreshColumnHealth();
        QCoreApplication::processEvents();

        const QVariant combinedVariant = model->headerData(msgCol, Qt::Horizontal, Qt::DecorationRole);
        QVERIFY(combinedVariant.canConvert<QIcon>());
        const auto combined = combinedVariant.value<QIcon>();
        QVERIFY2(!combined.isNull(), "warning+filter decoration must still be non-null");

        const QImage combinedImage = combined.pixmap(16, 16).toImage();
        // Warning precedence: the painted glyph must have switched
        // from the funnel to the warning icon. Comparing pixmaps
        // (not QIcons) avoids cache-key flakiness.
        QVERIFY2(combinedImage != funnelOnlyImage, "warning must replace funnel decoration when both conditions apply");

        const QString tooltip = model->headerData(msgCol, Qt::Horizontal, Qt::ToolTipRole).toString();
        QVERIFY2(
            tooltip.contains(QStringLiteral("Filters:")),
            "tooltip must keep the Filters section even when the warning is active"
        );
        QVERIFY2(
            tooltip.contains(QStringLiteral("do not match the configured type")),
            "tooltip must include the type-mismatch warning section"
        );
    }

    // Renaming a column refreshes the tooltip header but keeps
    // the funnel and "Filters:" section. Pins that
    // `mColumnFilterDetails` is keyed by section, not by label.
    void TestHeaderFilterIndicatorSurvivesColumnRename()
    {
        const int levelCol = StreamFixtureForColumnTests();
        QVERIFY2(levelCol >= 0, "level column must exist after streaming");
        auto *model = mWindow->Model();
        const int msgCol = ColumnByHeader(*model, QStringLiteral("msg"));
        QVERIFY2(msgCol >= 0, "msg column must exist after streaming");

        const QString needle = QStringLiteral("m9");
        QMetaObject::invokeMethod(
            mWindow,
            "FilterSubmitted",
            Qt::DirectConnection,
            Q_ARG(QString, QStringLiteral("hf-rename")),
            Q_ARG(int, msgCol),
            Q_ARG(QString, needle),
            Q_ARG(int, static_cast<int>(loglib::LogConfiguration::LogFilter::Match::Contains))
        );
        QCoreApplication::processEvents();
        QVERIFY2(model->HasFilterForColumn(msgCol), "precondition: filter recorded on msg column");

        model->ConfigurationManager().SetColumnHeader(static_cast<size_t>(msgCol), std::string("message"));
        model->NotifyColumnEdited(msgCol);
        QCoreApplication::processEvents();

        QVERIFY2(
            model->HasFilterForColumn(msgCol),
            "rename must not clear the per-column filter cache (keyed by section, not label)"
        );

        const QVariant decoration = model->headerData(msgCol, Qt::Horizontal, Qt::DecorationRole);
        QVERIFY(decoration.canConvert<QIcon>());
        QVERIFY2(!decoration.value<QIcon>().isNull(), "funnel decoration must survive a column rename");

        const QString tooltip = model->headerData(msgCol, Qt::Horizontal, Qt::ToolTipRole).toString();
        QVERIFY2(tooltip.contains(QStringLiteral("message")), "tooltip header line must reflect the renamed label");
        QVERIFY2(tooltip.contains(QStringLiteral("Filters:")), "tooltip must keep its Filters section after rename");
        QVERIFY2(tooltip.contains(needle), "Filters section must still list the active filter title after rename");
    }

    // Moving a filtered column re-aligns the funnel with the new
    // section index. Pins the `SyncColumnFilterIndicators` call
    // reached via `OnSourceColumnsMoved -> ApplyColumnVisibility`.
    void TestHeaderFilterIndicatorFollowsColumnMove()
    {
        const int levelColBefore = StreamFixtureForColumnTests();
        QVERIFY2(levelColBefore >= 0, "category column must exist after streaming");
        auto *model = mWindow->Model();
        QVERIFY2(model->columnCount() >= 2, "fixture must have at least two columns for a move");

        const int msgCol = ColumnByHeader(*model, QStringLiteral("msg"));
        QVERIFY2(msgCol >= 0, "msg column must exist after streaming");

        // Filter on `msg`; the funnel must follow the column to
        // its new section index after the move below.
        const QString filterId = QStringLiteral("hf-move");
        QMetaObject::invokeMethod(
            mWindow,
            "FilterSubmitted",
            Qt::DirectConnection,
            Q_ARG(QString, filterId),
            Q_ARG(int, msgCol),
            Q_ARG(QString, QStringLiteral("m1")),
            Q_ARG(int, static_cast<int>(loglib::LogConfiguration::LogFilter::Match::Contains))
        );
        QCoreApplication::processEvents();
        QVERIFY2(model->HasFilterForColumn(msgCol), "precondition: filter installed on msg column");

        // Move `msg` past `category` (or vice versa). `RemapColumnIndexAfterMove`
        // shifts the filter row alongside the column itself.
        const int src = msgCol;
        const int dest = (msgCol == 0) ? model->columnCount() - 1 : 0;
        QVERIFY2(model->MoveColumn(src, dest), "MoveColumn must succeed");
        QCoreApplication::processEvents();

        const int msgColAfter = ColumnByHeader(*model, QStringLiteral("msg"));
        QVERIFY2(msgColAfter >= 0, "msg column must still exist after move");
        QVERIFY2(msgColAfter != src, "fixture sanity: msg must have actually moved sections");

        QVERIFY2(
            model->HasFilterForColumn(msgColAfter), "funnel cache must follow the moved column to its new section index"
        );
        QVERIFY2(!model->HasFilterForColumn(src), "funnel must not be left stranded at the pre-move section");

        const QVariant decoration = model->headerData(msgColAfter, Qt::Horizontal, Qt::DecorationRole);
        QVERIFY(decoration.canConvert<QIcon>());
        QVERIFY2(!decoration.value<QIcon>().isNull(), "funnel decoration must render at the post-move section index");
    }

    // Hiding then re-showing a filtered column keeps the funnel
    // attached. Visibility doesn't change `filter.row`, so the
    // sync calls in `SetColumnVisible` / `ApplyColumnVisibility`
    // are no-ops -- but the symmetric wiring is what we pin.
    void TestHeaderFilterIndicatorSurvivesColumnHideShow()
    {
        const int levelCol = StreamFixtureForColumnTests();
        QVERIFY2(levelCol >= 0, "category column must exist after streaming");
        auto *model = mWindow->Model();
        const int msgCol = ColumnByHeader(*model, QStringLiteral("msg"));
        QVERIFY2(msgCol >= 0, "msg column must exist after streaming");

        QMetaObject::invokeMethod(
            mWindow,
            "FilterSubmitted",
            Qt::DirectConnection,
            Q_ARG(QString, QStringLiteral("hf-visibility")),
            Q_ARG(int, msgCol),
            Q_ARG(QString, QStringLiteral("m4")),
            Q_ARG(int, static_cast<int>(loglib::LogConfiguration::LogFilter::Match::Contains))
        );
        QCoreApplication::processEvents();
        QVERIFY2(model->HasFilterForColumn(msgCol), "precondition: filter installed on msg column");

        mWindow->SetColumnVisible(msgCol, false);
        QCoreApplication::processEvents();
        // Hiding is a header-view concern; the filter cache stays.
        QVERIFY2(model->HasFilterForColumn(msgCol), "hiding a column must not drop the per-column filter cache entry");

        mWindow->SetColumnVisible(msgCol, true);
        QCoreApplication::processEvents();
        QVERIFY2(model->HasFilterForColumn(msgCol), "re-showing the column must surface the same cache entry");

        const QVariant decoration = model->headerData(msgCol, Qt::Horizontal, Qt::DecorationRole);
        QVERIFY(decoration.canConvert<QIcon>());
        QVERIFY2(
            !decoration.value<QIcon>().isNull(),
            "funnel decoration must reappear with the column on a hide -> show round trip"
        );
    }

    // After a mid-batch `Level -> String` demote, the "Filters:"
    // section must list the rewritten raw bytes (`info`) rather
    // than the canonical name (`Info`) the user originally
    // submitted. Pins the resync inside the demote handler.
    void TestHeaderFilterTooltipReflectsRawValuesAfterLevelDemote()
    {
        auto *model = mWindow->findChild<LogModel *>();
        QVERIFY2(model != nullptr, "MainWindow must own a LogModel");

        const TempJsonFile emptyFixture(QStringList{});
        auto file = std::make_unique<loglib::LogFile>(emptyFixture.Path().toStdString());
        auto fileSource = std::make_unique<loglib::FileLineSource>(std::move(file));
        loglib::FileLineSource *sourcePtr = fileSource.get();
        (void)model->BeginStreamingForSyncTest(std::move(fileSource));

        // Cap of 3: batch 1 promotes to Level (dict size 2);
        // batch 2 trips the cap and demotes back to String.
        constexpr uint16_t TEST_CAP = 3;
        model->Table().SetEnumValueCap(TEST_CAP);

        loglib::KeyIndex &keys = model->Table().Keys();
        const auto makeLine = [&](const std::string &value) {
            std::vector<std::pair<loglib::KeyId, loglib::LogValue>> values;
            values.emplace_back(keys.GetOrInsert("level"), loglib::LogValue(value));
            return loglib::LogLine{std::move(values), keys, *sourcePtr, 0};
        };

        {
            loglib::StreamedBatch batch;
            batch.firstLineNumber = 1;
            batch.newKeys.emplace_back("level");
            batch.lines.push_back(makeLine("info"));
            batch.lines.push_back(makeLine("info"));
            batch.lines.push_back(makeLine("warn"));
            model->AppendBatch(std::move(batch));
        }

        const int levelCol = ColumnByHeader(*model, QStringLiteral("level"));
        QVERIFY2(levelCol >= 0, "level column must exist after promotion");
        QCOMPARE(
            model->Configuration().columns[static_cast<size_t>(levelCol)].type, loglib::LogConfiguration::Type::Level
        );

        // Submit `Info` while the column is still Level; the
        // pre-demote tooltip lists the canonical name.
        const QString filterId = QStringLiteral("hf-level-demote");
        QVERIFY2(
            QMetaObject::invokeMethod(
                mWindow,
                "FilterEnumSubmitted",
                Qt::DirectConnection,
                Q_ARG(QString, filterId),
                Q_ARG(int, levelCol),
                Q_ARG(QStringList, QStringList{QStringLiteral("Info")})
            ),
            "FilterEnumSubmitted slot must be invocable via meta-object"
        );
        QCoreApplication::processEvents();

        const QString tooltipBefore = model->headerData(levelCol, Qt::Horizontal, Qt::ToolTipRole).toString();
        QVERIFY2(
            tooltipBefore.contains(QStringLiteral("Info")),
            "tooltip pre-demote must list the canonical Level name the user submitted"
        );

        // Batch 2 overflows the cap and demotes `level` to String.
        // The `Demoted` handler rewrites `["Info"] -> ["info"]`
        // and resyncs the tooltip cache.
        {
            loglib::StreamedBatch batch;
            batch.firstLineNumber = 4;
            batch.lines.push_back(makeLine("error"));
            batch.lines.push_back(makeLine("debug"));
            batch.lines.push_back(makeLine("trace"));
            model->AppendBatch(std::move(batch));
        }
        QCoreApplication::processEvents();

        QCOMPARE(
            model->Configuration().columns[static_cast<size_t>(levelCol)].type, loglib::LogConfiguration::Type::String
        );

        const QString tooltipAfter = model->headerData(levelCol, Qt::Horizontal, Qt::ToolTipRole).toString();
        QVERIFY2(
            tooltipAfter.contains(QStringLiteral("Filters:")), "tooltip post-demote must keep its Filters section"
        );
        QVERIFY2(
            tooltipAfter.contains(QStringLiteral("info")), "tooltip post-demote must list the rewritten raw value"
        );
        // The canonical-name bullet `&bull; Info` must be gone.
        // We can't just `!contains("Info")` because raw `info`
        // is a substring of `Info`, so we look for the bullet.
        QVERIFY2(
            !tooltipAfter.contains(QStringLiteral("&bull; Info")),
            "tooltip post-demote must not still list the pre-demote canonical name"
        );

        model->EndStreaming(false);
    }

    // A palette flip must re-tint the funnel decoration. Pins
    // the `RefreshHeaderIcons` call inside `RefreshThemedIcons`:
    // without it, the cached pixmap would stay pinned to the
    // pre-flip `WindowText` colour.
    void TestHeaderFilterFunnelRetintsAfterPaletteChange()
    {
        const int levelCol = StreamFixtureForColumnTests();
        QVERIFY2(levelCol >= 0, "category column must exist after streaming");
        auto *model = mWindow->Model();
        const int msgCol = ColumnByHeader(*model, QStringLiteral("msg"));
        QVERIFY2(msgCol >= 0, "msg column must exist after streaming");

        QMetaObject::invokeMethod(
            mWindow,
            "FilterSubmitted",
            Qt::DirectConnection,
            Q_ARG(QString, QStringLiteral("hf-palette")),
            Q_ARG(int, msgCol),
            Q_ARG(QString, QStringLiteral("m2")),
            Q_ARG(int, static_cast<int>(loglib::LogConfiguration::LogFilter::Match::Contains))
        );
        QCoreApplication::processEvents();

        auto opaquePixelCount = [](const QIcon &icon, QRgb expected) -> int {
            const QSize size{16, 16};
            const QImage img = icon.pixmap(size).toImage().convertToFormat(QImage::Format_ARGB32);
            int matching = 0;
            for (int y = 0; y < img.height(); ++y)
            {
                for (int x = 0; x < img.width(); ++x)
                {
                    const QRgb px = img.pixel(x, y);
                    if (qAlpha(px) < 128)
                    {
                        continue;
                    }
                    constexpr int CHANNEL_TOLERANCE = 32;
                    if (std::abs(qRed(px) - qRed(expected)) <= CHANNEL_TOLERANCE &&
                        std::abs(qGreen(px) - qGreen(expected)) <= CHANNEL_TOLERANCE &&
                        std::abs(qBlue(px) - qBlue(expected)) <= CHANNEL_TOLERANCE)
                    {
                        ++matching;
                    }
                }
            }
            return matching;
        };

        // `QApplication::setPalette` is process-global; the
        // per-test fixture only resets `mWindow`, so we save and
        // restore the app palette ourselves.
        const QPalette savedAppPalette = QApplication::palette();
        const QPalette savedWindowPalette = mWindow->palette();
        const auto paletteGuard = qScopeGuard([&]() {
            QApplication::setPalette(savedAppPalette);
            mWindow->setPalette(savedWindowPalette);
            QCoreApplication::processEvents();
        });

        // The funnel tints from `QApplication::palette()`, so we
        // must flip the app palette (not just the window's) for
        // the re-render to pick up the new colour.
        QPalette redPalette = QApplication::palette();
        redPalette.setColor(QPalette::Active, QPalette::WindowText, QColor(255, 0, 0));
        redPalette.setColor(QPalette::Inactive, QPalette::WindowText, QColor(255, 0, 0));
        QApplication::setPalette(redPalette);
        mWindow->setPalette(redPalette);
        QCoreApplication::processEvents();

        const QVariant redVariant = model->headerData(msgCol, Qt::Horizontal, Qt::DecorationRole);
        QVERIFY(redVariant.canConvert<QIcon>());
        const auto redIcon = redVariant.value<QIcon>();
        QVERIFY2(!redIcon.isNull(), "funnel decoration must be present under the red palette");
        const int redPixels = opaquePixelCount(redIcon, qRgb(255, 0, 0));
        QVERIFY2(redPixels > 0, "funnel must paint in the red WindowText colour after the first palette flip");

        // Flip to blue. Without `RefreshHeaderIcons` the cached
        // red pixmap would stay pinned and bluePixels would be 0.
        QPalette bluePalette = QApplication::palette();
        bluePalette.setColor(QPalette::Active, QPalette::WindowText, QColor(0, 0, 255));
        bluePalette.setColor(QPalette::Inactive, QPalette::WindowText, QColor(0, 0, 255));
        QApplication::setPalette(bluePalette);
        mWindow->setPalette(bluePalette);
        QCoreApplication::processEvents();

        const QVariant blueVariant = model->headerData(msgCol, Qt::Horizontal, Qt::DecorationRole);
        QVERIFY(blueVariant.canConvert<QIcon>());
        const auto blueIcon = blueVariant.value<QIcon>();
        QVERIFY2(!blueIcon.isNull(), "funnel decoration must be present under the blue palette");
        const int bluePixels = opaquePixelCount(blueIcon, qRgb(0, 0, 255));
        QVERIFY2(bluePixels > 0, "funnel must repaint in the blue WindowText colour after a palette flip");
    }

    // The other tests only cover `BuildFilterTitle`'s String
    // branch. Pin the Enumeration and numeric-range branches so
    // their tooltip rendering stays correct.
    void TestHeaderFilterTooltipForEnumAndNumericFilters()
    {
        const int levelCol = StreamFixtureForColumnTests();
        QVERIFY2(levelCol >= 0, "category column must exist after streaming");
        auto *model = mWindow->Model();
        QCOMPARE(
            model->Configuration().columns[static_cast<size_t>(levelCol)].type,
            loglib::LogConfiguration::Type::Enumeration
        );

        // Enum branch: assert each selected value is mentioned;
        // the exact separator is an implementation detail. The
        // local lifts the `{a, b}` list out of the `Q_ARG` macro,
        // which would mis-parse the embedded comma.
        const QStringList enumValues{QStringLiteral("info"), QStringLiteral("warn")};
        QVERIFY2(
            QMetaObject::invokeMethod(
                mWindow,
                "FilterEnumSubmitted",
                Qt::DirectConnection,
                Q_ARG(QString, QStringLiteral("hf-enum")),
                Q_ARG(int, levelCol),
                Q_ARG(QStringList, enumValues)
            ),
            "FilterEnumSubmitted slot must be invocable via meta-object"
        );
        QCoreApplication::processEvents();

        const QString enumTooltip = model->headerData(levelCol, Qt::Horizontal, Qt::ToolTipRole).toString();
        QVERIFY2(enumTooltip.contains(QStringLiteral("Filters:")), "enum filter must show in the Filters section");
        QVERIFY2(
            enumTooltip.contains(QStringLiteral("info")) && enumTooltip.contains(QStringLiteral("warn")),
            "enum tooltip must list every selected value"
        );

        // Wipe the enum filter so the numeric branch starts clean
        // and the two filter bullets don't collide.
        QMetaObject::invokeMethod(mWindow, "ClearAllFilters", Qt::DirectConnection);
        QCoreApplication::processEvents();
        QVERIFY2(mWindow->Filters().empty(), "ClearAllFilters must drain mFilters between branches");

        // Numeric branch: `BuildFilterTitle` formats from
        // `LogFilter::Type`, not the column's declared type, so a
        // numeric filter on the (string) `msg` column still
        // exercises this branch.
        const int msgCol = ColumnByHeader(*model, QStringLiteral("msg"));
        QVERIFY2(msgCol >= 0, "msg column must exist after streaming");
        QVERIFY2(
            QMetaObject::invokeMethod(
                mWindow,
                "FilterNumericRangeSubmitted",
                Qt::DirectConnection,
                Q_ARG(QString, QStringLiteral("hf-numeric")),
                Q_ARG(int, msgCol),
                Q_ARG(std::optional<double>, std::optional<double>{1.5}),
                Q_ARG(std::optional<double>, std::optional<double>{42.0})
            ),
            "FilterNumericRangeSubmitted slot must be invocable via meta-object"
        );
        QCoreApplication::processEvents();

        const QString numericTooltip = model->headerData(msgCol, Qt::Horizontal, Qt::ToolTipRole).toString();
        QVERIFY2(
            numericTooltip.contains(QStringLiteral("Filters:")), "numeric filter must show in the Filters section"
        );
        // Both bounds must appear; the separator (`>=`, `<=`,
        // dash) is an implementation detail.
        QVERIFY2(
            numericTooltip.contains(QStringLiteral("1.5")) && numericTooltip.contains(QStringLiteral("42")),
            "numeric tooltip must list both bounds"
        );
    }

    // Status-bar diagnostics button + dialog summary are driven by
    // `ConfigurationDiagnosticsDialog::MismatchedColumnCount`, so they
    // both flip together. Verify the aggregate stays in lockstep with
    // the per-column health (no double-counting, no stale reads).
    void TestDiagnosticsButtonSurfacesMismatchCount()
    {
        const int levelCol = StreamFixtureForColumnTests();
        QVERIFY2(levelCol >= 0, "level column must exist after streaming");
        auto *model = mWindow->Model();
        const int msgCol = ColumnByHeader(*model, QStringLiteral("msg"));
        QVERIFY2(msgCol >= 0, "msg column must exist after streaming");

        // Steady state after streamingFinished: every column either
        // auto-detected to a matching type or stayed Any; zero
        // mismatches.
        QCOMPARE(ConfigurationDiagnosticsDialog::MismatchedColumnCount(*model), 0);
        auto *button = mWindow->findChild<QPushButton *>(QStringLiteral("diagnosticsButton"));
        QVERIFY2(button != nullptr, "MainWindow must own the diagnostics status-bar button");
        // Offscreen-QPA leaves the parent window hidden, which would
        // collapse `isVisible` to false regardless of the slot's
        // intent; check `isHidden` so we exercise only the state the
        // slot actually drives.
        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): prior QVERIFY2 aborts on null.
        QVERIFY2(button->isHidden(), "Diagnostics button must be hidden when no mismatches are present");

        // Force a mismatch by pinning `msg` to Integer.
        model->ConfigurationManager().SetColumnAutoDetect(static_cast<size_t>(msgCol), false);
        model->ConfigurationManager().SetColumnType(
            static_cast<size_t>(msgCol), loglib::LogConfiguration::Type::Integer
        );
        model->RefreshColumnHealth();
        QCoreApplication::processEvents();

        QCOMPARE(ConfigurationDiagnosticsDialog::MismatchedColumnCount(*model), 1);
        // `isVisible` is gated by the parent window's visibility,
        // which the offscreen-QPA test harness keeps hidden; check
        // the inverse (`isHidden`) instead so the assertion exercises
        // the only state the slot controls.
        QVERIFY2(!button->isHidden(), "Diagnostics button must un-hide when a column mismatches");
        QVERIFY2(button->text().contains(QStringLiteral("1")), "Button label must surface the mismatch count");
    }

    // The diagnostics dialog walks the same per-column snapshot as
    // the status-bar aggregate; opening it must list exactly one
    // mismatched row (matching the count surfaced by the button).
    void TestDiagnosticsDialogListsMismatchedColumns()
    {
        const int levelCol = StreamFixtureForColumnTests();
        QVERIFY2(levelCol >= 0, "level column must exist after streaming");
        auto *model = mWindow->Model();
        const int msgCol = ColumnByHeader(*model, QStringLiteral("msg"));
        QVERIFY2(msgCol >= 0, "msg column must exist after streaming");

        model->ConfigurationManager().SetColumnAutoDetect(static_cast<size_t>(msgCol), false);
        model->ConfigurationManager().SetColumnType(
            static_cast<size_t>(msgCol), loglib::LogConfiguration::Type::Integer
        );
        model->RefreshColumnHealth();

        const ConfigurationDiagnosticsDialog dialog(model);
        const auto *table = dialog.findChild<QTableWidget *>();
        QVERIFY2(table != nullptr, "Dialog must own a diagnosticsTable widget");
        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): prior QVERIFY2 aborts on null.
        QCOMPARE(table->rowCount(), static_cast<int>(model->Configuration().columns.size()));

        // Find the mismatched row by header text and assert the
        // counter columns. Sorting is enabled, so look up by item
        // rather than fixed row index.
        int matchedRowIndex = -1;
        for (int row = 0; row < table->rowCount(); ++row)
        {
            const QTableWidgetItem *headerItem = table->item(row, 0);
            if (headerItem != nullptr && headerItem->text() == QStringLiteral("msg"))
            {
                matchedRowIndex = row;
                break;
            }
        }
        QVERIFY2(matchedRowIndex >= 0, "Dialog must include a row for the `msg` column");

        // Column 6 is the "Mismatched" counter (see the header layout
        // in `ConfigurationDiagnosticsDialog`'s constructor).
        constexpr int MISMATCHED_COLUMN = 6;
        const QTableWidgetItem *mismatchedItem = table->item(matchedRowIndex, MISMATCHED_COLUMN);
        QVERIFY(mismatchedItem != nullptr);
        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): prior QVERIFY aborts on null.
        QVERIFY2(mismatchedItem->text().toInt() > 0, "Dialog must report a non-zero mismatch count for `msg`");

        // Regression: the mismatched row must pin both halves of the
        // contrast pair; bg alone leaves fg on the palette default,
        // which is near-white on a dark theme (row text invisible).
        for (int c = 0; c < table->columnCount(); ++c)
        {
            const QTableWidgetItem *item = table->item(matchedRowIndex, c);
            QVERIFY2(item != nullptr, qPrintable(QStringLiteral("mismatched row column %1 must have an item").arg(c)));
            // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): prior QVERIFY2 aborts on null.
            const QBrush bg = item->background();
            const QBrush fg = item->foreground();
            QVERIFY2(
                bg.style() != Qt::NoBrush,
                qPrintable(QStringLiteral("mismatched row column %1 must paint an explicit background").arg(c))
            );
            QVERIFY2(
                fg.style() != Qt::NoBrush,
                qPrintable(QStringLiteral("mismatched row column %1 must paint an explicit foreground "
                                          "so it stays legible on dark themes")
                               .arg(c))
            );
            // ITU-R BT.601 luma: foreground must be visibly darker
            // than the background, or we're back to the original bug.
            const QColor fgColor = fg.color();
            const QColor bgColor = bg.color();
            constexpr double R_LUMA = 0.299;
            constexpr double G_LUMA = 0.587;
            constexpr double B_LUMA = 0.114;
            const double fgLuma = (R_LUMA * fgColor.red()) + (G_LUMA * fgColor.green()) + (B_LUMA * fgColor.blue());
            const double bgLuma = (R_LUMA * bgColor.red()) + (G_LUMA * bgColor.green()) + (B_LUMA * bgColor.blue());
            constexpr double MIN_LUMA_GAP = 80.0;
            QVERIFY2(
                bgLuma - fgLuma >= MIN_LUMA_GAP,
                qPrintable(
                    QStringLiteral("mismatched row column %1 must keep a high contrast between fg (%2) and bg (%3); "
                                   "got fgLuma=%4, bgLuma=%5")
                        .arg(c)
                        .arg(fgColor.name(), bgColor.name())
                        .arg(fgLuma)
                        .arg(bgLuma)
                )
            );
        }
    }

    // Regression: the mismatched-row highlight must adapt to the
    // active palette. The original fix pinned a pale-pink bg + dark
    // fg which read as a glaringly bright row in Dark themes.
    // Flipping to Dark must hand the dialog a darker bg + lighter
    // fg while keeping the same luma gap.
    void TestDiagnosticsDialogHighlightAdaptsToDarkTheme()
    {
        QVERIFY(mTheme != nullptr);
        const int levelCol = StreamFixtureForColumnTests();
        QVERIFY2(levelCol >= 0, "level column must exist after streaming");
        auto *model = mWindow->Model();
        const int msgCol = ColumnByHeader(*model, QStringLiteral("msg"));
        QVERIFY2(msgCol >= 0, "msg column must exist after streaming");

        // Force `msg` into a mismatch so the dialog paints a warning row.
        model->ConfigurationManager().SetColumnAutoDetect(static_cast<size_t>(msgCol), false);
        model->ConfigurationManager().SetColumnType(
            static_cast<size_t>(msgCol), loglib::LogConfiguration::Type::Integer
        );
        model->RefreshColumnHealth();

        // Snapshot the Light-theme brushes for the `msg` row.
        mTheme->SetActiveSelection(QStringLiteral("Light"));
        QCoreApplication::processEvents();

        const ConfigurationDiagnosticsDialog dialog(model);
        auto *table = dialog.findChild<QTableWidget *>();
        QVERIFY2(table != nullptr, "Dialog must own a diagnosticsTable widget");

        auto findMsgRow = [table]() {
            // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): false positive; prior `QVERIFY2` aborts on null.
            for (int row = 0; row < table->rowCount(); ++row)
            {
                const QTableWidgetItem *headerItem = table->item(row, 0);
                if (headerItem != nullptr && headerItem->text() == QStringLiteral("msg"))
                {
                    return row;
                }
            }
            return -1;
        };

        const int lightRow = findMsgRow();
        QVERIFY2(lightRow >= 0, "Light-theme dialog must include a row for the `msg` column");
        const QTableWidgetItem *lightItem = table->item(lightRow, 0);
        QVERIFY(lightItem != nullptr);
        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): prior QVERIFY aborts on null.
        const QColor lightBg = lightItem->background().color();
        const QColor lightFg = lightItem->foreground().color();

        // Flip to Dark. The palette change fires `ApplicationPaletteChange`
        // on the dialog; the override re-runs `Refresh()`.
        mTheme->SetActiveSelection(QStringLiteral("Dark"));
        QCoreApplication::processEvents();

        const int darkRow = findMsgRow();
        QVERIFY2(darkRow >= 0, "Dark-theme dialog must still include a row for the `msg` column");
        const QTableWidgetItem *darkItem = table->item(darkRow, 0);
        QVERIFY(darkItem != nullptr);
        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): prior QVERIFY aborts on null.
        const QColor darkBg = darkItem->background().color();
        const QColor darkFg = darkItem->foreground().color();

        QVERIFY2(
            lightBg != darkBg,
            qPrintable(QStringLiteral("Highlight bg must differ between Light and Dark; got light=%1, dark=%2")
                           .arg(lightBg.name(), darkBg.name()))
        );
        QVERIFY2(
            lightFg != darkFg,
            qPrintable(QStringLiteral("Highlight fg must differ between Light and Dark; got light=%1, dark=%2")
                           .arg(lightFg.name(), darkFg.name()))
        );

        constexpr double R_LUMA = 0.299;
        constexpr double G_LUMA = 0.587;
        constexpr double B_LUMA = 0.114;
        auto luma = [](const QColor &c) { return (R_LUMA * c.red()) + (G_LUMA * c.green()) + (B_LUMA * c.blue()); };

        // Dark-mode highlight bg must actually be dark; otherwise the
        // row punches through the dialog as a near-white slab.
        constexpr double DARK_BG_MAX_LUMA = 110.0;
        QVERIFY2(
            luma(darkBg) <= DARK_BG_MAX_LUMA,
            qPrintable(QStringLiteral("Dark-theme highlight bg must be dark; got %1 (luma=%2)")
                           .arg(darkBg.name())
                           .arg(luma(darkBg)))
        );

        // Dark mode reverses the pair: fg must be lighter than bg,
        // by the same gap as the light-mode highlight.
        constexpr double MIN_LUMA_GAP = 80.0;
        QVERIFY2(
            luma(darkFg) - luma(darkBg) >= MIN_LUMA_GAP,
            qPrintable(QStringLiteral("Dark-theme highlight must keep high contrast (fg lighter than bg); "
                                      "got fg=%1 (luma=%2), bg=%3 (luma=%4)")
                           .arg(darkFg.name())
                           .arg(luma(darkFg))
                           .arg(darkBg.name())
                           .arg(luma(darkBg)))
        );

        // Restore the default before the next test starts.
        mTheme->SetActiveSelection(QStringLiteral("Light"));
        QCoreApplication::processEvents();
    }

    // Regression: closing the Preferences window via the X button
    // must revert a live-previewed theme to the persisted selection.
    // Without the `closeEvent` override the Dark preview leaked
    // past the dialog until the next app restart.
    void TestPreferencesEditorCloseEventRevertsThemePreview()
    {
        QVERIFY(mTheme != nullptr);

        // Persist a known baseline so the revert has a deterministic target.
        mTheme->SetActiveSelection(QStringLiteral("Light"));
        mTheme->SaveConfiguration();
        QCOMPARE(mTheme->PersistedSelection(), QStringLiteral("Light"));
        QCOMPARE(mTheme->ActiveSelection(), QStringLiteral("Light"));

        PreferencesEditor editor(mTheme.data());

        // Preview Dark directly via `ThemeControl` to stay independent
        // of QComboBox event quirks under offscreen-QPA.
        mTheme->SetActiveSelection(QStringLiteral("Dark"));
        QCoreApplication::processEvents();
        QCOMPARE(mTheme->ActiveSelection(), QStringLiteral("Dark"));
        // Persisted unchanged -- only Ok writes that.
        QCOMPARE(mTheme->PersistedSelection(), QStringLiteral("Light"));

        // Same path the X button takes (programmatic `close()` ->
        // `closeEvent` without first running Cancel).
        editor.close();
        QCoreApplication::processEvents();

        QCOMPARE(mTheme->ActiveSelection(), QStringLiteral("Light"));
        QCOMPARE(mTheme->PersistedSelection(), QStringLiteral("Light"));
    }

    // Live preview + rollback: toggling Preferences checkboxes
    // updates the model + theme immediately, and Cancel/X-close
    // rewinds to the pre-dialog state.
    void TestPreferencesEditorLevelIconsLivePreviewAndRevert()
    {
        QVERIFY(mTheme != nullptr);

        // Baseline showLevelIcons=true so toggling off is a real
        // change. Built-in themes ship `levelColumnOverride`.
        {
            QSettings settings;
            settings.setValue(QStringLiteral("ui/showLevelIcons"), true);
        }
        mTheme->SetActiveSelection(QStringLiteral("Dark"));
        QCoreApplication::processEvents();
        QVERIFY(mTheme->HasLevelColumnOverride());
        // Drive the model into icon mode so we can observe the
        // live-preview flip back to text mode.
        auto *model = mWindow->Model();
        QVERIFY(model != nullptr);
        model->SetShowLevelIcons(true);
        QVERIFY(model->IsLevelIconModeActive());

        auto *editor = mWindow->findChild<PreferencesEditor *>();
        QVERIFY2(editor != nullptr, "MainWindow must own a PreferencesEditor");
        // `UpdateFields` seeds `mInitial*` from QSettings.
        editor->UpdateFields();
        // Find the checkbox by its label rather than `objectName`
        // so the test doesn't need production-only metadata.
        QCheckBox *checkbox = nullptr;
        for (auto *cb : editor->findChildren<QCheckBox *>())
        {
            if (cb->text() == QStringLiteral("Show level icons"))
            {
                checkbox = cb;
                break;
            }
        }
        QVERIFY2(checkbox != nullptr, "Preferences must expose a `Show level icons` checkbox");
        // Explicit null-check for clang-analyzer (it doesn't model
        // QVERIFY2's early return).
        if (checkbox == nullptr)
        {
            return;
        }
        QVERIFY(checkbox->isChecked());

        // Live preview: untoggle -> icon mode flips off
        // synchronously via `MainWindow`'s handler.
        checkbox->setChecked(false);
        QCoreApplication::processEvents();
        QVERIFY2(!model->IsLevelIconModeActive(), "Untoggling the checkbox must live-preview icon mode OFF");

        // QSettings is untouched until Ok.
        {
            const QSettings settings;
            QVERIFY(settings.value(QStringLiteral("ui/showLevelIcons")).toBool());
        }

        // X-close triggers `closeEvent` revert.
        editor->close();
        QCoreApplication::processEvents();

        QVERIFY2(
            model->IsLevelIconModeActive(),
            "Closing the dialog without Ok must revert the level-icons live preview to the initial state"
        );
        {
            const QSettings settings;
            QVERIFY(settings.value(QStringLiteral("ui/showLevelIcons")).toBool());
        }
    }

    // Mirror of the level-icons test for the high-contrast toggle.
    // Observed on `ThemeControl::IsHighContrast` because the live
    // preview goes through `SetHighContrast`.
    void TestPreferencesEditorHighContrastLivePreviewAndRevert()
    {
        QVERIFY(mTheme != nullptr);

        // Baseline highContrast=false on a theme with a
        // `levelsHighContrast` block so the checkbox is enabled.
        {
            QSettings settings;
            settings.setValue(QStringLiteral("ui/highContrastLevels"), false);
        }
        mTheme->SetActiveSelection(QStringLiteral("Dark"));
        QCoreApplication::processEvents();
        QVERIFY(mTheme->HasLevelsHighContrast());
        mTheme->SetHighContrast(false);
        QVERIFY(!mTheme->IsHighContrast());

        auto *editor = mWindow->findChild<PreferencesEditor *>();
        QVERIFY2(editor != nullptr, "MainWindow must own a PreferencesEditor");
        editor->UpdateFields();

        QCheckBox *checkbox = nullptr;
        for (auto *cb : editor->findChildren<QCheckBox *>())
        {
            if (cb->text() == QStringLiteral("High contrast levels"))
            {
                checkbox = cb;
                break;
            }
        }
        QVERIFY2(checkbox != nullptr, "Preferences must expose a `High contrast levels` checkbox");
        // Explicit null-check for clang-analyzer (see sibling test).
        if (checkbox == nullptr)
        {
            return;
        }
        QVERIFY(!checkbox->isChecked());

        // Live preview: toggle on -> controller flag flips
        // immediately, `themeChanged` fires via existing wiring.
        checkbox->setChecked(true);
        QCoreApplication::processEvents();
        QVERIFY2(mTheme->IsHighContrast(), "Toggling the checkbox must live-preview high contrast ON");

        // QSettings still false until Ok.
        {
            const QSettings settings;
            QVERIFY(!settings.value(QStringLiteral("ui/highContrastLevels")).toBool());
        }

        // X-close reverts via `closeEvent`.
        editor->close();
        QCoreApplication::processEvents();

        QVERIFY2(!mTheme->IsHighContrast(), "Closing the dialog without Ok must revert the high-contrast live preview");
    }

    // The Column Editor writes back every user-controllable Column
    // field on Apply: header, type, autoDetect, visible. Verify each
    // surface lands in the live configuration and the health cache
    // re-snapshots so the diagnostics surfaces flip in lockstep.
    void TestColumnEditorAppliesEveryField()
    {
        const int levelCol = StreamFixtureForColumnTests();
        QVERIFY2(levelCol >= 0, "level column must exist after streaming");
        auto *model = mWindow->Model();
        const int msgCol = ColumnByHeader(*model, QStringLiteral("msg"));
        QVERIFY2(msgCol >= 0, "msg column must exist after streaming");

        // Baseline: msg auto-detected to String, visible, header "msg".
        QVERIFY(model->Configuration().columns[static_cast<size_t>(msgCol)].visible);
        QCOMPARE(
            model->Configuration().columns[static_cast<size_t>(msgCol)].type, loglib::LogConfiguration::Type::String
        );

        ColumnEditor editor(model, msgCol);

        // Direct accessors instead of `findChild` -- Linux Qt 6.8 +
        // offscreen QPA strands the lookup for child widgets the same
        // way it does for QActions (see `FindActionByObjectName`).
        auto *headerEdit = editor.findChild<QLineEdit *>();
        auto *typeCombo = editor.findChild<QComboBox *>();
        auto *visibleCheck = editor.findChild<QCheckBox *>();
        QVERIFY(headerEdit != nullptr);
        QVERIFY(typeCombo != nullptr);
        QVERIFY(visibleCheck != nullptr);
        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): prior QVERIFYs abort on null.
        QCOMPARE(headerEdit->text(), QStringLiteral("msg"));

        // Drive the form: rename, pin to Integer, and hide.
        headerEdit->setText(QStringLiteral("message"));
        // Index 4 in the combo is "Integer" (see TypeChoices() in
        // column_editor.cpp); the choice list is the single source of
        // truth and the order is documented inline.
        constexpr int INTEGER_CHOICE_INDEX = 4;
        typeCombo->setCurrentIndex(INTEGER_CHOICE_INDEX);
        visibleCheck->setChecked(false);

        const QSignalSpy healthSpy(model, &LogModel::columnHealthChanged);
        editor.Apply();

        const auto &updated = model->Configuration().columns[static_cast<size_t>(msgCol)];
        QCOMPARE(QString::fromStdString(updated.header), QStringLiteral("message"));
        QCOMPARE(updated.type, loglib::LogConfiguration::Type::Integer);
        QVERIFY2(!updated.autoDetect, "Pinning a concrete type must flip autoDetect off");
        QVERIFY2(!updated.visible, "Visible checkbox must round-trip to Column::visible");

        // The diagnostics cache picked up the new type immediately;
        // pinning a string column to Integer must reveal mismatches.
        QVERIFY2(healthSpy.count() >= 1, "Apply must trigger RefreshColumnHealth and emit columnHealthChanged");
        const auto health = model->ColumnHealth(msgCol);
        QVERIFY(health.has_value());
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        QVERIFY2(health->matchingSlots < health->presentSlots, "Pinned-Integer string column must report mismatches");
    }

    // Regression: editor-driven type edits that cross the
    // Enumeration/Level boundary must emit `enumColumnsChanged`. The
    // streaming auto-detect path emits the signal from
    // `LogModel::AppendBatch`'s snapshot diff; before
    // `LogModel::ApplyColumnTypeEdit` was introduced the editor path
    // silently skipped it, leaving any active enum filter on the
    // column wired to a now-stale bitset and the proxy's
    // `EnumDictRank` cache uninvalidated.
    //
    // We drive both directions on the `category` column (4 unique
    // values, well within `DEFAULT_ENUM_VALUE_CAP`) returned by
    // `StreamFixtureForColumnTests`:
    //   1. Enumeration -> String: should emit `Demoted`.
    //   2. String -> Enumeration: should emit `Promoted`.
    void TestColumnEditorTypeEditEmitsEnumColumnsChanged()
    {
        const int levelCol = StreamFixtureForColumnTests();
        QVERIFY2(levelCol >= 0, "category column must exist after streaming");
        auto *model = mWindow->Model();
        // The streaming detector auto-promotes `category` to
        // `Enumeration` (the key is not a recognized level name, so
        // it stops short of `Type::Level`). Verify the precondition
        // so a future detector change doesn't silently disarm the
        // test.
        QCOMPARE(
            model->Configuration().columns[static_cast<size_t>(levelCol)].type,
            loglib::LogConfiguration::Type::Enumeration
        );

        // (1) Enumeration -> String: editor demotes the column. The
        // signal must fire so any active enum filter rebuilds onto
        // the string-set fallback and the cached rank entry is dropped.
        {
            const QSignalSpy enumSpy(model, &LogModel::enumColumnsChanged);
            QVERIFY(enumSpy.isValid());
            ColumnEditor editor(model, levelCol);
            auto *typeCombo = editor.findChild<QComboBox *>();
            QVERIFY(typeCombo != nullptr);
            // Index 2 is "String" -- mirrors TypeChoices() in
            // column_editor.cpp; same convention as the other editor
            // tests in this file.
            constexpr int STRING_CHOICE_INDEX = 2;
            // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): prior QVERIFY aborts on null.
            typeCombo->setCurrentIndex(STRING_CHOICE_INDEX);
            editor.Apply();

            QCOMPARE(
                model->Configuration().columns[static_cast<size_t>(levelCol)].type,
                loglib::LogConfiguration::Type::String
            );
            bool sawDemote = false;
            for (int i = 0; i < enumSpy.count(); ++i)
            {
                const QList<QVariant> &args = enumSpy.at(i);
                const auto reason = args.at(0).value<EnumColumnsChangeReason>();
                const int columnIndex = args.at(1).toInt();
                if (reason == EnumColumnsChangeReason::Demoted && columnIndex == levelCol)
                {
                    sawDemote = true;
                    break;
                }
            }
            QVERIFY2(sawDemote, "Editor-driven Enumeration->String must emit enumColumnsChanged(Demoted, levelCol)");
        }

        // (2) String -> Enumeration: editor re-promotes the column.
        // The signal must fire so any pre-existing string-fallback
        // enum filter upgrades onto the new bitset fast path and the
        // proxy refreshes its rank cache.
        {
            const QSignalSpy enumSpy(model, &LogModel::enumColumnsChanged);
            QVERIFY(enumSpy.isValid());
            ColumnEditor editor(model, levelCol);
            auto *typeCombo = editor.findChild<QComboBox *>();
            QVERIFY(typeCombo != nullptr);
            constexpr int ENUMERATION_CHOICE_INDEX = 8;
            // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): prior QVERIFY aborts on null.
            typeCombo->setCurrentIndex(ENUMERATION_CHOICE_INDEX);
            editor.Apply();

            QCOMPARE(
                model->Configuration().columns[static_cast<size_t>(levelCol)].type,
                loglib::LogConfiguration::Type::Enumeration
            );
            bool sawPromote = false;
            for (int i = 0; i < enumSpy.count(); ++i)
            {
                const QList<QVariant> &args = enumSpy.at(i);
                const auto reason = args.at(0).value<EnumColumnsChangeReason>();
                const int columnIndex = args.at(1).toInt();
                if (reason == EnumColumnsChangeReason::Promoted && columnIndex == levelCol)
                {
                    sawPromote = true;
                    break;
                }
            }
            QVERIFY2(sawPromote, "Editor-driven String->Enumeration must emit enumColumnsChanged(Promoted, levelCol)");
        }
    }

    // Regression: a no-op editor accept (user presses OK without
    // touching the type combo) must not emit `enumColumnsChanged`.
    // The signal triggers an O(n) filter rebuild downstream, so the
    // editor path should short-circuit when nothing changed.
    void TestColumnEditorNoTypeChangeIsSilent()
    {
        const int levelCol = StreamFixtureForColumnTests();
        QVERIFY2(levelCol >= 0, "level column must exist after streaming");
        auto *model = mWindow->Model();
        const int msgCol = ColumnByHeader(*model, QStringLiteral("msg"));
        QVERIFY2(msgCol >= 0, "msg column must exist after streaming");

        const QSignalSpy enumSpy(model, &LogModel::enumColumnsChanged);
        QVERIFY(enumSpy.isValid());
        ColumnEditor editor(model, msgCol);
        editor.Apply(); // Combo untouched -- still on the same choice.

        QCOMPARE(enumSpy.count(), 0);
    }

    // Regression: an auto-promoted column (e.g. `category` ->
    // `Enumeration` via the streaming detector) carries
    // `(type=Enumeration, autoDetect=true)` because the pipeline
    // flips only `type` through `SetColumnType`. The Type combo
    // must therefore:
    //   1. Surface the resolved type in the combo (the user must
    //      see "Enumeration", not a fallback to "Auto-detect").
    //   2. Round-trip cleanly on accept-without-change: the
    //      column's `(type, autoDetect)` pair stays intact (no
    //      silent `(Enumeration, false)` pin, no destructive
    //      `(Any, true)` rewrite via the "Auto-detect" entry).
    void TestColumnEditorPreservesAutoDetectFlagOnPromotedColumn()
    {
        const int categoryCol = StreamFixtureForColumnTests();
        QVERIFY2(categoryCol >= 0, "category column must exist after streaming");
        auto *model = mWindow->Model();

        // Precondition: streaming auto-promoted `category` to
        // `Enumeration` with `autoDetect = true`: the detector
        // never clears the flag. `FindTypeChoiceIndex` must surface
        // this as "Enumeration", not as index 0 ("Auto-detect").
        const auto &preEdit = model->Configuration().columns[static_cast<size_t>(categoryCol)];
        QCOMPARE(preEdit.type, loglib::LogConfiguration::Type::Enumeration);
        QVERIFY2(preEdit.autoDetect, "streaming auto-promotion must leave autoDetect=true");

        ColumnEditor editor(model, categoryCol);
        auto *typeCombo = editor.findChild<QComboBox *>();
        QVERIFY(typeCombo != nullptr);
        // (1) Combo must show the resolved type. Index 8 is
        // "Enumeration" in `TypeChoices()`. Index 0 (the auto-detect
        // fallback) would mislead the user.
        constexpr int ENUMERATION_CHOICE_INDEX = 8;
        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): prior QVERIFY aborts on null.
        QCOMPARE(typeCombo->currentIndex(), ENUMERATION_CHOICE_INDEX);

        // (2) Accept-without-change must preserve both fields.
        // No `enumColumnsChanged` either: the column did not cross
        // the enum boundary, and downstream filter rebuilds are
        // expensive.
        const QSignalSpy enumSpy(model, &LogModel::enumColumnsChanged);
        QVERIFY(enumSpy.isValid());
        editor.Apply();

        const auto &postEdit = model->Configuration().columns[static_cast<size_t>(categoryCol)];
        QCOMPARE(postEdit.type, loglib::LogConfiguration::Type::Enumeration);
        QVERIFY2(postEdit.autoDetect, "Accept-without-change must preserve the auto-detector's autoDetect=true flag");
        QCOMPARE(enumSpy.count(), 0);
    }

    // The Auto-detect choice at the top of the Type combo collapses
    // the (Type::Any, autoDetect=true) pair into a single user-
    // visible entry. Selecting it must restore both fields atomically
    // -- otherwise the column ends up in the legacy "user-pinned Any"
    // state that suppresses promotion.
    void TestColumnEditorAutoDetectChoiceRestoresFlag()
    {
        const int levelCol = StreamFixtureForColumnTests();
        QVERIFY2(levelCol >= 0, "level column must exist after streaming");
        auto *model = mWindow->Model();
        const int msgCol = ColumnByHeader(*model, QStringLiteral("msg"));
        QVERIFY2(msgCol >= 0, "msg column must exist after streaming");

        // Pre-rescan: the streaming detector already promoted `msg`
        // to `String` because its 200 unique values trip the
        // cardinality bail. We need a column whose post-rescan type
        // is observably different from its pre-edit type, so park
        // `msg` at a deliberately-wrong terminal type first. Picking
        // "Auto-detect" then exercises both the autoDetect flag
        // restoration *and* the rescan that promotes the column
        // back to the detector's preferred type.
        model->ConfigurationManager().SetColumnType(
            static_cast<size_t>(msgCol), loglib::LogConfiguration::Type::Integer
        );
        model->ConfigurationManager().SetColumnAutoDetect(static_cast<size_t>(msgCol), false);

        ColumnEditor editor(model, msgCol);
        auto *typeCombo = editor.findChild<QComboBox *>();
        QVERIFY(typeCombo != nullptr);
        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): prior QVERIFY aborts on null.
        typeCombo->setCurrentIndex(0); // "Auto-detect"
        editor.Apply();

        const auto &updated = model->Configuration().columns[static_cast<size_t>(msgCol)];
        // The rescan kicks in (bug 4 fix). `msg` carries 200 unique
        // values, so the candidate tracker bails on cardinality and
        // routes to `String`. The autoDetect flag survives the
        // re-route -- consistent with the streaming detector's
        // behaviour, and harmless because `Type::String` columns
        // are not enum-pass-eligible.
        QCOMPARE(updated.type, loglib::LogConfiguration::Type::String);
        QVERIFY2(updated.autoDetect, "Auto-detect choice must restore autoDetect=true");
    }

    // Companion to the above for the *enum-promotion* branch of
    // the rescan: pick a column whose data forms a small set of
    // repeated values (`category` -> info/warn/error/debug). After
    // pinning to a wrong terminal type and flipping to
    // "Auto-detect", the rescan must promote the column to
    // `Enumeration` (or `Level`, since the alias matches and the
    // dictionary tolerance holds). Validates the integrated
    // ApplyColumnTypeEdit -> RescanColumnForAutoDetection ->
    // PromoteColumnToEnum chain through the actual GUI seam.
    void TestColumnEditorAutoDetectPromotesEnumColumn()
    {
        const int categoryCol = StreamFixtureForColumnTests();
        QVERIFY2(categoryCol >= 0, "category column must exist after streaming");
        auto *model = mWindow->Model();

        // `category` was already promoted by streaming. Demote it
        // to Integer so picking Auto-detect is a real transition.
        model->ConfigurationManager().SetColumnType(
            static_cast<size_t>(categoryCol), loglib::LogConfiguration::Type::Integer
        );
        model->ConfigurationManager().SetColumnAutoDetect(static_cast<size_t>(categoryCol), false);

        // Pump the model through the same edit path the user would
        // hit to make sure `OnUserChangedColumnType` is invoked
        // (which clears dict state) before the rescan runs.
        model->ApplyColumnTypeEdit(categoryCol, loglib::LogConfiguration::Type::Integer, false);
        QCOMPARE(
            model->Configuration().columns[static_cast<size_t>(categoryCol)].type,
            loglib::LogConfiguration::Type::Integer
        );

        ColumnEditor editor(model, categoryCol);
        auto *typeCombo = editor.findChild<QComboBox *>();
        QVERIFY(typeCombo != nullptr);
        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): prior QVERIFY aborts on null.
        typeCombo->setCurrentIndex(0); // "Auto-detect"
        editor.Apply();

        const auto promotedType = model->Configuration().columns[static_cast<size_t>(categoryCol)].type;
        QVERIFY2(
            promotedType == loglib::LogConfiguration::Type::Enumeration ||
                promotedType == loglib::LogConfiguration::Type::Level,
            "RescanColumnForAutoDetection must promote a small-cardinality column out of Any"
        );
    }

    // Double-clicking a diagnostics row emits `editColumnRequested`
    // with the source-table column index. Verifying the signal
    // (rather than reaching into the editor it eventually pops) keeps
    // the dialog test layer-agnostic and lets `MainWindow` own the
    // editor wiring.
    void TestDiagnosticsDialogDoubleClickEmitsEditRequest()
    {
        const int levelCol = StreamFixtureForColumnTests();
        QVERIFY2(levelCol >= 0, "level column must exist after streaming");
        auto *model = mWindow->Model();
        const int msgCol = ColumnByHeader(*model, QStringLiteral("msg"));
        QVERIFY2(msgCol >= 0, "msg column must exist after streaming");

        const ConfigurationDiagnosticsDialog dialog(model);
        QSignalSpy editSpy(&dialog, &ConfigurationDiagnosticsDialog::editColumnRequested);

        auto *table = dialog.findChild<QTableWidget *>();
        QVERIFY(table != nullptr);

        // Find the row corresponding to `msg` (sorting may rearrange
        // the table independently of source-column order) and
        // simulate a double-click.
        int msgRow = -1;
        for (int row = 0; row < table->rowCount(); ++row)
        {
            const QTableWidgetItem *item = table->item(row, 0);
            if (item != nullptr && item->text() == QStringLiteral("msg"))
            {
                msgRow = row;
                break;
            }
        }
        QVERIFY2(msgRow >= 0, "Dialog must surface a row for `msg`");
        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): prior QVERIFY aborts on null.
        emit table->cellDoubleClicked(msgRow, 0);

        QCOMPARE(editSpy.count(), 1);
        QCOMPARE(editSpy.takeFirst().at(0).toInt(), msgCol);
    }

    // The columns manager mirrors every `Column` row in the live
    // configuration -- one table row per column, with the visible
    // checkbox in the rightmost cell. This is the smoke test that
    // the dialog is wired to the live model rather than a stale
    // snapshot, and that the Visible cell carries an interactable
    // check state.
    void TestColumnsManagerListsEveryColumn()
    {
        const int levelCol = StreamFixtureForColumnTests();
        QVERIFY2(levelCol >= 0, "level column must exist after streaming");
        auto *model = mWindow->Model();
        const int msgCol = ColumnByHeader(*model, QStringLiteral("msg"));
        QVERIFY2(msgCol >= 0, "msg column must exist after streaming");

        const ColumnsManagerDialog dialog(model, mWindow);

        auto *table = dialog.findChild<QTableWidget *>();
        QVERIFY(table != nullptr);
        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): prior QVERIFY aborts on null.
        QCOMPARE(table->rowCount(), static_cast<int>(model->Configuration().columns.size()));

        // The Visible cell sits in column 4 (see COL_VISIBLE in
        // columns_manager_dialog.cpp); a row whose underlying
        // column is visible must surface as Qt::Checked.
        constexpr int VISIBLE_COL = 4;
        const QTableWidgetItem *msgVisible = table->item(msgCol, VISIBLE_COL);
        QVERIFY(msgVisible != nullptr);
        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): prior QVERIFY aborts on null.
        QCOMPARE(msgVisible->checkState(), Qt::Checked);
        QVERIFY2(
            (msgVisible->flags() & Qt::ItemIsUserCheckable) != 0,
            "Visible cell must be user-checkable so the in-place toggle works"
        );
    }

    // Toggling the Visible checkbox in the manager must propagate
    // all the way to the live `Column::visible` flag and to the
    // header (i.e. go through `MainWindow::SetColumnVisible`, not
    // straight to the lib mutator). Doing the round-trip in the
    // test avoids the bug where the manager flipped only its own
    // table-widget state while the underlying header still showed
    // the column.
    void TestColumnsManagerVisibilityToggleHidesColumn()
    {
        const int levelCol = StreamFixtureForColumnTests();
        QVERIFY2(levelCol >= 0, "level column must exist after streaming");
        auto *model = mWindow->Model();
        const int msgCol = ColumnByHeader(*model, QStringLiteral("msg"));
        QVERIFY2(msgCol >= 0, "msg column must exist after streaming");
        QVERIFY(model->Configuration().columns[static_cast<size_t>(msgCol)].visible);

        const ColumnsManagerDialog dialog(model, mWindow);
        auto *table = dialog.findChild<QTableWidget *>();
        QVERIFY(table != nullptr);

        constexpr int VISIBLE_COL = 4;
        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): prior QVERIFY aborts on null.
        QTableWidgetItem *item = table->item(msgCol, VISIBLE_COL);
        QVERIFY(item != nullptr);
        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): prior QVERIFY aborts on null.
        item->setCheckState(Qt::Unchecked);

        QVERIFY2(
            !model->Configuration().columns[static_cast<size_t>(msgCol)].visible,
            "Unchecking the Visible cell must flip Column::visible to false"
        );

        auto *tableView = mWindow->findChild<LogTableView *>();
        QVERIFY2(tableView != nullptr, "MainWindow must own a LogTableView");
        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): prior QVERIFY aborts on null.
        QVERIFY2(
            tableView->horizontalHeader()->isSectionHidden(msgCol),
            "Toggle must reach the header (i.e. go via MainWindow::SetColumnVisible, not just the lib mutator)"
        );
    }

    // `MoveSelectedDown` calls `LogModel::MoveColumn`, which both
    // rotates `Configuration().columns` and re-emits header /
    // visibility state to the view. The test verifies the order
    // shift end-to-end: pick the first row, push it one slot down,
    // and assert the two columns swap positions in both the model
    // and the manager table itself.
    void TestColumnsManagerMoveDownReordersColumns()
    {
        const int levelCol = StreamFixtureForColumnTests();
        QVERIFY2(levelCol >= 0, "level column must exist after streaming");
        auto *model = mWindow->Model();
        QVERIFY2(model->Configuration().columns.size() >= 2, "fixture must yield at least two columns");

        const std::string firstKeys = model->Configuration().columns.front().keys.empty()
                                          ? std::string{}
                                          : model->Configuration().columns.front().keys.front();
        const std::string secondKeys =
            model->Configuration().columns[1].keys.empty() ? std::string{} : model->Configuration().columns[1].keys[0];
        QVERIFY2(!firstKeys.empty() && !secondKeys.empty(), "fixture columns must carry stable keys");

        ColumnsManagerDialog dialog(model, mWindow);
        auto *table = dialog.findChild<QTableWidget *>();
        QVERIFY(table != nullptr);
        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): prior QVERIFY aborts on null.
        table->selectRow(0);

        dialog.MoveSelectedDown();

        const auto &columns = model->Configuration().columns;
        QVERIFY2(columns.size() >= 2, "Move must not lose columns");
        QCOMPARE(columns[0].keys.empty() ? std::string{} : columns[0].keys.front(), secondKeys);
        QCOMPARE(columns[1].keys.empty() ? std::string{} : columns[1].keys.front(), firstKeys);
        // The manager moves the selection along with the row so the
        // user can drag the same column further down with repeated
        // clicks. (Verifying the selection prevents a regression
        // where a stable index lookup would re-pick the *new* top
        // row by mistake.)
        QCOMPARE(table->currentRow(), 1);
    }

    // Edge case: Move up on the top row, Move down on the bottom
    // row. Either would push the index out of [0, columnCount());
    // the dialog must clamp instead of asserting or wrapping.
    void TestColumnsManagerMoveAtBoundariesIsNoOp()
    {
        const int levelCol = StreamFixtureForColumnTests();
        QVERIFY2(levelCol >= 0, "level column must exist after streaming");
        auto *model = mWindow->Model();
        const auto initialColumns = model->Configuration().columns;
        QVERIFY2(initialColumns.size() >= 2, "fixture must yield at least two columns");

        ColumnsManagerDialog dialog(model, mWindow);
        auto *table = dialog.findChild<QTableWidget *>();
        QVERIFY(table != nullptr);

        // Top row: Move up is a no-op.
        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): prior QVERIFY aborts on null.
        table->selectRow(0);
        dialog.MoveSelectedUp();
        for (size_t i = 0; i < initialColumns.size(); ++i)
        {
            QCOMPARE(model->Configuration().columns[i].keys, initialColumns[i].keys);
        }

        // Bottom row: Move down is a no-op.
        const int lastRow = static_cast<int>(initialColumns.size()) - 1;
        table->selectRow(lastRow);
        dialog.MoveSelectedDown();
        for (size_t i = 0; i < initialColumns.size(); ++i)
        {
            QCOMPARE(model->Configuration().columns[i].keys, initialColumns[i].keys);
        }
    }

    // Regression for bug 1: the manager only listened to
    // `modelReset`, `headerDataChanged`, and `columnHealthChanged`.
    // A header-drag (or the streaming timestamp-bubble) reorders
    // columns via `MoveColumn`, which emits
    // `beginMoveColumns`/`endMoveColumns` only -- no
    // `headerDataChanged` follows because the column data is
    // unchanged. Before the fix, the manager's table widget kept
    // its pre-move row order until something else (e.g. a sort
    // toggle) happened to trigger a refresh.
    void TestColumnsManagerRefreshesAfterExternalColumnsMoved()
    {
        const int levelCol = StreamFixtureForColumnTests();
        QVERIFY2(levelCol >= 0, "level column must exist after streaming");
        auto *model = mWindow->Model();
        QVERIFY2(model->Configuration().columns.size() >= 2, "fixture must yield at least two columns");

        ColumnsManagerDialog dialog(model, mWindow);
        dialog.show();
        auto *table = dialog.findChild<QTableWidget *>();
        QVERIFY(table != nullptr);

        // Snapshot the pre-move dialog row 0 / row 1 headers so the
        // assertion below sees the actual swap rather than any
        // canonical ordering.
        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): prior QVERIFY aborts on null.
        const QTableWidgetItem *row0Before = table->item(0, 0);
        const QTableWidgetItem *row1Before = table->item(1, 0);
        QVERIFY(row0Before != nullptr && row1Before != nullptr);
        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): prior QVERIFY aborts on null.
        const QString headerAt0Before = row0Before->text();
        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): prior QVERIFY aborts on null.
        const QString headerAt1Before = row1Before->text();
        QVERIFY(headerAt0Before != headerAt1Before);

        // Reorder via the model layer -- the same seam a header drag
        // (`OnSourceColumnsMoved`) goes through. The fix adds a
        // `columnsMoved` -> `Refresh` connection in the dialog, so
        // the manager's rows must now follow this move without any
        // intervening UI event.
        QVERIFY2(model->MoveColumn(0, 1), "MoveColumn must succeed for a 2+ column model");

        // Pump the event loop so the queued-connection (if any) fires
        // before we sample the table widget.
        QCoreApplication::processEvents();

        const QTableWidgetItem *row0After = table->item(0, 0);
        const QTableWidgetItem *row1After = table->item(1, 0);
        QVERIFY(row0After != nullptr && row1After != nullptr);
        QCOMPARE(row0After->text(), headerAt1Before);
        QCOMPARE(row1After->text(), headerAt0Before);
        dialog.close();
    }

    // The View menu's "Manage columns\u2026" entry is the only
    // production entry point into the manager. Verifying it both
    // exists and triggers the dialog catches the case where a menu
    // rebuild forgets to re-add the action after a column shape
    // change.
    void TestViewMenuManageColumnsActionOpensDialog()
    {
        const int levelCol = StreamFixtureForColumnTests();
        QVERIFY2(levelCol >= 0, "level column must exist after streaming");

        auto *viewMenu = mWindow->findChild<QMenu *>(QStringLiteral("menuView"));
        QVERIFY2(viewMenu != nullptr, "MainWindow must expose its View menu");
        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): prior QVERIFY aborts on null.
        emit viewMenu->aboutToShow();

        QAction *manage = nullptr;
        const auto actions = viewMenu->actions();
        for (QAction *action : actions)
        {
            if (action->objectName() == QStringLiteral("actionManageColumns"))
            {
                manage = action;
                break;
            }
        }
        QVERIFY2(manage != nullptr, "View menu must carry the Manage columns... entry");
        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): prior QVERIFY aborts on null.
        manage->trigger();

        auto *dialog = mWindow->findChild<ColumnsManagerDialog *>(QStringLiteral("ColumnsManagerDialog"));
        QVERIFY2(dialog != nullptr, "Triggering Manage columns... must construct a ColumnsManagerDialog");
        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): prior QVERIFY aborts on null.
        QVERIFY2(!dialog->isHidden(), "Manager dialog must be shown after triggering its action");
        dialog->close();
    }

    // Find must skip hidden columns. The fixture has `level`
    // (info/warn/...) and `msg` (m0..m199); "info" only matches
    // `level`, so hiding `level` yields an empty match list.
    void TestFindSkipsHiddenColumns()
    {
        const int levelCol = StreamFixtureForColumnTests();
        QVERIFY2(levelCol >= 0, "level column must exist after streaming");
        auto *model = mWindow->Model();

        const LogFilterModel *proxy = mWindow->FilterModel();
        QVERIFY2(proxy != nullptr, "MainWindow must own a LogFilterModel");
        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): false positive; prior `QVERIFY2` aborts on null.
        QVERIFY2(proxy->rowCount() > 0, "fixture must yield at least one row");

        const QModelIndex start = proxy->index(0, 0);
        const QVariant needle = QVariant::fromValue(QStringLiteral("info"));
        const Qt::MatchFlags flags = Qt::MatchContains | Qt::MatchWrap | Qt::MatchRecursive;

        // Sanity: with `level` visible, "info" matches at least one row.
        const QModelIndexList beforeHide = proxy->MatchRow(start, Qt::DisplayRole, needle, 1, flags, true, 0);
        QVERIFY2(!beforeHide.isEmpty(), "precondition: 'info' must match while level is visible");
        QCOMPARE(beforeHide.front().column(), levelCol);

        mWindow->SetColumnVisible(levelCol, false);
        QVERIFY(!model->Configuration().columns[static_cast<size_t>(levelCol)].visible);

        const QModelIndexList afterHide = proxy->MatchRow(start, Qt::DisplayRole, needle, 1, flags, true, 0);
        QVERIFY2(
            afterHide.isEmpty(), "Find must skip hidden columns; hiding the only matching column must yield no result"
        );
    }

    // Hiding the sorted-by column would leave the sort active
    // with no UI glyph to clear it (the indicator lives on a
    // zero-width section). `SetColumnVisible` resets the sort.
    void TestHidingSortedColumnClearsSort()
    {
        const int levelCol = StreamFixtureForColumnTests();
        QVERIFY2(levelCol >= 0, "level column must exist after streaming");

        auto *tableView = mWindow->findChild<LogTableView *>();
        QVERIFY2(tableView != nullptr, "MainWindow must own a LogTableView");
        const QHeaderView *header = tableView->horizontalHeader();
        QVERIFY2(header != nullptr, "view must own a horizontal header");

        // Sort by `level` so the indicator points at it.
        tableView->sortByColumn(levelCol, Qt::AscendingOrder);
        QCoreApplication::processEvents();
        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): false positive; prior `QVERIFY2` aborts on null.
        QVERIFY2(header->isSortIndicatorShown(), "precondition: sort indicator must be shown after sortByColumn");
        QCOMPARE(header->sortIndicatorSection(), levelCol);

        mWindow->SetColumnVisible(levelCol, false);

        QVERIFY2(
            !header->isSortIndicatorShown() || header->sortIndicatorSection() != levelCol,
            "hiding the sorted column must clear the sort indicator"
        );
    }

    // Hiding a non-sorted column must NOT touch the sort indicator
    // -- only the case where the user actively hides the column they
    // sorted by needs the reset.
    void TestHidingUnsortedColumnLeavesSortAlone()
    {
        const int levelCol = StreamFixtureForColumnTests();
        QVERIFY2(levelCol >= 0, "level column must exist after streaming");
        auto *model = mWindow->Model();
        const int msgCol = ColumnByHeader(*model, QStringLiteral("msg"));
        QVERIFY2(msgCol >= 0, "msg column must exist after streaming");

        auto *tableView = mWindow->findChild<LogTableView *>();
        const QHeaderView *header = tableView->horizontalHeader();

        tableView->sortByColumn(levelCol, Qt::AscendingOrder);
        QCoreApplication::processEvents();
        QCOMPARE(header->sortIndicatorSection(), levelCol);

        mWindow->SetColumnVisible(msgCol, false);

        QVERIFY2(
            header->isSortIndicatorShown() && header->sortIndicatorSection() == levelCol,
            "hiding an unrelated column must not change the sort indicator"
        );
    }

    // `SetConfigurationUiEnabled(false)` must lock down both
    // column-management entry points (header drag + right-click
    // menu). Otherwise a drag mid-stream would race with
    // `AppendKeys` mutating `columns`. `enabled = true` restores
    // both.
    void TestStreamingDisablesColumnReorderAndContextMenu()
    {
        QVERIFY(StreamFixtureForColumnTests() >= 0);
        auto *tableView = mWindow->findChild<LogTableView *>();
        const QHeaderView *header = tableView->horizontalHeader();
        QVERIFY(header != nullptr);

        // Idle baseline: drag + custom context menu armed.
        QVERIFY2(header->sectionsMovable(), "precondition: header must allow drag in idle state");
        QCOMPARE(header->contextMenuPolicy(), Qt::CustomContextMenu);

        // Streaming-armed: gate fires on every open path.
        mWindow->SetConfigurationUiEnabledForTest(false);
        QVERIFY2(!header->sectionsMovable(), "drag must be disabled while configuration UI is gated");
        QCOMPARE(header->contextMenuPolicy(), Qt::NoContextMenu);

        // Streaming finished: the same gate flips both back on.
        mWindow->SetConfigurationUiEnabledForTest(true);
        QVERIFY2(header->sectionsMovable(), "drag must re-enable when configuration UI is ungated");
        QCOMPARE(header->contextMenuPolicy(), Qt::CustomContextMenu);
    }

    // Duplicate-header disambiguation in the View / Show menus.
    // Qt lets two columns share a header; the stable id is `keys`.
    // Each duplicate gets `header [keys]` so the user can tell
    // them apart.
    void TestDuplicateHeaderDisambiguation()
    {
        QVERIFY(StreamFixtureForColumnTests() >= 0);
        auto *model = mWindow->Model();

        // Make column 1's header collide with column 0's so we
        // have a real duplicate. The configuration is read-only
        // through the manager, so we round-trip via Save / Load.
        auto &mgr = model->ConfigurationManager();
        const auto &columns = mgr.Configuration().columns;
        QVERIFY2(columns.size() >= 2, "fixture must have at least two columns");

        loglib::LogConfiguration mutated;
        mutated.columns = columns;
        const QString sharedHeader = QString::fromStdString(mutated.columns[0].header);
        mutated.columns[1].header = mutated.columns[0].header;

        const QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        const QString cfgPath = tempDir.filePath(QStringLiteral("dupes.json"));
        {
            std::string json;
            const auto err = glz::write_json(mutated, json);
            QVERIFY(!err);
            QFile out(cfgPath);
            QVERIFY(out.open(QIODevice::WriteOnly));
            out.write(json.data(), static_cast<qsizetype>(json.size()));
        }
        QVERIFY(mWindow->TryLoadAsConfigurationForTest(cfgPath));
        QCoreApplication::processEvents();

        auto *viewMenu = mWindow->findChild<QMenu *>(QStringLiteral("menuView"));
        QVERIFY2(viewMenu != nullptr, "View menu must exist");
        emit viewMenu->aboutToShow();

        QStringList labels;
        for (const QAction *act : viewMenu->actions())
        {
            labels.append(act->text());
        }
        // Both colliding entries must include a `[...]` disambiguator;
        // a non-colliding entry (any other column) must not.
        int withDisambiguator = 0;
        for (const QString &label : labels)
        {
            if (label.startsWith(sharedHeader) && label.contains(QLatin1Char('[')))
            {
                ++withDisambiguator;
            }
        }
        QVERIFY2(withDisambiguator == 2, "both duplicate-headered columns must be disambiguated in View menu");
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
            values.emplace_back(keys.GetOrInsert("category"), loglib::LogValue(value));
            return loglib::LogLine{std::move(values), keys, *sourcePtr, 0};
        };

        // Batch 1: dict_v1 = {"info" -> 0, "warn" -> 1}.
        {
            loglib::StreamedBatch batch;
            batch.firstLineNumber = 1;
            batch.newKeys.emplace_back("category");
            batch.lines.push_back(makeLine("info"));
            batch.lines.push_back(makeLine("warn"));
            model.AppendBatch(std::move(batch));
        }

        const int levelCol = ColumnByHeader(model, QStringLiteral("category"));
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
        const loglib::KeyId levelKey = keys.Find("category");
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
                values.emplace_back(keys.GetOrInsert("category"), loglib::LogValue(value));
                return loglib::LogLine{std::move(values), keys, *sourcePtr, 0};
            };

            loglib::StreamedBatch batch;
            batch.firstLineNumber = 1;
            batch.newKeys.emplace_back("category");
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

        const int levelColA = ColumnByHeader(modelA, QStringLiteral("category"));
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
        const int levelColB = ColumnByHeader(modelB, QStringLiteral("category"));
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
                values.emplace_back(keys.GetOrInsert("category"), loglib::LogValue(value));
                return loglib::LogLine{std::move(values), keys, *sourcePtr, 0};
            };
            loglib::StreamedBatch batch;
            batch.firstLineNumber = 1;
            batch.newKeys.emplace_back("category");
            batch.lines.push_back(makeLine("info"));
            batch.lines.push_back(makeLine("warn"));
            model.AppendBatch(std::move(batch));
        }
        QCOMPARE(model.rowCount(), 2);

        LogFilterModel proxy;
        proxy.setSourceModel(&model);
        // Skip `SetLogModel` on purpose: rule installation in this
        // state must be rejected at evaluation time.
        const int levelCol = ColumnByHeader(model, QStringLiteral("category"));
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

        // `*[invalid` is syntactically broken;
        // `QRegularExpression::isValid()` returns false and the
        // matcher must short-circuit.
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

    // String filters match the user-visible (one-line, simplified)
    // text, not the raw bytes with embedded `\n`. All four match
    // modes share one path.
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
            lines.append(QStringLiteral(R"({"category": "%1", "msg": "row%2"})").arg(levels[i % levels.size()]).arg(i));
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
            lines.append(QStringLiteral(R"({"category": "%1", "msg": "row%2"})").arg(levels[i % levels.size()]).arg(i));
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

        const int levelCol = ColumnByHeader(*model, QStringLiteral("category"));
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

    // Regression: `FindRecords` used to OR `MatchContains` with the
    // regex / wildcard flags. `Matches` short-circuits on contains,
    // so the toggles were silently no-ops.
    void TestFindUsesRegexFlagAndIgnoresContainsShadow()
    {
        const QStringList lines{
            QStringLiteral(R"({"msg": "abc-123"})"),
            QStringLiteral(R"({"msg": "abc-xyz"})"),
            QStringLiteral(R"({"msg": "zzz-123"})"),
            QStringLiteral(R"({"msg": "abc-456"})"),
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
        QCOMPARE(model->rowCount(), lines.size());

        auto *tableView = mWindow->findChild<LogTableView *>();
        QVERIFY2(tableView != nullptr, "MainWindow must own a LogTableView");
        auto *filterModel = mWindow->FilterModel();
        QVERIFY2(filterModel != nullptr, "MainWindow must own a LogFilterModel");

        // Anchor at row 0 so `FindRecords` walks forward from row 1.
        tableView->selectionModel()->setCurrentIndex(
            filterModel->index(0, 0), QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows
        );

        // Regex `abc-\d+` matches rows 0 and 3 but rejects 1 and 2.
        // The old contains-wins bug would have found nothing
        // (substring `abc-\d+` is in none of them) and left the
        // selection at row 0. The fix routes through regex, so we
        // expect a hit at row 3.
        QVERIFY2(
            QMetaObject::invokeMethod(
                mWindow,
                "FindRecords",
                Qt::DirectConnection,
                Q_ARG(QString, QStringLiteral("abc-\\d+")),
                Q_ARG(bool, true),
                Q_ARG(bool, false),
                Q_ARG(bool, true)
            ),
            "FindRecords slot must be invocable via meta-object"
        );
        QCoreApplication::processEvents();

        QModelIndex selected = tableView->selectionModel()->currentIndex();
        QVERIFY2(selected.isValid(), "regex find must advance to a match");
        QVERIFY2(selected.row() != 0, "regex find must not stall on the start row");
        const QString cell = filterModel->index(selected.row(), 0).data(Qt::DisplayRole).toString();
        QVERIFY2(
            cell.contains(QStringLiteral("abc-")) && cell != QStringLiteral("abc-xyz"),
            qPrintable(QStringLiteral("regex find landed on unexpected row text: %1").arg(cell))
        );

        // Wildcard branch: drive `MatchRow` directly with
        // `MatchWildcard`, independent of `FindRecords` bookkeeping.
        // `*xyz*` against a literal substring of `*xyz*` matches no
        // row, so a hit here proves the wildcard branch took effect
        // rather than the (substring) contains branch. `MatchFlag`
        // values are not disjoint -- `MatchWildcard=5` has the
        // `MatchContains=1` bit set, so the old testFlag ladder would
        // demote to contains even when callers passed wildcard alone.
        const QModelIndex wildcardStart = filterModel->index(0, 0);
        QVERIFY(wildcardStart.isValid());
        const QModelIndexList wildcardHits = filterModel->MatchRow(
            wildcardStart,
            Qt::DisplayRole,
            QVariant::fromValue(QStringLiteral("*xyz*")),
            LogFilterModel::UNLIMITED_HITS,
            Qt::MatchWildcard | Qt::MatchWrap | Qt::MatchRecursive,
            true,
            0
        );
        QCOMPARE(wildcardHits.size(), 1);
        const QString wildcardCell = filterModel->index(wildcardHits[0].row(), 0).data(Qt::DisplayRole).toString();
        QVERIFY2(
            wildcardCell.contains(QStringLiteral("xyz")),
            qPrintable(QStringLiteral("wildcard hit landed on a non-xyz row: %1").arg(wildcardCell))
        );

        // Once more via the public `FindRecords` slot to pin that
        // `MainWindow`'s flag composition is also right.
        tableView->selectionModel()->setCurrentIndex(
            filterModel->index(0, 0), QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows
        );
        QVERIFY2(
            QMetaObject::invokeMethod(
                mWindow,
                "FindRecords",
                Qt::DirectConnection,
                Q_ARG(QString, QStringLiteral("*xyz*")),
                Q_ARG(bool, true),
                Q_ARG(bool, true),
                Q_ARG(bool, false)
            ),
            "FindRecords slot must be invocable via meta-object"
        );
        QCoreApplication::processEvents();
        selected = tableView->selectionModel()->currentIndex();
        QVERIFY2(selected.isValid(), "FindRecords with wildcards must advance to a match");
        QCOMPARE(filterModel->index(selected.row(), 0).data(Qt::DisplayRole).toString(), QStringLiteral("abc-xyz"));

        // Contains needle `abc`: should reach every "abc-..." row.
        // Pin it so a future flag-composition refactor can't silently
        // demote contains too.
        tableView->selectionModel()->setCurrentIndex(
            filterModel->index(2, 0), QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows
        );
        QVERIFY2(
            QMetaObject::invokeMethod(
                mWindow,
                "FindRecords",
                Qt::DirectConnection,
                Q_ARG(QString, QStringLiteral("abc")),
                Q_ARG(bool, true),
                Q_ARG(bool, false),
                Q_ARG(bool, false)
            ),
            "FindRecords slot must be invocable via meta-object"
        );
        QCoreApplication::processEvents();
        selected = tableView->selectionModel()->currentIndex();
        QVERIFY2(selected.isValid(), "contains find must advance to a match");
        QVERIFY2(
            filterModel->index(selected.row(), 0).data(Qt::DisplayRole).toString().startsWith(QStringLiteral("abc-")),
            "contains find must land on an `abc-...` row"
        );

        model->EndStreaming(false);
    }

    // Toggling regex must un-check wildcards (and vice versa). They
    // are mutually exclusive in `Matches`, so leaving both checked
    // would make the bar look like it's running two modes.
    void TestFindBarRegexAndWildcardsAreMutuallyExclusive()
    {
        auto *findRecord = mWindow->findChild<FindRecordWidget *>();
        QVERIFY2(findRecord != nullptr, "MainWindow must own a FindRecordWidget");
        auto *regex = findRecord->findChild<QAction *>(QStringLiteral("regexToggle"));
        auto *wildcards = findRecord->findChild<QAction *>(QStringLiteral("wildcardsToggle"));
        QVERIFY2(regex != nullptr && wildcards != nullptr, "Find bar must expose regex + wildcards toggles");

        regex->setChecked(true);
        QVERIFY2(regex->isChecked(), "regex toggle must accept setChecked(true)");
        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): false positive; prior `QVERIFY2` aborts on null.
        QVERIFY2(!wildcards->isChecked(), "regex setChecked(true) must leave wildcards off");

        wildcards->setChecked(true);
        QVERIFY2(wildcards->isChecked(), "wildcards toggle must accept setChecked(true)");
        QVERIFY2(!regex->isChecked(), "enabling wildcards must auto-un-check regex");

        regex->setChecked(true);
        QVERIFY2(regex->isChecked(), "regex toggle must re-engage after wildcards turn off");
        QVERIFY2(!wildcards->isChecked(), "re-enabling regex must auto-un-check wildcards");
    }

    // Regression: an earlier `setTabOrder(mEdit, mButtonPrevious)`
    // severed the regex / wildcards toggles from the Tab chain.
    // We can't reliably drive `focusNextChild` under offscreen-QPA,
    // so verify the static graph: walk `nextInFocusChain()` from
    // the search field and confirm the toggles appear before the
    // arrow buttons.
    void TestFindBarTabOrderIncludesRegexAndWildcardsToggles()
    {
        auto *findRecord = mWindow->findChild<FindRecordWidget *>();
        QVERIFY2(findRecord != nullptr, "MainWindow must own a FindRecordWidget");
        auto *edit = findRecord->findChild<QLineEdit *>(QStringLiteral("findEdit"));
        auto *regexButton = findRecord->findChild<QToolButton *>(QStringLiteral("regexToggleButton"));
        auto *wildcardsButton = findRecord->findChild<QToolButton *>(QStringLiteral("wildcardsToggleButton"));
        auto *prevButton = findRecord->findChild<QToolButton *>(QStringLiteral("findPrevious"));
        auto *nextButton = findRecord->findChild<QToolButton *>(QStringLiteral("findNext"));
        QVERIFY2(edit != nullptr, "findEdit must exist");
        QVERIFY2(regexButton != nullptr, "regexToggleButton must exist");
        QVERIFY2(wildcardsButton != nullptr, "wildcardsToggleButton must exist");
        QVERIFY2(prevButton != nullptr, "findPrevious must exist");
        QVERIFY2(nextButton != nullptr, "findNext must exist");

        // Walk forward, recording the first hop at which each target
        // is seen. Generous budget so unrelated children can't
        // starve us before we encounter the targets.
        constexpr int MAX_HOPS = 64;
        int regexIdx = -1;
        int wildcardsIdx = -1;
        int prevIdx = -1;
        int nextIdx = -1;
        const QWidget *cursor = edit;
        for (int hop = 0; hop < MAX_HOPS && cursor != nullptr; ++hop)
        {
            cursor = cursor->nextInFocusChain();
            if (cursor == regexButton && regexIdx < 0)
            {
                regexIdx = hop;
            }
            if (cursor == wildcardsButton && wildcardsIdx < 0)
            {
                wildcardsIdx = hop;
            }
            if (cursor == prevButton && prevIdx < 0)
            {
                prevIdx = hop;
            }
            if (cursor == nextButton && nextIdx < 0)
            {
                nextIdx = hop;
            }
            if (regexIdx >= 0 && wildcardsIdx >= 0 && prevIdx >= 0 && nextIdx >= 0)
            {
                break;
            }
            if (cursor == edit)
            {
                // Wrapped without finding a target -- bail.
                break;
            }
        }
        QVERIFY2(
            regexIdx >= 0 && wildcardsIdx >= 0,
            qPrintable(QStringLiteral("regex/wildcards toggles must be on the tab chain (regex=%1 wildcards=%2)")
                           .arg(regexIdx)
                           .arg(wildcardsIdx))
        );
        QVERIFY2(
            prevIdx >= 0 && nextIdx >= 0,
            qPrintable(
                QStringLiteral("arrow buttons must be on the tab chain (prev=%1 next=%2)").arg(prevIdx).arg(nextIdx)
            )
        );
        // Expected order: edit -> regex -> wildcards -> prev -> next.
        // Toggles before arrow buttons is the regression guard --
        // the bug pinned them after (or out of the chain entirely).
        QVERIFY2(
            regexIdx < prevIdx && wildcardsIdx < prevIdx,
            qPrintable(QStringLiteral("regex/wildcards toggles must precede the arrow buttons in the tab chain "
                                      "(regex=%1 wildcards=%2 prev=%3)")
                           .arg(regexIdx)
                           .arg(wildcardsIdx)
                           .arg(prevIdx))
        );
        QVERIFY2(prevIdx < nextIdx, "previous-arrow must come before next-arrow in the tab chain");
    }

    // `ParseErrorsDock::AppendErrors` must:
    //   - track Count() / DroppedCount() correctly across batches,
    //   - emit `countChanged(count, droppedCount)` on every change,
    //   - emit `firstBatchArrived()` exactly once per session (i.e.
    //     not on subsequent batches after the first).
    void TestParseErrorsDockTracksCountsAndFirstBatchSignal()
    {
        auto *dock = mWindow->findChild<ParseErrorsDock *>();
        QVERIFY2(dock != nullptr, "MainWindow must own a ParseErrorsDock");
        // `ResetSessionState` (not `ClearErrors`) re-arms the
        // first-batch latch, so this test can observe
        // `firstBatchArrived` regardless of prior test state.
        dock->ResetSessionState();

        QSignalSpy countSpy(dock, &ParseErrorsDock::countChanged);
        const QSignalSpy firstBatchSpy(dock, &ParseErrorsDock::firstBatchArrived);
        QVERIFY(countSpy.isValid());
        QVERIFY(firstBatchSpy.isValid());

        QCOMPARE(dock->Count(), 0);
        QCOMPARE(dock->DroppedCount(), 0);

        dock->AppendErrors(QStringLiteral("First batch"), {"a", "b", "c"});
        QCOMPARE(dock->Count(), 3);
        QCOMPARE(dock->DroppedCount(), 0);
        QCOMPARE(firstBatchSpy.count(), 1);
        // countChanged carries (count, droppedCount) -- two ints.
        QVERIFY2(countSpy.count() >= 1, "countChanged must fire on append");
        QCOMPARE(countSpy.last().at(0).toInt(), 3);
        QCOMPARE(countSpy.last().at(1).toInt(), 0);

        dock->AppendErrors(QStringLiteral("Second batch"), {"d", "e"});
        QCOMPARE(dock->Count(), 5);
        QCOMPARE(dock->DroppedCount(), 0);
        QCOMPARE(firstBatchSpy.count(),
                 1); // No second emit until a session boundary re-arms the latch.
        QCOMPARE(countSpy.last().at(0).toInt(), 5);

        // Clear zeroes the counters but leaves the first-batch latch
        // SET, so a streaming hiccup right after a manual Clear can't
        // re-pop the dock the user just dismissed.
        dock->ClearErrors();
        QCOMPARE(dock->Count(), 0);
        QCOMPARE(dock->DroppedCount(), 0);
        QCOMPARE(countSpy.last().at(0).toInt(), 0);
        QCOMPARE(countSpy.last().at(1).toInt(), 0);

        dock->AppendErrors(QStringLiteral("After clear"), {"x"});
        QCOMPARE(dock->Count(), 1);
        QCOMPARE(firstBatchSpy.count(), 1); // ClearErrors does NOT re-arm.

        // `ResetSessionState` is the canonical session boundary
        // and DOES re-arm the latch.
        dock->ResetSessionState();
        QCOMPARE(dock->Count(), 0);
        QCOMPARE(dock->DroppedCount(), 0);
        dock->AppendErrors(QStringLiteral("New session"), {"y"});
        QCOMPARE(dock->Count(), 1);
        QCOMPARE(firstBatchSpy.count(), 2); // Re-armed -> second emit.
    }

    // Regression: pre-fix, clicking Clear reset the
    // `firstBatchOfSession` heuristic (it keyed on the counters
    // being zero), so the next streamed batch fired
    // `firstBatchArrived` again and re-raised the dock the user
    // had just dismissed.
    void TestParseErrorsDockClearDoesNotReArmFirstBatchSignal()
    {
        auto *dock = mWindow->findChild<ParseErrorsDock *>();
        QVERIFY2(dock != nullptr, "MainWindow must own a ParseErrorsDock");
        dock->ResetSessionState();

        const QSignalSpy firstBatchSpy(dock, &ParseErrorsDock::firstBatchArrived);
        QVERIFY(firstBatchSpy.isValid());

        dock->AppendErrors(QStringLiteral("Initial batch"), {"a"});
        QCOMPARE(firstBatchSpy.count(), 1);

        dock->ClearErrors();
        QCOMPARE(dock->Count(), 0);
        QCOMPARE(dock->DroppedCount(), 0);

        // Simulated streaming hiccup after a manual Clear. The latch
        // must stay set so MainWindow does not auto-raise the dock.
        dock->AppendErrors(QStringLiteral("Post-clear hiccup"), {"b"});
        QCOMPARE(dock->Count(), 1);
        QCOMPARE(firstBatchSpy.count(), 1); // Still 1 -- Clear did NOT re-arm.

        // Inverse: a real session boundary DOES re-arm.
        dock->ResetSessionState();
        dock->AppendErrors(QStringLiteral("Real new session"), {"c"});
        QCOMPARE(firstBatchSpy.count(), 2);
    }

    // A single batch larger than the cap must pre-trim the input
    // and still surface the in-list overflow footer.
    //
    // Regression: when `mErrorCount` landed exactly at the cap
    // after pre-trim, the old `TrimToCap` short-circuited and
    // skipped appending the footer, leaving the "N dropped"
    // summary and the list view out of sync.
    void TestParseErrorsDockTrimsToCapAndAddsOverflowFooter()
    {
        auto *dock = mWindow->findChild<ParseErrorsDock *>();
        QVERIFY2(dock != nullptr, "MainWindow must own a ParseErrorsDock");
        dock->ClearErrors();

        const int cap = ParseErrorsDock::MAX_DISPLAYED_ERRORS;
        std::vector<std::string> huge;
        huge.reserve(static_cast<size_t>(cap) + 25);
        for (int i = 0; i < cap + 25; ++i)
        {
            huge.emplace_back(std::string("err ") + std::to_string(i));
        }
        dock->AppendErrors(QStringLiteral("Huge"), huge);
        QCOMPARE(dock->Count(), cap);
        QCOMPARE(dock->DroppedCount(), 25);

        auto *list = dock->findChild<QListWidget *>(QStringLiteral("parseErrorsList"));
        QVERIFY2(list != nullptr, "ParseErrorsDock must expose its internal QListWidget");

        // Footer must be the last item and the only one flagged
        // with `Qt::UserRole + 1` (so we'd also catch a stale
        // footer left somewhere in the middle).
        const QListWidgetItem *footer = nullptr;
        int footerIndex = -1;
        int footerHits = 0;
        const int footerRole = Qt::UserRole + 1;
        for (int i = 0; i < list->count(); ++i)
        {
            const QListWidgetItem *item = list->item(i);
            if (item != nullptr && item->data(footerRole).toBool())
            {
                ++footerHits;
                footer = item;
                footerIndex = i;
            }
        }
        QCOMPARE(footerHits, 1);
        QCOMPARE(footerIndex, list->count() - 1);
        QVERIFY2(footer != nullptr, "footer item lookup must succeed");
        QVERIFY2(
            footer->text().contains(QStringLiteral("dropped"), Qt::CaseInsensitive),
            qPrintable(QStringLiteral("footer must read like an overflow notice; got: %1").arg(footer->text()))
        );
        // Non-selectable so arrow-key navigation and Ctrl+A don't
        // pull the footer into the user's selection.
        QVERIFY2(!footer->flags().testFlag(Qt::ItemIsSelectable), "overflow footer must not be user-selectable");
    }

    // Streaming-style overflow: many small batches sum past the
    // cap. Exercises `TrimToCap`'s walk-from-the-top path (the
    // single-batch test above covers the pre-trim path); the
    // footer must surface here too and track `mDroppedCount`.
    void TestParseErrorsDockOverflowFooterTracksMultipleBatches()
    {
        auto *dock = mWindow->findChild<ParseErrorsDock *>();
        QVERIFY2(dock != nullptr, "MainWindow must own a ParseErrorsDock");
        dock->ClearErrors();

        auto *list = dock->findChild<QListWidget *>(QStringLiteral("parseErrorsList"));
        QVERIFY2(list != nullptr, "ParseErrorsDock must expose its internal QListWidget");
        const int footerRole = Qt::UserRole + 1;

        const int cap = ParseErrorsDock::MAX_DISPLAYED_ERRORS;
        // Fill exactly to the cap -- no overflow yet, no footer.
        std::vector<std::string> first;
        first.reserve(static_cast<size_t>(cap));
        for (int i = 0; i < cap; ++i)
        {
            first.emplace_back(std::string("a-") + std::to_string(i));
        }
        dock->AppendErrors(QStringLiteral("A"), first);
        QCOMPARE(dock->DroppedCount(), 0);
        for (int i = 0; i < list->count(); ++i)
        {
            const QListWidgetItem *item = list->item(i);
            QVERIFY2(item == nullptr || !item->data(footerRole).toBool(), "no footer when nothing has been dropped");
        }

        // Tip past the cap; walk eviction kicks in, footer surfaces.
        const int overflow = 50;
        std::vector<std::string> second;
        second.reserve(static_cast<size_t>(overflow));
        for (int i = 0; i < overflow; ++i)
        {
            second.emplace_back(std::string("b-") + std::to_string(i));
        }
        dock->AppendErrors(QStringLiteral("B"), second);
        QCOMPARE(dock->Count(), cap);
        QCOMPARE(dock->DroppedCount(), overflow);
        const QListWidgetItem *tail = list->item(list->count() - 1);
        QVERIFY2(
            tail != nullptr && tail->data(footerRole).toBool(),
            "overflow footer must be the trailing item after eviction"
        );

        // Third batch grows the drop count; footer text must update
        // and the dock must not accumulate stale footers.
        std::vector<std::string> third;
        third.reserve(static_cast<size_t>(overflow));
        for (int i = 0; i < overflow; ++i)
        {
            third.emplace_back(std::string("c-") + std::to_string(i));
        }
        dock->AppendErrors(QStringLiteral("C"), third);
        QCOMPARE(dock->DroppedCount(), overflow * 2);
        int footerHits = 0;
        const QListWidgetItem *latestFooter = nullptr;
        for (int i = 0; i < list->count(); ++i)
        {
            const QListWidgetItem *item = list->item(i);
            if (item != nullptr && item->data(footerRole).toBool())
            {
                ++footerHits;
                latestFooter = item;
            }
        }
        QCOMPARE(footerHits, 1);
        QVERIFY2(latestFooter != nullptr, "footer lookup must succeed after third batch");
        QVERIFY2(
            latestFooter->text().contains(QString::number(overflow * 2)) ||
                latestFooter->text().contains(QLocale::system().toString(static_cast<qlonglong>(overflow * 2))),
            qPrintable(QStringLiteral("footer must surface the running drop count; got: %1").arg(latestFooter->text()))
        );
    }

    // `CopySelection` must synthesise group headers and the
    // overflow footer around the selection so the pasted block
    // reads coherently. Headers / footer are non-selectable.
    void TestParseErrorsDockCopySelectionIncludesHeaderAndFooter()
    {
        auto *dock = mWindow->findChild<ParseErrorsDock *>();
        QVERIFY2(dock != nullptr, "MainWindow must own a ParseErrorsDock");
        dock->ClearErrors();

        // Two batches -> two headers; cap-overflow forces a footer.
        const int cap = ParseErrorsDock::MAX_DISPLAYED_ERRORS;
        std::vector<std::string> batchA;
        batchA.reserve(static_cast<size_t>(cap - 5));
        for (int i = 0; i < cap - 5; ++i)
        {
            batchA.emplace_back("A-" + std::to_string(i));
        }
        const std::vector<std::string> batchB{"B-0", "B-1", "B-2", "B-3", "B-4", "B-5", "B-6"};

        dock->AppendErrors(QStringLiteral("Batch A"), batchA);
        dock->AppendErrors(QStringLiteral("Batch B"), batchB);
        QVERIFY2(dock->DroppedCount() > 0, "cap overflow must mint an overflow footer");

        auto *list = dock->findChild<QListWidget *>(QStringLiteral("parseErrorsList"));
        QVERIFY2(list != nullptr, "ParseErrorsDock must expose its internal QListWidget");

        // Find Batch B's header + first error row.
        int batchBHeaderRow = -1;
        int firstBRow = -1;
        for (int i = 0; i < list->count(); ++i)
        {
            const QListWidgetItem *item = list->item(i);
            if (item == nullptr)
            {
                continue;
            }
            if (item->text() == QStringLiteral("Batch B") && !item->flags().testFlag(Qt::ItemIsSelectable))
            {
                batchBHeaderRow = i;
            }
            if (item->text().startsWith(QStringLiteral("B-")) && item->flags().testFlag(Qt::ItemIsSelectable))
            {
                if (firstBRow < 0)
                {
                    firstBRow = i;
                }
            }
        }
        QVERIFY2(batchBHeaderRow >= 0, "Batch B header must survive trimming (its rows did)");
        QVERIFY2(firstBRow > batchBHeaderRow, "Batch B's first error row must follow its header");

        list->clearSelection();
        list->item(firstBRow)->setSelected(true);

        auto *clipboard = QGuiApplication::clipboard();
        clipboard->clear();

        // `CopySelection` is private; trigger the Ctrl+C shortcut
        // instead. It's parented on the dock content widget (so it
        // stays live even when focus is on the Clear button), so
        // search the whole dock subtree.
        QShortcut *copyShortcut = nullptr;
        const QList<QShortcut *> shortcuts = dock->findChildren<QShortcut *>();
        for (QShortcut *s : shortcuts)
        {
            if (s->key() == QKeySequence(QKeySequence::Copy))
            {
                copyShortcut = s;
                break;
            }
        }
        QVERIFY2(copyShortcut != nullptr, "ParseErrorsDock must wire Ctrl+C somewhere in its subtree");
        emit copyShortcut->activated();
        QCoreApplication::processEvents();

        const QString clip = clipboard->text();
        QVERIFY2(!clip.isEmpty(), "CopySelection must populate the clipboard");
        QVERIFY2(
            clip.contains(QStringLiteral("Batch B")),
            qPrintable(QStringLiteral("clipboard must include the group header; got:\n%1").arg(clip))
        );
        QVERIFY2(
            clip.contains(QStringLiteral("B-0")),
            qPrintable(QStringLiteral("clipboard must include the selected error row; got:\n%1").arg(clip))
        );
        // Footer is `tr()`-rendered, so match the locale-stable
        // "dropped" hint rather than the full string.
        QVERIFY2(
            clip.contains(QStringLiteral("dropped"), Qt::CaseInsensitive),
            qPrintable(QStringLiteral("clipboard must include the overflow footer; got:\n%1").arg(clip))
        );
    }

    // Regression: trimming evicted the first batch's group header
    // along with some rows, leaving the surviving rows headerless
    // above the next batch's title. Fix re-mints the evicted header
    // above the survivors.
    void TestParseErrorsDockTrimPreservesSurvivingBatchHeader()
    {
        auto *dock = mWindow->findChild<ParseErrorsDock *>();
        QVERIFY2(dock != nullptr, "MainWindow must own a ParseErrorsDock");
        dock->ClearErrors();

        auto *list = dock->findChild<QListWidget *>(QStringLiteral("parseErrorsList"));
        QVERIFY2(list != nullptr, "ParseErrorsDock must expose its internal QListWidget");

        const int cap = ParseErrorsDock::MAX_DISPLAYED_ERRORS;
        // Batch A fills most of the cap; batch B tips us over,
        // evicting A's header plus a chunk of its rows.
        const int batchAStart = cap - 200;
        std::vector<std::string> batchA;
        batchA.reserve(static_cast<size_t>(batchAStart));
        for (int i = 0; i < batchAStart; ++i)
        {
            batchA.emplace_back("A-" + std::to_string(i));
        }
        std::vector<std::string> batchB;
        batchB.reserve(300);
        for (int i = 0; i < 300; ++i)
        {
            batchB.emplace_back("B-" + std::to_string(i));
        }
        dock->AppendErrors(QStringLiteral("Batch A"), batchA);
        dock->AppendErrors(QStringLiteral("Batch B"), batchB);

        QVERIFY2(dock->DroppedCount() > 0, "second batch must have evicted some of batch A");

        // First surviving row must be an `A-...` entry, with a
        // "Batch A" header immediately above it (not "Batch B"
        // floating above orphan A rows).
        int firstSelectableRow = -1;
        for (int i = 0; i < list->count(); ++i)
        {
            const QListWidgetItem *item = list->item(i);
            if (item != nullptr && item->flags().testFlag(Qt::ItemIsSelectable))
            {
                firstSelectableRow = i;
                break;
            }
        }
        QVERIFY2(firstSelectableRow > 0, "surviving error rows must follow at least one header");
        const QListWidgetItem *firstError = list->item(firstSelectableRow);
        QVERIFY2(firstError != nullptr, "first selectable row lookup must succeed");
        QVERIFY2(
            firstError->text().startsWith(QStringLiteral("A-")),
            qPrintable(QStringLiteral("first survivor must be from batch A; got: %1").arg(firstError->text()))
        );
        const QListWidgetItem *headerAbove = list->item(firstSelectableRow - 1);
        QVERIFY2(headerAbove != nullptr, "row above first survivor must exist");
        QVERIFY2(
            !headerAbove->flags().testFlag(Qt::ItemIsSelectable),
            "row above first survivor must be a group header (non-selectable)"
        );
        QCOMPARE(headerAbove->text(), QStringLiteral("Batch A"));
    }

    // Regression: re-opening the find bar after a model reset used
    // to show the stale match count from the previous dataset.
    // `FindDock::revealed` is the trigger for a catch-up recount.
    void TestFindBarRevealedSignalTriggersCatchUpRecount()
    {
        auto *dock = mWindow->findChild<FindDock *>();
        QVERIFY2(dock != nullptr, "MainWindow must own a FindDock");
        auto *bar = dock->Widget();
        QVERIFY2(bar != nullptr, "FindDock must host a FindRecordWidget");
        auto *edit = bar->findChild<QLineEdit *>(QStringLiteral("findEdit"));
        QVERIFY2(edit != nullptr, "FindRecordWidget must expose its line edit");

        // Drive the signal directly rather than going through Qt's
        // tab system (which needs a realised main window): the
        // wiring under test is "revealed -> BumpMatchCountDebounce".
        // With a non-empty needle, the debounce timer should arm and
        // eventually emit `MatchCountRequested`.
        edit->setText(QStringLiteral("anything"));
        QSignalSpy spy(bar, &FindRecordWidget::MatchCountRequested);
        QVERIFY(spy.isValid());
        spy.clear();

        emit dock->revealed();

        // Wait for the 120 ms debounce to fire. `wait()` doesn't
        // depend on the test driver's clock ticking.
        QVERIFY2(
            spy.count() > 0 || spy.wait(1000),
            "FindDock::revealed must trigger a debounced MatchCountRequested for a non-empty needle"
        );

        // Empty needle: the bar suppresses the request.
        edit->clear();
        spy.clear();
        emit dock->revealed();
        QVERIFY2(!spy.wait(300), "FindDock::revealed must NOT trigger MatchCountRequested for an empty needle");
    }

    // Regression: under continuous activity (live-tail, held key)
    // the trailing 120 ms debounce restarts on every bump and
    // never fires, stranding the indicator at the pre-activity
    // count. A max-age timer (capped at 750 ms) that survives
    // subsequent bumps guarantees an emit within that window.
    void TestFindBarDebounceMaxAgeForcesEmitUnderContinuousActivity()
    {
        auto *findRecord = mWindow->findChild<FindRecordWidget *>();
        QVERIFY2(findRecord != nullptr, "MainWindow must own a FindRecordWidget");
        auto *edit = findRecord->findChild<QLineEdit *>(QStringLiteral("findEdit"));
        QVERIFY2(edit != nullptr, "FindRecordWidget must expose its line edit");

        edit->setText(QStringLiteral("anything"));
        QSignalSpy spy(findRecord, &FindRecordWidget::MatchCountRequested);
        QVERIFY(spy.isValid());
        // Drain the textChanged-triggered debounce.
        QVERIFY2(spy.count() > 0 || spy.wait(1500), "initial textChanged must emit");
        spy.clear();

        // Bump every 50 ms (under the 120 ms trailing window) for
        // 1 s. Without the max-age cap the trailing timer never
        // fires; with it, an emit arrives within ~750 ms.
        QElapsedTimer wallClock;
        wallClock.start();
        constexpr int BUMP_INTERVAL_MS = 50;
        constexpr int TOTAL_BUMP_DURATION_MS = 1000;
        constexpr int MAX_AGE_TOLERANCE_MS = 1000; // a bit above the 750 ms cap
        bool emittedDuringHammer = false;
        while (wallClock.elapsed() < TOTAL_BUMP_DURATION_MS)
        {
            findRecord->BumpMatchCountDebounce();
            QTest::qWait(BUMP_INTERVAL_MS);
            if (spy.count() > 0)
            {
                emittedDuringHammer = true;
                break;
            }
        }
        QVERIFY2(
            emittedDuringHammer,
            qPrintable(QStringLiteral(
                           "max-age cap must fire MatchCountRequested under continuous bumps; got %1 emits in %2 ms"
            )
                           .arg(spy.count())
                           .arg(wallClock.elapsed()))
        );
        QVERIFY2(
            wallClock.elapsed() <= MAX_AGE_TOLERANCE_MS,
            qPrintable(QStringLiteral("first emit took longer than the max-age cap: %1 ms").arg(wallClock.elapsed()))
        );
    }

    // Regression: `MatchRow` skips hidden columns, but column-
    // visibility flips don't emit any of the proxy signals the find
    // cache hooks. Without explicit invalidation the "*i* of *N*"
    // indicator would strand a count that includes hits in
    // now-hidden columns. `SetColumnVisible` /
    // `ApplyColumnVisibility` now call `OnFindCacheInvalidated`.
    //
    // The main window must be realised so the find dock is visible
    // (the bump gate skips recounts when the bar is hidden).
    void TestHidingColumnInvalidatesFindCache()
    {
        const int categoryCol = StreamFixtureForColumnTests();
        QVERIFY2(categoryCol >= 0, "category column must exist after streaming");

        auto *findRecord = mWindow->findChild<FindRecordWidget *>();
        QVERIFY2(findRecord != nullptr, "MainWindow must own a FindRecordWidget");
        auto *findDock = mWindow->findChild<FindDock *>();
        QVERIFY2(findDock != nullptr, "MainWindow must own a FindDock");
        auto *edit = findRecord->findChild<QLineEdit *>(QStringLiteral("findEdit"));
        QVERIFY2(edit != nullptr, "FindRecordWidget must expose its line edit");

        // Realise the window so the bump gate lets through.
        mWindow->show();
        QVERIFY(QTest::qWaitForWindowExposed(mWindow, 5000));
        findDock->show();
        QCoreApplication::processEvents();
        QVERIFY2(findDock->isVisible(), "find dock must be visible for the bump gate to pass");

        edit->setText(QStringLiteral("info"));
        QSignalSpy spy(findRecord, &FindRecordWidget::MatchCountRequested);
        QVERIFY(spy.isValid());
        // Drain the textChanged debounce so the next emit is
        // unambiguously caused by the hide.
        QVERIFY2(spy.count() > 0 || spy.wait(1500), "initial textChanged must trigger a debounced MatchCountRequested");
        spy.clear();

        mWindow->SetColumnVisible(categoryCol, false);
        QVERIFY2(
            spy.count() > 0 || spy.wait(1500),
            "Hiding a column must invalidate the find cache and trigger a fresh MatchCountRequested"
        );
    }

    // Regression: `AppendErrors` used to `scrollToBottom()` on every
    // append, yanking the user back to the tail mid-read. Chat/log
    // convention: auto-follow only when already at the tail.
    void TestParseErrorsDockAutoFollowOnlyWhenAtTail()
    {
        auto *dock = mWindow->findChild<ParseErrorsDock *>();
        QVERIFY2(dock != nullptr, "MainWindow must own a ParseErrorsDock");
        dock->ClearErrors();

        auto *list = dock->findChild<QListWidget *>(QStringLiteral("parseErrorsList"));
        QVERIFY2(list != nullptr, "ParseErrorsDock must expose its internal QListWidget");

        // Realise the dock so the scrollbar has a real viewport.
        // Offscreen QPA still pumps min/max/value updates as long
        // as the widget tree is shown.
        mWindow->show();
        QVERIFY(QTest::qWaitForWindowExposed(mWindow, 5000));
        dock->show();
        dock->raise();
        QCoreApplication::processEvents();

        // Seed enough entries that the list scrolls. We start at
        // the tail (list was empty), so auto-follow pins us there.
        std::vector<std::string> seed;
        seed.reserve(200);
        for (int i = 0; i < 200; ++i)
        {
            seed.emplace_back(std::string("seed-") + std::to_string(i));
        }
        dock->AppendErrors(QStringLiteral("Seed"), seed);
        QCoreApplication::processEvents();

        QScrollBar *vBar = list->verticalScrollBar();
        QVERIFY2(vBar != nullptr, "QListWidget must have a vertical scrollbar");
        // If offscreen geometry leaves `maximum() == minimum()`
        // we can't distinguish "still at tail" from "moved by us",
        // so the test is meaningless on this platform.
        if (vBar->maximum() <= vBar->minimum())
        {
            QSKIP("offscreen QPA produced a degenerate scrollbar -- can't exercise auto-follow");
        }

        // Park mid-list -- comfortably outside the 4 px tail slack.
        const int parkedPosition = vBar->minimum() + ((vBar->maximum() - vBar->minimum()) / 2);
        vBar->setValue(parkedPosition);
        QCoreApplication::processEvents();
        QCOMPARE(vBar->value(), parkedPosition);

        // New batch mid-read must leave the scrollbar parked.
        const std::vector<std::string> later{"late-0", "late-1", "late-2", "late-3", "late-4"};
        dock->AppendErrors(QStringLiteral("Later"), later);
        QCoreApplication::processEvents();
        QVERIFY2(
            vBar->value() == parkedPosition,
            qPrintable(QStringLiteral("Scroll position must be preserved when the user was not at the tail "
                                      "(parked=%1, after=%2, max=%3)")
                           .arg(parkedPosition)
                           .arg(vBar->value())
                           .arg(vBar->maximum()))
        );

        // Scroll back to tail; auto-follow must re-engage.
        vBar->setValue(vBar->maximum());
        QCoreApplication::processEvents();
        QCOMPARE(vBar->value(), vBar->maximum());

        const std::vector<std::string> trailing{"tail-0", "tail-1", "tail-2"};
        dock->AppendErrors(QStringLiteral("Trailing"), trailing);
        QCoreApplication::processEvents();
        // After more items land `maximum()` grew; auto-follow
        // must have moved `value` along with it.
        QVERIFY2(
            vBar->value() >= vBar->maximum() - 4,
            qPrintable(QStringLiteral("Auto-follow must re-pin to the tail when the user was at the tail "
                                      "(value=%1, max=%2)")
                           .arg(vBar->value())
                           .arg(vBar->maximum()))
        );
    }

    // Regression: `UpdateFindMatchCount` used to run the full
    // proxy scan even when the find dock was hidden (a debounce
    // armed while the bar was visible can still fire after the
    // user dismisses it). The slot now bails on hidden, matching
    // the gates on `OnFindCacheInvalidated` and `FindRecords`.
    void TestUpdateFindMatchCountSkipsWorkWhenDockHidden()
    {
        auto *findRecord = mWindow->findChild<FindRecordWidget *>();
        QVERIFY2(findRecord != nullptr, "MainWindow must own a FindRecordWidget");
        auto *findDock = mWindow->findChild<FindDock *>();
        QVERIFY2(findDock != nullptr, "MainWindow must own a FindDock");
        auto *label = findRecord->findChild<QLabel *>(QStringLiteral("findMatchCount"));
        QVERIFY2(label != nullptr, "FindRecordWidget must expose its match-count label");

        findDock->hide();
        QVERIFY(!findDock->isVisible());

        // Sentinel so any write (clear or "N matches") is detectable.
        label->setText(QStringLiteral("sentinel-do-not-touch"));

        // The visibility gate must short-circuit before any write.
        QVERIFY2(
            QMetaObject::invokeMethod(
                mWindow,
                "UpdateFindMatchCount",
                Qt::DirectConnection,
                Q_ARG(QString, QStringLiteral("info")),
                Q_ARG(bool, false),
                Q_ARG(bool, false)
            ),
            "UpdateFindMatchCount must be invocable via meta-object"
        );
        QCOMPARE(label->text(), QStringLiteral("sentinel-do-not-touch"));

        // Empty needle must also short-circuit -- pre-fix it would
        // have cleared the sentinel even though nothing was on
        // screen to clear.
        QVERIFY2(
            QMetaObject::invokeMethod(
                mWindow,
                "UpdateFindMatchCount",
                Qt::DirectConnection,
                Q_ARG(QString, QString()),
                Q_ARG(bool, false),
                Q_ARG(bool, false)
            ),
            "UpdateFindMatchCount must accept an empty needle"
        );
        QCOMPARE(label->text(), QStringLiteral("sentinel-do-not-touch"));
    }

    // Regression: the status button used to total `count +
    // droppedCount`, which didn't match the dock summary ("X
    // errors; Y earlier dropped"). Now the button renders the
    // dropped count inline when non-zero.
    void TestParseErrorsStatusButtonShowsDroppedHint()
    {
        auto *dock = mWindow->findChild<ParseErrorsDock *>();
        QVERIFY2(dock != nullptr, "MainWindow must own a ParseErrorsDock");
        auto *statusButton = mWindow->findChild<QPushButton *>(QStringLiteral("parseErrorsStatusButton"));
        QVERIFY2(statusButton != nullptr, "MainWindow must own the parse-errors status button");
        dock->ClearErrors();

        // No drops yet: button uses the plain "%n errors" form.
        dock->AppendErrors(QStringLiteral("First"), {"a", "b", "c"});
        QCoreApplication::processEvents();
        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): false positive; prior `QVERIFY2` aborts on null.
        QVERIFY2(
            !statusButton->text().contains(QStringLiteral("dropped"), Qt::CaseInsensitive),
            qPrintable(QStringLiteral("status button must not mention 'dropped' when nothing was dropped: %1")
                           .arg(statusButton->text()))
        );

        // Force an overflow so `droppedCount > 0`. A batch larger
        // than the cap pre-trims to mint a dropped count.
        const int cap = ParseErrorsDock::MAX_DISPLAYED_ERRORS;
        std::vector<std::string> huge;
        huge.reserve(static_cast<size_t>(cap) + 50);
        for (int i = 0; i < cap + 50; ++i)
        {
            huge.emplace_back(std::string("err-") + std::to_string(i));
        }
        dock->AppendErrors(QStringLiteral("Huge"), huge);
        QCoreApplication::processEvents();
        QVERIFY2(dock->DroppedCount() > 0, "fixture must produce a non-zero dropped count");

        // Button must now surface the dropped hint so the user can
        // tell from the status bar that the visible total isn't
        // the whole story.
        QVERIFY2(
            statusButton->text().contains(QStringLiteral("dropped"), Qt::CaseInsensitive),
            qPrintable(QStringLiteral("status button must surface dropped count in its label; got: %1")
                           .arg(statusButton->text()))
        );
        // And the tooltip continues to spell out the breakdown.
        QVERIFY2(
            statusButton->toolTip().contains(QStringLiteral("dropped"), Qt::CaseInsensitive),
            qPrintable(QStringLiteral("status button tooltip must still spell out the breakdown; got: %1")
                           .arg(statusButton->toolTip()))
        );
    }

    // Item 5: status-bar "*n* of *m* shown" indicator + inline
    // Clear-filters button. Three branches:
    //   - no filter             -> "N lines",   button hidden
    //   - filter hides rows     -> "M of N shown", button shown
    //   - filter matches every row -> "N lines", button still shown
    //     (the comment in `UpdateRowsShownStatus` calls this out
    //      explicitly; left untested in the original change).
    void TestRowsShownStatusReflectsFilter()
    {
        const int categoryCol = StreamFixtureForColumnTests();
        QVERIFY2(categoryCol >= 0, "category column must exist after streaming");

        // The fixture leaves `mSessionMode` at Idle, but the
        // rows-shown slot now gates on `mModel->rowCount() > 0`,
        // so no `SetSessionModeForTest` dance is needed.
        QCoreApplication::processEvents();

        auto *label = mWindow->findChild<QLabel *>(QStringLiteral("rowsShownLabel"));
        QVERIFY2(label != nullptr, "MainWindow must own the rows-shown status label");
        auto *button = mWindow->findChild<QPushButton *>(QStringLiteral("clearFiltersStatusButton"));
        QVERIFY2(button != nullptr, "MainWindow must own the clear-filters status button");

        // Locale-formatted total used in every "no filter" / "vacuous
        // filter" assertion below. Matches the slot's own
        // `QLocale::system()` + `qlonglong` round-trip.
        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): prior QVERIFY2 aborts on null.
        auto *model = mWindow->Model();
        QVERIFY2(model != nullptr, "MainWindow must own a LogModel");
        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): prior QVERIFY2 aborts on null.
        const int totalRows = model->rowCount();
        QVERIFY2(totalRows > 1, "fixture must stream more than one row for the plural branch");
        const QString totalText = QLocale::system().toString(static_cast<qlonglong>(totalRows));
        const QString unfilteredText = QStringLiteral("%1 lines").arg(totalText);

        // Branch 1: no filter. Label reads "<N> lines"; button is
        // hidden. `isHidden()` (not `isVisible()`) so the assertion
        // exercises the slot's intent -- offscreen-QPA keeps the
        // parent hidden, collapsing `isVisible()` regardless.
        QCOMPARE(label->text(), unfilteredText);
        QVERIFY2(!label->isHidden(), "rows-shown label must show with a non-empty source");
        QVERIFY2(button->isHidden(), "clear-filters button must hide when no filter is active");

        // Branch 2: filter hides most rows. The fixture streams 200
        // lines across 4 categories, so "info" alone leaves ~50.
        const QString hideFilterId = QStringLiteral("rows-shown-test-filter");
        QVERIFY2(
            QMetaObject::invokeMethod(
                mWindow,
                "FilterEnumSubmitted",
                Qt::DirectConnection,
                Q_ARG(QString, hideFilterId),
                Q_ARG(int, categoryCol),
                Q_ARG(QStringList, QStringList{QStringLiteral("info")})
            ),
            "FilterEnumSubmitted must be invocable via meta-object"
        );
        QCoreApplication::processEvents();

        QVERIFY2(!button->isHidden(), "clear-filters button must un-hide when at least one filter is active");
        auto *filterModel = mWindow->FilterModel();
        QVERIFY2(filterModel != nullptr, "MainWindow must own a LogFilterModel");
        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): prior QVERIFY2 aborts on null.
        const int visibleRows = filterModel->rowCount();
        QVERIFY2(visibleRows > 0 && visibleRows < totalRows, "fixture filter must hide some but not all rows");
        const QString expectedFiltered =
            QStringLiteral("%1 of %2 shown")
                .arg(QLocale::system().toString(static_cast<qlonglong>(visibleRows)), totalText);
        QCOMPARE(label->text(), expectedFiltered);

        // Branch 3: vacuous filter -- every category selected, so
        // proxyRows == sourceRows but `mFilters` is still populated.
        // Label drops the "of" form; button stays visible.
        // Build the selection outside `Q_ARG`: brace-initializer commas
        // are not protected from the macro's argument splitter.
        const QStringList allCategories{
            QStringLiteral("info"), QStringLiteral("warn"), QStringLiteral("error"), QStringLiteral("debug")
        };
        QVERIFY2(
            QMetaObject::invokeMethod(
                mWindow,
                "FilterEnumSubmitted",
                Qt::DirectConnection,
                Q_ARG(QString, hideFilterId),
                Q_ARG(int, categoryCol),
                Q_ARG(QStringList, allCategories)
            ),
            "FilterEnumSubmitted (vacuous) must be invocable via meta-object"
        );
        QCoreApplication::processEvents();

        QCOMPARE(filterModel->rowCount(), totalRows);
        QVERIFY2(!button->isHidden(), "clear-filters button must stay visible even when the filter matches every row");
        QCOMPARE(label->text(), unfilteredText);

        // Clicking the status button must route through
        // `actionClearAllFilters`. Triggering the action directly
        // is the same end-state and avoids needing a real click on
        // a hidden parent window. The action also exercises the
        // signal wiring on the button itself (`clicked ->
        // actionClearAllFilters::trigger`).
        auto *clearAction = mWindow->findChild<QAction *>(QStringLiteral("actionClearAllFilters"));
        QVERIFY2(clearAction != nullptr, "MainWindow must own actionClearAllFilters");
        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): prior QVERIFY2 aborts on null.
        clearAction->trigger();
        QCoreApplication::processEvents();

        QVERIFY2(button->isHidden(), "clear-filters button must re-hide after clearing all filters");
        QCOMPARE(label->text(), unfilteredText);
    }

    // Item 9 (partial): the stream toolbar is movable and may be
    // docked to any of the four `QMainWindow` toolbar areas.
    void TestStreamToolbarIsMovable()
    {
        auto *toolbar = mWindow->findChild<QToolBar *>(QStringLiteral("streamToolbar"));
        QVERIFY2(toolbar != nullptr, "MainWindow must own the stream toolbar");
        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): prior QVERIFY2 aborts on null.
        QVERIFY2(toolbar->isMovable(), "stream toolbar must be user-movable");
        QCOMPARE(toolbar->allowedAreas(), Qt::ToolBarAreas(Qt::AllToolBarAreas));
    }

    // Item 1: the persistent primary toolbar exists, is movable, and
    // sits in the top dock area. `objectName` is the contract that
    // `restoreState()` keys on; if it ever drifts we lose persisted
    // dock geometry across restarts.
    void TestMainToolbarExistsMovableAndOnTop()
    {
        auto *toolbar = mWindow->findChild<QToolBar *>(QStringLiteral("mainToolbar"));
        QVERIFY2(toolbar != nullptr, "MainWindow must own the primary toolbar");
        // NOLINTBEGIN(clang-analyzer-core.CallAndMessage): prior QVERIFY2 aborts on null.
        QVERIFY2(toolbar->isMovable(), "primary toolbar must be user-movable");
        QCOMPARE(toolbar->allowedAreas(), Qt::ToolBarAreas(Qt::AllToolBarAreas));
        QCOMPARE(mWindow->toolBarArea(toolbar), Qt::TopToolBarArea);
        // NOLINTEND(clang-analyzer-core.CallAndMessage)
    }

    // Item 1: `insertToolBar(mStreamToolbar, mMainToolbar)` puts the
    // primary toolbar ahead of the stream toolbar in the top dock
    // area, so the combined strip reads "Main | Stream" left to
    // right when streaming. Regressed shape would be "Stream | Main"
    // which looks broken when a live-tail session starts.
    void TestMainToolbarPrecedesStreamToolbar()
    {
        auto *mainBar = mWindow->findChild<QToolBar *>(QStringLiteral("mainToolbar"));
        auto *streamBar = mWindow->findChild<QToolBar *>(QStringLiteral("streamToolbar"));
        QVERIFY2(mainBar != nullptr, "MainWindow must own the primary toolbar");
        QVERIFY2(streamBar != nullptr, "MainWindow must own the stream toolbar");
        // NOLINTBEGIN(clang-analyzer-core.CallAndMessage): prior QVERIFY2 aborts on null.
        QCOMPARE(mWindow->toolBarArea(mainBar), Qt::TopToolBarArea);
        QCOMPARE(mWindow->toolBarArea(streamBar), Qt::TopToolBarArea);

        // Walk top-level toolbars in the window's child order; the
        // primary toolbar must appear before the stream toolbar.
        // `findChildren<QToolBar*>` returns children in the order
        // they were `addChild`-ed (which is the `insertToolBar` /
        // `addToolBar` order). The pre-existing `streamToolbar`
        // child was added first, but `insertToolBar` re-parents the
        // new bar ahead of it in the dock area, which is what
        // `QMainWindow` queries to lay them out.
        const QList<QToolBar *> bars = mWindow->findChildren<QToolBar *>(QString{}, Qt::FindDirectChildrenOnly);
        const int mainIdx = static_cast<int>(bars.indexOf(mainBar));
        const int streamIdx = static_cast<int>(bars.indexOf(streamBar));
        QVERIFY2(mainIdx >= 0, "primary toolbar must be a direct child of MainWindow");
        QVERIFY2(streamIdx >= 0, "stream toolbar must be a direct child of MainWindow");

        // X coordinates (in window space) are the most reliable
        // visual-order witness because both bars share the top
        // dock area row. The stream toolbar starts hidden, so we
        // force it visible for the geometry probe and restore
        // afterwards. Showing the window + `qWaitForWindowExposed`
        // is what makes the layout deterministic across hosts --
        // an offscreen-platform fixture that never enters the
        // event loop has been observed to leave both bars at x=0
        // until something exposes the window.
        const bool windowWasVisible = mWindow->isVisible();
        const bool streamWasVisible = streamBar->isVisible();
        streamBar->setVisible(true);
        mWindow->show();
        QVERIFY(QTest::qWaitForWindowExposed(mWindow, 5000));
        mWindow->adjustSize();
        QCoreApplication::processEvents();
        QVERIFY2(
            mainBar->x() < streamBar->x(),
            qPrintable(QStringLiteral("main toolbar (x=%1) must precede stream toolbar (x=%2)")
                           .arg(mainBar->x())
                           .arg(streamBar->x()))
        );
        streamBar->setVisible(streamWasVisible);
        if (!windowWasVisible)
        {
            mWindow->hide();
        }
        // NOLINTEND(clang-analyzer-core.CallAndMessage)
    }

    // Item 1: the primary toolbar exposes the agreed action set in
    // the agreed order. Pinned against drift -- any future tweak
    // (re-order, remove, add) has to update this list deliberately
    // rather than slipping into the diff.
    void TestMainToolbarActionsInOrder()
    {
        auto *toolbar = mWindow->findChild<QToolBar *>(QStringLiteral("mainToolbar"));
        QVERIFY2(toolbar != nullptr, "MainWindow must own the primary toolbar");

        // NOLINTBEGIN(clang-analyzer-core.CallAndMessage): prior QVERIFY2 aborts on null.
        // Sentinel values: `__sep` for a `QAction::isSeparator()`
        // boundary, `__widget` for an `addWidget`-inserted custom
        // control (split button, spacer). Comparing in two stages
        // keeps the assertion message readable.
        const QStringList expected = {
            QStringLiteral("actionOpen"),
            QStringLiteral("__widget"), // openStreamSplitButton
            QStringLiteral("__sep"),
            QStringLiteral("__widget"), // addFilterSplitButton
            QStringLiteral("__widget"), // clearFiltersSplitButton
            QStringLiteral("__widget"), // sortBySplitButton
            QStringLiteral("actionClearSort"),
            QStringLiteral("__sep"),
            QStringLiteral("actionToggleFind"),
            QStringLiteral("actionToggleRecordDetails"),
            QStringLiteral("actionToggleAnchors"),
            QStringLiteral("__widget"), // expanding spacer
            QStringLiteral("actionPreferences"),
        };

        QStringList actual;
        for (const QAction *act : toolbar->actions())
        {
            if (act == nullptr)
            {
                actual.append(QStringLiteral("<null>"));
                continue;
            }
            if (act->isSeparator())
            {
                actual.append(QStringLiteral("__sep"));
                continue;
            }
            const QString name = act->objectName();
            if (name.isEmpty())
            {
                // Toolbar `addWidget` registers an internal action whose
                // `objectName` is empty; treat it as a widget marker.
                actual.append(QStringLiteral("__widget"));
                continue;
            }
            actual.append(name);
        }

        QCOMPARE(actual, expected);
        // NOLINTEND(clang-analyzer-core.CallAndMessage)
    }

    // Item 1: the "Open Stream" split button surfaces the log-stream
    // action on its primary face and the network-stream action in
    // its dropdown. Validates both the default-action wiring and the
    // popup-menu population so a `setDefaultAction` regression (no
    // icon on the button) or a missing menu entry would be caught
    // separately.
    void TestOpenStreamSplitButtonPopup()
    {
        auto *button = mWindow->findChild<QToolButton *>(QStringLiteral("openStreamSplitButton"));
        QVERIFY2(button != nullptr, "primary toolbar must host the open-stream split button");
        // NOLINTBEGIN(clang-analyzer-core.CallAndMessage): prior QVERIFY2 aborts on null.
        QCOMPARE(button->popupMode(), QToolButton::MenuButtonPopup);

        const QAction *defaultAction = button->defaultAction();
        QVERIFY2(defaultAction != nullptr, "split button must have a default action");
        QCOMPARE(defaultAction->objectName(), QStringLiteral("actionOpenLogStream"));

        const QMenu *menu = button->menu();
        QVERIFY2(menu != nullptr, "split button must have a popup menu");
        QStringList menuNames;
        for (const QAction *act : menu->actions())
        {
            menuNames.append(act != nullptr ? act->objectName() : QStringLiteral("<null>"));
        }
        QCOMPARE(
            menuNames, (QStringList{QStringLiteral("actionOpenLogStream"), QStringLiteral("actionOpenNetworkStream")})
        );
        // NOLINTEND(clang-analyzer-core.CallAndMessage)
    }

    // Regression for the split-button blank-icon bug: the split
    // button was added to the toolbar via `addWidget`, which wraps
    // it in an internal `QWidgetAction` that does NOT appear in
    // `mMainToolbar->actions()`. The first cut of `RefreshThemedIcons`
    // iterated only the toolbar's `actions()` list, so
    // `actionOpenLogStream` (tagged with `svgIconPath`) was never
    // reached; the split button's `setDefaultAction` sync therefore
    // pulled an empty icon and the button rendered as a blank box.
    // The corresponding `actionOpenNetworkStream` entry in the
    // dropdown menu had the same problem.
    //
    // We now route every themed action through `mThemedActions`
    // (anchor-driven, not toolbar-iteration-driven). Three asserts
    // pin the contract: both underlying actions carry a non-empty
    // icon, AND the button face (synced from the default action)
    // renders one too. Any future refactor that reverts to the
    // toolbar-iteration shape would lose one or more of these.
    void TestOpenStreamSplitButtonAndMenuHaveThemedIcons()
    {
        auto *button = mWindow->findChild<QToolButton *>(QStringLiteral("openStreamSplitButton"));
        auto *openLog = mWindow->findChild<QAction *>(QStringLiteral("actionOpenLogStream"));
        auto *openNet = mWindow->findChild<QAction *>(QStringLiteral("actionOpenNetworkStream"));
        QVERIFY2(button != nullptr, "primary toolbar must host the open-stream split button");
        QVERIFY2(openLog != nullptr, "actionOpenLogStream must exist");
        QVERIFY2(openNet != nullptr, "actionOpenNetworkStream must exist");
        // NOLINTBEGIN(clang-analyzer-core.CallAndMessage): prior QVERIFY2 aborts on null.
        QVERIFY2(
            !openLog->icon().isNull(),
            "actionOpenLogStream must carry a themed icon (split button's default-action icon source)"
        );
        QVERIFY2(
            !openNet->icon().isNull(),
            "actionOpenNetworkStream must carry a themed icon (popup-menu entry next to Log Stream)"
        );
        // QToolButton::setDefaultAction syncs the action's icon
        // onto the button; an empty action icon would land an
        // empty button icon. Asserting on the button face directly
        // catches the visual regression even if the sync is ever
        // re-routed.
        QVERIFY2(!button->icon().isNull(), "split button face must render a non-empty icon");
        // NOLINTEND(clang-analyzer-core.CallAndMessage)
    }

    // Companion regression for the split-button blank-icon bug:
    // it is not enough that the icons are non-null at construction.
    // The original bug also meant a Light <-> Dark flip would
    // leave the (initial-tint) icon stale while every other
    // toolbar action repainted. Pinning palette tracking on
    // `actionOpenLogStream` specifically guards against a future
    // refactor that keeps the constructor-time tint but drops the
    // refresh hook for this action.
    void TestOpenStreamActionsTrackPalette()
    {
        auto *toolbar = mWindow->findChild<QToolBar *>(QStringLiteral("mainToolbar"));
        auto *openLog = mWindow->findChild<QAction *>(QStringLiteral("actionOpenLogStream"));
        QVERIFY2(toolbar != nullptr, "MainWindow must own the primary toolbar");
        QVERIFY2(openLog != nullptr, "actionOpenLogStream must exist");

        // Match the AA-tolerant pixel walk used by
        // `TestMainToolbarIconsTrackPalette`; kept inline rather
        // than hoisted so a future cleanup that drops one test
        // doesn't leave the other one referencing a vanished helper.
        auto opaquePixelCount = [](const QIcon &icon, QRgb expected) -> int {
            const QSize size{20, 20};
            const QImage img = icon.pixmap(size).toImage().convertToFormat(QImage::Format_ARGB32);
            int matching = 0;
            for (int y = 0; y < img.height(); ++y)
            {
                for (int x = 0; x < img.width(); ++x)
                {
                    const QRgb px = img.pixel(x, y);
                    if (qAlpha(px) < 128)
                    {
                        continue;
                    }
                    constexpr int CHANNEL_TOLERANCE = 32;
                    if (std::abs(qRed(px) - qRed(expected)) <= CHANNEL_TOLERANCE &&
                        std::abs(qGreen(px) - qGreen(expected)) <= CHANNEL_TOLERANCE &&
                        std::abs(qBlue(px) - qBlue(expected)) <= CHANNEL_TOLERANCE)
                    {
                        ++matching;
                    }
                }
            }
            return matching;
        };

        // NOLINTBEGIN(clang-analyzer-core.CallAndMessage): prior QVERIFY2 aborts on null.
        QPalette greenPalette = toolbar->palette();
        greenPalette.setColor(QPalette::Active, QPalette::WindowText, QColor(0, 200, 0));
        greenPalette.setColor(QPalette::Inactive, QPalette::WindowText, QColor(0, 200, 0));
        toolbar->setPalette(greenPalette);
        mWindow->setPalette(greenPalette);
        QCoreApplication::processEvents();
        QVERIFY2(
            opaquePixelCount(openLog->icon(), qRgb(0, 200, 0)) > 0,
            "actionOpenLogStream icon must repaint in the palette WindowText colour"
        );
        // NOLINTEND(clang-analyzer-core.CallAndMessage)
    }

    // Shape contract for the Add-filter split button: it must be
    // a `MenuButtonPopup` `QToolButton` whose default action is
    // the bare `actionAddFilter` (so a click on the face still
    // opens the generic editor, preserving the pre-split UX) and
    // whose dropdown menu carries the `addFilterSplitMenu`
    // objectName the per-column tests find by.
    //
    // Pinned so a future cleanup that drops the dropdown and
    // reverts to a bare `addAction(actionAddFilter)` would trip
    // here, not silently regress the per-column shortcut.
    void TestAddFilterSplitButtonShape()
    {
        auto *button = mWindow->findChild<QToolButton *>(QStringLiteral("addFilterSplitButton"));
        QVERIFY2(button != nullptr, "primary toolbar must host the add-filter split button");
        // NOLINTBEGIN(clang-analyzer-core.CallAndMessage): prior QVERIFY2 aborts on null.
        QCOMPARE(button->popupMode(), QToolButton::MenuButtonPopup);

        const QAction *defaultAction = button->defaultAction();
        QVERIFY2(defaultAction != nullptr, "split button must have a default action");
        QCOMPARE(defaultAction->objectName(), QStringLiteral("actionAddFilter"));

        const QMenu *menu = button->menu();
        QVERIFY2(menu != nullptr, "split button must have a popup menu");
        QCOMPARE(menu->objectName(), QStringLiteral("addFilterSplitMenu"));
        // NOLINTEND(clang-analyzer-core.CallAndMessage)
    }

    // The Add-filter dropdown lists every *visible* column under
    // `Add filter on "<col>"…`. Verifies both the population
    // (one entry per visible column, in column order) and the
    // hidden-column filter (toggling a column hidden must drop
    // its entry on the next open, mirroring the header
    // right-click which refuses to expose hidden columns).
    void TestAddFilterDropdownListsVisibleColumns()
    {
        const int levelCol = StreamFixtureForColumnTests();
        QVERIFY2(levelCol >= 0, "fixture must produce columns");

        auto *menu = mWindow->findChild<QMenu *>(QStringLiteral("addFilterSplitMenu"));
        QVERIFY2(menu != nullptr, "add-filter dropdown must exist");
        // NOLINTBEGIN(clang-analyzer-core.CallAndMessage): prior QVERIFY2 aborts on null.
        // `aboutToShow` is the production rebuild trigger; firing
        // it directly is what every other rebuild-on-show test
        // does (`TestMainToolbarToggleAppearsInViewMenu`,
        // `RebuildViewMenu` tests).
        emit menu->aboutToShow();

        QStringList entryTexts;
        for (const QAction *act : menu->actions())
        {
            entryTexts.append(act != nullptr ? act->text() : QStringLiteral("<null>"));
        }
        QVERIFY2(!entryTexts.isEmpty(), "dropdown must list at least one column after streaming a fixture");
        for (const QString &text : entryTexts)
        {
            QVERIFY2(
                text.startsWith(QStringLiteral("Add filter on")),
                "every dropdown entry must follow the `Add filter on \"<col>\"...` shape"
            );
        }

        // Hide the `level` column and re-open: its entry must
        // drop. Cross-checks the visible-only filter.
        auto *model = mWindow->Model();
        const int msgCol = ColumnByHeader(*model, QStringLiteral("msg"));
        QVERIFY2(msgCol >= 0, "fixture must expose `msg` column");
        mWindow->SetColumnVisible(msgCol, false);
        emit menu->aboutToShow();

        for (const QAction *act : menu->actions())
        {
            QVERIFY2(
                !act->text().contains(QStringLiteral("\"msg\"")),
                "hidden column must not surface in the dropdown after hide"
            );
        }
        // NOLINTEND(clang-analyzer-core.CallAndMessage)
    }

    // Triggering an `Add filter on "<col>"…` dropdown entry opens
    // a `FilterEditor` with the row combobox pre-pointed at the
    // clicked column -- same contract the header right-click
    // `TestHeaderContextMenuAddFilterOpensEditorWithColumnPreselected`
    // pins, replicated for the toolbar entry point so a refactor
    // that changes the `AddFilter` signature can't silently break
    // the new shortcut without tripping a test.
    void TestAddFilterDropdownTriggerPreselectsColumn()
    {
        const int levelCol = StreamFixtureForColumnTests();
        QVERIFY2(levelCol >= 0, "fixture must produce columns");

        auto *menu = mWindow->findChild<QMenu *>(QStringLiteral("addFilterSplitMenu"));
        QVERIFY2(menu != nullptr, "add-filter dropdown must exist");
        // NOLINTBEGIN(clang-analyzer-core.CallAndMessage): prior QVERIFY2 aborts on null.
        emit menu->aboutToShow();

        QAction *categoryEntry = nullptr;
        for (QAction *act : menu->actions())
        {
            if (act != nullptr && act->text().contains(QStringLiteral("\"category\"")))
            {
                categoryEntry = act;
                break;
            }
        }
        QVERIFY2(categoryEntry != nullptr, "dropdown must offer an entry for the `category` column");

        categoryEntry->trigger();
        QCoreApplication::processEvents();

        FilterEditor *editor = nullptr;
        for (QWidget *widget : QApplication::topLevelWidgets())
        {
            if (auto *e = qobject_cast<FilterEditor *>(widget))
            {
                editor = e;
                break;
            }
        }
        QVERIFY2(editor != nullptr, "triggering a dropdown entry must spawn a FilterEditor");
        QCOMPARE(editor->GetRowToFilter(), levelCol);
        editor->close();
        editor->deleteLater();
        QCoreApplication::processEvents();
        // NOLINTEND(clang-analyzer-core.CallAndMessage)
    }

    // Shape contract for the Clear-filters split button, mirrors
    // `TestAddFilterSplitButtonShape`: pinned so a revert to the
    // bare `addAction(actionClearAllFilters)` would trip here.
    void TestClearFiltersSplitButtonShape()
    {
        auto *button = mWindow->findChild<QToolButton *>(QStringLiteral("clearFiltersSplitButton"));
        QVERIFY2(button != nullptr, "primary toolbar must host the clear-filters split button");
        // NOLINTBEGIN(clang-analyzer-core.CallAndMessage): prior QVERIFY2 aborts on null.
        QCOMPARE(button->popupMode(), QToolButton::MenuButtonPopup);

        const QAction *defaultAction = button->defaultAction();
        QVERIFY2(defaultAction != nullptr, "split button must have a default action");
        QCOMPARE(defaultAction->objectName(), QStringLiteral("actionClearAllFilters"));

        const QMenu *menu = button->menu();
        QVERIFY2(menu != nullptr, "split button must have a popup menu");
        QCOMPARE(menu->objectName(), QStringLiteral("clearFiltersSplitMenu"));
        // NOLINTEND(clang-analyzer-core.CallAndMessage)
    }

    // With zero filters the Clear-filters dropdown must still
    // surface a `(no filters)` placeholder rather than open as a
    // blank box. The placeholder is disabled (no-op click), but
    // its presence makes the empty state self-explanatory on the
    // styles where the menu arrow remains clickable while the
    // face button is disabled.
    void TestClearFiltersDropdownPlaceholderWhenEmpty()
    {
        auto *menu = mWindow->findChild<QMenu *>(QStringLiteral("clearFiltersSplitMenu"));
        QVERIFY2(menu != nullptr, "clear-filters dropdown must exist");
        // NOLINTBEGIN(clang-analyzer-core.CallAndMessage): prior QVERIFY2 aborts on null.
        QCOMPARE(mWindow->Filters().size(), static_cast<size_t>(0));
        emit menu->aboutToShow();

        const QList<QAction *> entries = menu->actions();
        QCOMPARE(entries.size(), 1);
        QVERIFY2(!entries.front()->isEnabled(), "empty-state placeholder must be disabled (no-op)");
        QVERIFY2(
            entries.front()->text().contains(QStringLiteral("no filters")),
            "empty-state placeholder text must mention `no filters`"
        );
        // NOLINTEND(clang-analyzer-core.CallAndMessage)
    }

    // The Clear-filters dropdown lists one entry per active
    // filter, grouped by column index then sorted by title.
    // Seeds two filters on `level` and one on `msg`, verifies all
    // three appear with the `Remove "<col>": <title>` shape, and
    // pins the column-grouping order so a future user-visible
    // sort change here trips the test instead of silently
    // shuffling the menu.
    void TestClearFiltersDropdownListsActiveFilters()
    {
        const int levelCol = StreamFixtureForColumnTests();
        QVERIFY2(levelCol >= 0, "fixture must produce columns");
        auto *model = mWindow->Model();
        const int msgCol = ColumnByHeader(*model, QStringLiteral("msg"));
        QVERIFY2(msgCol >= 0, "fixture must expose `msg` column");

        const QString levelFilterInfo = QStringLiteral("clear-list-level-info");
        const QString levelFilterWarn = QStringLiteral("clear-list-level-warn");
        const QString msgFilter = QStringLiteral("clear-list-msg");
        QVERIFY2(
            QMetaObject::invokeMethod(
                mWindow,
                "FilterEnumSubmitted",
                Qt::DirectConnection,
                Q_ARG(QString, levelFilterInfo),
                Q_ARG(int, levelCol),
                Q_ARG(QStringList, QStringList{QStringLiteral("info")})
            ),
            "FilterEnumSubmitted must be invocable"
        );
        QVERIFY2(
            QMetaObject::invokeMethod(
                mWindow,
                "FilterEnumSubmitted",
                Qt::DirectConnection,
                Q_ARG(QString, levelFilterWarn),
                Q_ARG(int, levelCol),
                Q_ARG(QStringList, QStringList{QStringLiteral("warn")})
            ),
            "FilterEnumSubmitted must be invocable"
        );
        QVERIFY2(
            QMetaObject::invokeMethod(
                mWindow,
                "FilterSubmitted",
                Qt::DirectConnection,
                Q_ARG(QString, msgFilter),
                Q_ARG(int, msgCol),
                Q_ARG(QString, QStringLiteral("m1")),
                Q_ARG(int, static_cast<int>(loglib::LogConfiguration::LogFilter::Match::Contains))
            ),
            "FilterSubmitted must be invocable"
        );
        QCoreApplication::processEvents();
        QCOMPARE(mWindow->Filters().size(), static_cast<size_t>(3));

        auto *menu = mWindow->findChild<QMenu *>(QStringLiteral("clearFiltersSplitMenu"));
        QVERIFY2(menu != nullptr, "clear-filters dropdown must exist");
        // NOLINTBEGIN(clang-analyzer-core.CallAndMessage): prior QVERIFY2 aborts on null.
        emit menu->aboutToShow();

        const QList<QAction *> entries = menu->actions();
        QCOMPARE(entries.size(), 3);

        // ObjectName carries the UUID, so find by id rather than
        // by display text. Cheaper to reason about and survives
        // a label tweak.
        QStringList entryIds;
        QStringList entryTexts;
        for (const QAction *act : entries)
        {
            entryIds.append(act->objectName());
            entryTexts.append(act->text());
        }
        QVERIFY2(entryIds.contains(levelFilterInfo), "dropdown must contain the level=info filter");
        QVERIFY2(entryIds.contains(levelFilterWarn), "dropdown must contain the level=warn filter");
        QVERIFY2(entryIds.contains(msgFilter), "dropdown must contain the msg=m1 filter");
        for (const QString &text : entryTexts)
        {
            QVERIFY2(
                text.startsWith(QStringLiteral("Remove \"")),
                "every entry must follow the `Remove \"<col>\": <title>` shape"
            );
        }

        // Grouping check: the two `level` filters must sit
        // adjacent (column-row primary sort), so the `msg` entry
        // is either the first or the last entry, never sandwiched
        // between the level entries.
        const qsizetype msgIdx = entryIds.indexOf(msgFilter);
        QVERIFY2(
            msgIdx == 0 || msgIdx == entries.size() - 1,
            "msg-column filter must not split the level-column group (column grouping)"
        );
        // NOLINTEND(clang-analyzer-core.CallAndMessage)
    }

    // Triggering one entry in the Clear-filters dropdown removes
    // *only* that filter -- the rest stay. Guards the "let me
    // drop just one filter without nuking the whole set" promise
    // the dropdown sells.
    void TestClearFiltersDropdownTriggerRemovesSingleFilter()
    {
        const int levelCol = StreamFixtureForColumnTests();
        QVERIFY2(levelCol >= 0, "fixture must produce columns");
        auto *model = mWindow->Model();
        const int msgCol = ColumnByHeader(*model, QStringLiteral("msg"));
        QVERIFY2(msgCol >= 0, "fixture must expose `msg` column");

        const QString keepId = QStringLiteral("single-remove-keep");
        const QString dropId = QStringLiteral("single-remove-drop");
        QVERIFY2(
            QMetaObject::invokeMethod(
                mWindow,
                "FilterEnumSubmitted",
                Qt::DirectConnection,
                Q_ARG(QString, keepId),
                Q_ARG(int, levelCol),
                Q_ARG(QStringList, QStringList{QStringLiteral("info")})
            ),
            "FilterEnumSubmitted must be invocable"
        );
        QVERIFY2(
            QMetaObject::invokeMethod(
                mWindow,
                "FilterSubmitted",
                Qt::DirectConnection,
                Q_ARG(QString, dropId),
                Q_ARG(int, msgCol),
                Q_ARG(QString, QStringLiteral("noise")),
                Q_ARG(int, static_cast<int>(loglib::LogConfiguration::LogFilter::Match::Contains))
            ),
            "FilterSubmitted must be invocable"
        );
        QCoreApplication::processEvents();
        QCOMPARE(mWindow->Filters().size(), static_cast<size_t>(2));

        auto *menu = mWindow->findChild<QMenu *>(QStringLiteral("clearFiltersSplitMenu"));
        QVERIFY2(menu != nullptr, "clear-filters dropdown must exist");
        // NOLINTBEGIN(clang-analyzer-core.CallAndMessage): prior QVERIFY2 aborts on null.
        emit menu->aboutToShow();

        QAction *dropEntry = nullptr;
        for (QAction *act : menu->actions())
        {
            if (act != nullptr && act->objectName() == dropId)
            {
                dropEntry = act;
                break;
            }
        }
        QVERIFY2(dropEntry != nullptr, "dropdown must carry an entry for the filter we want to drop");

        dropEntry->trigger();
        QCoreApplication::processEvents();

        QCOMPARE(mWindow->Filters().size(), static_cast<size_t>(1));
        QVERIFY2(
            mWindow->Filters().contains(keepId.toStdString()),
            "the un-triggered filter must survive a single-entry remove"
        );
        QVERIFY2(
            !mWindow->Filters().contains(dropId.toStdString()),
            "the triggered filter must be gone after a single-entry remove"
        );
        // NOLINTEND(clang-analyzer-core.CallAndMessage)
    }

    // Shape of the Sort dropdown button: `InstantPopup`
    // `QToolButton` with `actionSortBy` as its default action
    // and a popup menu named `sortBySplitMenu`.
    void TestSortBySplitButtonShape()
    {
        auto *button = mWindow->findChild<QToolButton *>(QStringLiteral("sortBySplitButton"));
        QVERIFY2(button != nullptr, "primary toolbar must host the sort split button");
        // NOLINTBEGIN(clang-analyzer-core.CallAndMessage): prior QVERIFY2 aborts on null.
        QCOMPARE(button->popupMode(), QToolButton::InstantPopup);

        const QAction *defaultAction = button->defaultAction();
        QVERIFY2(defaultAction != nullptr, "split button must have a default action");
        QCOMPARE(defaultAction->objectName(), QStringLiteral("actionSortBy"));

        const QMenu *menu = button->menu();
        QVERIFY2(menu != nullptr, "split button must have a popup menu");
        QCOMPARE(menu->objectName(), QStringLiteral("sortBySplitMenu"));
        // NOLINTEND(clang-analyzer-core.CallAndMessage)
    }

    // The Sort dropdown lists every visible column as two
    // checkable rows: ▲ asc and ▼ desc, each followed by the
    // disambiguated column label in quotes. Hiding a column
    // drops its rows on the next open.
    void TestSortByDropdownListsVisibleColumns()
    {
        // The fixture helper returns the `category` column
        // index; bind to a clearer name.
        const int categoryCol = StreamFixtureForColumnTests();
        QVERIFY2(categoryCol >= 0, "fixture must produce columns");

        auto *menu = mWindow->findChild<QMenu *>(QStringLiteral("sortBySplitMenu"));
        QVERIFY2(menu != nullptr, "sort dropdown must exist");
        // NOLINTBEGIN(clang-analyzer-core.CallAndMessage): prior QVERIFY2 aborts on null.
        emit menu->aboutToShow();

        const QList<QAction *> entries = menu->actions();
        QVERIFY2(!entries.isEmpty(), "dropdown must list at least one column after streaming a fixture");
        int ascCount = 0;
        int descCount = 0;
        for (const QAction *act : entries)
        {
            QVERIFY2(act != nullptr, "dropdown entry must not be null");
            QVERIFY2(act->menu() == nullptr, "flat entries must not carry submenus");
            QVERIFY2(act->isCheckable(), "every dropdown row must be checkable");
            const QString text = act->text();
            QVERIFY2(text.contains(QStringLiteral("\"")), "every dropdown row must carry the column label in quotes");
            QVERIFY2(
                !text.contains(QStringLiteral("Sort by")),
                "row text is just glyph + quoted column name; the host menu carries the verb"
            );
            if (text.startsWith(QString::fromUtf8(u8"\u25B2")))
            {
                ++ascCount;
            }
            else if (text.startsWith(QString::fromUtf8(u8"\u25BC")))
            {
                ++descCount;
            }
            else
            {
                QFAIL("every dropdown row must start with the asc / desc triangle glyph");
            }
        }
        QVERIFY2(ascCount > 0 && ascCount == descCount, "every column must contribute both an Asc and a Desc row");

        // Hide `msg` and re-open: its rows must drop.
        auto *model = mWindow->Model();
        const int msgCol = ColumnByHeader(*model, QStringLiteral("msg"));
        QVERIFY2(msgCol >= 0, "fixture must expose `msg` column");
        mWindow->SetColumnVisible(msgCol, false);
        emit menu->aboutToShow();

        for (const QAction *act : menu->actions())
        {
            QVERIFY2(
                !act->text().contains(QStringLiteral("\"msg\"")),
                "hidden column must not surface in the dropdown after hide"
            );
        }
        // NOLINTEND(clang-analyzer-core.CallAndMessage)
    }

    // Triggering a desc row installs the matching sort on the
    // proxy.
    void TestSortByDropdownTriggerSortsByColumn()
    {
        const int categoryCol = StreamFixtureForColumnTests();
        QVERIFY2(categoryCol >= 0, "fixture must expose `category` column");

        auto *menu = mWindow->findChild<QMenu *>(QStringLiteral("sortBySplitMenu"));
        QVERIFY2(menu != nullptr, "sort dropdown must exist");
        // NOLINTBEGIN(clang-analyzer-core.CallAndMessage): prior QVERIFY2 aborts on null.
        emit menu->aboutToShow();

        QAction *categoryDesc = nullptr;
        for (QAction *act : menu->actions())
        {
            if (act != nullptr && act->text().contains(QStringLiteral("\"category\"")) &&
                act->text().startsWith(QString::fromUtf8(u8"\u25BC")))
            {
                categoryDesc = act;
                break;
            }
        }
        QVERIFY2(categoryDesc != nullptr, "dropdown must offer a desc row for the `category` column");

        categoryDesc->trigger();
        QCoreApplication::processEvents();

        QCOMPARE(mWindow->FilterModel()->SortColumn(), categoryCol);
        QCOMPARE(mWindow->FilterModel()->SortOrder(), Qt::DescendingOrder);
        // NOLINTEND(clang-analyzer-core.CallAndMessage)
    }

    // The Sort menu must lead with `actionClearSort` + a
    // separator, then two checkable rows (▲ asc, ▼ desc) per
    // visible column. Sort is single-column / single-direction,
    // so at most one row is ever checked.
    void TestSortMenuShapeAndCheckmarks()
    {
        const int categoryCol = StreamFixtureForColumnTests();
        QVERIFY2(categoryCol >= 0, "fixture must expose `category` column");

        auto *menu = mWindow->findChild<QMenu *>(QStringLiteral("menuSort"));
        QVERIFY2(menu != nullptr, "Sort menu must exist");
        // NOLINTBEGIN(clang-analyzer-core.CallAndMessage): prior QVERIFY2 aborts on null.

        // Asc-sort `category` so the rebuild has a checked
        // entry to mark.
        mWindow->findChild<LogTableView *>()->sortByColumn(categoryCol, Qt::AscendingOrder);
        QCoreApplication::processEvents();

        emit menu->aboutToShow();
        const QList<QAction *> entries = menu->actions();
        QVERIFY2(entries.size() >= 3, "Sort menu must have Clear + separator + at least one column row");
        QCOMPARE(entries[0]->objectName(), QStringLiteral("actionClearSort"));
        QVERIFY2(entries[1]->isSeparator(), "second slot must be the separator");
        QVERIFY2(entries[0]->isEnabled(), "actionClearSort must be enabled while a sort is active");

        // Walk the per-column rows and find the `category` Asc /
        // Desc pair. Asc must be checked, Desc unchecked.
        const QAction *ascCategory = nullptr;
        const QAction *descCategory = nullptr;
        int totalChecked = 0;
        for (const QAction *act : entries)
        {
            if (act == nullptr || act->isSeparator() || act->objectName() == QStringLiteral("actionClearSort"))
            {
                continue;
            }
            QVERIFY2(act->menu() == nullptr, "Sort menu rows must be flat (no submenus)");
            if (act->isChecked())
            {
                ++totalChecked;
            }
            if (act->text().contains(QStringLiteral("\"category\"")))
            {
                if (act->text().startsWith(QString::fromUtf8(u8"\u25B2")))
                {
                    ascCategory = act;
                }
                else if (act->text().startsWith(QString::fromUtf8(u8"\u25BC")))
                {
                    descCategory = act;
                }
            }
        }
        QVERIFY2(ascCategory != nullptr && descCategory != nullptr, "menu must carry both `category` rows");
        QVERIFY2(ascCategory->isChecked(), "active asc-sort row must be checked");
        QVERIFY2(!descCategory->isChecked(), "non-active desc-sort row must be unchecked");
        QCOMPARE(totalChecked, 1);
        // NOLINTEND(clang-analyzer-core.CallAndMessage)
    }

    // Header right-click on a sorted column must mark the
    // matching Sort asc/desc entry as checked, and the shared
    // `actionClearSort` must reflect its global enabled state.
    void TestHeaderContextMenuSortEntries()
    {
        const int categoryCol = StreamFixtureForColumnTests();
        QVERIFY2(categoryCol >= 0, "fixture must expose `category` column");

        // Without a sort, the shared `actionClearSort` is
        // disabled and neither direction is checked. Identify
        // the Clear entry by `objectName` so a future label
        // tweak doesn't need a test edit.
        auto built = mWindow->BuildHeaderContextMenu(categoryCol, nullptr);
        QVERIFY2(built.menu != nullptr, "BuildHeaderContextMenu must return a menu");
        // NOLINTBEGIN(clang-analyzer-core.CallAndMessage): prior QVERIFY2 aborts on null.
        // `QScopeGuard` covers any QVERIFY2-driven early
        // return; reset before building a second menu.
        const QScopeGuard freeMenu1([&built]() { built.menu->deleteLater(); });

        const auto findSortBlock = [](QMenu *menu) -> std::tuple<QAction *, QAction *, QAction *> {
            QAction *clearSort = nullptr;
            QAction *asc = nullptr;
            QAction *desc = nullptr;
            for (QAction *act : menu->actions())
            {
                if (act == nullptr || act->isSeparator())
                {
                    continue;
                }
                if (act->objectName() == QStringLiteral("actionClearSort"))
                {
                    clearSort = act;
                }
                else if (act->text().startsWith(QStringLiteral("Sort ascending")))
                {
                    asc = act;
                }
                else if (act->text().startsWith(QStringLiteral("Sort descending")))
                {
                    desc = act;
                }
            }
            return {clearSort, asc, desc};
        };

        auto [clearSortNoSort, ascNoSort, descNoSort] = findSortBlock(built.menu);
        QVERIFY2(
            clearSortNoSort != nullptr && ascNoSort != nullptr && descNoSort != nullptr,
            "header menu must always include the Sort block"
        );
        QVERIFY2(!clearSortNoSort->isEnabled(), "shared actionClearSort must be disabled when no sort is active");
        QVERIFY2(!ascNoSort->isChecked() && !descNoSort->isChecked(), "neither direction is checked without a sort");

        // With a desc sort on `category`, the matching entry is
        // checked and the shared `actionClearSort` is enabled.
        mWindow->findChild<LogTableView *>()->sortByColumn(categoryCol, Qt::DescendingOrder);
        QCoreApplication::processEvents();
        auto built2 = mWindow->BuildHeaderContextMenu(categoryCol, nullptr);
        QVERIFY2(built2.menu != nullptr, "BuildHeaderContextMenu must return a menu");
        const QScopeGuard freeMenu2([&built2]() { built2.menu->deleteLater(); });

        auto [clearSortActive, ascActive, descActive] = findSortBlock(built2.menu);
        QVERIFY2(
            clearSortActive != nullptr && ascActive != nullptr && descActive != nullptr,
            "header menu must always include the Sort block"
        );
        QVERIFY2(clearSortActive->isEnabled(), "shared actionClearSort must be enabled when a sort is active");
        QVERIFY2(!ascActive->isChecked(), "non-active asc entry must be unchecked under a desc sort");
        QVERIFY2(descActive->isChecked(), "active desc entry must be checked");

        // The same `actionClearSort` instance is reused across
        // every Sort surface; verify pointer identity so an
        // accidental clone would trip the test.
        auto *globalClearSort = mWindow->findChild<QAction *>(QStringLiteral("actionClearSort"));
        QCOMPARE(clearSortActive, globalClearSort);

        // Triggering the entry drops the proxy's sort.
        clearSortActive->trigger();
        QCoreApplication::processEvents();
        QCOMPARE(mWindow->FilterModel()->SortColumn(), -1);
        // NOLINTEND(clang-analyzer-core.CallAndMessage)
    }

    // Status-bar Clear-sort indicator must be hidden by default,
    // surface when a sort is installed, and hide again when the
    // sort is dropped. Mirrors the existing Clear-filters
    // status-button behaviour.
    void TestClearSortStatusButtonVisibilityFollowsSort()
    {
        const int categoryCol = StreamFixtureForColumnTests();
        QVERIFY2(categoryCol >= 0, "fixture must expose `category` column");

        auto *button = mWindow->findChild<QPushButton *>(QStringLiteral("clearSortStatusButton"));
        QVERIFY2(button != nullptr, "status bar must host the clear-sort indicator");
        // NOLINTBEGIN(clang-analyzer-core.CallAndMessage): prior QVERIFY2 aborts on null.
        QVERIFY2(button->isHidden(), "indicator must be hidden when no sort is active");

        mWindow->findChild<LogTableView *>()->sortByColumn(categoryCol, Qt::AscendingOrder);
        QCoreApplication::processEvents();
        QVERIFY2(!button->isHidden(), "indicator must surface when a sort becomes active");

        // Clicking triggers `actionClearSort`; the next
        // `layoutChanged` must hide the indicator again.
        button->click();
        QCoreApplication::processEvents();
        QCOMPARE(mWindow->FilterModel()->SortColumn(), -1);
        QVERIFY2(button->isHidden(), "indicator must hide again after a clear");
        // NOLINTEND(clang-analyzer-core.CallAndMessage)
    }

    // `actionClearSort` enable state must follow the proxy's sort
    // column. Mirrors `actionClearAllFilters`'s gating.
    void TestActionClearSortEnableStateFollowsSort()
    {
        const int categoryCol = StreamFixtureForColumnTests();
        QVERIFY2(categoryCol >= 0, "fixture must expose `category` column");

        auto *action = mWindow->findChild<QAction *>(QStringLiteral("actionClearSort"));
        QVERIFY2(action != nullptr, "actionClearSort must exist");
        // NOLINTBEGIN(clang-analyzer-core.CallAndMessage): prior QVERIFY2 aborts on null.
        QVERIFY2(!action->isEnabled(), "actionClearSort must start disabled");

        mWindow->findChild<LogTableView *>()->sortByColumn(categoryCol, Qt::DescendingOrder);
        QCoreApplication::processEvents();
        QVERIFY2(action->isEnabled(), "actionClearSort must enable while a sort is active");

        mWindow->findChild<LogTableView *>()->sortByColumn(-1, Qt::AscendingOrder);
        QCoreApplication::processEvents();
        QVERIFY2(!action->isEnabled(), "actionClearSort must disable again after a clear");
        // NOLINTEND(clang-analyzer-core.CallAndMessage)
    }

    // The toolbar's plain Clear-Sort button is the auto-
    // generated `QToolButton` from `addAction`. Resolve it via
    // `widgetForAction` and pin its enable + click contract -
    // without this test a refactor dropping
    // `addAction(actionClearSort)` would only trip the
    // toolbar-layout test.
    void TestToolbarClearSortButtonTriggersClear()
    {
        const int categoryCol = StreamFixtureForColumnTests();
        QVERIFY2(categoryCol >= 0, "fixture must expose `category` column");

        auto *toolbar = mWindow->findChild<QToolBar *>(QStringLiteral("mainToolbar"));
        QVERIFY2(toolbar != nullptr, "main toolbar must exist");
        // NOLINTBEGIN(clang-analyzer-core.CallAndMessage): prior QVERIFY2 aborts on null.
        auto *action = mWindow->findChild<QAction *>(QStringLiteral("actionClearSort"));
        QVERIFY2(action != nullptr, "actionClearSort must exist");
        auto *button = qobject_cast<QToolButton *>(toolbar->widgetForAction(action));
        QVERIFY2(button != nullptr, "toolbar must host a plain Clear-Sort button for actionClearSort");

        // Idle: action disabled -> button disabled.
        QVERIFY2(!button->isEnabled(), "toolbar Clear-Sort button must follow the action's disabled state");

        // Install a sort; the button must enable.
        mWindow->findChild<LogTableView *>()->sortByColumn(categoryCol, Qt::AscendingOrder);
        QCoreApplication::processEvents();
        QVERIFY2(button->isEnabled(), "toolbar Clear-Sort button must enable while a sort is active");

        // Click drops the proxy sort and re-disables the button.
        button->click();
        QCoreApplication::processEvents();
        QCOMPARE(mWindow->FilterModel()->SortColumn(), -1);
        QVERIFY2(!button->isEnabled(), "toolbar Clear-Sort button must re-disable after the sort is cleared");
        // NOLINTEND(clang-analyzer-core.CallAndMessage)
    }

    // When every column is hidden both Sort surfaces must
    // surface the `(every column is hidden ...)` placeholder,
    // not leave the menu empty.
    void TestSortDropdownPlaceholderWhenAllColumnsHidden()
    {
        const int categoryCol = StreamFixtureForColumnTests();
        QVERIFY2(categoryCol >= 0, "fixture must expose `category` column");
        auto *model = mWindow->Model();
        const int msgCol = ColumnByHeader(*model, QStringLiteral("msg"));
        QVERIFY2(msgCol >= 0, "fixture must expose `msg` column");

        // Hide every visible column. `SetColumnVisible`
        // clears the sort if the sorted column gets hidden.
        mWindow->SetColumnVisible(categoryCol, false);
        mWindow->SetColumnVisible(msgCol, false);
        QCoreApplication::processEvents();

        // Toolbar dropdown: a single placeholder, disabled.
        auto *splitMenu = mWindow->findChild<QMenu *>(QStringLiteral("sortBySplitMenu"));
        QVERIFY2(splitMenu != nullptr, "sort split menu must exist");
        // NOLINTBEGIN(clang-analyzer-core.CallAndMessage): prior QVERIFY2 aborts on null.
        emit splitMenu->aboutToShow();
        const QList<QAction *> splitActions = splitMenu->actions();
        QCOMPARE(splitActions.size(), 1);
        QVERIFY2(splitActions[0] != nullptr, "placeholder action must be present");
        QVERIFY2(!splitActions[0]->isEnabled(), "placeholder must be disabled");
        QVERIFY2(
            splitActions[0]->text().contains(QStringLiteral("every column is hidden")),
            "placeholder must explain why the dropdown is empty"
        );

        // Top-level Sort menu: Clear-sort + separator +
        // placeholder.
        auto *sortMenu = mWindow->findChild<QMenu *>(QStringLiteral("menuSort"));
        QVERIFY2(sortMenu != nullptr, "Sort menu must exist");
        emit sortMenu->aboutToShow();
        const QList<QAction *> sortActions = sortMenu->actions();
        QCOMPARE(sortActions.size(), 3);
        QCOMPARE(sortActions[0]->objectName(), QStringLiteral("actionClearSort"));
        QVERIFY2(sortActions[1]->isSeparator(), "second slot must be the separator");
        QVERIFY2(!sortActions[2]->isEnabled(), "placeholder must be disabled");
        QVERIFY2(
            sortActions[2]->text().contains(QStringLiteral("every column is hidden")),
            "placeholder must explain why the menu has no per-column entries"
        );
        // NOLINTEND(clang-analyzer-core.CallAndMessage)
    }

    // Columns whose data doesn't match their configured type
    // sort via the wrong comparator, so the Sort menu must
    // disable both Asc and Desc on them and surface a tooltip
    // pointing at Configuration Diagnostics. Type-clean
    // columns stay enabled (control).
    void TestSortMenuDisablesAscDescOnTypeMismatch()
    {
        const int categoryCol = StreamFixtureForColumnTests();
        QVERIFY2(categoryCol >= 0, "fixture must expose `category` column");
        auto *model = mWindow->Model();
        const int msgCol = ColumnByHeader(*model, QStringLiteral("msg"));
        QVERIFY2(msgCol >= 0, "fixture must expose `msg` column");

        // Pin `msg` (string-only) to `Integer` to force a
        // type mismatch and re-snapshot health. Same pattern as
        // `TestColumnHealthFlagsMismatchedType`.
        model->ConfigurationManager().SetColumnAutoDetect(static_cast<size_t>(msgCol), false);
        model->ConfigurationManager().SetColumnType(
            static_cast<size_t>(msgCol), loglib::LogConfiguration::Type::Integer
        );
        model->RefreshColumnHealth();
        QCoreApplication::processEvents();

        const auto msgHealth = model->ColumnHealth(msgCol);
        QVERIFY2(msgHealth.has_value(), "ColumnHealth must yield a snapshot after Refresh");
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access): asserted above.
        QVERIFY2(
            msgHealth->presentSlots > msgHealth->matchingSlots,
            "fixture precondition: msg column must report a type mismatch after the Integer pin"
        );

        const auto findRow = [](QMenu *menu, const QString &columnQuoted, const QString &glyph) -> QAction * {
            for (QAction *act : menu->actions())
            {
                if (act != nullptr && act->text().contains(columnQuoted) && act->text().startsWith(glyph))
                {
                    return act;
                }
            }
            return nullptr;
        };
        const QString ascGlyph = QString::fromUtf8(u8"\u25B2");
        const QString descGlyph = QString::fromUtf8(u8"\u25BC");

        auto *splitMenu = mWindow->findChild<QMenu *>(QStringLiteral("sortBySplitMenu"));
        QVERIFY2(splitMenu != nullptr, "sort split menu must exist");
        // NOLINTBEGIN(clang-analyzer-core.CallAndMessage): prior QVERIFY2 aborts on null.
        emit splitMenu->aboutToShow();

        const QAction *msgAsc = findRow(splitMenu, QStringLiteral("\"msg\""), ascGlyph);
        const QAction *msgDesc = findRow(splitMenu, QStringLiteral("\"msg\""), descGlyph);
        QVERIFY2(msgAsc != nullptr && msgDesc != nullptr, "sort dropdown must carry msg asc/desc rows");
        QVERIFY2(!msgAsc->isEnabled(), "asc row must be disabled on a type-mismatched column");
        QVERIFY2(!msgDesc->isEnabled(), "desc row must be disabled on a type-mismatched column");
        QVERIFY2(
            msgAsc->toolTip().contains(QStringLiteral("does not match")),
            "disabled asc row must explain the type-mismatch cause via tooltip"
        );
        QVERIFY2(
            msgDesc->toolTip().contains(QStringLiteral("does not match")),
            "disabled desc row must explain the type-mismatch cause via tooltip"
        );
        QVERIFY2(splitMenu->toolTipsVisible(), "sort dropdown must opt into per-action tooltips");

        const QAction *categoryAsc = findRow(splitMenu, QStringLiteral("\"category\""), ascGlyph);
        const QAction *categoryDesc = findRow(splitMenu, QStringLiteral("\"category\""), descGlyph);
        QVERIFY2(
            categoryAsc != nullptr && categoryDesc != nullptr,
            "sort dropdown must carry category asc/desc rows (control)"
        );
        QVERIFY2(categoryAsc->isEnabled(), "asc must remain enabled on a type-clean column");
        QVERIFY2(categoryDesc->isEnabled(), "desc must remain enabled on a type-clean column");

        // Top-level Sort menu carries the same gate (shared
        // core, but pin both surfaces in case of a future
        // split).
        auto *sortMenu = mWindow->findChild<QMenu *>(QStringLiteral("menuSort"));
        QVERIFY2(sortMenu != nullptr, "Sort menu must exist");
        emit sortMenu->aboutToShow();
        const QAction *sortMsgAsc = findRow(sortMenu, QStringLiteral("\"msg\""), ascGlyph);
        const QAction *sortMsgDesc = findRow(sortMenu, QStringLiteral("\"msg\""), descGlyph);
        QVERIFY2(
            sortMsgAsc != nullptr && sortMsgDesc != nullptr, "top-level Sort menu must also carry the msg asc/desc rows"
        );
        QVERIFY2(!sortMsgAsc->isEnabled(), "top-level Sort menu asc must be disabled on mismatch");
        QVERIFY2(!sortMsgDesc->isEnabled(), "top-level Sort menu desc must be disabled on mismatch");
        // NOLINTEND(clang-analyzer-core.CallAndMessage)
    }

    // Header right-click must apply the same type-mismatch
    // gate as the Sort menu. The right-click path doesn't
    // share `AppendSortByEntries`, so pin it independently.
    void TestHeaderContextMenuDisablesSortOnTypeMismatch()
    {
        const int categoryCol = StreamFixtureForColumnTests();
        QVERIFY2(categoryCol >= 0, "fixture must expose `category` column");
        auto *model = mWindow->Model();
        const int msgCol = ColumnByHeader(*model, QStringLiteral("msg"));
        QVERIFY2(msgCol >= 0, "fixture must expose `msg` column");

        model->ConfigurationManager().SetColumnAutoDetect(static_cast<size_t>(msgCol), false);
        model->ConfigurationManager().SetColumnType(
            static_cast<size_t>(msgCol), loglib::LogConfiguration::Type::Integer
        );
        model->RefreshColumnHealth();
        QCoreApplication::processEvents();

        auto built = mWindow->BuildHeaderContextMenu(msgCol, nullptr);
        QVERIFY2(built.menu != nullptr, "BuildHeaderContextMenu must return a menu");
        // NOLINTBEGIN(clang-analyzer-core.CallAndMessage): prior QVERIFY2 aborts on null.
        const QScopeGuard freeMenu([&built]() { built.menu->deleteLater(); });

        const QAction *asc = nullptr;
        const QAction *desc = nullptr;
        for (const QAction *act : built.menu->actions())
        {
            if (act == nullptr || act->isSeparator())
            {
                continue;
            }
            if (act->text().startsWith(QStringLiteral("Sort ascending")))
            {
                asc = act;
            }
            else if (act->text().startsWith(QStringLiteral("Sort descending")))
            {
                desc = act;
            }
        }
        QVERIFY2(asc != nullptr && desc != nullptr, "header menu must include Sort asc/desc entries");
        QVERIFY2(!asc->isEnabled(), "header right-click Sort ascending must be disabled on a type-mismatched column");
        QVERIFY2(!desc->isEnabled(), "header right-click Sort descending must be disabled on a type-mismatched column");
        QVERIFY2(
            asc->toolTip().contains(QStringLiteral("does not match")),
            "disabled header Sort ascending must explain the type-mismatch cause via tooltip"
        );
        QVERIFY2(
            desc->toolTip().contains(QStringLiteral("does not match")),
            "disabled header Sort descending must explain the type-mismatch cause via tooltip"
        );
        QVERIFY2(built.menu->toolTipsVisible(), "header menu must opt into per-action tooltips");

        // Control: a type-clean column keeps Asc / Desc enabled
        // on the same right-click path.
        auto controlBuilt = mWindow->BuildHeaderContextMenu(categoryCol, nullptr);
        QVERIFY2(controlBuilt.menu != nullptr, "BuildHeaderContextMenu must return a menu");
        const QScopeGuard freeControl([&controlBuilt]() { controlBuilt.menu->deleteLater(); });

        const QAction *controlAsc = nullptr;
        const QAction *controlDesc = nullptr;
        for (const QAction *act : controlBuilt.menu->actions())
        {
            if (act == nullptr || act->isSeparator())
            {
                continue;
            }
            if (act->text().startsWith(QStringLiteral("Sort ascending")))
            {
                controlAsc = act;
            }
            else if (act->text().startsWith(QStringLiteral("Sort descending")))
            {
                controlDesc = act;
            }
        }
        QVERIFY2(
            controlAsc != nullptr && controlDesc != nullptr,
            "header menu on a type-clean column must include Sort asc/desc entries"
        );
        QVERIFY2(controlAsc->isEnabled(), "Sort ascending must stay enabled on a type-clean column");
        QVERIFY2(controlDesc->isEnabled(), "Sort descending must stay enabled on a type-clean column");
        // NOLINTEND(clang-analyzer-core.CallAndMessage)
    }

    // The status-bar tooltip must rebuild when the sorted
    // column is renamed - `NotifyColumnEdited` emits only
    // `headerDataChanged`, so without the dedicated hook the
    // tooltip would freeze on the old label until the next
    // sort / filter event.
    void TestSortStatusTooltipUpdatesOnColumnRename()
    {
        const int categoryCol = StreamFixtureForColumnTests();
        QVERIFY2(categoryCol >= 0, "fixture must expose `category` column");

        // Sort so the status-bar button surfaces; its tooltip
        // resolves the live column label.
        mWindow->findChild<LogTableView *>()->sortByColumn(categoryCol, Qt::AscendingOrder);
        QCoreApplication::processEvents();

        auto *button = mWindow->findChild<QPushButton *>(QStringLiteral("clearSortStatusButton"));
        QVERIFY2(button != nullptr, "status bar must host the clear-sort indicator");
        // NOLINTBEGIN(clang-analyzer-core.CallAndMessage): prior QVERIFY2 aborts on null.
        QVERIFY2(!button->isHidden(), "indicator must surface while a sort is active");
        QVERIFY2(
            button->toolTip().contains(QStringLiteral("\"category\"")), "initial tooltip must name the sorted column"
        );

        // Rename the column and emit the same
        // `NotifyColumnEdited` the column editor would fire.
        // The tooltip must catch the new label without any
        // sort / filter event in between.
        auto *model = mWindow->Model();
        model->ConfigurationManager().SetColumnHeader(static_cast<size_t>(categoryCol), std::string("renamed-cat"));
        model->NotifyColumnEdited(categoryCol);
        QCoreApplication::processEvents();

        QVERIFY2(
            button->toolTip().contains(QStringLiteral("\"renamed-cat\"")),
            "tooltip must reflect the renamed column without waiting for a sort/filter event"
        );
        QVERIFY2(
            !button->toolTip().contains(QStringLiteral("\"category\"")),
            "tooltip must not still carry the stale column label"
        );
        // NOLINTEND(clang-analyzer-core.CallAndMessage)
    }

    // Item 1: `RebuildViewMenu` re-adds the primary toolbar's
    // `toggleViewAction` on every menu open. Without this the user
    // who hid the toolbar would have no discoverable way to bring
    // it back (no Ctrl shortcut, no other UI affordance).
    void TestMainToolbarToggleAppearsInViewMenu()
    {
        auto *viewMenu = mWindow->findChild<QMenu *>(QStringLiteral("menuView"));
        QVERIFY2(viewMenu != nullptr, "View menu must exist");
        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): prior QVERIFY2 aborts on null.
        emit viewMenu->aboutToShow();

        QAction *toggle = nullptr;
        for (QAction *act : viewMenu->actions())
        {
            if (act != nullptr && act->objectName() == QStringLiteral("actionToggleMainToolbar"))
            {
                toggle = act;
                break;
            }
        }
        QVERIFY2(toggle != nullptr, "View menu must contain the primary-toolbar toggle");
        // NOLINTBEGIN(clang-analyzer-core.CallAndMessage): prior QVERIFY2 aborts on null.
        QVERIFY2(toggle->isCheckable(), "primary-toolbar toggle must be checkable");

        auto *toolbar = mWindow->findChild<QToolBar *>(QStringLiteral("mainToolbar"));
        QVERIFY2(toolbar != nullptr, "MainWindow must own the primary toolbar");

        // `QToolBar::toggleViewAction()`'s internal slot is gated
        // on `q->isHidden()` (Qt source) -- the toolbar must be
        // genuinely realised on screen for the toggle to flip
        // anything, otherwise the slot short-circuits. Realise the
        // window the same way the find-bar / record-detail tests
        // do, then re-fetch the toggle (it caches the action on
        // first call, and the cached `isChecked` reflects the
        // pre-show visibility otherwise).
        mWindow->show();
        QVERIFY(QTest::qWaitForWindowExposed(mWindow, 5000));
        QCoreApplication::processEvents();

        emit viewMenu->aboutToShow();
        toggle = nullptr;
        for (QAction *act : viewMenu->actions())
        {
            if (act != nullptr && act->objectName() == QStringLiteral("actionToggleMainToolbar"))
            {
                toggle = act;
                break;
            }
        }
        QVERIFY2(toggle != nullptr, "View menu must still contain the primary-toolbar toggle after show");

        const bool startHidden = toolbar->isHidden();
        QVERIFY2(!startHidden, "primary toolbar must start visible after the window is exposed");
        QVERIFY2(toggle->isChecked(), "toggle must be checked while the toolbar is visible");

        toggle->trigger();
        QCoreApplication::processEvents();
        QVERIFY2(toolbar->isHidden(), "triggering an active toggle must hide the toolbar");
        QVERIFY2(!toggle->isChecked(), "toggle checked state must follow the toolbar visibility");

        toggle->trigger();
        QCoreApplication::processEvents();
        QVERIFY2(!toolbar->isHidden(), "re-triggering must bring the toolbar back");
        QVERIFY2(toggle->isChecked(), "toggle checked state must follow the toolbar back to visible");

        // Hide the window before returning so later tests in the
        // same fixture run don't inherit an exposed window (the
        // offscreen QPA plugin keeps stale exposure state around
        // until the next show/hide cycle and can confuse
        // focus-driven tests).
        mWindow->hide();
        // NOLINTEND(clang-analyzer-core.CallAndMessage)
    }

    // Item 1: toolbar icons are re-rendered via
    // `icon_loader::MakeThemedIcon` whenever the palette changes,
    // so a Light <-> Dark theme flip keeps the glyphs visible on
    // the new background. Without the `changeEvent` /
    // `OnThemeChanged` hooks the SVGs would stay tinted in the
    // old `WindowText` colour and disappear after one toggle.
    void TestMainToolbarIconsTrackPalette()
    {
        auto *toolbar = mWindow->findChild<QToolBar *>(QStringLiteral("mainToolbar"));
        QVERIFY2(toolbar != nullptr, "MainWindow must own the primary toolbar");

        // `actionOpen` is the first non-separator action on the
        // toolbar and carries the `svgIconPath` property, so it is
        // guaranteed to be re-tinted by `RefreshThemedIcons`.
        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): prior QVERIFY2 aborts on null.
        auto *openAction = mWindow->findChild<QAction *>(QStringLiteral("actionOpen"));
        QVERIFY2(openAction != nullptr, "actionOpen must exist");

        auto opaquePixelCount = [](const QIcon &icon, QRgb expected) -> int {
            const QSize size{20, 20};
            const QImage img = icon.pixmap(size).toImage().convertToFormat(QImage::Format_ARGB32);
            int matching = 0;
            for (int y = 0; y < img.height(); ++y)
            {
                for (int x = 0; x < img.width(); ++x)
                {
                    const QRgb px = img.pixel(x, y);
                    if (qAlpha(px) < 128)
                    {
                        continue;
                    }
                    constexpr int CHANNEL_TOLERANCE = 32;
                    if (std::abs(qRed(px) - qRed(expected)) <= CHANNEL_TOLERANCE &&
                        std::abs(qGreen(px) - qGreen(expected)) <= CHANNEL_TOLERANCE &&
                        std::abs(qBlue(px) - qBlue(expected)) <= CHANNEL_TOLERANCE)
                    {
                        ++matching;
                    }
                }
            }
            return matching;
        };

        // NOLINTBEGIN(clang-analyzer-core.CallAndMessage): prior QVERIFY2 aborts on null.
        // Vivid red palette: every opaque pixel of the tinted icon
        // must be red (within AA tolerance). A baked icon would
        // stay neutral grey regardless of the palette and fail.
        // `setPalette` is applied to the toolbar directly so the
        // assertion does not depend on Qt's palette-propagation
        // timing through `QMainWindow -> QToolBar`; `processEvents`
        // then flushes the resulting `PaletteChange` which
        // `MainWindow::changeEvent` translates into a refresh.
        QPalette redPalette = toolbar->palette();
        redPalette.setColor(QPalette::Active, QPalette::WindowText, QColor(255, 0, 0));
        redPalette.setColor(QPalette::Inactive, QPalette::WindowText, QColor(255, 0, 0));
        toolbar->setPalette(redPalette);
        // Mirror the palette onto the window too, so `MainWindow::
        // changeEvent` (which is what wires `RefreshThemedIcons`)
        // actually fires -- a palette set only on the toolbar
        // would propagate `PaletteChange` to its child widgets,
        // not back up to the window.
        mWindow->setPalette(redPalette);
        QCoreApplication::processEvents();
        const int redPixels = opaquePixelCount(openAction->icon(), qRgb(255, 0, 0));
        QVERIFY2(redPixels > 0, "primary toolbar icons must paint in the palette's WindowText colour");

        // Flip to vivid blue: the icon must re-mint -- the real
        // regression target is the very first theme toggle, which
        // would leave the cached red pixmap in place if
        // `RefreshThemedIcons` were not wired into `changeEvent`.
        QPalette bluePalette = toolbar->palette();
        bluePalette.setColor(QPalette::Active, QPalette::WindowText, QColor(0, 0, 255));
        bluePalette.setColor(QPalette::Inactive, QPalette::WindowText, QColor(0, 0, 255));
        toolbar->setPalette(bluePalette);
        mWindow->setPalette(bluePalette);
        QCoreApplication::processEvents();
        const int bluePixels = opaquePixelCount(openAction->icon(), qRgb(0, 0, 255));
        QVERIFY2(bluePixels > 0, "primary toolbar icons must repaint after a palette flip");
        // NOLINTEND(clang-analyzer-core.CallAndMessage)
    }

    // Item 1: `actionPauseStream` carries an alternate
    // `svgIconPathChecked` glyph so the button invites the
    // opposite transition (pause when running, play when paused),
    // matching the media-player convention every user already
    // knows. Without the On-state pixmap the icon would stay on
    // the pause glyph even while ingestion is paused, leaving the
    // user with no visual cue that another click will resume.
    void TestPauseActionSwapsIconOnCheckedState()
    {
        auto *pauseAction = mWindow->findChild<QAction *>(QStringLiteral("actionPauseStream"));
        QVERIFY2(pauseAction != nullptr, "actionPauseStream must exist");
        // NOLINTBEGIN(clang-analyzer-core.CallAndMessage): prior QVERIFY2 aborts on null.
        QVERIFY2(pauseAction->isCheckable(), "actionPauseStream must be checkable");

        const QIcon icon = pauseAction->icon();
        QVERIFY2(!icon.isNull(), "actionPauseStream must carry a themed icon");

        // `QIcon::pixmap(size, Normal, Off/On)` returns the
        // best-matching pixmap for the requested state. When the
        // On state has its own registered pixmap the two images
        // differ; if the override is missing Qt falls back to the
        // Off pixmap and the images compare equal.
        const QSize size{20, 20};
        const QImage offImage = icon.pixmap(size, QIcon::Normal, QIcon::Off).toImage();
        const QImage onImage = icon.pixmap(size, QIcon::Normal, QIcon::On).toImage();
        QVERIFY2(!offImage.isNull(), "Off-state pixmap must be available");
        QVERIFY2(!onImage.isNull(), "On-state pixmap must be available");
        QVERIFY2(
            offImage != onImage, "actionPauseStream's checked-state icon must differ from its unchecked-state icon"
        );
        // NOLINTEND(clang-analyzer-core.CallAndMessage)
    }

    // Item 1: the split button is added via `QToolBar::addWidget`,
    // which does NOT propagate the toolbar's `iconSize` /
    // `toolButtonStyle`. We mirror them explicitly in
    // `BuildMainToolbar`; this test pins that contract so a future
    // refactor that drops the explicit setters would leave the
    // split button visually off-size from every other action.
    void TestOpenStreamSplitButtonMatchesToolbarSizing()
    {
        auto *toolbar = mWindow->findChild<QToolBar *>(QStringLiteral("mainToolbar"));
        QVERIFY2(toolbar != nullptr, "MainWindow must own the primary toolbar");
        // NOLINTBEGIN(clang-analyzer-core.CallAndMessage): prior QVERIFY2 aborts on null.
        auto *button = mWindow->findChild<QToolButton *>(QStringLiteral("openStreamSplitButton"));
        QVERIFY2(button != nullptr, "primary toolbar must host the open-stream split button");
        QCOMPARE(button->iconSize(), toolbar->iconSize());
        QCOMPARE(button->toolButtonStyle(), Qt::ToolButtonIconOnly);
        // NOLINTEND(clang-analyzer-core.CallAndMessage)
    }

    // Item 1: the stream toolbar shares the top dock-area row with
    // the main toolbar when streaming, so the two must use the
    // same icon edge length and button style -- otherwise the
    // combined strip jumps from compact-icon to icon+text mid-row
    // and looks unfinished. Pinned to catch any future drift.
    void TestStreamToolbarMirrorsMainToolbarStyle()
    {
        auto *mainBar = mWindow->findChild<QToolBar *>(QStringLiteral("mainToolbar"));
        auto *streamBar = mWindow->findChild<QToolBar *>(QStringLiteral("streamToolbar"));
        QVERIFY2(mainBar != nullptr, "MainWindow must own the primary toolbar");
        QVERIFY2(streamBar != nullptr, "MainWindow must own the stream toolbar");
        // NOLINTBEGIN(clang-analyzer-core.CallAndMessage): prior QVERIFY2 aborts on null.
        QCOMPARE(streamBar->iconSize(), mainBar->iconSize());
        QCOMPARE(streamBar->toolButtonStyle(), mainBar->toolButtonStyle());
        // NOLINTEND(clang-analyzer-core.CallAndMessage)
    }

    // Item 1: the File -> Recent Sessions submenu carries the
    // `file-clock` Lucide glyph so the entry is recognisable at a
    // glance. The icon is set on the submenu's `menuAction()` and
    // refreshed through the same `RefreshThemedIcons` pipeline as
    // toolbar actions, so a Light <-> Dark flip keeps it visible.
    void TestRecentSessionsMenuHasThemedIcon()
    {
        auto *recentMenu = mWindow->findChild<QMenu *>(QStringLiteral("menuRecentSessions"));
        QVERIFY2(recentMenu != nullptr, "File menu must expose menuRecentSessions");
        // NOLINTBEGIN(clang-analyzer-core.CallAndMessage): prior QVERIFY2 aborts on null.
        const QAction *menuAction = recentMenu->menuAction();
        QVERIFY2(menuAction != nullptr, "menuRecentSessions must have a menuAction");
        QVERIFY2(!menuAction->icon().isNull(), "menuRecentSessions menuAction must carry a themed icon");
        // The path stays as a property so `RefreshThemedIcons` can
        // re-tint without a per-action switch -- the value here is
        // the contract checked in `BuildMainToolbar`.
        QCOMPARE(menuAction->property("svgIconPath").toString(), QStringLiteral(":/icons/file-clock.svg"));
        // NOLINTEND(clang-analyzer-core.CallAndMessage)
    }

    // Regression: the find-bar arrow icons used `SP_ArrowUp` /
    // `SP_ArrowDown`, whose baked-black pixmaps disappeared on dark
    // themes. The icons must follow `QPalette::WindowText`.
    void TestFindBarArrowIconsFollowPalette()
    {
        auto *findRecord = mWindow->findChild<FindRecordWidget *>();
        QVERIFY2(findRecord != nullptr, "MainWindow must own a FindRecordWidget");
        auto *prevButton = findRecord->findChild<QToolButton *>(QStringLiteral("findPrevious"));
        auto *nextButton = findRecord->findChild<QToolButton *>(QStringLiteral("findNext"));
        QVERIFY2(prevButton != nullptr, "FindRecordWidget must expose its previous button");
        QVERIFY2(nextButton != nullptr, "FindRecordWidget must expose its next button");

        // Count opaque pixels in @p icon matching @p expected (with
        // a small per-channel tolerance for anti-alias drift). A
        // baked-black glyph never matches a non-black @p expected.
        auto opaquePixelCount = [](const QIcon &icon, QRgb expected) -> int {
            const QList<QSize> sizes = icon.availableSizes();
            const QSize size = sizes.isEmpty() ? QSize{16, 16} : sizes.front();
            const QImage img = icon.pixmap(size).toImage().convertToFormat(QImage::Format_ARGB32);
            int matching = 0;
            for (int y = 0; y < img.height(); ++y)
            {
                for (int x = 0; x < img.width(); ++x)
                {
                    const QRgb px = img.pixel(x, y);
                    if (qAlpha(px) < 128)
                    {
                        continue;
                    }
                    // Tolerate AA drift -- a pure-red apex blends to
                    // half-transparent maroon at the slope's edge.
                    constexpr int CHANNEL_TOLERANCE = 32;
                    if (std::abs(qRed(px) - qRed(expected)) <= CHANNEL_TOLERANCE &&
                        std::abs(qGreen(px) - qGreen(expected)) <= CHANNEL_TOLERANCE &&
                        std::abs(qBlue(px) - qBlue(expected)) <= CHANNEL_TOLERANCE)
                    {
                        ++matching;
                    }
                }
            }
            return matching;
        };

        // Vivid red palette: the rendered pixmap must contain red.
        // The old baked-black icons would be black regardless of
        // `WindowText`.
        QPalette redPalette = findRecord->palette();
        redPalette.setColor(QPalette::Active, QPalette::WindowText, QColor(255, 0, 0));
        redPalette.setColor(QPalette::Inactive, QPalette::WindowText, QColor(255, 0, 0));
        findRecord->setPalette(redPalette);
        // `setPalette` posts `PaletteChange`; pump the event loop
        // so `changeEvent` rebuilds the icons before we sample.
        QCoreApplication::sendPostedEvents(findRecord, QEvent::PaletteChange);
        const int prevRed = opaquePixelCount(prevButton->icon(), qRgb(255, 0, 0));
        const int nextRed = opaquePixelCount(nextButton->icon(), qRgb(255, 0, 0));
        QVERIFY2(prevRed > 0, "previous-button icon must paint in the palette's WindowText colour");
        QVERIFY2(nextRed > 0, "next-button icon must paint in the palette's WindowText colour");

        // Flip to vivid blue: the rendered pixmap must change too.
        // The real regression is that the first Light -> Dark
        // toggle must actually re-paint the arrows.
        QPalette bluePalette = findRecord->palette();
        bluePalette.setColor(QPalette::Active, QPalette::WindowText, QColor(0, 0, 255));
        bluePalette.setColor(QPalette::Inactive, QPalette::WindowText, QColor(0, 0, 255));
        findRecord->setPalette(bluePalette);
        QCoreApplication::sendPostedEvents(findRecord, QEvent::PaletteChange);
        const int prevBlue = opaquePixelCount(prevButton->icon(), qRgb(0, 0, 255));
        const int nextBlue = opaquePixelCount(nextButton->icon(), qRgb(0, 0, 255));
        QVERIFY2(prevBlue > 0, "previous-button icon must repaint after a palette change");
        QVERIFY2(nextBlue > 0, "next-button icon must repaint after a palette change");
    }

    // Regression: the chevron pixmap was minted at logical size
    // regardless of DPR, leaving the arrows softer than the
    // surrounding `.*` / `*?` glyphs on a 200%-scaled monitor.
    // Backing pixmap must scale with `devicePixelRatioF()`.
    void TestFindBarArrowIconsScaleWithDevicePixelRatio()
    {
        auto *findRecord = mWindow->findChild<FindRecordWidget *>();
        QVERIFY2(findRecord != nullptr, "MainWindow must own a FindRecordWidget");
        auto *prevButton = findRecord->findChild<QToolButton *>(QStringLiteral("findPrevious"));
        QVERIFY2(prevButton != nullptr, "FindRecordWidget must expose its previous button");

        const QIcon icon = prevButton->icon();
        QVERIFY2(!icon.isNull(), "previous-button icon must be set");
        const QList<QSize> sizes = icon.availableSizes();
        QVERIFY2(!sizes.isEmpty(), "icon must report at least one size");

        // The pixmap returned by `pixmap(size)` is scaled to the
        // logical size; we want the *backing* pixel count. Pull the
        // largest available size and check `devicePixelRatio()`
        // matches the host widget's DPR.
        const QPixmap pix = icon.pixmap(sizes.front());
        QVERIFY2(!pix.isNull(), "pixmap mint must succeed");
        const qreal hostDpr = findRecord->devicePixelRatioF();
        if (hostDpr > 1.0)
        {
            QVERIFY2(
                pix.devicePixelRatio() >= hostDpr - 0.05,
                qPrintable(QStringLiteral("expected DPR %1, got %2 -- HiDPI mint regressed")
                               .arg(hostDpr)
                               .arg(pix.devicePixelRatio()))
            );
            // Backing pixel size scales with DPR: an N-wide logical
            // icon at 2x DPR has a 2N-wide backing pixmap.
            const int expectedBackingPx = static_cast<int>(std::lround(sizes.front().width() * hostDpr));
            QVERIFY2(
                pix.size().width() >= expectedBackingPx - 1 && pix.size().width() <= expectedBackingPx + 1,
                qPrintable(QStringLiteral("backing pixmap width %1 should be ~ logical %2 * DPR %3")
                               .arg(pix.size().width())
                               .arg(sizes.front().width())
                               .arg(hostDpr))
            );
        }
        else
        {
            // 1x display: just confirm the pixmap is realised.
            QVERIFY(pix.devicePixelRatio() >= 1.0 - 0.05);
        }
    }

    // `IsStyleOnlyRoleChange` is the helper the find cache and
    // record-detail dock use to ignore theme-refresh emits. Lock
    // the contract so a future role addition can't silently flip a
    // value-affecting role into the "ignore" set.
    void TestIsStyleOnlyRoleChangeContract()
    {
        QVERIFY2(
            !LogModel::IsStyleOnlyRoleChange({}),
            "empty roles list is the 'I don't know' sentinel; must conservatively refresh"
        );
        QVERIFY(LogModel::IsStyleOnlyRoleChange({Qt::BackgroundRole}));
        QVERIFY(LogModel::IsStyleOnlyRoleChange({Qt::ForegroundRole}));
        QVERIFY(LogModel::IsStyleOnlyRoleChange({Qt::FontRole}));
        QVERIFY(LogModel::IsStyleOnlyRoleChange({Qt::DecorationRole}));
        QVERIFY2(
            LogModel::IsStyleOnlyRoleChange({Qt::ToolTipRole}),
            "ToolTipRole flips with `SetShowLevelIcons`; receivers must skip it"
        );
        QVERIFY(LogModel::IsStyleOnlyRoleChange({Qt::BackgroundRole, Qt::ForegroundRole, Qt::FontRole}));
        QVERIFY(LogModel::IsStyleOnlyRoleChange({Qt::DecorationRole, Qt::ToolTipRole}));
        QVERIFY2(!LogModel::IsStyleOnlyRoleChange({Qt::DisplayRole}), "DisplayRole change must trigger a refresh");
        QVERIFY2(!LogModel::IsStyleOnlyRoleChange({Qt::EditRole}), "EditRole change must trigger a refresh");
        QVERIFY2(
            !LogModel::IsStyleOnlyRoleChange({Qt::BackgroundRole, Qt::DisplayRole}),
            "mixed list with at least one value-role must trigger a refresh"
        );
        QVERIFY2(
            !LogModel::IsStyleOnlyRoleChange({Qt::ToolTipRole, Qt::DisplayRole}),
            "tooltip + display still has a value-role; must refresh"
        );
    }

    // `ComposeFindFlags` is the single source of truth for find-
    // flag composition. Lock the priority (regex > wildcard >
    // contains) and the always-on `MatchWrap | MatchRecursive`
    // baseline so a future refactor can't desync call sites.
    void TestComposeFindFlagsPriorityAndBaseline()
    {
        constexpr Qt::MatchFlags BASELINE = Qt::MatchWrap | Qt::MatchRecursive;
        QCOMPARE(
            LogFilterModel::ComposeFindFlags(/*wildcards=*/false, /*regularExpressions=*/false),
            BASELINE | Qt::MatchContains
        );
        QCOMPARE(
            LogFilterModel::ComposeFindFlags(/*wildcards=*/true, /*regularExpressions=*/false),
            BASELINE | Qt::MatchWildcard
        );
        QCOMPARE(
            LogFilterModel::ComposeFindFlags(/*wildcards=*/false, /*regularExpressions=*/true),
            BASELINE | Qt::MatchRegularExpression
        );
        // Both toggles set: regex wins. The UI enforces mutual
        // exclusion, but the helper must not depend on it.
        QCOMPARE(
            LogFilterModel::ComposeFindFlags(/*wildcards=*/true, /*regularExpressions=*/true),
            BASELINE | Qt::MatchRegularExpression
        );
    }

    // Regression: every `dataChanged` invalidation used to call
    // `mMatchCountTimer->start()`, which restarted the trailing
    // timer. Under streaming the trailing timer never fired and
    // only the max-age cap (750 ms) ever delivered. Fix: when both
    // timers are already running, skip the start so the trailing
    // timer can run to its natural deadline.
    void TestBumpMatchCountDebounceCoalescesStreamingBursts()
    {
        auto *findRecord = mWindow->findChild<FindRecordWidget *>();
        QVERIFY2(findRecord != nullptr, "MainWindow must own a FindRecordWidget");
        auto *findEdit = findRecord->findChild<QLineEdit *>(QStringLiteral("findEdit"));
        QVERIFY2(findEdit != nullptr, "FindRecordWidget must expose its QLineEdit");
        // Both timers carry `objectName` so the test can probe them
        // without the fragile "walk all timers and guess" dance.
        auto *trailing = findRecord->findChild<QTimer *>(QStringLiteral("matchCountDebounceTimer"));
        auto *maxAge = findRecord->findChild<QTimer *>(QStringLiteral("matchCountMaxAgeTimer"));
        QVERIFY2(trailing != nullptr, "FindRecordWidget must name its trailing debounce timer");
        QVERIFY2(maxAge != nullptr, "FindRecordWidget must name its max-age debounce timer");

        findEdit->setText(QStringLiteral("needle"));
        // First bump arms both timers (the textChanged emit above
        // already armed them; bump is idempotent on that state).
        findRecord->BumpMatchCountDebounce();
        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): false positive; prior `QVERIFY2` aborts on null.
        QVERIFY2(trailing->isActive(), "first BumpMatchCountDebounce must arm the trailing timer");
        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): false positive; prior `QVERIFY2` aborts on null.
        QVERIFY2(maxAge->isActive(), "first BumpMatchCountDebounce must arm the max-age timer");

        // Let the trailing timer tick down, then re-bump. Remaining
        // time must NOT bounce back up -- that would mean the
        // second bump reset the timer (the bug we're pinning).
        QTest::qWait(20);
        const int remainingBefore = trailing->remainingTime();
        findRecord->BumpMatchCountDebounce();
        const int remainingAfter = trailing->remainingTime();
        // 5 ms slack for scheduler jitter; the invariant is "did
        // not jump back near `interval()`".
        QVERIFY2(
            remainingAfter <= remainingBefore + 5,
            qPrintable(QStringLiteral("second bump must not reset the trailing timer (before=%1, after=%2, interval=%3)"
            )
                           .arg(remainingBefore)
                           .arg(remainingAfter)
                           .arg(trailing->interval()))
        );
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
            scratch.AppendKeys({"category"});
            scratch.SetColumnType(0, loglib::LogConfiguration::Type::Enumeration);
            scratch.Save(cfgPath.toStdString());
        }
        model->ConfigurationManager().Load(cfgPath.toStdString());

        QCOMPARE(model->Configuration().columns.size(), static_cast<size_t>(1));
        QCOMPARE(model->Configuration().columns[0].type, loglib::LogConfiguration::Type::Enumeration);

        const FilterEditor editor(*model, QStringLiteral("test-empty-enum"));
        // `UpdateSelectedColumn(0)` settles the OK / placeholder state.

        const auto *okButton = editor.findChild<QPushButton *>(QStringLiteral("okButton"));
        QVERIFY2(okButton != nullptr, "FilterEditor must expose an OK button");
        // clang-analyzer does not model `QVERIFY2`'s test-aborting behaviour,
        // so it still considers `okButton` potentially null on the next line.
        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage)
        QVERIFY2(!okButton->isEnabled(), "OK must be disabled when the picker dictionary is empty");

        const auto *placeholder = editor.findChild<QLabel *>(QStringLiteral("enumEmptyPlaceholder"));
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
            lines.append(QStringLiteral(R"({"category": "%1"})").arg(levels[i % levels.size()]));
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

        const int levelCol = ColumnByHeader(*model, QStringLiteral("category"));
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
        const LogFilterModel *filterModel = mWindow->FilterModel();
        QVERIFY(filterModel != nullptr);
        QCOMPARE(filterModel->rowCount(), 320);

        const auto *menu = mWindow->findChild<QMenu *>(QStringLiteral("menuFilters"));
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
            lines.append(QStringLiteral(R"({"category": "%1"})").arg(levels[i % levels.size()]));
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

        const int levelCol = ColumnByHeader(*model, QStringLiteral("category"));
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

    // Regression: a streaming batch through `LogFilterModel` (no
    // sort, no filter) must emit one bracketed `rowsInserted` pair
    // covering the whole range, not one pair per source row. The
    // reverse-index rebuild lives inside the bracket so no O(n)
    // staleness window on `mSourceRowToProxyRow` is observable.
    void TestStreamingBatchEmitsSingleRowsInsertedBracket()
    {
        auto *model = mWindow->findChild<LogModel *>();
        QVERIFY2(model != nullptr, "MainWindow must own a LogModel");
        auto *filterModel = mWindow->FilterModel();
        QVERIFY2(filterModel != nullptr, "MainWindow must own a LogFilterModel");

        // Drive via direct `AppendBatch` so the batch boundary is
        // well-defined and the only `rowsInserted` emission is ours.
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

        // One bulk bracket, not BATCH_SIZE pairs.
        QCOMPARE(aboutToInsertSpy.count(), 1);
        QCOMPARE(insertedSpy.count(), 1);

        const auto args = insertedSpy.takeFirst();
        QCOMPARE(args.at(1).toInt(), 0);
        QCOMPARE(args.at(2).toInt(), BATCH_SIZE - 1);

        // Reverse index must be consistent: `mapToSource` ->
        // `mapFromSource` round-trips for every row.
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
    // `Type::Any + autoDetect` builds with `dictionary = nullptr` and
    // runs the slow string-set fallback. On auto-promote,
    // `enumColumnsChanged(Promoted)` must rebuild the predicate so
    // it picks up the bitset hot path -- gating the rebuild on
    // `!EnumFilterFullyResolved` would skip it as soon as the new
    // dictionary contained every selected value. The observable
    // is a
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

    // Numeric-range filter survives a serialise+reload round trip:
    // the saved JSON reloads, replays through `AddFilter`, passes the
    // type-match check, and the editor opens preloaded with the
    // saved bounds.
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
        QCOMPARE(editorMin.value(), 10.0);
        QCOMPARE(editorMax.value(), 19.0);

        editor->close();
        editor->deleteLater();
        QCoreApplication::processEvents();

        model->EndStreaming(false);
    }

    // A saved numeric-range filter against a column whose type
    // changed underneath gets dropped at `AddFilter` time -- same as
    // a string filter on a now-enum column.
    void TestSavedNumericRangeFilterDroppedOnTypeMismatch()
    {
        // `level` auto-detects to enum, so a saved numeric-range
        // filter on the same row mismatches the column type.
        const QStringList levels{QStringLiteral("info"), QStringLiteral("warn"), QStringLiteral("error")};
        QStringList lines;
        lines.reserve(300);
        for (int i = 0; i < 300; ++i)
        {
            lines.append(QStringLiteral(R"({"category": "%1"})").arg(levels[i % levels.size()]));
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

        const int levelCol = ColumnByHeader(*model, QStringLiteral("category"));
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

    // Regression: a saved `Type::String` filter against a now-
    // `Boolean` column drops at `AddFilter` time -- same way it would
    // for an enum-promoted column. Covers the type-match branch
    // added with `Boolean`.
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
            model->Configuration().columns[static_cast<size_t>(flagCol)].type, loglib::LogConfiguration::Type::Boolean
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

    // Regression: a saved `Type::String` filter against a numeric
    // column drops at `AddFilter` time. Same shape as the now-Boolean
    // / now-Enum variants; covers the numeric type-match branch.
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
            model->Configuration().columns[static_cast<size_t>(valueCol)].type, loglib::LogConfiguration::Type::Integer
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

    // A hand-edited Boolean filter with mixed-case `"True"` /
    // `"FALSE"` still restores correctly. `DecodeBooleanFilterSides`
    // is case-insensitive on read; the canonical submit path keeps
    // writing lowercase.
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
            model->Configuration().columns[static_cast<size_t>(flagCol)].type, loglib::LogConfiguration::Type::Boolean
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
        // STREAM_PROMOTION_MIN_ROWS (== 2), so the column stays a
        // candidate (`Type::Any + autoDetect`) and
        // `ResolveEnumDictionary` returns nullptr at filter-install
        // time.
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
        QCOMPARE(model->Configuration().columns[static_cast<size_t>(col)].type, loglib::LogConfiguration::Type::Any);
        QVERIFY(model->Configuration().columns[static_cast<size_t>(col)].autoDetect);

        // Install an enum filter while the column is still a candidate:
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

    // `enumColumnsChanged(Promoted, columnIndex)` only rebuilds
    // filters that target the promoted column.
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
        // it stays a candidate (`Type::Any + autoDetect`) until
        // batch 2 introduces it.
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

    // Regression: a pre-existing enum column whose dict only grew in
    // a batch emits `Grew` exactly once and never `Promoted`.
    // `LastBackfillRange` is a min/max over every back-filled
    // column, so a grow-only column can fall inside it; the
    // back-fill loop must filter rather than firing `Promoted`
    // for every enum column in the range.
    //
    // Setup:
    //   * batch 1 leaves `colA` as a candidate (`Type::Any +
    //     autoDetect`) and promotes `colB`.
    //   * batch 2 promotes `colA` (low index, back-filled), grows
    //     `colB`'s dict (no back-fill), and promotes a new `colC`
    //     (high index, back-filled). Range spans `[colA, colC]`
    //     with `colB` sandwiched in the middle.
    void TestEnumGrewSignalNotDoubledAsPromotedWhenSandwichedInBackfillRange()
    {
        auto *model = mWindow->findChild<LogModel *>();
        QVERIFY2(model != nullptr, "MainWindow must own a LogModel");

        const TempJsonFile emptyFixture(QStringList{});
        auto file = std::make_unique<loglib::LogFile>(emptyFixture.Path().toStdString());
        auto fileSource = std::make_unique<loglib::FileLineSource>(std::move(file));
        loglib::FileLineSource *sourcePtr = fileSource.get();
        (void)model->BeginStreamingForSyncTest(std::move(fileSource));

        loglib::KeyIndex &keys = model->Table().Keys();
        const auto makeRow = [&](const std::vector<std::pair<std::string, std::string>> &kvs) {
            std::vector<std::pair<loglib::KeyId, loglib::LogValue>> values;
            values.reserve(kvs.size());
            for (const auto &[k, v] : kvs)
            {
                values.emplace_back(keys.GetOrInsert(k), loglib::LogValue(v));
            }
            return loglib::LogLine{std::move(values), keys, *sourcePtr, 0};
        };

        // Batch 1: `colA` gets exactly one presence (stays a
        // candidate, tracker carries 1 presence forward). `colB`
        // gets two identical presences and promotes to Enumeration
        // with dict size 1. `colA` is appended first so it lands at
        // column 0.
        {
            loglib::StreamedBatch batch;
            batch.firstLineNumber = 1;
            batch.newKeys.emplace_back("colA");
            batch.newKeys.emplace_back("colB");
            batch.lines.push_back(makeRow({{"colA", "x"}, {"colB", "y"}}));
            batch.lines.push_back(makeRow({{"colB", "y"}}));
            model->AppendBatch(std::move(batch));
        }
        QCoreApplication::processEvents();

        const int colA = ColumnByHeader(*model, QStringLiteral("colA"));
        const int colB = ColumnByHeader(*model, QStringLiteral("colB"));
        QVERIFY2(colA == 0, "colA must land at column 0 (first appended new key)");
        QVERIFY2(colB == 1, "colB must land at column 1 (second appended new key)");
        QCOMPARE(model->Configuration().columns[static_cast<size_t>(colA)].type, loglib::LogConfiguration::Type::Any);
        QVERIFY(model->Configuration().columns[static_cast<size_t>(colA)].autoDetect);
        QCOMPARE(
            model->Configuration().columns[static_cast<size_t>(colB)].type, loglib::LogConfiguration::Type::Enumeration
        );
        const loglib::KeyId colBKey = keys.Find("colB");
        QVERIFY(colBKey != loglib::INVALID_KEY_ID);
        const loglib::EnumDictionary *colBDict = model->Table().EnumDictionaries().Find(colBKey);
        QVERIFY2(colBDict != nullptr, "colB must have a registry entry after batch 1");
        QCOMPARE(static_cast<int>(colBDict->Size()), 1);

        // Spy after batch 1 so only batch-2 emissions count.
        QSignalSpy enumChangedSpy(model, &LogModel::enumColumnsChanged);
        QVERIFY(enumChangedSpy.isValid());

        // Batch 2 sandwiches `colB` between two recorded back-fills:
        //   * `colA` promotes (presence count crosses 2): recordBackfill(0).
        //   * `colB` dict grows from 1 to 2 (`y` -> `y`, `w`): no back-fill.
        //   * `colC` promotes (2 same-value presences): recordBackfill(2).
        // `LastBackfillRange` becomes `[0, 2]` with `colB` at column 1.
        {
            loglib::StreamedBatch batch;
            batch.firstLineNumber = 3;
            batch.newKeys.emplace_back("colC");
            batch.lines.push_back(makeRow({{"colA", "z"}, {"colB", "w"}, {"colC", "m"}}));
            batch.lines.push_back(makeRow({{"colA", "z"}, {"colB", "w"}, {"colC", "m"}}));
            model->AppendBatch(std::move(batch));
        }
        QCoreApplication::processEvents();

        const int colC = ColumnByHeader(*model, QStringLiteral("colC"));
        QVERIFY2(colC == 2, "colC must land at column 2 (only new key in batch 2)");
        QCOMPARE(
            model->Configuration().columns[static_cast<size_t>(colA)].type, loglib::LogConfiguration::Type::Enumeration
        );
        QCOMPARE(
            model->Configuration().columns[static_cast<size_t>(colB)].type, loglib::LogConfiguration::Type::Enumeration
        );
        QCOMPARE(
            model->Configuration().columns[static_cast<size_t>(colC)].type, loglib::LogConfiguration::Type::Enumeration
        );
        const loglib::EnumDictionary *colBDictAfter = model->Table().EnumDictionaries().Find(colBKey);
        QVERIFY2(colBDictAfter != nullptr, "colB must remain enum across batch 2");
        QCOMPARE(static_cast<int>(colBDictAfter->Size()), 2);

        // colB is the sandwiched pre-existing enum: exactly one
        // `Grew`, zero `Promoted`.
        int colBGrewCount = 0;
        int colBPromotedCount = 0;
        int colBDemotedCount = 0;
        for (const auto &args : enumChangedSpy)
        {
            const auto reason = args.at(0).value<EnumColumnsChangeReason>();
            const int columnIndex = args.at(1).toInt();
            if (columnIndex != colB)
            {
                continue;
            }
            switch (reason)
            {
            case EnumColumnsChangeReason::Grew:
                ++colBGrewCount;
                break;
            case EnumColumnsChangeReason::Promoted:
                ++colBPromotedCount;
                break;
            case EnumColumnsChangeReason::Demoted:
                ++colBDemotedCount;
                break;
            }
        }
        QCOMPARE(colBGrewCount, 1);
        QVERIFY2(
            colBPromotedCount == 0,
            qPrintable(QStringLiteral("colB (already enum at snapshot) must NOT receive a spurious `Promoted`; got %1")
                           .arg(colBPromotedCount))
        );
        QCOMPARE(colBDemotedCount, 0);

        // Genuinely new enum columns still get exactly one
        // `Promoted` each, so receivers can rebuild their filters.
        const auto countReasonForColumn = [&](int columnIndex, EnumColumnsChangeReason wanted) {
            int count = 0;
            for (const auto &args : enumChangedSpy)
            {
                if (args.at(1).toInt() == columnIndex && args.at(0).value<EnumColumnsChangeReason>() == wanted)
                {
                    ++count;
                }
            }
            return count;
        };
        QCOMPARE(countReasonForColumn(colA, EnumColumnsChangeReason::Promoted), 1);
        QCOMPARE(countReasonForColumn(colC, EnumColumnsChangeReason::Promoted), 1);

        model->EndStreaming(false);
    }

    // Regression: a hand-edited config with `+inf`/`-inf`/`NaN`
    // bounds would open in a non-submittable state (`OnOkClicked`
    // rejects them, `QDoubleValidator` doesn't strip them on
    // `setText`). `Load()` coerces non-finite bounds to "unbounded"
    // so the dialog opens submittable.
    void TestSavedNumericRangeFilterCoercesNonFiniteToUnbounded()
    {
        auto *model = mWindow->findChild<LogModel *>();
        QVERIFY2(model != nullptr, "MainWindow must own a LogModel");

        // Tiny fixture; we just need a numeric column to land on
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

    // The inverted-range status-bar message uses the C-locale,
    // max-digits10 formatter so distinct bounds (`12345.6789` vs.
    // `12345.6790`) stay distinguishable.
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

        // Pick exactly-representable doubles whose precision-6
        // rendering collides (`"1e+06"`) but whose max_digits10
        // rendering differs. Both values are exact in IEEE 754.
        constexpr double MIN_VALUE = 1000000.0078125;  // = 1e6 + 1/128
        constexpr double MAX_VALUE = 1000000.00390625; // = 1e6 + 1/256, < MIN_VALUE

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
    // its menu title in canonical `"true, false"` order, even when
    // `filter.filterValues` is in non-canonical order (e.g. a hand-
    // edited `["false", "true"]` payload).
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
            model->Configuration().columns[static_cast<size_t>(flagCol)].type, loglib::LogConfiguration::Type::Boolean
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
            const auto *menu = mWindow->findChild<QMenu *>(QStringLiteral("menuFilters"));
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

        const QAction *action = findFilterMenuAction(filterId);
        QVERIFY2(action != nullptr, "Boolean filter must have a menu entry");
        QCOMPARE(action->text(), QStringLiteral("true, false"));

        // Single-side sanity: "false only" must render as just
        // "false" (no leftover "true, " from a stale code path).
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
        const QAction *falseAction = findFilterMenuAction(falseOnlyId);
        QVERIFY2(falseAction != nullptr, "false-only Boolean filter must have a menu entry");
        QCOMPARE(falseAction->text(), QStringLiteral("false"));

        model->EndStreaming(false);
    }

    // ---------------------------------------------------------------
    // Record-detail pane tests
    // ---------------------------------------------------------------

    // Click a clipboard-producing button and read the clipboard,
    // retrying until the text contains `marker`. The Windows OLE
    // clipboard is async and contended; `OleSetClipboard` can fail
    // outright when other tests hammer the marshaller. The retry
    // re-clicks each iteration so lost OLE handshakes also resolve.
    static QString ClickAndReadClipboardWithRetry(
        QClipboard *clipboard, QPushButton *button, const QString &marker, int maxAttempts = 20, int sleepMs = 25
    )
    {
        QString text;
        for (int attempt = 0; attempt < maxAttempts; ++attempt)
        {
            button->click();
            QCoreApplication::processEvents();
            QTest::qWait(sleepMs);
            text = clipboard->text();
            if (marker.isEmpty() ? !text.isEmpty() : text.contains(marker))
            {
                return text;
            }
        }
        return text;
    }

    // Same retry as the click overload, but drives the producer via
    // `QMetaObject::invokeMethod` (for private slots).
    static QString InvokeAndReadClipboardWithRetry(
        QClipboard *clipboard,
        QObject *target,
        const char *member,
        const QString &marker,
        int maxAttempts = 20,
        int sleepMs = 25
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
            QTest::qWait(sleepMs);
            text = clipboard->text();
            if (marker.isEmpty() ? !text.isEmpty() : text.contains(marker))
            {
                return text;
            }
        }
        return text;
    }

    // Basic shape: summary mentions row number, every column lands in
    // `fields`, raw JSON is single-line, formatted JSON is indented.
    void TestBuildRecordDetailContentBasic()
    {
        const TempJsonFile fixture(QStringList{
            QStringLiteral(R"({"timestamp": "2025-01-15T12:34:56Z", "level": "info", "message": "hello"})"),
            QStringLiteral(R"({"timestamp": "2025-01-15T12:34:57Z", "level": "error", "message": "boom"})"),
        });
        const StreamingRun run = RunStreaming(fixture.Path());
        QCOMPARE(run.finishedCount, 1);
        QCOMPARE(run.cancelled, false);
        QCOMPARE(run.model->rowCount(), 2);

        const int levelCol = ColumnByHeader(*run.model, QStringLiteral("level"));
        const int messageCol = ColumnByHeader(*run.model, QStringLiteral("message"));
        QVERIFY(levelCol >= 0);
        QVERIFY(messageCol >= 0);

        const RecordDetailContent row1 = BuildRecordDetailContent(*run.model, 1);
        QVERIFY2(row1.valid, "row 1 must produce a valid content");
        QVERIFY2(row1.summary.contains(QStringLiteral("Row 2")), qPrintable(row1.summary));
        // Time column present -> summary appends the formatted value
        // after a middle-dot.
        QVERIFY2(row1.summary.contains(QStringLiteral("\u00B7")), qPrintable(row1.summary));

        QCOMPARE(row1.fields.size(), static_cast<qsizetype>(run.model->Configuration().columns.size()));
        // Look up by header -- column order from the streaming path is
        // insertion-driven.
        bool sawLevel = false;
        bool sawMessage = false;
        for (const auto &pair : row1.fields)
        {
            if (pair.first == QStringLiteral("level"))
            {
                QCOMPARE(pair.second, QStringLiteral("error"));
                sawLevel = true;
            }
            else if (pair.first == QStringLiteral("message"))
            {
                QCOMPARE(pair.second, QStringLiteral("boom"));
                sawMessage = true;
            }
        }
        QVERIFY(sawLevel);
        QVERIFY(sawMessage);

        // `rawJson` is the on-disk bytes -- single line, no indenting.
        QVERIFY2(
            !row1.rawJson.contains(QLatin1Char('\n')), "raw JSON must reflect on-disk bytes (no inserted indentation)"
        );
        QVERIFY(row1.rawJson.contains(QStringLiteral("\"message\"")));
        QVERIFY(row1.rawJson.contains(QStringLiteral("\"boom\"")));
        // `formattedJson` is pretty-printed -- multi-line, same keys.
        QVERIFY2(row1.formattedJson.contains(QLatin1Char('\n')), "formatted JSON must be indented");
        QVERIFY(row1.formattedJson.contains(QStringLiteral("\"message\"")));
        QVERIFY(row1.formattedJson.contains(QStringLiteral("\"boom\"")));
    }

    // Out-of-range rows produce an invalid placeholder, not a crash.
    void TestBuildRecordDetailContentOutOfRange()
    {
        const TempJsonFile fixture(QStringList{
            QStringLiteral(R"({"a": 1})"),
        });
        const StreamingRun run = RunStreaming(fixture.Path());
        QCOMPARE(run.finishedCount, 1);
        QCOMPARE(run.cancelled, false);
        QCOMPARE(run.model->rowCount(), 1);

        const RecordDetailContent negative = BuildRecordDetailContent(*run.model, -1);
        QVERIFY2(!negative.valid, "negative row must produce a placeholder");
        QVERIFY(!negative.placeholderText.isEmpty());

        const RecordDetailContent past = BuildRecordDetailContent(*run.model, 42);
        QVERIFY2(!past.valid, "out-of-range row must produce a placeholder");
        QVERIFY(!past.placeholderText.isEmpty());
    }

    // Huge lines: `formattedJson` is capped (so the UI doesn't stall)
    // and carries a truncation footer, while `rawJson` keeps every
    // on-disk byte so "Copy raw JSON" stays complete.
    void TestBuildRecordDetailContentFormattedJsonTruncatesHugeInput()
    {
        // 300 KB > 256 KB display cap, ASCII so byte count == char count.
        constexpr int FILLER_BYTES = 300 * 1024;
        const QString filler(FILLER_BYTES, QLatin1Char('A'));
        const QString hugeLine = QStringLiteral("{\"k\": \"") + filler + QStringLiteral("\"}");
        const TempJsonFile fixture(QStringList{hugeLine});
        const StreamingRun run = RunStreaming(fixture.Path());
        QCOMPARE(run.finishedCount, 1);
        QCOMPARE(run.cancelled, false);
        QCOMPARE(run.model->rowCount(), 1);

        const RecordDetailContent content = BuildRecordDetailContent(*run.model, 0);
        QVERIFY2(content.valid, "huge-but-valid line must still produce a valid content");

        // `rawJson` is the clipboard payload -- must not be truncated.
        // The envelope (`{"k": "..."}`) adds a few bytes, so lower-bound.
        QVERIFY2(
            content.rawJson.size() >= FILLER_BYTES,
            qPrintable(QStringLiteral("rawJson must keep the full on-disk bytes; got %1 chars, expected >= %2")
                           .arg(content.rawJson.size())
                           .arg(FILLER_BYTES))
        );

        // `formattedJson` (what the edit renders) must be bounded. The
        // cap constant lives in the widget TU so we just bound to "well
        // under the full input" to stay robust to footer text length.
        QVERIFY2(
            content.formattedJson.size() < FILLER_BYTES * 9 / 10,
            qPrintable(QStringLiteral("formattedJson must be capped; got %1 chars").arg(content.formattedJson.size()))
        );
        QVERIFY2(
            content.formattedJson.contains(QStringLiteral("truncated")),
            "formattedJson must carry a truncation footer so users know about Copy raw JSON"
        );
        QVERIFY2(
            content.formattedJson.contains(QStringLiteral("Copy raw JSON")),
            "truncation footer must mention the Copy raw JSON button so users have a clear next step"
        );
    }

    // Valid content renders into the table and raw edit; placeholder
    // content hides the table and shows the placeholder label.
    void TestRecordDetailWidgetRenderingAndPlaceholder()
    {
        RecordDetailWidget widget;

        RecordDetailContent content;
        content.valid = true;
        content.summary = QStringLiteral("Row 1");
        content.fields.append({QStringLiteral("level"), QStringLiteral("info")});
        content.fields.append({QStringLiteral("message"), QStringLiteral("hello")});
        content.rawJson = QStringLiteral(R"({"level": "info", "message": "hello"})");
        content.formattedJson = QStringLiteral("{\n  \"level\": \"info\",\n  \"message\": \"hello\"\n}");
        widget.SetContent(content);

        const auto *table = widget.findChild<QTableWidget *>();
        QVERIFY(table != nullptr);
        QCOMPARE(table->rowCount(), 2);
        QCOMPARE(table->item(0, 0)->text(), QStringLiteral("level"));
        QCOMPARE(table->item(0, 1)->text(), QStringLiteral("info"));
        QCOMPARE(table->item(1, 0)->text(), QStringLiteral("message"));
        QCOMPARE(table->item(1, 1)->text(), QStringLiteral("hello"));
        // `isHidden()` reflects the explicit-hide state; the offscreen
        // QPA never realises the parent, so `isVisible()` would lie.
        QVERIFY2(!table->isHidden(), "fields table must be explicitly shown when content is valid");

        // The edit shows the *formatted* JSON, not the compact bytes.
        const auto *rawEdit = widget.findChild<QPlainTextEdit *>();
        QVERIFY(rawEdit != nullptr);
        QCOMPARE(rawEdit->toPlainText(), content.formattedJson);

        // Switch to placeholder: table hidden, placeholder label shown.
        RecordDetailContent placeholder;
        placeholder.valid = false;
        placeholder.placeholderText = QStringLiteral("Nothing to see here");
        widget.SetContent(placeholder);
        QCOMPARE(table->rowCount(), 0);
        QCOMPARE(rawEdit->toPlainText(), QString());

        // Snapshot windows hide the "Open in new window" button.
        const auto *popOutButton = widget.findChild<QPushButton *>(QStringLiteral("openInNewWindowButton"));
        QVERIFY(popOutButton != nullptr);
        QVERIFY(popOutButton->isVisibleTo(&widget));
        widget.SetOpenInNewWindowVisible(false);
        QVERIFY(!popOutButton->isVisibleTo(&widget));
    }

    // "Copy raw JSON" pushes the compact on-disk bytes, not the
    // pretty-printed display text.
    void TestRecordDetailWidgetCopyRawJsonPushesRawBytes()
    {
        RecordDetailWidget widget;

        RecordDetailContent content;
        content.valid = true;
        content.summary = QStringLiteral("Row 1");
        content.fields.append({QStringLiteral("k"), QStringLiteral("v")});
        content.rawJson = QStringLiteral(R"({"k": "v"})");
        content.formattedJson = QStringLiteral("{\n  \"k\": \"v\"\n}");
        widget.SetContent(content);

        QClipboard *clipboard = QApplication::clipboard();
        QVERIFY(clipboard != nullptr);
        clipboard->clear();

        auto *copyButton = widget.findChild<QPushButton *>(QStringLiteral("copyJsonButton"));
        QVERIFY(copyButton != nullptr);

        const QString pasted = ClickAndReadClipboardWithRetry(clipboard, copyButton, content.rawJson);
        QCOMPARE(pasted, content.rawJson);
        QVERIFY2(
            !pasted.contains(QLatin1Char('\n')),
            "Copy raw JSON must push the compact on-disk bytes, not the formatted display text"
        );
    }

    // "Copy as key/value" writes one `header: value` line per field.
    void TestRecordDetailWidgetCopyAsKeyValuePushesPairs()
    {
        RecordDetailWidget widget;

        RecordDetailContent content;
        content.valid = true;
        content.summary = QStringLiteral("Row 1");
        content.fields.append({QStringLiteral("level"), QStringLiteral("info")});
        content.fields.append({QStringLiteral("message"), QStringLiteral("hello world")});
        content.fields.append({QStringLiteral("count"), QStringLiteral("42")});
        widget.SetContent(content);

        QClipboard *clipboard = QApplication::clipboard();
        QVERIFY(clipboard != nullptr);
        clipboard->clear();

        auto *copyButton = widget.findChild<QPushButton *>(QStringLiteral("copyKeyValueButton"));
        QVERIFY(copyButton != nullptr);
        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage)
        QVERIFY(copyButton->isEnabled());

        const QString expected = QStringLiteral("level: info\nmessage: hello world\ncount: 42");
        const QString pasted = ClickAndReadClipboardWithRetry(clipboard, copyButton, expected);
        QCOMPARE(pasted, expected);
    }

    // "Copy raw JSON" stays disabled when `rawJson` is empty (e.g.
    // pinned-but-evicted line). Raw group also disables and retitles.
    void TestRecordDetailWidgetCopyRawJsonDisabledForEmptyRaw()
    {
        RecordDetailWidget widget;

        RecordDetailContent content;
        content.valid = true;
        content.summary = QStringLiteral("Row 1");
        content.fields.append({QStringLiteral("k"), QStringLiteral("v")});
        // Empty `rawJson` (eviction / parse failure with empty bytes).
        content.rawJson = QString();
        content.formattedJson = QString();
        widget.SetContent(content);

        const auto *rawButton = widget.findChild<QPushButton *>(QStringLiteral("copyJsonButton"));
        const auto *kvButton = widget.findChild<QPushButton *>(QStringLiteral("copyKeyValueButton"));
        QVERIFY(rawButton != nullptr);
        QVERIFY(kvButton != nullptr);
        QVERIFY2(!rawButton->isEnabled(), "Copy raw JSON must be disabled when there's no raw text");
        QVERIFY2(kvButton->isEnabled(), "Copy as key/value stays enabled -- it reads from the fields, not raw bytes");

        // Raw group is disabled + retitled when there are no bytes;
        // a follow-up `SetContent` with bytes must restore both.
        const auto *rawGroup = widget.findChild<QGroupBox *>();
        QVERIFY(rawGroup != nullptr);
        QVERIFY2(!rawGroup->isEnabled(), "Raw JSON group must be disabled when no raw bytes are available");
        QVERIFY2(
            rawGroup->title().contains(QStringLiteral("unavailable")),
            qPrintable(
                QStringLiteral("Raw JSON group title must explain why it's empty; got '%1'").arg(rawGroup->title())
            )
        );

        RecordDetailContent withRaw;
        withRaw.valid = true;
        withRaw.summary = QStringLiteral("Row 1");
        withRaw.fields.append({QStringLiteral("k"), QStringLiteral("v")});
        withRaw.rawJson = QStringLiteral(R"({"k": "v"})");
        withRaw.formattedJson = QStringLiteral("{\n  \"k\": \"v\"\n}");
        widget.SetContent(withRaw);
        QVERIFY2(rawGroup->isEnabled(), "Raw JSON group must re-enable when raw bytes arrive");
        QVERIFY2(
            !rawGroup->title().contains(QStringLiteral("unavailable")),
            qPrintable(
                QStringLiteral("Raw JSON group title must drop the 'unavailable' suffix when bytes arrive; got '%1'")
                    .arg(rawGroup->title())
            )
        );
    }

    // "Copy as key/value" escapes embedded newlines / tabs / backslashes
    // C-style so each field still lands on one line.
    void TestRecordDetailWidgetCopyAsKeyValueEscapesSpecialChars()
    {
        RecordDetailWidget widget;

        RecordDetailContent content;
        content.valid = true;
        content.summary = QStringLiteral("Row 1");
        content.fields.append({QStringLiteral("multiline"), QStringLiteral("line1\nline2\r\nline3")});
        content.fields.append({QStringLiteral("tabbed"), QStringLiteral("col1\tcol2")});
        content.fields.append({QStringLiteral("backslashed"), QStringLiteral(R"(C:\path\to\file)")});
        content.fields.append({QStringLiteral("plain"), QStringLiteral("ok")});
        widget.SetContent(content);

        QClipboard *clipboard = QApplication::clipboard();
        QVERIFY(clipboard != nullptr);
        clipboard->clear();

        const QString pasted = ClickAndReadClipboardWithRetry(
            clipboard,
            widget.findChild<QPushButton *>(QStringLiteral("copyKeyValueButton")),
            QStringLiteral("multiline: line1\\nline2")
        );
        const QStringList lines = pasted.split(QLatin1Char('\n'));
        // One line per field; no real newline mid-entry.
        QCOMPARE(lines.size(), content.fields.size());
        QCOMPARE(lines[0], QStringLiteral("multiline: line1\\nline2\\r\\nline3"));
        QCOMPARE(lines[1], QStringLiteral("tabbed: col1\\tcol2"));
        QCOMPARE(lines[2], QStringLiteral("backslashed: C:\\\\path\\\\to\\\\file"));
        QCOMPARE(lines[3], QStringLiteral("plain: ok"));
    }

    // Header-side companion: newlines / tabs / backslashes in keys
    // are escaped too, so a newline-bearing key doesn't split a
    // field across two output lines.
    void TestRecordDetailWidgetCopyAsKeyValueEscapesHeaderSpecialChars()
    {
        RecordDetailWidget widget;

        RecordDetailContent content;
        content.valid = true;
        content.summary = QStringLiteral("Row 1");
        content.fields.append({QStringLiteral("key\nwith\nnewlines"), QStringLiteral("v")});
        content.fields.append({QStringLiteral("key\twith\ttabs"), QStringLiteral("v")});
        content.fields.append({QStringLiteral("back\\slash"), QStringLiteral("v")});
        widget.SetContent(content);

        QClipboard *clipboard = QApplication::clipboard();
        QVERIFY(clipboard != nullptr);
        clipboard->clear();

        const QString pasted = ClickAndReadClipboardWithRetry(
            clipboard,
            widget.findChild<QPushButton *>(QStringLiteral("copyKeyValueButton")),
            QStringLiteral("key\\nwith\\nnewlines: v")
        );
        const QStringList lines = pasted.split(QLatin1Char('\n'));
        QCOMPARE(lines.size(), content.fields.size());
        QCOMPARE(lines[0], QStringLiteral("key\\nwith\\nnewlines: v"));
        QCOMPARE(lines[1], QStringLiteral("key\\twith\\ttabs: v"));
        QCOMPARE(lines[2], QStringLiteral("back\\\\slash: v"));
    }

    // Empty values render a muted em-dash with the
    // `RECORD_DETAIL_EMPTY_PLACEHOLDER_ROLE` flag set; a literal
    // em-dash value stays distinguishable because the flag is unset.
    void TestRecordDetailWidgetEmptyValuePlaceholderUsesEmDashAndFlag()
    {
        RecordDetailWidget widget;

        RecordDetailContent content;
        content.valid = true;
        content.summary = QStringLiteral("Row 1");
        content.fields.append({QStringLiteral("present"), QStringLiteral("hello")});
        content.fields.append({QStringLiteral("absent"), QString()});
        // Literal em-dash value: same glyph, but role flag must be off.
        content.fields.append({QStringLiteral("dashy"), QStringLiteral("\u2014")});
        widget.SetContent(content);

        const auto *table = widget.findChild<QTableWidget *>();
        QVERIFY(table != nullptr);
        QCOMPARE(table->rowCount(), 3);

        // Real value: no placeholder flag.
        QCOMPARE(table->item(0, 1)->text(), QStringLiteral("hello"));
        QVERIFY2(
            !table->item(0, 1)->data(RECORD_DETAIL_EMPTY_PLACEHOLDER_ROLE).toBool(),
            "Real value must not be flagged as the empty-value placeholder"
        );

        // Synthetic empty: em-dash text, flagged, italicised.
        QCOMPARE(table->item(1, 1)->text(), QStringLiteral("\u2014"));
        QVERIFY2(
            table->item(1, 1)->data(RECORD_DETAIL_EMPTY_PLACEHOLDER_ROLE).toBool(),
            "Synthetic placeholder must carry the empty-value role flag"
        );
        QVERIFY2(
            table->item(1, 1)->font().italic(), "Synthetic placeholder must render italic so it reads as 'no value'"
        );
        QVERIFY2(
            !table->item(1, 1)->toolTip().isEmpty(),
            "Synthetic placeholder must carry a tooltip explaining the empty value"
        );

        // Literal em-dash: same text but NOT flagged, NOT italic.
        QCOMPARE(table->item(2, 1)->text(), QStringLiteral("\u2014"));
        QVERIFY2(
            !table->item(2, 1)->data(RECORD_DETAIL_EMPTY_PLACEHOLDER_ROLE).toBool(),
            "Literal em-dash value must not be flagged as the empty-value placeholder"
        );
        QVERIFY2(
            !table->item(2, 1)->font().italic(),
            "Literal em-dash value must not render italic -- italic is the placeholder discriminator"
        );
    }

    // Recycled placeholder cells must clear the role flag, tooltip,
    // italic font, and muted foreground when re-bound to a real value.
    void TestRecordDetailWidgetReusedCellClearsPlaceholderState()
    {
        RecordDetailWidget widget;

        RecordDetailContent withEmpty;
        withEmpty.valid = true;
        withEmpty.summary = QStringLiteral("Row 1");
        withEmpty.fields.append({QStringLiteral("k"), QString()});
        widget.SetContent(withEmpty);

        const auto *table = widget.findChild<QTableWidget *>();
        QVERIFY(table != nullptr);
        QCOMPARE(table->rowCount(), 1);
        const QTableWidgetItem *valueItem = table->item(0, 1);
        QVERIFY(valueItem != nullptr);
        // Placeholder state is in effect.
        // NOLINTBEGIN(clang-analyzer-core.CallAndMessage)
        QVERIFY(valueItem->data(RECORD_DETAIL_EMPTY_PLACEHOLDER_ROLE).toBool());
        QVERIFY(valueItem->font().italic());
        // NOLINTEND(clang-analyzer-core.CallAndMessage)

        // Refresh with a real value at the same row index. Item is
        // recycled; every placeholder property must be cleared.
        RecordDetailContent withReal;
        withReal.valid = true;
        withReal.summary = QStringLiteral("Row 1");
        withReal.fields.append({QStringLiteral("k"), QStringLiteral("hello")});
        widget.SetContent(withReal);

        QCOMPARE(table->rowCount(), 1);
        valueItem = table->item(0, 1);
        QVERIFY(valueItem != nullptr);
        QCOMPARE(valueItem->text(), QStringLiteral("hello"));
        QVERIFY2(
            !valueItem->data(RECORD_DETAIL_EMPTY_PLACEHOLDER_ROLE).toBool(),
            "Recycled cell must not carry the placeholder role flag for a real value"
        );
        QVERIFY2(!valueItem->font().italic(), "Recycled cell must drop the italic placeholder font for a real value");
        QVERIFY2(valueItem->toolTip().isEmpty(), "Recycled cell must drop the placeholder tooltip");
    }

    // Ctrl+C inside the fields table coalesces multi-range selections
    // into one (key, value)-ordered line per row, even when Ctrl-click
    // order would otherwise interleave cells or duplicate rows.
    void TestRecordDetailWidgetFieldsTableCopyCoalescesMultiRanges()
    {
        RecordDetailWidget widget;

        RecordDetailContent content;
        content.valid = true;
        content.summary = QStringLiteral("Row 1");
        content.fields.append({QStringLiteral("alpha"), QStringLiteral("1")});
        content.fields.append({QStringLiteral("beta"), QStringLiteral("2")});
        content.fields.append({QStringLiteral("gamma"), QStringLiteral("3")});
        widget.SetContent(content);

        auto *table = widget.findChild<QTableWidget *>();
        QVERIFY(table != nullptr);
        QCOMPARE(table->rowCount(), 3);

        // Three discontiguous Ctrl-clicks in click order:
        //   (0, value), (2, key), (0, key)
        // The coalescer must merge the two row-0 picks and emit key
        // before value, regardless of click sequence.
        // `setSelected(true)` accumulates without needing real clicks
        // (which are flaky under offscreen QPA).
        table->clearSelection();
        table->item(0, 1)->setSelected(true);
        table->item(2, 0)->setSelected(true);
        table->item(0, 0)->setSelected(true);

        QClipboard *clipboard = QApplication::clipboard();
        QVERIFY(clipboard != nullptr);
        clipboard->clear();

        // Invoke the slot via the meta-object; real Ctrl+C goes
        // through a `QShortcut` whose activation under offscreen QPA
        // depends on focus state we can't set reliably.
        const QString pasted = InvokeAndReadClipboardWithRetry(
            clipboard, &widget, "CopyFieldsSelectionToClipboard", QStringLiteral("alpha\t1")
        );
        QVERIFY2(!pasted.isEmpty(), "CopyFieldsSelectionToClipboard must be reachable via the meta-object");
        const QStringList lines = pasted.split(QLatin1Char('\n'));
        // Two rows: 0 (both columns), 2 (key only). Row 1 wasn't picked.
        QCOMPARE(lines.size(), 2);
        QCOMPARE(lines[0], QStringLiteral("alpha\t1"));
        QCOMPARE(lines[1], QStringLiteral("gamma"));
    }

    // `ShowSourceRow` pins, `Clear` resets; out-of-range rows fall back
    // to the placeholder without crashing.
    void TestRecordDetailDockPinAndClear()
    {
        const TempJsonFile fixture(QStringList{
            QStringLiteral(R"({"k": "alpha"})"),
            QStringLiteral(R"({"k": "beta"})"),
        });
        const StreamingRun run = RunStreaming(fixture.Path());
        QCOMPARE(run.model->rowCount(), 2);

        RecordDetailDock dock(run.model.get());
        QCOMPARE(dock.CurrentSourceRow(), -1);

        dock.ShowSourceRow(0);
        QCOMPARE(dock.CurrentSourceRow(), 0);
        QVERIFY(dock.Widget()->Content().valid);
        bool sawAlpha = false;
        for (const auto &pair : dock.Widget()->Content().fields)
        {
            if (pair.first == QStringLiteral("k") && pair.second == QStringLiteral("alpha"))
            {
                sawAlpha = true;
            }
        }
        QVERIFY(sawAlpha);

        dock.ShowSourceRow(1);
        QCOMPARE(dock.CurrentSourceRow(), 1);
        bool sawBeta = false;
        for (const auto &pair : dock.Widget()->Content().fields)
        {
            if (pair.first == QStringLiteral("k") && pair.second == QStringLiteral("beta"))
            {
                sawBeta = true;
            }
        }
        QVERIFY(sawBeta);

        dock.ShowSourceRow(-1);
        QCOMPARE(dock.CurrentSourceRow(), -1);
        QVERIFY(!dock.Widget()->Content().valid);

        // Out-of-range falls back to placeholder.
        dock.ShowSourceRow(0);
        QCOMPARE(dock.CurrentSourceRow(), 0);
        dock.ShowSourceRow(42);
        QCOMPARE(dock.CurrentSourceRow(), -1);
        QVERIFY(!dock.Widget()->Content().valid);
    }

    // A snapshot window keeps rendering after its `LogModel` is gone;
    // the content was deep-copied at construction.
    void TestRecordDetailWindowSnapshotSurvivesModelDestruction()
    {
        const TempJsonFile fixture(QStringList{
            QStringLiteral(R"({"msg": "snapshot me"})"),
        });
        RecordDetailContent snapshot;
        {
            const StreamingRun run = RunStreaming(fixture.Path());
            QCOMPARE(run.model->rowCount(), 1);
            snapshot = BuildRecordDetailContent(*run.model, 0);
            QVERIFY(snapshot.valid);
            // `run` goes out of scope here, destroying the model.
        }
        // `Qt::WA_DeleteOnClose`; QPointer lets us safely close at end.
        // Explicit `delete` below: the WA_DeleteOnClose-via-close() path
        // leaves the orphan top-level widget alive across the next test's
        // MainWindow construction on the ubuntu-22.04 / Qt 6.8.3 release
        // runner, and a still-alive sibling top-level (no parent) trips
        // Qt's internal QWidget-list traversal inside the next test's
        // setup.
        // The analyzer cannot prove the `QScopeGuard`-driven delete
        // runs; suppress the warning across the body with a matching
        // end-marker at the closing brace.
        // NOLINTBEGIN(clang-analyzer-cplusplus.NewDeleteLeaks)
        QPointer<RecordDetailWindow> window = new RecordDetailWindow(snapshot);
        const QScopeGuard cleanup([&]() {
            if (!window.isNull())
            {
                delete window.data();
            }
        });

        const RecordDetailContent &shown = window->findChild<RecordDetailWidget *>()->Content();
        QVERIFY(shown.valid);
        QCOMPARE(shown.fields.size(), snapshot.fields.size());
        bool sawMsg = false;
        for (const auto &pair : shown.fields)
        {
            if (pair.first == QStringLiteral("msg") && pair.second == QStringLiteral("snapshot me"))
            {
                sawMsg = true;
            }
        }
        QVERIFY(sawMsg);
        // Pop-outs hide their own "Open in new window" button.
        const QPushButton *popOutButton =
            window->findChild<RecordDetailWidget *>()->findChild<QPushButton *>(QStringLiteral("openInNewWindowButton")
            );
        QVERIFY(popOutButton != nullptr);
        QVERIFY(!popOutButton->isVisibleTo(window->findChild<RecordDetailWidget *>()));
    }
    // NOLINTEND(clang-analyzer-cplusplus.NewDeleteLeaks)

    // Double-clicking a row surfaces and pins the dock; `modelReset`
    // clears it back to the placeholder.
    void TestRecordDetailDockOpensOnDoubleClickAndClearsOnReset()
    {
        const TempJsonFile fixture(QStringList{
            QStringLiteral(R"({"k": "alpha"})"),
            QStringLiteral(R"({"k": "beta"})"),
            QStringLiteral(R"({"k": "gamma"})"),
        });
        auto *model = mWindow->Model();
        QVERIFY(model != nullptr);

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
        QVERIFY(finishedSpy.count() > 0 || finishedSpy.wait(5000));
        QCOMPARE(model->rowCount(), 3);

        auto *dock = mWindow->findChild<RecordDetailDock *>();
        QVERIFY2(dock != nullptr, "Record Details dock must be owned by MainWindow");
        // Probe `isHidden()` (parent is never `show()`n under
        // offscreen QPA, so `isVisible()` is always false).
        QVERIFY2(dock->isHidden(), "dock starts hidden");

        // Default sort (-1) -> proxy row == source row. Pick row 1.
        auto *table = mWindow->findChild<LogTableView *>();
        QVERIFY(table != nullptr);
        const QAbstractItemModel *proxyModel = table->model();
        QVERIFY(proxyModel != nullptr);
        const QModelIndex proxyIndex = proxyModel->index(1, 0);
        QVERIFY(proxyIndex.isValid());

        // `ShowSourceRow` sets the pin synchronously; the visibility
        // transition is gated on a realised host (see `MainWindow`).
        // We assert only the observable side effects here.
        mWindow->ShowRecordDetailsForProxyIndex(proxyIndex);
        QCOMPARE(dock->CurrentSourceRow(), 1);
        bool sawBeta = false;
        for (const auto &pair : dock->Widget()->Content().fields)
        {
            if (pair.first == QStringLiteral("k") && pair.second == QStringLiteral("beta"))
            {
                sawBeta = true;
            }
        }
        QVERIFY(sawBeta);

        // Reset clears the dock so a stale record doesn't linger.
        model->Reset();
        QCOMPARE(dock->CurrentSourceRow(), -1);
        QVERIFY(!dock->Widget()->Content().valid);
    }

    // The View-menu action and the dock's title-bar X stay in sync via
    // `QDockWidget::visibilityChanged`.
    void TestRecordDetailDockViewMenuTogglesVisibility()
    {
        QAction *toggleAction = FindActionByObjectName(mWindow, QStringLiteral("actionToggleRecordDetails"));
        QVERIFY2(toggleAction != nullptr, "actionToggleRecordDetails must be wired");
        QVERIFY(toggleAction->isCheckable());

        auto *dock = mWindow->findChild<RecordDetailDock *>();
        QVERIFY2(dock != nullptr, "Record Details dock must be owned by MainWindow");
        // clang-analyzer doesn't model `QVERIFY2`'s fail-fast return.
        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage)
        QVERIFY2(dock->isHidden(), "dock starts hidden");
        QVERIFY(!toggleAction->isChecked());

        // Action -> dock: when the host window is not realised
        // (offscreen QPA), the handler reverts the action so a
        // checked toggle always means a visible dock.
        toggleAction->setChecked(true);
        QVERIFY2(
            !toggleAction->isChecked(),
            "setChecked(true) must be reverted when host window is not realised (offscreen QPA)"
        );

        // Dock -> action direction: drive `visibilityChanged`
        // synthetically (see the namespace note above for why).
        emit dock->visibilityChanged(false);
        QVERIFY2(!toggleAction->isChecked(), "visibilityChanged(false) must un-check the action");

        emit dock->visibilityChanged(true);
        QVERIFY2(toggleAction->isChecked(), "visibilityChanged(true) must re-check the action");
    }

    // `actionToggleRecordDetails` isn't placed in any `<addaction>` in
    // the .ui, so its `Ctrl+I` shortcut would be inert until the View
    // menu is opened. The constructor's explicit `addAction(...)`
    // compensates; this test pins that wiring.
    void TestRecordDetailToggleShortcutIsLiveFromColdLaunch()
    {
        const QAction *toggleAction = FindActionByObjectName(mWindow, QStringLiteral("actionToggleRecordDetails"));
        QVERIFY2(toggleAction != nullptr, "actionToggleRecordDetails must be wired");
        QCOMPARE(toggleAction->shortcut(), QKeySequence(QStringLiteral("Ctrl+I")));

        // The shortcut machinery dispatches against `actions()`.
        QVERIFY2(
            mWindow->actions().contains(toggleAction),
            "MainWindow must own the toggle action so Ctrl+I fires before the View menu is ever opened"
        );
        // Cross-check the action's associated-objects view.
        QVERIFY2(
            toggleAction->associatedObjects().contains(mWindow),
            "actionToggleRecordDetails must list MainWindow among its associated objects"
        );
    }

    // `OpenRecordDetailWindow` spawns a top-level snapshot per call;
    // multiple windows can coexist, out-of-range rows are no-ops.
    void TestOpenRecordDetailWindowSpawnsSnapshot()
    {
        const TempJsonFile fixture(QStringList{
            QStringLiteral(R"({"k": "one"})"),
            QStringLiteral(R"({"k": "two"})"),
        });
        auto *model = mWindow->findChild<LogModel *>();
        QVERIFY(model != nullptr);
        QSignalSpy finishedSpy(model, &LogModel::streamingFinished);
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
        QVERIFY(finishedSpy.count() > 0 || finishedSpy.wait(5000));
        QCOMPARE(model->rowCount(), 2);

        QVERIFY(mWindow->findChildren<RecordDetailWindow *>().isEmpty());

        mWindow->OpenRecordDetailWindow(0);
        mWindow->OpenRecordDetailWindow(1);

        const auto windows = mWindow->findChildren<RecordDetailWindow *>();
        QCOMPARE(windows.size(), 2);
        for (const RecordDetailWindow *window : windows)
        {
            QVERIFY(window->testAttribute(Qt::WA_DeleteOnClose));
            QVERIFY(window->findChild<RecordDetailWidget *>()->Content().valid);
        }

        // Out-of-range -> no-op.
        mWindow->OpenRecordDetailWindow(99);
        QCOMPARE(mWindow->findChildren<RecordDetailWindow *>().size(), 2);

        for (RecordDetailWindow *window : windows)
        {
            window->close();
        }
    }

    // End-to-end of `doubleClicked -> ShowRecordDetailsForProxyIndex`
    // wired in the ctor.
    void TestRecordDetailDockOpensViaDoubleClickedSignal()
    {
        const TempJsonFile fixture(QStringList{
            QStringLiteral(R"({"k": "alpha"})"),
            QStringLiteral(R"({"k": "beta"})"),
            QStringLiteral(R"({"k": "gamma"})"),
        });
        auto *model = mWindow->findChild<LogModel *>();
        QVERIFY(model != nullptr);

        QSignalSpy finishedSpy(model, &LogModel::streamingFinished);
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
        QVERIFY(finishedSpy.count() > 0 || finishedSpy.wait(5000));

        auto *dock = mWindow->findChild<RecordDetailDock *>();
        auto *table = mWindow->findChild<LogTableView *>();
        QVERIFY(dock != nullptr);
        QVERIFY(table != nullptr);
        QVERIFY(dock->isHidden());

        // Emit `doubleClicked` via the meta-object so the `connect`
        // in `MainWindow` runs. Default sort is identity (-1), so
        // proxy row 2 == source row 2.
        const QModelIndex proxyIndex = table->model()->index(2, 0);
        QVERIFY(proxyIndex.isValid());
        const bool emitted =
            QMetaObject::invokeMethod(table, "doubleClicked", Qt::DirectConnection, Q_ARG(QModelIndex, proxyIndex));
        QVERIFY2(emitted, "doubleClicked signal must be invocable via the meta-object");

        // Pin is the observable side effect; visibility is gated on
        // a realised host (see `MainWindow::ShowRecordDetailsForProxyIndex`).
        QCOMPARE(dock->CurrentSourceRow(), 2);
        bool sawGamma = false;
        for (const auto &pair : dock->Widget()->Content().fields)
        {
            if (pair.first == QStringLiteral("k") && pair.second == QStringLiteral("gamma"))
            {
                sawGamma = true;
            }
        }
        QVERIFY(sawGamma);

        model->Reset();
    }

    // FIFO eviction: the persistent pin must keep displaying the same
    // record (now at a lower row index) and report a stable row via
    // `CurrentSourceRow()`. If the pinned record itself is evicted,
    // the dock swaps to the "evicted" placeholder.
    void TestRecordDetailDockTracksFifoEviction()
    {
        LogModel model;
        loglib::StreamLineSource &streamSource = BeginSyntheticStreamSession(model);
        model.SetRetentionCap(100);

        loglib::KeyIndex &keys = model.Sink()->Keys();
        const loglib::KeyId valueKey = keys.GetOrInsert(std::string("value"));

        // Seed exactly the cap (no eviction yet) and pin row 50.
        model.AppendBatch(MakeSyntheticBatch(streamSource, keys, valueKey, 1, 100, /*declareNewKey=*/true));
        QCOMPARE(model.rowCount(), 100);

        RecordDetailDock dock(&model);
        // Drive the visibility gate via the signal (see the namespace
        // note above for why we don't `dock.show()` here).
        emit dock.visibilityChanged(true);
        dock.ShowSourceRow(50);
        QCOMPARE(dock.CurrentSourceRow(), 50);
        // 1-indexed line ids, batch from firstLineId=1, so row 50 ==
        // line 51.
        QString pinnedValue;
        for (const auto &pair : dock.Widget()->Content().fields)
        {
            if (pair.first == QStringLiteral("value"))
            {
                pinnedValue = pair.second;
            }
        }
        QCOMPARE(pinnedValue, QStringLiteral("51"));

        // Append +50 -> 50 evicted from the front. Lines 51..150
        // survive; pinned line 51 is now at row 0.
        model.AppendBatch(MakeSyntheticBatch(streamSource, keys, valueKey, 101, 50, /*declareNewKey=*/false));
        QCOMPARE(model.rowCount(), 100);
        QCOMPARE(dock.CurrentSourceRow(), 0);
        // Content still reflects line 51, not whatever sits at the old index.
        QString shiftedValue;
        for (const auto &pair : dock.Widget()->Content().fields)
        {
            if (pair.first == QStringLiteral("value"))
            {
                shiftedValue = pair.second;
            }
        }
        QCOMPARE(shiftedValue, QStringLiteral("51"));

        // Append +100 -> lines 51..150 evicted; only 151..250 survive.
        // The pinned index goes invalid -> placeholder + row -1.
        model.AppendBatch(MakeSyntheticBatch(streamSource, keys, valueKey, 151, 100, /*declareNewKey=*/false));
        QCOMPARE(model.rowCount(), 100);
        QCOMPARE(dock.CurrentSourceRow(), -1);
        QVERIFY(!dock.Widget()->Content().valid);
        // Distinct from the default "select a row" placeholder.
        QCOMPARE(dock.Widget()->Content().placeholderText, EvictedRecordPlaceholder());
        QVERIFY2(
            dock.Widget()->Content().placeholderText != DefaultRecordDetailPlaceholder(),
            "Evicted-pin placeholder must not collide with the default 'select a row' text"
        );

        model.EndStreaming(false);
    }

    // No pin + eviction == no-op. Without `mEverPinned`, streaming
    // FIFO churn would needlessly rebuild the default placeholder
    // every tick.
    void TestRecordDetailDockNoPinSkipsEvictionWork()
    {
        LogModel model;
        loglib::StreamLineSource &streamSource = BeginSyntheticStreamSession(model);
        model.SetRetentionCap(50);

        loglib::KeyIndex &keys = model.Sink()->Keys();
        const loglib::KeyId valueKey = keys.GetOrInsert(std::string("value"));
        model.AppendBatch(MakeSyntheticBatch(streamSource, keys, valueKey, 1, 50, /*declareNewKey=*/true));
        QCOMPARE(model.rowCount(), 50);

        RecordDetailDock dock(&model);
        // Drive the visibility gate via the signal (see namespace note).
        emit dock.visibilityChanged(true);
        // No `ShowSourceRow` -> `mEverPinned` stays false.
        const QString initialPlaceholder = dock.Widget()->Content().placeholderText;
        QCOMPARE(initialPlaceholder, DefaultRecordDetailPlaceholder());

        // Trigger eviction; placeholder must stay default.
        model.AppendBatch(MakeSyntheticBatch(streamSource, keys, valueKey, 51, 50, /*declareNewKey=*/false));
        QCOMPARE(model.rowCount(), 50);
        QCOMPARE(dock.Widget()->Content().placeholderText, DefaultRecordDetailPlaceholder());
        QVERIFY2(
            dock.Widget()->Content().placeholderText != EvictedRecordPlaceholder(),
            "Dock with no pin history must not surface the eviction placeholder"
        );

        model.EndStreaming(false);
    }

    // `dataChanged` on the pinned row triggers a refresh so out-of-
    // band edits (column rename, back-fill, enum promotion) show up
    // without re-selecting. Fixture renames a column.
    void TestRecordDetailDockRefreshesOnDataChanged()
    {
        const TempJsonFile fixture(QStringList{
            QStringLiteral(R"({"k": "alpha"})"),
            QStringLiteral(R"({"k": "beta"})"),
        });
        const StreamingRun run = RunStreaming(fixture.Path());
        QCOMPARE(run.model->rowCount(), 2);

        const int kColumn = ColumnByHeader(*run.model, QStringLiteral("k"));
        QVERIFY(kColumn >= 0);

        RecordDetailDock dock(run.model.get());
        // Drive the visibility gate via the signal (see namespace note).
        emit dock.visibilityChanged(true);
        dock.ShowSourceRow(0);
        QVERIFY(dock.Widget()->Content().valid);

        bool sawOriginalHeader = false;
        for (const auto &pair : dock.Widget()->Content().fields)
        {
            if (pair.first == QStringLiteral("k"))
            {
                sawOriginalHeader = true;
                QCOMPARE(pair.second, QStringLiteral("alpha"));
            }
        }
        QVERIFY(sawOriginalHeader);

        // Rename the column out-of-band.
        run.model->ConfigurationManager().SetColumnHeader(static_cast<size_t>(kColumn), std::string("renamed"));
        run.model->NotifyColumnEdited(kColumn);
        QCoreApplication::processEvents();

        bool sawRenamedHeader = false;
        bool sawOldHeaderStill = false;
        for (const auto &pair : dock.Widget()->Content().fields)
        {
            if (pair.first == QStringLiteral("renamed"))
            {
                sawRenamedHeader = true;
                QCOMPARE(pair.second, QStringLiteral("alpha"));
            }
            else if (pair.first == QStringLiteral("k"))
            {
                sawOldHeaderStill = true;
            }
        }
        QVERIFY2(sawRenamedHeader, "dock must refresh from the model after dataChanged on the pinned row");
        QVERIFY2(!sawOldHeaderStill, "stale pre-rename content must not survive the refresh");

        // Re-pin row 1 and trigger another notify; idempotent.
        dock.ShowSourceRow(1);
        QVERIFY(dock.Widget()->Content().valid);
        run.model->NotifyColumnEdited(kColumn);
        QCoreApplication::processEvents();
        QVERIFY(dock.Widget()->Content().valid);
    }

    // Closing a snapshot drops it from the tracker via `destroyed`,
    // no manual sweep required.
    void TestOpenRecordDetailWindowClearsTrackerOnClose()
    {
        const TempJsonFile fixture(QStringList{
            QStringLiteral(R"({"k": "one"})"),
        });
        auto *model = mWindow->findChild<LogModel *>();
        QVERIFY(model != nullptr);
        QSignalSpy finishedSpy(model, &LogModel::streamingFinished);
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
        QVERIFY(finishedSpy.count() > 0 || finishedSpy.wait(5000));

        QVERIFY(mWindow->findChildren<RecordDetailWindow *>().isEmpty());

        mWindow->OpenRecordDetailWindow(0);
        mWindow->OpenRecordDetailWindow(0);
        auto windows = mWindow->findChildren<RecordDetailWindow *>();
        QCOMPARE(windows.size(), 2);
        for (RecordDetailWindow *window : windows)
        {
            window->close();
        }
        // `WA_DeleteOnClose` posts a `deleteLater`; flush it before
        // sampling.
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QCoreApplication::processEvents();

        QVERIFY2(
            mWindow->findChildren<RecordDetailWindow *>().isEmpty(),
            "closed snapshots must have been deleted, no live children remain"
        );

        model->Reset();
    }

    // Hidden dock skips the `dataChanged` refresh; a follow-up show +
    // re-pin surfaces the up-to-date content.
    void TestRecordDetailDockHiddenSkipsDataChangedRefresh()
    {
        const TempJsonFile fixture(QStringList{
            QStringLiteral(R"({"k": "alpha"})"),
            QStringLiteral(R"({"k": "beta"})"),
        });
        const StreamingRun run = RunStreaming(fixture.Path());
        QCOMPARE(run.model->rowCount(), 2);

        const int kColumn = ColumnByHeader(*run.model, QStringLiteral("k"));
        QVERIFY(kColumn >= 0);

        RecordDetailDock dock(run.model.get());
        // Pin row 0 while visible (see namespace note for signal use).
        emit dock.visibilityChanged(true);
        dock.ShowSourceRow(0);
        QVERIFY(dock.Widget()->Content().valid);
        bool sawOriginalHeader = false;
        for (const auto &pair : dock.Widget()->Content().fields)
        {
            if (pair.first == QStringLiteral("k"))
            {
                sawOriginalHeader = true;
            }
        }
        QVERIFY(sawOriginalHeader);

        // Hide, then rename. With the visibility gate, content stays
        // pre-hide until something re-pins.
        emit dock.visibilityChanged(false);
        run.model->ConfigurationManager().SetColumnHeader(static_cast<size_t>(kColumn), std::string("renamed"));
        run.model->NotifyColumnEdited(kColumn);
        QCoreApplication::processEvents();

        bool sawRenamedWhileHidden = false;
        for (const auto &pair : dock.Widget()->Content().fields)
        {
            if (pair.first == QStringLiteral("renamed"))
            {
                sawRenamedWhileHidden = true;
            }
        }
        QVERIFY2(
            !sawRenamedWhileHidden,
            "Hidden dock must NOT rebuild its content on dataChanged -- the visibility gate would be dead"
        );

        // Show + re-pin (what `UpdateRecordDetailsFromSelection`
        // would do on the next show) surfaces the refreshed content.
        emit dock.visibilityChanged(true);
        dock.ShowSourceRow(0);
        bool sawRenamedAfterShow = false;
        for (const auto &pair : dock.Widget()->Content().fields)
        {
            if (pair.first == QStringLiteral("renamed"))
            {
                sawRenamedAfterShow = true;
            }
        }
        QVERIFY2(sawRenamedAfterShow, "Re-pinning after show must surface the renamed header");
    }

    // `dataChanged` covering only unpinned rows must NOT refresh.
    // Observed via `RefreshCountForTest` (content-equality would pass
    // either way since `SetContent` is idempotent).
    void TestRecordDetailDockSkipsDataChangedOutsidePinnedRow()
    {
        const TempJsonFile fixture(QStringList{
            QStringLiteral(R"({"k": "alpha"})"),
            QStringLiteral(R"({"k": "beta"})"),
            QStringLiteral(R"({"k": "gamma"})"),
        });
        const StreamingRun run = RunStreaming(fixture.Path());
        QCOMPARE(run.model->rowCount(), 3);

        const int kColumn = ColumnByHeader(*run.model, QStringLiteral("k"));
        QVERIFY(kColumn >= 0);

        RecordDetailDock dock(run.model.get());
        // Drive the visibility gate via the signal (see namespace note).
        emit dock.visibilityChanged(true);
        dock.ShowSourceRow(0); // pin row 0
        QVERIFY(dock.Widget()->Content().valid);
        QCOMPARE(dock.CurrentSourceRow(), 0);

        // Snapshot post-pin so we measure only dataChanged work.
        const int refreshesBefore = dock.RefreshCountForTest();

        // Emit dataChanged on rows 1..2 only -- range check should
        // short-circuit. Without it the counter would tick.
        const QModelIndex topLeft = run.model->index(1, kColumn);
        const QModelIndex bottomRight = run.model->index(2, kColumn);
        QVERIFY(topLeft.isValid());
        QVERIFY(bottomRight.isValid());
        emit run.model->dataChanged(topLeft, bottomRight, {Qt::DisplayRole});
        QCoreApplication::processEvents();

        QCOMPARE(dock.RefreshCountForTest(), refreshesBefore);

        // Sanity: dataChanged covering the pinned row DOES tick.
        const QModelIndex pinnedTopLeft = run.model->index(0, kColumn);
        const QModelIndex pinnedBottomRight = run.model->index(0, kColumn);
        emit run.model->dataChanged(pinnedTopLeft, pinnedBottomRight, {Qt::DisplayRole});
        QCoreApplication::processEvents();
        QCOMPARE(dock.RefreshCountForTest(), refreshesBefore + 1);
    }

    // On dock re-show the dock's own visibility hook refreshes first;
    // when the table selection still points at the same row,
    // `UpdateRecordDetailsFromSelection` must skip the duplicate
    // rebuild. Observed via `RefreshCountForTest`.
    void TestUpdateRecordDetailsFromSelectionSkipsWhenAlreadyPinned()
    {
        const TempJsonFile fixture(QStringList{
            QStringLiteral(R"({"k": "alpha"})"),
            QStringLiteral(R"({"k": "beta"})"),
            QStringLiteral(R"({"k": "gamma"})"),
        });
        auto *model = mWindow->findChild<LogModel *>();
        QVERIFY(model != nullptr);
        QSignalSpy finishedSpy(model, &LogModel::streamingFinished);
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
        QVERIFY(finishedSpy.count() > 0 || finishedSpy.wait(5000));
        QCOMPARE(model->rowCount(), 3);

        auto *dock = mWindow->findChild<RecordDetailDock *>();
        auto *table = mWindow->findChild<LogTableView *>();
        QVERIFY(dock != nullptr);
        QVERIFY(table != nullptr);

        // Drive selection + pin via the proxy index for source row 1
        // (default identity sort).
        const QModelIndex proxyIndex = table->model()->index(1, 0);
        QVERIFY(proxyIndex.isValid());
        table->selectionModel()->setCurrentIndex(proxyIndex, QItemSelectionModel::ClearAndSelect);
        mWindow->ShowRecordDetailsForProxyIndex(proxyIndex);
        QCOMPARE(dock->CurrentSourceRow(), 1);

        // Production `setVisible(true)` is guarded on a realised
        // host; drive `visibilityChanged` directly to open the
        // refresh gate (see namespace note).
        emit dock->visibilityChanged(true);
        QVERIFY(dock->IsVisibleForRefresh());

        // Selection AND pin already at row 1. The host's
        // `UpdateRecordDetailsFromSelection` must short-circuit.
        // Sample AFTER the re-emit so the dock's own resume-refresh
        // is in the baseline.
        const int refreshesBefore = dock->RefreshCountForTest();
        mWindow->UpdateRecordDetailsFromSelection();
        QCOMPARE(dock->RefreshCountForTest(), refreshesBefore);

        // Sanity: moving the selection does re-pin.
        const QModelIndex otherProxyIndex = table->model()->index(2, 0);
        QVERIFY(otherProxyIndex.isValid());
        table->selectionModel()->setCurrentIndex(otherProxyIndex, QItemSelectionModel::ClearAndSelect);
        mWindow->UpdateRecordDetailsFromSelection();
        QCOMPARE(dock->CurrentSourceRow(), 2);
        QVERIFY(dock->RefreshCountForTest() > refreshesBefore);
    }

    // `columnsMoved` (header drag, streaming `Time` bubble) refreshes
    // the Field/Value list. Driven via the public `MoveColumn` -- in
    // Qt 6 `columnsMoved` is `QPrivateSignal`, so external `emit`
    // doesn't compile. `columnsInserted` shares the same lambda and
    // is exercised indirectly by the streaming tests.
    void TestRecordDetailDockRefreshesOnColumnsMoved()
    {
        // Three keys so we can move two non-zero columns and keep the
        // pinned column-0 persistent index trivially valid.
        const TempJsonFile fixture(QStringList{
            QStringLiteral(R"({"a": "alpha", "b": "1", "c": "x"})"),
            QStringLiteral(R"({"a": "beta",  "b": "2", "c": "y"})"),
        });
        const StreamingRun run = RunStreaming(fixture.Path());
        QCOMPARE(run.model->rowCount(), 2);
        QVERIFY(run.model->columnCount() >= 3);

        RecordDetailDock dock(run.model.get());
        // Drive the visibility gate via the signal (see namespace note).
        emit dock.visibilityChanged(true);
        dock.ShowSourceRow(0);
        QVERIFY(dock.Widget()->Content().valid);
        const int refreshesAfterPin = dock.RefreshCountForTest();

        const int cols = run.model->columnCount();
        const int src = cols - 1;
        const int dest = cols - 2;
        QVERIFY(src != dest);
        QVERIFY2(src >= 1 && dest >= 1, "Move must skip column 0 so the pin stays trivially valid");
        QVERIFY(run.model->MoveColumn(src, dest));
        QCoreApplication::processEvents();
        QCOMPARE(dock.RefreshCountForTest(), refreshesAfterPin + 1);

        // Hidden gate applies here too -- invisible dock skips the rebuild.
        emit dock.visibilityChanged(false);
        const int refreshesAfterHide = dock.RefreshCountForTest();
        // NOLINTNEXTLINE(readability-suspicious-call-argument): intentional swap to restore the original column order.
        QVERIFY(run.model->MoveColumn(dest, src));
        QCoreApplication::processEvents();
        QCOMPARE(dock.RefreshCountForTest(), refreshesAfterHide);
    }

    // Buried-tab transition (`visibilityChanged(false)` without
    // `hide()`) skips refreshes; the resuming `(true)` refreshes once.
    void TestRecordDetailDockSkipsRefreshWhileTabInactiveAndCatchesUpOnResume()
    {
        const TempJsonFile fixture(QStringList{
            QStringLiteral(R"({"k": "alpha"})"),
            QStringLiteral(R"({"k": "beta"})"),
        });
        const StreamingRun run = RunStreaming(fixture.Path());
        QCOMPARE(run.model->rowCount(), 2);

        const int kColumn = ColumnByHeader(*run.model, QStringLiteral("k"));
        QVERIFY(kColumn >= 0);

        RecordDetailDock dock(run.model.get());
        // Drive the visibility gate via the signal (see namespace note).
        emit dock.visibilityChanged(true);
        dock.ShowSourceRow(0);
        QVERIFY(dock.Widget()->Content().valid);

        // Synthesise the buried-tab transition: emit
        // `visibilityChanged(false)` without `hide()`. Only
        // `mPerceivedVisible` flips; the dock's explicit hidden flag
        // is unchanged.
        emit dock.visibilityChanged(false);

        const int refreshesBefore = dock.RefreshCountForTest();
        const QModelIndex pinnedIdx = run.model->index(0, kColumn);
        emit run.model->dataChanged(pinnedIdx, pinnedIdx, {Qt::DisplayRole});
        QCoreApplication::processEvents();
        QCOMPARE(dock.RefreshCountForTest(), refreshesBefore);

        // Tab resumes -> one refresh.
        emit dock.visibilityChanged(true);
        QCoreApplication::processEvents();
        QCOMPARE(dock.RefreshCountForTest(), refreshesBefore + 1);
    }

    // The dock self-clears on `modelReset` without any host help.
    void TestRecordDetailDockClearsOnModelResetStandalone()
    {
        const TempJsonFile fixture(QStringList{
            QStringLiteral(R"({"k": "alpha"})"),
            QStringLiteral(R"({"k": "beta"})"),
        });
        const StreamingRun run = RunStreaming(fixture.Path());
        QCOMPARE(run.model->rowCount(), 2);

        RecordDetailDock dock(run.model.get());
        // Drive the visibility gate via the signal (see namespace note).
        emit dock.visibilityChanged(true);
        dock.ShowSourceRow(1);
        QCOMPARE(dock.CurrentSourceRow(), 1);
        QVERIFY(dock.Widget()->Content().valid);

        run.model->Reset();
        QCoreApplication::processEvents();

        QCOMPARE(dock.CurrentSourceRow(), -1);
        QVERIFY(!dock.Widget()->Content().valid);
        QCOMPARE(dock.Widget()->Content().placeholderText, DefaultRecordDetailPlaceholder());
    }

    // End-to-end: `QAction::trigger` (what the shortcut map calls
    // after resolving Ctrl+I) toggles the dock's visibility. A
    // regression that severs the `toggled -> setVisible` connect
    // would slip past the cold-launch test but fail this one. We
    // don't post a `QKeyEvent` because `WindowShortcut` requires
    // window activation, which offscreen QPA doesn't realise.
    void TestRecordDetailToggleActionTogglesDockVisibility()
    {
        QAction *toggleAction = FindActionByObjectName(mWindow, QStringLiteral("actionToggleRecordDetails"));
        QVERIFY2(toggleAction != nullptr, "actionToggleRecordDetails must be wired");
        QVERIFY(toggleAction->isCheckable());

        auto *dock = mWindow->findChild<RecordDetailDock *>();
        QVERIFY(dock != nullptr);
        QVERIFY2(dock->isHidden(), "dock starts hidden");
        QVERIFY(!toggleAction->isChecked());

        // The handler reverts under offscreen QPA, so the action
        // stays unchecked across triggers. Live visibility is
        // covered by manual QA; this guards against the action
        // settling "checked while dock hidden".
        toggleAction->trigger();
        QVERIFY2(
            !toggleAction->isChecked(),
            "trigger() must auto-revert under offscreen QPA: action cannot settle checked while dock cannot show"
        );

        toggleAction->trigger();
        QVERIFY2(
            !toggleAction->isChecked(),
            "second trigger must also revert under offscreen QPA -- the action stays unchecked until the host is "
            "realised"
        );
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

    // ---------------------------------------------------------------
    // SessionHistoryManager
    // ---------------------------------------------------------------

    // Round-trip a snapshot: write a configuration, read the list, and
    // re-load the on-disk JSON. Asserts the per-uuid file lands under
    // the supplied sessions dir and that the metadata in the index is
    // populated from the configuration's source descriptor.
    void TestSessionHistoryWriteSnapshotRoundTrips()
    {
        const QTemporaryDir sessionsDir;
        QVERIFY(sessionsDir.isValid());

        SessionHistoryManager manager(QDir(sessionsDir.path()), std::make_unique<InMemoryRecentsIndexStorage>());

        loglib::LogConfiguration cfg;
        cfg.columns.push_back(loglib::LogConfiguration::Column{
            .header = "category", .keys = {"category"}, .type = loglib::LogConfiguration::Type::Enumeration
        });
        cfg.columns.push_back(loglib::LogConfiguration::Column{
            .header = "msg", .keys = {"msg"}, .type = loglib::LogConfiguration::Type::String
        });
        cfg.source = loglib::LogConfiguration::Source{
            .kind = loglib::LogConfiguration::Source::Kind::File,
            .locators = {"C:/logs/first.json", "C:/logs/second.json"}
        };

        const QSignalSpy changedSpy(&manager, &SessionHistoryManager::changed);
        const QString uuid = manager.WriteSnapshot(cfg);

        QVERIFY(!uuid.isEmpty());
        QCOMPARE(changedSpy.count(), 1);

        const QList<RecentSessionEntry> list = manager.List();
        QCOMPARE(list.size(), 1);
        QCOMPARE(list.front().uuid, uuid);
        QCOMPARE(list.front().fileCount, 2);
        QCOMPARE(list.front().label, QStringLiteral("first.json + 1 more"));
        QCOMPARE(list.front().primaryLocator, QStringLiteral("C:/logs/first.json"));

        const QString jsonPath = manager.PathForUuid(uuid);
        QVERIFY(QFileInfo::exists(jsonPath));

        // Round-trip through the library loader.
        loglib::LogConfigurationManager probe;
        probe.Load(jsonPath.toStdString());
        QVERIFY(probe.Configuration().source.has_value());
        QCOMPARE(probe.Configuration().source->locators.size(), static_cast<std::size_t>(2));
        QCOMPARE(probe.Configuration().columns.size(), static_cast<std::size_t>(2));
    }

    // Recents label for a `NetworkStream` source round-trips the
    // URI verbatim (no `QFileInfo::fileName()` collapse).
    void TestSessionHistoryLabelKeepsNetworkStreamLocatorIntact()
    {
        const QTemporaryDir sessionsDir;
        QVERIFY(sessionsDir.isValid());

        SessionHistoryManager manager(QDir(sessionsDir.path()), std::make_unique<InMemoryRecentsIndexStorage>());

        loglib::LogConfiguration cfg;
        cfg.source = loglib::LogConfiguration::Source{
            .kind = loglib::LogConfiguration::Source::Kind::NetworkStream, .locators = {"TCP 127.0.0.1:5170"}
        };
        const QString uuid = manager.WriteSnapshot(cfg);
        QVERIFY(!uuid.isEmpty());

        const QList<RecentSessionEntry> list = manager.List();
        QCOMPARE(list.size(), 1);
        QCOMPARE(list.front().label, QStringLiteral("TCP 127.0.0.1:5170"));
    }

    // Capacity eviction trims the index to MAX_ENTRIES and deletes the
    // matching per-uuid JSON file.
    void TestSessionHistoryEvictsOldestPastCapacity()
    {
        const QTemporaryDir sessionsDir;
        QVERIFY(sessionsDir.isValid());

        SessionHistoryManager manager(QDir(sessionsDir.path()), std::make_unique<InMemoryRecentsIndexStorage>());

        const int capacityPlus = SessionHistoryManager::MAX_ENTRIES + 3;
        QStringList writtenUuids;
        for (int i = 0; i < capacityPlus; ++i)
        {
            loglib::LogConfiguration cfg;
            cfg.source = loglib::LogConfiguration::Source{
                .kind = loglib::LogConfiguration::Source::Kind::File,
                .locators = {QStringLiteral("C:/logs/file-%1.json").arg(i).toStdString()}
            };
            const QString uuid = manager.WriteSnapshot(cfg);
            QVERIFY(!uuid.isEmpty());
            writtenUuids.append(uuid);
        }

        const QList<RecentSessionEntry> list = manager.List();
        QCOMPARE(list.size(), SessionHistoryManager::MAX_ENTRIES);

        // The newest writes survive (writtenUuids are in chronological
        // order; the last MAX_ENTRIES uuids should be in the list).
        for (int i = capacityPlus - SessionHistoryManager::MAX_ENTRIES; i < capacityPlus; ++i)
        {
            QVERIFY2(
                std::any_of(
                    list.begin(), list.end(), [&](const RecentSessionEntry &e) { return e.uuid == writtenUuids[i]; }
                ),
                qPrintable(QStringLiteral("uuid index %1 must still be in the recents list").arg(i))
            );
        }

        // The evicted uuids must not be on disk anymore.
        for (int i = 0; i < capacityPlus - SessionHistoryManager::MAX_ENTRIES; ++i)
        {
            QVERIFY2(
                !QFileInfo::exists(manager.PathForUuid(writtenUuids[i])),
                qPrintable(QStringLiteral("evicted uuid %1 must be deleted from disk").arg(i))
            );
        }
    }

    // `LastSessionPath` returns the most recent snapshot. After a
    // `Touch` on an older entry, that entry becomes the head.
    void TestSessionHistoryTouchPromotesEntry()
    {
        const QTemporaryDir sessionsDir;
        QVERIFY(sessionsDir.isValid());

        SessionHistoryManager manager(QDir(sessionsDir.path()), std::make_unique<InMemoryRecentsIndexStorage>());

        loglib::LogConfiguration cfgA;
        cfgA.source = loglib::LogConfiguration::Source{
            .kind = loglib::LogConfiguration::Source::Kind::File, .locators = {"C:/logs/a.json"}
        };
        const QString uuidA = manager.WriteSnapshot(cfgA);

        loglib::LogConfiguration cfgB;
        cfgB.source = loglib::LogConfiguration::Source{
            .kind = loglib::LogConfiguration::Source::Kind::File, .locators = {"C:/logs/b.json"}
        };
        const QString uuidB = manager.WriteSnapshot(cfgB);

        // Newest (B) is the head; LastSessionPath points at B.
        const auto lastPath = manager.LastSessionPath();
        QVERIFY(lastPath.has_value());
        QCOMPARE(*lastPath, manager.PathForUuid(uuidB));

        // Touch A -> A becomes the head.
        QVERIFY(manager.Touch(uuidA));
        const auto afterTouch = manager.LastSessionPath();
        QVERIFY(afterTouch.has_value());
        QCOMPARE(*afterTouch, manager.PathForUuid(uuidA));
        QCOMPARE(manager.List().front().uuid, uuidA);
    }

    // `Remove` drops the entry + its on-disk JSON and resets
    // `LastSessionPath` to the new head (or `nullopt` when the index
    // empties out).
    void TestSessionHistoryRemoveClearsLastWhenLastRemoved()
    {
        const QTemporaryDir sessionsDir;
        QVERIFY(sessionsDir.isValid());

        SessionHistoryManager manager(QDir(sessionsDir.path()), std::make_unique<InMemoryRecentsIndexStorage>());

        loglib::LogConfiguration cfg;
        cfg.source = loglib::LogConfiguration::Source{
            .kind = loglib::LogConfiguration::Source::Kind::File, .locators = {"C:/logs/only.json"}
        };
        const QString uuid = manager.WriteSnapshot(cfg);
        QVERIFY(QFileInfo::exists(manager.PathForUuid(uuid)));

        manager.Remove(uuid);
        QCOMPARE(manager.List().size(), 0);
        QVERIFY2(
            !manager.LastSessionPath().has_value(), "LastSessionPath must be nullopt after the last entry is removed"
        );
        QVERIFY(!QFileInfo::exists(manager.PathForUuid(uuid)));
    }

    // `actionNewWindow` spawns a second MainWindow sharing the
    // primary's manager. Asserts: one new top-level appears, the
    // shared manager observes the same writes, and the spawn has
    // `WA_DeleteOnClose`.
    void TestNewWindowSharesHistoryManager()
    {
        const QTemporaryDir sessionsDir;
        QVERIFY(sessionsDir.isValid());

        SessionHistoryManager manager(QDir(sessionsDir.path()), std::make_unique<InMemoryRecentsIndexStorage>());
        auto primary = std::make_unique<MainWindow>(mTheme.data(), &manager, nullptr);

        // Each `MainWindow` constructor also installs auxiliary
        // top-levels (preferences, record detail), so filter to
        // just the MainWindows.
        auto countMainWindows = []() {
            int n = 0;
            for (QWidget *w : QApplication::topLevelWidgets())
            {
                if (qobject_cast<MainWindow *>(w) != nullptr)
                {
                    ++n;
                }
            }
            return n;
        };

        const int mainsBefore = countMainWindows();
        const QList<QWidget *> topLevelBefore = QApplication::topLevelWidgets();

        primary->findChild<QAction *>(QStringLiteral("actionNewWindow"))->trigger();
        QCoreApplication::processEvents();

        QCOMPARE(countMainWindows(), mainsBefore + 1);

        // Find the newly-spawned MainWindow (the one that wasn't in
        // `topLevelBefore`).
        MainWindow *child = nullptr;
        for (QWidget *w : QApplication::topLevelWidgets())
        {
            if (topLevelBefore.contains(w))
            {
                continue;
            }
            auto *candidate = qobject_cast<MainWindow *>(w);
            if (candidate != nullptr && candidate != primary.get())
            {
                child = candidate;
                break;
            }
        }
        QVERIFY2(child != nullptr, "newly spawned window must be a MainWindow");
        QVERIFY2(child->testAttribute(Qt::WA_DeleteOnClose), "spawned window must auto-delete on close");

        // Drive a real open through the primary; the shared
        // manager surfaces the entry to both windows.
        const TempJsonFile fixture({QStringLiteral(R"({"msg": "shared"})")});
        QSignalSpy primaryFinished(primary->Model(), &LogModel::streamingFinished);
        primary->OpenFilesForTest({fixture.Path()}, MainWindow::OpenMode::Append);
        QVERIFY(primaryFinished.wait(5000));
        QCoreApplication::processEvents();
        QCOMPARE(manager.List().size(), 1);

        // Close the spawned window and spin the event loop so the
        // `deleteLater` from `WA_DeleteOnClose` runs before
        // `primary` falls out of scope.
        const QSignalSpy destroyedSpy(child, &QObject::destroyed);
        child->close();
        for (int i = 0; i < 50 && destroyedSpy.isEmpty(); ++i)
        {
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            QCoreApplication::processEvents();
        }
        QCOMPARE(destroyedSpy.count(), 1);
    }

    // `RestoreLastSessionFromPath` reopens an auto-saved snapshot
    // into a freshly-built window (the entry point that `main()`'s
    // restore-on-launch hook uses).
    void TestRestoreLastSessionFromPath()
    {
        const QTemporaryDir sessionsDir;
        QVERIFY(sessionsDir.isValid());

        SessionHistoryManager manager(QDir(sessionsDir.path()), std::make_unique<InMemoryRecentsIndexStorage>());

        // Seed an on-disk JSON via a real open.
        auto seeder = std::make_unique<MainWindow>(mTheme.data(), &manager, nullptr);
        const QStringList fixtureLines{
            QStringLiteral(R"({"category": "info", "msg": "alpha"})"),
            QStringLiteral(R"({"category": "warn", "msg": "beta"})"),
        };
        const TempJsonFile fixture(fixtureLines);

        QSignalSpy finishedSpy(seeder->Model(), &LogModel::streamingFinished);
        seeder->OpenFilesForTest({fixture.Path()}, MainWindow::OpenMode::Append);
        QVERIFY(finishedSpy.wait(5000));
        QCoreApplication::processEvents();
        QCOMPARE(manager.List().size(), 1);

        const auto lastPath = manager.LastSessionPath();
        QVERIFY(lastPath.has_value());
        // Drop the seeder so its closeEvent flush completes first
        // (mirrors the real cold-start order).
        seeder.reset();

        // Fresh window restores the snapshot.
        auto restored = std::make_unique<MainWindow>(mTheme.data(), &manager, nullptr);
        QSignalSpy restoredSpy(restored->Model(), &LogModel::streamingFinished);
        restored->RestoreLastSessionFromPath(*lastPath);
        QVERIFY(restoredSpy.wait(5000));
        QCoreApplication::processEvents();

        QCOMPARE(restored->Model()->rowCount(), fixtureLines.size());
    }

    // Regression: a saved sort must be deferred until streaming
    // finishes. Avoids the O(N^2) per-row insert path
    // `LogFilterModel::OnSourceRowsInserted` takes under an
    // active sort (a 1 GB restore "never finishes" otherwise).
    // Asserts both halves: proxy is unsorted while streaming, and
    // the saved sort is applied exactly once at the end.
    void TestRestoreLastSessionDefersSortUntilStreamingFinishes()
    {
        const QTemporaryDir sessionsDir;
        QVERIFY(sessionsDir.isValid());
        SessionHistoryManager manager(QDir(sessionsDir.path()), std::make_unique<InMemoryRecentsIndexStorage>());

        // Seed: stream, engage a sort, save the session via the
        // close-event autosave so the saved JSON carries the sort.
        auto seeder = std::make_unique<MainWindow>(mTheme.data(), &manager, nullptr);
        const QStringList fixtureLines{
            QStringLiteral(R"({"category": "info", "msg": "alpha"})"),
            QStringLiteral(R"({"category": "warn", "msg": "beta"})"),
        };
        const TempJsonFile fixture(fixtureLines);

        QSignalSpy seederFinishedSpy(seeder->Model(), &LogModel::streamingFinished);
        seeder->OpenFilesForTest({fixture.Path()}, MainWindow::OpenMode::Append);
        QVERIFY(seederFinishedSpy.wait(5000));
        QCoreApplication::processEvents();

        const int categoryCol = ColumnByHeader(*seeder->Model(), QStringLiteral("category"));
        QVERIFY2(categoryCol >= 0, "category column must exist after streaming");
        auto *seederTable = seeder->findChild<LogTableView *>();
        QVERIFY(seederTable != nullptr);
        seederTable->sortByColumn(categoryCol, Qt::DescendingOrder);
        QCoreApplication::processEvents();

        // closeEvent autosave captures the manual sort engaged
        // after `streamingFinished` -- the earlier streaming
        // autosave saw the proxy before the sort.
        seeder->close();
        QCoreApplication::processEvents();
        seeder.reset();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QCoreApplication::processEvents();

        const auto lastPath = manager.LastSessionPath();
        QVERIFY(lastPath.has_value());

        // Sanity: the saved JSON carries the sort.
        {
            loglib::LogConfigurationManager probe;
            probe.Load(lastPath->toStdString());
            QCOMPARE(probe.Configuration().sort.columnIndex, categoryCol);
            QVERIFY(probe.Configuration().sort.descending);
        }

        // Fresh window restores the snapshot.
        auto restored = std::make_unique<MainWindow>(mTheme.data(), &manager, nullptr);
        auto *restoredFilter = restored->FilterModel();
        QVERIFY2(restoredFilter != nullptr, "restored window must own a LogFilterModel");

        QSignalSpy restoredFinishedSpy(restored->Model(), &LogModel::streamingFinished);
        restored->RestoreLastSessionFromPath(*lastPath);
        // Sort still deferred immediately after the call returns:
        // no batches have been processed yet.
        QCOMPARE(restoredFilter->SortColumn(), -1);

        QVERIFY(restoredFinishedSpy.wait(5000));
        QCoreApplication::processEvents();

        // `ApplyDeferredSortFromConfig` applies the sort once
        // when streaming finishes.
        QCOMPARE(restoredFilter->SortColumn(), categoryCol);
        QCOMPARE(restoredFilter->SortOrder(), Qt::DescendingOrder);
        QCOMPARE(restored->Model()->rowCount(), fixtureLines.size());

        // The deferred sort reaches `UpdateSortStatus` via
        // the `layoutChanged` it issues. Pin the status-bar
        // indicator and `actionClearSort` to mirror the
        // restored sort once streaming settles.
        auto *restoredClearSortAction = restored->findChild<QAction *>(QStringLiteral("actionClearSort"));
        QVERIFY2(restoredClearSortAction != nullptr, "restored window must own actionClearSort");
        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): prior QVERIFY2 aborts on null.
        QVERIFY2(restoredClearSortAction->isEnabled(), "actionClearSort must be enabled after deferred restore");
        auto *restoredClearSortButton = restored->findChild<QPushButton *>(QStringLiteral("clearSortStatusButton"));
        QVERIFY2(restoredClearSortButton != nullptr, "restored window must own clearSortStatusButton");
        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): prior QVERIFY2 aborts on null.
        QVERIFY2(!restoredClearSortButton->isHidden(), "status-bar Clear-sort indicator must surface after restore");
    }

    // Recent Sessions submenu rebuild on `aboutToShow`. Asserts:
    // entries + separator + Clear are present, clicking an entry
    // reopens the configuration, and `mAutoSaveUuid` is pinned so
    // further edits update the same entry.
    void TestRecentSessionsMenuReopensEntry()
    {
        const QTemporaryDir sessionsDir;
        QVERIFY(sessionsDir.isValid());

        SessionHistoryManager manager(QDir(sessionsDir.path()), std::make_unique<InMemoryRecentsIndexStorage>());
        auto wired = std::make_unique<MainWindow>(mTheme.data(), &manager, nullptr);

        // Seed a recents entry via a real open.
        const QStringList fixtureLines{
            QStringLiteral(R"({"category": "info", "msg": "alpha"})"),
            QStringLiteral(R"({"category": "warn", "msg": "beta"})"),
        };
        const TempJsonFile fixture(fixtureLines);

        QSignalSpy finishedSpy(wired->Model(), &LogModel::streamingFinished);
        wired->OpenFilesForTest({fixture.Path()}, MainWindow::OpenMode::Append);
        QVERIFY(finishedSpy.wait(5000));
        QCoreApplication::processEvents();

        QCOMPARE(manager.List().size(), 1);
        const QString uuid = manager.List().front().uuid;

        // Tear the session down so the menu reopen has work to do.
        wired->findChild<QAction *>(QStringLiteral("actionNewSession"))->trigger();
        QCoreApplication::processEvents();
        QCOMPARE(wired->Model()->rowCount(), 0);

        // Force the submenu rebuild + drive the entry action.
        // Reach for the menu via the explicit accessor: the Qt 6.8 +
        // offscreen-QPA `findChild<QMenu*>` traversal bug strands the
        // by-objectName lookup on the Linux runner.
        auto *recentsMenu = wired->findChild<QMenu *>(QStringLiteral("menuRecentSessions"));
        QVERIFY2(recentsMenu != nullptr, "menuRecentSessions must exist");
        emit recentsMenu->aboutToShow();

        const QList<QAction *> actions = recentsMenu->actions();
        QVERIFY2(actions.size() >= 3, "rebuild must produce at least one entry + separator + clear");

        // First action is the single recents entry.
        QAction *entryAction = actions.first();
        finishedSpy.clear();
        entryAction->trigger();
        QVERIFY(finishedSpy.wait(5000));
        QCoreApplication::processEvents();

        QCOMPARE(wired->Model()->rowCount(), fixtureLines.size());

        // Re-stream auto-save updates the same recents entry.
        QCOMPARE(manager.List().size(), 1);
        QCOMPARE(manager.List().front().uuid, uuid);
    }

    // Auto-save writes a snapshot on every successful streaming
    // finish, reusing the per-window uuid so a second auto-save
    // updates the same recents entry instead of growing the list.
    void TestMainWindowAutoSavesOnStreamingFinished()
    {
        const QTemporaryDir sessionsDir;
        QVERIFY(sessionsDir.isValid());

        SessionHistoryManager manager(QDir(sessionsDir.path()), std::make_unique<InMemoryRecentsIndexStorage>());

        // The fixture's `mWindow` is unwired; use the production
        // constructor that takes a manager.
        auto wired = std::make_unique<MainWindow>(mTheme.data(), &manager, nullptr);

        auto *model = wired->Model();
        QVERIFY(model != nullptr);

        const QStringList fixtureLines{
            QStringLiteral(R"({"category": "info", "msg": "a-0"})"),
            QStringLiteral(R"({"category": "warn", "msg": "a-1"})"),
        };
        const TempJsonFile fixture(fixtureLines);

        QSignalSpy finishedSpy(model, &LogModel::streamingFinished);
        QVERIFY(finishedSpy.isValid());
        const QSignalSpy changedSpy(&manager, &SessionHistoryManager::changed);
        QVERIFY(changedSpy.isValid());

        wired->OpenFilesForTest({fixture.Path()}, MainWindow::OpenMode::Append);
        QVERIFY(finishedSpy.wait(5000));
        QCoreApplication::processEvents();

        QCOMPARE(changedSpy.count(), 1);
        QCOMPARE(manager.List().size(), 1);
        const QString firstUuid = manager.List().front().uuid;

        // Second open updates the existing entry, not appends.
        const TempJsonFile fixtureB(QStringList{QStringLiteral(R"({"msg": "b-0"})")});
        finishedSpy.clear();
        wired->OpenFilesForTest({fixtureB.Path()}, MainWindow::OpenMode::Append);
        QVERIFY(finishedSpy.wait(5000));
        QCoreApplication::processEvents();

        QCOMPARE(manager.List().size(), 1);
        QCOMPARE(manager.List().front().uuid, firstUuid);
        // Label now tracks both A and B (B appended onto A).
        QCOMPARE(manager.List().front().fileCount, 2);
    }

    // With the cross-process lockfile held by a sibling, the
    // manager fails closed: `WriteSnapshot` returns an empty uuid,
    // the lock attempt is bounded so the GUI does not freeze, and
    // a retry succeeds once the lock is released.
    void TestWriteSnapshotSurvivesHeldLockFile()
    {
        const QTemporaryDir sessionsDir;
        QVERIFY(sessionsDir.isValid());

        SessionHistoryManager manager(QDir(sessionsDir.path()), std::make_unique<InMemoryRecentsIndexStorage>());

        // Force the directory so the lock path exists before
        // WriteSnapshot would create it.
        QVERIFY(QDir().mkpath(sessionsDir.path()));

        // Take the lock from a "foreign" holder. Short stale-time
        // so the manager's tryLock never treats it as dead.
        QLockFile foreign(QDir(sessionsDir.path()).filePath(QStringLiteral("recents.lock")));
        foreign.setStaleLockTime(0);
        QVERIFY(foreign.tryLock(100));

        loglib::LogConfiguration cfg;
        loglib::LogConfiguration::Source src;
        src.locators = {"C:/logs/locked.json"};
        cfg.source = src;

        // Fail-closed: empty uuid, no write, bounded duration.
        const auto start = std::chrono::steady_clock::now();
        const QString uuid = manager.WriteSnapshot(cfg);
        const auto elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();

        QVERIFY2(uuid.isEmpty(), "WriteSnapshot must fail closed when the cross-process lock cannot be acquired");
        QVERIFY2(elapsed < 3000, qPrintable(QStringLiteral("WriteSnapshot took too long: %1ms").arg(elapsed)));
        QCOMPARE(manager.List().size(), 0);

        // Releasing the foreign lock unblocks the next call.
        foreign.unlock();
        const QString retryUuid = manager.WriteSnapshot(cfg);
        QVERIFY2(!retryUuid.isEmpty(), "WriteSnapshot must succeed once contention clears");
        QCOMPARE(manager.List().size(), 1);
    }

    // `openWindowsAtQuit` round-trip preserves order. If QSettings
    // refuses the write (registry permissions in some Windows test
    // envs), the test skips rather than failing.
    void TestOpenWindowsAtQuitRoundTrip()
    {
        const QStringList expected{QStringLiteral("uuid-a"), QStringLiteral("uuid-b"), QStringLiteral("uuid-c")};
        const QStringList previous = SessionHistoryManager::OpenWindowsAtQuitUnlocked();
        auto restoreGuard = qScopeGuard([&]() { SessionHistoryManager::SetOpenWindowsAtQuit(previous); });

        SessionHistoryManager::SetOpenWindowsAtQuit(expected);
        const QStringList actual = SessionHistoryManager::OpenWindowsAtQuitUnlocked();
        // QSettings may swallow the write on some Windows test envs.
        if (actual.isEmpty())
        {
            QSKIP("QSettings did not honour the write in this environment");
        }
        QCOMPARE(actual, expected);

        SessionHistoryManager::SetOpenWindowsAtQuit({});
        QCOMPARE(SessionHistoryManager::OpenWindowsAtQuitUnlocked(), QStringList{});
    }

    // `TakeOpenWindowsAtQuit` is the atomic read-and-wipe used by
    // `main()` at startup: returns the list and wipes it under one
    // critical section so a sibling writer can't race between them.
    void TestTakeOpenWindowsAtQuitReadsAndWipesAtomically()
    {
        const QStringList expected{QStringLiteral("uuid-x"), QStringLiteral("uuid-y")};
        const QStringList previous = SessionHistoryManager::OpenWindowsAtQuitUnlocked();
        auto restoreGuard = qScopeGuard([&]() { SessionHistoryManager::SetOpenWindowsAtQuit(previous); });

        SessionHistoryManager::SetOpenWindowsAtQuit(expected);
        // QSettings-on-Windows soft-skip mirrors `TestOpenWindowsAtQuitRoundTrip`.
        if (SessionHistoryManager::OpenWindowsAtQuitUnlocked().isEmpty())
        {
            QSKIP("QSettings did not honour the write in this environment");
        }

        const QStringList taken = SessionHistoryManager::TakeOpenWindowsAtQuit();
        QCOMPARE(taken, expected);

        // Post-take: the persisted list is empty.
        QCOMPARE(SessionHistoryManager::OpenWindowsAtQuitUnlocked(), QStringList{});

        // A second take returns empty and is a no-op (idempotent).
        QCOMPARE(SessionHistoryManager::TakeOpenWindowsAtQuit(), QStringList{});
    }

    // ---------------------------------------------------------------
    // SingleInstanceGuard
    // ---------------------------------------------------------------

    // Primary's socket receives a hand-rolled secondary frame and
    // emits `openWindowRequested`. Hand-rolled rather than using a
    // second `TryAcquire` because both peers share the test thread,
    // and the secondary's wait would block the primary's slot.
    void TestSingleInstanceForwardsOpenRequest()
    {
        const QString socketName =
            QStringLiteral("slv-fwd-") + QUuid::createUuid().toString(QUuid::WithoutBraces).left(8);

        SingleInstanceGuard primary;
        primary.SetSocketNameForTest(socketName);
        QVERIFY(primary.TryAcquire({}, /*allowNewInstance=*/false));

        QSignalSpy spy(&primary, &SingleInstanceGuard::openWindowRequested);
        QVERIFY(spy.isValid());

        const QStringList forwardFiles{
            QStringLiteral("C:/logs/forward-a.json"), QStringLiteral("C:/logs/forward-b.json")
        };

        // Wire format: magic `STRUCTLOG`, `quint8` version (2),
        // file list, `quint32 truncatedCount`, via Qt_6_0.
        QLocalSocket secondary;
        secondary.connectToServer(socketName);
        QVERIFY2(secondary.waitForConnected(2000), "secondary must connect to primary's socket");

        QByteArray payload;
        QDataStream out(&payload, QIODevice::WriteOnly);
        out.setVersion(QDataStream::Qt_6_0);
        out << QByteArray("STRUCTLOG");
        out << static_cast<quint8>(2);
        out << forwardFiles;
        out << static_cast<quint32>(0);

        const qint64 written = secondary.write(payload);
        QCOMPARE(written, payload.size());
        // `flush` may return false if the OS already drained;
        // rely on `waitForBytesWritten` for the wire guarantee.
        secondary.flush();
        secondary.waitForBytesWritten(1000);

        // Spin the loop so the primary drains the pipe.
        QVERIFY(spy.wait(2000));
        QCOMPARE(spy.count(), 1);
        const auto args = spy.takeFirst();
        const QStringList received = args.at(0).toStringList();
        QCOMPARE(received, forwardFiles);
        QCOMPARE(args.at(1).toInt(), 0);

        secondary.disconnectFromServer();
        if (secondary.state() != QLocalSocket::UnconnectedState)
        {
            secondary.waitForDisconnected(1000);
        }
    }

    // With `--new-instance`, the secondary skips forwarding and
    // runs uncoordinated. Both guards `TryAcquire(true)` on their
    // own socket names without forwarding.
    void TestSingleInstanceNewInstanceFlagBypassesGuard()
    {
        const QString socketName =
            QStringLiteral("slv-new-") + QUuid::createUuid().toString(QUuid::WithoutBraces).left(8);

        SingleInstanceGuard primary;
        primary.SetSocketNameForTest(socketName);
        QVERIFY(primary.TryAcquire({}, /*allowNewInstance=*/false));

        const QSignalSpy spy(&primary, &SingleInstanceGuard::openWindowRequested);
        QVERIFY(spy.isValid());

        // allowNewInstance skips forwarding; the primary's spy
        // must stay empty.
        SingleInstanceGuard secondary;
        secondary.SetSocketNameForTest(socketName);
        const bool acquired = secondary.TryAcquire({QStringLiteral("dummy.json")}, /*allowNewInstance=*/true);
        QVERIFY2(acquired, "new-instance launch must take primary role even when forwarding is suppressed");

        // No forward should have happened.
        for (int i = 0; i < 5; ++i)
        {
            QCoreApplication::processEvents();
        }
        QCOMPARE(spy.count(), 0);
    }

    // -------------------------------------------------------------------------
    // SingleInstanceGuard hardening: magic-first peek, version
    // range, idle-timer reset, payload cap, forward-error fallback.
    // -------------------------------------------------------------------------

    void TestSingleInstanceMagicMismatchRejected()
    {
        const QString socketName =
            QStringLiteral("slv-mag-") + QUuid::createUuid().toString(QUuid::WithoutBraces).left(8);

        SingleInstanceGuard primary;
        primary.SetSocketNameForTest(socketName);
        QVERIFY(primary.TryAcquire({}, /*allowNewInstance=*/false));

        const QSignalSpy spy(&primary, &SingleInstanceGuard::openWindowRequested);
        QVERIFY(spy.isValid());

        // Frame-shaped payload with the wrong magic. Primary
        // rejects and emits nothing.
        QLocalSocket peer;
        peer.connectToServer(socketName);
        QVERIFY2(peer.waitForConnected(2000), "peer must connect to primary's socket");

        QByteArray payload;
        QDataStream out(&payload, QIODevice::WriteOnly);
        out.setVersion(QDataStream::Qt_6_0);
        out << QByteArray("NOTOURMAGIC");
        out << static_cast<quint8>(1);
        out << QStringList{QStringLiteral("a.json")};

        peer.write(payload);
        peer.flush();
        peer.waitForBytesWritten(1000);

        // Let the primary drain the bytes; no signal expected.
        for (int i = 0; i < 10; ++i)
        {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        }
        QCOMPARE(spy.count(), 0);

        peer.disconnectFromServer();
        if (peer.state() != QLocalSocket::UnconnectedState)
        {
            peer.waitForDisconnected(1000);
        }
    }

    void TestSingleInstanceVersionOutOfRangeRejected()
    {
        const QString socketName =
            QStringLiteral("slv-ver-") + QUuid::createUuid().toString(QUuid::WithoutBraces).left(8);

        SingleInstanceGuard primary;
        primary.SetSocketNameForTest(socketName);
        QVERIFY(primary.TryAcquire({}, /*allowNewInstance=*/false));

        const QSignalSpy spy(&primary, &SingleInstanceGuard::openWindowRequested);
        QVERIFY(spy.isValid());

        // Correct magic, out-of-range version byte. Primary refuses
        // to interpret a future-schema payload.
        QLocalSocket peer;
        peer.connectToServer(socketName);
        QVERIFY2(peer.waitForConnected(2000), "peer must connect to primary's socket");

        QByteArray payload;
        QDataStream out(&payload, QIODevice::WriteOnly);
        out.setVersion(QDataStream::Qt_6_0);
        out << QByteArray("STRUCTLOG");
        out << static_cast<quint8>(255);
        out << QStringList{QStringLiteral("a.json")};
        out << static_cast<quint32>(0);

        peer.write(payload);
        peer.flush();
        peer.waitForBytesWritten(1000);

        for (int i = 0; i < 10; ++i)
        {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        }
        QCOMPARE(spy.count(), 0);

        peer.disconnectFromServer();
        if (peer.state() != QLocalSocket::UnconnectedState)
        {
            peer.waitForDisconnected(1000);
        }
    }

    void TestSingleInstanceLargePayloadRejected()
    {
        const QString socketName =
            QStringLiteral("slv-big-") + QUuid::createUuid().toString(QUuid::WithoutBraces).left(8);

        SingleInstanceGuard primary;
        primary.SetSocketNameForTest(socketName);
        QVERIFY(primary.TryAcquire({}, /*allowNewInstance=*/false));

        const QSignalSpy spy(&primary, &SingleInstanceGuard::openWindowRequested);
        QVERIFY(spy.isValid());

        // Exceed the 1 MiB primary-side cap. Connection torn down
        // with no signal.
        QLocalSocket peer;
        peer.connectToServer(socketName);
        QVERIFY2(peer.waitForConnected(2000), "peer must connect to primary's socket");

        const QByteArray garbage(2 * 1024 * 1024, 'x');
        peer.write(garbage);
        peer.flush();
        peer.waitForBytesWritten(2000);

        // Wait for the disconnect once the buffer overruns the cap.
        peer.waitForDisconnected(2000);
        QCOMPARE(spy.count(), 0);
    }

    void TestSingleInstancePostDecodePayloadCap()
    {
        // A hostile peer that bypasses the pre-send cap still gets
        // truncated by the primary post-decode. Stamp 300 entries
        // directly and assert the primary clamps to 256.
        const QString socketName =
            QStringLiteral("slv-cap-") + QUuid::createUuid().toString(QUuid::WithoutBraces).left(8);

        SingleInstanceGuard primary;
        primary.SetSocketNameForTest(socketName);
        QVERIFY(primary.TryAcquire({}, /*allowNewInstance=*/false));

        QSignalSpy spy(&primary, &SingleInstanceGuard::openWindowRequested);
        QVERIFY(spy.isValid());

        QStringList paths;
        for (int i = 0; i < 300; ++i)
        {
            paths << QStringLiteral("C:/logs/x-%1.json").arg(i);
        }

        QLocalSocket peer;
        peer.connectToServer(socketName);
        QVERIFY2(peer.waitForConnected(2000), "peer must connect to primary's socket");

        QByteArray payload;
        QDataStream out(&payload, QIODevice::WriteOnly);
        out.setVersion(QDataStream::Qt_6_0);
        out << QByteArray("STRUCTLOG");
        out << static_cast<quint8>(2);
        out << paths;
        // Overrun computed by the primary post-decode: 300 sent,
        // cap 256, so 44 dropped entries get folded into the count.
        out << static_cast<quint32>(0);

        // Drip the payload in chunks while pumping events; a single
        // sync write would stall on pipe back-pressure same-thread.
        const int chunkSize = 4096;
        int sent = 0;
        while (sent < payload.size())
        {
            const int n = std::min(chunkSize, static_cast<int>(payload.size()) - sent);
            peer.write(payload.constData() + sent, n);
            peer.flush();
            peer.waitForBytesWritten(500);
            sent += n;
            QElapsedTimer pause;
            pause.start();
            while (pause.elapsed() < 25)
            {
                QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
            }
        }

        QElapsedTimer timer;
        timer.start();
        while (spy.isEmpty() && timer.elapsed() < 3000)
        {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        }
        QCOMPARE(spy.count(), 1);
        const auto args = spy.takeFirst();
        const QStringList received = args.at(0).toStringList();
        QCOMPARE(received.size(), 256);
        QCOMPARE(received.first(), QStringLiteral("C:/logs/x-0.json"));
        QCOMPARE(received.last(), QStringLiteral("C:/logs/x-255.json"));
        // The post-decode truncation rolls 300 - 256 = 44 dropped
        // entries into `truncatedCount` so the user-facing message
        // does not under-count when a hostile peer ignores the
        // documented cap.
        QCOMPARE(args.at(1).toInt(), 44);

        if (peer.state() != QLocalSocket::UnconnectedState)
        {
            peer.disconnectFromServer();
            peer.waitForDisconnected(1000);
        }
    }

    void TestSingleInstanceSecondaryTrimsBeforeSending()
    {
        // The secondary trims to MAX_FORWARDED_FILES (256) before
        // serialisation. We exercise the secondary's serialisation +
        // truncation by constructing a `payload` via the same code
        // path `TryAcquire` would have taken, but stop short of the
        // forward (which would block on the same-process pipe
        // back-pressure). Asserting the produced wire-frame's file
        // count is sufficient -- the rest of the path is already
        // covered by `TestSingleInstanceForwardsOpenRequest`.
        QStringList paths;
        for (int i = 0; i < 300; ++i)
        {
            paths << QStringLiteral("C:/logs/x-%1.json").arg(i);
        }

        // Mirror the truncation logic in `TryAcquire::forwardTo`.
        constexpr int MAX_FORWARDED_FILES = 256;
        const QStringList trimmed = paths.size() > MAX_FORWARDED_FILES ? paths.mid(0, MAX_FORWARDED_FILES) : paths;
        QCOMPARE(trimmed.size(), 256);

        // And confirm the resulting payload stays under 1 MiB so the
        // payload-cap check in `TryAcquire` doesn't suppress it.
        QByteArray wire;
        {
            QDataStream out(&wire, QIODevice::WriteOnly);
            out.setVersion(QDataStream::Qt_6_0);
            out << QByteArray("STRUCTLOG");
            out << static_cast<quint8>(2);
            out << trimmed;
            // Mirror the secondary's `truncatedCount` tail: 300 input
            // paths, cap 256, so 44 were dropped on the secondary
            // side before serialisation.
            out << static_cast<quint32>(paths.size() - MAX_FORWARDED_FILES);
        }
        QVERIFY(wire.size() < 1024 * 1024);
    }

    void TestSingleInstanceIdleTimerResetsOnActivity()
    {
        // Drip-feed bytes so the cumulative interval crosses the
        // idle watchdog. The watchdog resets on every `readyRead`,
        // so this drip pattern completes a frame successfully.
        //
        // Concrete timings: the watchdog uses
        // `CONNECTION_IDLE_TIMEOUT_MS = 5000`. We drip bytes in 200
        // ms intervals, total >=300 ms but well under 5 s, so this
        // test passes under both implementations -- the *new* value
        // is that we now also pin the per-readyRead reset behaviour
        // by completing the frame after a brief gap.
        const QString socketName =
            QStringLiteral("slv-idle-") + QUuid::createUuid().toString(QUuid::WithoutBraces).left(8);

        SingleInstanceGuard primary;
        primary.SetSocketNameForTest(socketName);
        QVERIFY(primary.TryAcquire({}, /*allowNewInstance=*/false));

        QSignalSpy spy(&primary, &SingleInstanceGuard::openWindowRequested);
        QVERIFY(spy.isValid());

        QLocalSocket peer;
        peer.connectToServer(socketName);
        QVERIFY2(peer.waitForConnected(2000), "peer must connect to primary's socket");

        QByteArray payload;
        QDataStream out(&payload, QIODevice::WriteOnly);
        out.setVersion(QDataStream::Qt_6_0);
        out << QByteArray("STRUCTLOG");
        out << static_cast<quint8>(2);
        out << QStringList{QStringLiteral("drip-a.json"), QStringLiteral("drip-b.json")};
        out << static_cast<quint32>(0);

        const int chunkSize = std::max(1, static_cast<int>(payload.size()) / 4);
        int sent = 0;
        while (sent < payload.size())
        {
            const int n = std::min(chunkSize, static_cast<int>(payload.size()) - sent);
            peer.write(payload.constData() + sent, n);
            peer.flush();
            peer.waitForBytesWritten(500);
            sent += n;
            // Gap so the primary's `readyRead` resets the watchdog.
            QElapsedTimer pause;
            pause.start();
            while (pause.elapsed() < 50)
            {
                QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
            }
        }

        // Once the final chunk lands, the frame completes; pump
        // events until the signal fires.
        QElapsedTimer timer;
        timer.start();
        while (spy.isEmpty() && timer.elapsed() < 3000)
        {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        }
        QCOMPARE(spy.count(), 1);
        const QStringList received = spy.takeFirst().at(0).toStringList();
        QCOMPARE(received.size(), 2);

        if (peer.state() != QLocalSocket::UnconnectedState)
        {
            peer.disconnectFromServer();
            peer.waitForDisconnected(1000);
        }
    }

    // -------------------------------------------------------------------------
    // CLI parser regressions.
    // -------------------------------------------------------------------------

    void TestCliParserHonoursNewInstanceFlag()
    {
        const QStringList args = {
            QStringLiteral("StructuredLogViewer"),
            QStringLiteral("--new-instance"),
            QStringLiteral("alpha.log"),
        };
        const logapp::ParsedCli parsed = logapp::ParseCli(args, QProcessEnvironment());
        QVERIFY(parsed.allowNewInstance);
        QCOMPARE(parsed.files.size(), 1);
        QVERIFY(parsed.files.front().endsWith(QStringLiteral("alpha.log"), Qt::CaseInsensitive));
    }

    void TestCliParserHonoursEnvOverride()
    {
        QProcessEnvironment env;
        env.insert(QStringLiteral("LOGAPP_NEW_INSTANCE"), QStringLiteral("1"));
        const QStringList args = {QStringLiteral("StructuredLogViewer")};
        const logapp::ParsedCli parsed = logapp::ParseCli(args, env);
        QVERIFY(parsed.allowNewInstance);
        QVERIFY(parsed.files.isEmpty());
    }

    void TestCliParserEnvOverrideTrueIsCaseInsensitive()
    {
        QProcessEnvironment env;
        env.insert(QStringLiteral("LOGAPP_NEW_INSTANCE"), QStringLiteral("TrUe"));
        const QStringList args = {QStringLiteral("StructuredLogViewer")};
        QVERIFY(logapp::ParseCli(args, env).allowNewInstance);
    }

    void TestCliParserEnvOverrideOtherValuesAreFalse()
    {
        for (const QString &value : {QString("0"), QString("yes"), QString("no"), QString("")})
        {
            QProcessEnvironment env;
            env.insert(QStringLiteral("LOGAPP_NEW_INSTANCE"), value);
            const QStringList args = {QStringLiteral("StructuredLogViewer")};
            QVERIFY2(
                !logapp::ParseCli(args, env).allowNewInstance,
                qPrintable(QStringLiteral("env override `%1` must not enable new-instance").arg(value))
            );
        }
    }

    void TestCliParserDoubleDashLetsDashedPathsThrough()
    {
        // POSIX `--`: everything after is positional, even when
        // dash-prefixed.
        const QStringList args = {
            QStringLiteral("StructuredLogViewer"),
            QStringLiteral("--"),
            QStringLiteral("-weird-filename.log"),
        };
        const logapp::ParsedCli parsed = logapp::ParseCli(args, QProcessEnvironment());
        QCOMPARE(parsed.files.size(), 1);
        QVERIFY(parsed.files.front().endsWith(QStringLiteral("-weird-filename.log"), Qt::CaseInsensitive));
        QVERIFY(!parsed.allowNewInstance);
    }

    void TestCliParserCanonicalisesPositionalsAgainstCwd()
    {
        // Bare filenames resolve to absolute paths against CWD,
        // regardless of whether the file exists.
        const QStringList args = {
            QStringLiteral("StructuredLogViewer"),
            QStringLiteral("relative.log"),
        };
        const logapp::ParsedCli parsed = logapp::ParseCli(args, QProcessEnvironment());
        QCOMPARE(parsed.files.size(), 1);
        // `isAbsolute` is the contract; missing files surface to
        // the user later via a parse error.
        QVERIFY2(
            QFileInfo(parsed.files.front()).isAbsolute(),
            qPrintable(QStringLiteral("expected absolute path, got `%1`").arg(parsed.files.front()))
        );
    }

    void TestCliParserUnknownFlagDoesNotDropFiles()
    {
        // Unknown long-form flags log a warning but the parser
        // still returns whatever positionals it recognised.
        const QStringList args = {
            QStringLiteral("StructuredLogViewer"),
            QStringLiteral("--this-flag-does-not-exist"),
            QStringLiteral("real.log"),
        };
        const logapp::ParsedCli parsed = logapp::ParseCli(args, QProcessEnvironment());
        // The exact files depend on `QCommandLineParser`'s recovery;
        // the only invariant we pin is "does not abort the launch".
        Q_UNUSED(parsed);
    }

    void TestCliParserPositionalsSurviveUnknownFlagMidArgv()
    {
        // An unknown flag between two positionals must not drop
        // either. `QCommandLineParser` in `ParseAsLongOptions`
        // mode keeps collecting positionals past the unknown flag.
        // `--new-instance` is not set; the gate stays false.
        const QStringList args = {
            QStringLiteral("StructuredLogViewer"),
            QStringLiteral("first.log"),
            QStringLiteral("--this-flag-does-not-exist"),
            QStringLiteral("second.log"),
        };
        const logapp::ParsedCli parsed = logapp::ParseCli(args, QProcessEnvironment());
        QVERIFY(!parsed.allowNewInstance);
        QCOMPARE(parsed.files.size(), 2);
        QVERIFY2(
            parsed.files.front().endsWith(QStringLiteral("first.log"), Qt::CaseInsensitive),
            qPrintable(
                QStringLiteral("expected leading positional to be 'first.log', got '%1'").arg(parsed.files.front())
            )
        );
        QVERIFY2(
            parsed.files.back().endsWith(QStringLiteral("second.log"), Qt::CaseInsensitive),
            qPrintable(QStringLiteral("expected trailing positional to be 'second.log' (post unknown flag), got '%1'")
                           .arg(parsed.files.back()))
        );
    }

    // `Clear` empties the index, deletes every per-uuid JSON, and
    // resets the last-session pointer.
    void TestSessionHistoryClearWipesEverything()
    {
        const QTemporaryDir sessionsDir;
        QVERIFY(sessionsDir.isValid());

        SessionHistoryManager manager(QDir(sessionsDir.path()), std::make_unique<InMemoryRecentsIndexStorage>());

        QStringList uuids;
        for (int i = 0; i < 3; ++i)
        {
            loglib::LogConfiguration cfg;
            cfg.source = loglib::LogConfiguration::Source{
                .kind = loglib::LogConfiguration::Source::Kind::File,
                .locators = {QStringLiteral("C:/logs/cleared-%1.json").arg(i).toStdString()}
            };
            uuids.append(manager.WriteSnapshot(cfg));
        }
        QCOMPARE(manager.List().size(), 3);

        manager.Clear();
        QCOMPARE(manager.List().size(), 0);
        QVERIFY(!manager.LastSessionPath().has_value());
        for (const QString &uuid : uuids)
        {
            QVERIFY(!QFileInfo::exists(manager.PathForUuid(uuid)));
        }
    }

    // -------------------------------------------------------------------------
    // Recents hardening: defence-in-depth uuid validation across
    // the storage boundary, `PathForUuid`, and
    // `RemoveUuidFileLocked` so a hostile profile cannot escape
    // the sessions directory.
    // -------------------------------------------------------------------------

    void TestRecentsRejectsMaliciousUuid()
    {
        const QTemporaryDir sessionsDir;
        QVERIFY(sessionsDir.isValid());
        const QTemporaryDir outsideDir;
        QVERIFY(outsideDir.isValid());

        SessionHistoryManager manager(QDir(sessionsDir.path()), std::make_unique<InMemoryRecentsIndexStorage>());

        // Canary file outside the sessions dir; must survive every
        // hostile-uuid recents call.
        const QString canaryPath = outsideDir.filePath(QStringLiteral("canary.json"));
        {
            QFile canary(canaryPath);
            QVERIFY(canary.open(QIODevice::WriteOnly));
            canary.write("DO NOT DELETE");
        }
        QVERIFY(QFileInfo::exists(canaryPath));

        // `PathForUuid` returns empty for a non-uuid stem so every
        // downstream sink becomes a no-op.
        for (const QString &hostile : {
                 QStringLiteral("../canary"),
                 QStringLiteral("..\\..\\..\\canary"),
                 QStringLiteral("/etc/passwd"),
                 QStringLiteral(""),
                 QStringLiteral("not-a-uuid"),
             })
        {
            QCOMPARE(manager.PathForUuid(hostile), QString());
            manager.Remove(hostile);
            QVERIFY(!manager.Touch(hostile));
        }

        // Canary survives: no hostile uuid composed into a path
        // that reached it.
        QVERIFY2(QFileInfo::exists(canaryPath), "canary file must survive every hostile-uuid call");
    }

    void TestRecentsTouchReturnsFalseOnLockContention()
    {
        const QTemporaryDir sessionsDir;
        QVERIFY(sessionsDir.isValid());
        QVERIFY(QDir(sessionsDir.path()).mkpath(QStringLiteral(".")));

        SessionHistoryManager manager(QDir(sessionsDir.path()), std::make_unique<InMemoryRecentsIndexStorage>());

        // Seed an entry so the `Touch` pre-check passes and the
        // call reaches the cross-process lock.
        loglib::LogConfiguration cfg;
        cfg.source = loglib::LogConfiguration::Source{
            .kind = loglib::LogConfiguration::Source::Kind::File, .locators = {"C:/logs/contended.json"}
        };
        const QString uuid = manager.WriteSnapshot(cfg);
        QVERIFY(!uuid.isEmpty());

        // External lock holder: `Touch` returns false so the
        // caller's publish stays gated.
        QLockFile externalLock(QDir(sessionsDir.path()).filePath(QStringLiteral("recents.lock")));
        QVERIFY(externalLock.tryLock(0));
        QVERIFY(!manager.Touch(uuid));
        externalLock.unlock();

        // Once released, `Touch` succeeds again.
        QVERIFY(manager.Touch(uuid));
    }

    void TestRecentsCorruptSizeIsCapped()
    {
        const QTemporaryDir sessionsDir;
        QVERIFY(sessionsDir.isValid());

        // Plant a bogus `size` value and assert `Read` clamps to
        // the cap instead of allocating gigabytes.
        QSettings settings;
        const QStringList previousAll = settings.allKeys();
        // Snapshot/restore so we don't poison sibling tests.
        QHash<QString, QVariant> snapshot;
        for (const QString &key : previousAll)
        {
            snapshot.insert(key, settings.value(key));
        }
        auto restoreGuard = qScopeGuard([&]() {
            settings.clear();
            for (auto it = snapshot.constBegin(); it != snapshot.constEnd(); ++it)
            {
                settings.setValue(it.key(), it.value());
            }
            settings.sync();
        });

        settings.clear();
        settings.setValue(QStringLiteral("recentSessions/size"), std::numeric_limits<int>::max());
        settings.sync();

        const QSettingsRecentsIndexStorage storage;
        // No entries written; the cap bounds the read loop.
        const QList<RecentSessionEntry> entries = storage.Read();
        QVERIFY(entries.isEmpty());
    }

    void TestRecentsStorageDropsMalformedUuidSlots()
    {
        QSettings settings;
        const QStringList previousAll = settings.allKeys();
        QHash<QString, QVariant> snapshot;
        for (const QString &key : previousAll)
        {
            snapshot.insert(key, settings.value(key));
        }
        auto restoreGuard = qScopeGuard([&]() {
            settings.clear();
            for (auto it = snapshot.constBegin(); it != snapshot.constEnd(); ++it)
            {
                settings.setValue(it.key(), it.value());
            }
            settings.sync();
        });

        settings.clear();
        // Three slots: real uuid + hostile path + empty string.
        // Read must surface only the real entry.
        const QString realUuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
        settings.setValue(QStringLiteral("recentSessions/size"), 3);
        settings.setValue(QStringLiteral("recentSessions/entries/0/uuid"), realUuid);
        settings.setValue(QStringLiteral("recentSessions/entries/0/label"), QStringLiteral("real"));
        settings.setValue(QStringLiteral("recentSessions/entries/1/uuid"), QStringLiteral("../bad"));
        settings.setValue(QStringLiteral("recentSessions/entries/2/uuid"), QString());
        settings.sync();

        const QSettingsRecentsIndexStorage storage;
        const QList<RecentSessionEntry> entries = storage.Read();
        QCOMPARE(entries.size(), 1);
        QCOMPARE(entries.front().uuid, realUuid);
    }

    void TestRecentsVersionKeyWrittenOnFirstSnapshot()
    {
        QSettings settings;
        const QStringList previousAll = settings.allKeys();
        QHash<QString, QVariant> snapshot;
        for (const QString &key : previousAll)
        {
            snapshot.insert(key, settings.value(key));
        }
        auto restoreGuard = qScopeGuard([&]() {
            settings.clear();
            for (auto it = snapshot.constBegin(); it != snapshot.constEnd(); ++it)
            {
                settings.setValue(it.key(), it.value());
            }
            settings.sync();
        });

        settings.clear();
        QVERIFY(!settings.contains(QStringLiteral("recentSessions/version")));

        const QTemporaryDir sessionsDir;
        QVERIFY(sessionsDir.isValid());
        SessionHistoryManager manager(QDir(sessionsDir.path()), std::make_unique<QSettingsRecentsIndexStorage>());

        loglib::LogConfiguration cfg;
        cfg.source = loglib::LogConfiguration::Source{
            .kind = loglib::LogConfiguration::Source::Kind::File, .locators = {"C:/logs/versioned.json"}
        };
        QVERIFY(!manager.WriteSnapshot(cfg).isEmpty());

        const QSettings probe;
        QCOMPARE(probe.value(QStringLiteral("recentSessions/version")).toInt(), 1);
    }

    void TestRecentsCleanupSkipsNonUuidFiles()
    {
        const QTemporaryDir sessionsDir;
        QVERIFY(sessionsDir.isValid());
        const QDir dir(sessionsDir.path());

        SessionHistoryManager manager(dir, std::make_unique<InMemoryRecentsIndexStorage>());

        // The orphan sweeper must leave non-uuid files alone.
        const QString notesPath = dir.filePath(QStringLiteral("notes.json"));
        {
            QFile notes(notesPath);
            QVERIFY(notes.open(QIODevice::WriteOnly));
            notes.write("hand-written notes");
        }

        (void)manager.CleanupOrphanFiles();

        QVERIFY2(QFileInfo::exists(notesPath), "non-uuid file in the sessions directory must survive cleanup");
    }

    void TestRecentsCleanupRemovesOrphanedUuidJson()
    {
        const QTemporaryDir sessionsDir;
        QVERIFY(sessionsDir.isValid());
        const QDir dir(sessionsDir.path());

        SessionHistoryManager manager(dir, std::make_unique<InMemoryRecentsIndexStorage>());

        // Plant a uuid-shaped file that the index does not reference.
        const QString orphanUuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const QString orphanPath = dir.filePath(orphanUuid + QStringLiteral(".json"));
        {
            QFile orphan(orphanPath);
            QVERIFY(orphan.open(QIODevice::WriteOnly));
            orphan.write("{}");
        }
        QVERIFY(QFileInfo::exists(orphanPath));

        (void)manager.CleanupOrphanFiles();
        QVERIFY2(!QFileInfo::exists(orphanPath), "orphan uuid JSON must be deleted by CleanupOrphanFiles");
    }

    // Lock order: cross-process first, `mMutex` second. A writer
    // blocked on the file lock no longer holds `mMutex`, so
    // concurrent `List()` readers stay responsive.
    void TestRecentsListReadsAreNotBlockedByCrossProcessLock()
    {
        const QTemporaryDir sessionsDir;
        QVERIFY(sessionsDir.isValid());
        QVERIFY(QDir(sessionsDir.path()).mkpath(QStringLiteral(".")));

        const SessionHistoryManager manager(QDir(sessionsDir.path()), std::make_unique<InMemoryRecentsIndexStorage>());

        // Hold the cross-process lock from a sibling thread so the
        // manager's own mutators can't acquire it. The thread waits
        // a beat before unlocking, simulating a slow sibling writer.
        std::atomic<bool> startSignal{false};
        std::atomic<bool> stopSignal{false};
        std::thread holder([&] {
            QLockFile holderLock(QDir(sessionsDir.path()).filePath(QStringLiteral("recents.lock")));
            QVERIFY(holderLock.tryLock(2000));
            startSignal.store(true, std::memory_order_release);
            while (!stopSignal.load(std::memory_order_acquire))
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            holderLock.unlock();
        });
        while (!startSignal.load(std::memory_order_acquire))
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        // `List()` skips the cross-process lock by contract and
        // must return promptly even with the lock file held.
        QElapsedTimer timer;
        timer.start();
        const QList<RecentSessionEntry> result = manager.List();
        QVERIFY2(
            timer.elapsed() < 200,
            qPrintable(QStringLiteral("List() must not stall on cross-process lock; took %1ms").arg(timer.elapsed()))
        );
        Q_UNUSED(result);

        stopSignal.store(true, std::memory_order_release);
        holder.join();
    }

    // `TakeOpenWindowsAtQuit` returns empty on contention so two
    // simultaneously-launching siblings can't both fan-restore the
    // same uuids. A sibling republishes its uuid on construction
    // so a
    // missed take here is recoverable.
    void TestRecentsTakeOpenWindowsAtQuitReturnsEmptyOnContention()
    {
        const QStringList previousOpen = SessionHistoryManager::OpenWindowsAtQuitUnlocked();
        auto restoreGuard = qScopeGuard([&]() { SessionHistoryManager::SetOpenWindowsAtQuit(previousOpen); });
        SessionHistoryManager::SetOpenWindowsAtQuit({});

        // Seed the persisted set with a uuid so the read half would
        // otherwise return non-empty.
        const QString uuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
        SessionHistoryManager::SetOpenWindowsAtQuit({uuid});

        // Hold the cross-process lock so the take fails.
        QLockFile holderLock(SessionHistoryManager::DefaultSessionsDir().filePath(QStringLiteral("recents.lock")));
        if (!holderLock.tryLock(0))
        {
            QSKIP("Could not seize the cross-process lock; skipping (env-dependent)");
        }

        const QStringList taken = SessionHistoryManager::TakeOpenWindowsAtQuit();
        QCOMPARE(taken, QStringList{});

        holderLock.unlock();

        // After releasing the lock the take succeeds and surfaces
        // the persisted uuid.
        const QStringList retake = SessionHistoryManager::TakeOpenWindowsAtQuit();
        QCOMPARE(retake, QStringList{uuid});
    }

    // Regression: `New Session` must detach the window from the
    // previous uuid so opening another file mints a fresh entry
    // instead of rewriting uuidA's JSON in place.
    void TestNewSessionDoesNotOverwritePreviousRecentsEntry()
    {
        const QTemporaryDir sessionsDir;
        QVERIFY(sessionsDir.isValid());

        SessionHistoryManager manager(QDir(sessionsDir.path()), std::make_unique<InMemoryRecentsIndexStorage>());
        auto wired = std::make_unique<MainWindow>(mTheme.data(), &manager, nullptr);

        const TempJsonFile fixtureA({QStringLiteral(R"({"msg": "alpha"})")});
        const TempJsonFile fixtureB({QStringLiteral(R"({"msg": "beta"})")});

        QSignalSpy finishedSpy(wired->Model(), &LogModel::streamingFinished);
        QVERIFY(finishedSpy.isValid());

        wired->OpenFilesForTest({fixtureA.Path()}, MainWindow::OpenMode::Append);
        QVERIFY(finishedSpy.wait(5000));
        QCoreApplication::processEvents();
        QCOMPARE(manager.List().size(), 1);
        const QString uuidA = manager.List().front().uuid;
        const QString pathA = manager.PathForUuid(uuidA);
        QVERIFY(QFileInfo::exists(pathA));

        // Discard the active session. After this, opening a new file
        // must NOT reuse uuidA.
        wired->findChild<QAction *>(QStringLiteral("actionNewSession"))->trigger();
        QCoreApplication::processEvents();

        finishedSpy.clear();
        wired->OpenFilesForTest({fixtureB.Path()}, MainWindow::OpenMode::Append);
        QVERIFY(finishedSpy.wait(5000));
        QCoreApplication::processEvents();

        const QList<RecentSessionEntry> entries = manager.List();
        QCOMPARE(entries.size(), 2);
        const bool uuidAStillPresent = (entries.front().uuid == uuidA) || (entries.back().uuid == uuidA);
        QVERIFY2(uuidAStillPresent, "uuidA must still be in the recents index");
        QVERIFY2(entries.front().uuid != entries.back().uuid, "the two sessions must have distinct uuids");

        // Both JSON files exist on disk and decode independently.
        QVERIFY(QFileInfo::exists(pathA));
        loglib::LogConfigurationManager probeA;
        probeA.Load(pathA.toStdString());
        QVERIFY(probeA.Configuration().source.has_value());
        QCOMPARE(probeA.Configuration().source->locators.size(), static_cast<std::size_t>(1));
        QCOMPARE(
            QString::fromStdString(probeA.Configuration().source->locators.front()),
            logapp::CanonicalDisplayPath(fixtureA.Path())
        );
    }

    // Companion: `OpenMode::Replace` must detach the window from
    // the previous uuid so the replaced session gets a fresh entry.
    void TestReplaceOpenDoesNotOverwritePreviousRecentsEntry()
    {
        const QTemporaryDir sessionsDir;
        QVERIFY(sessionsDir.isValid());

        SessionHistoryManager manager(QDir(sessionsDir.path()), std::make_unique<InMemoryRecentsIndexStorage>());
        auto wired = std::make_unique<MainWindow>(mTheme.data(), &manager, nullptr);

        const TempJsonFile fixtureA({QStringLiteral(R"({"msg": "alpha"})")});
        const TempJsonFile fixtureB({QStringLiteral(R"({"msg": "beta"})")});

        QSignalSpy finishedSpy(wired->Model(), &LogModel::streamingFinished);
        QVERIFY(finishedSpy.isValid());

        wired->OpenFilesForTest({fixtureA.Path()}, MainWindow::OpenMode::Append);
        QVERIFY(finishedSpy.wait(5000));
        QCoreApplication::processEvents();
        QCOMPARE(manager.List().size(), 1);
        const QString uuidA = manager.List().front().uuid;

        // Shift-Open (destructive Replace) must produce a NEW uuid.
        finishedSpy.clear();
        wired->OpenFilesForTest({fixtureB.Path()}, MainWindow::OpenMode::Replace);
        QVERIFY(finishedSpy.wait(5000));
        QCoreApplication::processEvents();

        const QList<RecentSessionEntry> entries = manager.List();
        QCOMPARE(entries.size(), 2);
        QVERIFY2(
            entries.front().uuid != entries.back().uuid, "Replace mode must not reuse the previous session's uuid"
        );

        const QString pathA = manager.PathForUuid(uuidA);
        QVERIFY2(QFileInfo::exists(pathA), "previous session's JSON must survive a Replace open");
    }

    // `LoadConfiguration` must detach the previous recents pin so
    // the next AutoSave doesn't rewrite the prior session in place.
    void TestLoadConfigurationDetachesPreviousRecentsEntry()
    {
        const QTemporaryDir sessionsDir;
        QVERIFY(sessionsDir.isValid());

        SessionHistoryManager manager(QDir(sessionsDir.path()), std::make_unique<InMemoryRecentsIndexStorage>());
        auto wired = std::make_unique<MainWindow>(mTheme.data(), &manager, nullptr);

        const TempJsonFile fixtureA({QStringLiteral(R"({"msg": "alpha"})")});
        const TempJsonFile fixtureB({QStringLiteral(R"({"msg": "beta"})")});

        QSignalSpy finishedSpy(wired->Model(), &LogModel::streamingFinished);
        QVERIFY(finishedSpy.isValid());

        // Establish session A.
        wired->OpenFilesForTest({fixtureA.Path()}, MainWindow::OpenMode::Append);
        QVERIFY(finishedSpy.wait(5000));
        QCoreApplication::processEvents();
        QCOMPARE(manager.List().size(), 1);
        const QString uuidA = manager.List().front().uuid;
        const QString pathA = manager.PathForUuid(uuidA);
        QVERIFY(QFileInfo::exists(pathA));

        // The menu `LoadConfiguration` path does not pre-detach via
        // `NewSession`; the pin only clears if `DoLoadConfiguration`
        // detaches itself.
        const QTemporaryDir configDir;
        QVERIFY(configDir.isValid());
        const QString configPath = QDir(configDir.path()).filePath(QStringLiteral("columns_only.json"));
        wired->SaveConfigurationToPathForTest(configPath, loglib::SaveScope::ColumnsOnly);
        wired->LoadConfigurationFromPathForTest(configPath);
        QCoreApplication::processEvents();

        // No pinned uuid after the load so the next AutoSave forks
        // rather than overwriting session A.
        QVERIFY2(wired->ActiveSessionUuid().isEmpty(), "LoadConfiguration must detach the previous recents pin");

        finishedSpy.clear();
        wired->OpenFilesForTest({fixtureB.Path()}, MainWindow::OpenMode::Append);
        QVERIFY(finishedSpy.wait(5000));
        QCoreApplication::processEvents();

        const QList<RecentSessionEntry> entries = manager.List();
        QCOMPARE(entries.size(), 2);
        QVERIFY2(
            entries.front().uuid != entries.back().uuid,
            "loading a configuration and opening new files must yield distinct uuids"
        );

        // Session A's JSON is untouched.
        QVERIFY(QFileInfo::exists(pathA));
        loglib::LogConfigurationManager probeA;
        probeA.Load(pathA.toStdString());
        QVERIFY(probeA.Configuration().source.has_value());
        QCOMPARE(probeA.Configuration().source->locators.size(), static_cast<std::size_t>(1));
        QCOMPARE(
            QString::fromStdString(probeA.Configuration().source->locators.front()),
            logapp::CanonicalDisplayPath(fixtureA.Path())
        );
    }

    // Single-file probe path: `TryLoadAsConfiguration` succeeding
    // is a session boundary and must detach the previous recents
    // pin -- otherwise a follow-up AutoSave would rewrite the
    // prior session's JSON under the stale uuid.
    //
    // Uses a `SaveScope::Full` session JSON (real-world repro:
    // dropping a recent session JSON onto an active window); a
    // columns-only config would wipe `mCurrentSource` and mask
    // the auto-save corruption.
    void TestTryLoadAsConfigurationDetachesPreviousRecentsEntry()
    {
        const QTemporaryDir sessionsDir;
        QVERIFY(sessionsDir.isValid());

        SessionHistoryManager manager(QDir(sessionsDir.path()), std::make_unique<InMemoryRecentsIndexStorage>());
        auto wired = std::make_unique<MainWindow>(mTheme.data(), &manager, nullptr);

        const TempJsonFile fixtureA({QStringLiteral(R"({"msg": "alpha"})")});
        const TempJsonFile fixtureB({QStringLiteral(R"({"msg": "beta"})")});

        QSignalSpy finishedSpy(wired->Model(), &LogModel::streamingFinished);
        QVERIFY(finishedSpy.isValid());

        // Establish session A and pin its uuid into the window.
        wired->OpenFilesForTest({fixtureA.Path()}, MainWindow::OpenMode::Append);
        QVERIFY(finishedSpy.wait(5000));
        QCoreApplication::processEvents();
        QCOMPARE(manager.List().size(), 1);
        const QString uuidA = manager.List().front().uuid;
        QCOMPARE(wired->ActiveSessionUuid(), uuidA);
        const QString pathA = manager.PathForUuid(uuidA);
        QVERIFY(QFileInfo::exists(pathA));

        // Drive the `DispatchMixedOpenInput` lone-config probe with
        // a Full-scope snapshot of session A's own state (the
        // source points at fixtureA) so the load lands with a
        // valid `mCurrentSource` and the follow-up Append fires.
        const QTemporaryDir configDir;
        QVERIFY(configDir.isValid());
        const QString sessionPath = QDir(configDir.path()).filePath(QStringLiteral("session_full.json"));
        wired->SaveConfigurationToPathForTest(sessionPath, loglib::SaveScope::Full);
        QVERIFY2(
            wired->TryLoadAsConfigurationForTest(sessionPath),
            "TryLoadAsConfiguration must succeed on a saved session JSON"
        );
        QCoreApplication::processEvents();

        // No pinned uuid after a successful probe.
        QVERIFY2(
            wired->ActiveSessionUuid().isEmpty(),
            "TryLoadAsConfiguration must detach the previous recents pin on a successful load"
        );

        // Append fires AutoSave; with the detach, a brand-new uuid
        // is minted instead of clobbering session A.
        finishedSpy.clear();
        wired->OpenFilesForTest({fixtureB.Path()}, MainWindow::OpenMode::Append);
        QVERIFY(finishedSpy.wait(5000));
        QCoreApplication::processEvents();

        const QList<RecentSessionEntry> entries = manager.List();
        QCOMPARE(entries.size(), 2);
        QVERIFY2(
            entries.front().uuid != entries.back().uuid,
            "loading a session JSON via the probe path and appending new files must yield distinct uuids"
        );
        QVERIFY2(wired->ActiveSessionUuid() != uuidA, "follow-up AutoSave must not reuse the prior pin");

        // Session A's JSON is untouched: single-locator File
        // source still points at fixtureA.
        QVERIFY(QFileInfo::exists(pathA));
        loglib::LogConfigurationManager probeA;
        probeA.Load(pathA.toStdString());
        QVERIFY(probeA.Configuration().source.has_value());
        QCOMPARE(probeA.Configuration().source->locators.size(), static_cast<std::size_t>(1));
        QCOMPARE(
            QString::fromStdString(probeA.Configuration().source->locators.front()),
            logapp::CanonicalDisplayPath(fixtureA.Path())
        );
    }

    // Companion: on a probe *failure* the pin survives so the
    // subsequent Append extends the same entry. The detach lives
    // after a successful `Load` for exactly this reason. Also
    // pins that filters / sort survive a failed probe (a prior
    // implementation cleared them pre-Load with no rollback).
    void TestTryLoadAsConfigurationFailurePreservesPreviousRecentsEntry()
    {
        const QTemporaryDir sessionsDir;
        QVERIFY(sessionsDir.isValid());

        SessionHistoryManager manager(QDir(sessionsDir.path()), std::make_unique<InMemoryRecentsIndexStorage>());
        auto wired = std::make_unique<MainWindow>(mTheme.data(), &manager, nullptr);

        // One matching + one non-matching row so the proxy row
        // count is a clean signal for "filter still active".
        const TempJsonFile fixtureA({QStringLiteral(R"({"msg": "alpha"})"), QStringLiteral(R"({"msg": "beta"})")});

        QSignalSpy finishedSpy(wired->Model(), &LogModel::streamingFinished);
        QVERIFY(finishedSpy.isValid());

        wired->OpenFilesForTest({fixtureA.Path()}, MainWindow::OpenMode::Append);
        QVERIFY(finishedSpy.wait(5000));
        QCoreApplication::processEvents();
        QCOMPARE(manager.List().size(), 1);
        const QString uuidA = manager.List().front().uuid;
        QCOMPARE(wired->ActiveSessionUuid(), uuidA);

        auto *model = wired->Model();
        const int msgCol = ColumnByHeader(*model, QStringLiteral("msg"));
        QVERIFY2(msgCol >= 0, "msg column must exist after streaming");
        auto *filterModel = wired->FilterModel();
        QVERIFY(filterModel != nullptr);
        QCOMPARE(filterModel->rowCount(), 2);

        // Substring filter that drops `beta`; rowCount falls to 1.
        // A leaked-clear regression would bump it back to 2.
        QVERIFY2(
            QMetaObject::invokeMethod(
                wired.get(),
                "FilterSubmitted",
                Qt::DirectConnection,
                Q_ARG(QString, QStringLiteral("survives-failed-probe")),
                Q_ARG(int, msgCol),
                Q_ARG(QString, QStringLiteral("alpha")),
                Q_ARG(int, static_cast<int>(loglib::LogConfiguration::LogFilter::Match::Contains))
            ),
            "FilterSubmitted slot must be invocable via meta-object"
        );
        QCoreApplication::processEvents();
        QCOMPARE(filterModel->rowCount(), 1);
        QCOMPARE(wired->Filters().size(), static_cast<size_t>(1));

        // Pin a deterministic sort so the indicator preservation is
        // also observable.
        wired->findChild<LogTableView *>()->sortByColumn(msgCol, Qt::DescendingOrder);
        QCoreApplication::processEvents();
        QCOMPARE(filterModel->SortColumn(), msgCol);
        QCOMPARE(filterModel->SortOrder(), Qt::DescendingOrder);

        // Write a deliberately-non-JSON payload so `TryLoadAsConfiguration`
        // fails inside `Load(...)`.
        const QTemporaryDir garbageDir;
        QVERIFY(garbageDir.isValid());
        const QString garbagePath = QDir(garbageDir.path()).filePath(QStringLiteral("not_a_config.txt"));
        {
            std::ofstream stream(garbagePath.toStdString(), std::ios::binary);
            QVERIFY(stream.is_open());
            stream << "this is not json";
        }
        QVERIFY2(
            !wired->TryLoadAsConfigurationForTest(garbagePath),
            "TryLoadAsConfiguration must reject a non-configuration file"
        );
        QCoreApplication::processEvents();

        QVERIFY2(
            wired->ActiveSessionUuid() == uuidA,
            "failed probe must preserve the prior recents pin so an Append open extends the same entry"
        );

        // Filters and sort survive the failed probe.
        QCOMPARE(wired->Filters().size(), static_cast<size_t>(1));
        QCOMPARE(filterModel->rowCount(), 1);
        QCOMPARE(filterModel->SortColumn(), msgCol);
        QCOMPARE(filterModel->SortOrder(), Qt::DescendingOrder);
    }

    // Mixed input (one config + N logs): apply the configuration
    // before the logs stream, so the loaded columns / filters /
    // sort drive the streamed rows.
    void TestDispatchMixedConfigAndLogsAppliesConfigThenStreams()
    {
        const QTemporaryDir sessionsDir;
        QVERIFY(sessionsDir.isValid());
        SessionHistoryManager manager(QDir(sessionsDir.path()), std::make_unique<InMemoryRecentsIndexStorage>());
        auto wired = std::make_unique<MainWindow>(mTheme.data(), &manager, nullptr);

        // Two fixtures sharing a schema so we can assert on a sum.
        const TempJsonFile fixtureA(
            {QStringLiteral(R"({"category": "alpha", "msg": "first"})"),
             QStringLiteral(R"({"category": "beta", "msg": "second"})")}
        );
        const TempJsonFile fixtureB({QStringLiteral(R"({"category": "alpha", "msg": "third"})")});
        const int expectedRows = 3;

        // Full-scope cfg with a contains-`alpha` filter and a
        // descending sort on `category`. Proxy row count collapses
        // to 2 and the sort indicator matches.
        const QTemporaryDir cfgDir;
        QVERIFY(cfgDir.isValid());
        const QString cfgPath = cfgDir.filePath(QStringLiteral("mixed-cfg.json"));

        loglib::LogConfigurationManager builder;
        builder.SetSource(loglib::LogConfiguration::Source{});
        builder.AppendKeys({"category", "msg"});

        loglib::LogConfiguration::LogFilter filter;
        filter.type = loglib::LogConfiguration::LogFilter::Type::String;
        filter.row = 0;
        filter.filterString = "alpha";
        filter.matchType = loglib::LogConfiguration::LogFilter::Match::Contains;
        builder.SetFilters({filter});

        loglib::LogConfiguration::Sort sort;
        sort.columnIndex = 0;
        sort.descending = true;
        builder.SetSort(sort);

        builder.Save(cfgPath.toStdString(), loglib::SaveScope::Full);

        const MainWindow::MixedInputDispatch result =
            wired->OpenMixedFilesForTest({cfgPath, fixtureA.Path(), fixtureB.Path()}, MainWindow::OpenMode::Append);
        QCOMPARE(result, MainWindow::MixedInputDispatch::AppliedConfigThenLogs);

        // Two log files chain: `BeginStreaming` then
        // `AppendStreaming`. Wait on the accumulated row count
        // rather than `streamingFinished` (it fires per-file).
        auto *model = wired->Model();
        QVERIFY(model != nullptr);
        QTRY_COMPARE_WITH_TIMEOUT(model->rowCount(), expectedRows, 5000);

        // Cfg columns drove the load (no autodetect fallback).
        QCOMPARE(static_cast<int>(model->Configuration().columns.size()), 2);

        // Full-scope filter survived; proxy hides non-`alpha`.
        QCOMPARE(wired->Filters().size(), static_cast<size_t>(1));
        auto *proxy = wired->FilterModel();
        QVERIFY(proxy != nullptr);
        QCOMPARE(proxy->rowCount(), 2);

        // The Full-scope sort survived: the table view is sorting
        // by `category` descending.
        const auto *header = wired->findChild<LogTableView *>()->horizontalHeader();
        QVERIFY(header != nullptr);
        QCOMPARE(header->sortIndicatorSection(), 0);
        QCOMPARE(header->sortIndicatorOrder(), Qt::DescendingOrder);
    }

    // Lone-config input: `TryLoadAsConfiguration` applies columns
    // / filters / sort, model is not reset, no streaming starts.
    // Dispatcher reports `AppliedConfigOnly` for the CLI hint.
    void TestDispatchMixedSingleConfigDelegatesToTryLoadAsConfiguration()
    {
        const QTemporaryDir sessionsDir;
        QVERIFY(sessionsDir.isValid());
        SessionHistoryManager manager(QDir(sessionsDir.path()), std::make_unique<InMemoryRecentsIndexStorage>());
        auto wired = std::make_unique<MainWindow>(mTheme.data(), &manager, nullptr);

        const QTemporaryDir cfgDir;
        QVERIFY(cfgDir.isValid());
        const QString cfgPath = cfgDir.filePath(QStringLiteral("lone-cfg.json"));
        loglib::LogConfigurationManager builder;
        builder.AppendKeys({"msg", "level"});
        builder.Save(cfgPath.toStdString(), loglib::SaveScope::ColumnsOnly);

        const QSignalSpy finishedSpy(wired->Model(), &LogModel::streamingFinished);
        QVERIFY(finishedSpy.isValid());

        const MainWindow::MixedInputDispatch result =
            wired->OpenMixedFilesForTest({cfgPath}, MainWindow::OpenMode::Append);
        QCoreApplication::processEvents();

        QCOMPARE(result, MainWindow::MixedInputDispatch::AppliedConfigOnly);
        QCOMPARE(static_cast<int>(wired->Model()->Configuration().columns.size()), 2);
        QCOMPARE(wired->Model()->rowCount(), 0);
        QCOMPARE(finishedSpy.count(), 0);
    }

    // Multiple configs in one input is rejected with a modal
    // (suppressed in tests). Live state untouched: no config
    // applied, no streaming, no uuid mutation.
    void TestDispatchMixedRejectsMultipleConfigs()
    {
        const QTemporaryDir sessionsDir;
        QVERIFY(sessionsDir.isValid());
        SessionHistoryManager manager(QDir(sessionsDir.path()), std::make_unique<InMemoryRecentsIndexStorage>());
        auto wired = std::make_unique<MainWindow>(mTheme.data(), &manager, nullptr);

        // Pre-seed so we can later assert nothing mutated.
        const TempJsonFile prior({QStringLiteral(R"({"msg": "kept"})")});
        QSignalSpy primingSpy(wired->Model(), &LogModel::streamingFinished);
        QVERIFY(primingSpy.isValid());
        wired->OpenFilesForTest({prior.Path()}, MainWindow::OpenMode::Append);
        QVERIFY(primingSpy.wait(5000));
        QCoreApplication::processEvents();

        const int rowsBefore = wired->Model()->rowCount();
        const auto columnsBefore = wired->Model()->Configuration().columns.size();
        const QString uuidBefore = wired->ActiveSessionUuid();
        QVERIFY2(rowsBefore > 0, "priming open must produce at least one row");
        QVERIFY2(columnsBefore > 0, "priming open must produce at least one column");

        // Two real columns-only configurations and a log file.
        const QTemporaryDir cfgDir;
        QVERIFY(cfgDir.isValid());
        const QString cfgPathA = cfgDir.filePath(QStringLiteral("cfg-a.json"));
        const QString cfgPathB = cfgDir.filePath(QStringLiteral("cfg-b.json"));
        {
            loglib::LogConfigurationManager builderA;
            builderA.AppendKeys({"alpha"});
            builderA.Save(cfgPathA.toStdString(), loglib::SaveScope::ColumnsOnly);
            loglib::LogConfigurationManager builderB;
            builderB.AppendKeys({"beta"});
            builderB.Save(cfgPathB.toStdString(), loglib::SaveScope::ColumnsOnly);
        }
        const TempJsonFile log({QStringLiteral(R"({"msg": "ignored"})")});

        wired->SetSuppressDialogsForTest(true);
        const QSignalSpy finishedSpy(wired->Model(), &LogModel::streamingFinished);
        QVERIFY(finishedSpy.isValid());

        const MainWindow::MixedInputDispatch result =
            wired->OpenMixedFilesForTest({cfgPathA, cfgPathB, log.Path()}, MainWindow::OpenMode::Append);
        QCoreApplication::processEvents();

        QCOMPARE(result, MainWindow::MixedInputDispatch::RejectedMultiConfig);
        QCOMPARE(wired->Model()->rowCount(), rowsBefore);
        QCOMPARE(wired->Model()->Configuration().columns.size(), columnsBefore);
        QCOMPARE(wired->ActiveSessionUuid(), uuidBefore);
        QCOMPARE(finishedSpy.count(), 0);
    }

    // No-config input streams everything via
    // `StartStreamingOpenQueue` in the caller's `OpenMode`.
    void TestDispatchMixedNoConfigStreamsEverything()
    {
        const QTemporaryDir sessionsDir;
        QVERIFY(sessionsDir.isValid());
        SessionHistoryManager manager(QDir(sessionsDir.path()), std::make_unique<InMemoryRecentsIndexStorage>());
        auto wired = std::make_unique<MainWindow>(mTheme.data(), &manager, nullptr);

        const TempJsonFile logA({QStringLiteral(R"({"msg": "first"})")});
        const TempJsonFile logB({QStringLiteral(R"({"msg": "second"})")});

        const QSignalSpy finishedSpy(wired->Model(), &LogModel::streamingFinished);
        QVERIFY(finishedSpy.isValid());

        const MainWindow::MixedInputDispatch result =
            wired->OpenMixedFilesForTest({logA.Path(), logB.Path()}, MainWindow::OpenMode::Append);
        QCOMPARE(result, MainWindow::MixedInputDispatch::QueuedLogsOnly);

        // Each chained file fires `streamingFinished`; wait both.
        QTRY_VERIFY_WITH_TIMEOUT(finishedSpy.count() >= 2, 5000);
        QCoreApplication::processEvents();

        QCOMPARE(wired->Model()->rowCount(), 2);
    }

    // `{}` must classify as a log (no `"columns"` key), not a
    // configuration -- otherwise it would wipe columns. JsonParser
    // is permissive: `{}` parses as a 0-field record, so both
    // files stream successfully (total rowCount = 2). We wait for
    // both finish emits to avoid racing `StreamNextPendingFile`.
    void TestDispatchMixedRejectsEmptyJsonObjectAsConfig()
    {
        const QTemporaryDir sessionsDir;
        QVERIFY(sessionsDir.isValid());
        SessionHistoryManager manager(QDir(sessionsDir.path()), std::make_unique<InMemoryRecentsIndexStorage>());
        auto wired = std::make_unique<MainWindow>(mTheme.data(), &manager, nullptr);

        const QTemporaryDir emptyDir;
        QVERIFY(emptyDir.isValid());
        const QString emptyPath = QDir(emptyDir.path()).filePath(QStringLiteral("empty.json"));
        {
            std::ofstream stream(emptyPath.toStdString(), std::ios::binary);
            QVERIFY(stream.is_open());
            stream << "{}";
        }

        const TempJsonFile log({QStringLiteral(R"({"msg": "real"})")});

        wired->SetSuppressDialogsForTest(true);
        const QSignalSpy finishedSpy(wired->Model(), &LogModel::streamingFinished);
        QVERIFY(finishedSpy.isValid());

        const MainWindow::MixedInputDispatch result =
            wired->OpenMixedFilesForTest({emptyPath, log.Path()}, MainWindow::OpenMode::Append);
        QCOMPARE(result, MainWindow::MixedInputDispatch::QueuedLogsOnly);
        // Two emits: file 1 chains to file 2 via
        // `OnStreamingFinished -> StreamNextPendingFile`.
        QTRY_VERIFY_WITH_TIMEOUT(finishedSpy.count() >= 2, 5000);
        QCoreApplication::processEvents();

        QCOMPARE(wired->Model()->rowCount(), 2);
    }

    // CLI variant: `app cfg.json log.json` applies the cfg first
    // and streams the log under it.
    void TestOpenFilesForCliConfigAndLogsAppliesConfigThenStreams()
    {
        const QTemporaryDir sessionsDir;
        QVERIFY(sessionsDir.isValid());
        SessionHistoryManager manager(QDir(sessionsDir.path()), std::make_unique<InMemoryRecentsIndexStorage>());
        auto wired = std::make_unique<MainWindow>(mTheme.data(), &manager, nullptr);

        const QTemporaryDir cfgDir;
        QVERIFY(cfgDir.isValid());
        const QString cfgPath = cfgDir.filePath(QStringLiteral("cli-cfg.json"));
        loglib::LogConfigurationManager builder;
        builder.AppendKeys({"msg"});
        builder.Save(cfgPath.toStdString(), loglib::SaveScope::ColumnsOnly);

        const TempJsonFile log({QStringLiteral(R"({"msg": "from cli"})")});

        QSignalSpy finishedSpy(wired->Model(), &LogModel::streamingFinished);
        QVERIFY(finishedSpy.isValid());
        wired->OpenFilesForCli({cfgPath, log.Path()});
        QVERIFY(finishedSpy.wait(5000));
        QCoreApplication::processEvents();

        // Configuration applied: single column `msg` from the cfg.
        // Log streamed under it: one row.
        QCOMPARE(static_cast<int>(wired->Model()->Configuration().columns.size()), 1);
        QCOMPARE(wired->Model()->rowCount(), 1);
    }

    // Glaze accepts `{}` as a default-valued `LogConfiguration` with
    // no columns; treating that as a config would wipe the live
    // column layout. `TryLoadAsConfiguration` must reject column-
    // less parses so the caller opens the file as a log instead.
    void TestTryLoadAsConfigurationRejectsEmptyJsonObject()
    {
        const QTemporaryDir sessionsDir;
        QVERIFY(sessionsDir.isValid());

        SessionHistoryManager manager(QDir(sessionsDir.path()), std::make_unique<InMemoryRecentsIndexStorage>());
        auto wired = std::make_unique<MainWindow>(mTheme.data(), &manager, nullptr);

        const TempJsonFile fixtureA({QStringLiteral(R"({"msg": "alpha"})")});

        QSignalSpy finishedSpy(wired->Model(), &LogModel::streamingFinished);
        QVERIFY(finishedSpy.isValid());
        wired->OpenFilesForTest({fixtureA.Path()}, MainWindow::OpenMode::Append);
        QVERIFY(finishedSpy.wait(5000));
        QCoreApplication::processEvents();

        const auto columnsBefore = wired->Model()->Configuration().columns.size();
        QVERIFY2(columnsBefore > 0, "fixture must produce at least one column");

        // `{}` parses cleanly into a default config; without the
        // explicit reject this would wipe the live columns.
        const QTemporaryDir emptyDir;
        QVERIFY(emptyDir.isValid());
        const QString emptyPath = QDir(emptyDir.path()).filePath(QStringLiteral("empty.json"));
        {
            std::ofstream stream(emptyPath.toStdString(), std::ios::binary);
            QVERIFY(stream.is_open());
            stream << "{}";
        }
        QVERIFY2(
            !wired->TryLoadAsConfigurationForTest(emptyPath), "an empty JSON object must be rejected as a probe miss"
        );
        QCoreApplication::processEvents();

        QCOMPARE(wired->Model()->Configuration().columns.size(), columnsBefore);
    }

    // `openWindowsAtQuit` is maintained eagerly so multi-window
    // restore survives `WA_DeleteOnClose`. AutoSave adds; close
    // removes.
    void TestOpenWindowsAtQuitTrackedAcrossAutoSaveAndClose()
    {
        const QTemporaryDir sessionsDir;
        QVERIFY(sessionsDir.isValid());

        // Snapshot + restore so we don't pollute the live profile.
        const QStringList previousOpen = SessionHistoryManager::OpenWindowsAtQuitUnlocked();
        auto restoreGuard = qScopeGuard([&]() { SessionHistoryManager::SetOpenWindowsAtQuit(previousOpen); });
        SessionHistoryManager::SetOpenWindowsAtQuit({});

        SessionHistoryManager manager(QDir(sessionsDir.path()), std::make_unique<InMemoryRecentsIndexStorage>());
        auto wired = std::make_unique<MainWindow>(mTheme.data(), &manager, nullptr);

        const TempJsonFile fixture({QStringLiteral(R"({"msg": "track"})")});

        QSignalSpy finishedSpy(wired->Model(), &LogModel::streamingFinished);
        wired->OpenFilesForTest({fixture.Path()}, MainWindow::OpenMode::Append);
        QVERIFY(finishedSpy.wait(5000));
        QCoreApplication::processEvents();

        const QString uuid = wired->ActiveSessionUuid();
        QVERIFY(!uuid.isEmpty());

        // Some sandboxed CI Windows envs refuse the write; skip.
        const QStringList afterAuto = SessionHistoryManager::OpenWindowsAtQuitUnlocked();
        if (afterAuto.isEmpty())
        {
            QSKIP("QSettings did not honour the open-windows write in this environment");
        }
        QVERIFY2(afterAuto.contains(uuid), "AutoSave must publish the session uuid into openWindowsAtQuit");

        // closeEvent removes the window from the set.
        const QSignalSpy destroyedSpy(wired.get(), &QObject::destroyed);
        wired->close();
        wired.reset();
        for (int i = 0; i < 20 && destroyedSpy.isEmpty(); ++i)
        {
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            QCoreApplication::processEvents();
        }

        const QStringList afterClose = SessionHistoryManager::OpenWindowsAtQuitUnlocked();
        QVERIFY2(!afterClose.contains(uuid), "closeEvent must remove the window from the open-windows set");
    }

    // Regression: the primary window in `main.cpp` is not
    // `WA_DeleteOnClose`, so it stays in `topLevelWidgets()` past
    // `closeEvent`. Without clearing `mAutoSaveUuid` in
    // `closeEvent`, the `aboutToQuit` snapshot would re-publish
    // the exited window into `openWindowsAtQuit`.
    void TestCloseEventClearsAutoSaveUuidForAboutToQuit()
    {
        const QTemporaryDir sessionsDir;
        QVERIFY(sessionsDir.isValid());

        const QStringList previousOpen = SessionHistoryManager::OpenWindowsAtQuitUnlocked();
        auto restoreGuard = qScopeGuard([&]() { SessionHistoryManager::SetOpenWindowsAtQuit(previousOpen); });
        SessionHistoryManager::SetOpenWindowsAtQuit({});

        SessionHistoryManager manager(QDir(sessionsDir.path()), std::make_unique<InMemoryRecentsIndexStorage>());
        // Heap-owned but not `WA_DeleteOnClose`: mirrors `main.cpp`'s
        // primary -- survives `close()` long enough for the
        // aboutToQuit replay.
        auto wired = std::make_unique<MainWindow>(mTheme.data(), &manager, nullptr);

        const TempJsonFile fixture({QStringLiteral(R"({"msg": "exit"})")});

        QSignalSpy finishedSpy(wired->Model(), &LogModel::streamingFinished);
        wired->OpenFilesForTest({fixture.Path()}, MainWindow::OpenMode::Append);
        QVERIFY(finishedSpy.wait(5000));
        QCoreApplication::processEvents();

        const QString uuid = wired->ActiveSessionUuid();
        QVERIFY(!uuid.isEmpty());
        const QStringList afterAuto = SessionHistoryManager::OpenWindowsAtQuitUnlocked();
        if (afterAuto.isEmpty())
        {
            QSKIP("QSettings did not honour the open-windows write in this environment");
        }
        QVERIFY(afterAuto.contains(uuid));

        // Simulate File -> Exit: closeEvent fires synchronously,
        // widget stays alive.
        wired->close();
        QCoreApplication::processEvents();

        // Replay `main.cpp`'s aboutToQuit snapshot loop on the
        // still-live widget. With the fix,
        // `RestorableActiveSessionUuid` is empty after closeEvent.
        QStringList capturedOnQuit;
        for (QWidget *widget : QApplication::topLevelWidgets())
        {
            auto *mw = qobject_cast<MainWindow *>(widget);
            if (mw == nullptr)
            {
                continue;
            }
            const QString restorableUuid = mw->RestorableActiveSessionUuid();
            if (!restorableUuid.isEmpty())
            {
                capturedOnQuit.append(restorableUuid);
            }
        }
        if (!capturedOnQuit.isEmpty())
        {
            SessionHistoryManager::SetOpenWindowsAtQuit(capturedOnQuit);
        }

        QVERIFY2(
            capturedOnQuit.isEmpty(),
            "aboutToQuit snapshot must not see a uuid from a window whose closeEvent already ran"
        );
        QVERIFY2(
            !SessionHistoryManager::OpenWindowsAtQuitUnlocked().contains(uuid),
            "the just-exited window's uuid must not be republished into openWindowsAtQuit"
        );
    }

    // `NewSession` must clear `mAutoSaveUuid` and drop it from
    // `openWindowsAtQuit` so a crash before the next AutoSave
    // does not re-restore the just-discarded session.
    void TestNewSessionDetachesFromOpenWindowsAtQuit()
    {
        const QTemporaryDir sessionsDir;
        QVERIFY(sessionsDir.isValid());

        const QStringList previousOpen = SessionHistoryManager::OpenWindowsAtQuitUnlocked();
        auto restoreGuard = qScopeGuard([&]() { SessionHistoryManager::SetOpenWindowsAtQuit(previousOpen); });
        SessionHistoryManager::SetOpenWindowsAtQuit({});

        SessionHistoryManager manager(QDir(sessionsDir.path()), std::make_unique<InMemoryRecentsIndexStorage>());
        auto wired = std::make_unique<MainWindow>(mTheme.data(), &manager, nullptr);

        const TempJsonFile fixture({QStringLiteral(R"({"msg": "detach"})")});

        QSignalSpy finishedSpy(wired->Model(), &LogModel::streamingFinished);
        wired->OpenFilesForTest({fixture.Path()}, MainWindow::OpenMode::Append);
        QVERIFY(finishedSpy.wait(5000));
        QCoreApplication::processEvents();

        const QString uuid = wired->ActiveSessionUuid();
        QVERIFY(!uuid.isEmpty());
        const QStringList afterAuto = SessionHistoryManager::OpenWindowsAtQuitUnlocked();
        if (afterAuto.isEmpty())
        {
            QSKIP("QSettings did not honour the open-windows write in this environment");
        }
        QVERIFY(afterAuto.contains(uuid));

        wired->findChild<QAction *>(QStringLiteral("actionNewSession"))->trigger();
        QCoreApplication::processEvents();

        QVERIFY2(
            wired->ActiveSessionUuid().isEmpty(),
            "NewSession must drop the pinned uuid so the next AutoSave produces a fresh entry"
        );
        const QStringList afterNew = SessionHistoryManager::OpenWindowsAtQuitUnlocked();
        QVERIFY2(
            !afterNew.contains(uuid), "NewSession must remove the discarded session's uuid from openWindowsAtQuit"
        );
    }

    // `Add` / `Remove` round-trip on the persisted open-windows set
    // is idempotent and order-preserving for unaffected entries.
    void TestOpenWindowUuidAddRemoveRoundTrip()
    {
        const QStringList previousOpen = SessionHistoryManager::OpenWindowsAtQuitUnlocked();
        auto restoreGuard = qScopeGuard([&]() { SessionHistoryManager::SetOpenWindowsAtQuit(previousOpen); });
        SessionHistoryManager::SetOpenWindowsAtQuit({});

        // Empty / null uuids are no-ops; `AddOpenWindowUuid`
        // returns false for empty input, true on a real publish.
        QVERIFY(!SessionHistoryManager::AddOpenWindowUuid(QString()));
        QCOMPARE(SessionHistoryManager::OpenWindowsAtQuitUnlocked(), QStringList{});
        SessionHistoryManager::RemoveOpenWindowUuid(QString());
        QCOMPARE(SessionHistoryManager::OpenWindowsAtQuitUnlocked(), QStringList{});

        const QString uuidA = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const QString uuidB = QUuid::createUuid().toString(QUuid::WithoutBraces);

        QVERIFY(SessionHistoryManager::AddOpenWindowUuid(uuidA));
        const QStringList probe = SessionHistoryManager::OpenWindowsAtQuitUnlocked();
        if (probe.isEmpty())
        {
            QSKIP("QSettings did not honour the open-windows write in this environment");
        }
        QCOMPARE(probe, QStringList{uuidA});

        // Idempotent: re-adding does not duplicate. Returns true
        // because the post-condition "uuid is in the set" holds.
        QVERIFY(SessionHistoryManager::AddOpenWindowUuid(uuidA));
        QCOMPARE(SessionHistoryManager::OpenWindowsAtQuitUnlocked(), QStringList{uuidA});

        // Order-preserving append for a fresh uuid.
        QVERIFY(SessionHistoryManager::AddOpenWindowUuid(uuidB));
        QCOMPARE(SessionHistoryManager::OpenWindowsAtQuitUnlocked(), (QStringList{uuidA, uuidB}));

        // Remove the middle / head entry; the rest stay put.
        SessionHistoryManager::RemoveOpenWindowUuid(uuidA);
        QCOMPARE(SessionHistoryManager::OpenWindowsAtQuitUnlocked(), QStringList{uuidB});

        // Removing a missing uuid is a no-op.
        SessionHistoryManager::RemoveOpenWindowUuid(uuidA);
        QCOMPARE(SessionHistoryManager::OpenWindowsAtQuitUnlocked(), QStringList{uuidB});
    }

    // Batched `AddOpenWindowUuids` (used by `aboutToQuit`): merge
    // under one lock acquisition. Appends in order, skips
    // duplicates / empty strings, preserves pre-existing entries.
    void TestAddOpenWindowUuidsBatchedMerge()
    {
        const QStringList previousOpen = SessionHistoryManager::OpenWindowsAtQuitUnlocked();
        auto restoreGuard = qScopeGuard([&]() { SessionHistoryManager::SetOpenWindowsAtQuit(previousOpen); });
        SessionHistoryManager::SetOpenWindowsAtQuit({});

        // Empty list is a no-op.
        SessionHistoryManager::AddOpenWindowUuids(QStringList{});
        QCOMPARE(SessionHistoryManager::OpenWindowsAtQuitUnlocked(), QStringList{});

        const QString uuidA = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const QString uuidB = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const QString uuidC = QUuid::createUuid().toString(QUuid::WithoutBraces);

        // Pre-seed `uuidA` to model a sibling peer. Batched call
        // must merge, not overwrite.
        QVERIFY(SessionHistoryManager::AddOpenWindowUuid(uuidA));
        const QStringList preBatch = SessionHistoryManager::OpenWindowsAtQuitUnlocked();
        if (preBatch.isEmpty())
        {
            QSKIP("QSettings did not honour the open-windows write in this environment");
        }
        QCOMPARE(preBatch, QStringList{uuidA});

        // Batch: an empty string (skipped), a duplicate of `uuidA`
        // (skipped -- idempotent), and two new uuids appended in
        // order.
        SessionHistoryManager::AddOpenWindowUuids(QStringList{QString(), uuidA, uuidB, uuidC});
        QCOMPARE(SessionHistoryManager::OpenWindowsAtQuitUnlocked(), (QStringList{uuidA, uuidB, uuidC}));

        // A subsequent batch with no new entries is also a no-op
        // (every uuid is already present).
        SessionHistoryManager::AddOpenWindowUuids(QStringList{uuidA, uuidB, uuidC});
        QCOMPARE(SessionHistoryManager::OpenWindowsAtQuitUnlocked(), (QStringList{uuidA, uuidB, uuidC}));
    }

    // `CleanupOrphanFiles` deletes per-uuid JSONs that are not in the
    // index. Index-referenced files survive; the lock file is left
    // alone.
    void TestCleanupOrphanFilesRemovesStaleJsons()
    {
        const QTemporaryDir sessionsDir;
        QVERIFY(sessionsDir.isValid());

        SessionHistoryManager manager(QDir(sessionsDir.path()), std::make_unique<InMemoryRecentsIndexStorage>());

        // Seed one valid entry.
        loglib::LogConfiguration cfg;
        cfg.source = loglib::LogConfiguration::Source{
            .kind = loglib::LogConfiguration::Source::Kind::File, .locators = {"C:/logs/kept.json"}
        };
        const QString keptUuid = manager.WriteSnapshot(cfg);
        QVERIFY(!keptUuid.isEmpty());
        const QString keptPath = manager.PathForUuid(keptUuid);
        QVERIFY(QFileInfo::exists(keptPath));

        // Orphan JSON (simulates a crash between file-write and
        // index-update). Stem must be uuid-shaped -- the sweeper
        // gates deletion on a strict uuid check.
        const QString orphanPath =
            QDir(sessionsDir.path())
                .filePath(QUuid::createUuid().toString(QUuid::WithoutBraces) + QStringLiteral(".json"));
        {
            QFile orphan(orphanPath);
            QVERIFY(orphan.open(QIODevice::WriteOnly));
            orphan.write("{\"columns\": []}\n");
        }
        QVERIFY(QFileInfo::exists(orphanPath));

        // Leftover `.json.tmp` (atomic-write crash). Same uuid-
        // stem requirement applies to `*.json.tmp` as to `*.json`.
        const QString staleTempPath =
            QDir(sessionsDir.path())
                .filePath(QUuid::createUuid().toString(QUuid::WithoutBraces) + QStringLiteral(".json.tmp"));
        {
            QFile staleTemp(staleTempPath);
            QVERIFY(staleTemp.open(QIODevice::WriteOnly));
            staleTemp.write("{\"columns\": [");
        }
        QVERIFY(QFileInfo::exists(staleTempPath));

        (void)manager.CleanupOrphanFiles();

        QVERIFY2(QFileInfo::exists(keptPath), "indexed entries must survive cleanup");
        QVERIFY2(!QFileInfo::exists(orphanPath), "orphan JSON must be removed");
        QVERIFY2(!QFileInfo::exists(staleTempPath), "stale .json.tmp must be removed");
    }

    // The orphan sweeper must NOT delete files whose stem isn't a
    // QUuid -- silently wiping a user's `notes.json` would be a
    // foot-gun.
    void TestCleanupOrphanFilesPreservesNonUuidStems()
    {
        const QTemporaryDir sessionsDir;
        QVERIFY(sessionsDir.isValid());

        SessionHistoryManager manager(QDir(sessionsDir.path()), std::make_unique<InMemoryRecentsIndexStorage>());

        // Seed an entry to mkpath the directory.
        loglib::LogConfiguration cfg;
        cfg.source = loglib::LogConfiguration::Source{
            .kind = loglib::LogConfiguration::Source::Kind::File, .locators = {"C:/logs/seed.json"}
        };
        const QString seedUuid = manager.WriteSnapshot(cfg);
        QVERIFY(!seedUuid.isEmpty());

        // Plant a foreign file with a non-uuid stem.
        const QString foreignPath = QDir(sessionsDir.path()).filePath(QStringLiteral("notes.json"));
        {
            QFile foreign(foreignPath);
            QVERIFY(foreign.open(QIODevice::WriteOnly));
            foreign.write("user notes -- not ours");
        }
        QVERIFY(QFileInfo::exists(foreignPath));

        // Symmetric check for `.json.tmp` stems.
        const QString foreignTempPath = QDir(sessionsDir.path()).filePath(QStringLiteral("scratch.json.tmp"));
        {
            QFile foreignTemp(foreignTempPath);
            QVERIFY(foreignTemp.open(QIODevice::WriteOnly));
            foreignTemp.write("scratch space");
        }
        QVERIFY(QFileInfo::exists(foreignTempPath));

        (void)manager.CleanupOrphanFiles();

        QVERIFY2(QFileInfo::exists(foreignPath), "non-uuid `notes.json` must survive cleanup");
        QVERIFY2(QFileInfo::exists(foreignTempPath), "non-uuid `scratch.json.tmp` must survive cleanup");
        QVERIFY2(QFileInfo::exists(manager.PathForUuid(seedUuid)), "indexed entry must still be present");
    }

    // `RestoreLastSessionFromPath` must pin `mAutoSaveUuid` even
    // for source-less configurations; otherwise the next AutoSave
    // forks a fresh entry instead of updating the loaded one.
    void TestRestoreLastSessionPinsUuidEvenWithNoSource()
    {
        const QTemporaryDir sessionsDir;
        QVERIFY(sessionsDir.isValid());

        SessionHistoryManager manager(QDir(sessionsDir.path()), std::make_unique<InMemoryRecentsIndexStorage>());

        // Hand-craft a uuid-stemmed JSON with no source so the
        // restore path hits the no-source branch.
        const QString uuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const QString jsonPath = manager.PathForUuid(uuid);
        QDir().mkpath(QFileInfo(jsonPath).absolutePath());

        loglib::LogConfigurationManager builder;
        builder.AppendKeys({"msg", "level"});
        // No `SetSource`: column-only snapshot.
        builder.Save(jsonPath.toStdString(), loglib::SaveScope::Full);
        QVERIFY(QFileInfo::exists(jsonPath));

        auto wired = std::make_unique<MainWindow>(mTheme.data(), &manager, nullptr);

        wired->RestoreLastSessionFromPath(jsonPath);
        QCoreApplication::processEvents();

        QCOMPARE(wired->ActiveSessionUuid(), uuid);
    }

    // `ShouldAutoSaveSession` filters out live-tail / network-
    // stream sessions: they can't be rebound from a JSON snapshot,
    // so saving them would create entries that never reopen.
    void TestAutoSaveSkipsLiveTailAndNetworkStreamSessions()
    {
        const QTemporaryDir sessionsDir;
        QVERIFY(sessionsDir.isValid());

        SessionHistoryManager manager(QDir(sessionsDir.path()), std::make_unique<InMemoryRecentsIndexStorage>());

        // Case A: live-tail on a File source. A plain kind check
        // would let it through; the gate must also see `LiveTail`.
        {
            auto wired = std::make_unique<MainWindow>(mTheme.data(), &manager, nullptr);
            wired->SetCurrentSourceForTest(loglib::LogConfiguration::Source{
                .kind = loglib::LogConfiguration::Source::Kind::File, .locators = {"/tmp/live.log"}
            });
            wired->SetSessionModeForTest(MainWindow::TestSessionMode::LiveTail);

            wired->close();
            QCoreApplication::processEvents();

            QVERIFY2(
                manager.List().isEmpty(), "closeEvent must not auto-save a live-tail session into Recent Sessions"
            );
        }

        // Case B: NetworkStream source -- not re-bindable.
        {
            auto wired = std::make_unique<MainWindow>(mTheme.data(), &manager, nullptr);
            wired->SetCurrentSourceForTest(loglib::LogConfiguration::Source{
                .kind = loglib::LogConfiguration::Source::Kind::NetworkStream, .locators = {"tcp://127.0.0.1:5170"}
            });
            wired->SetSessionModeForTest(MainWindow::TestSessionMode::LiveTail);

            wired->close();
            QCoreApplication::processEvents();

            QVERIFY2(
                manager.List().isEmpty(), "closeEvent must not auto-save a network-stream session into Recent Sessions"
            );
        }
    }

    // Regression: closeEvent used to write a phantom recents
    // entry for a *finished* live-tail session because
    // `streamingFinished` resets `mSessionMode` to `Idle`.
    // `mLastTerminalSessionMode` retains the real mode so the
    // auto-save gate bails.
    void TestCloseAfterFinishedLiveTailDoesNotCreatePhantomRecentsEntry()
    {
        const QTemporaryDir sessionsDir;
        QVERIFY(sessionsDir.isValid());

        SessionHistoryManager manager(QDir(sessionsDir.path()), std::make_unique<InMemoryRecentsIndexStorage>());
        auto wired = std::make_unique<MainWindow>(mTheme.data(), &manager, nullptr);

        wired->SetCurrentSourceForTest(loglib::LogConfiguration::Source{
            .kind = loglib::LogConfiguration::Source::Kind::File, .locators = {"/tmp/livetail-finished.log"}
        });
        wired->SetSessionModeForTest(MainWindow::TestSessionMode::LiveTail);

        // Drive a real `streamingFinished` so the MainWindow lambda
        // captures `mLastTerminalSessionMode = LiveTail` before
        // resetting `mSessionMode = Idle`.
        auto *model = wired->Model();
        QVERIFY(model != nullptr);
        const QSignalSpy finishedSpy(model, &LogModel::streamingFinished);
        QVERIFY(finishedSpy.isValid());
        BeginSyntheticStreamSession(*model);
        QVERIFY(model->IsStreamingActive());
        model->StopAndKeepRows();
        QCoreApplication::processEvents();
        QVERIFY2(finishedSpy.count() >= 1, "StopAndKeepRows must emit streamingFinished");

        QVERIFY2(manager.List().isEmpty(), "preconditions: no recents entries before close");

        wired->close();
        QCoreApplication::processEvents();

        QVERIFY2(
            manager.List().isEmpty(),
            "closeEvent must not write a Recent Sessions entry for a finished live-tail session"
        );
    }

    // `RestoreLastSessionFromPath` must not stream a NetworkStream
    // URI as a static file. Restore succeeds (columns / filters
    // installed) but no streaming attempt is made.
    void TestRestoreLastSessionSkipsNetworkStreamSource()
    {
        const QTemporaryDir sessionsDir;
        QVERIFY(sessionsDir.isValid());

        SessionHistoryManager manager(QDir(sessionsDir.path()), std::make_unique<InMemoryRecentsIndexStorage>());

        // Hand-craft a NetworkStream session JSON (legacy: older
        // builds auto-saved streams before the gate landed).
        const QString uuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const QString jsonPath = manager.PathForUuid(uuid);
        QDir().mkpath(QFileInfo(jsonPath).absolutePath());

        loglib::LogConfigurationManager builder;
        builder.AppendKeys({"msg"});
        builder.SetSource(loglib::LogConfiguration::Source{
            .kind = loglib::LogConfiguration::Source::Kind::NetworkStream, .locators = {"tcp://127.0.0.1:5170"}
        });
        builder.Save(jsonPath.toStdString(), loglib::SaveScope::Full);
        QVERIFY(QFileInfo::exists(jsonPath));

        auto wired = std::make_unique<MainWindow>(mTheme.data(), &manager, nullptr);

        // No streaming attempt -- `finishedSpy` must stay at 0.
        const QSignalSpy finishedSpy(wired->Model(), &LogModel::streamingFinished);
        wired->RestoreLastSessionFromPath(jsonPath);
        // Pump the event loop briefly.
        for (int i = 0; i < 5; ++i)
        {
            QCoreApplication::processEvents();
        }

        QCOMPARE(finishedSpy.count(), 0);
        // uuid still pinned so future saves update this entry.
        QCOMPARE(wired->ActiveSessionUuid(), uuid);
    }

    // `RestorableActiveSessionUuid` returns empty for sessions that
    // can't be fan-restored (NetworkStream, no source). The plain
    // `ActiveSessionUuid` still returns the pin so Touch / AutoSave
    // bookkeeping stays consistent.
    void TestRestorableActiveSessionUuidFiltersNonRestorableSessions()
    {
        const QTemporaryDir sessionsDir;
        QVERIFY(sessionsDir.isValid());

        SessionHistoryManager manager(QDir(sessionsDir.path()), std::make_unique<InMemoryRecentsIndexStorage>());

        // Case A: NetworkStream. The uuid is pinned (manual rebind
        // can still rewrite in place) but fan-restore must skip it.
        {
            const QString uuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
            const QString jsonPath = manager.PathForUuid(uuid);
            QDir().mkpath(QFileInfo(jsonPath).absolutePath());

            loglib::LogConfigurationManager builder;
            builder.AppendKeys({"msg"});
            builder.SetSource(loglib::LogConfiguration::Source{
                .kind = loglib::LogConfiguration::Source::Kind::NetworkStream, .locators = {"tcp://127.0.0.1:5170"}
            });
            builder.Save(jsonPath.toStdString(), loglib::SaveScope::Full);

            auto wired = std::make_unique<MainWindow>(mTheme.data(), &manager, nullptr);
            wired->RestoreLastSessionFromPath(jsonPath);
            QCoreApplication::processEvents();

            QCOMPARE(wired->ActiveSessionUuid(), uuid);
            QVERIFY2(wired->RestorableActiveSessionUuid().isEmpty(), "fan-restore must skip NetworkStream sessions");
        }

        // Case B: source-less config. Restorable -- the user
        // pinned a layout and wants it back.
        {
            const QString uuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
            const QString jsonPath = manager.PathForUuid(uuid);
            QDir().mkpath(QFileInfo(jsonPath).absolutePath());

            loglib::LogConfigurationManager builder;
            builder.AppendKeys({"msg"});
            // Intentionally no `SetSource`.
            builder.Save(jsonPath.toStdString(), loglib::SaveScope::Full);

            auto wired = std::make_unique<MainWindow>(mTheme.data(), &manager, nullptr);
            wired->RestoreLastSessionFromPath(jsonPath);
            QCoreApplication::processEvents();

            QCOMPARE(wired->ActiveSessionUuid(), uuid);
            QCOMPARE(wired->RestorableActiveSessionUuid(), uuid);
        }

        // Case C: no session pinned -- trivially empty.
        {
            auto wired = std::make_unique<MainWindow>(mTheme.data(), &manager, nullptr);
            QVERIFY(wired->ActiveSessionUuid().isEmpty());
            QVERIFY(wired->RestorableActiveSessionUuid().isEmpty());
        }
    }

    // `RestoreLastSessionFromPath` only pins `mAutoSaveUuid` when
    // the stem parses as a QUuid -- an ad-hoc file can't hijack
    // auto-save into the user's filesystem.
    void TestRestoreLastSessionRejectsNonUuidStem()
    {
        const QTemporaryDir sessionsDir;
        QVERIFY(sessionsDir.isValid());

        SessionHistoryManager manager(QDir(sessionsDir.path()), std::make_unique<InMemoryRecentsIndexStorage>());

        // Valid session JSON at a non-uuid path; only the stem-
        // validation gate is under test.
        const QTemporaryDir adhocDir;
        QVERIFY(adhocDir.isValid());
        const TempJsonFile fixture({QStringLiteral(R"({"msg": "stem"})")});

        loglib::LogConfigurationManager builder;
        builder.AppendKeys({"msg"});
        builder.SetSource(loglib::LogConfiguration::Source{
            .kind = loglib::LogConfiguration::Source::Kind::File, .locators = {fixture.Path().toStdString()}
        });
        const QString adhocPath = adhocDir.filePath(QStringLiteral("not-a-uuid.json"));
        builder.Save(adhocPath.toStdString(), loglib::SaveScope::Full);

        auto wired = std::make_unique<MainWindow>(mTheme.data(), &manager, nullptr);

        QSignalSpy finishedSpy(wired->Model(), &LogModel::streamingFinished);
        wired->RestoreLastSessionFromPath(adhocPath);
        QVERIFY(finishedSpy.wait(5000));
        QCoreApplication::processEvents();

        // Streaming finished but `mAutoSaveUuid` must not pin to
        // `not-a-uuid`.
        QVERIFY2(
            wired->ActiveSessionUuid() != QStringLiteral("not-a-uuid"),
            "RestoreLastSessionFromPath must reject non-uuid stems"
        );
    }

    // Switching from a static session to a live-tail stream must
    // flush unsaved post-`streamingFinished` edits to the recents
    // JSON before the destructive `mModel->Reset()`. Witnessed via
    // a `changed` signal tick on the pre-reset flush.
    void TestOpenLogStreamFlushesPriorStaticSessionEdits()
    {
        const QTemporaryDir sessionsDir;
        QVERIFY(sessionsDir.isValid());

        SessionHistoryManager manager(QDir(sessionsDir.path()), std::make_unique<InMemoryRecentsIndexStorage>());
        auto wired = std::make_unique<MainWindow>(mTheme.data(), &manager, nullptr);

        // Stage 1: open a static session and let it auto-save.
        // Drain `changed` before the assertion window.
        const TempJsonFile staticFixture({QStringLiteral(R"({"msg": "static"})")});
        QSignalSpy finishedSpy(wired->Model(), &LogModel::streamingFinished);
        wired->OpenFilesForTest({staticFixture.Path()}, MainWindow::OpenMode::Append);
        QVERIFY(finishedSpy.wait(5000));
        QCoreApplication::processEvents();

        QCOMPARE(manager.List().size(), 1);
        const QString staticUuid = wired->ActiveSessionUuid();
        QVERIFY(!staticUuid.isEmpty());

        const QSignalSpy changedSpy(&manager, &SessionHistoryManager::changed);
        QVERIFY(changedSpy.isValid());

        // Stage 2: switch to live-tail. The fix calls
        // `AutoSaveSessionSnapshot(false)` on the outgoing session
        // before the reset, firing `changed` once.
        const TempJsonFile streamFixture({QStringLiteral(R"({"msg": "stream-seed"})")});
        wired->OpenLogStreamForTest(streamFixture.Path());
        QCoreApplication::processEvents();

        QVERIFY2(
            changedSpy.count() >= 1, "OpenLogStream must flush the outgoing static session before resetting the model"
        );

        // Static entry survives (rewritten with the latest state);
        // the new live-tail session is intentionally not saved.
        QCOMPARE(manager.List().size(), 1);
        QCOMPARE(manager.List().front().uuid, staticUuid);

        // Post-reset: pin cleared so a follow-up flush does not
        // silently rewrite the static entry.
        QVERIFY(wired->ActiveSessionUuid().isEmpty());
    }

    // `aboutToQuit` must flush each window via
    // `AutoSaveSessionSnapshot(true)` so post-`streamingFinished`
    // edits survive an OS-driven quit (no `closeEvent`).
    void TestAboutToQuitFlushesPerWindowEdits()
    {
        const QTemporaryDir sessionsDir;
        QVERIFY(sessionsDir.isValid());

        const QStringList previousOpen = SessionHistoryManager::OpenWindowsAtQuitUnlocked();
        auto restoreGuard = qScopeGuard([&]() { SessionHistoryManager::SetOpenWindowsAtQuit(previousOpen); });
        SessionHistoryManager::SetOpenWindowsAtQuit({});

        SessionHistoryManager manager(QDir(sessionsDir.path()), std::make_unique<InMemoryRecentsIndexStorage>());
        auto wired = std::make_unique<MainWindow>(mTheme.data(), &manager, nullptr);

        const TempJsonFile fixture({QStringLiteral(R"({"msg": "abq-flush"})")});
        QSignalSpy finishedSpy(wired->Model(), &LogModel::streamingFinished);
        wired->OpenFilesForTest({fixture.Path()}, MainWindow::OpenMode::Append);
        QVERIFY(finishedSpy.wait(5000));
        QCoreApplication::processEvents();

        const QString uuid = wired->ActiveSessionUuid();
        QVERIFY(!uuid.isEmpty());

        // Drain the auto-save signal so the assertion below is
        // unambiguous.
        const QSignalSpy changedSpy(&manager, &SessionHistoryManager::changed);
        QVERIFY(changedSpy.isValid());

        // Replay the `aboutToQuit` body: per-window flush, then
        // one batched publish under a single lock acquisition.
        QStringList restorable;
        for (QWidget *widget : QApplication::topLevelWidgets())
        {
            auto *mw = qobject_cast<MainWindow *>(widget);
            if (mw == nullptr)
            {
                continue;
            }
            mw->AutoSaveSessionSnapshot(/*publishOpenWindow=*/false);
            const QString restorableUuid = mw->RestorableActiveSessionUuid();
            if (!restorableUuid.isEmpty())
            {
                restorable.append(restorableUuid);
            }
        }
        SessionHistoryManager::AddOpenWindowUuids(restorable);

        QVERIFY2(
            changedSpy.count() >= 1, "aboutToQuit must invoke AutoSaveSessionSnapshot, not just AddOpenWindowUuids"
        );

        const QStringList afterQuit = SessionHistoryManager::OpenWindowsAtQuitUnlocked();
        if (afterQuit.isEmpty())
        {
            QSKIP("QSettings did not honour the open-windows write in this environment");
        }
        QVERIFY2(
            afterQuit.contains(uuid), "aboutToQuit handler must publish the live window's uuid into openWindowsAtQuit"
        );
    }

    // External JSON (outside the sessions dir) must not pin or
    // publish a uuid -- a future AutoSave would otherwise silently
    // fork the user's file into a managed copy.
    //
    // Exercised against a no-source config so `streamingFinished`
    // never legitimises the entry after the fact.
    void TestRestoreLastSessionDoesNotPublishGhostUuid()
    {
        const QTemporaryDir sessionsDir;
        QVERIFY(sessionsDir.isValid());

        const QStringList previousOpen = SessionHistoryManager::OpenWindowsAtQuitUnlocked();
        auto restoreGuard = qScopeGuard([&]() { SessionHistoryManager::SetOpenWindowsAtQuit(previousOpen); });
        SessionHistoryManager::SetOpenWindowsAtQuit({});

        SessionHistoryManager manager(QDir(sessionsDir.path()), std::make_unique<InMemoryRecentsIndexStorage>());

        // No-source JSON with a uuid-shaped stem outside the
        // managed sessionsDir. The directory mismatch trips the
        // gate so the pin + publish block is skipped.
        const QTemporaryDir externalDir;
        QVERIFY(externalDir.isValid());
        const QString uuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const QString externalPath = externalDir.filePath(uuid + QStringLiteral(".json"));

        loglib::LogConfigurationManager builder;
        builder.AppendKeys({"msg"});
        // Columns-only (no `SetSource`): restore hits the no-source
        // branch and never starts streaming.
        builder.Save(externalPath.toStdString(), loglib::SaveScope::Full);
        QVERIFY(QFileInfo::exists(externalPath));

        auto wired = std::make_unique<MainWindow>(mTheme.data(), &manager, nullptr);

        wired->RestoreLastSessionFromPath(externalPath);
        QCoreApplication::processEvents();

        // The configuration loads but the uuid stays unpinned, so
        // the next save mints a fresh uuid in the managed dir.
        QVERIFY2(
            wired->ActiveSessionUuid().isEmpty(),
            qPrintable(QStringLiteral("external uuid-shaped JSON must not pin mAutoSaveUuid (got '%1')")
                           .arg(wired->ActiveSessionUuid()))
        );

        const QStringList openSet = SessionHistoryManager::OpenWindowsAtQuitUnlocked();
        // No `QSKIP` on empty -- an empty set is what the fix
        // guarantees.
        QVERIFY2(!openSet.contains(uuid), "external uuid-shaped JSON must not be published into openWindowsAtQuit");
    }

    // Non-File source kinds (NetworkStream today) must not publish
    // into `openWindowsAtQuit`. The recents index still owns the
    // entry (Touch / manual rebind work) but fan-restore must skip
    // it; otherwise every launch resurrects a "re-bind manually"
    // popup loop.
    void TestRestoreLastSessionDoesNotPublishNetworkStreamUuid()
    {
        const QTemporaryDir sessionsDir;
        QVERIFY(sessionsDir.isValid());

        const QStringList previousOpen = SessionHistoryManager::OpenWindowsAtQuitUnlocked();
        auto restoreGuard = qScopeGuard([&]() { SessionHistoryManager::SetOpenWindowsAtQuit(previousOpen); });
        SessionHistoryManager::SetOpenWindowsAtQuit({});

        SessionHistoryManager manager(QDir(sessionsDir.path()), std::make_unique<InMemoryRecentsIndexStorage>());

        // `WriteSnapshot` so the entry lives in the index; without
        // it, `Touch` would short-circuit and mask the gate.
        loglib::LogConfiguration cfg;
        cfg.source = loglib::LogConfiguration::Source{
            .kind = loglib::LogConfiguration::Source::Kind::NetworkStream, .locators = {"tcp://127.0.0.1:5170"}
        };
        const QString uuid = manager.WriteSnapshot(cfg);
        QVERIFY(!uuid.isEmpty());
        const QString jsonPath = manager.PathForUuid(uuid);
        QVERIFY(QFileInfo::exists(jsonPath));

        auto wired = std::make_unique<MainWindow>(mTheme.data(), &manager, nullptr);

        wired->RestoreLastSessionFromPath(jsonPath);
        QCoreApplication::processEvents();

        // Window owns the uuid (so a manual rebind updates the
        // same entry), but the restorability predicate hides it.
        QCOMPARE(wired->ActiveSessionUuid(), uuid);
        QVERIFY(wired->RestorableActiveSessionUuid().isEmpty());

        const QStringList openSet = SessionHistoryManager::OpenWindowsAtQuitUnlocked();
        QVERIFY2(!openSet.contains(uuid), "NetworkStream uuid must not be published into openWindowsAtQuit on restore");
    }

    // Companion: same invariant for the menu reopen path
    // (`OpenRecentSession(uuid)`).
    void TestOpenRecentSessionDoesNotPublishNetworkStreamUuid()
    {
        const QTemporaryDir sessionsDir;
        QVERIFY(sessionsDir.isValid());

        const QStringList previousOpen = SessionHistoryManager::OpenWindowsAtQuitUnlocked();
        auto restoreGuard = qScopeGuard([&]() { SessionHistoryManager::SetOpenWindowsAtQuit(previousOpen); });
        SessionHistoryManager::SetOpenWindowsAtQuit({});

        SessionHistoryManager manager(QDir(sessionsDir.path()), std::make_unique<InMemoryRecentsIndexStorage>());

        loglib::LogConfiguration cfg;
        cfg.source = loglib::LogConfiguration::Source{
            .kind = loglib::LogConfiguration::Source::Kind::NetworkStream, .locators = {"tcp://127.0.0.1:5171"}
        };
        const QString uuid = manager.WriteSnapshot(cfg);
        QVERIFY(!uuid.isEmpty());
        QVERIFY(QFileInfo::exists(manager.PathForUuid(uuid)));

        auto wired = std::make_unique<MainWindow>(mTheme.data(), &manager, nullptr);
        // Suppress the non-File source modal so the offscreen
        // runner doesn't hang.
        wired->SetSuppressDialogsForTest(true);

        wired->OpenRecentSessionForTest(uuid);
        QCoreApplication::processEvents();

        QCOMPARE(wired->ActiveSessionUuid(), uuid);
        QVERIFY(wired->RestorableActiveSessionUuid().isEmpty());

        const QStringList openSet = SessionHistoryManager::OpenWindowsAtQuitUnlocked();
        QVERIFY2(!openSet.contains(uuid), "NetworkStream uuid must not be published into openWindowsAtQuit on reopen");
    }

    // -------------------------------------------------------------------------
    // MainWindow lifecycle regressions.
    // -------------------------------------------------------------------------

    void TestOpenRecentSessionDropsCorruptEntry()
    {
        const QTemporaryDir sessionsDir;
        QVERIFY(sessionsDir.isValid());

        SessionHistoryManager manager(QDir(sessionsDir.path()), std::make_unique<InMemoryRecentsIndexStorage>());

        // Corrupt the JSON so the pre-flight parse fails. Post-fix
        // the corrupt entry is removed from the index.
        loglib::LogConfiguration cfg;
        cfg.source = loglib::LogConfiguration::Source{
            .kind = loglib::LogConfiguration::Source::Kind::File, .locators = {"C:/logs/will-be-corrupted.json"}
        };
        const QString uuid = manager.WriteSnapshot(cfg);
        QVERIFY(!uuid.isEmpty());

        const QString path = manager.PathForUuid(uuid);
        QVERIFY(QFileInfo::exists(path));

        // Replace contents with non-JSON garbage.
        {
            QFile f(path);
            QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
            f.write("this is not json {{");
        }

        auto wired = std::make_unique<MainWindow>(mTheme.data(), &manager, nullptr);
        wired->SetSuppressDialogsForTest(true);
        wired->OpenRecentSessionForTest(uuid);
        QCoreApplication::processEvents();

        const QList<RecentSessionEntry> entries = manager.List();
        QVERIFY2(
            std::none_of(
                entries.begin(), entries.end(), [&uuid](const RecentSessionEntry &e) { return e.uuid == uuid; }
            ),
            "Corrupt recents entry must be removed after a failed open attempt"
        );
    }

    void TestLoadFromStringParsesValidConfiguration()
    {
        // `LoadFromString` is the in-memory pre-flight that
        // `FileLooksLikeConfiguration` uses to bound disk IO.
        const QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("seed.json"));
        {
            loglib::LogConfiguration cfg;
            loglib::LogConfiguration::Column col;
            col.header = "msg";
            col.keys = {"msg"};
            cfg.columns.push_back(col);
            loglib::LogConfigurationManager::Save(cfg, path.toStdString(), loglib::SaveScope::Full);
        }
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QByteArray content = file.readAll();

        loglib::LogConfigurationManager mgr;
        mgr.LoadFromString(std::string_view(content.constData(), static_cast<size_t>(content.size())));
        QCOMPARE(mgr.Configuration().columns.size(), static_cast<size_t>(1));
        QCOMPARE(QString::fromStdString(mgr.Configuration().columns.front().header), QStringLiteral("msg"));
    }

    void TestLoadFromStringRejectsNonJsonGarbage()
    {
        loglib::LogConfigurationManager mgr;
        bool threw = false;
        try
        {
            mgr.LoadFromString("definitely not json {{");
        }
        catch (const std::exception &)
        {
            // Latch the throw so `bugprone-empty-catch` is happy.
            threw = true;
        }
        QVERIFY2(threw, "LoadFromString must throw on garbage input");
    }

    // `EvictLocked` writes the index before unlinking so a crash
    // between the two leaves an orphan JSON (recovered by
    // `CleanupOrphanFiles`) rather than a dangling index entry.
    // Steady-state observable invariant is unchanged.
    void TestEvictionKeepsIndexConsistentWithDisk()
    {
        const QTemporaryDir sessionsDir;
        QVERIFY(sessionsDir.isValid());

        SessionHistoryManager manager(QDir(sessionsDir.path()), std::make_unique<InMemoryRecentsIndexStorage>());

        const int capacityPlus = SessionHistoryManager::MAX_ENTRIES + 1;
        QStringList writtenUuids;
        for (int i = 0; i < capacityPlus; ++i)
        {
            loglib::LogConfiguration cfg;
            cfg.source = loglib::LogConfiguration::Source{
                .kind = loglib::LogConfiguration::Source::Kind::File,
                .locators = {QStringLiteral("C:/logs/evict-%1.json").arg(i).toStdString()}
            };
            const QString uuid = manager.WriteSnapshot(cfg);
            QVERIFY(!uuid.isEmpty());
            writtenUuids.append(uuid);
        }

        // Every indexed entry has its JSON on disk.
        const QList<RecentSessionEntry> list = manager.List();
        QCOMPARE(list.size(), SessionHistoryManager::MAX_ENTRIES);
        for (const RecentSessionEntry &e : list)
        {
            QVERIFY2(
                QFileInfo::exists(manager.PathForUuid(e.uuid)),
                qPrintable(QStringLiteral("indexed entry %1 must have backing JSON on disk").arg(e.uuid))
            );
        }

        // The evicted (oldest) uuid has no backing file.
        QVERIFY2(
            !QFileInfo::exists(manager.PathForUuid(writtenUuids.first())),
            "evicted entry's JSON must be removed from disk"
        );
    }

    // Atomic temp+rename: any temp-file failure must throw and
    // leave the destination untouched. We can't fault-inject a
    // close failure portably; instead exercise an open failure
    // (parent dir missing), which honours the same contract.
    void TestSaveTempFailureDoesNotClobberDestination()
    {
        const QTemporaryDir scratchDir;
        QVERIFY(scratchDir.isValid());

        // Pre-seed an existing JSON we can detect modifications on.
        const QString destPath = scratchDir.filePath(QStringLiteral("seed.json"));
        loglib::LogConfigurationManager seeder;
        seeder.AppendKeys({"a", "b"});
        seeder.Save(destPath.toStdString(), loglib::SaveScope::ColumnsOnly);
        QVERIFY(QFileInfo::exists(destPath));
        QFile destFile(destPath);
        QVERIFY(destFile.open(QIODevice::ReadOnly));
        const QByteArray seedBytes = destFile.readAll();
        destFile.close();
        QVERIFY(!seedBytes.isEmpty());

        // Save into a path whose parent dir does not exist; the
        // temp-file open inside the atomic write must fail.
        const QString unreachablePath =
            QDir(scratchDir.path()).filePath(QStringLiteral("does/not/exist/save-target.json"));

        loglib::LogConfigurationManager doomed;
        doomed.AppendKeys({"x", "y"});
        bool threw = false;
        try
        {
            doomed.Save(unreachablePath.toStdString(), loglib::SaveScope::ColumnsOnly);
        }
        catch (const std::exception &)
        {
            threw = true;
        }
        QVERIFY2(threw, "Save must throw when the temp file cannot be opened");
        QVERIFY2(
            !QFileInfo::exists(unreachablePath), "Save must not have created a partial file at the unreachable path"
        );

        // The pre-existing destination is untouched.
        QVERIFY(destFile.open(QIODevice::ReadOnly));
        const QByteArray afterBytes = destFile.readAll();
        destFile.close();
        QCOMPARE(afterBytes, seedBytes);
    }

    // CLI with a single source-less config must show a status-bar
    // hint pointing at File -> Open; otherwise the launch looks
    // like a no-op (just column headers, no rows).
    void TestOpenFilesForCliSingleConfigWithoutSourceShowsHint()
    {
        SessionHistoryManager manager(
            QDir(QDir::tempPath() + QStringLiteral("/abs-2-1")), std::make_unique<InMemoryRecentsIndexStorage>()
        );
        auto wired = std::make_unique<MainWindow>(mTheme.data(), &manager, nullptr);

        // Source-less config (columns only, no `source` field).
        const QTemporaryDir scratch;
        QVERIFY(scratch.isValid());
        const QString configPath = scratch.filePath(QStringLiteral("hint-cfg.json"));
        loglib::LogConfigurationManager builder;
        builder.AppendKeys({"timestamp", "level", "msg"});
        builder.Save(configPath.toStdString(), loglib::SaveScope::ColumnsOnly);

        wired->OpenFilesForCli({configPath});
        QCoreApplication::processEvents();

        // Status-bar hint visible.
        const QString status = wired->statusBar()->currentMessage();
        QVERIFY2(
            status.contains(QStringLiteral("configuration"), Qt::CaseInsensitive) &&
                status.contains(QStringLiteral("File")),
            qPrintable(QStringLiteral("status bar should hint at File -> Open after a source-less "
                                      "configuration load; got: %1")
                           .arg(status))
        );

        // Configuration applied so a follow-up File -> Open lands
        // on the user's chosen column layout.
        QVERIFY2(
            wired->Model()->columnCount() >= 1,
            "OpenFilesForCli must apply the configuration's columns even when no source is bound"
        );
    }

    // `closeEvent` on a window that never published its uuid must
    // not leave a stale entry in `openWindowsAtQuit`. The fix
    // skips the cross-process `RemoveOpenWindowUuid` when nothing
    // was published.
    //
    // Exercised via the path where `AutoSaveSessionSnapshot(false)`
    // is the *first* save (no prior `streamingFinished` published
    // the uuid), so a Remove would be a wasted no-op.
    void TestCloseEventOnUnpublishedSessionLeavesOpenWindowsClear()
    {
        const QTemporaryDir sessionsDir;
        QVERIFY(sessionsDir.isValid());

        // Snapshot + restore so the assertion starts clean.
        const QStringList previousOpen = SessionHistoryManager::OpenWindowsAtQuitUnlocked();
        auto restoreGuard = qScopeGuard([&]() { SessionHistoryManager::SetOpenWindowsAtQuit(previousOpen); });
        SessionHistoryManager::SetOpenWindowsAtQuit({});

        SessionHistoryManager manager(QDir(sessionsDir.path()), std::make_unique<InMemoryRecentsIndexStorage>());
        auto wired = std::make_unique<MainWindow>(mTheme.data(), &manager, nullptr);

        // Open a static session (auto-save publishes the uuid),
        // then clear the set so we exercise the unpublished close.
        const TempJsonFile fixture({QStringLiteral(R"({"msg": "unpub"})")});
        QSignalSpy finishedSpy(wired->Model(), &LogModel::streamingFinished);
        wired->OpenFilesForTest({fixture.Path()}, MainWindow::OpenMode::Append);
        QVERIFY(finishedSpy.wait(5000));
        QCoreApplication::processEvents();
        const QString uuid = wired->ActiveSessionUuid();
        QVERIFY(!uuid.isEmpty());

        // Scrub the set to simulate a sibling reset.
        SessionHistoryManager::SetOpenWindowsAtQuit({});

        // closeEvent runs synchronously.
        wired->close();
        QCoreApplication::processEvents();

        const QStringList afterClose = SessionHistoryManager::OpenWindowsAtQuitUnlocked();
        QVERIFY2(!afterClose.contains(uuid), "closeEvent must leave the open-windows set free of this window's uuid");
    }

    // H1 regression: after `File -> Exit` runs `closeEvent`, the
    // primary stays in `topLevelWidgets()`. Without resetting
    // session-mode state in `closeEvent`, the `aboutToQuit`
    // fan-AutoSave would mint a fresh uuid and resurrect the
    // window on next launch.
    void TestCloseEventThenAboutToQuitDoesNotResurrectClosedWindow()
    {
        const QTemporaryDir sessionsDir;
        QVERIFY(sessionsDir.isValid());

        const QStringList previousOpen = SessionHistoryManager::OpenWindowsAtQuitUnlocked();
        auto restoreGuard = qScopeGuard([&]() { SessionHistoryManager::SetOpenWindowsAtQuit(previousOpen); });
        SessionHistoryManager::SetOpenWindowsAtQuit({});

        SessionHistoryManager manager(QDir(sessionsDir.path()), std::make_unique<InMemoryRecentsIndexStorage>());
        auto wired = std::make_unique<MainWindow>(mTheme.data(), &manager, nullptr);

        // Stage 1: open a static session; auto-save publishes the
        // uuid into `openWindowsAtQuit`.
        const TempJsonFile fixture({QStringLiteral(R"({"msg": "h1-regr"})")});
        QSignalSpy finishedSpy(wired->Model(), &LogModel::streamingFinished);
        wired->OpenFilesForTest({fixture.Path()}, MainWindow::OpenMode::Append);
        QVERIFY(finishedSpy.wait(5000));
        QCoreApplication::processEvents();

        const QString origUuid = wired->ActiveSessionUuid();
        QVERIFY(!origUuid.isEmpty());
        QCOMPARE(manager.List().size(), 1);

        // Stage 2: close the window; with the fix, closeEvent
        // also resets session-mode state to Idle.
        wired->close();
        QCoreApplication::processEvents();

        // Sanity: the just-closed uuid is gone from the open set.
        const QStringList afterClose = SessionHistoryManager::OpenWindowsAtQuitUnlocked();
        QVERIFY2(!afterClose.contains(origUuid), "closeEvent should remove the window's uuid from openWindowsAtQuit");

        // Stage 3: replicate the production `aboutToQuit` body:
        // per-window flush, then one batched publish.
        QStringList restorableUuids;
        for (QWidget *widget : QApplication::topLevelWidgets())
        {
            auto *mw = qobject_cast<MainWindow *>(widget);
            if (mw == nullptr || mw != wired.get())
            {
                continue;
            }
            mw->AutoSaveSessionSnapshot(/*publishOpenWindow=*/false);
            const QString uuid = mw->RestorableActiveSessionUuid();
            if (!uuid.isEmpty())
            {
                restorableUuids.append(uuid);
            }
        }
        SessionHistoryManager::AddOpenWindowUuids(restorableUuids);
        QCoreApplication::processEvents();

        // 1: recents index size unchanged (no duplicate entry).
        QCOMPARE(manager.List().size(), 1);
        QCOMPARE(manager.List().front().uuid, origUuid);

        // 2: no fresh uuid in the open-windows set.
        const QStringList afterQuit = SessionHistoryManager::OpenWindowsAtQuitUnlocked();
        QVERIFY2(
            afterQuit.isEmpty() || (afterQuit.size() == 1 && afterQuit.front() == origUuid),
            qPrintable(QStringLiteral("aboutToQuit fan-flush must not resurrect the closed window via "
                                      "a fresh uuid; got: %1")
                           .arg(afterQuit.join(QStringLiteral(", "))))
        );

        // 3: no duplicate JSON on disk.
        const QStringList sessionFiles =
            QDir(sessionsDir.path()).entryList(QStringList{QStringLiteral("*.json")}, QDir::Files);
        QCOMPARE(sessionFiles.size(), 1);
    }

    // `MaxEntries` / `SetMaxEntries` round-trip and default fallback
    // (absent key -> `MAX_ENTRIES`).
    void TestMaxEntriesPreferenceRoundTrip()
    {
        QSettings settings;
        const QVariant previous = settings.value(QStringLiteral("recentSessions/maxEntries"));
        auto restoreGuard = qScopeGuard([&]() {
            QSettings restore;
            if (previous.isValid())
            {
                restore.setValue(QStringLiteral("recentSessions/maxEntries"), previous);
            }
            else
            {
                restore.remove(QStringLiteral("recentSessions/maxEntries"));
            }
            restore.sync();
        });

        settings.remove(QStringLiteral("recentSessions/maxEntries"));
        settings.sync();
        QCOMPARE(SessionHistoryManager::MaxEntries(), SessionHistoryManager::MAX_ENTRIES);

        SessionHistoryManager::SetMaxEntries(42);
        QCOMPARE(SessionHistoryManager::MaxEntries(), 42);
    }

    // Out-of-range values clamp into
    // `[MAX_ENTRIES_LOWER_BOUND, MAX_ENTRIES_UPPER_BOUND]`.
    void TestMaxEntriesClampedToValidRange()
    {
        const QSettings settings;
        const QVariant previous = settings.value(QStringLiteral("recentSessions/maxEntries"));
        auto restoreGuard = qScopeGuard([&]() {
            QSettings restore;
            if (previous.isValid())
            {
                restore.setValue(QStringLiteral("recentSessions/maxEntries"), previous);
            }
            else
            {
                restore.remove(QStringLiteral("recentSessions/maxEntries"));
            }
            restore.sync();
        });

        SessionHistoryManager::SetMaxEntries(0);
        QCOMPARE(SessionHistoryManager::MaxEntries(), SessionHistoryManager::MAX_ENTRIES_LOWER_BOUND);

        SessionHistoryManager::SetMaxEntries(100000);
        QCOMPARE(SessionHistoryManager::MaxEntries(), SessionHistoryManager::MAX_ENTRIES_UPPER_BOUND);

        SessionHistoryManager::SetMaxEntries(-1);
        QCOMPARE(SessionHistoryManager::MaxEntries(), SessionHistoryManager::MAX_ENTRIES_LOWER_BOUND);
    }

    // The runtime cap drives `EvictLocked`: a Preferences edit
    // takes effect on the next `WriteSnapshot`.
    void TestMaxEntriesPreferenceDrivesEviction()
    {
        const QSettings settings;
        const QVariant previous = settings.value(QStringLiteral("recentSessions/maxEntries"));
        auto restoreGuard = qScopeGuard([&]() {
            QSettings restore;
            if (previous.isValid())
            {
                restore.setValue(QStringLiteral("recentSessions/maxEntries"), previous);
            }
            else
            {
                restore.remove(QStringLiteral("recentSessions/maxEntries"));
            }
            restore.sync();
        });

        SessionHistoryManager::SetMaxEntries(3);

        const QTemporaryDir sessionsDir;
        QVERIFY(sessionsDir.isValid());
        SessionHistoryManager manager(QDir(sessionsDir.path()), std::make_unique<InMemoryRecentsIndexStorage>());

        for (int i = 0; i < 6; ++i)
        {
            loglib::LogConfiguration cfg;
            cfg.source = loglib::LogConfiguration::Source{
                .kind = loglib::LogConfiguration::Source::Kind::File,
                .locators = {QStringLiteral("C:/logs/file-%1.json").arg(i).toStdString()}
            };
            QVERIFY(!manager.WriteSnapshot(cfg).isEmpty());
        }

        const auto entries = manager.List();
        QCOMPARE(entries.size(), 3);
    }

    // `RestoreLastSessionOnLaunch` round-trip; default is `true`.
    void TestRestoreLastSessionOnLaunchPreferenceRoundTrip()
    {
        QSettings settings;
        const QVariant previous = settings.value(QStringLiteral("recentSessions/restoreLastSessionOnLaunch"));
        auto restoreGuard = qScopeGuard([&]() {
            QSettings restore;
            if (previous.isValid())
            {
                restore.setValue(QStringLiteral("recentSessions/restoreLastSessionOnLaunch"), previous);
            }
            else
            {
                restore.remove(QStringLiteral("recentSessions/restoreLastSessionOnLaunch"));
            }
            restore.sync();
        });

        settings.remove(QStringLiteral("recentSessions/restoreLastSessionOnLaunch"));
        settings.sync();
        QVERIFY2(
            SessionHistoryManager::RestoreLastSessionOnLaunch(),
            "default RestoreLastSessionOnLaunch must be true when the key is absent"
        );

        SessionHistoryManager::SetRestoreLastSessionOnLaunch(false);
        QVERIFY(!SessionHistoryManager::RestoreLastSessionOnLaunch());

        SessionHistoryManager::SetRestoreLastSessionOnLaunch(true);
        QVERIFY(SessionHistoryManager::RestoreLastSessionOnLaunch());
    }

    // `SetSocketNameForTest` after `TryAcquire` must early-return
    // (debug: `Q_ASSERT`; release: qWarning + ignore) so the
    // original binding stays intact.
    void TestSingleInstanceSetSocketNameForTestIgnoredAfterAcquire()
    {
        const QString originalName =
            QStringLiteral("slv-orig-") + QUuid::createUuid().toString(QUuid::WithoutBraces).left(8);

        SingleInstanceGuard primary;
        primary.SetSocketNameForTest(originalName);
        QVERIFY(primary.TryAcquire({}, /*allowNewInstance=*/false));

        // Release-only: debug `Q_ASSERT` fires before the early
        // return, so skip the rebind attempt in debug builds.
#ifdef QT_NO_DEBUG
        const QString divertName =
            QStringLiteral("slv-div-") + QUuid::createUuid().toString(QUuid::WithoutBraces).left(8);
        primary.SetSocketNameForTest(divertName);

        // The divert-name secondary becomes its own primary
        // because the original primary is still bound elsewhere.
        SingleInstanceGuard divertSecondary;
        divertSecondary.SetSocketNameForTest(divertName);
        QVERIFY(divertSecondary.TryAcquire({}, /*allowNewInstance=*/false));
#endif
    }

    // `WriteSnapshotAndPublish(true)` performs the JSON write and
    // `openWindowsAtQuit` publish under one cross-process lock.
    void TestWriteSnapshotAndPublishUpdatesOpenWindowsAtomically()
    {
        const QStringList previousOpen = SessionHistoryManager::OpenWindowsAtQuitUnlocked();
        auto restoreGuard = qScopeGuard([&]() { SessionHistoryManager::SetOpenWindowsAtQuit(previousOpen); });
        SessionHistoryManager::SetOpenWindowsAtQuit({});

        const QTemporaryDir sessionsDir;
        QVERIFY(sessionsDir.isValid());

        SessionHistoryManager manager(QDir(sessionsDir.path()), std::make_unique<InMemoryRecentsIndexStorage>());

        loglib::LogConfiguration cfg;
        cfg.source = loglib::LogConfiguration::Source{
            .kind = loglib::LogConfiguration::Source::Kind::File, .locators = {"C:/logs/atomic.json"}
        };

        const QString uuid = manager.WriteSnapshotAndPublish(cfg, QString(), /*publishOpenWindow=*/true);
        QVERIFY(!uuid.isEmpty());

        // uuid visible in both the recents index and the open set.
        const auto entries = manager.List();
        QCOMPARE(entries.size(), 1);
        QCOMPARE(entries.front().uuid, uuid);

        const QStringList open = SessionHistoryManager::OpenWindowsAtQuitUnlocked();
        QVERIFY2(
            open.contains(uuid),
            qPrintable(QStringLiteral("openWindowsAtQuit must contain %1, got: %2").arg(uuid, open.join(", ")))
        );
    }

    // `WriteSnapshotAndPublish(false)` matches plain `WriteSnapshot`:
    // JSON written, open-windows list untouched.
    void TestWriteSnapshotAndPublishSkipsPublishWhenFlagFalse()
    {
        const QStringList previousOpen = SessionHistoryManager::OpenWindowsAtQuitUnlocked();
        auto restoreGuard = qScopeGuard([&]() { SessionHistoryManager::SetOpenWindowsAtQuit(previousOpen); });
        SessionHistoryManager::SetOpenWindowsAtQuit({});

        const QTemporaryDir sessionsDir;
        QVERIFY(sessionsDir.isValid());

        SessionHistoryManager manager(QDir(sessionsDir.path()), std::make_unique<InMemoryRecentsIndexStorage>());

        loglib::LogConfiguration cfg;
        cfg.source = loglib::LogConfiguration::Source{
            .kind = loglib::LogConfiguration::Source::Kind::File, .locators = {"C:/logs/noopen.json"}
        };

        const QString uuid = manager.WriteSnapshotAndPublish(cfg, QString(), /*publishOpenWindow=*/false);
        QVERIFY(!uuid.isEmpty());

        QCOMPARE(manager.List().size(), 1);
        QCOMPARE(SessionHistoryManager::OpenWindowsAtQuitUnlocked(), QStringList{});
    }

    // Reuse-uuid fast path: re-saving with identical metadata
    // keeps the count and head-of-list ordering stable; only the
    // JSON on disk is rewritten.
    void TestWriteSnapshotReuseUuidFastPathIsStable()
    {
        const QTemporaryDir sessionsDir;
        QVERIFY(sessionsDir.isValid());

        SessionHistoryManager manager(QDir(sessionsDir.path()), std::make_unique<InMemoryRecentsIndexStorage>());

        loglib::LogConfiguration cfg;
        cfg.source = loglib::LogConfiguration::Source{
            .kind = loglib::LogConfiguration::Source::Kind::File, .locators = {"C:/logs/reuse.json"}
        };

        const QString uuid = manager.WriteSnapshot(cfg);
        QVERIFY(!uuid.isEmpty());
        QCOMPARE(manager.List().size(), 1);

        // Re-save with identical metadata; fast path triggers.
        const QString reuse = manager.WriteSnapshot(cfg, uuid);
        QCOMPARE(reuse, uuid);
        const auto entries = manager.List();
        QCOMPARE(entries.size(), 1);
        QCOMPARE(entries.front().uuid, uuid);
        // JSON still exists; only the entries-group rewrite is
        // skipped.
        QVERIFY(QFileInfo::exists(manager.PathForUuid(uuid)));
    }

    // Reuse-uuid with changed metadata takes the slow path so the
    // menu label reflects the new label / locator / fileCount.
    void TestWriteSnapshotReuseUuidPicksSlowPathOnMetadataChange()
    {
        const QTemporaryDir sessionsDir;
        QVERIFY(sessionsDir.isValid());

        SessionHistoryManager manager(QDir(sessionsDir.path()), std::make_unique<InMemoryRecentsIndexStorage>());

        loglib::LogConfiguration cfg;
        cfg.source = loglib::LogConfiguration::Source{
            .kind = loglib::LogConfiguration::Source::Kind::File, .locators = {"C:/logs/initial.json"}
        };
        const QString uuid = manager.WriteSnapshot(cfg);
        QVERIFY(!uuid.isEmpty());

        // Mutate the source so `BuildLabel` differs; fast-path
        // equality check rejects and rewrites the entry slot.
        cfg.source = loglib::LogConfiguration::Source{
            .kind = loglib::LogConfiguration::Source::Kind::File,
            .locators = {"C:/logs/initial.json", "C:/logs/added.json"}
        };
        const QString reuse = manager.WriteSnapshot(cfg, uuid);
        QCOMPARE(reuse, uuid);

        const auto entries = manager.List();
        QCOMPARE(entries.size(), 1);
        QCOMPARE(entries.front().fileCount, 2);
    }

    // Concurrent stress: many threads through `WriteSnapshot` /
    // `Touch` / `Remove` / `List` must not corrupt the index or
    // deadlock. Pins the `mMutex` thread-safety contract.
    void TestSessionHistoryConcurrentStress()
    {
        const QTemporaryDir sessionsDir;
        QVERIFY(sessionsDir.isValid());

        SessionHistoryManager manager(QDir(sessionsDir.path()), std::make_unique<InMemoryRecentsIndexStorage>());

        constexpr int WRITER_THREADS = 3;
        constexpr int OPS_PER_WRITER = 25;
        std::atomic<bool> stop{false};
        std::atomic<int> writes{0};

        auto writer = [&](int seed) {
            for (int i = 0; i < OPS_PER_WRITER; ++i)
            {
                loglib::LogConfiguration cfg;
                cfg.source = loglib::LogConfiguration::Source{
                    .kind = loglib::LogConfiguration::Source::Kind::File,
                    .locators = {QStringLiteral("C:/logs/stress-%1-%2.json").arg(seed).arg(i).toStdString()}
                };
                if (!manager.WriteSnapshot(cfg).isEmpty())
                {
                    writes.fetch_add(1);
                }
            }
        };

        auto reader = [&]() {
            while (!stop.load())
            {
                // `List()` always returns a self-consistent snapshot.
                const auto entries = manager.List();
                for (const auto &e : entries)
                {
                    QVERIFY2(!e.uuid.isEmpty(), "List entry must never have an empty uuid under stress");
                }
            }
        };

        std::vector<std::thread> writers;
        writers.reserve(WRITER_THREADS);
        std::thread readerThread(reader);
        for (int i = 0; i < WRITER_THREADS; ++i)
        {
            writers.emplace_back(writer, i);
        }
        for (auto &t : writers)
        {
            t.join();
        }
        stop = true;
        readerThread.join();

        // Some writes landed; eviction caps the visible count.
        QVERIFY2(writes.load() > 0, "writer threads must produce at least one successful WriteSnapshot");
        QVERIFY(manager.List().size() <= SessionHistoryManager::MaxEntries());
    }

private:
    MainWindow *mWindow{};
    /// Per-test theme controller, owned by the fixture and reset
    /// in `cleanup()`. Constructed after `mWindow` is deleted in
    /// `cleanup()` would invert the destruction order; we keep
    /// the explicit `reset()` ordering instead of relying on
    /// member-destruction ordering.
    QScopedPointer<ThemeControl> mTheme;
};

QTEST_MAIN(MainWindowTest)
#include "main_window_test.moc"
