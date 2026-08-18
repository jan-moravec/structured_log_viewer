// Tab-switch activation benchmark for the production MainWindow path.
// Fixture: two 100,000-row JSONL tabs, 1,000 anchors each, visible shared
// docks, 10 warm-up switches and 50 measured activations.
//
// Gate (see CONTRIBUTING.md Acceptance bar): p95 ≤ 100 ms. Stay within
// 20 % of the controlled-CI baseline once that number is recorded in
// the PR. Prints hardware class, row counts, dock visibility, and
// p50/p95 on every run. Do not disable this test if a machine misses
// 100 ms; record the hardware class from the WARN line instead.

#include "anchor_manager.hpp"
#include "anchors_dock.hpp"
#include "find_dock.hpp"
#include "histogram_dock.hpp"
#include "histogram_model.hpp"
#include "log_model.hpp"
#include "log_session.hpp"
#include "main_window.hpp"
#include "parse_errors_dock.hpp"
#include "qstring_path.hpp"
#include "record_detail_dock.hpp"

#include <test_common/log_format.hpp>
#include <test_common/log_generator.hpp>

#include <QApplication>
#include <QDockWidget>
#include <QObject>
#include <QSignalSpy>
#include <QString>
#include <QSysInfo>
#include <QTemporaryDir>
#include <QThread>
#include <QtTest/QtTest>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <ranges>
#include <span>
#include <vector>

namespace
{

constexpr int kRowCount = 100'000;
constexpr int kAnchorCount = 1'000;
constexpr int kWarmupSwitches = 10;
constexpr int kMeasuredSwitches = 50;
constexpr double kP95CeilingMs = 100.0;
constexpr std::uint32_t kFixtureSeed = 0xC0FFEEu;
// 2026-01-01T00:00:00Z. Matches the lib benchmark timestamp pin so the
// generated JSONL has a stable time column for histogram rebuilds.
constexpr std::chrono::seconds kBenchmarkBaseEpoch{1'767'225'600};

[[nodiscard]] test_common::TimestampPolicy DeterministicTimestamps()
{
    return {
        .baseTime = std::chrono::system_clock::time_point{kBenchmarkBaseEpoch},
        .interval = std::chrono::milliseconds{1},
    };
}

void WriteJsonlFixture(const QString &path, int rowCount)
{
    const auto records = test_common::GenerateRandomLogRecords(
        static_cast<std::size_t>(rowCount), kFixtureSeed, DeterministicTimestamps()
    );
    const test_common::LogFormat format = test_common::JsonLines();
    std::ofstream stream(logapp::QStringToFsPath(path), std::ios::binary);
    QVERIFY2(stream.is_open(), "fixture must open for writing");
    for (const auto &record : records)
    {
        stream << format.writeLine(record) << '\n';
    }
    stream.flush();
    QVERIFY2(stream.good(), "fixture must write completely");
}

void LoadFileIntoActiveTab(MainWindow &window, const QString &path)
{
    LogModel *model = window.activeSession() != nullptr ? window.activeSession()->Model() : nullptr;
    QVERIFY(model != nullptr);
    const QSignalSpy spy(model, &LogModel::streamingFinished);
    window.OpenFilesForTest({path}, MainWindow::OpenMode::Replace);
    QTRY_VERIFY_WITH_TIMEOUT(spy.count() >= 1, 180'000);
    QCOMPARE(model->rowCount(), kRowCount);
}

void SeedAnchors(LogSession *session, int count)
{
    QVERIFY(session != nullptr && session->Model() != nullptr && session->Anchors() != nullptr);
    LogModel *model = session->Model();
    const int rows = model->rowCount();
    QVERIFY(rows >= count);
    std::vector<AnchorManager::Key> keys;
    keys.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i)
    {
        const auto key = model->AnchorKeyForRow(i);
        QVERIFY2(key.has_value(), "each seeded row must resolve an anchor key");
        keys.push_back(*key);
    }
    QVERIFY(session->Anchors()->SetAnchors(std::span<const AnchorManager::Key>(keys), 1));
}

template <typename Dock> Dock *ShowDock(MainWindow &window)
{
    auto *dock = window.findChild<Dock *>();
    if (dock != nullptr)
    {
        dock->show();
        dock->raise();
    }
    return dock;
}

[[nodiscard]] QString HardwareClass()
{
    return QStringLiteral("%1 / %2 / %3 threads")
        .arg(QSysInfo::prettyProductName(), QSysInfo::currentCpuArchitecture())
        .arg(QThread::idealThreadCount());
}

void WarnLine(const QString &line)
{
    qWarning().noquote() << line;
    std::fprintf(stderr, "%s\n", qPrintable(line));
    std::fflush(stderr);
}

[[nodiscard]] double PercentileMs(std::vector<double> samples, double percentile)
{
    std::ranges::sort(samples);
    const double index = percentile * static_cast<double>(samples.size() - 1);
    const auto lo = static_cast<std::size_t>(index);
    const auto hi = std::min(lo + 1, samples.size() - 1);
    const double frac = index - static_cast<double>(lo);
    return (samples[lo] * (1.0 - frac)) + (samples[hi] * frac);
}

} // namespace

// NOLINTNEXTLINE(misc-use-internal-linkage): `Q_OBJECT` QtTest fixture.
class SessionTabsBench : public QObject
{
    Q_OBJECT

private slots:
    static void initTestCase()
    {
        QVERIFY2(MainWindow::InitializeTimezoneDatabase(), "tzdata must be available next to the test binary");
    }

