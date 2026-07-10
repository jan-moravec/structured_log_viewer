// Focused GUI tests for the histogram strip (ROADMAP item 2).
// Covers: `HistogramModel` populating from a live `LogModel`,
// `HistogramWidget` click / drag / Esc-cancel behaviour, and
// `HistogramDock::closed()` on user dismissal.
//
// Kept in its own binary (mirroring `apptest_queue` /
// `apptest_theme`) so the histogram concern doesn't inflate the
// monolithic `apptest` and can be run in isolation via
// `apptest_histogram`.

#include "anchor_manager.hpp"
#include "histogram_dock.hpp"
#include "histogram_model.hpp"
#include "histogram_widget.hpp"
#include "log_model.hpp"
#include "main_window.hpp"
#include "qt_streaming_log_sink.hpp"

#include <loglib/file_line_source.hpp>
#include <loglib/histogram_bucket_index.hpp>
#include <loglib/internal/advanced_parser_options.hpp>
#include <loglib/log_configuration.hpp>
#include <loglib/log_file.hpp>
#include <loglib/log_level.hpp>
#include <loglib/log_parse_sink.hpp>
#include <loglib/parser_options.hpp>
#include <loglib/parsers/json_parser.hpp>
#include <loglib/stop_token.hpp>

#include <QAbstractItemModel>
#include <QAbstractProxyModel>
#include <QApplication>
#include <QCloseEvent>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QModelIndex>
#include <QSettings>
#include <QSignalSpy>
#include <QString>
#include <QStringList>
#include <QTableView>
#include <QTemporaryDir>
#include <QtTest/QtTest>

#include <bitset>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <memory>
#include <span>
#include <string>
#include <utility>

namespace
{

// Minimal JSON fixture: N rows with an ISO-8601 `time` field the
// parser promotes to `Type::Time`, plus a `level` field the
// canonical-level detector picks up. `body` keeps the row payload
// non-empty. Timestamps are 1 s apart starting at 2026-01-01Z so a
// 1 s bucket rung produces exactly N buckets.
class HistogramFixture
{
public:
    HistogramFixture(int rows, int stepSeconds = 1)
    {
        QVERIFY2(mDir.isValid(), "QTemporaryDir creation must succeed");
        mPath = mDir.filePath("histogram.jsonl");
        std::ofstream stream(mPath.toStdString(), std::ios::binary);
        QVERIFY2(stream.is_open(), "fixture must open for writing");
        static const QStringList LEVELS = {
            QStringLiteral("info"),
            QStringLiteral("warn"),
            QStringLiteral("error"),
            QStringLiteral("debug"),
        };
        for (int i = 0; i < rows; ++i)
        {
            const int totalSeconds = i * stepSeconds;
            const int hour = totalSeconds / 3600;
            const int minute = (totalSeconds / 60) % 60;
            const int second = totalSeconds % 60;
            // Note: the `LogConfigurationManager::AppendKeys`-generated
            // Time column ships parseFormats {%FT%T%Ez, %F %T%Ez, %FT%T,
            // %F %T} — a trailing "Z" is not one of them, so we use the
            // unzoned `%FT%T` shape.
            const QString line =
                QStringLiteral(R"({"time": "2026-01-01T%1:%2:%3", "level": "%4", "body": "msg %5"})")
                    .arg(hour, 2, 10, QChar('0'))
                    .arg(minute, 2, 10, QChar('0'))
                    .arg(second, 2, 10, QChar('0'))
                    .arg(LEVELS[i % LEVELS.size()])
                    .arg(i);
            stream << line.toStdString() << '\n';
        }
    }

    [[nodiscard]] QString Path() const
    {
        return mPath;
    }

private:
    QTemporaryDir mDir;
    QString mPath;
};

// Same shape as `HistogramFixture` but with the `time` key emitted
// *last*, and a run-of-integers `line_number` field wedged in front
// of it. This forces `LogModel`'s post-`AppendBatch` bubble to move
// the Time column from position 3 down to 0 -- exercising the
// `columnsMoved` refresh path in `HistogramModel`. The `line_number`
// values are small non-negative integers that classify as a
// numeric column, so if the histogram accidentally reads them as
// microseconds since epoch (the pre-fix regression), the observed
// range collapses into 1970-01-01 UTC instead of the 2026 range
// the timestamps actually carry.
class HistogramFixtureTimeLast
{
public:
    explicit HistogramFixtureTimeLast(int rows, int stepSeconds = 1)
    {
        QVERIFY2(mDir.isValid(), "QTemporaryDir creation must succeed");
        mPath = mDir.filePath("histogram_time_last.jsonl");
        std::ofstream stream(mPath.toStdString(), std::ios::binary);
        QVERIFY2(stream.is_open(), "fixture must open for writing");
        static const QStringList LEVELS = {
            QStringLiteral("info"),
            QStringLiteral("warn"),
            QStringLiteral("error"),
            QStringLiteral("debug"),
        };
        for (int i = 0; i < rows; ++i)
        {
            const int totalSeconds = i * stepSeconds;
            const int hour = totalSeconds / 3600;
            const int minute = (totalSeconds / 60) % 60;
            const int second = totalSeconds % 60;
            const QString line =
                QStringLiteral(
                    R"({"level": "%1", "line_number": %2, "body": "msg %3", "time": "2026-01-01T%4:%5:%6"})"
                )
                    .arg(LEVELS[i % LEVELS.size()])
                    .arg(i)
                    .arg(i)
                    .arg(hour, 2, 10, QChar('0'))
                    .arg(minute, 2, 10, QChar('0'))
                    .arg(second, 2, 10, QChar('0'));
            stream << line.toStdString() << '\n';
        }
    }

