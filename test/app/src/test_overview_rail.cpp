// GUI tests for the overview rail (ROADMAP item 13). Covers
// `OverviewRailModel` bucketing / tick tracking,
// `OverviewRailWidget` sizing / paint contract, the
// `LogTableView::AttachOverviewRail` viewport-margin hook, and
// the MainWindow toggle + find-cache push wiring.
//
// Kept in its own binary (mirroring `apptest_histogram` /
// `apptest_theme`) so the concern is runnable in isolation via
// `apptest_overview_rail`.

#include "anchor_manager.hpp"
#include "find_dock.hpp"
#include "log_filter_model.hpp"
#include "log_model.hpp"
#include "log_table_view.hpp"
#include "main_window.hpp"
#include "overview_rail_model.hpp"
#include "overview_rail_widget.hpp"
#include "qt_streaming_log_sink.hpp"
#include "row_order_proxy_model.hpp"
#include "theme_control.hpp"

#include <loglib/file_line_source.hpp>
#include <loglib/internal/advanced_parser_options.hpp>
#include <loglib/log_file.hpp>
#include <loglib/log_filter.hpp>
#include <loglib/log_level.hpp>
#include <loglib/log_parse_sink.hpp>
#include <loglib/parser_options.hpp>
#include <loglib/parsers/json_parser.hpp>
#include <loglib/stop_token.hpp>
#include <loglib/theme.hpp>

#include <QAbstractItemModel>
#include <QAction>
#include <QApplication>
#include <QCoreApplication>
#include <QImage>
#include <QItemSelectionModel>
#include <QMetaObject>
#include <QModelIndex>
#include <QPainter>
#include <QRect>
#include <QScrollBar>
#include <QSet>
#include <QSettings>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QToolBar>
#include <QVariant>
#include <QtTest/QtTest>

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

namespace
{

// N rows of `{time, level, body}` JSON. `level` cycles through
// four canonical levels so buckets carry a mix of severities and
// dominant-level logic has something to disambiguate.
class RailFixture
{
public:
    explicit RailFixture(int rows)
    {
        QVERIFY2(mDir.isValid(), "QTemporaryDir creation must succeed");
        mPath = mDir.filePath("rail.jsonl");
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
            const int hour = i / 3600;
            const int minute = (i / 60) % 60;
            const int second = i % 60;
            const QString line = QStringLiteral(R"({"time": "2026-01-01T%1:%2:%3", "level": "%4", "body": "msg %5"})")
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

// Feed the JSON at @p path into @p model synchronously (mirrors
// the `test_histogram_dock.cpp` pattern).
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

// Spin briefly for at least one `bucketsChanged`. Coalesced
// through a 50 ms timer (mirrors the histogram model).
void WaitForBucketsChanged(OverviewRailModel &model)
{
    QSignalSpy spy(&model, &OverviewRailModel::bucketsChanged);
    QVERIFY(spy.isValid());
    if (spy.count() > 0)
    {
        return;
    }
    QVERIFY2(spy.wait(1000), "bucketsChanged must arrive within the timeout");
}

// Wrap the proxy chain a production MainWindow builds so tests
// can drive the model against realistic mapToSource walks.
struct ProxyChain
{
    RowOrderProxyModel *rowOrder;
    LogFilterModel *filter;
};

ProxyChain BuildProxyChain(LogModel *model, QObject *parent)
{
    auto *rowOrder = new RowOrderProxyModel(parent);
    rowOrder->setSourceModel(model);

    auto *filter = new LogFilterModel(parent);
    filter->setSourceModel(rowOrder);
    filter->SetLogModel(model);
    return {.rowOrder = rowOrder, .filter = filter};
}

} // namespace

// NOLINTNEXTLINE(misc-use-internal-linkage): `Q_OBJECT` QtTest fixture.
class OverviewRailTest : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        QVERIFY2(
            MainWindow::InitializeTimezoneDatabase(), "Failed to initialise timezone database; see qCritical above."
        );
        // Route every QSettings / QStandardPaths lookup this
        // suite makes to a throw-away location. Prevents the
        // MainWindow toggle test from writing real registry keys
        // on Windows, and pins the test's org / app name so slot
        // ordering can't leak state between tests.
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName(QStringLiteral("StructuredLogViewerTest"));
        QCoreApplication::setApplicationName(QStringLiteral("OverviewRailTest"));
    }

    void cleanupTestCase()
    {
        // Belt-and-braces cleanup: even under `setTestModeEnabled`
        // the file/registry hive lingers on disk. Clearing keeps
        // successive local runs deterministic.
        QSettings settings;
        settings.clear();
    }

    /// Empty model + no anchors -> zero row count, no anchor
    /// ticks. Confirms `Rebuild()` doesn't populate spurious
    /// buckets when there's nothing to bucket.
    static void TestEmptyModelHasNoBuckets()
    {
        LogModel model;
        QObject owner;
        const auto chain = BuildProxyChain(&model, &owner);
        OverviewRailModel rail(chain.filter, &model, /*anchors=*/nullptr);
        rail.SetBucketCount(32);
        QCOMPARE(rail.BucketCount(), static_cast<std::size_t>(32));
        QCOMPARE(rail.ProxyRowCount(), 0);
        QVERIFY(!rail.HasAnchorTicks());
        QVERIFY(!rail.HasMatchTicks());
        for (const auto &bucket : rail.Buckets())
        {
            QCOMPARE(bucket.levels.Total(), static_cast<std::uint32_t>(0));
            QCOMPARE(bucket.matchCount, static_cast<std::uint32_t>(0));
            QVERIFY(bucket.anchorSlots.none());
        }
    }

    /// After streaming 40 rows the rail's row count matches the
    /// proxy's, buckets sum to the total, and dominant levels
    /// include at least one non-Unknown entry (catches a bug
    /// where `LevelForSourceRow` always returns `Unknown`).
    static void TestBucketsPopulateFromStream()
    {
        LogModel model;
        QObject owner;
        const auto chain = BuildProxyChain(&model, &owner);
        OverviewRailModel rail(chain.filter, &model, /*anchors=*/nullptr);
        constexpr std::size_t BUCKET_COUNT = 40;
        rail.SetBucketCount(BUCKET_COUNT);

        constexpr int ROWS = 40;
        const RailFixture fixture(ROWS);
        StreamJsonPathInto(model, fixture.Path());
        WaitForBucketsChanged(rail);

        QCOMPARE(rail.ProxyRowCount(), ROWS);
        std::uint32_t total = 0;
        int nonUnknownBuckets = 0;
        for (const auto &bucket : rail.Buckets())
        {
            total += bucket.levels.Total();
            if (bucket.levels.Total() > 0)
            {
                const loglib::LogLevel dom = rail.DominantLevel(&bucket - rail.Buckets().data());
                if (dom != loglib::LogLevel::Unknown)
                {
                    ++nonUnknownBuckets;
                }
            }
        }
        QCOMPARE(total, static_cast<std::uint32_t>(ROWS));
        QVERIFY2(nonUnknownBuckets > 0, "every bucket resolved to Unknown -- level lookup is broken");
    }

    /// `SetMatchProxyRows` folds match ticks into the bucket
    /// vector and flips `HasMatchTicks`. Rows outside the proxy
    /// range are silently ignored (protects the drag scrub from
    /// stale cached rows).
    static void TestMatchRowsFoldIntoBuckets()
    {
        LogModel model;
        QObject owner;
        const auto chain = BuildProxyChain(&model, &owner);
        OverviewRailModel rail(chain.filter, &model, /*anchors=*/nullptr);
        rail.SetBucketCount(20);

        constexpr int ROWS = 20;
        const RailFixture fixture(ROWS);
        StreamJsonPathInto(model, fixture.Path());
        WaitForBucketsChanged(rail);
        QCOMPARE(rail.ProxyRowCount(), ROWS);

        QVERIFY(!rail.HasMatchTicks());
        // Rows 5, 10, 15, plus out-of-range 999 (must be dropped
        // silently) and negative -1 (also dropped).
        const QSignalSpy matchSpy(&rail, &OverviewRailModel::matchesChanged);
        rail.SetMatchProxyRows({-1, 5, 10, 15, 999});
        QVERIFY(rail.HasMatchTicks());
        QCOMPARE(matchSpy.count(), 1);

        std::uint32_t totalMatches = 0;
        for (const auto &bucket : rail.Buckets())
        {
            totalMatches += bucket.matchCount;
        }
        QCOMPARE(totalMatches, static_cast<std::uint32_t>(3));

        // Clearing drops ticks.
        rail.SetMatchProxyRows({});
        QVERIFY(!rail.HasMatchTicks());

        // `HasMatchTicks` reflects the *bucketed* count, not the
        // raw list. A caller pushing only out-of-range rows (all
        // silently dropped by the folder) must NOT see ticks.
        rail.SetMatchProxyRows({-1, 999, -5, 42});
        QVERIFY2(!rail.HasMatchTicks(), "all-out-of-range match rows must fold to zero ticks");
    }

    /// `SetMatchProxyRows` is the find-bar hot path — every
    /// keystroke calls it, so it must NOT walk the whole proxy
    /// to re-fill level counts / anchor bits. Pin the invariant
    /// by capturing per-bucket level counts, calling
    /// `SetMatchProxyRows`, and asserting the level counts are
    /// bit-for-bit unchanged (only `matchCount` moved).
    static void TestSetMatchProxyRowsLeavesLevelCountsUntouched()
    {
        LogModel model;
        QObject owner;
        const auto chain = BuildProxyChain(&model, &owner);
        OverviewRailModel rail(chain.filter, &model, /*anchors=*/nullptr);
        rail.SetBucketCount(20);

        constexpr int ROWS = 20;
        const RailFixture fixture(ROWS);
        StreamJsonPathInto(model, fixture.Path());
        WaitForBucketsChanged(rail);
        QCOMPARE(rail.ProxyRowCount(), ROWS);

        std::vector<loglib::LevelBucket> baseline;
        baseline.reserve(rail.BucketCount());
        for (const auto &bucket : rail.Buckets())
        {
            baseline.push_back(bucket.levels);
        }

        rail.SetMatchProxyRows({2, 7, 11});

        std::size_t i = 0;
        for (const auto &bucket : rail.Buckets())
        {
            QCOMPARE(bucket.levels.counts, baseline[i].counts);
            ++i;
        }
    }

    /// `Rebuild()` is coalesced through a 50 ms timer, so a
    /// burst of proxy signals must collapse to a single
    /// `bucketsChanged` emission (and a single walk of the
    /// row set). Without the coalesce, `LogFilterModel`'s
    /// per-row `rowsInserted` volley under an active sort would
    /// pay `O(rowCount)` per row.
    static void TestRebuildIsCoalesced()
    {
        LogModel model;
        QObject owner;
        const auto chain = BuildProxyChain(&model, &owner);
        OverviewRailModel rail(chain.filter, &model, /*anchors=*/nullptr);
        rail.SetBucketCount(16);

        constexpr int ROWS = 16;
        const RailFixture fixture(ROWS);
        StreamJsonPathInto(model, fixture.Path());
        WaitForBucketsChanged(rail);

        // Fire many rebuild requests inside one quiet window.
        // A single timeout should suffice for all of them.
        QSignalSpy spy(&rail, &OverviewRailModel::bucketsChanged);
        for (int i = 0; i < 10; ++i)
        {
            rail.Rebuild();
        }
        QVERIFY(spy.wait(1000));
        QCOMPARE_LE(spy.count(), 1);
    }

    /// `SetBucketCount(0)` -- the mechanism
    /// `MainWindow::SetOverviewRailVisible(false)` uses to stop
    /// paying rebuild cost while the rail is hidden -- must make
    /// subsequent proxy signals cheap. Assert `BucketCount()` is
    /// zero after the drop; the `RebuildInternal` short-circuit
    /// on `mBuckets.empty()` is what makes the hidden state free.
    static void TestSetBucketCountZeroDropsBuckets()
    {
        LogModel model;
        QObject owner;
        const auto chain = BuildProxyChain(&model, &owner);
        OverviewRailModel rail(chain.filter, &model, /*anchors=*/nullptr);
        rail.SetBucketCount(32);
        QCOMPARE(rail.BucketCount(), static_cast<std::size_t>(32));

        rail.SetBucketCount(0);
        QCOMPARE(rail.BucketCount(), static_cast<std::size_t>(0));
        QVERIFY(rail.Buckets().empty());
    }

    /// `ProxyRowForYPixel` maps rail Y linearly onto proxy rows,
    /// bypassing the bucket step. A click near the top of a
    /// bucket-spanning range must resolve to a lower row than a
    /// click near the bottom of the same range (klogg-style
    /// sub-bucket precision so scrubbing feels smooth).
    static void TestProxyRowForYPixelSubBucketPrecision()
    {
        LogModel model;
        QObject owner;
        const auto chain = BuildProxyChain(&model, &owner);
        OverviewRailModel rail(chain.filter, &model, /*anchors=*/nullptr);
        // Deliberately fewer buckets than rows so each bucket
        // spans several rows -- the sub-bucket mapping is only
        // observable under that ratio.
        rail.SetBucketCount(10);

        constexpr int ROWS = 100;
        const RailFixture fixture(ROWS);
        StreamJsonPathInto(model, fixture.Path());
        WaitForBucketsChanged(rail);

        // Bucket 0 spans rows [0, 10). Y=0 should land row 0,
        // Y=9 should land row >= 4 (a bucket-first mapping would
        // return 0 for both).
        QCOMPARE(rail.ProxyRowForYPixel(0, 100), 0);
        const int nearBucketBottom = rail.ProxyRowForYPixel(9, 100);
        QVERIFY2(nearBucketBottom >= 4, "sub-bucket precision must move within a single bucket");
    }

    /// Adding an anchor via `AnchorManager` flows into the
    /// rail's bit set and flips `HasAnchorTicks`. Reset drops
    /// the ticks and lowers the flag.
    static void TestAnchorTicksFollowAnchorManager()
    {
        AnchorManager anchors;
        // Model must be wired to the anchor manager so
        // `AnchorSlotForRow` resolves the slot the rail reads.
        LogModel model(/*parent=*/nullptr, /*theme=*/nullptr, &anchors);
        QObject owner;
        const auto chain = BuildProxyChain(&model, &owner);
        OverviewRailModel rail(chain.filter, &model, &anchors);
        rail.SetBucketCount(16);

        constexpr int ROWS = 16;
        const RailFixture fixture(ROWS);
        StreamJsonPathInto(model, fixture.Path());
        WaitForBucketsChanged(rail);

        QVERIFY(!rail.HasAnchorTicks());
        // Anchor row 8; the source model's canonical locator is
        // the temp file path. The proxy chain preserves row 8 as
        // proxy row 8 (no sort / filter installed).
        const auto keyOpt = model.AnchorKeyForRow(8);
        QVERIFY2(keyOpt.has_value(), "row 8 must have an anchor key after streaming");
        if (!keyOpt.has_value())
        {
            return;
        }
        // Anchor mutations flow through `Rebuild()`, which is
        // coalesced through the 50 ms rebuild timer. Waiting on
        // the signal spy synchronises the test against the timer
        // without hard-coding a sleep.
        QSignalSpy anchorSpy(&rail, &OverviewRailModel::anchorBucketsChanged);
        anchors.SetAnchor(*keyOpt, 3);
        QVERIFY2(anchorSpy.wait(1000), "anchor rebuild must emit anchorBucketsChanged within the timeout");
        QVERIFY(rail.HasAnchorTicks());

        anchors.ClearAll();
        QVERIFY2(anchorSpy.wait(1000), "anchor clear must emit anchorBucketsChanged within the timeout");
        QVERIFY(!rail.HasAnchorTicks());
    }

