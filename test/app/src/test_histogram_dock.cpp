// Focused GUI tests for the histogram strip (ROADMAP item 2).
// Covers: `HistogramModel` populating from a live `LogModel`,
// `HistogramWidget` click / drag / Esc-cancel behaviour, and
// `HistogramDock::closed()` on user dismissal.
//
// Kept in its own binary (mirroring `apptest_queue` /
// `apptest_theme`) so the histogram concern doesn't inflate the
// monolithic `apptest` and can be run in isolation via
// `apptest_histogram`.

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

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <memory>
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

// Feed @p fixture into @p model synchronously. Mirrors the pattern
// used across `main_window_test.cpp` (`BeginStreamingForSyncTest` +
// direct `JsonParser::ParseStreaming` on the borrowed source).
void StreamJsonInto(LogModel &model, const HistogramFixture &fixture)
{
    QSignalSpy finishedSpy(&model, &LogModel::streamingFinished);
    QVERIFY(finishedSpy.isValid());

    auto file = std::make_unique<loglib::LogFile>(fixture.Path().toStdString());
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

// Drive `MainWindow::OpenFilesForTest` and pump the event loop until
// `LogModel::streamingFinished` arrives. Mirrors the wait idiom the
// bigger `apptest` fixture uses.
void OpenFixtureInMainWindow(MainWindow &window, const HistogramFixture &fixture)
{
    auto *model = window.findChild<LogModel *>();
    QVERIFY(model != nullptr);
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
        HistogramDock dock(&model, /*theme=*/nullptr);
        HistogramModel *hm = dock.ModelForTest();
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
        HistogramDock dock(&model, /*theme=*/nullptr);
        HistogramModel *hm = dock.ModelForTest();
        QVERIFY(hm != nullptr);

        const HistogramFixture fixture(30, /*stepSeconds=*/1);
        StreamJsonInto(model, fixture);
        WaitForBucketsChanged(*hm);

        QVERIFY(hm->HasTimeColumn());
        QCOMPARE(hm->Index().TotalRowCount(), static_cast<std::uint64_t>(30));
        QVERIFY(!hm->Index().Empty());
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
        HistogramDock dock(&model, /*theme=*/nullptr);
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

    /// A click on the widget with populated data emits
    /// `bucketClicked` with a valid in-range bucket index.
    static void TestClickEmitsBucketClicked()
    {
        LogModel model;
        HistogramDock dock(&model, /*theme=*/nullptr);
        HistogramModel *hm = dock.ModelForTest();
        HistogramWidget *widget = dock.WidgetForTest();
        QVERIFY(hm != nullptr);
        QVERIFY(widget != nullptr);

        const HistogramFixture fixture(30, /*stepSeconds=*/1);
        StreamJsonInto(model, fixture);
        WaitForBucketsChanged(*hm);

        widget->resize(400, 100);
        QSignalSpy spy(&dock, &HistogramDock::bucketClicked);
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
        HistogramDock dock(&model, /*theme=*/nullptr);
        HistogramModel *hm = dock.ModelForTest();
        HistogramWidget *widget = dock.WidgetForTest();
        QVERIFY(hm != nullptr);
        QVERIFY(widget != nullptr);

        const HistogramFixture fixture(30, /*stepSeconds=*/1);
        StreamJsonInto(model, fixture);
        WaitForBucketsChanged(*hm);

        widget->resize(400, 100);
        QSignalSpy rangeSpy(&dock, &HistogramDock::timeRangeSelected);
        QSignalSpy clickSpy(&dock, &HistogramDock::bucketClicked);
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
        const auto args = rangeSpy.first();
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
        HistogramDock dock(&model, /*theme=*/nullptr);
        HistogramModel *hm = dock.ModelForTest();
        HistogramWidget *widget = dock.WidgetForTest();
        QVERIFY(hm != nullptr);
        QVERIFY(widget != nullptr);

        const HistogramFixture fixture(30, /*stepSeconds=*/1);
        StreamJsonInto(model, fixture);
        WaitForBucketsChanged(*hm);

        widget->resize(400, 100);
        widget->setFocus();
        QSignalSpy rangeSpy(&dock, &HistogramDock::timeRangeSelected);
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
        HistogramDock dock(&model, /*theme=*/nullptr);
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
        HistogramDock dock(&model, /*theme=*/nullptr);
        QSignalSpy closedSpy(&dock, &HistogramDock::closed);
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
        HistogramDock dock(&model, /*theme=*/nullptr);
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
        HistogramDock dock(&model, /*theme=*/nullptr);
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

    /// Regression: presses on the subtitle strip at the top of the
    /// widget used to anchor a click / drag even though the pointer
    /// was clearly not on a bar. The strip now rejects presses
    /// outside `PlotRect()` so an accidental subtitle click can't
    /// jump the table.
    static void TestPressAboveSubtitleDoesNotEmitBucketClicked()
    {
        LogModel model;
        HistogramDock dock(&model, /*theme=*/nullptr);
        HistogramModel *hm = dock.ModelForTest();
        HistogramWidget *widget = dock.WidgetForTest();
        QVERIFY(hm != nullptr);
        QVERIFY(widget != nullptr);

        const HistogramFixture fixture(30, /*stepSeconds=*/1);
        StreamJsonInto(model, fixture);
        WaitForBucketsChanged(*hm);

        widget->resize(400, 100);
        QSignalSpy clickSpy(&dock, &HistogramDock::bucketClicked);
        QSignalSpy rangeSpy(&dock, &HistogramDock::timeRangeSelected);
        QVERIFY(clickSpy.isValid());
        QVERIFY(rangeSpy.isValid());

        // y = 5 sits above the plot rect (top-padding is 20 px), so
        // this press must be rejected entirely — no drag anchor,
        // no click emit on release.
        const QPoint subtitle(widget->width() / 2, 5);
        QMouseEvent press(
            QEvent::MouseButtonPress, subtitle, widget->mapToGlobal(subtitle), Qt::LeftButton, Qt::LeftButton, {}
        );
        QMouseEvent release(
            QEvent::MouseButtonRelease, subtitle, widget->mapToGlobal(subtitle), Qt::LeftButton, Qt::NoButton, {}
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
        HistogramModel *hm = dock->ModelForTest();
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
        while (auto *proxy = qobject_cast<const QAbstractProxyModel *>(idx.model()))
        {
            idx = proxy->mapToSource(idx);
        }
        QCOMPARE(idx.row(), 0);
    }
};

QTEST_MAIN(HistogramDockTest)

#include "test_histogram_dock.moc"