    [[nodiscard]] QString Path() const
    {
        return mPath;
    }

private:
    QTemporaryDir mDir;
    QString mPath;
};

// Feed the JSON file at @p path into @p model synchronously.
// Mirrors the pattern used across `main_window_test.cpp`
// (`BeginStreamingForSyncTest` + direct `JsonParser::ParseStreaming`
// on the borrowed source).
void StreamJsonPathInto(LogModel &model, const QString &path)
{
    QSignalSpy finishedSpy(&model, &LogModel::streamingFinished);
    QVERIFY(finishedSpy.isValid());

    auto file = std::make_unique<loglib::LogFile>(path.toStdString());
    auto fileSource = std::make_unique<loglib::FileLineSource>(std::move(file));
    loglib::FileLineSource *parseSource = fileSource.get();
    const loglib::StopToken stopToken = model.BeginStreamingForSyncTest(std::move(fileSource));

    loglib::ParserOptions options;
    options.stopToken = stopToken;
    loglib::internal::AdvancedParserOptions advanced;
    advanced.threads = 1;
    loglib::JsonParser::ParseStreaming(*parseSource, *model.Sink(), options, advanced);

    const bool finished = finishedSpy.count() > 0 || finishedSpy.wait(5000);
    QVERIFY2(finished, "streamingFinished must arrive within the timeout");
    model.EndStreaming(false);
}

void StreamJsonInto(LogModel &model, const HistogramFixture &fixture)
{
    StreamJsonPathInto(model, fixture.Path());
}

void StreamJsonInto(LogModel &model, const HistogramFixtureTimeLast &fixture)
{
    StreamJsonPathInto(model, fixture.Path());
}

// Wait for `HistogramModel::bucketsChanged` to fire at least once.
// The signal is coalesced through a 50 ms `QTimer`, so a spin here
// (rather than an immediate assert) keeps the test deterministic.
void WaitForBucketsChanged(HistogramModel &model)
{
    QSignalSpy spy(&model, &HistogramModel::bucketsChanged);
    QVERIFY(spy.isValid());
    if (spy.count() > 0)
    {
        return;
    }
    QVERIFY2(spy.wait(1000), "bucketsChanged must arrive after streaming");
}

// Ensure a pre-attached spy has observed at least one
// `anchorBucketsChanged` emission. The signal is *synchronous*
// (direct-connected within the same thread), so callers must
// construct the spy *before* the anchor mutation -- creating it
// afterwards would miss the emit and force this to spin uselessly
// for a full second. We spin briefly anyway in case a future
// coalesce timer is introduced.
void ExpectAnchorBucketsChanged(QSignalSpy &spy)
{
    QVERIFY(spy.isValid());
    if (spy.count() > 0)
    {
        return;
    }
    QVERIFY2(spy.wait(1000), "anchorBucketsChanged must arrive after an anchor mutation");
}

// Drive `MainWindow::OpenFilesForTest` and pump the event loop until
// `LogModel::streamingFinished` arrives. Mirrors the wait idiom the
// bigger `apptest` fixture uses.
void OpenFixtureInMainWindow(MainWindow &window, const HistogramFixture &fixture)
{
    auto *model = window.findChild<LogModel *>();
    QVERIFY(model != nullptr);
    // Guard the analyzer: `QVERIFY` inside a non-slot helper only
    // registers a failure; it does not abort the caller, so a null
    // `model` would dereference below without the explicit return.
    if (model == nullptr)
    {
        return;
    }
    QSignalSpy finishedSpy(model, &LogModel::streamingFinished);
    QVERIFY(finishedSpy.isValid());
    window.OpenFilesForTest({fixture.Path()}, MainWindow::OpenMode::Replace);
    if (finishedSpy.count() == 0)
    {
        QVERIFY2(finishedSpy.wait(5000), "streamingFinished must arrive");
    }
    QVERIFY(model->rowCount() > 0);
    // Pump once more so any coalesced `bucketsChanged` timers /
    // queued histogram slots run before the assertions.
    QCoreApplication::processEvents();
}

} // namespace

// NOLINTNEXTLINE(misc-use-internal-linkage): `Q_OBJECT` QtTest fixture.
class HistogramDockTest : public QObject
{
    Q_OBJECT

private slots:
    /// One-shot fixture setup: install the timezone database so
    /// `date::CurrentZone()` returns a live pointer when the
    /// widget formats hover / subtitle labels. Without this the
    /// first `date::zoned_time` construction throws (memoized in
    /// `loglib::CurrentZone`) and the test binary crashes on the
    /// first paint that touches a populated index. Mirrors the
    /// setup `main_window_test.cpp::initTestCase` performs for
    /// the sibling `apptest` binary.
    void initTestCase()
    {
        QVERIFY2(
            MainWindow::InitializeTimezoneDatabase(),
            "Failed to initialise timezone database; see qCritical above. The staged "
            "`tzdata/` directory must live next to the apptest binary "
            "(handled by test/app/CMakeLists.txt)."
        );
    }

    /// A dock created against a log with no time column should sit
    /// in the empty state: no buckets, `HasTimeColumn` false,
    /// `bucketClicked` a no-op even on click.
    static void TestEmptyStateWhenNoTimeColumn()
    {
        LogModel model;
        const HistogramDock dock(&model, /*theme=*/nullptr, /*anchors=*/nullptr);
        const HistogramModel *hm = dock.ModelForTest();
        QVERIFY(hm != nullptr);
        QVERIFY(!hm->HasTimeColumn());
        QCOMPARE(hm->TimeColumnIndex(), -1);
        QVERIFY(hm->Index().Empty());
    }

    /// Streaming a JSON fixture with a `time` field populates the
    /// bucket index with the expected total row count. `HasTimeColumn`
    /// flips to true.
    static void TestBucketIndexPopulatesFromLogModel()
    {
        LogModel model;
        const HistogramDock dock(&model, /*theme=*/nullptr, /*anchors=*/nullptr);
        HistogramModel *hm = dock.ModelForTest();
        QVERIFY(hm != nullptr);

        const HistogramFixture fixture(30, /*stepSeconds=*/1);
        StreamJsonInto(model, fixture);
        WaitForBucketsChanged(*hm);

        QVERIFY(hm->HasTimeColumn());
        QCOMPARE(hm->Index().TotalRowCount(), static_cast<std::uint64_t>(30));
        QVERIFY(!hm->Index().Empty());
    }

    /// Regression: when the source file emits the timestamp key
    /// *after* other columns (as happens for CSVs whose `timestamp`
    /// header sits last, and for JSON logs that don't put `time`
    /// first), `LogModel` post-`AppendBatch` bubbles the newly
    /// observed `Type::Time` column down to position 0. That move
    /// mutates the meaning of every column index behind the scenes:
    /// what used to be column 3 (the timestamp) becomes column 0,
    /// and the old column 3 slot now points at an unrelated field.
    ///
    /// `HistogramModel` caches the time / level column indices, so
    /// the move must invalidate them. Before the fix it didn't,
    /// and a follow-up rebuild (triggered e.g. by a level-column
    /// promotion in the same batch) would read the wrong column --
    /// most visibly, a `line_number` integer column re-interpreted
    /// as microseconds since epoch, collapsing the observed range
    /// to somewhere near 1970-01-01 UTC no matter what timestamps
    /// the log actually carried.
    ///
    /// The fixture puts `time` last with a numeric `line_number`
    /// column ahead of it, and timestamps land in 2026. The
    /// observed range must survive the bubble intact.
    static void TestTimeColumnCacheSurvivesColumnBubble()
    {
        LogModel model;
        const HistogramDock dock(&model, /*theme=*/nullptr, /*anchors=*/nullptr);
        HistogramModel *hm = dock.ModelForTest();
        QVERIFY(hm != nullptr);

        // 30 rows, 1 s apart, starting at 2026-01-01T00:00:00.
        constexpr int ROWS = 30;
        const HistogramFixtureTimeLast fixture(ROWS, /*stepSeconds=*/1);
        StreamJsonInto(model, fixture);
        WaitForBucketsChanged(*hm);

        // The Time column must have been bubbled to position 0.
        QVERIFY(hm->HasTimeColumn());
        QCOMPARE(hm->TimeColumnIndex(), 0);

        // The bucket index still holds every row.
        QCOMPARE(hm->Index().TotalRowCount(), static_cast<std::uint64_t>(ROWS));

        // And the observed range must reflect the actual 2026
        // timestamps, not the 1970-adjacent range you get from
        // reading `line_number` (0..29) as microseconds.
        const auto range = hm->ObservedRange();
        QVERIFY2(range.has_value(), "observed range must be populated after streaming");
        // 2026-01-01T00:00:00 UTC in microseconds since epoch.
        constexpr std::int64_t YEAR_2026_EPOCH_US = 1'767'225'600LL * 1'000'000LL;
        // Any lower bound comfortably inside 2026 is fine here:
        // the assertion is really "not 1970". Use start-of-2026
        // as the floor with a small safety cushion for rounding.
        const std::int64_t minUs = range->min.time_since_epoch().count();
        const std::int64_t maxUs = range->max.time_since_epoch().count();
        QVERIFY2(
            minUs >= YEAR_2026_EPOCH_US,
            "observed min timestamp fell before 2026 -- histogram is likely reading a non-time column"
        );
        QVERIFY(maxUs >= minUs);
        // Sanity: 30 rows, 1 s apart, so the span is at least 29 s.
        constexpr std::int64_t ONE_SECOND_US = 1'000'000LL;
        QVERIFY(maxUs - minUs >= 29LL * ONE_SECOND_US);
    }