    static void BenchTabSwitchActivation()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const QString path = temp.filePath(QStringLiteral("tabs.jsonl"));
        WriteJsonlFixture(path, kRowCount);

        MainWindow window;
        window.SetSuppressDialogsForTest(true);
        LoadFileIntoActiveTab(window, path);
        window.AddNewTabForTest(/*makeActive=*/true);
        LoadFileIntoActiveTab(window, path);

        LogSession *sessionA = window.SessionAtTab(0);
        LogSession *sessionB = window.SessionAtTab(1);
        QVERIFY(sessionA != nullptr && sessionB != nullptr);
        SeedAnchors(sessionA, kAnchorCount);
        SeedAnchors(sessionB, kAnchorCount);

        window.show();
        window.resize(1600, 900);
        auto *histogram = ShowDock<HistogramDock>(window);
        auto *findDock = ShowDock<FindDock>(window);
        auto *anchors = ShowDock<AnchorsDock>(window);
        auto *parseErrors = ShowDock<ParseErrorsDock>(window);
        auto *recordDetail = ShowDock<RecordDetailDock>(window);
        QVERIFY(
            histogram != nullptr && findDock != nullptr && anchors != nullptr && parseErrors != nullptr &&
            recordDetail != nullptr
        );
        QTRY_VERIFY(histogram->isVisible());
        QTRY_VERIFY(findDock->isVisible());
        QTRY_VERIFY(anchors->isVisible());
        QTRY_VERIFY(parseErrors->isVisible());
        QTRY_VERIFY(recordDetail->isVisible());
        QVERIFY2(
            histogram->ModelForTest()->HasTimeColumn(),
            "fixture must promote a time column so histogram rebuilds on switch"
        );
        QTRY_VERIFY(!histogram->ModelForTest()->IsDeferredBindPending());
        QVERIFY(!histogram->ModelForTest()->Index().Empty());

        const QString hardware = HardwareClass();
        WarnLine(QStringLiteral("[session_tabs] hardware: %1").arg(hardware));
        WarnLine(QStringLiteral("[session_tabs] rows: tab0=%1 tab1=%2 anchors=%3")
                     .arg(sessionA->Model()->rowCount())
                     .arg(sessionB->Model()->rowCount())
                     .arg(kAnchorCount));
        const auto yesNo = [](bool visible) { return visible ? QStringLiteral("yes") : QStringLiteral("no"); };
        WarnLine(QStringLiteral(
                     "[session_tabs] docks visible: histogram=%1 find=%2 anchors=%3 parseErrors=%4 recordDetail=%5"
        )
                     .arg(
                         yesNo(histogram->isVisible()),
                         yesNo(findDock->isVisible()),
                         yesNo(anchors->isVisible()),
                         yesNo(parseErrors->isVisible()),
                         yesNo(recordDetail->isVisible())
                     ));

        int next = 0;
        for (int i = 0; i < kWarmupSwitches; ++i)
        {
            window.ActivateTabForTest(next);
            next = 1 - next;
        }

        std::vector<double> samples;
        samples.reserve(static_cast<std::size_t>(kMeasuredSwitches));
        for (int i = 0; i < kMeasuredSwitches; ++i)
        {
            const auto t0 = std::chrono::steady_clock::now();
            window.ActivateTabForTest(next);
            const auto elapsed = std::chrono::steady_clock::now() - t0;
            samples.push_back(std::chrono::duration<double, std::milli>(elapsed).count());
            next = 1 - next;
        }

        const double p50 = PercentileMs(samples, 0.50);
        const double p95 = PercentileMs(samples, 0.95);
        WarnLine(QStringLiteral("[session_tabs] activation p50=%1 ms p95=%2 ms (n=%3)")
                     .arg(p50, 0, 'f', 3)
                     .arg(p95, 0, 'f', 3)
                     .arg(kMeasuredSwitches));

        QVERIFY2(
            p95 <= kP95CeilingMs,
            qPrintable(QStringLiteral("tab-switch p95 %1 ms exceeds %2 ms on %3")
                           .arg(p95, 0, 'f', 3)
                           .arg(kP95CeilingMs, 0, 'f', 0)
                           .arg(hardware))
        );
    }
};

QTEST_MAIN(SessionTabsBench)
#include "benchmark_session_tabs.moc"
