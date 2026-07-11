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
#include "log_filter_model.hpp"
#include "log_model.hpp"
#include "log_table_view.hpp"
#include "main_window.hpp"
#include "overview_rail_model.hpp"
#include "overview_rail_widget.hpp"
#include "qt_streaming_log_sink.hpp"
#include "row_order_proxy_model.hpp"

#include <loglib/file_line_source.hpp>
#include <loglib/internal/advanced_parser_options.hpp>
#include <loglib/log_file.hpp>
#include <loglib/log_level.hpp>
#include <loglib/log_parse_sink.hpp>
#include <loglib/parser_options.hpp>
#include <loglib/parsers/json_parser.hpp>
#include <loglib/stop_token.hpp>
#include <loglib/theme.hpp>

#include <QAbstractItemModel>
#include <QAction>
#include <QApplication>
#include <QModelIndex>
#include <QSettings>
#include <QSignalSpy>
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
    return {rowOrder, filter};
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
            MainWindow::InitializeTimezoneDatabase(),
            "Failed to initialise timezone database; see qCritical above."
        );
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
        QSignalSpy matchSpy(&rail, &OverviewRailModel::matchesChanged);
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
        QSignalSpy anchorSpy(&rail, &OverviewRailModel::anchorBucketsChanged);
        anchors.SetAnchor(*keyOpt, 3);
        // Anchor mutation triggers a full rebuild in the rail;
        // the signal is coalesced with buckets rebuilds via a
        // direct `emit`, so no wait is needed after the mutation.
        // Give Qt a tick to route the queued anchor emit.
        QCoreApplication::processEvents();
        QVERIFY(rail.HasAnchorTicks());
        QVERIFY(anchorSpy.count() >= 1);

        anchors.ClearAll();
        QCoreApplication::processEvents();
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

    /// `sizeHint()` returns a positive DPI-fluent width and a
    /// zero preferred height (rail grows vertically to fit the
    /// hosting margin). `minimumSizeHint` never exceeds it.
    static void TestWidgetSizeHintIsDpiFluent()
    {
        LogModel model;
        AnchorManager anchors;
        QObject owner;
        const auto chain = BuildProxyChain(&model, &owner);
        OverviewRailModel rail(chain.filter, &model, &anchors, &owner);
        OverviewRailWidget widget(&rail, /*theme=*/nullptr, /*tableView=*/nullptr);
        // Ensure the widget has a valid style / font (offscreen QPA
        // still routes both). The DPI-fluent width should be > 0
        // and >= the minimum floor.
        widget.ensurePolished();
        const int width = widget.sizeHint().width();
        QVERIFY2(width > 0, "size hint width must be positive after ensurePolished");
        const int minWidth = widget.minimumSizeHint().width();
        QVERIFY(minWidth > 0);
        QVERIFY(width >= minWidth);
        // sizeHint() returns 0 height so the parent layout drives
        // vertical extent.
        QCOMPARE(widget.sizeHint().height(), 0);
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
        // which would leave `ReservedRightMargin` at zero. The
        // `OverviewRailWidget` in production supplies its own
        // DPI-fluent width; test just needs any positive value.
        constexpr int RAIL_WIDTH = 16;
        auto *rail = new QWidget(nullptr);
        rail->setFixedWidth(RAIL_WIDTH);
        view.AttachOverviewRail(rail);
        QCOMPARE(view.OverviewRail(), rail);
        QCOMPARE(view.ReservedRightMargin(), RAIL_WIDTH);
        QCOMPARE(rail->parent(), &view);

        view.AttachOverviewRail(nullptr);
        QCOMPARE(view.OverviewRail(), static_cast<QWidget *>(nullptr));
        QCOMPARE(view.ReservedRightMargin(), 0);
        // Widget survives detach (caller owns it). Delete manually.
        delete rail;
    }

    /// The MainWindow overview-rail toggle exists as a checkable
    /// action, defaults to on (per the ROADMAP), and toggling
    /// off attaches / detaches the widget from the table view.
    static void TestMainWindowOverviewRailToggle()
    {
        // Isolate the QSettings key so a prior run doesn't seed
        // the pref off. A scoped org / app name keeps writes in a
        // throw-away registry hive.
        QCoreApplication::setOrganizationName(QStringLiteral("StructuredLogViewerTest"));
        QCoreApplication::setApplicationName(QStringLiteral("OverviewRailToggle"));
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

    /// The Ctrl+Shift+O shortcut fires the toggle. Confirms the
    /// shortcut is registered on the window so the discovery path
    /// (Shortcuts dialog + toolbar tooltip) matches expectations.
    static void TestMainWindowOverviewRailShortcut()
    {
        MainWindow window;
        auto *action = window.findChild<QAction *>(QStringLiteral("actionToggleOverviewRail"));
        QVERIFY(action != nullptr);
        const QKeySequence expected(Qt::CTRL | Qt::SHIFT | Qt::Key_O);
        QCOMPARE(action->shortcut(), expected);
    }
};

QTEST_MAIN(OverviewRailTest)
#include "test_overview_rail.moc"