    /// The cached `LevelColumnIndex` mirrors `LogModel::FirstLevelColumnIndex`
    /// after a stream. Guards against regressions in
    /// `ComputeLevelColumnIndex` / `OnModelReset` and, transitively,
    /// against the mid-stream promotion path in `OnRowsInserted` /
    /// `OnEnumColumnsChanged` reading the wrong column. Also asserts
    /// that at least one canonical (non-Unknown) level appears in the
    /// merged buckets — a `LevelForRow` bug that always returned
    /// Unknown would land every row in slot 0 and pass the row-count
    /// assertion, so we need a level-attribution check to catch it.
    static void TestLevelColumnIndexTracksLogModel()
    {
        LogModel model;
        const HistogramDock dock(&model, /*theme=*/nullptr, /*anchors=*/nullptr);
        HistogramModel *hm = dock.ModelForTest();
        QVERIFY(hm != nullptr);

        // Empty model — no level column yet.
        QCOMPARE(hm->LevelColumnIndex(), -1);

        const HistogramFixture fixture(30, /*stepSeconds=*/1);
        StreamJsonInto(model, fixture);
        WaitForBucketsChanged(*hm);

        // After streaming a fixture that carries canonical levels
        // (`info`, `warn`, `error`, `debug`) the LogModel promotes
        // the `level` column to `Type::Level`, and our cached mirror
        // must agree.
        const int expected = model.FirstLevelColumnIndex();
        QVERIFY2(expected >= 0, "fixture must produce a Type::Level column");
        QCOMPARE(hm->LevelColumnIndex(), expected);

        // Row totals go through slot 0 (Unknown) when
        // `mLevelColumnIndex < 0`; if that path leaked through
        // we'd see zero canonical rows across all buckets.
        std::uint64_t canonicalRows = 0;
        for (const auto &bucket : hm->Index().Buckets())
        {
            for (std::size_t slot = 1; slot < bucket.counts.size(); ++slot)
            {
                canonicalRows += bucket.counts[slot];
            }
        }
        QVERIFY2(canonicalRows > 0, "at least one row must land in a canonical level slot");
    }

    /// Idle state: with a populated log but no hover in flight,
    /// the details strip must carry the plot-summary format --
    /// the same content the old top subtitle used to render.
    /// Guards against a regression where a stale `mLastHoverBucket`
    /// (or an accidental swap of the format branches) would leave
    /// the strip showing hover text with no pointer on the widget.
    static void TestIdleDetailsShowsPlotSummary()
    {
        LogModel model;
        const HistogramDock dock(&model, /*theme=*/nullptr, /*anchors=*/nullptr);
        HistogramModel *hm = dock.ModelForTest();
        const HistogramWidget *widget = dock.WidgetForTest();
        QVERIFY(hm != nullptr);
        QVERIFY(widget != nullptr);

        const HistogramFixture fixture(30, /*stepSeconds=*/1);
        StreamJsonInto(model, fixture);
        WaitForBucketsChanged(*hm);

        // No hover event dispatched: the strip must be in idle
        // mode. The plot summary starts with `bucket:` and lists
        // the total row count under the current locale.
        QCOMPARE(widget->LastHoverBucketForTest(), -1);
        const QString details = widget->DetailsTextForTest();
        QVERIFY2(details.startsWith(QStringLiteral("bucket:")), qPrintable(details));
        QVERIFY2(details.contains(QStringLiteral("rows:")), qPrintable(details));
    }

    /// Hover state: when the pointer sits over a populated bar
    /// the details strip switches to the hovered-bucket format --
    /// range, total, and per-level counts on the same line. This
    /// is the content that used to live in the OS `QToolTip`; it
    /// now has to reach the strip on the next `paintEvent` after
    /// the mouse move. `mLastHoverBucket` is the observable
    /// latching signal because the format string is derived from
    /// it deterministically.
    static void TestHoverUpdatesDetailsLine()
    {
        LogModel model;
        const HistogramDock dock(&model, /*theme=*/nullptr, /*anchors=*/nullptr);
        HistogramModel *hm = dock.ModelForTest();
        HistogramWidget *widget = dock.WidgetForTest();
        QVERIFY(hm != nullptr);
        QVERIFY(widget != nullptr);

        const HistogramFixture fixture(30, /*stepSeconds=*/1);
        StreamJsonInto(model, fixture);
        WaitForBucketsChanged(*hm);

        widget->resize(400, 100);

        // Pointer at the middle of the plot rect: guaranteed
        // inside the bars since the fixture packs 30 rows of
        // 1 s buckets across the full width.
        const QPoint centre(widget->width() / 2, widget->height() / 2);
        QMouseEvent hover(QEvent::MouseMove, centre, widget->mapToGlobal(centre), Qt::NoButton, Qt::NoButton, {});
        QApplication::sendEvent(widget, &hover);
        QVERIFY2(widget->LastHoverBucketForTest() >= 0, "hover over a populated bar must latch a visual-column index");

        const QString details = widget->DetailsTextForTest();
        // The hover format includes a "total:" segment followed
        // by at least one canonical level:count fragment. The
        // fixture rotates levels {info, warn, error, debug} across
        // its rows, so the merged column at the widget centre is
        // guaranteed to contain at least one non-zero level.
        // `CanonicalLevelName` capitalises the enum name, so the
        // fragment reads e.g. "Info: 5", not "info: 5".
        QVERIFY2(details.contains(QStringLiteral("total:")), qPrintable(details));
        QVERIFY2(
            details.contains(QStringLiteral("Info:")) || details.contains(QStringLiteral("Warn:")) ||
                details.contains(QStringLiteral("Error:")) || details.contains(QStringLiteral("Debug:")),
            qPrintable(details)
        );
        QVERIFY2(
            !details.startsWith(QStringLiteral("bucket:")),
            "hover format must not fall back to the plot-summary prefix"
        );

        // Dedup guard: a follow-up move inside the same bar must
        // leave the cached column index unchanged. If the guard
        // ever regresses (or somebody re-adds a per-event repaint
        // side effect on `mLastHoverBucket`), the value would
        // spuriously flip through -1 and back on every motion.
        const int firstColumn = widget->LastHoverBucketForTest();
        const QPoint nudge(centre.x() + 1, centre.y());
        QMouseEvent tinyMove(QEvent::MouseMove, nudge, widget->mapToGlobal(nudge), Qt::NoButton, Qt::NoButton, {});
        QApplication::sendEvent(widget, &tinyMove);
        QCOMPARE(widget->LastHoverBucketForTest(), firstColumn);
    }

