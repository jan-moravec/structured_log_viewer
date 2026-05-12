// Single-shot end-to-end timing benchmark for the production Qt path:
// `LogModel -> RowOrderProxyModel -> LogFilterModel`, parser running on
// a `QtConcurrent::run` worker, GUI-thread drain via `QSignalSpy::wait`.
// Three runs (one per row of the matrix below) print throughput in the
// same shape as the lib's `[large]` Catch2 benchmark so the two corpora
// are directly comparable.
//
// Matrix:
//   1. newest-first OFF (default)
//   2. newest-first ON, session in stream mode
//   3. newest-first ON, session in static mode
//
// Stream-mode and static-mode share the proxy code, but staging both
// ensures the per-mode wiring continues to drive the proxy correctly
// once the `StreamingControl::Static*` accessors land in Phase 2.

#include "log_filter_model.hpp"
#include "log_model.hpp"
#include "qt_streaming_log_sink.hpp"
#include "row_order_proxy_model.hpp"

#include <loglib/enum_dictionary.hpp>
#include <loglib/file_line_source.hpp>
#include <loglib/key_index.hpp>
#include <loglib/log_compare.hpp>
#include <loglib/log_configuration.hpp>
#include <loglib/log_file.hpp>
#include <loglib/log_filter.hpp>
#include <loglib/log_processing.hpp>
#include <loglib/log_table.hpp>
#include <loglib/parser_options.hpp>
#include <loglib/parsers/json_parser.hpp>
#include <loglib/stop_token.hpp>

#include <test_common/json_log_line.hpp>
#include <test_common/log_generator.hpp>

#include <QObject>
#include <QSignalSpy>
#include <QString>
#include <QTemporaryDir>
#include <QtTest/QtTest>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <memory>
#include <numeric>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{

// Walk the CWD ancestor chain for a `tzdata/` sibling and hand it to
// `loglib::Initialize`. Mirrors the search the lib's `InitializeTimezoneData`
// helper performs, minus the Catch2 dependency.
void StageTimezoneData()
{
    static const auto TZ_DATA = std::filesystem::path("tzdata");
    std::filesystem::path path = std::filesystem::current_path();
    while (true)
    {
        const auto tzdataPath = path / TZ_DATA;
        std::error_code ec;
        if (std::filesystem::exists(tzdataPath, ec))
        {
            loglib::Initialize(tzdataPath);
            return;
        }
        const auto parent = path.parent_path();
        if (parent.empty() || parent == path)
        {
            break;
        }
        path = parent;
    }
    qWarning() << "tzdata/ not found in CWD ancestors; timestamp promotion disabled "
                  "for this run. Invoke from the directory holding the staged tzdata "
                  "folder (typically build/<preset>/bin/<config>/).";
}

// Generate `count` random JSON log lines and write them as JSONL into
// `path`. Returns the byte count of the written file.
std::size_t WriteJsonlFixture(const std::filesystem::path &path, std::size_t count)
{
    auto logs = test_common::GenerateRandomJsonLogs(count);
    std::ofstream stream(path, std::ios::binary);
    for (const auto &line : logs)
    {
        stream << line.ToString() << '\n';
    }
    stream.flush();
    return std::filesystem::file_size(path);
}

struct RunResult
{
    std::chrono::steady_clock::duration elapsed{};
    int rowCount = 0;
    bool finished = false;
};

