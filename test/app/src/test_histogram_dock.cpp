// GUI tests for the histogram strip (ROADMAP item 2). Covers
// `HistogramModel` streaming updates, `HistogramWidget` click / drag
// / Esc-cancel, and `HistogramDock::closed()`.
//
// Kept in its own binary (like `apptest_queue` / `apptest_theme`)
// so the concern is runnable in isolation via `apptest_histogram`.

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

// N rows of `{time, level, body}` JSON. Timestamps step by
// @p stepSeconds from 2026-01-01, so a 1 s rung produces exactly
// N buckets. `time` triggers `Type::Time` promotion; `level`
// triggers canonical-level promotion.
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
            // Time column ships parseFormats {%FT%T%Ez, %F %T%Ez,
            // %FT%T, %F %T}; no trailing "Z" form, so use %FT%T.
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

// Like `HistogramFixture` but `time` is emitted last with an
// integer `line_number` ahead of it. Forces `LogModel` to bubble
// Time from position 3 to 0, exercising `HistogramModel`'s
// `columnsMoved` refresh path. Pre-fix regression read
// `line_number` as microseconds and collapsed the range to 1970.
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

// Feed the JSON at @p path into @p model synchronously (mirrors
// the `main_window_test.cpp` pattern).
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

// Wait for at least one `bucketsChanged`. The signal is coalesced
// through a 50 ms timer, so we spin instead of asserting inline.
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

// Wait for at least one `anchorBucketsChanged` on a pre-attached
// spy. The signal is synchronous, so the spy must be created BEFORE
// the anchor mutation; we still spin briefly in case a coalesce
// timer is added later.
void ExpectAnchorBucketsChanged(QSignalSpy &spy)
{
    QVERIFY(spy.isValid());
    if (spy.count() > 0)
    {
        return;
    }
    QVERIFY2(spy.wait(1000), "anchorBucketsChanged must arrive after an anchor mutation");
}

// Drive `MainWindow::OpenFilesForTest` and wait for
// `LogModel::streamingFinished` (mirrors the `apptest` idiom).
void OpenFixtureInMainWindow(MainWindow &window, const HistogramFixture &fixture)
{
    auto *model = window.findChild<LogModel *>();
    QVERIFY(model != nullptr);
    // `QVERIFY` in a non-slot helper only records a failure — an
    // explicit return is needed to avoid a null-deref below.
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
    // Pump once more so the coalesced `bucketsChanged` timer fires.
    QCoreApplication::processEvents();
}

} // namespace

// NOLINTNEXTLINE(misc-use-internal-linkage): `Q_OBJECT` QtTest fixture.
class HistogramDockTest : public QObject
{
    Q_OBJECT

private slots:
    /// Install the timezone database so `date::CurrentZone()` returns
    /// a live pointer when the widget formats labels. Without it, the
    /// first `zoned_time` throws and the test binary crashes on the
    /// first paint over a populated index.
    void initTestCase()
    {
        QVERIFY2(
            MainWindow::InitializeTimezoneDatabase(),
            "Failed to initialise timezone database; see qCritical above. The staged "
            "`tzdata/` directory must live next to the apptest binary "
            "(handled by test/app/CMakeLists.txt)."
        );
    }

    /// No time column -> empty state: no buckets, `HasTimeColumn` false.
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
    /// bucket index and flips `HasTimeColumn` to true.
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

    /// Regression: when `time` isn't the first field, `LogModel`
    /// bubbles the newly observed `Type::Time` column to position 0
    /// after `AppendBatch`. `HistogramModel`'s cached column indices
    /// must react, otherwise a rebuild reads the wrong column (e.g.
    /// `line_number` as microseconds, collapsing the range to ~1970).
    /// The fixture places `time` last with a numeric field ahead of
    /// it; the observed range must survive the bubble intact in 2026.
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

        // Time column must have been bubbled to position 0.
        QVERIFY(hm->HasTimeColumn());
        QCOMPARE(hm->TimeColumnIndex(), 0);

        QCOMPARE(hm->Index().TotalRowCount(), static_cast<std::uint64_t>(ROWS));