    /// Regression companion: when the pointer leaves the widget
    /// entirely, the hover cache must reset so the details strip
    /// snaps back to the plot summary. Without this, a lingering
    /// hover readout would keep referencing a bar the pointer
    /// isn't over any more, and a fresh entry over the same
    /// column would be shortcircuited by the dedup guard.
    static void TestLeaveEventResetsDetailsToSummary()
    {
        LogModel model;
        const HistogramDock dock(&model, /*theme=*/nullptr, /*anchors=*/nullptr);
        HistogramModel *hm = dock.ModelForTest();
        HistogramWidget *widget = dock.WidgetForTest();
        QVERIFY(hm != nullptr);
        QVERIFY(widget != nullptr);

        const HistogramFixture fixture(30, /*stepSeconds=*/1);
        StreamJsonInto(model, fixture);
        WaitForBucketsChanged(*hm);

        widget->resize(400, 100);

        const QPoint centre(widget->width() / 2, widget->height() / 2);
        QMouseEvent hover(QEvent::MouseMove, centre, widget->mapToGlobal(centre), Qt::NoButton, Qt::NoButton, {});
        QApplication::sendEvent(widget, &hover);
        QVERIFY(widget->LastHoverBucketForTest() >= 0);
        QVERIFY(!widget->DetailsTextForTest().startsWith(QStringLiteral("bucket:")));

        QEvent leave(QEvent::Leave);
        QApplication::sendEvent(widget, &leave);
        // Cache reset is the observable signal that `leaveEvent`
        // ran; the details format then falls through to the plot
        // summary branch, which starts with `bucket:`.
        QCOMPARE(widget->LastHoverBucketForTest(), -1);
        QVERIFY2(
            widget->DetailsTextForTest().startsWith(QStringLiteral("bucket:")),
            qPrintable(widget->DetailsTextForTest())
        );
    }

    /// A click on the widget with populated data emits
    /// `bucketClicked` with a valid in-range bucket index.
    static void TestClickEmitsBucketClicked()
    {
        LogModel model;
        const HistogramDock dock(&model, /*theme=*/nullptr, /*anchors=*/nullptr);
        HistogramModel *hm = dock.ModelForTest();
        HistogramWidget *widget = dock.WidgetForTest();
        QVERIFY(hm != nullptr);
        QVERIFY(widget != nullptr);

        const HistogramFixture fixture(30, /*stepSeconds=*/1);
        StreamJsonInto(model, fixture);
        WaitForBucketsChanged(*hm);

        widget->resize(400, 100);
        const QSignalSpy spy(&dock, &HistogramDock::bucketClicked);
        QVERIFY(spy.isValid());

        const QPoint centre(widget->width() / 2, widget->height() / 2);
        QMouseEvent press(QEvent::MouseButtonPress, centre, widget->mapToGlobal(centre), Qt::LeftButton, Qt::LeftButton, {});
        QMouseEvent release(QEvent::MouseButtonRelease, centre, widget->mapToGlobal(centre), Qt::LeftButton, Qt::NoButton, {});
        QApplication::sendEvent(widget, &press);
        QApplication::sendEvent(widget, &release);

        QCOMPARE(spy.count(), 1);
        const auto bucketIdx = spy.first().value(0).value<std::size_t>();
        QVERIFY(bucketIdx < hm->Index().Buckets().size());
    }

    /// A press-move-release beyond the drag threshold emits
    /// `timeRangeSelected` with an ordered `[from, to]` pair.
    static void TestDragEmitsTimeRangeSelected()
    {
        LogModel model;
        const HistogramDock dock(&model, /*theme=*/nullptr, /*anchors=*/nullptr);
        HistogramModel *hm = dock.ModelForTest();
        HistogramWidget *widget = dock.WidgetForTest();
        QVERIFY(hm != nullptr);
        QVERIFY(widget != nullptr);

        const HistogramFixture fixture(30, /*stepSeconds=*/1);
        StreamJsonInto(model, fixture);
        WaitForBucketsChanged(*hm);

        widget->resize(400, 100);
        const QSignalSpy rangeSpy(&dock, &HistogramDock::timeRangeSelected);
        const QSignalSpy clickSpy(&dock, &HistogramDock::bucketClicked);
        QVERIFY(rangeSpy.isValid());
        QVERIFY(clickSpy.isValid());

        const QPoint start(widget->width() / 4, widget->height() / 2);
        const QPoint middle((widget->width() * 3) / 4, widget->height() / 2);
        const QPoint end((widget->width() * 3) / 4, widget->height() / 2);
        QMouseEvent press(QEvent::MouseButtonPress, start, widget->mapToGlobal(start), Qt::LeftButton, Qt::LeftButton, {});
        QMouseEvent move(QEvent::MouseMove, middle, widget->mapToGlobal(middle), Qt::NoButton, Qt::LeftButton, {});
        QMouseEvent release(QEvent::MouseButtonRelease, end, widget->mapToGlobal(end), Qt::LeftButton, Qt::NoButton, {});
        QApplication::sendEvent(widget, &press);
        QApplication::sendEvent(widget, &move);
        QApplication::sendEvent(widget, &release);

        QCOMPARE(rangeSpy.count(), 1);
        // Drag must not double-emit a bucketClicked.
        QCOMPARE(clickSpy.count(), 0);
        const auto &args = rangeSpy.first();
        const qint64 fromUs = args.value(0).toLongLong();
        const qint64 toUs = args.value(1).toLongLong();
        QVERIFY(fromUs <= toUs);
    }