// One end-to-end run through the full Qt pipeline at `reversed`. The
// proxy chain is local to the call so each run starts from a clean
// slate (no left-over rows / cached mappings).
RunResult RunOnce(const std::filesystem::path &logPath, bool reversed)
{
    auto model = std::make_unique<LogModel>();
    auto rowProxy = std::make_unique<RowOrderProxyModel>();
    rowProxy->setSourceModel(model.get());
    rowProxy->SetReversed(reversed);
    auto filterProxy = std::make_unique<LogFilterModel>();
    filterProxy->setSourceModel(rowProxy.get());
    filterProxy->SetLogModel(model.get());

    QSignalSpy finishedSpy(model.get(), &LogModel::streamingFinished);

    auto file = std::make_unique<loglib::LogFile>(logPath.string());
    auto fileSource = std::make_unique<loglib::FileLineSource>(std::move(file));
    loglib::FileLineSource *fileSourcePtr = fileSource.get();

    const auto t0 = std::chrono::steady_clock::now();
    model->BeginStreaming(std::move(fileSource), [fileSourcePtr, sink = model->Sink()](loglib::StopToken token) {
        loglib::ParserOptions options;
        options.stopToken = std::move(token);
        const loglib::JsonParser parser;
        parser.ParseStreaming(*fileSourcePtr, *sink, options);
    });

    RunResult result;
    result.finished = finishedSpy.wait(180'000);
    result.elapsed = std::chrono::steady_clock::now() - t0;
    result.rowCount = model->rowCount();
    return result;
}

// Format the per-run line so it matches the lib's `ReportThroughput`
// output: `<label>: <MiB/s> MB/s, <lines/s> lines/s (<bytes> bytes /
// <lines> lines / <seconds>s)`. Note: the lib labels MiB/s as "MB/s" for
// historical reasons; we mirror that for grep-ability across bench corpora.
QString FormatThroughput(const QString &label, std::chrono::nanoseconds elapsed, std::size_t bytes, std::size_t lines)
{
    const double seconds = std::chrono::duration<double>(elapsed).count();
    if (seconds <= 0.0)
    {
        return label + QStringLiteral(": (elapsed=0)");
    }
    constexpr double MIB = 1024.0 * 1024.0;
    const double mibps = static_cast<double>(bytes) / MIB / seconds;
    const double linesPerSec = static_cast<double>(lines) / seconds;
    return QStringLiteral("%1: %2 MB/s, %3 lines/s (%4 bytes / %5 lines / %6s)")
        .arg(label)
        .arg(mibps, 0, 'g', 6)
        .arg(linesPerSec, 0, 'g', 6)
        .arg(bytes)
        .arg(lines)
        .arg(seconds, 0, 'g', 5);
}

} // namespace