        // Observed range must land in 2026, not the ~1970 window you
        // get when `line_number` is read as microseconds.
        const auto range = hm->ObservedRange();
        QVERIFY2(range.has_value(), "observed range must be populated after streaming");
        constexpr std::int64_t YEAR_2026_EPOCH_US = 1'767'225'600LL * 1'000'000LL;
        const std::int64_t minUs = range->min.time_since_epoch().count();
        const std::int64_t maxUs = range->max.time_since_epoch().count();
        QVERIFY2(
            minUs >= YEAR_2026_EPOCH_US,
            "observed min timestamp fell before 2026 -- histogram is likely reading a non-time column"
        );
        QVERIFY(maxUs >= minUs);
        // 30 rows, 1 s apart -> span >= 29 s.
        constexpr std::int64_t ONE_SECOND_US = 1'000'000LL;
        QVERIFY(maxUs - minUs >= 29LL * ONE_SECOND_US);
    }

    /// Cached `LevelColumnIndex` mirrors `LogModel::FirstLevelColumnIndex`
    /// after streaming, and at least one canonical level lands in a
    /// bucket (catches a `LevelForRow` bug that always returns
    /// `Unknown` yet passes row-count assertions).
    static void TestLevelColumnIndexTracksLogModel()
    {
        LogModel model;
        const HistogramDock dock(&model, /*theme=*/nullptr, /*anchors=*/nullptr);
        HistogramModel *hm = dock.ModelForTest();
        QVERIFY(hm != nullptr);

        QCOMPARE(hm->LevelColumnIndex(), -1);

        const HistogramFixture fixture(30, /*stepSeconds=*/1);
        StreamJsonInto(model, fixture);
        WaitForBucketsChanged(*hm);

        // Fixture's canonical `level` values promote the column to
        // `Type::Level`; cached mirror must agree.
        const int expected = model.FirstLevelColumnIndex();
        QVERIFY2(expected >= 0, "fixture must produce a Type::Level column");
        QCOMPARE(hm->LevelColumnIndex(), expected);

        // Rows land in slot 0 (Unknown) when `mLevelColumnIndex < 0`,
        // so zero canonical rows means the level column isn't read.
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

    /// Idle: data loaded, no hover -> details strip shows the plot
    /// summary. Guards against a stale `mLastHoverBucket` leaving
    /// hover text visible without a pointer on the widget.
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

        // No hover dispatched -> idle mode. Plot summary starts with
        // `bucket:` and lists the row count.
        QCOMPARE(widget->LastHoverBucketForTest(), -1);
        const QString details = widget->DetailsTextForTest();
        QVERIFY2(details.startsWith(QStringLiteral("bucket:")), qPrintable(details));
        QVERIFY2(details.contains(QStringLiteral("rows:")), qPrintable(details));
    }

    /// Hover: pointing at a populated bar switches the details strip
    /// to the hovered-bucket format. `mLastHoverBucket` is the
    /// observable latch.
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

        // The fixture packs 30 rows across the width, so the widget
        // centre is guaranteed to land inside a bar.
        const QPoint centre(widget->width() / 2, widget->height() / 2);
        QMouseEvent hover(QEvent::MouseMove, centre, widget->mapToGlobal(centre), Qt::NoButton, Qt::NoButton, {});
        QApplication::sendEvent(widget, &hover);
        QVERIFY2(widget->LastHoverBucketForTest() >= 0, "hover over a populated bar must latch a visual-column index");

        const QString details = widget->DetailsTextForTest();
        // Hover format: "total:" plus at least one canonical
        // "<Level>: <count>" fragment. `CanonicalLevelName`
        // capitalises the level (e.g. "Info: 5").
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

        // Dedup: a follow-up move inside the same bar mustn't
        // flip the cached column index. A regression here would
        // spuriously flip through -1 on every mouse motion.
        const int firstColumn = widget->LastHoverBucketForTest();
        const QPoint nudge(centre.x() + 1, centre.y());
        QMouseEvent tinyMove(QEvent::MouseMove, nudge, widget->mapToGlobal(nudge), Qt::NoButton, Qt::NoButton, {});
        QApplication::sendEvent(widget, &tinyMove);
        QCOMPARE(widget->LastHoverBucketForTest(), firstColumn);
    }

    /// `leaveEvent` resets the hover cache so the details strip
    /// snaps back to the plot summary. Without this, the readout
    /// would linger and the dedup guard would swallow the next
    /// entry over the same column.
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
        // Cache reset is the signal that `leaveEvent` ran; the
        // details text then falls through to the plot summary.
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

    /// Press-move-release past the drag threshold emits
    /// `timeRangeSelected` with `from <= to`.
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
        // Drag must not also fire `bucketClicked`.
        QCOMPARE(clickSpy.count(), 0);
        const auto &args = rangeSpy.first();
        const qint64 fromUs = args.value(0).toLongLong();
        const qint64 toUs = args.value(1).toLongLong();
        QVERIFY(fromUs <= toUs);
    }

    /// Esc during a drag cancels: no `timeRangeSelected` on release,
    /// and the drag brush disappears.
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

        // The drag was cancelled, so the release must not emit a range.
        QMouseEvent release(QEvent::MouseButtonRelease, middle, widget->mapToGlobal(middle), Qt::LeftButton, Qt::NoButton, {});
        QApplication::sendEvent(widget, &release);

        QCOMPARE(rangeSpy.count(), 0);
    }

    /// `Z` / `Shift+Z` walk the bucket rung.
    static void TestZoomKeysWalkTheLadder()
    {
        LogModel model;
        const HistogramDock dock(&model, /*theme=*/nullptr, /*anchors=*/nullptr);
        HistogramModel *hm = dock.ModelForTest();
        HistogramWidget *widget = dock.WidgetForTest();
        QVERIFY(hm != nullptr);
        QVERIFY(widget != nullptr);

        // 30 s of data: auto rung is OneSecond, so Shift+Z grows it.
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

    /// `closed()` fires on `closeEvent`; matches the `AnchorsDock`
    /// contract used by `WireDockToggle`.
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

    /// `FirstRowInBucket` returns the first source row in that
    /// bucket. With 1 s buckets stepped 1 s apart, bucket `i`
    /// contains row `i`.
    static void TestFirstRowInBucketMapsToRowIndex()
    {
        LogModel model;
        const HistogramDock dock(&model, /*theme=*/nullptr, /*anchors=*/nullptr);
        HistogramModel *hm = dock.ModelForTest();
        QVERIFY(hm != nullptr);

        const HistogramFixture fixture(10, /*stepSeconds=*/1);
        StreamJsonInto(model, fixture);
        WaitForBucketsChanged(*hm);
        // Pin the rung so this isn't racing `ApplyAutoBucketSize`.
        // `SetBucketSize` rebuilds synchronously.
        hm->SetBucketSize(loglib::HistogramBucketSize::OneSecond);

        QCOMPARE(hm->FirstRowInBucket(0), 0);
        QCOMPARE(hm->FirstRowInBucket(5), 5);
        // At least 10 buckets (fencepost may add one).
        QVERIFY(hm->Index().Buckets().size() >= 10);
    }

    /// Regression: after `SetBucketSize` pinned the rung,
    /// `ApplyAutoBucketSize` was silently vetoed and the widget's
    /// "Reset zoom (auto)" entry became inert. `ResetBucketSizeToAuto`
    /// must drop the pin and re-pick a rung.
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
    /// anchor a click / drag. The widget now rejects presses outside
    /// `PlotRect()` so an accidental touch on the details strip
    /// can't jump the table. Chrome now lives at the *bottom* — the
    /// old top subtitle moved into the hover-readout strip.
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

        // y = height - 4 sits inside the ~18 px details strip at the
        // bottom, so this press must be rejected entirely (no drag
        // anchor, no click on release).
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

        // Sanity: a press inside the plot rect still emits a click.
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

    /// End-to-end: `AddTimeRangeFilterFromHistogram` installs a
    /// `Type::Time` filter under the sentinel `histogram-time-range`
    /// ID so a second drag replaces it instead of stacking. Guards
    /// the plumbing from `mouseReleaseEvent` to `MainWindow::mFilters`.
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

        // A second drag must replace the filter, not stack a second.
        window.AddTimeRangeFilterFromHistogram(from, from);
        const auto &filters2 = window.Filters();
        QCOMPARE(filters2.count("histogram-time-range"), std::size_t{1});
        QCOMPARE(*filters2.at("histogram-time-range").filterEnd, from);
    }

    /// End-to-end: `JumpToFirstRowInBucket` moves the table's
    /// current selection to the source row of the first row in
    /// that bucket. With 1 s rows and the auto rung `OneSecond`,
    /// bucket `0` maps to source row `0`.
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
        // Pin the rung so the bucket->row mapping is deterministic.
        hm->SetBucketSize(loglib::HistogramBucketSize::OneSecond);

        window.JumpToFirstRowInBucket(0);

        auto *table = window.findChild<QTableView *>();
        QVERIFY(table != nullptr);
        const QModelIndex currentProxy = table->currentIndex();
        QVERIFY2(currentProxy.isValid(), "JumpToFirstRowInBucket must move the current index");
        // Current index lives in the proxy chain; walk each proxy
        // to reach the source row.
        QModelIndex idx = currentProxy;
        while (const auto *proxy = qobject_cast<const QAbstractProxyModel *>(idx.model()))
        {
            idx = proxy->mapToSource(idx);
        }
        QCOMPARE(idx.row(), 0);
    }

    /// Anchoring a row flips the matching bucket's slot bit in
    /// `AnchorSlotsPerBucket()` and turns on `HasAnchorTicks()`.
    /// Streams 10 rows 1 s apart at the finest rung (one row per
    /// bucket), anchors row 5 at slot 3, inspects the bitmask.
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
        // Every other bucket must stay empty (no OR into neighbours).
        for (std::size_t i = 0; i < anchorMasks.size(); ++i)
        {
            if (i == static_cast<std::size_t>(TARGET_ROW))
            {
                continue;
            }
            QVERIFY2(anchorMasks[i].none(), "non-target buckets must stay empty");
        }
    }

    /// Removing the only anchor clears the bucket's mask and flips
    /// `HasAnchorTicks()` back to false. Guards a leak where the
    /// incremental update forgot to decrement the popcount cache.
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

        // Fresh spy so the earlier add emission doesn't satisfy the
        // wait prematurely.
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

    /// When the widget is narrower than the bucket count, paint folds
    /// adjacent buckets into one visual column (`stride > 1`). The
    /// anchor tick strip must fold the same way, or a lonely anchor
    /// vanishes when its bucket shares a column with empty neighbours.
    static void TestAnchorTickHandlesStride()
    {
        AnchorManager anchors;
        LogModel model(/*parent=*/nullptr, /*theme=*/nullptr, /*anchors=*/&anchors);
        const HistogramDock dock(&model, /*theme=*/nullptr, /*anchors=*/&anchors);
        HistogramModel *hm = dock.ModelForTest();
        HistogramWidget *widget = dock.WidgetForTest();
        QVERIFY(hm != nullptr);
        QVERIFY(widget != nullptr);

        // 120 buckets against a ~40 px plot width forces stride >= 3.
        constexpr int ROWS = 120;
        const HistogramFixture fixture(ROWS, /*stepSeconds=*/1);
        StreamJsonInto(model, fixture);
        WaitForBucketsChanged(*hm);
        hm->SetBucketSize(loglib::HistogramBucketSize::OneSecond);
        QCOMPARE(hm->Index().Buckets().size(), static_cast<std::size_t>(ROWS));

        // Anchor mid-way so the fold has to absorb it into an
        // interior visual column.
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
        // Force one paint so any lazy layout state is primed and the
        // mask helper is exercised (stride is computed live from
        // `width()`, so the resize alone would suffice).
        widget->repaint();

        // Find the visual column carrying the anchor bit. Paint uses
        // the same fold, so this proves paint sees the anchor.
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

    /// Clicking on the anchor tick strip routes to `anchorClicked`
    /// with the anchored source row, not `bucketClicked` with a
    /// bucket index. Without this the click lands on the bucket's
    /// first row instead of the anchor the user pointed at.
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

        // Click on the tick strip inside the anchored column.
        // At stride 1, column N is raw bucket N.
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

    /// A click below the tick strip still routes through
    /// `bucketClicked`, even when the same column carries an anchor
    /// tick above. Guards against the tick-click path swallowing
    /// bar clicks in anchored columns.
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

        // Mid-plot click sits well below the ~10 px tick strip, so
        // the bar-click path should win.
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

    /// Adding an anchor must not resize `PlotRect`: the tick strip is
    /// an overlay over the top of the bars, not reserved chrome. This
    /// keeps the bars from visibly reflowing on anchor toggles.
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

    /// Without an anchor, the tick strip reserves no pixels. Compares
    /// `AnchorTickStripHeightForTest()` against three baselines: no
    /// anchor manager, empty manager, streamed rows but no anchors.
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

        // Empty anchor manager wired: same layout as the baseline.
        AnchorManager emptyAnchors;
        LogModel modelWithEmptyAnchors(/*parent=*/nullptr, /*theme=*/nullptr, /*anchors=*/&emptyAnchors);
        const HistogramDock dockEmpty(&modelWithEmptyAnchors, /*theme=*/nullptr, /*anchors=*/&emptyAnchors);
        HistogramWidget *widgetEmpty = dockEmpty.WidgetForTest();
        QVERIFY(widgetEmpty != nullptr);
        widgetEmpty->resize(400, 120);
        QCOMPARE(widgetEmpty->AnchorTickStripHeightForTest(), 0);

        // Rows streamed in but no anchors -> strip stays collapsed.
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