    /// `ProxyRowForYPixel` maps rail Y linearly onto proxy rows,
    /// clamped into `[0, rowCount)`, and `FirstProxyRowInBucket`
    /// inverts the mapping consistently.
    static void TestProxyRowForYPixelClampsAndInverts()
    {
        LogModel model;
        QObject owner;
        const auto chain = BuildProxyChain(&model, &owner);
        OverviewRailModel rail(chain.filter, &model, /*anchors=*/nullptr);
        rail.SetBucketCount(100);

        constexpr int ROWS = 100;
        const RailFixture fixture(ROWS);
        StreamJsonPathInto(model, fixture.Path());
        WaitForBucketsChanged(rail);

        // Top of the rail resolves to row 0.
        QCOMPARE(rail.ProxyRowForYPixel(0, 100), 0);
        // Middle resolves to row ~50.
        const int mid = rail.ProxyRowForYPixel(50, 100);
        QVERIFY2(mid >= 40 && mid <= 60, "midpoint Y must land near the middle row");
        // Overshoot clamps into the row range.
        QCOMPARE(rail.ProxyRowForYPixel(1000, 100), ROWS - 1);

        // Round trip through FirstProxyRowInBucket for bucket 0
        // should return row 0.
        QCOMPARE(rail.FirstProxyRowInBucket(0), 0);
        // The last bucket must resolve to a row <= last proxy row.
        const int lastFirst = rail.FirstProxyRowInBucket(rail.BucketCount() - 1);
        QVERIFY(lastFirst >= 0);
        QVERIFY(lastFirst < ROWS);
    }

    /// Log-scaled bar width contract: an empty bucket paints
    /// nothing; a 1-row bucket still lights up a visible tick at
    /// least `MIN_BAR_WIDTH_PX` wide; a max-count bucket
    /// saturates to the full column width; and intensity is
    /// monotone non-decreasing in the row count. Pins the
    /// widget's "small bins still show" guarantee without
    /// pixel-scraping the paint output.
    static void TestLevelBarWidthIsLogScaled()
    {
        constexpr int COLUMN_WIDTH = 14;
        QCOMPARE(OverviewRailWidget::WidthForCountForTest(/*count=*/0, /*maxCount=*/100, COLUMN_WIDTH), 0);
        QCOMPARE(OverviewRailWidget::WidthForCountForTest(/*count=*/5, /*maxCount=*/0, COLUMN_WIDTH), 0);
        QCOMPARE(OverviewRailWidget::WidthForCountForTest(/*count=*/5, /*maxCount=*/100, /*columnWidth=*/0), 0);
        // 1-row bucket: log scale would give a sub-1-px bar for
        // large maxCount; the min-width clamp lifts it to 2 px.
        const int oneRow = OverviewRailWidget::WidthForCountForTest(1, 10000, COLUMN_WIDTH);
        QVERIFY2(oneRow >= 2, "1-row bucket must paint at least MIN_BAR_WIDTH_PX so sparse activity stays visible");
        QVERIFY(oneRow <= COLUMN_WIDTH);
        // Max-count bucket: intensity 1.0 -> full column width.
        QCOMPARE(OverviewRailWidget::WidthForCountForTest(10000, 10000, COLUMN_WIDTH), COLUMN_WIDTH);
        // A mid-range bucket sits strictly between the min-clamp
        // and the max fill under log scaling, so we can visually
        // distinguish "some activity" from "hot spot".
        const int mid = OverviewRailWidget::WidthForCountForTest(100, 10000, COLUMN_WIDTH);
        QVERIFY(mid > oneRow);
        QVERIFY(mid < COLUMN_WIDTH);
        // Monotone non-decreasing in count.
        int previous = 0;
        for (std::uint32_t count :
             {std::uint32_t{1},
              std::uint32_t{4},
              std::uint32_t{16},
              std::uint32_t{64},
              std::uint32_t{256},
              std::uint32_t{1024},
              std::uint32_t{4096}})
        {
            const int w = OverviewRailWidget::WidthForCountForTest(count, 4096, COLUMN_WIDTH);
            QVERIFY2(w >= previous, "bar width must be monotone non-decreasing in the bucket count");
            previous = w;
        }
    }

    /// Helper: render `widget` to a QImage and count how many
    /// pixels in the leftmost 4 px of the rail's level column
    /// differ from the widget's background colour. Reports the
    /// widget's Base colour so failing tests can log what "wash"
    /// they saw the bars blending into.
    static int CountLitLevelPixels(OverviewRailWidget *widget, QColor *outBaseColor = nullptr)
    {
        QImage image(widget->size(), QImage::Format_ARGB32);
        image.fill(Qt::transparent);
        widget->render(&image);
        const QColor bg = widget->palette().color(QPalette::Base);
        if (outBaseColor != nullptr)
        {
            *outBaseColor = bg;
        }
        constexpr int LEVEL_COL_LEFT = 2;
        int lit = 0;
        for (int y = 0; y < image.height(); ++y)
        {
            for (int x = LEVEL_COL_LEFT; x < LEVEL_COL_LEFT + 4; ++x)
            {
                if (QColor(image.pixel(x, y)).rgb() != bg.rgb())
                {
                    ++lit;
                }
            }
        }
        return lit;
    }

    /// End-to-end paint contract: rendering the widget onto a
    /// `QImage` MUST produce visible level-bar pixels (i.e. any
    /// pixel that isn't the widget's background colour) inside
    /// the level column area, once the model has non-empty
    /// buckets. Regression guard for "bars are drawn with a
    /// colour that matches the widget background" bugs (e.g. the
    /// pre-fix issue where `ColorForLevel(Info)` returned a tone
    /// indistinguishable from `QPalette::Window`).
    static void TestRailPaintsVisibleLevelBars()
    {
        LogModel model;
        AnchorManager anchors;
        QObject owner;
        const auto chain = BuildProxyChain(&model, &owner);
        OverviewRailModel railModel(chain.filter, &model, &anchors, &owner);
        railModel.SetBucketCount(100);

        constexpr int ROWS = 500;
        const RailFixture fixture(ROWS);
        StreamJsonPathInto(model, fixture.Path());
        WaitForBucketsChanged(railModel);
        QVERIFY2(railModel.BucketCount() > 0, "test fixture must yield a non-zero bucket count");

        auto *widget = new OverviewRailWidget(&railModel, /*theme=*/nullptr, /*tableView=*/nullptr);
        widget->resize(60, 200);
        widget->show();
        QColor bg;
        const int lit = CountLitLevelPixels(widget, &bg);
        QVERIFY2(
            lit > 0,
            qPrintable(QStringLiteral("no level-bar pixels found (bg=%1) -- bars are painting invisibly").arg(bg.name())
            )
        );
        delete widget;
    }

    /// End-to-end integration: attach the rail through
    /// `LogTableView::AttachOverviewRail` (production wiring) and
    /// verify bars paint against the *live* geometry the widget
    /// picks under the Dark theme -- narrower than the offscreen
    /// unit tests use, so this catches regressions in the sizeHint /
    /// three-column split that only show at production widths.
    static void TestRailPaintsVisibleBarsAtProductionWidth()
    {
        ThemeControl theme;
        theme.SetActiveSelection(QStringLiteral("Dark"));

        LogTableView view;
        view.resize(1200, 700);
        view.show();
        LogModel model;
        view.setModel(&model);

        AnchorManager anchors;
        QObject owner;
        const auto chain = BuildProxyChain(&model, &owner);
        OverviewRailModel railModel(chain.filter, &model, &anchors, &owner);
        auto *widget = new OverviewRailWidget(&railModel, &theme, &view);
        view.AttachOverviewRail(widget);

        QCoreApplication::processEvents();

        constexpr int ROWS = 500;
        const RailFixture fixture(ROWS);
        StreamJsonPathInto(model, fixture.Path());
        WaitForBucketsChanged(railModel);
        QVERIFY(railModel.BucketCount() > 0);

        // The rail's width comes from its own `sizeHint` (via
        // `AttachOverviewRail`), not from a manual resize. That's
        // exactly the code path the production wiring exercises.
        QCoreApplication::processEvents();
        const int railWidth = widget->width();
        QVERIFY2(
            railWidth >= 24,
            qPrintable(QStringLiteral("rail is only %1 px wide -- narrower than the "
                                      "minimum, so the level column would collapse "
                                      "to a single pixel")
                           .arg(railWidth))
        );

        QImage image(widget->size(), QImage::Format_ARGB32);
        image.fill(Qt::transparent);
        widget->render(&image);

        // Sample the full content slot (single section from
        // `RAIL_UNDERLAY_INSET = 1` to the widget's right edge
        // minus that inset). The rail collapsed to a single
        // full-width section for its bin bars; match / anchor
        // overlays share the same slot.
        const QColor bg = widget->palette().color(QPalette::Base);
        constexpr int CONTENT_LEFT = 2;
        const int contentRight = std::max(CONTENT_LEFT + 4, railWidth - 1);
        int highContrastPixels = 0;
        for (int y = 0; y < image.height(); ++y)
        {
            for (int x = CONTENT_LEFT; x < contentRight; ++x)
            {
                const QColor c(image.pixel(x, y));
                const int delta =
                    std::abs(c.red() - bg.red()) + std::abs(c.green() - bg.green()) + std::abs(c.blue() - bg.blue());
                if (delta >= 30)
                {
                    ++highContrastPixels;
                }
            }
        }
        QVERIFY2(
            highContrastPixels > image.height() / 4,
            qPrintable(QStringLiteral("only %1 high-contrast pixels in the content slot at production width "
                                      "%2 px -- bars are not painting visibly against Base %3")
                           .arg(highContrastPixels)
                           .arg(railWidth)
                           .arg(bg.name()))
        );

        view.AttachOverviewRail(nullptr);
        delete widget;
    }

    /// Stacked severity segments using row *background* colours.
    /// On a mixed-level bucket the paint pass must split the bar
    /// into per-level segments and colour each from the theme's
    /// row background (`ThemeControl::BackgroundFor`), *not* the
    /// foreground -- foregrounds are bright pastels tuned for
    /// text-on-row legibility, and painting them as bars
    /// composited to a "light band" wash across every bucket
    /// that carried any Warn / Error / Fatal row (the "rail is
    /// still too light" regression). Row backgrounds are the
    /// theme designer's chosen severity *tint* on Base, so the
    /// rail reads as a mini-map of the row-tinted table.
    ///
    /// Verifies (Dark theme, normal contrast):
    /// 1. Buckets containing Error paint the row-background tone
    ///    `#352121` -- the same tint the Error row shows on the
    ///    main table.
    /// 2. Buckets containing Warn but no Error paint `#272620`.
    /// 3. The *foreground* tones (Warn `#FCD34D`, Error
    ///    `#FCA5A5`, Fatal `#FECACA`) do *not* appear anywhere
    ///    on the rail. Their presence would be an accidental
    ///    revert to the "bright pastels" paint that caused the
    ///    "too light" regression.
    static void TestRailPaintsStackedBackgroundSegments()
    {
        ThemeControl theme;
        theme.SetActiveSelection(QStringLiteral("Dark"));

        LogModel model;
        AnchorManager anchors;
        QObject owner;
        const auto chain = BuildProxyChain(&model, &owner);
        OverviewRailModel railModel(chain.filter, &model, &anchors, &owner);
        railModel.SetBucketCount(100);

        // 500 rows / 4-level cycle from `RailFixture` gives every
        // bucket a mix of Info + Warn + Error + Debug (buckets
        // large enough to span a full cycle) or a 2--3 level
        // slice of that cycle (buckets holding a fractional
        // cycle).
        constexpr int ROWS = 500;
        const RailFixture fixture(ROWS);
        StreamJsonPathInto(model, fixture.Path());
        WaitForBucketsChanged(railModel);
        QVERIFY(railModel.BucketCount() > 0);

        auto *widget = new OverviewRailWidget(&railModel, &theme, /*tableView=*/nullptr);
        widget->resize(60, 200);
        widget->show();

        QImage image(widget->size(), QImage::Format_ARGB32);
        image.fill(Qt::transparent);
        widget->render(&image);

        // Dark theme row-background tones (levels).
        const QRgb errorBgRgb = QColor(QStringLiteral("#352121")).rgb();
        const QRgb warnBgRgb = QColor(QStringLiteral("#272620")).rgb();
        const QRgb debugBgRgb = QColor(QStringLiteral("#20252E")).rgb();
        // Bright foregrounds -- the *regression* tones that
        // stacked-segments produced when we painted from
        // `ForegroundFor` instead of `BackgroundFor`. They must
        // be entirely absent.
        const QRgb errorFgRgb = QColor(QStringLiteral("#FCA5A5")).rgb();
        const QRgb warnFgRgb = QColor(QStringLiteral("#FCD34D")).rgb();
        const QRgb fatalFgRgb = QColor(QStringLiteral("#FECACA")).rgb();

        int errorBgPixels = 0;
        int warnBgPixels = 0;
        int debugBgPixels = 0;
        int brightFgPixels = 0;
        constexpr int CONTENT_LEFT = 2;
        const int contentRight = std::max(CONTENT_LEFT + 4, image.width() - 1);
        for (int y = 0; y < image.height(); ++y)
        {
            for (int x = CONTENT_LEFT; x < contentRight; ++x)
            {
                const QRgb rgb = image.pixel(x, y);
                if (rgb == errorBgRgb)
                {
                    ++errorBgPixels;
                }
                else if (rgb == warnBgRgb)
                {
                    ++warnBgPixels;
                }
                else if (rgb == debugBgRgb)
                {
                    ++debugBgPixels;
                }
                else if (rgb == errorFgRgb || rgb == warnFgRgb || rgb == fatalFgRgb)
                {
                    ++brightFgPixels;
                }
            }
        }
        qInfo() << "stacked-bg paint: Error-bg" << errorBgPixels << "Warn-bg" << warnBgPixels << "Debug-bg"
                << debugBgPixels << "bright-fg (regression)" << brightFgPixels;

        QVERIFY2(errorBgPixels > 0, "stacked segments must paint the Dark theme's Error row-bg tone (#352121)");
        QVERIFY2(warnBgPixels > 0, "stacked segments must paint the Dark theme's Warn row-bg tone (#272620)");
        // Debug row-bg painting is a spot-check: at the widget's
        // 200-px height Debug buckets pack tightly, but any
        // bucket that includes a Debug row must at least paint
        // its 1-px floor.
        QVERIFY2(
            debugBgPixels > 0,
            qPrintable(QStringLiteral("stacked segments must paint the Dark theme's Debug row-bg tone (#20252E); "
                                      "found only %1 Debug-bg pixels")
                           .arg(debugBgPixels))
        );
        QVERIFY2(
            brightFgPixels == 0,
            qPrintable(QStringLiteral("bright foreground pastels must not appear on the rail -- their presence "
                                      "reverts the 'rail is too light' fix; found %1 bright-fg pixels")
                           .arg(brightFgPixels))
        );

        delete widget;
    }

    /// Rare-anomaly visibility: a bucket with 1 Fatal row and
    /// 100 Trace rows must still paint a visible slice of the
    /// Fatal background tone in that bucket's bar (via the
    /// `MIN_SEGMENT_WIDTH_PX` floor in the stacked-segment
    /// paint), so a needle-in-haystack Fatal isn't rounded away
    /// to zero pixels. Verified by streaming a hand-rolled log
    /// where every bucket is Trace-heavy except one bucket that
    /// contains a single Fatal row, then asserting the Fatal
    /// row-bg tone appears in the rendered rail.
    static void TestRareFatalPaintsAtBucketFloor()
    {
        ThemeControl theme;
        theme.SetActiveSelection(QStringLiteral("Dark"));

        LogModel model;
        AnchorManager anchors;
        QObject owner;
        const auto chain = BuildProxyChain(&model, &owner);
        OverviewRailModel railModel(chain.filter, &model, &anchors, &owner);
        railModel.SetBucketCount(50);

        const QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("fatal_needle.jsonl"));
        {
            std::ofstream stream(path.toStdString(), std::ios::binary);
            QVERIFY(stream.is_open());
            constexpr int ROWS = 500;
            constexpr int FATAL_ROW = 250;
            for (int i = 0; i < ROWS; ++i)
            {
                const QString level = (i == FATAL_ROW) ? QStringLiteral("fatal") : QStringLiteral("trace");
                const QString line =
                    QStringLiteral(R"({"time": "2026-01-01T00:00:00", "level": "%1", "body": "msg %2"})")
                        .arg(level)
                        .arg(i);
                stream << line.toStdString() << '\n';
            }
        }
        StreamJsonPathInto(model, path);
        WaitForBucketsChanged(railModel);
        QVERIFY(railModel.BucketCount() > 0);

        auto *widget = new OverviewRailWidget(&railModel, &theme, /*tableView=*/nullptr);
        widget->resize(60, 200);
        widget->show();

        QImage image(widget->size(), QImage::Format_ARGB32);
        image.fill(Qt::transparent);
        widget->render(&image);