    /// Esc during an in-progress drag cancels: no
    /// `timeRangeSelected` on subsequent release, and the drag brush
    /// disappears.
    static void TestEscCancelsInProgressDrag()
    {
        LogModel model;
        const HistogramDock dock(&model, /*theme=*/nullptr, /*anchors=*/nullptr);
        HistogramModel *hm = dock.ModelForTest();
        HistogramWidget *widget = dock.WidgetForTest();
        QVERIFY(hm != nullptr);
        QVERIFY(widget != nullptr);

        const HistogramFixture fixture(30, /*stepSeconds=*/1);
        StreamJsonInto(model, fixture);
        WaitForBucketsChanged(*hm);

        widget->resize(400, 100);
        widget->setFocus();
        const QSignalSpy rangeSpy(&dock, &HistogramDock::timeRangeSelected);
        QVERIFY(rangeSpy.isValid());

        const QPoint start(widget->width() / 4, widget->height() / 2);
        const QPoint middle(widget->width() / 2, widget->height() / 2);
        QMouseEvent press(QEvent::MouseButtonPress, start, widget->mapToGlobal(start), Qt::LeftButton, Qt::LeftButton, {});
        QMouseEvent move(QEvent::MouseMove, middle, widget->mapToGlobal(middle), Qt::NoButton, Qt::LeftButton, {});
        QKeyEvent esc(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
        QApplication::sendEvent(widget, &press);
        QApplication::sendEvent(widget, &move);
        QApplication::sendEvent(widget, &esc);

        // Now release the button; because the drag was cancelled the
        // release must not emit a range.
        QMouseEvent release(QEvent::MouseButtonRelease, middle, widget->mapToGlobal(middle), Qt::LeftButton, Qt::NoButton, {});
        QApplication::sendEvent(widget, &release);

        QCOMPARE(rangeSpy.count(), 0);
    }

    /// z / Shift+Z zoom in / out on the bucket rung.
    static void TestZoomKeysWalkTheLadder()
    {
        LogModel model;
        const HistogramDock dock(&model, /*theme=*/nullptr, /*anchors=*/nullptr);
        HistogramModel *hm = dock.ModelForTest();
        HistogramWidget *widget = dock.WidgetForTest();
        QVERIFY(hm != nullptr);
        QVERIFY(widget != nullptr);

        // A ~30 s fixture; the auto rung is OneSecond so Shift+Z can
        // grow it to TenSeconds.
        const HistogramFixture fixture(30, /*stepSeconds=*/1);
        StreamJsonInto(model, fixture);
        WaitForBucketsChanged(*hm);

        const auto initial = hm->Index().BucketSize();
        widget->setFocus();
        QKeyEvent shiftZ(QEvent::KeyPress, Qt::Key_Z, Qt::ShiftModifier);
        QApplication::sendEvent(widget, &shiftZ);
        QVERIFY(hm->Index().BucketSize() > initial);

        const auto coarser = hm->Index().BucketSize();
        QKeyEvent z(QEvent::KeyPress, Qt::Key_Z, Qt::NoModifier);
        QApplication::sendEvent(widget, &z);
        QVERIFY(hm->Index().BucketSize() < coarser);
    }

    /// `HistogramDock::closed()` fires on `closeEvent`; matches the
    /// `AnchorsDock::closed` contract that `WireDockToggle` relies
    /// on.
    static void TestCloseEmitsClosedSignal()
    {
        LogModel model;
        HistogramDock dock(&model, /*theme=*/nullptr, /*anchors=*/nullptr);
        const QSignalSpy closedSpy(&dock, &HistogramDock::closed);
        QVERIFY(closedSpy.isValid());

        QCloseEvent event;
        QApplication::sendEvent(&dock, &event);

        QCOMPARE(closedSpy.count(), 1);
    }

    /// `FirstRowInBucket` returns the first source-row whose
    /// timestamp falls in that bucket. With 1 s buckets and rows
    /// stepped 1 s apart, bucket `i` contains exactly row `i`.
    static void TestFirstRowInBucketMapsToRowIndex()
    {
        LogModel model;
        const HistogramDock dock(&model, /*theme=*/nullptr, /*anchors=*/nullptr);
        HistogramModel *hm = dock.ModelForTest();
        QVERIFY(hm != nullptr);

        const HistogramFixture fixture(10, /*stepSeconds=*/1);
        StreamJsonInto(model, fixture);
        WaitForBucketsChanged(*hm);
        // Pin the rung so the assertion isn't racing an
        // ApplyAutoBucketSize call. `SetBucketSize` fully rebuilds
        // synchronously — no need to wait on `bucketsChanged`, and
        // waiting would deadlock when the auto rung already matches.
        hm->SetBucketSize(loglib::HistogramBucketSize::OneSecond);

        QCOMPARE(hm->FirstRowInBucket(0), 0);
        QCOMPARE(hm->FirstRowInBucket(5), 5);
        // Bucket count is at least 10 (fenceposts may add one).
        QVERIFY(hm->Index().Buckets().size() >= 10);
    }

    /// Regression: after the user manually pinned a rung via
    /// `SetBucketSize` (Z / Shift+Z / Ctrl+wheel), calling
    /// `ApplyAutoBucketSize` was silently vetoed by the manual-pin
    /// latch, leaving the widget's context-menu "Reset zoom (auto)"
    /// entry inert. `ResetBucketSizeToAuto` must drop the pin and
    /// re-pick a rung freshly.
    static void TestResetBucketSizeToAutoDropsManualPin()
    {
        LogModel model;
        const HistogramDock dock(&model, /*theme=*/nullptr, /*anchors=*/nullptr);
        HistogramModel *hm = dock.ModelForTest();
        QVERIFY(hm != nullptr);

        // A 30 s span picks `OneSecond` on the auto ladder.
        const HistogramFixture fixture(30, /*stepSeconds=*/1);
        StreamJsonInto(model, fixture);
        WaitForBucketsChanged(*hm);
        const auto autoRung = hm->Index().BucketSize();
        QCOMPARE(autoRung, loglib::HistogramBucketSize::OneSecond);

        // Manually pin a coarser rung. `SetBucketSize` sets the
        // internal `mBucketSizePinned` latch, which used to make
        // the subsequent `ApplyAutoBucketSize` a no-op.
        hm->SetBucketSize(loglib::HistogramBucketSize::TenMinutes);
        QCOMPARE(hm->Index().BucketSize(), loglib::HistogramBucketSize::TenMinutes);

        // A plain `ApplyAutoBucketSize` here would be swallowed by
        // the pin. `ResetBucketSizeToAuto` explicitly drops the pin
        // first, so the rung snaps back to the auto pick.
        hm->ResetBucketSizeToAuto();
        QCOMPARE(hm->Index().BucketSize(), autoRung);

        // The pin must actually be gone: a subsequent
        // `ApplyAutoBucketSize` (streaming-side path) still has to
        // work without another explicit reset.
        hm->SetBucketSize(loglib::HistogramBucketSize::OneMinute);
        hm->ResetBucketSizeToAuto();
        QCOMPARE(hm->Index().BucketSize(), autoRung);
        hm->ApplyAutoBucketSize();
        QCOMPARE(hm->Index().BucketSize(), autoRung);
    }

    /// Regression: presses on the chrome around the bars used to
    /// anchor a click / drag even though the pointer was clearly
    /// not on a bar. The widget now rejects presses outside
    /// `PlotRect()` so an accidental touch on the details strip
    /// (or the top padding) can't jump the table. The chrome
    /// lives at the *bottom* of the widget now -- the old top
    /// subtitle was moved into the same strip as the hover readout.
    static void TestPressOnDetailsStripDoesNotEmitBucketClicked()
    {
        LogModel model;
        const HistogramDock dock(&model, /*theme=*/nullptr, /*anchors=*/nullptr);
        HistogramModel *hm = dock.ModelForTest();
        HistogramWidget *widget = dock.WidgetForTest();
        QVERIFY(hm != nullptr);
        QVERIFY(widget != nullptr);

        const HistogramFixture fixture(30, /*stepSeconds=*/1);
        StreamJsonInto(model, fixture);
        WaitForBucketsChanged(*hm);

        widget->resize(400, 100);
        const QSignalSpy clickSpy(&dock, &HistogramDock::bucketClicked);
        const QSignalSpy rangeSpy(&dock, &HistogramDock::timeRangeSelected);
        QVERIFY(clickSpy.isValid());
        QVERIFY(rangeSpy.isValid());

        // y = height - 4 lands inside the details strip at the
        // bottom (its height is 18 px, so anything within the
        // last 18 rows sits outside `PlotRect`). This press must
        // be rejected entirely -- no drag anchor, no click emit
        // on release.
        const QPoint details(widget->width() / 2, widget->height() - 4);
        QMouseEvent press(
            QEvent::MouseButtonPress, details, widget->mapToGlobal(details), Qt::LeftButton, Qt::LeftButton, {}
        );
        QMouseEvent release(
            QEvent::MouseButtonRelease, details, widget->mapToGlobal(details), Qt::LeftButton, Qt::NoButton, {}
        );
        QApplication::sendEvent(widget, &press);
        QApplication::sendEvent(widget, &release);

        QCOMPARE(clickSpy.count(), 0);
        QCOMPARE(rangeSpy.count(), 0);

        // Sanity: a press inside the plot rect still emits a click,
        // proving the fixture is otherwise wired up.
        const QPoint bar(widget->width() / 2, widget->height() / 2);
        QMouseEvent barPress(
            QEvent::MouseButtonPress, bar, widget->mapToGlobal(bar), Qt::LeftButton, Qt::LeftButton, {}
        );
        QMouseEvent barRelease(
            QEvent::MouseButtonRelease, bar, widget->mapToGlobal(bar), Qt::LeftButton, Qt::NoButton, {}
        );
        QApplication::sendEvent(widget, &barPress);
        QApplication::sendEvent(widget, &barRelease);
        QCOMPARE(clickSpy.count(), 1);
    }

    /// End-to-end: `MainWindow::AddTimeRangeFilterFromHistogram`
    /// (the slot behind `HistogramDock::timeRangeSelected`) must
    /// install a `Type::Time` filter under the sentinel
    /// `histogram-time-range` ID so a second drag replaces it
    /// instead of stacking. Guards the plumbing from
    /// `HistogramWidget::mouseReleaseEvent` all the way through to
    /// `MainWindow::mFilters`.
    static void TestMainWindowInstallsHistogramTimeRangeFilter()
    {
        QSettings().clear();
        MainWindow window;
        const HistogramFixture fixture(30, /*stepSeconds=*/1);
        OpenFixtureInMainWindow(window, fixture);

        auto *dock = window.findChild<HistogramDock *>();
        QVERIFY(dock != nullptr);
        const HistogramModel *hm = dock->ModelForTest();
        QVERIFY(hm != nullptr);
        QVERIFY(hm->HasTimeColumn());

        const auto range = hm->ObservedRange();
        QVERIFY(range.has_value());
        const qint64 from = range->min.time_since_epoch().count();
        const qint64 to = range->max.time_since_epoch().count();

        window.AddTimeRangeFilterFromHistogram(from, to);

        const auto &filters = window.Filters();
        const auto it = filters.find("histogram-time-range");
        QVERIFY2(it != filters.end(), "sentinel filter ID must be installed");
        QCOMPARE(it->second.type, loglib::LogConfiguration::LogFilter::Type::Time);
        QCOMPARE(it->second.row, hm->TimeColumnIndex());
        QVERIFY(it->second.filterBegin.has_value());
        QVERIFY(it->second.filterEnd.has_value());
        QCOMPARE(*it->second.filterBegin, from);
        QCOMPARE(*it->second.filterEnd, to);

        // A second drag with a narrower range must *replace* the
        // filter rather than stacking a second entry.
        window.AddTimeRangeFilterFromHistogram(from, from);
        const auto &filters2 = window.Filters();
        QCOMPARE(filters2.count("histogram-time-range"), std::size_t{1});
        QCOMPARE(*filters2.at("histogram-time-range").filterEnd, from);
    }

    /// End-to-end: `MainWindow::JumpToFirstRowInBucket` moves the
    /// table's current selection to the source row of the first
    /// row in that bucket. With rows 1 s apart and the auto rung
    /// picking `OneSecond`, bucket `0` maps to source row `0`.
    static void TestMainWindowJumpToFirstRowInBucket()
    {
        QSettings().clear();
        MainWindow window;
        const HistogramFixture fixture(10, /*stepSeconds=*/1);
        OpenFixtureInMainWindow(window, fixture);

        auto *dock = window.findChild<HistogramDock *>();
        QVERIFY(dock != nullptr);
        HistogramModel *hm = dock->ModelForTest();
        QVERIFY(hm != nullptr);
        // Pin the rung so the bucket->row mapping is deterministic
        // even when the auto pick would already agree.
        hm->SetBucketSize(loglib::HistogramBucketSize::OneSecond);

        window.JumpToFirstRowInBucket(0);

        auto *table = window.findChild<QTableView *>();
        QVERIFY(table != nullptr);
        const QModelIndex currentProxy = table->currentIndex();
        QVERIFY2(currentProxy.isValid(), "JumpToFirstRowInBucket must move the current index");
        // The current index lives in the proxy chain; map to source
        // by walking each proxy in turn.
        QModelIndex idx = currentProxy;
        while (const auto *proxy = qobject_cast<const QAbstractProxyModel *>(idx.model()))
        {
            idx = proxy->mapToSource(idx);
        }
        QCOMPARE(idx.row(), 0);
    }

    /// Anchoring a row must flip the corresponding bucket's slot
    /// bit in `HistogramModel::AnchorSlotsPerBucket()` and turn on
    /// `HasAnchorTicks()`. Stream 10 rows one second apart at the
    /// finest rung so each row lands in its own bucket, then anchor
    /// row 5 at palette slot 3 and inspect the bitmask.
    static void TestAnchorTickShowsBucketWithAnchor()
    {
        AnchorManager anchors;
        LogModel model(/*parent=*/nullptr, /*theme=*/nullptr, /*anchors=*/&anchors);
        const HistogramDock dock(&model, /*theme=*/nullptr, /*anchors=*/&anchors);
        HistogramModel *hm = dock.ModelForTest();
        QVERIFY(hm != nullptr);

        constexpr int ROWS = 10;
        const HistogramFixture fixture(ROWS, /*stepSeconds=*/1);
        StreamJsonInto(model, fixture);
        WaitForBucketsChanged(*hm);
        hm->SetBucketSize(loglib::HistogramBucketSize::OneSecond);
        QVERIFY(hm->HasTimeColumn());
        QCOMPARE(hm->Index().Buckets().size(), static_cast<std::size_t>(ROWS));
        QVERIFY(!hm->HasAnchorTicks());

        constexpr int TARGET_ROW = 5;
        constexpr std::uint8_t TARGET_SLOT = 3;
        const auto key = model.AnchorKeyForRow(TARGET_ROW);
        QVERIFY2(key.has_value(), "AnchorKeyForRow(5) must resolve after streaming");
        QSignalSpy anchorSpy(hm, &HistogramModel::anchorBucketsChanged);
        anchors.SetAnchor(*key, TARGET_SLOT);
        ExpectAnchorBucketsChanged(anchorSpy);

        QVERIFY(hm->HasAnchorTicks());
        const auto anchorMasks = hm->AnchorSlotsPerBucket();
        QVERIFY(anchorMasks.size() == hm->Index().Buckets().size());
        QVERIFY2(
            anchorMasks[TARGET_ROW].test(TARGET_SLOT),
            "target bucket must carry the anchor slot bit after SetAnchor"
        );
        // Every other bucket must remain empty -- no accidental
        // OR-in on a neighbouring slot.
        for (std::size_t i = 0; i < anchorMasks.size(); ++i)
        {
            if (i == static_cast<std::size_t>(TARGET_ROW))
            {
                continue;
            }
            QVERIFY2(anchorMasks[i].none(), "non-target buckets must stay empty");
        }
    }

    /// Removing the only anchor must clear the bucket's mask and
    /// flip `HasAnchorTicks()` back to false. Guards against a leak
    /// where the incremental update path forgot to decrement the
    /// running popcount cache.
    static void TestAnchorTickRespondsToRemoval()
    {
        AnchorManager anchors;
        LogModel model(/*parent=*/nullptr, /*theme=*/nullptr, /*anchors=*/&anchors);
        const HistogramDock dock(&model, /*theme=*/nullptr, /*anchors=*/&anchors);
        HistogramModel *hm = dock.ModelForTest();
        QVERIFY(hm != nullptr);

        constexpr int ROWS = 10;
        const HistogramFixture fixture(ROWS, /*stepSeconds=*/1);
        StreamJsonInto(model, fixture);
        WaitForBucketsChanged(*hm);
        hm->SetBucketSize(loglib::HistogramBucketSize::OneSecond);

        constexpr int TARGET_ROW = 5;
        constexpr std::uint8_t TARGET_SLOT = 3;
        const auto key = model.AnchorKeyForRow(TARGET_ROW);
        QVERIFY(key.has_value());
        QSignalSpy addSpy(hm, &HistogramModel::anchorBucketsChanged);
        anchors.SetAnchor(*key, TARGET_SLOT);
        ExpectAnchorBucketsChanged(addSpy);
        QVERIFY(hm->HasAnchorTicks());

        // Fresh spy for the removal so the earlier add emission
        // doesn't satisfy the wait prematurely.
        QSignalSpy removeSpy(hm, &HistogramModel::anchorBucketsChanged);
        anchors.RemoveAnchor(*key);
        ExpectAnchorBucketsChanged(removeSpy);

        QVERIFY2(!hm->HasAnchorTicks(), "HasAnchorTicks must flip back to false after the last anchor is removed");
        const auto anchorMasks = hm->AnchorSlotsPerBucket();
        QVERIFY2(
            anchorMasks.empty() || anchorMasks[TARGET_ROW].none(),
            "target bucket's mask must be cleared after RemoveAnchor"
        );
    }

    /// When the widget is much narrower than the bucket count, the
    /// paint routine folds adjacent buckets into one visual column
    /// (`stride > 1`). The anchor tick strip must fold the same way
    /// so a lonely anchor doesn't vanish just because its bucket
    /// happens to share a column with empty neighbours.
    static void TestAnchorTickHandlesStride()
    {
        AnchorManager anchors;
        LogModel model(/*parent=*/nullptr, /*theme=*/nullptr, /*anchors=*/&anchors);
        const HistogramDock dock(&model, /*theme=*/nullptr, /*anchors=*/&anchors);
        HistogramModel *hm = dock.ModelForTest();
        HistogramWidget *widget = dock.WidgetForTest();
        QVERIFY(hm != nullptr);
        QVERIFY(widget != nullptr);

        // 120 buckets against a ~40 px plot width forces stride ≥ 3.
        constexpr int ROWS = 120;
        const HistogramFixture fixture(ROWS, /*stepSeconds=*/1);
        StreamJsonInto(model, fixture);
        WaitForBucketsChanged(*hm);
        hm->SetBucketSize(loglib::HistogramBucketSize::OneSecond);
        QCOMPARE(hm->Index().Buckets().size(), static_cast<std::size_t>(ROWS));

        // Anchor a row somewhere in the middle so the fold has to
        // absorb it into an interior visual column.
        constexpr int TARGET_ROW = 60;
        constexpr std::uint8_t TARGET_SLOT = 2;
        const auto key = model.AnchorKeyForRow(TARGET_ROW);
        QVERIFY(key.has_value());
        QSignalSpy anchorSpy(hm, &HistogramModel::anchorBucketsChanged);
        anchors.SetAnchor(*key, TARGET_SLOT);
        ExpectAnchorBucketsChanged(anchorSpy);
        QVERIFY(hm->HasAnchorTicks());

        // Small widget -> narrow plot -> stride > 1.
        widget->resize(60, 100);
        QCoreApplication::processEvents();
        // Force at least one paint so any lazy layout state is
        // primed. `AnchorTickRect`'s stride is computed on the fly
        // from `width()`, so the resize above is enough on its own,
        // but the paint here also validates the mask helper.
        widget->repaint();

        // Locate the visual column that contains the anchored raw
        // bucket by inspecting each column's mask. At least one
        // column must carry the anchor bit; the widget's stride
        // fold is the same one paint uses, so hitting the assert
        // proves the paint sees the anchor.
        bool foundAnchoredColumn = false;
        for (std::size_t col = 0; std::cmp_less(col, widget->width()); ++col)
        {
            const auto mask = widget->VisualColumnAnchorMaskForTest(col);
            if (mask.test(TARGET_SLOT))
            {
                foundAnchoredColumn = true;
                break;
            }
        }
        QVERIFY2(foundAnchoredColumn, "stride>1 fold must preserve the anchor bit in some visual column");
    }

    /// Clicking directly on the anchor tick strip must route to
    /// `HistogramDock::anchorClicked` with the anchored source row,
    /// not `bucketClicked` with a bucket index. Without this the
    /// reader sees the tick, clicks on it, and lands on the bucket's
    /// first row instead of the anchor they visually pointed at.
    static void TestClickOnTickJumpsToAnchoredRow()
    {
        AnchorManager anchors;
        LogModel model(/*parent=*/nullptr, /*theme=*/nullptr, /*anchors=*/&anchors);
        const HistogramDock dock(&model, /*theme=*/nullptr, /*anchors=*/&anchors);
        HistogramModel *hm = dock.ModelForTest();
        HistogramWidget *widget = dock.WidgetForTest();
        QVERIFY(hm != nullptr);
        QVERIFY(widget != nullptr);

        constexpr int ROWS = 30;
        const HistogramFixture fixture(ROWS, /*stepSeconds=*/1);
        StreamJsonInto(model, fixture);
        WaitForBucketsChanged(*hm);
        hm->SetBucketSize(loglib::HistogramBucketSize::OneSecond);

        constexpr int TARGET_ROW = 5;
        constexpr std::uint8_t TARGET_SLOT = 3;
        const auto key = model.AnchorKeyForRow(TARGET_ROW);
        QVERIFY(key.has_value());
        QSignalSpy anchorBucketsSpy(hm, &HistogramModel::anchorBucketsChanged);
        anchors.SetAnchor(*key, TARGET_SLOT);
        ExpectAnchorBucketsChanged(anchorBucketsSpy);
        QVERIFY(hm->HasAnchorTicks());

        // Wide widget -> stride 1, so column N maps to raw bucket N.
        widget->resize(600, 100);
        QCoreApplication::processEvents();

        const QRect tickRect = widget->AnchorTickRectForTest();
        QVERIFY2(tickRect.height() > 0, "AnchorTickRect must be non-empty when an anchor exists");

        // Click on the tick strip inside the column that contains
        // the anchored row. `PlotRect().left()` + column offset
        // lands on the bar for row 5 at stride 1.
        const QRect plotRect = widget->PlotRectForTest();
        const double columnWidth = static_cast<double>(plotRect.width()) / static_cast<double>(ROWS);
        const int columnX = plotRect.left() + static_cast<int>((TARGET_ROW + 0.5) * columnWidth);
        const int tickY = tickRect.top() + (tickRect.height() / 2);
        const QPoint tickPoint(columnX, tickY);

        const QSignalSpy anchorSpy(&dock, &HistogramDock::anchorClicked);
        const QSignalSpy bucketSpy(&dock, &HistogramDock::bucketClicked);
        QVERIFY(anchorSpy.isValid());
        QVERIFY(bucketSpy.isValid());

        QMouseEvent press(
            QEvent::MouseButtonPress, tickPoint, widget->mapToGlobal(tickPoint), Qt::LeftButton, Qt::LeftButton, {}
        );
        QMouseEvent release(
            QEvent::MouseButtonRelease, tickPoint, widget->mapToGlobal(tickPoint), Qt::LeftButton, Qt::NoButton, {}
        );
        QApplication::sendEvent(widget, &press);
        QApplication::sendEvent(widget, &release);

        QCOMPARE(anchorSpy.count(), 1);
        QCOMPARE(bucketSpy.count(), 0);
        QCOMPARE(anchorSpy.first().value(0).toInt(), TARGET_ROW);
    }

    /// A click on the bar area (below the tick strip) must still
    /// route through `bucketClicked` even when the same column
    /// carries an anchor tick above. Guards against the tick-click
    /// path accidentally swallowing bar clicks that happen to land
    /// in an anchored column.
    static void TestClickBelowTickStripEmitsBucketClicked()
    {
        AnchorManager anchors;
        LogModel model(/*parent=*/nullptr, /*theme=*/nullptr, /*anchors=*/&anchors);
        const HistogramDock dock(&model, /*theme=*/nullptr, /*anchors=*/&anchors);
        HistogramModel *hm = dock.ModelForTest();
        HistogramWidget *widget = dock.WidgetForTest();
        QVERIFY(hm != nullptr);
        QVERIFY(widget != nullptr);

        constexpr int ROWS = 30;
        const HistogramFixture fixture(ROWS, /*stepSeconds=*/1);
        StreamJsonInto(model, fixture);
        WaitForBucketsChanged(*hm);
        hm->SetBucketSize(loglib::HistogramBucketSize::OneSecond);

        const auto key = model.AnchorKeyForRow(5);
        QVERIFY(key.has_value());
        QSignalSpy anchorBucketsSpy(hm, &HistogramModel::anchorBucketsChanged);
        anchors.SetAnchor(*key, 0);
        ExpectAnchorBucketsChanged(anchorBucketsSpy);

        widget->resize(600, 100);
        QCoreApplication::processEvents();

        // Mid-plot click sits well below the ~10 px tick strip at
        // the top, so the bar-click path should win.
        const QRect plotRect = widget->PlotRectForTest();
        const QPoint bar(plotRect.center());

        const QSignalSpy anchorSpy(&dock, &HistogramDock::anchorClicked);
        const QSignalSpy bucketSpy(&dock, &HistogramDock::bucketClicked);
        QVERIFY(anchorSpy.isValid());
        QVERIFY(bucketSpy.isValid());

        QMouseEvent press(QEvent::MouseButtonPress, bar, widget->mapToGlobal(bar), Qt::LeftButton, Qt::LeftButton, {});
        QMouseEvent release(
            QEvent::MouseButtonRelease, bar, widget->mapToGlobal(bar), Qt::LeftButton, Qt::NoButton, {}
        );
        QApplication::sendEvent(widget, &press);
        QApplication::sendEvent(widget, &release);

        QCOMPARE(bucketSpy.count(), 1);
        QCOMPARE(anchorSpy.count(), 0);
    }

    /// Adding an anchor must not resize `PlotRect`: the tick strip
    /// is an *overlay* over the top of the bars, not reserved chrome
    /// above them. Guarantees the bars don't visibly reflow when the
    /// user toggles an anchor mid-session.
    static void TestPlotRectStableAcrossAnchorToggle()
    {
        AnchorManager anchors;
        LogModel model(/*parent=*/nullptr, /*theme=*/nullptr, /*anchors=*/&anchors);
        const HistogramDock dock(&model, /*theme=*/nullptr, /*anchors=*/&anchors);
        HistogramModel *hm = dock.ModelForTest();
        HistogramWidget *widget = dock.WidgetForTest();
        QVERIFY(hm != nullptr);
        QVERIFY(widget != nullptr);

        const HistogramFixture fixture(10, /*stepSeconds=*/1);
        StreamJsonInto(model, fixture);
        WaitForBucketsChanged(*hm);

        widget->resize(400, 120);
        const QRect plotBefore = widget->PlotRectForTest();
        QVERIFY(plotBefore.height() > 0);

        const auto key = model.AnchorKeyForRow(3);
        QVERIFY(key.has_value());
        QSignalSpy anchorSpy(hm, &HistogramModel::anchorBucketsChanged);
        anchors.SetAnchor(*key, 1);
        ExpectAnchorBucketsChanged(anchorSpy);
        QVERIFY(hm->HasAnchorTicks());

        const QRect plotAfter = widget->PlotRectForTest();
        QCOMPARE(plotAfter, plotBefore);
    }

    /// Without any anchor, the tick strip must not reserve any
    /// pixels above the plot rect. Otherwise every log that never
    /// anchors a row pays a chunk of visual budget for a feature
    /// it doesn't use. Compares `PlotRect().height()` against the
    /// baseline height of a dock with an empty `AnchorManager`.
    static void TestAnchorTickHiddenWithoutAnchors()
    {
        // Baseline: no anchor manager wired at all.
        LogModel modelBaseline;
        const HistogramDock dockBaseline(&modelBaseline, /*theme=*/nullptr, /*anchors=*/nullptr);
        HistogramWidget *widgetBaseline = dockBaseline.WidgetForTest();
        QVERIFY(widgetBaseline != nullptr);
        widgetBaseline->resize(400, 120);
        const int baselineHeight = widgetBaseline->AnchorTickStripHeightForTest();
        QCOMPARE(baselineHeight, 0);

        // Wired anchor manager but empty: identical layout.
        AnchorManager emptyAnchors;
        LogModel modelWithEmptyAnchors(/*parent=*/nullptr, /*theme=*/nullptr, /*anchors=*/&emptyAnchors);
        const HistogramDock dockEmpty(&modelWithEmptyAnchors, /*theme=*/nullptr, /*anchors=*/&emptyAnchors);
        HistogramWidget *widgetEmpty = dockEmpty.WidgetForTest();
        QVERIFY(widgetEmpty != nullptr);
        widgetEmpty->resize(400, 120);
        QCOMPARE(widgetEmpty->AnchorTickStripHeightForTest(), 0);

        // Even with rows streamed in (but no anchors), the strip
        // must stay collapsed.
        AnchorManager streamedAnchors;
        LogModel modelStreamed(/*parent=*/nullptr, /*theme=*/nullptr, /*anchors=*/&streamedAnchors);
        const HistogramDock dockStreamed(&modelStreamed, /*theme=*/nullptr, /*anchors=*/&streamedAnchors);
        HistogramWidget *widgetStreamed = dockStreamed.WidgetForTest();
        QVERIFY(widgetStreamed != nullptr);
        widgetStreamed->resize(400, 120);
        HistogramModel *hmStreamed = dockStreamed.ModelForTest();
        QVERIFY(hmStreamed != nullptr);
        const HistogramFixture fixture(10, /*stepSeconds=*/1);
        StreamJsonInto(modelStreamed, fixture);
        WaitForBucketsChanged(*hmStreamed);
        QCOMPARE(widgetStreamed->AnchorTickStripHeightForTest(), 0);
    }
};

QTEST_MAIN(HistogramDockTest)

#include "test_histogram_dock.moc"