// NOLINTNEXTLINE(misc-use-internal-linkage): `Q_OBJECT` test fixture; moc + QTest registration expect this shape.
class MainWindowBench : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        StageTimezoneData();

        // Single shared 1'000'000-line fixture for all three runs.
        // 1 M / ~170 MiB matches the lib's `[large]` benchmark so the
        // numbers here are directly comparable. Pinned to the test's
        // own QTemporaryDir so it survives `runConfig` but is cleaned
        // up at suite teardown.
        QVERIFY(mTempDir.isValid());
        mLogPath = std::filesystem::path(mTempDir.path().toStdString()) / "bench.jsonl";
        // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
        mBytes = WriteJsonlFixture(mLogPath, LINE_COUNT);
        QVERIFY(mBytes > 0);
    }

    void BenchNewestFirstOff()
    {
        RunConfig(QStringLiteral("Qt path, newest-first OFF (default)"), false);
    }

    void BenchNewestFirstStreamMode()
    {
        RunConfig(QStringLiteral("Qt path, newest-first ON (stream mode)"), true);
    }

    void BenchNewestFirstStaticMode()
    {
        RunConfig(QStringLiteral("Qt path, newest-first ON (static mode)"), true);
    }

    // Enum filter wall-clock under the production proxy chain. Mirrors
    // the lib-level `[log_filter][large]` benchmark but lands the rows
    // through the full `LogModel -> RowOrderProxyModel -> LogFilterModel`
    // path so any regression in the proxy's `filterAcceptsRow` shows up
    // here even if the lib path stays fast.
    void BenchEnumFilterApply()
    {
        const ProxyChain chain = BuildLoadedChain();
        const auto &columns = chain.model->Configuration().columns;
        int levelCol = -1;
        for (size_t i = 0; i < columns.size(); ++i)
        {
            if (std::find(columns[i].keys.begin(), columns[i].keys.end(), std::string("level")) !=
                columns[i].keys.end())
            {
                levelCol = static_cast<int>(i);
                break;
            }
        }
        QVERIFY2(levelCol >= 0, "fixture must produce a `level` column");
        QCOMPARE(columns[static_cast<size_t>(levelCol)].type, loglib::LogConfiguration::Type::Enumeration);

        const loglib::EnumDictionary *dict = nullptr;
        const loglib::KeyId keyId =
            chain.model->Table().Keys().Find(columns[static_cast<size_t>(levelCol)].keys.front());
        QVERIFY(keyId != loglib::INVALID_KEY_ID);
        dict = chain.model->Table().EnumDictionaries().Find(keyId);
        QVERIFY(dict != nullptr);

        const std::vector<std::string> selectedHolders = {"info", "warning"};
        std::vector<std::string_view> selectedViews;
        selectedViews.reserve(selectedHolders.size());
        for (const auto &v : selectedHolders)
        {
            selectedViews.emplace_back(v);
        }
        std::vector<loglib::RowPredicate> rules;
        rules.emplace_back(
            std::in_place_type<loglib::EnumRowPredicate>,
            static_cast<size_t>(levelCol),
            std::span<const std::string_view>(selectedViews),
            dict
        );

        // Two breakdown measurements:
        //   * predicate-only: walk every row in the same `LogTable`
        //     through `EnumRowPredicate::MatchesRow`. Mirrors the
        //     lib-level `[log_filter][large]` numbers and proves the
        //     non-Qt cost stays small.
        //   * proxy-roundtrip: also include `QSortFilterProxyModel`'s
        //     `invalidateFilter` + lazy row-map rebuild on `rowCount()`.
        //     The proxy's internal bookkeeping with ~500 k survivors
        //     dominates this number; the lib-level benchmark is the
        //     canonical regression gate.
        const loglib::LogTable &table = chain.model->Table();
        const auto &predicate = rules.front();
        size_t predicateHits = 0;
        const auto predicateStart = std::chrono::steady_clock::now();
        for (size_t row = 0, n = static_cast<size_t>(table.RowCount()); row < n; ++row)
        {
            if (loglib::MatchesRow(predicate, table, row))
            {
                ++predicateHits;
            }
        }
        const auto predicateElapsed = std::chrono::steady_clock::now() - predicateStart;

        const auto t0 = std::chrono::steady_clock::now();
        chain.filterProxy->SetFilterRules(std::move(rules));
        // Force the proxy to materialise its row map (the invalidation
        // is lazy; `rowCount()` triggers the walk).
        const int filteredRows = chain.filterProxy->rowCount();
        const auto elapsed = std::chrono::steady_clock::now() - t0;

        using Ms = std::chrono::duration<double, std::milli>;
        qDebug().noquote() << QStringLiteral("EnumRowPredicate over %1 rows: %2 ms (%3 hits)")
                                  .arg(static_cast<std::size_t>(table.RowCount()))
                                  .arg(Ms(predicateElapsed).count(), 0, 'f', 2)
                                  .arg(predicateHits);
        qDebug().noquote(
        ) << QStringLiteral("Apply enum filter (Qt proxy roundtrip) over %1 rows: %2 ms (%3 rows survived)")
                 .arg(static_cast<std::size_t>(chain.model->rowCount()))
                 .arg(Ms(elapsed).count(), 0, 'f', 2)
                 .arg(filteredRows);

        QVERIFY2(filteredRows > 0 && filteredRows < chain.model->rowCount(), "filter selectivity sanity check");
        QCOMPARE(static_cast<int>(predicateHits), filteredRows);
        // Predicate-only path is the canonical gate: previously the
        // same walk via `data(SortRole) -> QVariant<QString>` chain
        // took ~10x longer per row and the GUI freeze was many seconds.
        QVERIFY2(
            Ms(predicateElapsed).count() < 200.0,
            qPrintable(QStringLiteral("predicate walk regressed: %1 ms").arg(Ms(predicateElapsed).count()))
        );
        // Proxy-roundtrip gate: now that the filter pass hands the
        // per-row predicate loop to `loglib::FilterAcceptedRows`
        // (TBB-parallel) and the log->source remap is one cached
        // `mapFromSource` hop per survivor, the full filter cycle on
        // 1 M rows lands well under 100 ms locally. 500 ms is a
        // generous CI ceiling -- breaking it means either the lib's
        // parallel pass collapsed to sequential or the proxy went
        // back to per-row chain walks.
        QVERIFY2(
            Ms(elapsed).count() < 500.0,
            qPrintable(QStringLiteral("filter proxy roundtrip regressed: %1 ms").arg(Ms(elapsed).count()))
        );
    }

    // Enum-column sort wall-clock under the production proxy chain.
    void BenchEnumColumnSort()
    {
        const ProxyChain chain = BuildLoadedChain();
        const auto &columns = chain.model->Configuration().columns;
        int levelCol = -1;
        for (size_t i = 0; i < columns.size(); ++i)
        {
            if (std::find(columns[i].keys.begin(), columns[i].keys.end(), std::string("level")) !=
                columns[i].keys.end())
            {
                levelCol = static_cast<int>(i);
                break;
            }
        }
        QVERIFY2(levelCol >= 0, "fixture must produce a `level` column");
        QCOMPARE(columns[static_cast<size_t>(levelCol)].type, loglib::LogConfiguration::Type::Enumeration);

        using Ms = std::chrono::duration<double, std::milli>;

        // Lib-level gate: time a pure `loglib::CompareRows`-driven
        // `std::ranges::sort` over an `indices` vector so a regression
        // in the lib's compare path can't hide behind Qt proxy overhead.
        // Pattern mirrors `test/lib/src/benchmark_log_filter.cpp`'s
        // `[log_filter][log_compare][large]` case, which ships with a
        // 1500 ms ceiling. We give ourselves an extra ~30% margin here
        // because the app-side fixture (`GenerateRandomJsonLogs` --
        // `timestamp/level/message/component`) is wider than the lean
        // `level`-only lib fixture, so per-row `FindCompact` does a
        // touch more work per compare. On the GitHub Actions Linux
        // runner the lib case measures ~750 ms and this case ~880 ms;
        // a fast Apple Silicon laptop sees ~60 ms / ~80 ms respectively.
        {
            const loglib::LogTable &table = chain.model->Table();
            const size_t rowCount = table.RowCount();
            QCOMPARE(static_cast<std::size_t>(rowCount), LINE_COUNT);
            std::vector<size_t> indices(rowCount);
            // `std::iota` rather than `std::ranges::iota`: the latter is
            // C++23 and AppleClang 17's libc++ still lacks it.
            // NOLINTNEXTLINE(modernize-use-ranges): `std::ranges::iota` is C++23 and unavailable on AppleClang 17
            std::iota(indices.begin(), indices.end(), size_t{0});

            const loglib::KeyId levelKey = table.Keys().Find(columns[static_cast<size_t>(levelCol)].keys.front());
            QVERIFY2(levelKey != loglib::INVALID_KEY_ID, "level key must resolve in the table");
            const loglib::EnumDictionary *dictionary = table.EnumDictionaries().Find(levelKey);
            QVERIFY2(dictionary != nullptr, "level column must have a dictionary");
            const loglib::EnumDictRank rank{*dictionary};

            const auto libT0 = std::chrono::steady_clock::now();
            std::ranges::sort(indices, [&](size_t a, size_t b) {
                return loglib::CompareRows(table, a, b, static_cast<size_t>(levelCol), &rank) < 0;
            });
            const auto libElapsed = std::chrono::steady_clock::now() - libT0;

            qDebug().noquote() << QStringLiteral("Lib-only sort by enum column over %1 rows: %2 ms")
                                      .arg(static_cast<std::size_t>(indices.size()))
                                      .arg(Ms(libElapsed).count(), 0, 'f', 2);
            QVERIFY2(
                Ms(libElapsed).count() < 2000.0,
                qPrintable(QStringLiteral("lib-only CompareRows enum sort regressed: %1 ms").arg(Ms(libElapsed).count())
                )
            );
        }

        // Drop any cached ranks so the very first `lessThan` triggers a
        // rebuild and the time below covers the cold-cache cost too.
        chain.filterProxy->InvalidateEnumRanks();
        chain.filterProxy->sort(-1);

        const auto t0 = std::chrono::steady_clock::now();
        chain.filterProxy->sort(levelCol, Qt::AscendingOrder);
        // Force the row map to materialise.
        const int rowCount = chain.filterProxy->rowCount();
        const auto elapsed = std::chrono::steady_clock::now() - t0;

        qDebug().noquote() << QStringLiteral("Sort by enum column over %1 rows: %2 ms")
                                  .arg(static_cast<std::size_t>(rowCount))
                                  .arg(Ms(elapsed).count(), 0, 'f', 2);

        QCOMPARE(rowCount, static_cast<int>(LINE_COUNT));
        // Proxy-roundtrip sort gate. After the Phase 1 rewrite the
        // sort runs through `loglib::SortPermutationByColumn`: log
        // rows are resolved once (no per-compare proxy walk), a
        // `uint16_t` rank is pre-materialised in parallel for enum
        // columns, and `tbb::parallel_sort` permutes them. On the dev
        // box the full proxy cycle lands ~68 ms; 1 s is the generous
        // CI ceiling and also the user-visible target from the
        // performance plan. A regression past this threshold means
        // either the comparator stopped being branch-free or the
        // log-row pre-resolution disappeared.
        QVERIFY2(
            Ms(elapsed).count() < 1000.0,
            qPrintable(QStringLiteral("sort proxy roundtrip regressed: %1 ms").arg(Ms(elapsed).count()))
        );
    }