        // Dark theme's Fatal row background (`#4A1E1E`) must
        // be present somewhere in the rendered rail. Its
        // presence proves the 1-px severity floor keeps rare
        // anomalies visible; a regression that dropped the
        // floor for high-severity would zero the count.
        const QRgb expectedFatalBgRgb = QColor(QStringLiteral("#4A1E1E")).rgb();
        int fatalBgPixels = 0;
        constexpr int CONTENT_LEFT = 2;
        const int contentRight = std::max(CONTENT_LEFT + 4, image.width() - 1);
        for (int y = 0; y < image.height(); ++y)
        {
            for (int x = CONTENT_LEFT; x < contentRight; ++x)
            {
                if (image.pixel(x, y) == expectedFatalBgRgb)
                {
                    ++fatalBgPixels;
                }
            }
        }
        QVERIFY2(
            fatalBgPixels > 0, "rare Fatal (1-in-500) must paint the Fatal row-bg tone at the 1-px severity floor"
        );

        delete widget;
    }

    /// Log-weighted segment sizing: rare severities get a
    /// *visibly larger* slice than linear proportion would
    /// award. On a bucket with 99 Trace + 1 Fatal (1 % Fatal),
    /// linear shares floor the Fatal segment to 1 px (noise on
    /// a Trace-dominated rail); the `log2(count + 1)` weighting
    /// raises Fatal's share to
    /// log2(2) / (log2(100) + log2(2)) ~= 13 % of the bar --
    /// visibly readable without flipping the majority. This
    /// test uses a fixture where *every* bucket has the same
    /// 99 : 1 ratio (one Fatal every 100 rows), so the measured
    /// Fatal-to-Trace pixel ratio equals the per-bucket ratio
    /// independent of bucket count.
    static void TestSegmentSharesUseLogWeighting()
    {
        ThemeControl theme;
        theme.SetActiveSelection(QStringLiteral("Dark"));

        LogModel model;
        AnchorManager anchors;
        QObject owner;
        const auto chain = BuildProxyChain(&model, &owner);
        OverviewRailModel railModel(chain.filter, &model, &anchors, &owner);
        // Non-zero bucket count *before* streaming so the
        // rebuild that follows fires `bucketsChanged`.
        // `SyncBucketCountToHeight` on `widget->show()` later
        // resizes this to the widget's rail height.
        railModel.SetBucketCount(100);

        const QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("log_weight.jsonl"));
        {
            std::ofstream stream(path.toStdString(), std::ios::binary);
            QVERIFY(stream.is_open());
            constexpr int ROWS = 5000;
            for (int i = 0; i < ROWS; ++i)
            {
                const QString level = (i % 100 == 0) ? QStringLiteral("fatal") : QStringLiteral("trace");
                const QString line =
                    QStringLiteral(R"({"time": "2026-01-01T00:00:00", "level": "%1", "body": "msg %2"})")
                        .arg(level)
                        .arg(i);
                stream << line.toStdString() << '\n';
            }
        }
        StreamJsonPathInto(model, path);
        WaitForBucketsChanged(railModel);
        QVERIFY(railModel.BucketCount() > 0);

        auto *widget = new OverviewRailWidget(&railModel, &theme, /*tableView=*/nullptr);
        widget->resize(60, 200);
        widget->show();

        QImage image(widget->size(), QImage::Format_ARGB32);
        image.fill(Qt::transparent);
        widget->render(&image);

        const QRgb fatalBg = QColor(QStringLiteral("#4A1E1E")).rgb();
        const QRgb traceBg = QColor(QStringLiteral("#1F2228")).rgb();
        int fatalPixels = 0;
        int tracePixels = 0;
        constexpr int CONTENT_LEFT = 2;
        const int contentRight = std::max(CONTENT_LEFT + 4, image.width() - 1);
        for (int y = 0; y < image.height(); ++y)
        {
            for (int x = CONTENT_LEFT; x < contentRight; ++x)
            {
                const QRgb rgb = image.pixel(x, y);
                if (rgb == fatalBg)
                {
                    ++fatalPixels;
                }
                else if (rgb == traceBg)
                {
                    ++tracePixels;
                }
            }
        }
        qInfo() << "log-weight paint: Fatal px" << fatalPixels << "Trace px" << tracePixels;

        const int totalLevelPixels = fatalPixels + tracePixels;
        QVERIFY2(totalLevelPixels > 0, "no severity pixels found in the rendered rail");
        // Compare against the *linear-share* baseline. On a
        // 1 % rare level with a 60 px content width and the
        // 1 px severity floor, linear proportion allocates
        // Fatal ~= floor(58 * 1 / 25) + 1 = 3 px per
        // Fatal-carrying bucket. The rail spawns roughly one
        // Fatal bucket per 4 buckets (5000 rows / ~196
        // buckets ~= 25 rows / bucket, one Fatal every 100
        // rows) -- so linear ceiling is ~= 3 px * 50 buckets
        // = 150 Fatal pixels. Log-weighted shares raise Fatal
        // to ~15 % per Fatal-bucket, yielding ~450 pixels in
        // the same geometry. Assert Fatal pixels exceed 2x
        // the linear ceiling so a regression back to linear
        // fails loudly, without pinning the exact ratio (test
        // stays robust to future paint-pass tuning).
        constexpr int LINEAR_CEILING_PX = 150;
        QVERIFY2(
            fatalPixels > 2 * LINEAR_CEILING_PX,
            qPrintable(QStringLiteral("log-weighted segments must give a 1-in-100 Fatal >%1 px on this "
                                      "geometry (linear would give ~%2 px); got %3 px")
                           .arg(2 * LINEAR_CEILING_PX)
                           .arg(LINEAR_CEILING_PX)
                           .arg(fatalPixels))
        );
        QVERIFY2(
            tracePixels > fatalPixels,
            "log-weighted segments must keep the majority level (Trace) larger than the rare Fatal"
        );

        delete widget;
    }

    /// High-contrast preference recolours the rail. `ThemeControl`
    /// reads the `ui/highContrastLevels` setting into
    /// `mHighContrast`, and `BackgroundFor(level)` -- which the
    /// rail's paint pass reaches for -- resolves through
    /// `StyleForLevel(theme, level, mHighContrast)`. Toggling the
    /// preference must therefore switch every bucket's severity
    /// segments from the theme's normal row-bg palette to the
    /// louder `levelsHighContrast` palette without any extra
    /// wiring in the widget. Verified by rendering the same rail
    /// twice against the Dark theme (normal + high-contrast) and
    /// asserting the Error segments switch from `#352121`
    /// (normal) to `#4C1D1D` (high contrast).
    static void TestRailRespectsHighContrastPreference()
    {
        ThemeControl theme;
        theme.SetActiveSelection(QStringLiteral("Dark"));
        QVERIFY(theme.HasLevelsHighContrast());
        theme.SetHighContrast(false);

        LogModel model;
        AnchorManager anchors;
        QObject owner;
        const auto chain = BuildProxyChain(&model, &owner);
        OverviewRailModel railModel(chain.filter, &model, &anchors, &owner);
        railModel.SetBucketCount(100);

        constexpr int ROWS = 500;
        const RailFixture fixture(ROWS);
        StreamJsonPathInto(model, fixture.Path());
        WaitForBucketsChanged(railModel);

        auto *widget = new OverviewRailWidget(&railModel, &theme, /*tableView=*/nullptr);
        widget->resize(60, 200);
        widget->show();

        const QRgb normalErrorBg = QColor(QStringLiteral("#352121")).rgb();
        const QRgb loudErrorBg = QColor(QStringLiteral("#4C1D1D")).rgb();

        auto countErrorTones = [&](QRgb wanted) {
            QImage image(widget->size(), QImage::Format_ARGB32);
            image.fill(Qt::transparent);
            widget->render(&image);
            int count = 0;
            constexpr int CONTENT_LEFT = 2;
            const int contentRight = std::max(CONTENT_LEFT + 4, image.width() - 1);
            for (int y = 0; y < image.height(); ++y)
            {
                for (int x = CONTENT_LEFT; x < contentRight; ++x)
                {
                    if (image.pixel(x, y) == wanted)
                    {
                        ++count;
                    }
                }
            }
            return count;
        };

        const int normalHitsNormal = countErrorTones(normalErrorBg);
        const int normalHitsLoud = countErrorTones(loudErrorBg);
        QVERIFY2(normalHitsNormal > 0, "normal-contrast paint must render the theme's normal Error row-bg");
        QVERIFY2(normalHitsLoud == 0, "normal-contrast paint must not render the loud Error row-bg");

        theme.SetHighContrast(true);
        widget->update();
        QApplication::processEvents();

        const int loudHitsNormal = countErrorTones(normalErrorBg);
        const int loudHitsLoud = countErrorTones(loudErrorBg);
        QVERIFY2(loudHitsLoud > 0, "high-contrast paint must render the theme's `levelsHighContrast` Error row-bg");
        QVERIFY2(loudHitsNormal == 0, "high-contrast paint must not render the normal Error row-bg");

        delete widget;
    }

    /// Overlay contract: match ticks repaint the *entire content
    /// width* of any bucket carrying a match row, overriding the
    /// base-layer bin painting. Verifies the single-section
    /// rework -- the old three-column layout dedicated only ~20 %
    /// of the underlay to matches, which was hard to spot on a
    /// narrow rail; the overlay approach makes every match bucket
    /// paint as a solid full-width Highlight band. The test
    /// pushes a single match into a known bucket and confirms
    /// the whole content slot at that Y band is dominated by
    /// the palette Highlight colour.
    static void TestMatchOverlayCoversFullContentWidth()
    {
        ThemeControl theme;
        theme.SetActiveSelection(QStringLiteral("Dark"));

        LogModel model;
        AnchorManager anchors;
        QObject owner;
        const auto chain = BuildProxyChain(&model, &owner);
        OverviewRailModel railModel(chain.filter, &model, &anchors, &owner);

        // Small bucket count so a single match row lights up a
        // sizeable Y band we can sample confidently.
        railModel.SetBucketCount(20);

        constexpr int ROWS = 200;
        const RailFixture fixture(ROWS);
        StreamJsonPathInto(model, fixture.Path());
        WaitForBucketsChanged(railModel);
        QVERIFY(railModel.BucketCount() > 0);

        // Push a match at row 50 (roughly the middle). Every
        // bucket has activity by construction, so the base layer
        // paints level bars first and the overlay must repaint
        // the matched bucket in Highlight over the top.
        railModel.SetMatchProxyRows({50});
        QVERIFY(railModel.HasMatchTicks());

        auto *widget = new OverviewRailWidget(&railModel, &theme, /*tableView=*/nullptr);
        widget->resize(60, 200);
        widget->show();

        QImage image(widget->size(), QImage::Format_ARGB32);
        image.fill(Qt::transparent);
        widget->render(&image);

        const QColor highlight = widget->palette().color(QPalette::Highlight);

        // Locate the Y band of the bucket that received the
        // match by scanning for the row whose content slot is
        // dominantly Highlight-coloured. `matchProxyRow / rows *
        // bucketCount` gives the target bucket; use the widget's
        // helper to resolve it to a Y range.
        const std::size_t matchBucket = std::min<std::size_t>(
            railModel.BucketCount() - 1,
            ((static_cast<std::size_t>(50) * railModel.BucketCount()) / static_cast<std::size_t>(ROWS))
        );
        const int yTop = widget->YForBucketForTest(matchBucket);
        const int yBottom =
            matchBucket + 1 < railModel.BucketCount() ? widget->YForBucketForTest(matchBucket + 1) : image.height();

        // Sample the *right* portion of the content slot (columns
        // past where the level bar could ever reach at ~20 % row
        // density). If the overlay repainted the full content
        // width, we expect Highlight there; if only the old
        // match column painted (right 20 % of the underlay), the
        // sample also passes -- but the left portion of the
        // content slot must also be Highlight for the overlay
        // semantic to hold.
        constexpr int CONTENT_LEFT = 2;
        const int contentRight = std::max(CONTENT_LEFT + 4, image.width() - 1);
        int highlightPixelsLeftHalf = 0;
        int highlightPixelsRightHalf = 0;
        const int mid = (CONTENT_LEFT + contentRight) / 2;
        for (int y = yTop; y < yBottom; ++y)
        {
            for (int x = CONTENT_LEFT; x < contentRight; ++x)
            {
                const QColor c(image.pixel(x, y));
                const int delta = std::abs(c.red() - highlight.red()) + std::abs(c.green() - highlight.green()) +
                                  std::abs(c.blue() - highlight.blue());
                if (delta < 30)
                {
                    if (x < mid)
                    {
                        ++highlightPixelsLeftHalf;
                    }
                    else
                    {
                        ++highlightPixelsRightHalf;
                    }
                }
            }
        }
        QVERIFY2(
            highlightPixelsLeftHalf > 0 && highlightPixelsRightHalf > 0,
            qPrintable(QStringLiteral("match overlay must span the full content width -- found %1 Highlight px on the "
                                      "left half and %2 on the right half of the matched bucket's Y band")
                           .arg(highlightPixelsLeftHalf)
                           .arg(highlightPixelsRightHalf))
        );

        delete widget;
    }

    /// Same as `TestRailPaintsVisibleLevelBars` but with the real
    /// Dark theme loaded. Reproduces the user-reported "bars are
    /// invisible" bug where the theme's subtle row-background
    /// tint for Info-heavy buckets composited onto the rail's own
    /// Base wash and disappeared. Row-background painting is
    /// intentionally subtle (Warn bg delta 11, Error bg delta 21
    /// against Base) -- the test therefore asserts (a) *any*
    /// non-Base pixels are painted and (b) at least one distinctly
    /// contrasting tone is present so the rail is never invisible.
    static void TestRailPaintsVisibleBarsUnderDarkTheme()
    {
        ThemeControl theme;
        theme.SetActiveSelection(QStringLiteral("Dark"));
        QCOMPARE(QString::fromStdString(theme.Active().name), QStringLiteral("Dark"));

        LogModel model;
        AnchorManager anchors;
        QObject owner;
        const auto chain = BuildProxyChain(&model, &owner);
        OverviewRailModel railModel(chain.filter, &model, &anchors, &owner);
        railModel.SetBucketCount(100);

        constexpr int ROWS = 500;
        const RailFixture fixture(ROWS);
        StreamJsonPathInto(model, fixture.Path());
        WaitForBucketsChanged(railModel);

        auto *widget = new OverviewRailWidget(&railModel, &theme, /*tableView=*/nullptr);
        widget->resize(60, 200);
        widget->show();

        QImage image(widget->size(), QImage::Format_ARGB32);
        image.fill(Qt::transparent);
        widget->render(&image);

        const QColor bg = widget->palette().color(QPalette::Base);
        // Sample a vertical strip in the level column (left side of
        // the rail underlay). Log every distinct non-background
        // colour we find so a failing render tells us exactly what
        // ghost tone was used.
        constexpr int LEVEL_COL_LEFT = 2;
        QSet<QRgb> distinctColors;
        int litPixels = 0;
        int highContrastPixels = 0;
        for (int y = 0; y < image.height(); ++y)
        {
            for (int x = LEVEL_COL_LEFT; x < LEVEL_COL_LEFT + 4; ++x)
            {
                const QRgb pixel = image.pixel(x, y);
                if (QColor(pixel).rgb() == bg.rgb())
                {
                    continue;
                }
                ++litPixels;
                distinctColors.insert(pixel);
                // Perceptual delta: sum of absolute channel diffs.
                // Anything < 30 is essentially invisible on screen
                // (the eye can't resolve <10% per-channel deltas on
                // small strips at typical desktop viewing distance).
                const QColor c(pixel);
                const int delta =
                    std::abs(c.red() - bg.red()) + std::abs(c.green() - bg.green()) + std::abs(c.blue() - bg.blue());
                if (delta >= 30)
                {
                    ++highContrastPixels;
                }
            }
        }

        // Emit the colour set as a debug info so a failing test
        // shows *what* tones the rail is using.
        QStringList colorNames;
        for (const QRgb c : distinctColors)
        {
            colorNames << QColor(c).name();
        }
        qInfo() << "Dark rail bar colours found:" << colorNames << "against bg" << bg.name();
        qInfo() << "Lit pixels:" << litPixels << "  high-contrast pixels:" << highContrastPixels;

        QVERIFY2(litPixels > 0, "no level-bar pixels found under Dark theme (bars painted invisibly)");
        // At least one bucket must paint a *distinctly*
        // contrasting tone (delta >= 30). Under the row-
        // background palette this is guaranteed by any Error /
        // Fatal bucket (Error bg delta 21 is borderline; Fatal
        // bg delta 48 clears the bar); the `RailFixture` cycle
        // ensures every bucket lands an Error row. Guards
        // against a regression that mis-routes paint back to
        // Base (invisible) or to a single near-Base tint (also
        // invisible).
        QVERIFY2(
            highContrastPixels > 0,
            qPrintable(QStringLiteral("Dark theme: no bar pixels contrast strongly against bg %1. Bars painted "
                                      "with `ColorForLevel` are near-invisible on the widget background.")
                           .arg(bg.name()))
        );

        delete widget;
    }

    /// Whole-file overview contract: the rail projects the file's
    /// row range across its *entire* widget height (klogg / glogg /
    /// VS Code minimap style), not just the viewport-Y slice below
    /// the header. Pinned via `YForBucketForTest(0)` -- the first
    /// bucket's Y must land near the top of the widget (well above
    /// the viewport's top edge) so bars fill the whole rail
    /// instead of being confined to a header-height-shifted strip.
    static void TestRailProjectsAcrossFullWidgetHeight()
    {
        LogTableView view;
        view.resize(800, 400);
        view.show();
        LogModel model;
        view.setModel(&model);

        AnchorManager anchors;
        QObject owner;
        const auto chain = BuildProxyChain(&model, &owner);
        OverviewRailModel railModel(chain.filter, &model, &anchors, &owner);
        auto *widget = new OverviewRailWidget(&railModel, /*theme=*/nullptr, &view);
        view.AttachOverviewRail(widget);

        // Give Qt a chance to lay out header + viewport under the
        // reserved margin so `viewport()->geometry().top()` reflects
        // the actual header height (offscreen QPA still routes the
        // layout pass).
        QCoreApplication::processEvents();

        constexpr int ROWS = 40;
        const RailFixture fixture(ROWS);
        StreamJsonPathInto(model, fixture.Path());
        WaitForBucketsChanged(railModel);
        QVERIFY(railModel.BucketCount() > 0);

        const int viewportTop = view.viewport()->geometry().top();
        QVERIFY2(viewportTop > 0, "test needs a non-degenerate header height to be meaningful");

        // Bar for bucket 0 must sit *above* the viewport's top
        // edge in the widget's own coordinate system -- i.e., the
        // rail's row-to-Y projection spans the full widget, not
        // just the viewport-Y slice. Otherwise the rail wastes the
        // header-Y strip as empty chrome, which reads as "the rail
        // is broken -- half of it is empty".
        const int firstBucketY = widget->YForBucketForTest(0);
        QVERIFY2(firstBucketY < viewportTop, "first bucket must sit above the viewport top (whole-file overview)");

        // Last bucket must sit near the widget bottom so the file's
        // final rows are reachable via a click at the rail's foot.
        const int lastBucketY = widget->YForBucketForTest(static_cast<int>(railModel.BucketCount()) - 1);
        QVERIFY2(lastBucketY >= viewportTop, "last bucket must be at or below the viewport top");
        QVERIFY(lastBucketY < widget->height());

        view.AttachOverviewRail(nullptr);
        delete widget;
    }

    /// Viewport indicator centring: on tall logs the visible-row
    /// Y-range naturally maps to fewer than `INDICATOR_MIN_HEIGHT_PX`
    /// rail pixels, so the widget inflates the indicator up to the
    /// minimum. That inflation must happen *around the centre* of
    /// the visible range, not by pinning the indicator's top to
    /// the visible-top's Y -- otherwise a row selected in the
    /// middle of the viewport is drawn near the top of the
    /// indicator on the rail, which reads as "current-view
    /// preview is misaligned relative to what I see in the
    /// table" (regression report). Verified by scrolling to the
    /// middle of a tall model and asserting the indicator's
    /// vertical mid-point lands on (or very close to) the Y of
    /// the middle visible row.
    static void TestViewportIndicatorCentersOnVisibleRange()
    {
        LogTableView view;
        view.resize(800, 400);
        view.show();
        LogModel model;
        view.setModel(&model);

        AnchorManager anchors;
        QObject owner;
        const auto chain = BuildProxyChain(&model, &owner);
        OverviewRailModel railModel(chain.filter, &model, &anchors, &owner);
        auto *widget = new OverviewRailWidget(&railModel, /*theme=*/nullptr, &view);
        view.AttachOverviewRail(widget);

        QCoreApplication::processEvents();

        // Enough rows that ~20 visible rows fall below the
        // indicator's min-height floor when projected onto the
        // rail -- i.e. the min-height inflation path *must* run.
        constexpr int ROWS = 10000;
        const RailFixture fixture(ROWS);
        StreamJsonPathInto(model, fixture.Path());
        WaitForBucketsChanged(railModel);
        QVERIFY(railModel.BucketCount() > 0);

        // Scroll the table to somewhere in the middle so the
        // visible range is a small window well away from either
        // edge. `PositionAtCenter` centres `middleRow` in the
        // viewport (subject to Qt's scrollbar snap) -- exactly
        // the layout that used to trigger the "anchor at the
        // top of the indicator" bug.
        constexpr int MIDDLE_ROW = ROWS / 2;
        const QModelIndex midIdx = view.model()->index(MIDDLE_ROW, 0);
        QVERIFY(midIdx.isValid());
        view.scrollTo(midIdx, QAbstractItemView::PositionAtCenter);
        QCoreApplication::processEvents();

        const QRect indicator = widget->ViewportIndicatorRectForTest();
        QVERIFY2(!indicator.isEmpty(), "viewport indicator must be non-empty when the table has rows");

        // Y coordinate on the rail where the *middle* visible row
        // projects to. If the indicator top was pinned to the
        // visible-top's Y (pre-fix behaviour), the middle-row Y
        // would land near the *top edge* of the indicator on a
        // very tall log. With centring, the middle-row Y must
        // land close to the indicator's vertical mid-point.
        const int middleBucket = widget->BucketAtYForTest(
            widget->height() / 2 // seed near the widget centre
        );
        // Bucket index for the middle row via the projection the
        // widget itself uses (row -> bucket = row * nBuckets /
        // rowCount). Guarded against a torn bucket count.
        const std::size_t nBuckets = railModel.BucketCount();
        QVERIFY(nBuckets > 0);
        const std::size_t midRowBucket = std::min<std::size_t>(
            nBuckets - 1, (static_cast<std::size_t>(MIDDLE_ROW) * nBuckets) / static_cast<std::size_t>(ROWS)
        );
        const int midRowY = widget->YForBucketForTest(midRowBucket);

        const int indicatorMidY = indicator.top() + (indicator.height() / 2);
        const int misalignment = std::abs(indicatorMidY - midRowY);

        // Tolerance: half the indicator height plus one pixel of
        // rounding slack from Qt's row-to-scrollbar snapping. If
        // the fix is in place, `misalignment` is typically zero
        // or one; pre-fix the middle row landed at the indicator
        // top so `misalignment ≈ indicator.height() / 2`, far
        // above the tolerance.
        const int tolerance = 3;
        QVERIFY2(
            misalignment <= tolerance,
            qPrintable(QStringLiteral("indicator midpoint (%1) must be within %2 px of the middle visible row's "
                                      "rail-Y (%3); off by %4 px. indicator=%5x%6 at (%7,%8)")
                           .arg(indicatorMidY)
                           .arg(tolerance)
                           .arg(midRowY)
                           .arg(misalignment)
                           .arg(indicator.width())
                           .arg(indicator.height())
                           .arg(indicator.left())
                           .arg(indicator.top()))
        );
        // Silence unused-var warning when the seed bucket isn't
        // referenced by any assertion above; the call itself
        // exercises the widget's Y-to-bucket path.
        Q_UNUSED(middleBucket);

        view.AttachOverviewRail(nullptr);
        delete widget;
    }

    /// Single-section content contract: the level bins, match
    /// ticks, and anchor ticks all share one full-width content
    /// slot inside the usable underlay. Match / anchor ticks are
    /// *overlays* on top of the bins (see the paint pass) rather
    /// than dedicated columns, so a search hit or user anchor
    /// paints the whole rail width in that bucket and reads as
    /// an unambiguous "here!" marker even on a narrow rail.
    static void TestContentRectSpansFullUnderlay()
    {
        constexpr int UNDERLAY_LEFT = 2;
        constexpr int UNDERLAY_WIDTH = 25; // 28 px rail - 1 sep - 2 inset
        const QRect content = OverviewRailWidget::ContentRectForTest(UNDERLAY_LEFT, UNDERLAY_WIDTH);

        QCOMPARE(content.left(), UNDERLAY_LEFT);
        QCOMPARE(content.width(), UNDERLAY_WIDTH);

        // Degenerate zero-width underlay: helper returns an
        // empty rect without crashing.
        const QRect empty = OverviewRailWidget::ContentRectForTest(UNDERLAY_LEFT, 0);
        QCOMPARE(empty.width(), 0);
    }

    /// `sizeHint()` returns a positive DPI-fluent width and a
    /// zero preferred height (rail grows vertically to fit the
    /// hosting margin). `minimumSizeHint` never exceeds it.
    /// Default width mode is Medium (the shipped Preferences /
    /// `QSettings` default).
    static void TestWidgetSizeHintIsDpiFluent()
    {
        LogModel model;
        AnchorManager anchors;
        QObject owner;
        const auto chain = BuildProxyChain(&model, &owner);
        OverviewRailModel rail(chain.filter, &model, &anchors, &owner);
        const OverviewRailWidget widget(&rail, /*theme=*/nullptr, /*tableView=*/nullptr);
        // Ensure the widget has a valid style / font (offscreen QPA
        // still routes both). The DPI-fluent width should be > 0
        // and >= the minimum floor.
        widget.ensurePolished();
        QCOMPARE(widget.WidthMode(), OverviewRailWidthMode::Medium);
        const int width = widget.sizeHint().width();
        QVERIFY2(width > 0, "size hint width must be positive after ensurePolished");
        const int minWidth = widget.minimumSizeHint().width();
        QVERIFY(minWidth > 0);
        QVERIFY(width >= minWidth);
        // sizeHint() returns 0 height so the parent layout drives
        // vertical extent.
        QCOMPARE(widget.sizeHint().height(), 0);
    }

    /// Width modes scale the same DPI base: Medium > Narrow and
    /// Wide > Medium. Unknown settings strings parse to Medium.
    static void TestWidthModesScaleSizeHint()
    {
        LogModel model;
        AnchorManager anchors;
        QObject owner;
        const auto chain = BuildProxyChain(&model, &owner);
        OverviewRailModel rail(chain.filter, &model, &anchors, &owner);
        OverviewRailWidget widget(&rail, /*theme=*/nullptr, /*tableView=*/nullptr);
        widget.ensurePolished();

        QCOMPARE(ParseOverviewRailWidthMode(QStringLiteral("narrow")), OverviewRailWidthMode::Narrow);
        QCOMPARE(ParseOverviewRailWidthMode(QStringLiteral("medium")), OverviewRailWidthMode::Medium);
        QCOMPARE(ParseOverviewRailWidthMode(QStringLiteral("wide")), OverviewRailWidthMode::Wide);
        QCOMPARE(ParseOverviewRailWidthMode(QStringLiteral("garbage")), OverviewRailWidthMode::Medium);
        QCOMPARE(OverviewRailWidthModeToSettingsString(OverviewRailWidthMode::Wide), QStringLiteral("wide"));

        widget.SetWidthMode(OverviewRailWidthMode::Narrow);
        const int narrow = widget.sizeHint().width();
        widget.SetWidthMode(OverviewRailWidthMode::Medium);
        const int medium = widget.sizeHint().width();
        widget.SetWidthMode(OverviewRailWidthMode::Wide);
        const int wide = widget.sizeHint().width();

        QVERIFY2(medium > narrow, "Medium must be wider than Narrow");
        QVERIFY2(wide > medium, "Wide must be wider than Medium");
        QVERIFY(narrow >= 24);
        QVERIFY(wide <= 128);
    }

    /// Changing the width mode on an attached rail refreshes the
    /// reserved right margin so the viewport tracks sizeHint.
    static void TestWidthModeChangeRefreshesReservedMargin()
    {
        LogTableView view;
        view.resize(800, 400);
        view.show();

        LogModel model;
        AnchorManager anchors;
        QObject owner;
        const auto chain = BuildProxyChain(&model, &owner);
        OverviewRailModel railModel(chain.filter, &model, &anchors, &owner);
        auto *widget = new OverviewRailWidget(&railModel, /*theme=*/nullptr, &view);
        widget->SetWidthMode(OverviewRailWidthMode::Narrow);
        view.AttachOverviewRail(widget);
        QCoreApplication::processEvents();

        const int narrowMargin = view.ReservedRightMargin();
        QCOMPARE(narrowMargin, widget->sizeHint().width());

        widget->SetWidthMode(OverviewRailWidthMode::Wide);
        view.RefreshOverviewRailMargin();
        QCoreApplication::processEvents();

        const int wideMargin = view.ReservedRightMargin();
        QCOMPARE(wideMargin, widget->sizeHint().width());
        QVERIFY2(wideMargin > narrowMargin, "Wide mode must enlarge the reserved table margin");

        view.AttachOverviewRail(nullptr);
        delete widget;
    }

    /// Attaching a rail widget to `LogTableView` reserves a
    /// positive right margin; detaching zeroes it. The widget is
    /// reparented to the view while attached and returned to the
    /// caller on detach.
    static void TestAttachOverviewRailReservesMargin()
    {
        LogTableView view;
        view.resize(800, 400);
        view.show();

        // Use a QWidget with a fixed size so `sizeHint()` returns
        // a positive width — a bare QWidget's sizeHint is (-1,-1),
        // which would leave the reservation at zero. The
        // `OverviewRailWidget` in production supplies its own
        // DPI-fluent width; the test just needs any positive value.
        constexpr int RAIL_WIDTH = 16;
        auto *rail = new QWidget(nullptr);
        rail->setFixedWidth(RAIL_WIDTH);
        view.AttachOverviewRail(rail);
        QCOMPARE(view.OverviewRail(), rail);
        // Reserved right margin is JUST the rail width.
        // `QAbstractScrollArea::layoutChildren_helper` already
        // reserves `PM_ScrollBarExtent` for the vertical
        // scrollbar independently of viewport margins, so the
        // scrollbar auto-places past the rail without needing an
        // extra reservation here.
        QCOMPARE(view.ReservedRightMargin(), RAIL_WIDTH);
        QCOMPARE(rail->parent(), &view);

        // Force an `updateGeometries` pass so `QTableView`'s
        // implicit `setViewportMargins(headerW, headerH, 0, 0)`
        // gets a chance to wipe our right margin. Our override
        // must re-apply it, otherwise the rail's slot silently
        // disappears on the first row insert / column change /
        // header resize in production.
        view.UpdateGeometriesForTest();
        QCOMPARE(view.ViewportMarginsForTest().right(), RAIL_WIDTH);

        // Geometry: the rail must sit immediately right of the
        // viewport and MUST NOT overlap the scrollbar's rect --
        // that was the "invisible rail" bug where the scrollbar
        // painted on top of the rail's rightmost pixels. The Y
        // span covers the frame from Y=0 (aligned with the top
        // of the horizontal header) down to the viewport bottom
        // so the strip reads as one continuous piece of chrome
        // from the header down to the scrollbar corner. The
        // level bars / viewport indicator themselves paint only
        // in the viewport-Y slice (`InteractiveRailRect` skips
        // the header-Y strip) so a bar for a visible row still
        // aligns horizontally with that row's table entry --
        // the header-Y strip above stays as wash chrome.
        const QRect railGeom = rail->geometry();
        const QRect vpGeom = view.viewport()->geometry();
        QCOMPARE(railGeom.left(), vpGeom.right() + 1);
        QCOMPARE(railGeom.width(), RAIL_WIDTH);
        QCOMPARE(railGeom.top(), 0);
        QCOMPARE(railGeom.bottom(), vpGeom.bottom());
        if (const QScrollBar *vsb = view.verticalScrollBar(); vsb != nullptr && vsb->isVisible())
        {
            QVERIFY2(
                railGeom.right() < vsb->geometry().left(), "rail must not overlap the vertical scrollbar's paint rect"
            );
        }

        view.AttachOverviewRail(nullptr);
        QCOMPARE(view.OverviewRail(), static_cast<QWidget *>(nullptr));
        QCOMPARE(view.ReservedRightMargin(), 0);
        // Widget survives detach (caller owns it). Delete manually.
        delete rail;
    }

    /// Regression against the bug where `QTableView::updateGeometries()`
    /// unconditionally calls `setViewportMargins(vHeaderW, hHeaderH,
    /// 0, 0)` and would silently wipe the rail's reserved slot on
    /// the next row insert / column change / header resize. Force
    /// several implicit re-layouts and assert the margin sticks
    /// each time.
    static void TestRailMarginSurvivesUpdateGeometries()
    {
        LogTableView view;
        view.resize(800, 400);
        view.show();

        constexpr int RAIL_WIDTH = 18;
        auto *rail = new QWidget(nullptr);
        rail->setFixedWidth(RAIL_WIDTH);
        view.AttachOverviewRail(rail);

        // Attach a small model so `updateGeometries` runs through
        // the same code paths a live table hits (columns, rows,
        // scrollbar range checks).
        LogModel model;
        view.setModel(&model);

        for (int i = 0; i < 5; ++i)
        {
            view.UpdateGeometriesForTest();
            QCOMPARE(view.ViewportMarginsForTest().right(), RAIL_WIDTH);
            const QRect railGeom = rail->geometry();
            const QRect vpGeom = view.viewport()->geometry();
            QCOMPARE(railGeom.left(), vpGeom.right() + 1);
            QCOMPARE(railGeom.width(), RAIL_WIDTH);
            // Rail Y span must stay from the frame top down to
            // the viewport bottom across every implicit re-layout
            // so the strip reads as one continuous piece of
            // chrome (`InteractiveRailRect` handles the header-Y
            // skip for the bars themselves).
            QCOMPARE(railGeom.top(), 0);
            QCOMPARE(railGeom.bottom(), vpGeom.bottom());
        }

        view.AttachOverviewRail(nullptr);
        delete rail;
    }

    /// The MainWindow overview-rail toggle exists as a checkable
    /// action, defaults to on (per the ROADMAP), and toggling
    /// off attaches / detaches the widget from the table view.
    static void TestMainWindowOverviewRailToggle()
    {
        // `initTestCase` set the org / app name and enabled
        // test-mode QSettings; clear the specific key we probe
        // so a prior run doesn't seed the pref off.
        {
            QSettings settings;
            settings.remove(QStringLiteral("ui/showOverviewRail"));
        }

        MainWindow window;
        window.show();

        auto *action = window.findChild<QAction *>(QStringLiteral("actionToggleOverviewRail"));
        QVERIFY2(action != nullptr, "MainWindow must expose actionToggleOverviewRail");
        QVERIFY(action->isCheckable());
        QVERIFY2(action->isChecked(), "overview rail should be on by default (ROADMAP item 13)");

        auto *view = window.findChild<LogTableView *>();
        QVERIFY(view != nullptr);
        QVERIFY2(view->OverviewRail() != nullptr, "rail widget must attach on default-on toggle");
        QVERIFY(view->ReservedRightMargin() > 0);

        // Toggle off -- widget detaches, margin reclaimed.
        action->trigger();
        QVERIFY(!action->isChecked());
        QCOMPARE(view->OverviewRail(), static_cast<QWidget *>(nullptr));
        QCOMPARE(view->ReservedRightMargin(), 0);

        // Toggle back on -- widget re-attaches.
        action->trigger();
        QVERIFY(action->isChecked());
        QVERIFY(view->OverviewRail() != nullptr);
        QVERIFY(view->ReservedRightMargin() > 0);

        // Persisted preference reflects the current state.
        const QSettings settings;
        QCOMPARE(settings.value(QStringLiteral("ui/showOverviewRail")).toBool(), true);
    }

    /// The Ctrl+Shift+R shortcut fires the toggle. Confirms the
    /// shortcut is registered on the window so the discovery path
    /// (Shortcuts dialog + toolbar tooltip) matches expectations.
    static void TestMainWindowOverviewRailShortcut()
    {
        const MainWindow window;
        auto *action = window.findChild<QAction *>(QStringLiteral("actionToggleOverviewRail"));
        QVERIFY(action != nullptr);
        const QKeySequence expected(Qt::CTRL | Qt::SHIFT | Qt::Key_R);
        QCOMPARE(action->shortcut(), expected);
    }

    /// Toggling the rail off drops the underlying
    /// `OverviewRailModel`'s bucket vector so subsequent proxy
    /// signals short-circuit inside `RebuildInternal`. Toggling
    /// back on re-arms the bucket count from the widget's height
    /// (via `showEvent` -> `SyncBucketCountToHeight`).
    static void TestMainWindowOverviewRailHideDropsBuckets()
    {
        {
            QSettings settings;
            settings.remove(QStringLiteral("ui/showOverviewRail"));
        }
        MainWindow window;
        window.show();

        auto *action = window.findChild<QAction *>(QStringLiteral("actionToggleOverviewRail"));
        QVERIFY(action != nullptr);
        auto *railModel = window.findChild<OverviewRailModel *>();
        QVERIFY2(railModel != nullptr, "OverviewRailModel must be a QObject child of MainWindow");
        QVERIFY2(action->isChecked(), "rail should be on by default");
        QVERIFY2(railModel->BucketCount() > 0, "model must be armed while the rail is visible");

        action->trigger();
        QVERIFY(!action->isChecked());
        QCOMPARE(railModel->BucketCount(), static_cast<std::size_t>(0));

        action->trigger();
        QVERIFY(action->isChecked());
        QVERIFY2(
            railModel->BucketCount() > 0, "re-showing the rail must re-sync the bucket count from the widget height"
        );
    }

    /// Closing and reopening the find dock must restore unbiased
    /// rail ticks from cached per-bucket counts — not the capped
    /// `sortedRows` list. Pins the regression where a cache-hit
    /// debounce after `revealed` left the rail painting only the
    /// first 10 000 hits of a common needle.
    static void TestFindDockRevealRestoresUnbiasedMatchTicks()
    {
        {
            QSettings settings;
            settings.remove(QStringLiteral("ui/showOverviewRail"));
        }
        MainWindow window;
        window.show();
        QCoreApplication::processEvents();

        constexpr int ROWS = 40;
        const RailFixture fixture(ROWS);
        auto *model = window.Model();
        QVERIFY(model != nullptr);
        StreamJsonPathInto(*model, fixture.Path());
        QCOMPARE(window.FilterModel()->rowCount(), ROWS);

        auto *railModel = window.findChild<OverviewRailModel *>();
        QVERIFY(railModel != nullptr);
        QVERIFY(railModel->BucketCount() > 0);

        auto *findDock = window.findChild<FindDock *>();
        QVERIFY(findDock != nullptr);
        findDock->show();
        QCoreApplication::processEvents();
        QVERIFY(findDock->isVisible());

        // "msg" matches every fixture row — spreads ticks across
        // the whole rail when bucketed correctly.
        QVERIFY(QMetaObject::invokeMethod(
            &window,
            "UpdateFindMatchCount",
            Qt::DirectConnection,
            Q_ARG(QString, QStringLiteral("msg")),
            Q_ARG(bool, false),
            Q_ARG(bool, false)
        ));
        QVERIFY(railModel->HasMatchTicks());

        std::vector<uint32_t> baseline;
        baseline.reserve(railModel->BucketCount());
        uint32_t baselineTotal = 0;
        std::size_t bucketsWithTicks = 0;
        for (const auto &bucket : railModel->Buckets())
        {
            baseline.push_back(bucket.matchCount);
            baselineTotal += bucket.matchCount;
            if (bucket.matchCount > 0)
            {
                ++bucketsWithTicks;
            }
        }
        QCOMPARE(baselineTotal, static_cast<uint32_t>(ROWS));
        QVERIFY2(bucketsWithTicks > 1, "matches must span more than one bucket before close");

        // Close clears live rail ticks; cache survives.
        emit findDock->closed();
        QVERIFY2(!railModel->HasMatchTicks(), "closing find must clear rail match ticks");

        // Reveal must restore the same unbiased distribution —
        // not a top-biased strip from capped sortedRows.
        emit findDock->revealed();
        QVERIFY2(railModel->HasMatchTicks(), "revealed find must restore rail match ticks");
        QCOMPARE(railModel->BucketCount(), baseline.size());
        for (std::size_t i = 0; i < baseline.size(); ++i)
        {
            QCOMPARE(railModel->Buckets()[i].matchCount, baseline[i]);
        }
    }

    /// Resizing the rail widget past a bucket-count boundary
    /// (e.g. user drags the window taller, `SyncBucketCountToHeight`
    /// calls `SetBucketCount(newH)`) invalidates the durable
    /// `mMatchBucketCounts` — sizes disagree, so `ApplyStoredMatch-
    /// BucketCounts` no-ops and the freshly-rebuilt buckets have
    /// zero ticks. Without the `bucketsChanged` -> `PushFindMatches-
    /// ToOverviewRail` wiring in `MainWindow`, the rail would then
    /// paint an empty match strip until the next find-bar debounce
    /// (or forever, if the user isn't typing). Pins that the wire
    /// triggers a synchronous recount and re-installs ticks at the
    /// new H.
    static void TestRailResizeReinstallsFindTicksAtNewBucketCount()
    {
        {
            QSettings settings;
            settings.remove(QStringLiteral("ui/showOverviewRail"));
        }
        MainWindow window;
        window.show();
        QCoreApplication::processEvents();

        constexpr int ROWS = 40;
        const RailFixture fixture(ROWS);
        auto *model = window.Model();
        QVERIFY(model != nullptr);
        StreamJsonPathInto(*model, fixture.Path());
        QCOMPARE(window.FilterModel()->rowCount(), ROWS);

        auto *railModel = window.findChild<OverviewRailModel *>();
        QVERIFY(railModel != nullptr);
        const std::size_t initialBuckets = railModel->BucketCount();
        QVERIFY(initialBuckets > 0);

        auto *findDock = window.findChild<FindDock *>();
        QVERIFY(findDock != nullptr);
        findDock->show();
        QCoreApplication::processEvents();

        // Broad needle: every row is a hit, so every bucket must
        // light up when ticks are correctly installed.
        QVERIFY(QMetaObject::invokeMethod(
            &window,
            "UpdateFindMatchCount",
            Qt::DirectConnection,
            Q_ARG(QString, QStringLiteral("msg")),
            Q_ARG(bool, false),
            Q_ARG(bool, false)
        ));
        QVERIFY2(railModel->HasMatchTicks(), "broad find must light up rail ticks before resize");

        // Simulate a rail-widget height change by pushing a new
        // bucket count into the model — same call path the widget
        // takes through `SyncBucketCountToHeight`. Pick a value
        // that is provably different from `initialBuckets` so the
        // durable-counts fast path can't accidentally rescue us.
        const std::size_t newBuckets = (initialBuckets == 24) ? 48 : 24;
        railModel->SetBucketCount(newBuckets);

        // Post-condition: the `bucketsChanged` slot fires
        // synchronously (DirectConnection, same thread), the slot
        // sees size-mismatched cached counts, drops into the
        // rescan branch of `PushFindMatchesToOverviewRail`, and
        // installs fresh ticks against `newBuckets` before this
        // call returns. No event-loop pump needed.
        QCOMPARE(railModel->BucketCount(), newBuckets);
        QVERIFY2(
            railModel->HasMatchTicks(),
            "rail-resize past a bucket-count boundary must not leave ticks empty; "
            "bucketsChanged is wired to PushFindMatchesToOverviewRail's rescan branch"
        );
        // Every fixture row is a hit and there are more rows than
        // buckets, so *every* bucket should carry a tick after the
        // rescan. Pins the "unbiased distribution" property in
        // addition to plain `HasMatchTicks`.
        for (std::size_t i = 0; i < newBuckets; ++i)
        {
            QVERIFY2(railModel->Buckets()[i].matchCount > 0, "every bucket must carry a tick after resize rescan");
        }
    }

    /// A drag scrub across the rail must preserve the user's
    /// existing selection. Simulates the emit the widget sends
    /// on `mouseMoveEvent` (replaceSelection == false) and pins
    /// that `ScrollToProxyRow` doesn't clobber the caller's
    /// selection model.
    static void TestScrollToProxyRowPreservesSelectionOnDrag()
    {
        {
            QSettings settings;
            settings.remove(QStringLiteral("ui/showOverviewRail"));
        }
        MainWindow window;
        window.show();

        auto *view = window.findChild<LogTableView *>();
        QVERIFY(view != nullptr);

        // Populate the model so there are rows to select.
        constexpr int ROWS = 30;
        const RailFixture fixture(ROWS);
        auto *model = window.Model();
        QVERIFY(model != nullptr);
        StreamJsonPathInto(*model, fixture.Path());

        auto *proxy = window.FilterModel();
        QVERIFY(proxy != nullptr);
        QCOMPARE(proxy->rowCount(), ROWS);

        // Seed a multi-row selection [3, 4, 5].
        auto *selection = view->selectionModel();
        QVERIFY(selection != nullptr);
        selection->clearSelection();
        for (int r = 3; r <= 5; ++r)
        {
            selection->select(proxy->index(r, 0), QItemSelectionModel::Select | QItemSelectionModel::Rows);
        }
        QCOMPARE(selection->selectedRows().size(), 3);

        // Drag scrub to row 20 -- selection should survive.
        window.ScrollToProxyRow(20, /*replaceSelection=*/false);
        QCOMPARE(selection->selectedRows().size(), 3);

        // Fresh click at row 25 -- selection is replaced with
        // just that row (matches `SelectSourceRow` semantics).
        window.ScrollToProxyRow(25, /*replaceSelection=*/true);
        QCOMPARE(selection->selectedRows().size(), 1);
        QCOMPARE(selection->selectedRows().front().row(), 25);
    }

    /// Toggling the rail off (`SetBucketCount(0)`) drops the
    /// bucket vector but preserves the cached match rows.
    /// Toggling back on (`SetBucketCount(H)`) folds the cached
    /// ticks into fresh buckets so the widget doesn't paint a
    /// blank match band. Pins the "hide, keep find results,
    /// reveal, ticks restored" cycle used by
    /// `MainWindow::SetOverviewRailVisible`.
    static void TestHideAndShowRestoresMatchTicks()
    {
        LogModel model;
        QObject owner;
        const auto chain = BuildProxyChain(&model, &owner);
        OverviewRailModel rail(chain.filter, &model, /*anchors=*/nullptr);
        rail.SetBucketCount(20);

        constexpr int ROWS = 20;
        const RailFixture fixture(ROWS);
        StreamJsonPathInto(model, fixture.Path());
        WaitForBucketsChanged(rail);
        QCOMPARE(rail.ProxyRowCount(), ROWS);

        rail.SetMatchProxyRows({4, 9, 14});
        QVERIFY(rail.HasMatchTicks());

        // Hide: bucket vector drops, `HasMatchTicks` -> false
        // (bucketed count is zero because there are no buckets
        // to fold into), but the raw match list is retained.
        rail.SetBucketCount(0);
        QCOMPARE(rail.BucketCount(), static_cast<std::size_t>(0));
        QVERIFY2(!rail.HasMatchTicks(), "hidden rail must report no match ticks");

        // Reveal: `SetBucketCount(H)` reallocates buckets,
        // `RebuildInternal` folds the retained match list into
        // them, and `HasMatchTicks` flips back on without the
        // caller re-pushing the list.
        rail.SetBucketCount(20);
        QVERIFY2(rail.HasMatchTicks(), "revealed rail must restore the cached match ticks");
        std::uint32_t totalMatches = 0;
        for (const auto &bucket : rail.Buckets())
        {
            totalMatches += bucket.matchCount;
        }
        QCOMPARE(totalMatches, static_cast<std::uint32_t>(3));
    }

    /// Installing a filter on the outer proxy fires
    /// `layoutChanged`, which the rail wires to `Rebuild`.
    /// Assert we get a `bucketsChanged` emit within the
    /// coalesce window (pins the wiring path that
    /// `TestRebuildIsCoalesced` doesn't exercise).
    static void TestLayoutChangedTriggersRebuild()
    {
        LogModel model;
        QObject owner;
        const auto chain = BuildProxyChain(&model, &owner);
        OverviewRailModel rail(chain.filter, &model, /*anchors=*/nullptr);
        rail.SetBucketCount(16);

        constexpr int ROWS = 20;
        const RailFixture fixture(ROWS);
        StreamJsonPathInto(model, fixture.Path());
        WaitForBucketsChanged(rail);
        QCOMPARE(rail.ProxyRowCount(), ROWS);

        // Install a callback filter on column 0 that drops
        // every other source row. The rules change fires
        // `layoutChanged` on `LogFilterModel`, which the rail
        // wires to `Rebuild` -> coalesce timer.
        QSignalSpy spy(&rail, &OverviewRailModel::bucketsChanged);
        QVERIFY(spy.isValid());
        std::vector<loglib::RowPredicate> rules;
        rules.emplace_back(
            std::in_place_type<loglib::CallbackStringRowPredicate>,
            /*columnIndex=*/static_cast<std::size_t>(0),
            /*match=*/
            [callCount = std::make_shared<std::size_t>(0)](std::string_view) mutable {
                return ((*callCount)++ % 2) == 0;
            }
        );
        chain.filter->SetFilterRules(std::move(rules));

        QVERIFY2(spy.wait(1000), "layoutChanged must trigger a rebuild via the coalesce timer");
        QVERIFY2(rail.ProxyRowCount() < ROWS, "filter must reduce the proxy row count");
    }

    /// Duplicate entries in the match-rows list must not
    /// double-count into their bucket. Pins the `sort + unique`
    /// normalisation in `SetMatchProxyRows`.
    static void TestSetMatchProxyRowsDedupsDuplicates()
    {
        LogModel model;
        QObject owner;
        const auto chain = BuildProxyChain(&model, &owner);
        OverviewRailModel rail(chain.filter, &model, /*anchors=*/nullptr);
        rail.SetBucketCount(20);

        constexpr int ROWS = 20;
        const RailFixture fixture(ROWS);
        StreamJsonPathInto(model, fixture.Path());
        WaitForBucketsChanged(rail);

        // Same three unique rows, plus assorted duplicates and
        // out-of-order entries.
        rail.SetMatchProxyRows({5, 10, 5, 15, 10, 5, 15, 15});
        QVERIFY(rail.HasMatchTicks());
        std::uint32_t total = 0;
        for (const auto &bucket : rail.Buckets())
        {
            total += bucket.matchCount;
        }
        QCOMPARE(total, static_cast<std::uint32_t>(3));
    }

    /// `enumColumnsChanged(Grew, ...)` never moves the level
    /// column (only `Promoted` / `Demoted` do), so it must not
    /// schedule a rebuild. Regression against a wide-log parse
    /// paying `O(nColumns)` scans on every dictionary grow.
    static void TestEnumGrewDoesNotTriggerRebuild()
    {
        LogModel model;
        QObject owner;
        const auto chain = BuildProxyChain(&model, &owner);
        OverviewRailModel rail(chain.filter, &model, /*anchors=*/nullptr);
        rail.SetBucketCount(16);

        constexpr int ROWS = 16;
        const RailFixture fixture(ROWS);
        StreamJsonPathInto(model, fixture.Path());
        WaitForBucketsChanged(rail);

        const QSignalSpy spy(&rail, &OverviewRailModel::bucketsChanged);
        QVERIFY(spy.isValid());
        // Fire a synthetic `Grew` on the level column (the
        // wide-log worst case). The rail's connect explicitly
        // short-circuits `Grew` so the coalesce timer must not
        // arm.
        emit model.enumColumnsChanged(EnumColumnsChangeReason::Grew, /*columnIndex=*/0);

        // Wait > coalesce interval to be sure we didn't
        // just miss a scheduled emit. 200 ms is well past the
        // 50 ms timer plus event-loop slack.
        QTest::qWait(200);
        QCOMPARE_LE(spy.count(), 0);
    }

    /// A rail widget destroyed through a non-`AttachOverviewRail`
    /// path silently clears the view's `QPointer`. The next
    /// geometry pass must reclaim the reserved right margin so
    /// the viewport doesn't keep a phantom gap.
    static void TestPhantomMarginResetsWhenRailDestroyed()
    {
        LogTableView view;
        view.resize(800, 400);
        view.show();

        constexpr int RAIL_WIDTH = 16;
        auto *rail = new QWidget(nullptr);
        rail->setFixedWidth(RAIL_WIDTH);
        view.AttachOverviewRail(rail);
        QVERIFY(view.ReservedRightMargin() > 0);

        // External destruction: `mOverviewRail` (QPointer) is
        // cleared silently by Qt's QObject teardown. The view
        // must notice on the next geometry pass and reclaim the
        // margin without a call to `AttachOverviewRail(nullptr)`.
        delete rail;
        QCOMPARE(view.OverviewRail(), static_cast<QWidget *>(nullptr));

        // Trigger `UpdateOverviewRailGeometry` via a resize.
        view.resize(view.width() + 10, view.height());
        // Give Qt a chance to deliver the resize event.
        QCoreApplication::processEvents();
        QCOMPARE(view.ReservedRightMargin(), 0);
    }

    /// `SetMatchBucketCounts` installs pre-bucketed match counts
    /// directly, without going through the raw-row fold path.
    /// This is the API `MainWindow::UpdateFindMatchCount` uses to
    /// feed the rail unbiased ticks when a broad needle would
    /// otherwise fill an O(matches) row vector on the GUI thread.
    static void TestSetMatchBucketCountsBasic()
    {
        LogModel model;
        QObject owner;
        const auto chain = BuildProxyChain(&model, &owner);
        OverviewRailModel rail(chain.filter, &model, /*anchors=*/nullptr);
        rail.SetBucketCount(20);

        constexpr int ROWS = 20;
        const RailFixture fixture(ROWS);
        StreamJsonPathInto(model, fixture.Path());
        WaitForBucketsChanged(rail);
        QCOMPARE(rail.ProxyRowCount(), ROWS);
        QVERIFY(!rail.HasMatchTicks());

        std::vector<uint32_t> counts(20, uint32_t{0});
        counts[3] = 7;
        counts[19] = 42;

        const QSignalSpy matchSpy(&rail, &OverviewRailModel::matchesChanged);
        rail.SetMatchBucketCounts(counts, /*totalMatches=*/49);
        QCOMPARE(matchSpy.count(), 1);
        QVERIFY(rail.HasMatchTicks());

        std::size_t i = 0;
        for (const auto &bucket : rail.Buckets())
        {
            QCOMPARE(bucket.matchCount, counts[i]);
            ++i;
        }

        // Zeroing every bucket flips `HasMatchTicks` back off,
        // matching the row-list path.
        rail.SetMatchBucketCounts(std::vector<uint32_t>(20, uint32_t{0}), /*totalMatches=*/0);
        QVERIFY(!rail.HasMatchTicks());
    }

    /// `SetMatchBucketCounts` must leave the level counts and
    /// anchor bits untouched; only the tick counters move.
    /// Mirrors `TestSetMatchProxyRowsLeavesLevelCountsUntouched`
    /// for the bucketed API so future refactors can't silently
    /// tie a match-set push to a full rebuild.
    static void TestSetMatchBucketCountsLeavesLevelCountsUntouched()
    {
        LogModel model;
        QObject owner;
        const auto chain = BuildProxyChain(&model, &owner);
        OverviewRailModel rail(chain.filter, &model, /*anchors=*/nullptr);
        rail.SetBucketCount(20);

        constexpr int ROWS = 20;
        const RailFixture fixture(ROWS);
        StreamJsonPathInto(model, fixture.Path());
        WaitForBucketsChanged(rail);
        QCOMPARE(rail.ProxyRowCount(), ROWS);

        std::vector<loglib::LevelBucket> baseline;
        baseline.reserve(rail.BucketCount());
        for (const auto &bucket : rail.Buckets())
        {
            baseline.push_back(bucket.levels);
        }

        std::vector<uint32_t> counts(20, uint32_t{0});
        counts[5] = 3;
        counts[10] = 5;
        rail.SetMatchBucketCounts(counts, /*totalMatches=*/8);

        std::size_t i = 0;
        for (const auto &bucket : rail.Buckets())
        {
            QCOMPARE(bucket.levels.counts, baseline[i].counts);
            ++i;
        }
    }

    /// Size mismatch (mid-resize race, or a caller with stale
    /// bucket-count info) must be dropped silently so a half-
    /// applied vector cannot leak into the rail. Pins the
    /// defensive size check on `SetMatchBucketCounts`.
    static void TestSetMatchBucketCountsSizeMismatchDropsSilently()
    {
        LogModel model;
        QObject owner;
        const auto chain = BuildProxyChain(&model, &owner);
        OverviewRailModel rail(chain.filter, &model, /*anchors=*/nullptr);
        rail.SetBucketCount(20);

        constexpr int ROWS = 20;
        const RailFixture fixture(ROWS);
        StreamJsonPathInto(model, fixture.Path());
        WaitForBucketsChanged(rail);
        QCOMPARE(rail.ProxyRowCount(), ROWS);

        // Prime the rail with a known set of ticks so we can
        // prove the mismatched call didn't overwrite them.
        rail.SetMatchProxyRows({5, 10, 15});
        QVERIFY(rail.HasMatchTicks());
        const uint32_t baselineTotal = [&]() {
            uint32_t sum = 0;
            for (const auto &bucket : rail.Buckets())
            {
                sum += bucket.matchCount;
            }
            return sum;
        }();
        QCOMPARE(baselineTotal, static_cast<uint32_t>(3));

        const QSignalSpy matchSpy(&rail, &OverviewRailModel::matchesChanged);
        // Off-by-one on the vector size: rail has 20 buckets.
        rail.SetMatchBucketCounts(std::vector<uint32_t>(19, uint32_t{7}), /*totalMatches=*/133);
        QCOMPARE(matchSpy.count(), 0);

        // Original tick totals must survive the rejected push.
        uint32_t survivedTotal = 0;
        for (const auto &bucket : rail.Buckets())
        {
            survivedTotal += bucket.matchCount;
        }
        QCOMPARE(survivedTotal, baselineTotal);
    }

    /// After `SetMatchBucketCounts` the raw-rows vector must be
    /// cleared *and* the durable bucket counts retained, so a
    /// follow-up full rebuild (anchor edit, coalesced proxy
    /// change, hide→show at the same H) re-applies the same
    /// ticks without double-folding a stale row list on top.
    static void TestSetMatchBucketCountsClearsRawRowsSoRebuildDoesNotDoubleFold()
    {
        LogModel model;
        QObject owner;
        const auto chain = BuildProxyChain(&model, &owner);
        OverviewRailModel rail(chain.filter, &model, /*anchors=*/nullptr);
        rail.SetBucketCount(20);

        constexpr int ROWS = 20;
        const RailFixture fixture(ROWS);
        StreamJsonPathInto(model, fixture.Path());
        WaitForBucketsChanged(rail);

        // Seed the raw-rows path so `mMatchProxyRows` is
        // non-empty going in.
        rail.SetMatchProxyRows({2, 4, 6, 8});
        QCOMPARE(rail.HasMatchTicks(), true);

        // Now overwrite via the bucketed API. The buckets should
        // reflect the new counts exactly, not "new counts + old
        // row-list fold".
        std::vector<uint32_t> counts(20, uint32_t{0});
        counts[10] = 1;
        rail.SetMatchBucketCounts(counts, /*totalMatches=*/1);

        // Force a synchronous full rebuild. Durable bucket counts
        // must survive (same H); the cleared raw-rows list must
        // not resurrect the earlier {2,4,6,8} fold on top.
        rail.RebuildNow();
        uint32_t afterRebuild = 0;
        for (const auto &bucket : rail.Buckets())
        {
            afterRebuild += bucket.matchCount;
        }
        QCOMPARE(afterRebuild, static_cast<uint32_t>(1));
        QCOMPARE(rail.Buckets()[10].matchCount, static_cast<uint32_t>(1));
        QVERIFY(rail.HasMatchTicks());
    }

    /// Bucket-counts path mirrors `TestHideAndShowRestoresMatchTicks`:
    /// `SetBucketCount(0)` drops the live vector but keeps durable
    /// counts, and `SetBucketCount(H)` re-applies them without the
    /// caller re-pushing.
    static void TestSetMatchBucketCountsSurviveHideAndShow()
    {
        LogModel model;
        QObject owner;
        const auto chain = BuildProxyChain(&model, &owner);
        OverviewRailModel rail(chain.filter, &model, /*anchors=*/nullptr);
        rail.SetBucketCount(20);

        constexpr int ROWS = 20;
        const RailFixture fixture(ROWS);
        StreamJsonPathInto(model, fixture.Path());
        WaitForBucketsChanged(rail);

        std::vector<uint32_t> counts(20, uint32_t{0});
        counts[3] = 5;
        counts[17] = 2;
        rail.SetMatchBucketCounts(counts, /*totalMatches=*/7);
        QVERIFY(rail.HasMatchTicks());

        rail.SetBucketCount(0);
        QVERIFY2(!rail.HasMatchTicks(), "hidden rail must report no match ticks");

        rail.SetBucketCount(20);
        QVERIFY2(rail.HasMatchTicks(), "revealed rail must restore durable bucket counts");
        QCOMPARE(rail.Buckets()[3].matchCount, static_cast<uint32_t>(5));
        QCOMPARE(rail.Buckets()[17].matchCount, static_cast<uint32_t>(2));
        uint32_t total = 0;
        for (const auto &bucket : rail.Buckets())
        {
            total += bucket.matchCount;
        }
        QCOMPARE(total, static_cast<uint32_t>(7));
    }

    /// Changing H invalidates durable bucket counts (size mismatch).
    /// Rebuild must not invent ticks; the caller (MainWindow) is
    /// responsible for rescanning against the new bucket count.
    static void TestSetMatchBucketCountsDroppedOnBucketCountChange()
    {
        LogModel model;
        QObject owner;
        const auto chain = BuildProxyChain(&model, &owner);
        OverviewRailModel rail(chain.filter, &model, /*anchors=*/nullptr);
        rail.SetBucketCount(20);

        constexpr int ROWS = 20;
        const RailFixture fixture(ROWS);
        StreamJsonPathInto(model, fixture.Path());
        WaitForBucketsChanged(rail);

        std::vector<uint32_t> counts(20, uint32_t{0});
        counts[5] = 4;
        rail.SetMatchBucketCounts(counts, /*totalMatches=*/4);
        QVERIFY(rail.HasMatchTicks());

        rail.SetBucketCount(30);
        QVERIFY2(!rail.HasMatchTicks(), "H change must drop size-mismatched durable counts");
    }

    /// Regression for the "stale proxy row count misplaces ticks"
    /// bug. `RefreshMatchTicks` / `FoldMatchTicksIntoBuckets` use
    /// `mProxyRowCount`, which was only written inside the
    /// coalesced `RebuildInternal`. A `SetMatchProxyRows` call
    /// scheduled between a filter/insert layout change and the
    /// next rebuild would fold rows referenced to the live row
    /// count against a stale denominator, dropping hits (when
    /// the proxy grew) or misplacing them into the last bucket
    /// (either direction).
    ///
    /// The fix: `SetMatchProxyRows` refreshes `mProxyRowCount`
    /// from the live proxy at entry, so the fold uses the same
    /// denominator the caller scanned against.
    ///
    /// We drive the divergence with a filter install: it fires
    /// `layoutChanged` on `LogFilterModel`, which the rail wires
    /// to `Rebuild` -> `ScheduleRebuild` (50 ms coalesce timer).
    /// Between `SetFilterRules` returning and the timer firing,
    /// no event-loop iteration happens in this test, so the
    /// cached `mProxyRowCount` is guaranteed to be stale when
    /// we call `SetMatchProxyRows` on the next line.
    static void TestSetMatchProxyRowsRefreshesRowCountAfterProxyChurn()
    {
        LogModel model;
        QObject owner;
        const auto chain = BuildProxyChain(&model, &owner);
        OverviewRailModel rail(chain.filter, &model, /*anchors=*/nullptr);
        rail.SetBucketCount(20);

        constexpr int ROWS = 40;
        const RailFixture fixture(ROWS);
        StreamJsonPathInto(model, fixture.Path());
        WaitForBucketsChanged(rail);
        QCOMPARE(rail.ProxyRowCount(), ROWS);

        // Install a filter that drops every other row. The
        // proxy's row count halves (~20 rows) but the rail's
        // cached `mProxyRowCount` stays at 40 until the coalesce
        // timer fires ~50 ms later.
        std::vector<loglib::RowPredicate> rules;
        rules.emplace_back(
            std::in_place_type<loglib::CallbackStringRowPredicate>,
            static_cast<std::size_t>(0),
            [callCount = std::make_shared<std::size_t>(0)](std::string_view) mutable {
                return ((*callCount)++ % 2) == 0;
            }
        );
        chain.filter->SetFilterRules(std::move(rules));
        const int liveRowCount = chain.filter->rowCount();
        QVERIFY2(liveRowCount > 0 && liveRowCount < ROWS, "filter must reduce the proxy row count");

        // Under the pre-fix code path, `mProxyRowCount` stays 40
        // and folding row `liveRowCount - 1` (in range under the
        // filter, but potentially misbucketed against the old
        // denominator) misplaces the tick. Under the fix,
        // `mProxyRowCount` refreshes and the tick lands correctly.
        rail.SetMatchProxyRows({liveRowCount - 1});
        QCOMPARE(rail.ProxyRowCount(), liveRowCount);
        QVERIFY(rail.HasMatchTicks());

        // Sanity: the tick sits in one of the tail buckets --
        // `(liveRowCount - 1) * 20 / liveRowCount` rounds to
        // ~19 for any positive `liveRowCount`.
        uint32_t totalTicks = 0;
        std::size_t highestBucketWithTick = 0;
        for (std::size_t i = 0; i < rail.BucketCount(); ++i)
        {
            const uint32_t c = rail.Buckets()[i].matchCount;
            totalTicks += c;
            if (c > 0)
            {
                highestBucketWithTick = i;
            }
        }
        QCOMPARE(totalTicks, static_cast<uint32_t>(1));
        QCOMPARE(highestBucketWithTick, rail.BucketCount() - 1);
    }

    /// Companion regression for the bucketed API: same stale-
    /// denominator failure mode, checked through
    /// `SetMatchBucketCounts` (which is what `MainWindow`
    /// actually calls under the fix).
    static void TestSetMatchBucketCountsRefreshesRowCountAfterProxyChurn()
    {
        LogModel model;
        QObject owner;
        const auto chain = BuildProxyChain(&model, &owner);
        OverviewRailModel rail(chain.filter, &model, /*anchors=*/nullptr);
        rail.SetBucketCount(20);

        constexpr int ROWS = 40;
        const RailFixture fixture(ROWS);
        StreamJsonPathInto(model, fixture.Path());
        WaitForBucketsChanged(rail);
        QCOMPARE(rail.ProxyRowCount(), ROWS);

        std::vector<loglib::RowPredicate> rules;
        rules.emplace_back(
            std::in_place_type<loglib::CallbackStringRowPredicate>,
            static_cast<std::size_t>(0),
            [callCount = std::make_shared<std::size_t>(0)](std::string_view) mutable {
                return ((*callCount)++ % 2) == 0;
            }
        );
        chain.filter->SetFilterRules(std::move(rules));
        const int liveRowCount = chain.filter->rowCount();
        QVERIFY2(liveRowCount > 0 && liveRowCount < ROWS, "filter must reduce the proxy row count");

        std::vector<uint32_t> counts(rail.BucketCount(), uint32_t{0});
        counts[rail.BucketCount() - 1] = 3;
        rail.SetMatchBucketCounts(counts, /*totalMatches=*/3);

        // The row count must refresh independently of the raw-
        // rows path: `SetMatchBucketCounts` has its own
        // `mProxyRowCount = mProxyModel->rowCount()` assignment
        // so downstream `HasMatchTicks` / `BucketForProxyRow`
        // math sees the current denominator.
        QCOMPARE(rail.ProxyRowCount(), liveRowCount);
        QVERIFY(rail.HasMatchTicks());
    }

    /// `LogFilterModel::ForEachMatchingRow` walks every matching
    /// row without allocating a `QList<QModelIndex>`, which is
    /// how `MainWindow::UpdateFindMatchCount` avoids the memory
    /// spike on a broad needle. Pin the contract: the callback
    /// fires once per matching row, in ascending order, and
    /// returning `false` stops the walk mid-stream. Removing the
    /// hard 10 000-hit cap on the sorted-rows cache is only safe
    /// if callers can accumulate counters cheaply without the
    /// intermediate list.
    static void TestForEachMatchingRowStreamsWithoutMaterialising()
    {
        LogModel model;
        QObject owner;
        const auto chain = BuildProxyChain(&model, &owner);

        constexpr int ROWS = 30;
        const RailFixture fixture(ROWS);
        StreamJsonPathInto(model, fixture.Path());
        QCOMPARE(chain.filter->rowCount(), ROWS);

        // Every fixture row carries a `"msg N"` body, so "msg"
        // matches every row.
        const QModelIndex start = chain.filter->index(0, 0);
        QVERIFY(start.isValid());

        std::vector<int> seenRows;
        seenRows.reserve(ROWS);
        chain.filter->ForEachMatchingRow(
            start,
            Qt::DisplayRole,
            QVariant::fromValue(QStringLiteral("msg")),
            Qt::MatchContains | Qt::MatchWrap,
            /*forward=*/true,
            /*skipFirstN=*/0,
            [&](const QModelIndex &proxyIndex) -> bool {
                seenRows.push_back(proxyIndex.row());
                return true;
            }
        );

        QCOMPARE(static_cast<int>(seenRows.size()), ROWS);
        QVERIFY2(std::ranges::is_sorted(seenRows), "ForEachMatchingRow must yield rows in ascending order");
        QVERIFY2(std::ranges::adjacent_find(seenRows) == seenRows.end(), "ForEachMatchingRow must not repeat a row");

        // Returning `false` stops the walk on the first hit. The
        // caller has full control over the accumulator shape.
        int visitedCount = 0;
        chain.filter->ForEachMatchingRow(
            start,
            Qt::DisplayRole,
            QVariant::fromValue(QStringLiteral("msg")),
            Qt::MatchContains | Qt::MatchWrap,
            /*forward=*/true,
            /*skipFirstN=*/0,
            [&](const QModelIndex &) -> bool {
                ++visitedCount;
                return false;
            }
        );
        QCOMPARE(visitedCount, 1);
    }

    /// Regression for the "uncapped find scan" bug. Pin the two
    /// consumer patterns `MainWindow::UpdateFindMatchCount` uses
    /// to avoid the O(matches) allocation while keeping the rail
    /// unbiased:
    ///   1. A capped row vector for the "*i* of *N*" binary
    ///      search (never grows past `SCAN_CAP` regardless of
    ///      total hits).
    ///   2. An `nBuckets`-sized counter vector for the rail (sees
    ///      every hit, including tail rows past the row-vector
    ///      cap).
    ///
    /// This test drives `ForEachMatchingRow` with a broad needle
    /// that matches every row and asserts both invariants
    /// simultaneously.
    static void TestForEachMatchingRowFeedsCappedListPlusUnbiasedBuckets()
    {
        LogModel model;
        QObject owner;
        const auto chain = BuildProxyChain(&model, &owner);

        constexpr int ROWS = 60;
        constexpr int SCAN_CAP = 10;
        constexpr std::size_t BUCKETS = 6;
        const RailFixture fixture(ROWS);
        StreamJsonPathInto(model, fixture.Path());
        QCOMPARE(chain.filter->rowCount(), ROWS);

        const QModelIndex start = chain.filter->index(0, 0);
        QVERIFY(start.isValid());

        std::vector<int> cappedRows;
        cappedRows.reserve(SCAN_CAP);
        std::vector<uint32_t> bucketCounts(BUCKETS, uint32_t{0});
        uint32_t totalMatches = 0;
        chain.filter->ForEachMatchingRow(
            start,
            Qt::DisplayRole,
            QVariant::fromValue(QStringLiteral("msg")),
            Qt::MatchContains | Qt::MatchWrap,
            /*forward=*/true,
            /*skipFirstN=*/0,
            [&](const QModelIndex &proxyIndex) -> bool {
                ++totalMatches;
                const std::size_t bucket =
                    (static_cast<std::size_t>(proxyIndex.row()) * BUCKETS) / static_cast<std::size_t>(ROWS);
                ++bucketCounts[std::min(bucket, BUCKETS - 1)];
                if (static_cast<int>(cappedRows.size()) < SCAN_CAP)
                {
                    cappedRows.push_back(proxyIndex.row());
                }
                return true;
            }
        );

        QCOMPARE(totalMatches, static_cast<uint32_t>(ROWS));
        QCOMPARE(static_cast<int>(cappedRows.size()), SCAN_CAP);
        // The capped list holds only the first N rows, biased to
        // the top of the log (this is the trade-off the row cap
        // makes -- the "*i* of *N*" navigator degrades past the
        // cap, but the total count and the rail stay accurate).
        QCOMPARE(cappedRows.front(), 0);
        QCOMPARE(cappedRows.back(), SCAN_CAP - 1);

        // Bucket sum must equal total (no matches dropped). Every
        // bucket must carry at least one hit since matches are
        // spread across all rows -- this is the "unbiased rail"
        // property that the cap on `cappedRows` alone would have
        // broken.
        uint32_t summedFromBuckets = 0;
        for (std::size_t i = 0; i < BUCKETS; ++i)
        {
            summedFromBuckets += bucketCounts[i];
            QVERIFY2(
                bucketCounts[i] > 0, qPrintable(QStringLiteral("bucket %1 must carry at least one match tick").arg(i))
            );
        }
        QCOMPARE(summedFromBuckets, totalMatches);
    }

    /// Pins the early-exit policy `UpdateFindMatchCount` uses so a
    /// common needle cannot force a full proxy walk on the GUI
    /// thread: once the navigator list is past its cap *and* every
    /// rail bucket already has a presence tick, the walk stops.
    /// Paint is presence-only, so further density is irrelevant;
    /// sparse needles (not every bucket lit) still scan to the end.
    static void TestFindMatchScanEarlyExitsWhenRailPresenceSettled()
    {
        LogModel model;
        QObject owner;
        const auto chain = BuildProxyChain(&model, &owner);

        constexpr int ROWS = 60;
        constexpr int SCAN_CAP = 10;
        constexpr std::size_t BUCKETS = 6;
        const RailFixture fixture(ROWS);
        StreamJsonPathInto(model, fixture.Path());
        QCOMPARE(chain.filter->rowCount(), ROWS);

        const QModelIndex start = chain.filter->index(0, 0);
        QVERIFY(start.isValid());

        std::vector<int> cappedRows;
        cappedRows.reserve(SCAN_CAP);
        std::vector<uint32_t> bucketCounts(BUCKETS, uint32_t{0});
        uint32_t totalMatches = 0;
        std::size_t bucketsHit = 0;
        bool scanExhausted = true;
        int callbackInvocations = 0;
        chain.filter->ForEachMatchingRow(
            start,
            Qt::DisplayRole,
            QVariant::fromValue(QStringLiteral("msg")),
            Qt::MatchContains | Qt::MatchWrap,
            /*forward=*/true,
            /*skipFirstN=*/0,
            [&](const QModelIndex &proxyIndex) -> bool {
                ++callbackInvocations;
                ++totalMatches;
                const std::size_t bucket =
                    (static_cast<std::size_t>(proxyIndex.row()) * BUCKETS) / static_cast<std::size_t>(ROWS);
                uint32_t &slot = bucketCounts[std::min(bucket, BUCKETS - 1)];
                if (slot == 0)
                {
                    ++bucketsHit;
                }
                ++slot;
                if (static_cast<int>(cappedRows.size()) < SCAN_CAP)
                {
                    cappedRows.push_back(proxyIndex.row());
                }
                if (totalMatches > static_cast<uint32_t>(SCAN_CAP) && bucketsHit >= BUCKETS)
                {
                    scanExhausted = false;
                    return false;
                }
                return true;
            }
        );

        QVERIFY2(!scanExhausted, "dense needle must early-exit once every bucket has a tick");
        QVERIFY2(callbackInvocations < ROWS, "early-exit must stop before visiting every matching row");
        QCOMPARE(static_cast<int>(cappedRows.size()), SCAN_CAP);
        QCOMPARE(bucketsHit, BUCKETS);
        for (std::size_t i = 0; i < BUCKETS; ++i)
        {
            QVERIFY2(bucketCounts[i] > 0, "every bucket must carry a presence tick after early-exit");
        }

        // Sparse needle: only the first few rows match a unique
        // body token, so not every bucket lights up and the walk
        // must exhaust the proxy (no early-exit).
        int sparseInvocations = 0;
        bool sparseExhausted = true;
        chain.filter->ForEachMatchingRow(
            start,
            Qt::DisplayRole,
            QVariant::fromValue(QStringLiteral("msg 0")),
            Qt::MatchContains | Qt::MatchWrap,
            /*forward=*/true,
            /*skipFirstN=*/0,
            [&](const QModelIndex &) -> bool {
                ++sparseInvocations;
                // Cap proven + "all buckets hit" can never both be
                // true for a single-row hit set spanning one bucket.
                if (sparseInvocations > SCAN_CAP)
                {
                    sparseExhausted = false;
                    return false;
                }
                return true;
            }
        );
        QVERIFY2(sparseExhausted, "sparse needle must not take the dense early-exit path");
        QCOMPARE(sparseInvocations, 1);
    }

    /// Regression against the "resize-storm rescan" bug. A live
    /// drag-resize of the window fires a resize event per pixel
    /// of rail-height change, and each `SetBucketCount(H±1)` on
    /// the model would invalidate the durable per-bucket
    /// find-match counts and force `MainWindow`'s
    /// `bucketsChanged` -> `PushFindMatchesToOverviewRail`
    /// wiring to run a synchronous full-table find rescan --
    /// seconds of freeze on a large log with the Find bar open.
    ///
    /// The widget debounces `SyncBucketCountToHeight()` through
    /// `mBucketSyncTimer`; a burst of resize events must collapse
    /// to a single `SetBucketCount(finalH)` when the drag settles.
    static void TestResizeStormCoalescesToSingleBucketCountUpdate()
    {
        LogModel model;
        QObject owner;
        const auto chain = BuildProxyChain(&model, &owner);
        OverviewRailModel railModel(chain.filter, &model, /*anchors=*/nullptr, &owner);

        constexpr int ROWS = 40;
        const RailFixture fixture(ROWS);
        StreamJsonPathInto(model, fixture.Path());

        auto *widget = new OverviewRailWidget(&railModel, /*theme=*/nullptr, /*tableView=*/nullptr);
        // Initial size + show fires the synchronous first-sync
        // path in `showEvent`. Subsequent resize events are what
        // this test asserts on -- they must go through the
        // debounce, not straight to `SetBucketCount`.
        widget->resize(60, 100);
        widget->show();
        QCoreApplication::processEvents();

        // Spy starts after the initial show so the baseline is
        // clean; anything the spy sees now is the response to
        // the resize burst below.
        QSignalSpy spy(&railModel, &OverviewRailModel::bucketsChanged);
        QVERIFY(spy.isValid());

        // Simulate a drag-resize storm: many resize events with
        // different heights in rapid succession. Without the
        // debounce, each one would land on the model as a
        // separate `SetBucketCount(H)` and emit `bucketsChanged`.
        constexpr int STORM_START_H = 101;
        constexpr int STORM_END_H = 130;
        for (int h = STORM_START_H; h <= STORM_END_H; ++h)
        {
            widget->resize(60, h);
        }

        // Immediately after the storm the debounce is still
        // armed; no bucket-count update has landed on the model
        // yet. `processEvents` here would let queued Qt events
        // run but must NOT let the timer fire.
        QCoreApplication::processEvents();
        QCOMPARE(spy.count(), 0);

        // Wait for the debounce interval to elapse and the
        // trailing sync to fire. Generous slack over the
        // 100 ms debounce so CI jitter doesn't flake.
        QVERIFY2(spy.wait(1000), "debounced sync must land within a comfortable slack over BUCKET_SYNC_DEBOUNCE_MS");

        // One more event-loop spin to prove no stragglers land
        // after the trailing edge (defends against a future bug
        // where the timer re-arms itself under some condition).
        QCoreApplication::processEvents();
        QCOMPARE(spy.count(), 1);

        // Bucket count reflects the *final* widget height, not
        // any of the intermediate storm values -- pins that the
        // coalesce takes the last resize's height, not an
        // arbitrary one from the middle.
        const QRect finalRail = widget->rect().adjusted(0, 2, 0, -2);
        QCOMPARE(static_cast<int>(railModel.BucketCount()), std::max(0, finalRail.height()));

        delete widget;
    }

    /// `showEvent` bypasses the resize-debounce timer so the
    /// first paint after a show sees correct bucket geometry.
    /// A toggle / tab-switch / initial appearance is a discrete
    /// user action and must feel instant; deferring it to the
    /// debounce would paint a stale (or empty) rail for up to
    /// `BUCKET_SYNC_DEBOUNCE_MS` on every show.
    static void TestShowFlushesPendingBucketSyncSynchronously()
    {
        LogModel model;
        QObject owner;
        const auto chain = BuildProxyChain(&model, &owner);
        OverviewRailModel railModel(chain.filter, &model, /*anchors=*/nullptr, &owner);

        constexpr int ROWS = 20;
        const RailFixture fixture(ROWS);
        StreamJsonPathInto(model, fixture.Path());

        auto *widget = new OverviewRailWidget(&railModel, /*theme=*/nullptr, /*tableView=*/nullptr);
        // Pre-show resize: `SetBucketCount` has NOT run yet
        // because `resizeEvent` before show is queued but the
        // widget's own machinery only exercises it inside the
        // show flow. Either way, this must be a hard 0 before
        // the show fires.
        widget->resize(60, 150);
        QCOMPARE(static_cast<int>(railModel.BucketCount()), 0);

        const QSignalSpy spy(&railModel, &OverviewRailModel::bucketsChanged);
        QVERIFY(spy.isValid());

        widget->show();

        // Contract: bucket count reflects the widget height
        // *before* `show()` returns. No `processEvents`, no
        // timer wait. Paint tests rely on this -- rendering
        // straight after `show()` must not see a zero-bucket
        // model.
        QVERIFY2(
            railModel.BucketCount() > 0,
            "showEvent must apply the current widget height synchronously; the "
            "resize-debounce timer is only for interactive drag-resize storms"
        );
        QCOMPARE(spy.count(), 1);

        delete widget;
    }

    /// `showEvent` cancels any pending resize-debounce so a
    /// resize-then-show sequence doesn't double-fire the sync
    /// (once synchronously in show, once when the debounced
    /// timer expires against the same H). Both would emit
    /// `bucketsChanged`, and the second is a wasted paint
    /// invalidation.
    static void TestShowCancelsPendingResizeDebounce()
    {
        LogModel model;
        QObject owner;
        const auto chain = BuildProxyChain(&model, &owner);
        OverviewRailModel railModel(chain.filter, &model, /*anchors=*/nullptr, &owner);

        constexpr int ROWS = 20;
        const RailFixture fixture(ROWS);
        StreamJsonPathInto(model, fixture.Path());

        auto *widget = new OverviewRailWidget(&railModel, /*theme=*/nullptr, /*tableView=*/nullptr);
        widget->resize(60, 100);
        widget->show();
        QCoreApplication::processEvents();

        // Hide + resize (arms the debounce) + show. If show
        // doesn't cancel the pending debounce, the timer fires
        // ~100 ms later on the same H, wasting a
        // `bucketsChanged` + repaint pass.
        widget->hide();
        widget->resize(60, 140);
        // Confirm the debounce is armed post-resize: no
        // `bucketsChanged` yet.
        const QSignalSpy spy(&railModel, &OverviewRailModel::bucketsChanged);
        QVERIFY(spy.isValid());
        widget->show();
        // Show fires exactly one emit synchronously.
        QCOMPARE(spy.count(), 1);
        // Wait long enough for a stray debounce to sneak in
        // (200 ms > 100 ms debounce) and pin that none does.
        QTest::qWait(200);
        QCOMPARE(spy.count(), 1);

        delete widget;
    }

    /// `wheelEvent` on the rail forwards to the table view's
    /// vertical scrollbar so scrolling over the rail feels like
    /// scrolling past the scrollbar. Regression: the forwarded
    /// event's *local* position must sit inside the scrollbar's
    /// rect, not the rail widget's -- `QAbstractSlider::wheelEvent`
    /// currently reads only `angleDelta`, but a style /
    /// accessibility layer that inspects position must not see
    /// rail-widget coordinates that don't correspond to anything
    /// on the scrollbar. `angleDelta` must be preserved verbatim
    /// so the scrollbar acts on the same scroll the user gave.
    static void TestWheelForwardTranslatesPositionIntoScrollbarCoords()
    {
        LogTableView view;
        view.resize(600, 400);

        LogModel model;
        AnchorManager anchors;
        QObject owner;
        const auto chain = BuildProxyChain(&model, &owner);
        view.setModel(chain.filter);
        view.show();
        QCoreApplication::processEvents();

        // Enough rows to force a scrollable range so the vbar
        // is realised with a positive rect.
        constexpr int ROWS = 400;
        const RailFixture fixture(ROWS);
        StreamJsonPathInto(model, fixture.Path());
        QCoreApplication::processEvents();

        QScrollBar *vbar = view.verticalScrollBar();
        QVERIFY(vbar != nullptr);
        // The scrollbar has to be realised (visible) so its
        // rect is populated; without rows the vbar range is
        // 0 and Qt hides it.
        QVERIFY2(vbar->isVisible(), "vertical scrollbar must be visible for the wheel forward test");
        const QRect vbarRect = vbar->rect();
        QVERIFY(vbarRect.width() > 0 && vbarRect.height() > 0);

        OverviewRailModel railModel(chain.filter, &model, &anchors, &owner);
        auto *widget = new OverviewRailWidget(&railModel, /*theme=*/nullptr, &view);
        widget->resize(30, 300);
        widget->show();
        QCoreApplication::processEvents();

        // Event filter captures wheel events landing on the
        // vbar so the test inspects the forwarded coordinates
        // without pumping the real style through.
        class WheelSpy : public QObject
        {
        public:
            QList<QPointF> localPositions;
            QList<QPoint> angleDeltas;
            bool eventFilter(QObject *watched, QEvent *event) override
            {
                if (event->type() == QEvent::Wheel)
                {
                    // `QEvent::Wheel` guarantees the dynamic
                    // type; Qt doesn't enable RTTI on `QEvent`.
                    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast)
                    auto *w = static_cast<QWheelEvent *>(event);
                    localPositions.append(w->position());
                    angleDeltas.append(w->angleDelta());
                }
                return QObject::eventFilter(watched, event);
            }
        };
        WheelSpy spy;
        vbar->installEventFilter(&spy);

        // Synthesise a wheel event at a point inside the rail
        // widget. In rail-widget coords the point is nowhere
        // near `vbar->rect()`; the wheelEvent path must
        // translate it via `vbar->mapFromGlobal` before
        // forwarding.
        const QPointF railLocal(10.0, 40.0);
        const QPoint globalPos = widget->mapToGlobal(railLocal.toPoint());
        const QPoint angleDelta(0, -120);
        QWheelEvent wheel(
            railLocal,
            QPointF(globalPos),
            QPoint(),
            angleDelta,
            Qt::NoButton,
            Qt::NoModifier,
            Qt::NoScrollPhase,
            /*inverted=*/false
        );
        QApplication::sendEvent(widget, &wheel);

        // Delta preserved verbatim -- the scrollbar sees the
        // same scroll the user gave.
        QCOMPARE(spy.angleDeltas.size(), 1);
        QCOMPARE(spy.angleDeltas.first(), angleDelta);

        // Position now sits inside the scrollbar's coordinate
        // space. The vbar's local rect is anchored at (0, 0),
        // so a translated point should fall inside the rect
        // (or at worst touch its bounds). If the widget were
        // still forwarding rail-widget coords, the point's X
        // would be far past `vbarRect.right()`.
        QCOMPARE(spy.localPositions.size(), 1);
        const QPointF forwarded = spy.localPositions.first();
        QVERIFY2(
            forwarded.x() <= static_cast<qreal>(vbarRect.width()),
            qPrintable(QStringLiteral("forwarded local X %1 must land within vbar width %2; larger value means "
                                      "rail-widget coords leaked through")
                           .arg(forwarded.x())
                           .arg(vbarRect.width()))
        );

        vbar->removeEventFilter(&spy);
        delete widget;
    }

    /// Rail click / scrub must disengage Follow newest. `scrollTo`
    /// is programmatic and would not fire `userScrolledAwayFromTail`
    /// on its own — without an explicit Follow off in
    /// `ScrollToProxyRow`, a live-tail batch would yank the
    /// viewport back to the tail after the user navigated away
    /// via the minimap.
    static void TestScrollToProxyRowDisengagesFollow()
    {
        {
            QSettings settings;
            settings.remove(QStringLiteral("ui/showOverviewRail"));
        }
        MainWindow window;
        window.show();
        QCoreApplication::processEvents();

        constexpr int ROWS = 40;
        const RailFixture fixture(ROWS);
        auto *model = window.Model();
        QVERIFY(model != nullptr);
        StreamJsonPathInto(*model, fixture.Path());

        auto *followAction = window.findChild<QAction *>(QStringLiteral("actionFollowTail"));
        QVERIFY(followAction != nullptr);
        followAction->setEnabled(true);
        followAction->setChecked(true);
        QVERIFY(followAction->isChecked());

        // Scrub path (replaceSelection == false) must also
        // disengage — exploring via drag is still browsing.
        window.ScrollToProxyRow(5, /*replaceSelection=*/false);
        QVERIFY2(
            !followAction->isChecked(),
            "rail scrub must disengage Follow newest so live-tail cannot yank the viewport back"
        );

        followAction->setChecked(true);
        window.ScrollToProxyRow(10, /*replaceSelection=*/true);
        QVERIFY2(
            !followAction->isChecked(),
            "rail click must disengage Follow newest so live-tail cannot yank the viewport back"
        );
    }

    /// Closing find clears rail ticks but keeps `mFindMatchCache`
    /// so a later reveal can restore without re-scanning.
    /// Toggling the overview rail off→on must NOT re-apply that
    /// cache while find is still closed — otherwise stale search
    /// ticks reappear with no active find bar.
    static void TestRailToggleDoesNotRestoreTicksWhenFindClosed()
    {
        {
            QSettings settings;
            settings.remove(QStringLiteral("ui/showOverviewRail"));
        }
        MainWindow window;
        window.show();
        QCoreApplication::processEvents();

        constexpr int ROWS = 40;
        const RailFixture fixture(ROWS);
        auto *model = window.Model();
        QVERIFY(model != nullptr);
        StreamJsonPathInto(*model, fixture.Path());

        auto *railModel = window.findChild<OverviewRailModel *>();
        QVERIFY(railModel != nullptr);
        QVERIFY(railModel->BucketCount() > 0);

        auto *findDock = window.findChild<FindDock *>();
        QVERIFY(findDock != nullptr);
        findDock->show();
        QCoreApplication::processEvents();
        QVERIFY(findDock->isVisible());

        QVERIFY(QMetaObject::invokeMethod(
            &window,
            "UpdateFindMatchCount",
            Qt::DirectConnection,
            Q_ARG(QString, QStringLiteral("msg")),
            Q_ARG(bool, false),
            Q_ARG(bool, false)
        ));
        QVERIFY(railModel->HasMatchTicks());

        // Genuine dismissal: ticks clear, cache survives.
        emit findDock->closed();
        findDock->hide();
        QCoreApplication::processEvents();
        QVERIFY2(!railModel->HasMatchTicks(), "closing find must clear rail match ticks");

        // Rail off→on with find still closed must leave ticks empty.
        window.SetOverviewRailVisible(false);
        QCoreApplication::processEvents();
        window.SetOverviewRailVisible(true);
        QCoreApplication::processEvents();
        // showEvent may re-arm buckets asynchronously relative to
        // attach; flush any pending bucket sync from the widget.
        auto *railWidget = window.findChild<OverviewRailWidget *>();
        QVERIFY(railWidget != nullptr);
        railWidget->FlushPendingBucketSyncForTest();
        QCoreApplication::processEvents();

        QVERIFY2(
            !railModel->HasMatchTicks(),
            "toggling overview rail on after find was closed must not resurrect cached match ticks"
        );
        QVERIFY2(railModel->BucketCount() > 0, "rail toggle-on must re-arm a non-zero bucket vector");
    }

    /// Tabbing the find dock away (visibilityChanged(false)
    /// without `closed`) must clear rail match ticks, matching
    /// the "*i* of *N*" indicator which is only shown while the
    /// bar is visible. `closed` alone is not enough: tabified
    /// bottom docks fire visibilityChanged on tab switch but
    /// leave the dock "open" from the menu-checkmark's point
    /// of view.
    static void TestFindTabHideClearsMatchTicks()
    {
        {
            QSettings settings;
            settings.remove(QStringLiteral("ui/showOverviewRail"));
        }
        MainWindow window;
        window.show();
        QCoreApplication::processEvents();

        constexpr int ROWS = 40;
        const RailFixture fixture(ROWS);
        auto *model = window.Model();
        QVERIFY(model != nullptr);
        StreamJsonPathInto(*model, fixture.Path());

        auto *railModel = window.findChild<OverviewRailModel *>();
        QVERIFY(railModel != nullptr);

        auto *findDock = window.findChild<FindDock *>();
        QVERIFY(findDock != nullptr);
        findDock->show();
        QCoreApplication::processEvents();
        QVERIFY(findDock->isVisible());

        QVERIFY(QMetaObject::invokeMethod(
            &window,
            "UpdateFindMatchCount",
            Qt::DirectConnection,
            Q_ARG(QString, QStringLiteral("msg")),
            Q_ARG(bool, false),
            Q_ARG(bool, false)
        ));
        QVERIFY(railModel->HasMatchTicks());

        // Tab-hide path: `hide()` fires visibilityChanged(false)
        // but does NOT emit FindDock::closed (that is reserved
        // for genuine dismissal). Match ticks must still clear.
        findDock->hide();
        QCoreApplication::processEvents();
        QVERIFY2(
            !railModel->HasMatchTicks(),
            "tab-hiding the find dock must clear rail match ticks even without FindDock::closed"
        );

        // Production `FindDock::showEvent` emits `revealed` only
        // after the dock is visible again; push is gated on
        // `IsFindBarVisible()`, so restore requires a real show.
        findDock->show();
        QCoreApplication::processEvents();
        emit findDock->revealed();
        QVERIFY2(railModel->HasMatchTicks(), "revealed find must restore rail match ticks after a tab hide");
    }

    /// Wheel over the rail must attribute the scrollbar change
    /// as user-initiated so Follow newest disengages. The rail
    /// forwards via `sendEvent` to the scrollbar and bypasses
    /// `LogTableView::wheelEvent`; without
    /// `AttributeNextScrollToUser` (or a reliable
    /// `actionTriggered` path) Follow would stay engaged.
    static void TestRailWheelDisengagesFollow()
    {
        {
            QSettings settings;
            settings.remove(QStringLiteral("ui/showOverviewRail"));
        }
        MainWindow window;
        window.show();
        QCoreApplication::processEvents();

        constexpr int ROWS = 200;
        const RailFixture fixture(ROWS);
        auto *model = window.Model();
        QVERIFY(model != nullptr);
        StreamJsonPathInto(*model, fixture.Path());
        QCoreApplication::processEvents();

        auto *view = window.findChild<LogTableView *>();
        QVERIFY(view != nullptr);
        auto *railWidget = window.findChild<OverviewRailWidget *>();
        QVERIFY(railWidget != nullptr);
        QVERIFY(railWidget->isVisible());

        auto *followAction = window.findChild<QAction *>(QStringLiteral("actionFollowTail"));
        QVERIFY(followAction != nullptr);
        followAction->setEnabled(true);
        followAction->setChecked(true);

        QScrollBar *vbar = view->verticalScrollBar();
        QVERIFY(vbar != nullptr);
        QVERIFY(vbar->maximum() > 0);
        vbar->setValue(vbar->maximum());
        view->SetTailEdge(LogTableView::TailEdge::Bottom);
        QVERIFY(followAction->isChecked());

        const QSignalSpy awaySpy(view, &LogTableView::userScrolledAwayFromTail);
        QVERIFY(awaySpy.isValid());

        // Wheel up on the rail (negative angleDelta = scroll
        // content up / scrollbar value down from the bottom
        // tail). Must leave the tail edge and disengage Follow.
        const QPointF railLocal(10.0, 40.0);
        const QPoint globalPos = railWidget->mapToGlobal(railLocal.toPoint());
        QWheelEvent wheel(
            railLocal,
            QPointF(globalPos),
            QPoint(),
            QPoint(0, 120), // angleDelta: scroll toward older rows
            Qt::NoButton,
            Qt::NoModifier,
            Qt::NoScrollPhase,
            /*inverted=*/false
        );
        QApplication::sendEvent(railWidget, &wheel);
        QCoreApplication::processEvents();

        QVERIFY2(awaySpy.count() >= 1, "rail wheel away from tail must emit userScrolledAwayFromTail");
        QVERIFY2(!followAction->isChecked(), "rail wheel away from tail must disengage Follow newest");
    }
};

QTEST_MAIN(OverviewRailTest)
#include "test_overview_rail.moc"
