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
#include <QApplication>
#include <QCloseEvent>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QSignalSpy>
#include <QString>
#include <QStringList>
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
};

QTEST_MAIN(HistogramDockTest)

#include "test_histogram_dock.moc"