private:
    static constexpr std::size_t LINE_COUNT = 1'000'000;

    void RunConfig(const QString &label, bool reversed)
    {
        const RunResult run = RunOnce(mLogPath, reversed);
        QVERIFY2(run.finished, "streamingFinished must fire within the 180 s timeout");
        QCOMPARE(static_cast<std::size_t>(run.rowCount), LINE_COUNT);
        qDebug().noquote() << FormatThroughput(label, run.elapsed, mBytes, LINE_COUNT);
    }

    struct ProxyChain
    {
        std::unique_ptr<LogModel> model;
        std::unique_ptr<RowOrderProxyModel> rowProxy;
        std::unique_ptr<LogFilterModel> filterProxy;
    };

    /// Build a fresh proxy chain and drive a streaming parse of
    /// `mLogPath` to completion. Used by the filter / sort benchmarks
    /// so each measurement starts from a clean Qt-side state.
    ProxyChain BuildLoadedChain()
    {
        ProxyChain chain;
        chain.model = std::make_unique<LogModel>();
        chain.rowProxy = std::make_unique<RowOrderProxyModel>();
        chain.rowProxy->setSourceModel(chain.model.get());
        chain.filterProxy = std::make_unique<LogFilterModel>();
        chain.filterProxy->setSourceModel(chain.rowProxy.get());
        chain.filterProxy->SetLogModel(chain.model.get());

        QSignalSpy finishedSpy(chain.model.get(), &LogModel::streamingFinished);
        auto file = std::make_unique<loglib::LogFile>(mLogPath.string());
        auto fileSource = std::make_unique<loglib::FileLineSource>(std::move(file));
        loglib::FileLineSource *fileSourcePtr = fileSource.get();
        chain.model->BeginStreaming(
            std::move(fileSource),
            [fileSourcePtr, sink = chain.model->Sink()](loglib::StopToken token) {
                loglib::ParserOptions options;
                options.stopToken = std::move(token);
                const loglib::JsonParser parser;
                parser.ParseStreaming(*fileSourcePtr, *sink, options);
            }
        );
        const bool finished = finishedSpy.wait(180'000);
        Q_UNUSED(finished);
        return chain;
    }

    QTemporaryDir mTempDir;
    std::filesystem::path mLogPath;
    std::size_t mBytes = 0;
};

QTEST_MAIN(MainWindowBench)
#include "benchmark_main_window.moc"
