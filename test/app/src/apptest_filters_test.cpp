#include "advanced_filter_editor.hpp"
#include "anchor_manager.hpp"
#include "anchors_dock.hpp"
#include "apptest_support.hpp"
#include "cli_parser.hpp"
#include "column_editor.hpp"
#include "columns_manager_dialog.hpp"
#include "configuration_diagnostics_dialog.hpp"
#include "filter_editor.hpp"
#include "find_dock.hpp"
#include "find_record_widget.hpp"
#include "highlight_rule_set.hpp"
#include "jump_to_tail_pill.hpp"
#include "level_cell_delegate.hpp"
#include "log_filter_model.hpp"
#include "log_model.hpp"
#include "log_table_view.hpp"
#include "main_window.hpp"
#include "parse_errors_dock.hpp"
#include "preferences_editor.hpp"
#include "qstring_path.hpp"
#include "qt_streaming_log_sink.hpp"
#include "record_detail_dock.hpp"
#include "record_detail_widget.hpp"
#include "record_detail_window.hpp"
#include "row_order_proxy_model.hpp"
#include "session_bundle_dialog.hpp"
#include "session_history_manager.hpp"
#include "single_instance_guard.hpp"
#include "streaming_control.hpp"
#include "theme_control.hpp"
#include "uuid_utils.hpp"

#include <loglib/core/bytes_producer.hpp>
#include <loglib/core/compact_log_value.hpp>
#include <loglib/core/enum_dictionary.hpp>
#include <loglib/core/file_line_source.hpp>
#include <loglib/core/key_index.hpp>
#include <loglib/core/log_level.hpp>
#include <loglib/core/log_line.hpp>
#include <loglib/core/log_value.hpp>
#include <loglib/core/stop_token.hpp>
#include <loglib/core/stream_line_source.hpp>
#include <loglib/ingest/decompressing_byte_source.hpp>
#include <loglib/ingest/rotation_siblings.hpp>
#include <loglib/ingest/tailing_bytes_producer.hpp>
#include <loglib/ingest/tcp_server_producer.hpp>
#include <loglib/ingest/udp_server_producer.hpp>
#include <loglib/output/file_loading.hpp>
#include <loglib/output/session_bundle.hpp>
#include <loglib/parse/log_parse_sink.hpp>
#include <loglib/parse/parser_options.hpp>
#include <loglib/parse/parsers/csv_parser.hpp>
#include <loglib/parse/parsers/json_parser.hpp>
#include <loglib/parse/parsers/logfmt_parser.hpp>
#include <loglib/query/query_parser.hpp>
#include <loglib/query/row_filter.hpp>
#include <loglib/schema/filter_expression.hpp>
#include <loglib/schema/log_configuration.hpp>
#include <loglib/support/theme.hpp>
#include <loglib/support/tzdata_search.hpp>
#include <loglib/time/time_zone_context.hpp>

#include <test_common/network_log_client.hpp>
#include <test_common/wait.hpp>

#include <QAbstractItemDelegate>
#include <QAbstractItemModel>
#include <QAction>
#include <QApplication>
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
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QUuid>
#include <QVariant>
#include <QWheelEvent>
#include <QtTest/QtTest>

#include <zlib.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

using namespace apptest_support;

// NOLINTNEXTLINE(misc-use-internal-linkage): `Q_OBJECT` QtTest fixture; moc expects this declaration shape.
class FiltersTest : public apptest_support::AppTestBase
{
    Q_OBJECT

private slots:

    // LoadFile must give the same table as the GUI's static load: the same columns, in the same order, with the same
    // types and the same formatted cells. This guards the library loader against drifting from LogModel.
    static void TestLoadFileMatchesStaticLoadInModel()
    {
        const QTemporaryDir directory;
        QVERIFY(directory.isValid());

        const EquivalenceFixtures fixtures = MakeEquivalenceFixtures();

        struct Case
        {
            const char *name;
            QString path;
            std::unique_ptr<loglib::LogParser> parser;
        };
        std::vector<Case> cases;
        cases.push_back({
            .name = "json",
            .path = WriteEquivalenceFixture(directory, "equivalence.jsonl", fixtures.json),
            .parser = std::make_unique<loglib::JsonParser>(),
        });
        cases.push_back({
            .name = "logfmt",
            .path = WriteEquivalenceFixture(directory, "equivalence.log", fixtures.logfmt),
            .parser = std::make_unique<loglib::LogfmtParser>(),
        });
        cases.push_back({
            .name = "csv",
            .path = WriteEquivalenceFixture(directory, "equivalence.csv", fixtures.csv),
            .parser = std::make_unique<loglib::CsvParser>(),
        });

        for (const Case &testCase : cases)
        {
            const auto model = RunStaticLoadInModel(testCase.path, *testCase.parser);
            loglib::LoadOptions loadOptions;
            loadOptions.displayTimeZone = model->Table().DisplayTimeZone();
            loadOptions.defaultSourceTimeZone = model->Table().DefaultSourceTimeZone();
            const loglib::LoadResult loaded = loglib::LoadFile(logapp::QStringToFsPath(testCase.path), loadOptions);
            const loglib::LogTable &fromModel = model->Table();
            const loglib::LogTable &fromLibrary = loaded.table;
            const QString name = QString::fromLatin1(testCase.name);

            QVERIFY2(loaded.errors.empty(), qPrintable(name));
            QCOMPARE(static_cast<int>(fromLibrary.RowCount()), static_cast<int>(fromModel.RowCount()));
            QCOMPARE(static_cast<int>(fromLibrary.ColumnCount()), static_cast<int>(fromModel.ColumnCount()));
            QVERIFY2(fromLibrary.ColumnCount() >= 5, qPrintable(name));

            const auto &modelColumns = fromModel.Layout().Columns();
            const auto &libraryColumns = fromLibrary.Layout().Columns();
            for (size_t column = 0; column < fromLibrary.ColumnCount(); ++column)
            {
                QVERIFY2(
                    fromLibrary.GetHeader(column) == fromModel.GetHeader(column),
                    qPrintable(name + QStringLiteral(": header of column %1").arg(column))
                );
                QVERIFY2(
                    libraryColumns[column].type == modelColumns[column].type,
                    qPrintable(
                        name + QStringLiteral(": type of column %1 (%2, library %3 vs model %4)")
                                   .arg(column)
                                   .arg(QString::fromStdString(fromLibrary.GetHeader(column)))
                                   .arg(static_cast<int>(libraryColumns[column].type))
                                   .arg(static_cast<int>(modelColumns[column].type))
                    )
                );
            }
            for (size_t row = 0; row < fromLibrary.RowCount(); row += 7)
            {
                for (size_t column = 0; column < fromLibrary.ColumnCount(); ++column)
                {
                    QVERIFY2(
                        fromLibrary.GetFormattedValue(row, column) == fromModel.GetFormattedValue(row, column),
                        qPrintable(name + QStringLiteral(": cell %1, %2").arg(row).arg(column))
                    );
                }
            }
        }
    }

    static void TestFixtureIsoTTimestamp()
    {
        const FixtureFile fixture(":/fixtures/iso_t_timestamp.jsonl");
        StreamingRun run = RunStreaming(fixture.Path());
        // 2024-04-28T07:14:30 → 1714288470 seconds since epoch as a wall clock.
        AssertTimestampFixture(run, NaiveToUtcUs(1714288470000000), 3);
    }

    static void TestFixtureIsoSpaceTimestamp()
    {
        const FixtureFile fixture(":/fixtures/iso_space_timestamp.jsonl");
        StreamingRun run = RunStreaming(fixture.Path());
        // 2024-04-28 07:14:30 → 1714288470 seconds since epoch as a wall clock.
        AssertTimestampFixture(run, NaiveToUtcUs(1714288470000000), 3);
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
        // 2024-04-28T07:14:30.123 → 1714288470.123 → 1714288470123000 µs as a wall clock.
        AssertTimestampFixture(run, NaiveToUtcUs(1714288470123000), 3);

        // Spot-check the µs and 0.5s rows separately so all three fractional
        // widths in the fixture are exercised.
        const int tsCol = ColumnByHeader(*run.model, QStringLiteral("timestamp"));
        const qint64 row1Us = run.model->data(run.model->index(1, tsCol), LogModelItemDataRole::SortRole).toLongLong();
        QCOMPARE(row1Us, NaiveToUtcUs(1714288470123456));
        const qint64 row2Us = run.model->data(run.model->index(2, tsCol), LogModelItemDataRole::SortRole).toLongLong();
        QCOMPARE(row2Us, NaiveToUtcUs(1714288470500000));
    }

    static void TestFixtureAltTimestampKeys()
    {
        const FixtureFile fixture(":/fixtures/alt_timestamp_keys.jsonl");
        const StreamingRun run = RunStreaming(fixture.Path());
        QCOMPARE(run.finishedCount, 1);
        QCOMPARE(run.cancelled, false);
        QCOMPARE(run.model->rowCount(), 3);
        QVERIFY(run.model->StreamingErrors().empty());

        const auto &columns = run.model->Layout().Columns();
        const auto findColumn = [&](const std::string &header) -> const loglib::Column * {
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
            QCOMPARE(column->type, loglib::ColumnType::Time);
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

    // Logfmt fixture: ISO-T timestamps promote to `Type::Time` like
    // the JSON path. Reuses `AssertTimestampFixture` so the pinned
    // format-detection contract stays in lockstep across formats.
    static void TestLogfmtFixtureIsoTTimestamp()
    {
        const FixtureFile fixture(":/fixtures/iso_t_timestamp.logfmt");
        StreamingRun run = RunStreamingLogfmt(fixture.Path());
        // 2024-04-28T07:14:30 -> 1714288470 seconds since epoch as a wall clock.
        AssertTimestampFixture(run, NaiveToUtcUs(1714288470000000), 3);
    }

    // Goto Line happy path: drive the post-dialog body via the
    // test seam so both the 1-based-to-0-based flip and the
    // visibility probe run end-to-end. Line 12 -> source row 11.
    void TestMainWindowGotoLineJumpsToRequestedSourceRow()
    {
        auto *model = mWindow->Model();
        auto *view = mWindow->findChild<LogTableView *>();
        QVERIFY(view != nullptr);

        loglib::StreamLineSource &streamSource = BeginSyntheticStreamSession(*model);
        QtStreamingLogSink *sink = model->Sink();
        QVERIFY(sink != nullptr);
        loglib::KeyIndex &keys = sink->Keys();
        const loglib::KeyId valueKey = keys.GetOrInsert(std::string("value"));
        sink->OnBatch(MakeSyntheticBatch(streamSource, valueKey, 1, 20, true));
        QCoreApplication::processEvents();
        QCOMPARE(model->rowCount(), 20);

        view->selectionModel()->clearSelection();
        view->setCurrentIndex(QModelIndex{});

        mWindow->ExecuteGotoLineForTest(QStringLiteral("12"));
        QCoreApplication::processEvents();
        QCOMPARE(view->currentIndex().row(), 11);
        QCOMPARE(view->selectionModel()->selectedRows().count(), 1);
        QCOMPARE(view->selectionModel()->selectedRows().first().row(), 11);

        model->EndStreaming(false);
    }

    // Regression pin: the range check reads the live row count,
    // not a value captured before the modal. Driving the seam with
    // an oversize input simulates the shrink-during-modal case;
    // the hint must show the current range, not the stale one.
    void TestGotoLineOutOfRangeAfterShrinkSurfacesLiveRange()
    {
        auto *model = mWindow->Model();
        auto *view = mWindow->findChild<LogTableView *>();
        QVERIFY(view != nullptr);

        loglib::StreamLineSource &streamSource = BeginSyntheticStreamSession(*model);
        QtStreamingLogSink *sink = model->Sink();
        QVERIFY(sink != nullptr);
        loglib::KeyIndex &keys = sink->Keys();
        const loglib::KeyId valueKey = keys.GetOrInsert(std::string("value"));
        sink->OnBatch(MakeSyntheticBatch(streamSource, valueKey, 1, 10, true));
        QCoreApplication::processEvents();
        QCOMPARE(model->rowCount(), 10);

        view->selectionModel()->clearSelection();
        view->setCurrentIndex(QModelIndex{});
        mWindow->statusBar()->clearMessage();

        // Line 999 is well past the current 10-row model. The seam
        // must reject with a specific "out of range (1 - 10)" hint,
        // NOT jump the view.
        mWindow->ExecuteGotoLineForTest(QStringLiteral("999"));
        QCoreApplication::processEvents();

        const QString message = mWindow->statusBar()->currentMessage();
        QVERIFY2(
            message.contains(QStringLiteral("out of range"), Qt::CaseInsensitive),
            qUtf8Printable(QStringLiteral("Expected out-of-range hint, got: '%1'").arg(message))
        );
        // The bracketed range must reflect the *current* row count,
        // not the value captured before a hypothetical modal open.
        // If someone regresses to a stale captured value the range
        // would be wrong and this substring check would fail.
        QVERIFY2(
            message.contains(QStringLiteral("1 - 10")),
            qUtf8Printable(QStringLiteral("Expected live range '1 - 10' in hint, got: '%1'").arg(message))
        );
        // View selection unchanged (no accidental jump).
        QVERIFY(view->selectionModel()->selectedRows().isEmpty());

        model->EndStreaming(false);
    }

    // Regression pin: a filter-hidden target must surface the
    // caller-specific "Line %1 is currently filtered out." hint
    // instead of `SelectSourceRow`'s generic fallback.
    void TestGotoLineFilteredOutRowSurfacesSpecificHint()
    {
        const int timeCol = StreamFixtureWithTimeColumnForRowMenuTests();
        QVERIFY2(timeCol >= 0, "timestamp column must exist after streaming");
        auto *model = mWindow->Model();
        auto *proxy = mWindow->FilterModel();
        auto *view = mWindow->findChild<LogTableView *>();
        QVERIFY(model != nullptr);
        QVERIFY(proxy != nullptr);
        QVERIFY(view != nullptr);
        QCOMPARE(model->rowCount(), 3);

        const int msgCol = ColumnByHeader(*model, QStringLiteral("msg"));
        QVERIFY2(msgCol >= 0, "msg column must exist");

        // Keep only "third" (source row 2). Lines 1 and 2 become
        // filter-hidden targets; line 3 stays reachable.
        loglib::CallbackStringRowPredicate keepThird(static_cast<std::size_t>(msgCol), [](std::string_view value) {
            return value == std::string_view{"third"};
        });
        std::vector<loglib::RowPredicate> rules;
        rules.emplace_back(std::move(keepThird));
        proxy->SetFilterRules(std::move(rules));
        QCoreApplication::processEvents();
        QCOMPARE(proxy->rowCount(), 1);

        // Line 1 is filter-hidden -> specific hint, no jump.
        view->selectionModel()->clearSelection();
        view->setCurrentIndex(QModelIndex{});
        mWindow->statusBar()->clearMessage();
        mWindow->ExecuteGotoLineForTest(QStringLiteral("1"));
        QCoreApplication::processEvents();

        const QString hiddenMessage = mWindow->statusBar()->currentMessage();
        QVERIFY2(
            hiddenMessage.contains(QStringLiteral("filtered out"), Qt::CaseInsensitive),
            qUtf8Printable(QStringLiteral("Expected 'filtered out' hint, got: '%1'").arg(hiddenMessage))
        );
        QVERIFY2(
            hiddenMessage.contains(QStringLiteral("Line 1")),
            qUtf8Printable(QStringLiteral("Expected 'Line 1' in hint, got: '%1'").arg(hiddenMessage))
        );
        QVERIFY(view->selectionModel()->selectedRows().isEmpty());

        // Line 3 is visible -> jump succeeds and the "filtered out"
        // hint is NOT re-printed. Under the single-row visible
        // filter, source row 2 maps to proxy row 0.
        mWindow->statusBar()->clearMessage();
        mWindow->ExecuteGotoLineForTest(QStringLiteral("3"));
        QCoreApplication::processEvents();
        QCOMPARE(view->currentIndex().row(), 0);
        QCOMPARE(view->selectionModel()->selectedRows().count(), 1);

        // Restore the pristine "no filter" state so downstream tests
        // see the fixture unchanged.
        proxy->SetFilterRules({});
        QCoreApplication::processEvents();
    }

    // Empty-model early bail: the seam surfaces "No log loaded."
    // and does nothing else.
    void TestGotoLineNoLogLoadedSurfacesHint()
    {
        auto *model = mWindow->Model();
        QVERIFY(model != nullptr);
        QCOMPARE(model->rowCount(), 0);

        mWindow->statusBar()->clearMessage();
        mWindow->ExecuteGotoLineForTest(QStringLiteral("1"));
        QCoreApplication::processEvents();

        const QString message = mWindow->statusBar()->currentMessage();
        QVERIFY2(
            message.contains(QStringLiteral("No log loaded"), Qt::CaseInsensitive),
            qUtf8Printable(QStringLiteral("Expected 'No log loaded' hint, got: '%1'").arg(message))
        );
    }

    // Parser: relative `-Nh` / `-Nm` shortcuts (case-insensitive,
    // whitespace-tolerant) resolve against the pinned @p now.
    // `isNaive == false` since @p now is already UTC.
    void TestParseGotoTimestampInputAcceptsRelativeShortcuts()
    {
        const std::chrono::system_clock::time_point kNow =
            std::chrono::system_clock::time_point{std::chrono::microseconds{1'700'000'000'000'000LL}};
        const int64_t kNowMicros = 1'700'000'000'000'000LL;

        const std::vector<std::string> noFormats;

        const auto oneHourAgo = MainWindow::ParseGotoTimestampInput(QStringLiteral("-1h"), noFormats, kNow);
        QVERIFY(oneHourAgo.has_value());
        QCOMPARE(oneHourAgo->micros, kNowMicros - (3600LL * 1'000'000LL));
        QVERIFY(!oneHourAgo->isNaive);

        const auto thirtyMinAgo = MainWindow::ParseGotoTimestampInput(QStringLiteral("-30m"), noFormats, kNow);
        QVERIFY(thirtyMinAgo.has_value());
        QCOMPARE(thirtyMinAgo->micros, kNowMicros - (30LL * 60LL * 1'000'000LL));
        QVERIFY(!thirtyMinAgo->isNaive);

        // Case-insensitive, tolerant of internal whitespace.
        const auto capitalH = MainWindow::ParseGotoTimestampInput(QStringLiteral("- 2 H"), noFormats, kNow);
        QVERIFY(capitalH.has_value());
        QCOMPARE(capitalH->micros, kNowMicros - (2LL * 3600LL * 1'000'000LL));

        // `+N` and bare `N` also mean "N units ago" (parity with
        // lnav / less; "future" is meaningless in a log viewer).
        const auto plusOneHour = MainWindow::ParseGotoTimestampInput(QStringLiteral("+1h"), noFormats, kNow);
        QVERIFY(plusOneHour.has_value());
        QCOMPARE(plusOneHour->micros, kNowMicros - (3600LL * 1'000'000LL));

        const auto bareOneHour = MainWindow::ParseGotoTimestampInput(QStringLiteral("1h"), noFormats, kNow);
        QVERIFY(bareOneHour.has_value());
        QCOMPARE(bareOneHour->micros, kNowMicros - (3600LL * 1'000'000LL));
    }

    // Parser: relative shortcuts must reject over-`int64_t`
    // micros rather than wrap (a wrap would jump the user
    // forward). Both units are covered since the cap arithmetic
    // uses a per-unit `microsPerUnit`.
    void TestParseGotoTimestampInputRejectsOverflowingRelativeShortcut()
    {
        const std::chrono::system_clock::time_point kNow = std::chrono::system_clock::now();
        const std::vector<std::string> noFormats;

        // 10^19 hours -- above both the int64 hour cap and 2^63.
        QVERIFY(
            !MainWindow::ParseGotoTimestampInput(QStringLiteral("-10000000000000000000h"), noFormats, kNow).has_value()
        );

        // Hour boundary: cap itself parses, cap+1 rejects.
        const auto hourCap = static_cast<qulonglong>(std::numeric_limits<int64_t>::max() / (3600LL * 1'000'000LL));
        const auto overHourCap =
            MainWindow::ParseGotoTimestampInput(QString::number(hourCap + 1) + QStringLiteral("h"), noFormats, kNow);
        QVERIFY(!overHourCap.has_value());

        const auto atHourCap =
            MainWindow::ParseGotoTimestampInput(QString::number(hourCap) + QStringLiteral("h"), noFormats, kNow);
        QVERIFY(atHourCap.has_value());

        // Minute boundary: distinct code path (different
        // `microsPerUnit`), so a branch-swap regression would
        // pass the hour test above and fail here.
        const auto minuteCap = static_cast<qulonglong>(std::numeric_limits<int64_t>::max() / (60LL * 1'000'000LL));
        const auto overMinuteCap =
            MainWindow::ParseGotoTimestampInput(QString::number(minuteCap + 1) + QStringLiteral("m"), noFormats, kNow);
        QVERIFY(!overMinuteCap.has_value());

        const auto atMinuteCap =
            MainWindow::ParseGotoTimestampInput(QString::number(minuteCap) + QStringLiteral("m"), noFormats, kNow);
        QVERIFY(atMinuteCap.has_value());
    }

    // Parser: empty and garbage strings return nullopt.
    void TestParseGotoTimestampInputRejectsGarbage()
    {
        const std::chrono::system_clock::time_point kNow = std::chrono::system_clock::now();

        QVERIFY(!MainWindow::ParseGotoTimestampInput(QString{}, {}, kNow).has_value());
        QVERIFY(!MainWindow::ParseGotoTimestampInput(QStringLiteral("   "), {}, kNow).has_value());
        QVERIFY(!MainWindow::ParseGotoTimestampInput(QStringLiteral("not a timestamp"), {}, kNow).has_value());
        // Mixed sign / letters in the relative shortcut still fail.
        QVERIFY(!MainWindow::ParseGotoTimestampInput(QStringLiteral("-1x"), {}, kNow).has_value());
        QVERIFY(!MainWindow::ParseGotoTimestampInput(QStringLiteral("--1h"), {}, kNow).has_value());
    }

    // Goto Timestamp binary search: no user sort active. The
    // fixture streams three timestamps 1 s apart; the query
    // targets the middle row exactly.
    void TestFindFirstRowAtOrAfterBinarySearchesWhenUnsorted()
    {
        const int timeCol = StreamFixtureWithTimeColumnForRowMenuTests();
        QVERIFY2(timeCol >= 0, "timestamp column must exist after streaming");
        auto *proxy = mWindow->FilterModel();
        QVERIFY(proxy != nullptr);
        QCOMPARE(proxy->SortColumn(), -1);

        // Middle row's exact timestamp: 2024-04-28T10:00:02+00:00.
        // Feed epoch micros directly so the test is timezone-agnostic.
        const auto *table = &mWindow->Model()->Table();
        const auto midMicros = loglib::AsEpochMicroseconds(table->GetValue(1, static_cast<std::size_t>(timeCol)));
        QVERIFY(midMicros.has_value());

        QCOMPARE(mWindow->FindFirstRowAtOrAfter(timeCol, *midMicros), 1);

        // A tick earlier still lands on the middle row.
        QCOMPARE(mWindow->FindFirstRowAtOrAfter(timeCol, *midMicros - 1), 1);

        // Way past the last timestamp: no row qualifies.
        const auto lastMicros = loglib::AsEpochMicroseconds(table->GetValue(2, static_cast<std::size_t>(timeCol)));
        QVERIFY(lastMicros.has_value());
        QCOMPARE(mWindow->FindFirstRowAtOrAfter(timeCol, *lastMicros + 1), -1);
    }

    // Regression pin: a mid-file *missing* timestamp must not
    // break the fast-path binary search. Middle row omits the
    // `timestamp` key entirely. Covers every boundary: before /
    // at-first, between valid rows (skip the missing one), at /
    // past last.
    void TestFindFirstRowAtOrAfterHandlesInterleavedMissingTimestamps()
    {
        auto *model = mWindow->Model();
        Q_ASSERT(model != nullptr);

        const QStringList lines{
            QStringLiteral(R"({"timestamp":"2024-04-28T10:00:01+00:00","msg":"first"})"),
            QStringLiteral(R"({"msg":"middle has no timestamp"})"),
            QStringLiteral(R"({"timestamp":"2024-04-28T10:00:03+00:00","msg":"third"})"),
        };
        const TempJsonFile fixture(lines);
        QSignalSpy finishedSpy(model, &LogModel::streamingFinished);
        QVERIFY(finishedSpy.isValid());
        auto fileSource = std::make_unique<loglib::FileLineSource>(fixture.Path().toStdString());
        loglib::FileLineSource *fileSourcePtr = fileSource.get();
        const loglib::StopToken stopToken = model->BeginStreamingForSyncTest(std::move(fileSource));
        loglib::ParserOptions options;
        options.stopToken = stopToken;
        options.threads = 1;
        loglib::JsonParser{}.Parse(*fileSourcePtr, *model->Sink(), options);
        if (finishedSpy.count() == 0)
        {
            finishedSpy.wait(60000);
        }
        QCoreApplication::processEvents();

        const int timeCol = ColumnByHeader(*model, QStringLiteral("timestamp"));
        QVERIFY2(timeCol >= 0, "timestamp column must exist");
        QCOMPARE(model->rowCount(), 3);

        const auto *table = &model->Table();
        const auto firstMicros = loglib::AsEpochMicroseconds(table->GetValue(0, static_cast<std::size_t>(timeCol)));
        const auto lastMicros = loglib::AsEpochMicroseconds(table->GetValue(2, static_cast<std::size_t>(timeCol)));
        QVERIFY(firstMicros.has_value());
        QVERIFY(lastMicros.has_value());

        // Before the earliest timestamp -> row 0 (row 1 is missing
        // and must be skipped over, not treated as a match).
        QCOMPARE(mWindow->FindFirstRowAtOrAfter(timeCol, *firstMicros - 1'000'000), 0);
        QCOMPARE(mWindow->FindFirstRowAtOrAfter(timeCol, *firstMicros), 0);

        // Between the two valid timestamps -> row 2 (row 1 is
        // missing; must NOT be returned).
        QCOMPARE(mWindow->FindFirstRowAtOrAfter(timeCol, *firstMicros + 1), 2);

        // Exactly-last / past-last.
        QCOMPARE(mWindow->FindFirstRowAtOrAfter(timeCol, *lastMicros), 2);
        QCOMPARE(mWindow->FindFirstRowAtOrAfter(timeCol, *lastMicros + 1), -1);
    }

    // Regression pin: with a filter that hides source rows 0 and 1
    // (keeps only row 2), the fast path must jump forward to the
    // visible row rather than returning the hidden one.
    void TestFindFirstRowAtOrAfterSkipsFilterHiddenRowsInFastPath()
    {
        const int timeCol = StreamFixtureWithTimeColumnForRowMenuTests();
        QVERIFY2(timeCol >= 0, "timestamp column must exist after streaming");
        auto *model = mWindow->Model();
        auto *proxy = mWindow->FilterModel();
        QVERIFY(model != nullptr);
        QVERIFY(proxy != nullptr);
        QCOMPARE(model->rowCount(), 3);

        const int msgCol = ColumnByHeader(*model, QStringLiteral("msg"));
        QVERIFY2(msgCol >= 0, "msg column must exist");

        // Keep only the row whose `msg` equals "third" (source
        // row 2). `CallbackStringRowPredicate` is the same variant
        // arm the Filters dock uses.
        loglib::CallbackStringRowPredicate keepThird(static_cast<std::size_t>(msgCol), [](std::string_view value) {
            return value == std::string_view{"third"};
        });

        std::vector<loglib::RowPredicate> rules;
        rules.emplace_back(std::move(keepThird));
        proxy->SetFilterRules(std::move(rules));
        QCoreApplication::processEvents();
        QCOMPARE(proxy->rowCount(), 1);
        QCOMPARE(proxy->SortColumn(), -1);

        // Target before every row's ts: without the visibility
        // step this would return row 0 (filtered out); with it,
        // it must jump forward to visible row 2 ("third").
        const auto *table = &model->Table();
        const auto firstMicros = loglib::AsEpochMicroseconds(table->GetValue(0, static_cast<std::size_t>(timeCol)));
        QVERIFY(firstMicros.has_value());
        QCOMPARE(mWindow->FindFirstRowAtOrAfter(timeCol, *firstMicros - 1'000'000), 2);
        QCOMPARE(mWindow->FindFirstRowAtOrAfter(timeCol, *firstMicros), 2);

        // Target past the only visible row's ts: no live row
        // qualifies -- must be -1, not a hidden row further down.
        const auto lastMicros = loglib::AsEpochMicroseconds(table->GetValue(2, static_cast<std::size_t>(timeCol)));
        QVERIFY(lastMicros.has_value());
        QCOMPARE(mWindow->FindFirstRowAtOrAfter(timeCol, *lastMicros + 1), -1);

        // Restore the pristine "no filter" state so downstream
        // tests in this suite see the fixture unchanged.
        proxy->SetFilterRules({});
        QCoreApplication::processEvents();
    }

    // User-sort path: descending `msg` sort forces a real
    // permutation. The scan walks proxy rows in display order,
    // so the "first" qualifying row is the last-in-file
    // ("third", source row 2).
    void TestFindFirstRowAtOrAfterLinearScansWhenUserSortActive()
    {
        const int timeCol = StreamFixtureWithTimeColumnForRowMenuTests();
        QVERIFY2(timeCol >= 0, "timestamp column must exist after streaming");
        const int msgCol = ColumnByHeader(*mWindow->Model(), QStringLiteral("msg"));
        QVERIFY2(msgCol >= 0, "msg column must exist after streaming");

        auto *view = mWindow->findChild<LogTableView *>();
        QVERIFY(view != nullptr);
        view->sortByColumn(msgCol, Qt::DescendingOrder);
        QCoreApplication::processEvents();

        auto *proxy = mWindow->FilterModel();
        QVERIFY(proxy != nullptr);
        QCOMPARE(proxy->SortColumn(), msgCol);

        // Earliest possible target (INT64_MIN) still picks the
        // *first* row in display order, which under `msg DESC` is
        // source row 2 ("third").
        const int found = mWindow->FindFirstRowAtOrAfter(timeCol, std::numeric_limits<int64_t>::min());
        QCOMPARE(found, 2);
    }

    // Seam: post-`exec` body dispatches naive-vs-zoned, updates
    // the sticky-input mirror BEFORE the parse (so a garbage
    // input the user wants to correct survives), and lands on
    // the qualifying source row.
    void TestExecuteGotoTimestampSeamJumpsAndRemembersInput()
    {
        const int timeCol = StreamFixtureWithTimeColumnForRowMenuTests();
        QVERIFY2(timeCol >= 0, "timestamp column must exist after streaming");
        auto *model = mWindow->Model();
        auto *view = mWindow->findChild<LogTableView *>();
        QVERIFY(model != nullptr);
        QVERIFY(view != nullptr);
        QCOMPARE(model->rowCount(), 3);

        const std::chrono::system_clock::time_point kNow =
            std::chrono::system_clock::time_point{std::chrono::microseconds{1'700'000'000'000'000LL}};

        view->selectionModel()->clearSelection();
        view->setCurrentIndex(QModelIndex{});
        mWindow->statusBar()->clearMessage();

        // Zoned ISO input matching the fixture's middle row. The
        // zoned parse skips the naive -> UTC shift (double-shift
        // regression pin) so the row lands regardless of host TZ.
        mWindow->ExecuteGotoTimestampForTest(QStringLiteral("2024-04-28T10:00:02+00:00"), kNow);
        QCoreApplication::processEvents();
        QCOMPARE(view->currentIndex().row(), 1);

        // Sticky mirror keeps the raw string, not a canonicalised
        // form.
        QCOMPARE(mWindow->LastGotoTimestampInputForTest(), QStringLiteral("2024-04-28T10:00:02+00:00"));

        // Garbage input: parse fails, sticky mirror still updates
        // so the user can correct without retyping the whole
        // string.
        mWindow->ExecuteGotoTimestampForTest(QStringLiteral("not a timestamp"), kNow);
        QCoreApplication::processEvents();
        QCOMPARE(mWindow->LastGotoTimestampInputForTest(), QStringLiteral("not a timestamp"));
        const QString hint = mWindow->statusBar()->currentMessage();
        QVERIFY2(
            hint.contains(QStringLiteral("Could not parse"), Qt::CaseInsensitive),
            qUtf8Printable(QStringLiteral("Expected parse-error hint, got: '%1'").arg(hint))
        );
    }

    // Seam: an out-of-range target surfaces the "no visible row
    // at or after that time" hint without moving the selection.
    void TestExecuteGotoTimestampSeamHintsWhenNoRowQualifies()
    {
        const int timeCol = StreamFixtureWithTimeColumnForRowMenuTests();
        QVERIFY2(timeCol >= 0, "timestamp column must exist after streaming");
        auto *view = mWindow->findChild<LogTableView *>();
        QVERIFY(view != nullptr);

        const std::chrono::system_clock::time_point kNow =
            std::chrono::system_clock::time_point{std::chrono::microseconds{1'700'000'000'000'000LL}};

        view->selectionModel()->clearSelection();
        view->setCurrentIndex(QModelIndex{});
        mWindow->statusBar()->clearMessage();

        // 2099 is well past the fixture's 2024 rows.
        mWindow->ExecuteGotoTimestampForTest(QStringLiteral("2099-01-01T00:00:00+00:00"), kNow);
        QCoreApplication::processEvents();

        const QString hint = mWindow->statusBar()->currentMessage();
        QVERIFY2(
            hint.contains(QStringLiteral("No visible row"), Qt::CaseInsensitive),
            qUtf8Printable(QStringLiteral("Expected 'No visible row' hint, got: '%1'").arg(hint))
        );
        QVERIFY(view->selectionModel()->selectedRows().isEmpty());
    }

    // `NewSession()` wipes the sticky Goto Timestamp mirror so
    // a stale reference doesn't pre-populate the next dialog.
    void TestGotoTimestampStickyInputClearsOnNewSession()
    {
        const int timeCol = StreamFixtureWithTimeColumnForRowMenuTests();
        QVERIFY2(timeCol >= 0, "timestamp column must exist after streaming");

        const std::chrono::system_clock::time_point kNow =
            std::chrono::system_clock::time_point{std::chrono::microseconds{1'700'000'000'000'000LL}};

        mWindow->ExecuteGotoTimestampForTest(QStringLiteral("2024-04-28T10:00:02+00:00"), kNow);
        QCoreApplication::processEvents();
        QCOMPARE(mWindow->LastGotoTimestampInputForTest(), QStringLiteral("2024-04-28T10:00:02+00:00"));

        // Trigger via the wired action so the private slot is
        // reached the same way `Ctrl+N` reaches it in the app.
        auto *action = mWindow->findChild<QAction *>(QStringLiteral("actionNewSession"));
        QVERIFY2(action != nullptr, "actionNewSession must be reachable via objectName");
        action->trigger();
        QCoreApplication::processEvents();
        QCOMPARE(mWindow->LastGotoTimestampInputForTest(), QString{});
    }

    // Seam: relative shortcut resolves against the pinned @p now
    // (not `system_clock::now()`) and lands on the qualifying
    // row. `-1h` from `midMicros + 1h` lands on the middle row.
    void TestExecuteGotoTimestampSeamResolvesRelativeShortcutAgainstPinnedNow()
    {
        const int timeCol = StreamFixtureWithTimeColumnForRowMenuTests();
        QVERIFY2(timeCol >= 0, "timestamp column must exist after streaming");
        auto *model = mWindow->Model();
        auto *view = mWindow->findChild<LogTableView *>();
        QVERIFY(model != nullptr);
        QVERIFY(view != nullptr);

        // Middle row's stored ts is the "target" the shortcut
        // will resolve to when `now` sits exactly one hour later.
        const auto *table = &model->Table();
        const auto midMicros = loglib::AsEpochMicroseconds(table->GetValue(1, static_cast<std::size_t>(timeCol)));
        QVERIFY(midMicros.has_value());
        const std::chrono::system_clock::time_point pinnedNow{
            std::chrono::microseconds{*midMicros + (3600LL * 1'000'000LL)}
        };

        view->selectionModel()->clearSelection();
        view->setCurrentIndex(QModelIndex{});

        mWindow->ExecuteGotoTimestampForTest(QStringLiteral("-1h"), pinnedNow);
        QCoreApplication::processEvents();

        // `-1h` from `midMicros + 1h` == `midMicros` -> row 1.
        QCOMPARE(view->currentIndex().row(), 1);
    }

    // Non-monotonic branch: with monotonicity forced false, the
    // fast path is unsafe. Fall back to a proxy walk that picks
    // the visible row with the smallest ts >= target. The fixture
    // is monotonic, so the ANSWER is unchanged; the point is that
    // the alternate code path runs and still returns correctly.
    void TestFindFirstRowAtOrAfterUsesProxyIterationWhenNonMonotonic()
    {
        const int timeCol = StreamFixtureWithTimeColumnForRowMenuTests();
        QVERIFY2(timeCol >= 0, "timestamp column must exist after streaming");
        auto *model = mWindow->Model();
        QVERIFY(model != nullptr);

        // Baseline: fast path returns the middle row exactly.
        const auto *table = &model->Table();
        const auto midMicros = loglib::AsEpochMicroseconds(table->GetValue(1, static_cast<std::size_t>(timeCol)));
        QVERIFY(midMicros.has_value());
        QVERIFY(model->TimestampsAreMonotonic());
        QCOMPARE(mWindow->FindFirstRowAtOrAfter(timeCol, *midMicros), 1);

        mWindow->ForceTimestampsNonMonotonicForTest();
        QVERIFY(!model->TimestampsAreMonotonic());

        // Same query, different branch, same answer.
        QCOMPARE(mWindow->FindFirstRowAtOrAfter(timeCol, *midMicros), 1);

        // A tick before the middle row still picks the middle row
        // (proxy iteration returns the min ts satisfying >=).
        QCOMPARE(mWindow->FindFirstRowAtOrAfter(timeCol, *midMicros - 1), 1);

        // Past the last row -> -1.
        const auto lastMicros = loglib::AsEpochMicroseconds(table->GetValue(2, static_cast<std::size_t>(timeCol)));
        QVERIFY(lastMicros.has_value());
        QCOMPARE(mWindow->FindFirstRowAtOrAfter(timeCol, *lastMicros + 1), -1);
    }

    // Streaming path: the monotonicity flag flips false when a
    // second batch's first valid ts predates the first batch's
    // last (multi-file `Append` / rotation). Pinned end-to-end
    // through `LogModel::AppendBatch`.
    void TestLogModelTimestampsMonotonicFlipsOnBatchBoundaryInversion()
    {
        auto *model = mWindow->Model();
        QVERIFY(model != nullptr);
        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): false positive; prior `QVERIFY` aborts on null.
        QVERIFY(model->TimestampsAreMonotonic());

        // Batch 1: three rows at 2024-04-28T10:00:0{1,2,3}Z.
        const QStringList firstBatchLines{
            QStringLiteral(R"({"timestamp":"2024-04-28T10:00:01+00:00","msg":"first"})"),
            QStringLiteral(R"({"timestamp":"2024-04-28T10:00:02+00:00","msg":"second"})"),
            QStringLiteral(R"({"timestamp":"2024-04-28T10:00:03+00:00","msg":"third"})"),
        };
        const TempJsonFile firstFixture(firstBatchLines);
        QSignalSpy firstFinishedSpy(model, &LogModel::streamingFinished);
        auto firstSource = std::make_unique<loglib::FileLineSource>(firstFixture.Path().toStdString());
        loglib::FileLineSource *firstPtr = firstSource.get();
        const loglib::StopToken firstToken = model->BeginStreamingForSyncTest(std::move(firstSource));
        loglib::ParserOptions firstOpts;
        firstOpts.stopToken = firstToken;
        firstOpts.threads = 1;
        loglib::JsonParser{}.Parse(*firstPtr, *model->Sink(), firstOpts);
        if (firstFinishedSpy.count() == 0)
        {
            firstFinishedSpy.wait(60000);
        }
        QCoreApplication::processEvents();
        QCOMPARE(model->rowCount(), 3);
        // Still monotonic after a well-ordered batch.
        QVERIFY(model->TimestampsAreMonotonic());

        // Batch 2 (via `AppendStreaming`): rows dated BEFORE the
        // first batch -- the multi-file `Append` scenario.
        const QStringList secondBatchLines{
            QStringLiteral(R"({"timestamp":"2024-04-28T09:00:01+00:00","msg":"older-a"})"),
            QStringLiteral(R"({"timestamp":"2024-04-28T09:00:02+00:00","msg":"older-b"})"),
        };
        const TempJsonFile secondFixture(secondBatchLines);
        QSignalSpy secondFinishedSpy(model, &LogModel::streamingFinished);
        auto secondSource = std::make_unique<loglib::FileLineSource>(secondFixture.Path().toStdString());
        loglib::FileLineSource *secondPtr = secondSource.get();
        const loglib::StopToken secondToken = model->AppendStreaming(std::move(secondSource), {});
        loglib::ParserOptions secondOpts;
        secondOpts.stopToken = secondToken;
        secondOpts.threads = 1;
        loglib::JsonParser{}.Parse(*secondPtr, *model->Sink(), secondOpts);
        if (secondFinishedSpy.count() == 0)
        {
            secondFinishedSpy.wait(60000);
        }
        QCoreApplication::processEvents();
        QCOMPARE(model->rowCount(), 5);
        // Flag flipped false and does NOT self-heal.
        QVERIFY(!model->TimestampsAreMonotonic());
    }

    // `NewSession()` restores the monotonicity flag so a fresh
    // well-ordered log doesn't inherit a prior "non-monotonic"
    // verdict.
    void TestLogModelTimestampsMonotonicResetsOnNewSession()
    {
        auto *model = mWindow->Model();
        QVERIFY(model != nullptr);

        mWindow->ForceTimestampsNonMonotonicForTest();
        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): false positive; prior `QVERIFY` aborts on null.
        QVERIFY(!model->TimestampsAreMonotonic());

        auto *action = mWindow->findChild<QAction *>(QStringLiteral("actionNewSession"));
        QVERIFY2(action != nullptr, "actionNewSession must be reachable via objectName");
        action->trigger();
        QCoreApplication::processEvents();
        QVERIFY(model->TimestampsAreMonotonic());
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
        const auto &columns = run.model->Layout().Columns();
        QVERIFY(static_cast<size_t>(levelCol) < columns.size());
        QCOMPARE(columns[static_cast<size_t>(levelCol)].type, loglib::ColumnType::Enumeration);

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

    // AdvancedFilterEditor: a well-formed query parses cleanly,
    // enables the OK button, and round-trips through `Result()`.
    void TestAdvancedFilterEditorAcceptsWellFormedQuery()
    {
        AdvancedFilterEditor editor;
        editor.SetQueryText(QStringLiteral("service:auth AND level:error"));
        QCoreApplication::processEvents();

        const auto result = editor.Result();
        QVERIFY2(result.has_value(), "well-formed query must parse successfully");
        // The parsed tree must be a two-leaf AND (implicit-AND merges
        // with the explicit AND at the same precedence level).
        const auto *andNode = std::get_if<loglib::FilterExpression::And>(&result->node);
        QVERIFY2(andNode != nullptr, "top-level node must be AND");
        QCOMPARE(static_cast<int>(andNode->children.size()), 2);

        // OK is exposed via the QDialogButtonBox; find and confirm
        // it's enabled so the user can commit.
        auto *ok = editor.findChild<QDialogButtonBox *>(QStringLiteral("advancedFilterButtonBox"));
        QVERIFY2(ok != nullptr, "AdvancedFilterEditor must expose its button box");
        auto *okBtn = ok->button(QDialogButtonBox::Ok);
        QVERIFY2(okBtn != nullptr && okBtn->isEnabled(), "OK must be enabled on a valid query");
    }

    // AdvancedFilterEditor: a syntactically invalid query clears the
    // result and disables the OK button.
    void TestAdvancedFilterEditorDisablesOkOnParseError()
    {
        AdvancedFilterEditor editor;
        // Leading operator without a column is a lex error at the
        // very first character; the parser produces a diagnostic
        // with an offset >= 0.
        editor.SetQueryText(QStringLiteral("service:"));
        QCoreApplication::processEvents();

        QVERIFY2(!editor.Result().has_value(), "malformed query must not yield a Result()");
        auto *box = editor.findChild<QDialogButtonBox *>(QStringLiteral("advancedFilterButtonBox"));
        QVERIFY2(box != nullptr, "AdvancedFilterEditor must expose its button box");
        auto *okBtn = box->button(QDialogButtonBox::Ok);
        QVERIFY2(okBtn != nullptr && !okBtn->isEnabled(), "OK must be disabled on a parse error");

        // Recovering to a valid query re-enables OK -- this proves
        // the reparse-on-change wiring survives round-trips through
        // an error state.
        editor.SetQueryText(QStringLiteral("service:auth"));
        QCoreApplication::processEvents();
        QVERIFY(editor.Result().has_value());
        QVERIFY(okBtn->isEnabled());
    }

    // AdvancedFilterEditor: `LoadFromExpression` seeds the text field
    // from an existing tree via `FormatExpression`, and `Result()`
    // round-trips back to an equivalent AST. Match-all seeds as an
    // empty field (users start on a clean slate).
    void TestAdvancedFilterEditorRoundTripsThroughFormatAndParse()
    {
        AdvancedFilterEditor editor;

        // Default-constructed = match all -> empty field.
        const loglib::FilterExpression matchAll;
        editor.LoadFromExpression(matchAll);
        QCoreApplication::processEvents();
        QCOMPARE(editor.QueryText(), QString());
        const auto matchAllResult = editor.Result();
        QVERIFY(matchAllResult.has_value());
        QVERIFY(loglib::IsMatchAll(*matchAllResult));

        // Non-trivial expression built via the loglib parser (which is
        // the wire the editor uses internally). `LoadFromExpression`
        // must render it back to a form the parser accepts as
        // equivalent.
        const auto seeded = loglib::ParseQuery("service:auth AND NOT level:info", loglib::ProcessDefaultTimeZone());
        QVERIFY2(seeded.has_value(), "seed query must parse");
        editor.LoadFromExpression(*seeded);
        QCoreApplication::processEvents();
        QVERIFY2(!editor.QueryText().isEmpty(), "non-empty tree must seed a non-empty field");
        const auto result = editor.Result();
        QVERIFY2(result.has_value(), "seeded tree must reparse cleanly");
        QVERIFY(*result == *seeded);
    }

    // Regression: `ParseQuery` only extracts the pattern between the
    // `/.../` delimiters; it does not compile the regex. Without a
    // second-stage `QRegularExpression::isValid()` check the Advanced
    // editor would accept a query like `msg~/*[bad/`, hand it to
    // `MakeStringMatcher`, and silently reject every row. Confirm the
    // editor now surfaces the failure and keeps OK disabled.
    void TestAdvancedFilterEditorRejectsInvalidRegex()
    {
        AdvancedFilterEditor editor;
        editor.SetQueryText(QStringLiteral("msg ~ /*[bad/"));
        QCoreApplication::processEvents();

        QVERIFY2(
            !editor.Result().has_value(),
            "invalid regex must invalidate the cached Result so OpenAdvancedFilter can't commit it"
        );
        auto *box = editor.findChild<QDialogButtonBox *>(QStringLiteral("advancedFilterButtonBox"));
        QVERIFY2(box != nullptr, "AdvancedFilterEditor must expose its button box");
        auto *okBtn = box->button(QDialogButtonBox::Ok);
        QVERIFY2(okBtn != nullptr && !okBtn->isEnabled(), "OK must be disabled when a regex leaf fails to compile");

        // The status label must name the offending pattern so the
        // user can fix it. The exact QRegularExpression error text
        // is Qt-version-dependent; assert on our stable prefix and
        // the pattern only.
        auto *status = editor.findChild<QLabel *>(QStringLiteral("advancedFilterStatusLabel"));
        QVERIFY2(status != nullptr, "AdvancedFilterEditor must expose its status label");
        const QString msg = status->text();
        QVERIFY2(
            msg.contains(QStringLiteral("Invalid regular expression"), Qt::CaseSensitive),
            qPrintable(QStringLiteral("status must explain the rejection; got '%1'").arg(msg))
        );
        QVERIFY2(
            msg.contains(QStringLiteral("*[bad")),
            qPrintable(QStringLiteral("status must name the offending pattern; got '%1'").arg(msg))
        );

        // Recovering to a valid regex re-enables OK (mirrors the
        // parse-error recovery in `TestAdvancedFilterEditorDisablesOkOnParseError`).
        editor.SetQueryText(QStringLiteral("msg ~ /good.+/"));
        QCoreApplication::processEvents();
        QVERIFY(editor.Result().has_value());
        QVERIFY(okBtn->isEnabled());
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
        SaveLayoutForTest(*model, savedPath.toStdString());
        model->Reset();
        LoadLayoutForTest(*model, savedPath.toStdString());
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
        QCOMPARE(installed.type, loglib::LeafRule::Type::Time);
        QCOMPARE(installed.columnKeys, model->Layout().Columns()[static_cast<size_t>(timeCol)].keys);
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
        QCOMPARE(installed.type, loglib::LeafRule::Type::Time);
        QCOMPARE(installed.columnKeys, model->Layout().Columns()[static_cast<size_t>(timeCol)].keys);
        QVERIFY2(!installed.filterBegin.has_value(), "open lower bound must be std::nullopt, not INT64_MIN");
        QVERIFY(installed.filterEnd.has_value());
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        QCOMPARE(*installed.filterEnd, row2Micros);
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
        QCOMPARE(it->second.type, loglib::LeafRule::Type::Time);
        QCOMPARE(it->second.columnKeys, model->Layout().Columns()[static_cast<size_t>(timeCol)].keys);
        QVERIFY(it->second.filterBegin.has_value());
        QVERIFY2(!it->second.filterEnd.has_value(), "open upper bound must round-trip as std::nullopt");
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        QCOMPARE(*it->second.filterBegin, row1Micros);

        // Editor deletes itself via `Qt::WA_DeleteOnClose` on accept;
        // spin the loop to match other editor-using tests.
        QCoreApplication::processEvents();
    }

    // The time-range editor must show bounds in the display zone (the
    // zone the table renders) and emit them back unchanged when Ok is
    // clicked without edits. `Asia/Kolkata` (+05:30, no DST) keeps the
    // expectation independent of the host zone.
    void TestFilterEditorTimeRangeRoundTripsInNonUtcDisplayZone()
    {
        const int timeCol = StreamFixtureWithTimeColumnForRowMenuTests();
        QVERIFY2(timeCol >= 0, "timestamp column must exist after streaming");
        const auto *model = mWindow->Model();
        const qint64 row0Micros = model->data(model->index(0, timeCol), LogModelItemDataRole::SortRole).toLongLong();
        const qint64 row2Micros = model->data(model->index(2, timeCol), LogModelItemDataRole::SortRole).toLongLong();
        QVERIFY2(row0Micros > 0 && row2Micros > row0Micros, "fixture rows must carry increasing timestamps");

        std::vector<std::filesystem::path> searched;
        const auto tzdata = loglib::FindTzdataDirectory(
            loglib::DefaultTzdataSearchRoots(logapp::QStringToFsPath(QCoreApplication::applicationDirPath())), searched
        );
        QVERIFY2(!tzdata.empty(), "tzdata must be available next to the test binary");
        const loglib::TimeZoneContext previousZone = loglib::ProcessDefaultTimeZone();
        const QScopeGuard zoneRestorer([&previousZone] { loglib::SetProcessDefaultTimeZone(previousZone); });
        loglib::SetProcessDefaultTimeZone(loglib::TimeZoneContext::Load(tzdata, "Asia/Kolkata"));

        FilterEditor editor(*model, QStringLiteral("tz-round-trip"));
        editor.Load(timeCol, std::optional<qint64>{row0Micros}, std::optional<qint64>{row2Micros});

        const auto *beginDate = editor.findChild<QDateEdit *>(QStringLiteral("beginDateEdit"));
        const auto *beginTime = editor.findChild<QTimeEdit *>(QStringLiteral("beginTimeEdit"));
        const auto *endDate = editor.findChild<QDateEdit *>(QStringLiteral("endDateEdit"));
        const auto *endTime = editor.findChild<QTimeEdit *>(QStringLiteral("endTimeEdit"));
        QVERIFY(beginDate != nullptr);
        QVERIFY(beginTime != nullptr);
        QVERIFY(endDate != nullptr);
        QVERIFY(endTime != nullptr);

        const auto shownWallClock = [](const QDateEdit *date, const QTimeEdit *time) {
            return date->date().toString(QStringLiteral("yyyy-MM-dd")) + QLatin1Char(' ') +
                   time->time().toString(QStringLiteral("HH:mm:ss.zzz"));
        };
        const auto displayZoneWallClock = [](qint64 micros) {
            return QString::fromStdString(
                loglib::ProcessDefaultTimeZone().Format(loglib::TimeStamp{std::chrono::microseconds{micros}}, "%F %T")
            );
        };
        QCOMPARE(shownWallClock(beginDate, beginTime), displayZoneWallClock(row0Micros));
        QCOMPARE(shownWallClock(endDate, endTime), displayZoneWallClock(row2Micros));

        std::optional<qint64> submittedBegin;
        std::optional<qint64> submittedEnd;
        int submissions = 0;
        QObject::connect(
            &editor,
            &FilterEditor::FilterTimeStampSubmitted,
            &editor,
            [&](const QString &, int, std::optional<qint64> begin, std::optional<qint64> end) {
                submittedBegin = begin;
                submittedEnd = end;
                ++submissions;
            }
        );
        auto *ok = editor.findChild<QPushButton *>(QStringLiteral("okButton"));
        QVERIFY(ok != nullptr);
        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): false positive; prior `QVERIFY` aborts on null.
        ok->click();
        QCoreApplication::processEvents();

        QCOMPARE(submissions, 1);
        QVERIFY(submittedBegin.has_value());
        QVERIFY(submittedEnd.has_value());
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        QCOMPARE(*submittedBegin, row0Micros);
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        QCOMPARE(*submittedEnd, row2Micros);
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
        SaveLayoutForTest(*model, savedPath.toStdString());

        // The single-file open path; `DoLoadConfiguration` does the
        // same reset, so both entry points share the contract.
        QVERIFY2(mWindow->TryLoadAsConfigurationForTest(savedPath), "TryLoadAsConfiguration must succeed");
        QCoreApplication::processEvents();

        QVERIFY2(
            !header->isSortIndicatorShown() || header->sortIndicatorSection() == -1,
            "sort indicator must clear after configuration load"
        );
    }

    // Regression: `RebuildFiltersFromConfiguration` opens with
    // `ResetSimpleFilterState`, which disables
    // `actionClearAllFilters`, and only `AddLogFilter` re-enables it.
    // Loading a session whose saved expression is Advanced-only adds
    // no simple leaves, so the action stayed disabled while rows were
    // actively being filtered -- and the status-bar clear button
    // triggers that same action, so it sat visible but inert. There
    // was no route back to an unfiltered view short of a restart.
    void TestAdvancedOnlyLoadEnablesClearAllFilters()
    {
        const int levelCol = StreamFixtureForColumnTests();
        QVERIFY2(levelCol >= 0, "level column must exist after streaming");
        auto *model = mWindow->Model();
        const int msgCol = ColumnByHeader(*model, QStringLiteral("msg"));
        QVERIFY2(msgCol >= 0, "msg column must exist after streaming");
        const std::vector<std::string> msgKeys = model->Layout().Columns()[static_cast<size_t>(msgCol)].keys;

        auto *clearAction = mWindow->findChild<QAction *>(QStringLiteral("actionClearAllFilters"));
        QVERIFY2(clearAction != nullptr, "MainWindow must own actionClearAllFilters");

        // `msg:m1 OR msg:m2` -- an `Or` root, so the load path finds
        // no top-level Leaf to extract into `mSimpleLeaves`.
        std::vector<loglib::FilterExpression> orChildren;
        orChildren.push_back(
            loglib::MakeLeaf(
                loglib::LeafRule{
                    .type = loglib::LeafRule::Type::String,
                    .columnKeys = msgKeys,
                    .matchType = loglib::LeafRule::Match::Contains,
                    .filterString = std::string("m1"),
                }
            )
        );
        orChildren.push_back(
            loglib::MakeLeaf(
                loglib::LeafRule{
                    .type = loglib::LeafRule::Type::String,
                    .columnKeys = msgKeys,
                    .matchType = loglib::LeafRule::Match::Contains,
                    .filterString = std::string("m2"),
                }
            )
        );
        mWindow->activeSession()->MutableView().expression = loglib::MakeOr(std::move(orChildren));

        const QTemporaryDir savedDir;
        QVERIFY(savedDir.isValid());
        const QString savedPath = savedDir.filePath(QStringLiteral("advanced-only.json"));
        mWindow->SaveConfigurationToPathForTest(savedPath);

        // Wipe the runtime expression so the load is the only source
        // of truth. Setting it through the manager leaves the action
        // in its pristine disabled state, which is exactly the
        // pre-load condition that used to survive the load.
        mWindow->activeSession()->MutableView().expression = loglib::FilterExpression{};
        QCoreApplication::processEvents();
        QVERIFY2(!clearAction->isEnabled(), "precondition: action starts disabled with no simple filters");

        mWindow->SetSuppressDialogsForTest(true);
        mWindow->LoadConfigurationFromPathForTest(savedPath);
        QCoreApplication::processEvents();

        QVERIFY2(mWindow->Filters().empty(), "an Advanced-only expression must not populate mSimpleLeaves");
        QVERIFY2(
            !loglib::IsMatchAll(mWindow->activeSession()->View().expression),
            "precondition: the loaded Advanced tree must still be filtering rows"
        );
        QVERIFY2(clearAction->isEnabled(), "Clear All Filters must be enabled after loading an Advanced-only session");
        // The status-bar twin isn't asserted here: a configuration
        // load resets the model, and with no rows to count the whole
        // "N of M rows" status group (button included) is correctly
        // hidden. `TestStatusBarClearButtonVisibleForAdvancedOnlyExpression`
        // covers the button against a populated session.

        // And the affordance must actually work, not just look live.
        clearAction->trigger();
        QCoreApplication::processEvents();
        QVERIFY2(
            loglib::IsMatchAll(mWindow->activeSession()->View().expression),
            "triggering the re-enabled action must clear the loaded Advanced tree"
        );
    }

    // Regression: a follow-up simple-mode edit after the user commits
    // an Advanced `Or` / `Not` tree must not clobber that tree. The
    // trigger is `SyncSessionView`, which runs on
    // every `AddLogFilter` / `ClearFilter`; before the fix it read
    // the existing root, saw non-`And`, and dropped the whole subtree.
    void TestSimpleFilterEditPreservesAdvancedRoot()
    {
        const int levelCol = StreamFixtureForColumnTests();
        QVERIFY2(levelCol >= 0, "level column must exist after streaming");
        auto *model = mWindow->Model();
        const int msgCol = ColumnByHeader(*model, QStringLiteral("msg"));
        QVERIFY2(msgCol >= 0, "msg column must exist after streaming");
        const std::vector<std::string> msgKeys = model->Layout().Columns()[static_cast<size_t>(msgCol)].keys;

        // Install a `NOT msg:m1` Advanced tree exactly as
        // `OpenAdvancedFilter` would after user commit.
        const loglib::FilterExpression advancedTree = loglib::MakeNot(
            loglib::MakeLeaf(
                loglib::LeafRule{
                    .type = loglib::LeafRule::Type::String,
                    .columnKeys = msgKeys,
                    .matchType = loglib::LeafRule::Match::Contains,
                    .filterString = std::string("m1"),
                }
            )
        );
        mWindow->activeSession()->MutableView().expression = advancedTree;

        // A simple-mode leaf submission funnels through `AddLogFilter`
        // -> `SyncSessionView`. The Advanced tree
        // must still be reachable in the resulting configuration.
        QMetaObject::invokeMethod(
            mWindow,
            "FilterSubmitted",
            Qt::DirectConnection,
            Q_ARG(QString, QStringLiteral("simple-after-advanced")),
            Q_ARG(int, msgCol),
            Q_ARG(QString, QStringLiteral("m2")),
            Q_ARG(int, static_cast<int>(loglib::LeafRule::Match::Contains))
        );
        QCoreApplication::processEvents();

        const auto &current = mWindow->activeSession()->View().expression;
        const auto *asAnd = std::get_if<loglib::FilterExpression::And>(&current.node);
        QVERIFY2(asAnd != nullptr, "root must be And after simple-mode edit");
        bool sawAdvancedNot = false;
        bool sawSimpleLeaf = false;
        for (const auto &child : asAnd->children)
        {
            if (const auto *notNode = std::get_if<loglib::FilterExpression::Not>(&child.node); notNode != nullptr)
            {
                QVERIFY(notNode->child != nullptr);
                QVERIFY(*notNode->child == *std::get<loglib::FilterExpression::Not>(advancedTree.node).child);
                sawAdvancedNot = true;
            }
            else if (const auto *leaf = std::get_if<loglib::FilterExpression::Leaf>(&child.node); leaf != nullptr)
            {
                sawSimpleLeaf = true;
                QVERIFY(leaf->rule.filterString.has_value());
                QCOMPARE(*leaf->rule.filterString, std::string("m2"));
            }
        }
        QVERIFY2(sawAdvancedNot, "Advanced NOT subtree must be preserved through the mirror");
        QVERIFY2(sawSimpleLeaf, "Newly submitted simple leaf must be present alongside it");
    }

    // Regression: `Exactly ""` is a valid string predicate that matches
    // genuinely empty values. Once extracted into the simple-mode surface,
    // its title must remain distinguishable from a missing payload.
    void TestAdvancedExactEmptyStringHasAccurateFilterTitle()
    {
        const int levelCol = StreamFixtureForColumnTests();
        QVERIFY2(levelCol >= 0, "level column must exist after streaming");
        auto *model = mWindow->Model();
        const int msgCol = ColumnByHeader(*model, QStringLiteral("msg"));
        QVERIFY2(msgCol >= 0, "msg column must exist after streaming");
        const std::vector<std::string> msgKeys = model->Layout().Columns()[static_cast<size_t>(msgCol)].keys;

        loglib::LeafRule exactEmpty;
        exactEmpty.type = loglib::LeafRule::Type::String;
        exactEmpty.columnKeys = msgKeys;
        exactEmpty.matchType = loglib::LeafRule::Match::Exactly;
        exactEmpty.filterString = std::string{};
        mWindow->CommitAdvancedFilterForTest(loglib::MakeLeaf(std::move(exactEmpty)));
        QCoreApplication::processEvents();

        QCOMPARE(static_cast<int>(mWindow->Filters().size()), 1);
        const auto *filtersMenu = mWindow->findChild<QMenu *>(QStringLiteral("menuFilters"));
        QVERIFY2(filtersMenu != nullptr, "MainWindow must expose its Filters menu");
        bool foundExactEmptyTitle = false;
        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): prior `QVERIFY2` aborts on null.
        for (const QAction *action : filtersMenu->actions())
        {
            if (action->menu() != nullptr && action->text() == QStringLiteral("\"\""))
            {
                foundExactEmptyTitle = true;
                break;
            }
        }
        QVERIFY2(foundExactEmptyTitle, "exact-empty filter menu entry must render as two quotes");

        const QString tooltip = model->headerData(msgCol, Qt::Horizontal, Qt::ToolTipRole).toString();
        QVERIFY2(
            tooltip.contains(QStringLiteral("&quot;&quot;")),
            "column tooltip must contain the HTML-escaped exact-empty title"
        );
        QVERIFY2(
            !tooltip.contains(QStringLiteral("(unset)")), "exact-empty filter must not look like a missing payload"
        );
    }

    // Companion to the bare-Leaf case above: an Advanced commit whose
    // root is `And{[Leaf, Leaf, Or{...}]}` must extract the Leaves
    // into `mSimpleLeaves` and keep only the Or subtree as the
    // non-Leaf remainder, so the Filters-menu entry count matches
    // the extracted-leaf count and Mirror preserves the Or on a
    // follow-up edit.
    void TestAdvancedFilterMixedTreeExtractsSimpleLeaves()
    {
        const int levelCol = StreamFixtureForColumnTests();
        QVERIFY2(levelCol >= 0, "level column must exist after streaming");
        auto *model = mWindow->Model();
        const int msgCol = ColumnByHeader(*model, QStringLiteral("msg"));
        QVERIFY2(msgCol >= 0, "msg column must exist after streaming");
        const std::vector<std::string> msgKeys = model->Layout().Columns()[static_cast<size_t>(msgCol)].keys;
        const std::vector<std::string> levelKeys = model->Layout().Columns()[static_cast<size_t>(levelCol)].keys;

        // Build `msg:m1 AND msg:m2 AND (msg:m3 OR msg:m4)` -- two
        // representable-as-simple leaves plus one non-Leaf child.
        loglib::FilterExpression leafM1 = loglib::MakeLeaf(
            loglib::LeafRule{
                .type = loglib::LeafRule::Type::String,
                .columnKeys = msgKeys,
                .matchType = loglib::LeafRule::Match::Contains,
                .filterString = std::string("m1"),
            }
        );
        loglib::FilterExpression leafM2 = loglib::MakeLeaf(
            loglib::LeafRule{
                .type = loglib::LeafRule::Type::String,
                .columnKeys = msgKeys,
                .matchType = loglib::LeafRule::Match::Contains,
                .filterString = std::string("m2"),
            }
        );
        std::vector<loglib::FilterExpression> orChildren;
        orChildren.push_back(
            loglib::MakeLeaf(
                loglib::LeafRule{
                    .type = loglib::LeafRule::Type::String,
                    .columnKeys = msgKeys,
                    .matchType = loglib::LeafRule::Match::Contains,
                    .filterString = std::string("m3"),
                }
            )
        );
        orChildren.push_back(
            loglib::MakeLeaf(
                loglib::LeafRule{
                    .type = loglib::LeafRule::Type::String,
                    .columnKeys = msgKeys,
                    .matchType = loglib::LeafRule::Match::Contains,
                    .filterString = std::string("m4"),
                }
            )
        );
        loglib::FilterExpression orNode = loglib::MakeOr(std::move(orChildren));
        std::vector<loglib::FilterExpression> andChildren;
        andChildren.push_back(std::move(leafM1));
        andChildren.push_back(std::move(leafM2));
        andChildren.push_back(std::move(orNode));

        mWindow->CommitAdvancedFilterForTest(loglib::MakeAnd(std::move(andChildren)));
        QCoreApplication::processEvents();

        // Two Filters-menu entries (the extracted Leaves); the Or
        // stays under the wire as a non-Leaf child.
        QCOMPARE(static_cast<int>(mWindow->Filters().size()), 2);

        const auto &current = mWindow->activeSession()->View().expression;
        const auto *asAnd = std::get_if<loglib::FilterExpression::And>(&current.node);
        QVERIFY2(asAnd != nullptr, "root must be And after Advanced commit of a mixed tree");
        QCOMPARE(static_cast<int>(asAnd->children.size()), 3);
        int seenLeaves = 0;
        int seenOr = 0;
        for (const auto &child : asAnd->children)
        {
            if (std::holds_alternative<loglib::FilterExpression::Leaf>(child.node))
            {
                ++seenLeaves;
            }
            else if (std::holds_alternative<loglib::FilterExpression::Or>(child.node))
            {
                ++seenOr;
            }
        }
        QCOMPARE(seenLeaves, 2);
        QCOMPARE(seenOr, 1);
    }

    // Companion to the button-visibility fix: removing the last
    // simple-mode leaf via `ClearFilter` used to count Filters-menu
    // entries and disable `actionClearAllFilters` when the count hit
    // zero, ignoring any Advanced-mode subtree still installed on
    // `LogConfiguration::expression`. That left the action disabled
    // (and, via the shared trigger, the status-bar button wired to a
    // no-op) even though rows stayed filtered by the Advanced Or/Not
    // remainder. The fix gates on `IsMatchAll(expression)` post-Mirror.
    void TestClearFilterKeepsActionEnabledWithAdvancedRemainder()
    {
        const int levelCol = StreamFixtureForColumnTests();
        QVERIFY2(levelCol >= 0, "level column must exist after streaming");
        auto *model = mWindow->Model();
        const int msgCol = ColumnByHeader(*model, QStringLiteral("msg"));
        QVERIFY2(msgCol >= 0, "msg column must exist after streaming");
        const std::vector<std::string> msgKeys = model->Layout().Columns()[static_cast<size_t>(msgCol)].keys;

        auto *clearAction = mWindow->findChild<QAction *>(QStringLiteral("actionClearAllFilters"));
        QVERIFY2(clearAction != nullptr, "MainWindow must own actionClearAllFilters");
        auto *button = mWindow->findChild<QPushButton *>(QStringLiteral("clearFiltersStatusButton"));
        QVERIFY2(button != nullptr, "MainWindow must own the clear-filters status button");

        // Commit a mixed tree: one simple leaf + one Or subtree.
        // After `ApplyAdvancedFilterResult` this is one entry in
        // `mSimpleLeaves` (the Leaf) plus one Or child on the wire
        // expression -- the same shape a user would produce by
        // typing `msg:m1 AND (msg:m2 OR msg:m3)` in the editor.
        std::vector<loglib::FilterExpression> orChildren;
        orChildren.push_back(
            loglib::MakeLeaf(
                loglib::LeafRule{
                    .type = loglib::LeafRule::Type::String,
                    .columnKeys = msgKeys,
                    .matchType = loglib::LeafRule::Match::Contains,
                    .filterString = std::string("m2"),
                }
            )
        );
        orChildren.push_back(
            loglib::MakeLeaf(
                loglib::LeafRule{
                    .type = loglib::LeafRule::Type::String,
                    .columnKeys = msgKeys,
                    .matchType = loglib::LeafRule::Match::Contains,
                    .filterString = std::string("m3"),
                }
            )
        );
        std::vector<loglib::FilterExpression> andChildren;
        andChildren.push_back(
            loglib::MakeLeaf(
                loglib::LeafRule{
                    .type = loglib::LeafRule::Type::String,
                    .columnKeys = msgKeys,
                    .matchType = loglib::LeafRule::Match::Contains,
                    .filterString = std::string("m1"),
                }
            )
        );
        andChildren.push_back(loglib::MakeOr(std::move(orChildren)));
        mWindow->CommitAdvancedFilterForTest(loglib::MakeAnd(std::move(andChildren)));
        QCoreApplication::processEvents();

        QCOMPARE(static_cast<int>(mWindow->Filters().size()), 1);
        QVERIFY2(clearAction->isEnabled(), "precondition: action must be enabled with a mixed Advanced tree");
        QVERIFY2(!button->isHidden(), "precondition: button must be visible with a mixed Advanced tree");

        // Remove the sole simple leaf. `mSimpleLeaves` empties, but
        // the Or child stays on the expression -- the action / button
        // must not react as if all filters are gone.
        const QString simpleLeafId = QString::fromStdString(mWindow->Filters().begin()->first);
        QMetaObject::invokeMethod(
            mWindow, "ClearFilter", Qt::DirectConnection, Q_ARG(QString, simpleLeafId), Q_ARG(bool, false)
        );
        QCoreApplication::processEvents();

        QVERIFY2(mWindow->Filters().empty(), "simple leaf must be gone after ClearFilter");
        QVERIFY2(
            !loglib::IsMatchAll(mWindow->activeSession()->View().expression),
            "the Advanced Or subtree must survive ClearFilter of the sibling simple leaf"
        );
        QVERIFY2(
            clearAction->isEnabled(),
            "actionClearAllFilters must stay enabled while the Advanced Or subtree is still filtering rows"
        );
        QVERIFY2(
            !button->isHidden(),
            "clear-filters status button must stay visible while the Advanced Or subtree is still filtering rows"
        );
    }

    // Regression: `Clear All Filters` is the user's explicit signal
    // that they want every row visible. Before the fix,
    // `ClearAllFilters` cleared `mSimpleLeaves` and then called
    // `SyncSessionView`, which preserves any
    // pre-existing Advanced-mode Or/Not root -- so an Advanced tree
    // committed via the editor stayed active with no UI cue (menu
    // showed "(no filters)", funnel decorations were gone, yet rows
    // were still being filtered out). The fix resets the whole
    // expression before mirroring; the Advanced editor must be
    // reopened to reintroduce a boolean tree.
    void TestClearAllFiltersDropsAdvancedTree()
    {
        const int levelCol = StreamFixtureForColumnTests();
        QVERIFY2(levelCol >= 0, "level column must exist after streaming");
        auto *model = mWindow->Model();
        const int msgCol = ColumnByHeader(*model, QStringLiteral("msg"));
        QVERIFY2(msgCol >= 0, "msg column must exist after streaming");
        const std::vector<std::string> msgKeys = model->Layout().Columns()[static_cast<size_t>(msgCol)].keys;

        // Install a `NOT msg:m1` Advanced tree exactly as
        // `OpenAdvancedFilter` would after user commit.
        const loglib::FilterExpression advancedTree = loglib::MakeNot(
            loglib::MakeLeaf(
                loglib::LeafRule{
                    .type = loglib::LeafRule::Type::String,
                    .columnKeys = msgKeys,
                    .matchType = loglib::LeafRule::Match::Contains,
                    .filterString = std::string("m1"),
                }
            )
        );
        mWindow->activeSession()->MutableView().expression = advancedTree;
        QCoreApplication::processEvents();
        QVERIFY2(
            !loglib::IsMatchAll(mWindow->activeSession()->View().expression),
            "precondition: Advanced tree must be live before ClearAllFilters"
        );

        QMetaObject::invokeMethod(mWindow, "ClearAllFilters", Qt::DirectConnection);
        QCoreApplication::processEvents();

        QVERIFY2(
            loglib::IsMatchAll(mWindow->activeSession()->View().expression),
            "ClearAllFilters must reset the expression to match-all even when the "
            "root is an Advanced Or/Not/And-with-non-Leaf subtree"
        );
        QVERIFY2(mWindow->Filters().empty(), "simple-mode leaves must be drained too");
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
            Q_ARG(int, static_cast<int>(loglib::LeafRule::Match::Contains))
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
            Q_ARG(int, static_cast<int>(loglib::LeafRule::Match::Contains))
        );
        QMetaObject::invokeMethod(
            mWindow,
            "FilterSubmitted",
            Qt::DirectConnection,
            Q_ARG(QString, QStringLiteral("hf-apple")),
            Q_ARG(int, msgCol),
            Q_ARG(QString, QStringLiteral("apple")),
            Q_ARG(int, static_cast<int>(loglib::LeafRule::Match::Contains))
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
            Q_ARG(int, static_cast<int>(loglib::LeafRule::Match::Contains))
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
            Q_ARG(int, static_cast<int>(loglib::LeafRule::Match::Contains))
        );
        QCoreApplication::processEvents();
        const QVariant funnelOnlyVariant = model->headerData(msgCol, Qt::Horizontal, Qt::DecorationRole);
        QVERIFY(funnelOnlyVariant.canConvert<QIcon>());
        const auto funnelOnly = funnelOnlyVariant.value<QIcon>();
        QVERIFY2(!funnelOnly.isNull(), "filter-only decoration must be the funnel icon");
        const QImage funnelOnlyImage = funnelOnly.pixmap(16, 16).toImage();

        model->MutableLayout().SetColumnAutoDetect(static_cast<size_t>(msgCol), false);
        model->MutableLayout().SetColumnType(static_cast<size_t>(msgCol), loglib::ColumnType::Integer);
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
            Q_ARG(int, static_cast<int>(loglib::LeafRule::Match::Contains))
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
            auto fileSource = std::make_unique<loglib::FileLineSource>(emptyFixture.Path().toStdString());
            loglib::FileLineSource *sourcePtr = fileSource.get();
            (void)model.BeginStreamingForSyncTest(std::move(fileSource));

            loglib::KeyIndex &keys = model.Table().Keys();
            const auto makeLine = [&](const std::string &value) {
                std::vector<std::pair<loglib::KeyId, loglib::LogValue>> values;
                values.emplace_back(keys.GetOrInsert("category"), loglib::LogValue(value));
                return loglib::LogLine{std::move(values), *sourcePtr, 0};
            };

            loglib::RowBatch batch;
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
            modelA.Table().Keys().Find(modelA.Layout().Columns()[static_cast<size_t>(levelColA)].keys.front());
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
            modelB.Table().Keys().Find(modelB.Layout().Columns()[static_cast<size_t>(levelColB)].keys.front());
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
            auto fileSource = std::make_unique<loglib::FileLineSource>(emptyFixture.Path().toStdString());
            loglib::FileLineSource *sourcePtr = fileSource.get();
            (void)model.BeginStreamingForSyncTest(std::move(fileSource));

            loglib::KeyIndex &keys = model.Table().Keys();
            const auto makeLine = [&](const std::string &value) {
                std::vector<std::pair<loglib::KeyId, loglib::LogValue>> values;
                values.emplace_back(keys.GetOrInsert("category"), loglib::LogValue(value));
                return loglib::LogLine{std::move(values), *sourcePtr, 0};
            };
            loglib::RowBatch batch;
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

        auto fileSource = std::make_unique<loglib::FileLineSource>(fixture.Path().toStdString());
        loglib::FileLineSource *fileSourcePtr = fileSource.get();
        const loglib::StopToken stopToken = model->BeginStreamingForSyncTest(std::move(fileSource));

        loglib::ParserOptions options;
        options.stopToken = stopToken;
        options.threads = 1;
        const loglib::JsonParser parser;
        parser.Parse(*fileSourcePtr, *model->Sink(), options);
        QVERIFY2(finishedSpy.count() > 0 || finishedSpy.wait(60000), "streamingFinished must arrive");
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
                Q_ARG(int, static_cast<int>(loglib::LeafRule::Match::RegularExpression))
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

        auto fileSource = std::make_unique<loglib::FileLineSource>(fixture.Path().toStdString());
        loglib::FileLineSource *fileSourcePtr = fileSource.get();
        const loglib::StopToken stopToken = model->BeginStreamingForSyncTest(std::move(fileSource));

        loglib::ParserOptions options;
        options.stopToken = stopToken;
        options.threads = 1;
        const loglib::JsonParser parser;
        parser.Parse(*fileSourcePtr, *model->Sink(), options);
        QVERIFY2(finishedSpy.count() > 0 || finishedSpy.wait(60000), "streamingFinished must arrive");
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

        const auto submitFilter = [&](const QString &filterId, const QString &pattern, loglib::LeafRule::Match match) {
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
            QStringLiteral("contains-displayed"), QStringLiteral("line1 line2"), loglib::LeafRule::Match::Contains
        );
        QCOMPARE(filterModel->rowCount(), 1);
        clearFilter(QStringLiteral("contains-displayed"));

        // Exactly: full simplified value.
        submitFilter(
            QStringLiteral("exact-displayed"), QStringLiteral("line1 line2"), loglib::LeafRule::Match::Exactly
        );
        QCOMPARE(filterModel->rowCount(), 1);
        clearFilter(QStringLiteral("exact-displayed"));

        // RegularExpression: `^line1 line2$` only spans the simplified
        // form -- `^...$` without `MultilineOption` won't span a `\n`.
        submitFilter(
            QStringLiteral("regex-displayed"),
            QStringLiteral("^line1 line2$"),
            loglib::LeafRule::Match::RegularExpression
        );
        QCOMPARE(filterModel->rowCount(), 1);
        clearFilter(QStringLiteral("regex-displayed"));

        // Wildcard: Qt-flavoured `line1 *2` only matches the
        // simplified single-line form.
        submitFilter(
            QStringLiteral("wildcard-displayed"), QStringLiteral("line1 *2"), loglib::LeafRule::Match::Wildcard
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

        auto fileSource = std::make_unique<loglib::FileLineSource>(fixture.Path().toStdString());
        loglib::FileLineSource *fileSourcePtr = fileSource.get();
        const loglib::StopToken stopToken = model->BeginStreamingForSyncTest(std::move(fileSource));

        loglib::ParserOptions options;
        options.stopToken = stopToken;
        options.threads = 1;
        const loglib::JsonParser parser;
        parser.Parse(*fileSourcePtr, *model->Sink(), options);
        QVERIFY2(finishedSpy.count() > 0 || finishedSpy.wait(60000), "streamingFinished must arrive");
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

        auto fileSource = std::make_unique<loglib::FileLineSource>(fixture.Path().toStdString());
        loglib::FileLineSource *fileSourcePtr = fileSource.get();
        const loglib::StopToken stopToken = model->BeginStreamingForSyncTest(std::move(fileSource));

        loglib::ParserOptions options;
        options.stopToken = stopToken;
        options.threads = 1;
        const loglib::JsonParser parser;
        parser.Parse(*fileSourcePtr, *model->Sink(), options);
        QVERIFY2(finishedSpy.count() > 0 || finishedSpy.wait(60000), "streamingFinished must arrive");
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

    // Regression: Find Next must land on every consecutive match.
    // A double-advance (e.g. FindRecords firing twice per Enter /
    // click, or skipFirstN skipping a row and a match) presents as
    // "every other hit is skipped".
    void TestFindNextVisitsEveryConsecutiveMatch()
    {
        // Every row contains "needle" so matches are consecutive
        // proxy rows 0..N-1 — the strongest repro for skip-by-two.
        constexpr int FIXTURE_LINES = 8;
        QStringList lines;
        lines.reserve(FIXTURE_LINES);
        for (int i = 0; i < FIXTURE_LINES; ++i)
        {
            lines.append(QStringLiteral(R"({"msg": "needle row%1"})").arg(i));
        }
        const TempJsonFile fixture(lines);

        auto *model = mWindow->findChild<LogModel *>();
        QVERIFY2(model != nullptr, "MainWindow must own a LogModel");

        QSignalSpy finishedSpy(model, &LogModel::streamingFinished);
        QVERIFY(finishedSpy.isValid());

        auto fileSource = std::make_unique<loglib::FileLineSource>(fixture.Path().toStdString());
        loglib::FileLineSource *fileSourcePtr = fileSource.get();
        const loglib::StopToken stopToken = model->BeginStreamingForSyncTest(std::move(fileSource));

        loglib::ParserOptions options;
        options.stopToken = stopToken;
        options.threads = 1;
        const loglib::JsonParser parser;
        parser.Parse(*fileSourcePtr, *model->Sink(), options);
        QVERIFY2(finishedSpy.count() > 0 || finishedSpy.wait(60000), "streamingFinished must arrive");
        QCOMPARE(model->rowCount(), FIXTURE_LINES);

        auto *tableView = mWindow->findChild<LogTableView *>();
        QVERIFY2(tableView != nullptr, "MainWindow must own a LogTableView");
        const LogFilterModel *filterModel = mWindow->FilterModel();
        QVERIFY2(filterModel != nullptr, "MainWindow must own a LogFilterModel proxy");
        QCOMPARE(filterModel->rowCount(), FIXTURE_LINES);

        // Clear any inherited selection so the first Next starts at
        // row 0 with skipFirstN==0.
        tableView->selectionModel()->clearSelection();
        tableView->selectionModel()->setCurrentIndex(filterModel->index(0, 0), QItemSelectionModel::NoUpdate);

        QList<int> visited;
        visited.reserve(FIXTURE_LINES);
        for (int step = 0; step < FIXTURE_LINES; ++step)
        {
            QVERIFY2(
                QMetaObject::invokeMethod(
                    mWindow,
                    "FindRecords",
                    Qt::DirectConnection,
                    Q_ARG(QString, QStringLiteral("needle")),
                    Q_ARG(bool, true),
                    Q_ARG(bool, false),
                    Q_ARG(bool, false)
                ),
                "FindRecords slot must be invocable via meta-object"
            );
            const QModelIndex selected = tableView->selectionModel()->currentIndex();
            QVERIFY2(selected.isValid(), "FindRecords must select a match");
            visited.append(selected.row());
        }

        QList<int> expected;
        expected.reserve(FIXTURE_LINES);
        for (int i = 0; i < FIXTURE_LINES; ++i)
        {
            expected.append(i);
        }
        QCOMPARE(visited, expected);

        model->EndStreaming(false);
    }

    // Same consecutive-match walk, but driven through the find bar's
    // Enter / Next-button signals so a double-emit of `FindRecords`
    // cannot hide behind direct slot invocation.
    void TestFindNextViaUiDoesNotDoubleAdvance()
    {
        constexpr int FIXTURE_LINES = 6;
        QStringList lines;
        lines.reserve(FIXTURE_LINES);
        for (int i = 0; i < FIXTURE_LINES; ++i)
        {
            lines.append(QStringLiteral(R"({"msg": "needle row%1"})").arg(i));
        }
        const TempJsonFile fixture(lines);

        auto *model = mWindow->findChild<LogModel *>();
        QVERIFY2(model != nullptr, "MainWindow must own a LogModel");

        QSignalSpy finishedSpy(model, &LogModel::streamingFinished);
        QVERIFY(finishedSpy.isValid());

        auto fileSource = std::make_unique<loglib::FileLineSource>(fixture.Path().toStdString());
        loglib::FileLineSource *fileSourcePtr = fileSource.get();
        const loglib::StopToken stopToken = model->BeginStreamingForSyncTest(std::move(fileSource));

        loglib::ParserOptions options;
        options.stopToken = stopToken;
        options.threads = 1;
        const loglib::JsonParser parser;
        parser.Parse(*fileSourcePtr, *model->Sink(), options);
        QVERIFY2(finishedSpy.count() > 0 || finishedSpy.wait(60000), "streamingFinished must arrive");

        auto *findRecord = mWindow->findChild<FindRecordWidget *>();
        QVERIFY2(findRecord != nullptr, "MainWindow must own a FindRecordWidget");
        auto *edit = findRecord->findChild<QLineEdit *>(QStringLiteral("findEdit"));
        auto *nextButton = findRecord->findChild<QToolButton *>(QStringLiteral("findNext"));
        QVERIFY2(edit != nullptr && nextButton != nullptr, "find edit + next button must exist");

        auto *tableView = mWindow->findChild<LogTableView *>();
        QVERIFY2(tableView != nullptr, "MainWindow must own a LogTableView");
        const LogFilterModel *filterModel = mWindow->FilterModel();
        QVERIFY2(filterModel != nullptr, "MainWindow must own a LogFilterModel proxy");

        tableView->selectionModel()->clearSelection();
        tableView->selectionModel()->setCurrentIndex(filterModel->index(0, 0), QItemSelectionModel::NoUpdate);

        edit->setText(QStringLiteral("needle"));
        QSignalSpy findSpy(findRecord, &FindRecordWidget::FindRecords);
        QVERIFY(findSpy.isValid());

        // Enter in the search field must emit FindRecords exactly once
        // and advance by exactly one match.
        edit->setFocus();
        QTest::keyClick(edit, Qt::Key_Return);
        QCOMPARE(findSpy.count(), 1);
        QCOMPARE(tableView->selectionModel()->currentIndex().row(), 0);

        // Next button: same single-step contract.
        findSpy.clear();
        QTest::mouseClick(nextButton, Qt::LeftButton);
        QCOMPARE(findSpy.count(), 1);
        QCOMPARE(tableView->selectionModel()->currentIndex().row(), 1);

        // Enter again from the edit field after the button click
        // (focus may have moved) — still one step, not a double jump.
        findSpy.clear();
        edit->setFocus();
        QTest::keyClick(edit, Qt::Key_Return);
        QCOMPARE(findSpy.count(), 1);
        QCOMPARE(tableView->selectionModel()->currentIndex().row(), 2);

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

        auto fileSource = std::make_unique<loglib::FileLineSource>(fixture.Path().toStdString());
        loglib::FileLineSource *fileSourcePtr = fileSource.get();
        const loglib::StopToken stopToken = model->BeginStreamingForSyncTest(std::move(fileSource));

        loglib::ParserOptions options;
        options.stopToken = stopToken;
        options.threads = 1;
        const loglib::JsonParser parser;
        parser.Parse(*fileSourcePtr, *model->Sink(), options);
        QVERIFY2(finishedSpy.count() > 0 || finishedSpy.wait(60000), "streamingFinished must arrive");
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
            qPrintable(QStringLiteral(
                           "regex/wildcards toggles must precede the arrow buttons in the tab chain "
                           "(regex=%1 wildcards=%2 prev=%3)"
            )
                           .arg(regexIdx)
                           .arg(wildcardsIdx)
                           .arg(prevIdx))
        );
        QVERIFY2(prevIdx < nextIdx, "previous-arrow must come before next-arrow in the tab chain");
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
            spy.count() > 0 || spy.wait(60000),
            "FindDock::revealed must trigger a debounced MatchCountRequested for a non-empty needle"
        );

        // Empty needle: the bar suppresses the request.
        edit->clear();
        spy.clear();
        emit dock->revealed();
        // Absence: the debounce is 120 ms and nothing marks the end of a quiet period, so wait past it and check that
        // no request came.
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
        QVERIFY2(spy.count() > 0 || spy.wait(60000), "initial textChanged must emit");
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
        // A stalled machine delays the emit's delivery, not the cap.
        qint64 oversleptMs = 0;
        while (wallClock.elapsed() - oversleptMs < TOTAL_BUMP_DURATION_MS)
        {
            findRecord->BumpMatchCountDebounce();
            const qint64 beforeWait = wallClock.elapsed();
            // Models continuous activity: a bump every 50 ms, faster than the trailing debounce, which is the
            // input the max-age cap exists for. The pace is the input and not a wait for an event.
            QTest::qWait(BUMP_INTERVAL_MS);
            oversleptMs += std::max<qint64>(0, wallClock.elapsed() - beforeWait - BUMP_INTERVAL_MS);
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
            wallClock.elapsed() - oversleptMs <= MAX_AGE_TOLERANCE_MS,
            qPrintable(QStringLiteral("first emit took longer than the max-age cap: %1 ms").arg(wallClock.elapsed()))
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

    // Regression: `SetMatchInfo(overflowed=true)` used to only
    // append "+" to the visible total, which was ambiguous
    // between "the total is a lower bound (scan bailed)" and
    // "the total is exact but cursor-position lookup is capped".
    // The label now installs a tooltip that spells both effects
    // out; `overflowed=false` must clear the tooltip so a
    // narrower search doesn't inherit a stale hover from the
    // previous broad one. Pins both edges.
    void TestSetMatchInfoOverflowedInstallsTooltip()
    {
        auto *findRecord = mWindow->findChild<FindRecordWidget *>();
        QVERIFY2(findRecord != nullptr, "MainWindow must own a FindRecordWidget");
        auto *label = findRecord->findChild<QLabel *>(QStringLiteral("findMatchCount"));
        QVERIFY2(label != nullptr, "FindRecordWidget must expose its match-count label");

        // Baseline: no matches, tooltip clear.
        findRecord->SetMatchInfo(0, 0);
        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): prior QVERIFY2 aborts on null.
        QVERIFY2(label->toolTip().isEmpty(), "empty result must clear the tooltip");

        // Exact count (not overflowed): visible text has no "+"
        // and no tooltip.
        findRecord->SetMatchInfo(3, 42, /*overflowed=*/false);
        QVERIFY2(!label->text().contains(QChar('+')), "non-overflowed total must not carry a '+' suffix");
        QVERIFY2(label->toolTip().isEmpty(), "non-overflowed match info must not install a tooltip");

        // Overflowed: visible text keeps "+" (lower-bound cue),
        // AND the tooltip explains what the "+" means and calls
        // out the cursor-position degradation. Keyword probes
        // rather than exact-text matches so translation drift or
        // wording tweaks don't tank the test.
        findRecord->SetMatchInfo(0, 25000, /*overflowed=*/true);
        QVERIFY2(label->text().contains(QChar('+')), "overflowed total must retain the '+' lower-bound cue");
        const QString tooltip = label->toolTip();
        QVERIFY2(!tooltip.isEmpty(), "overflowed match info must install an explanatory tooltip");
        QVERIFY2(
            tooltip.contains(QStringLiteral("lower bound"), Qt::CaseInsensitive) ||
                tooltip.contains(QStringLiteral("bails"), Qt::CaseInsensitive),
            qPrintable(QStringLiteral(
                           "tooltip should mention the scan's early-exit / lower-bound "
                           "nature so the user can interpret the '+' correctly: %1"
            )
                           .arg(tooltip))
        );
        QVERIFY2(
            tooltip.contains(QStringLiteral("current"), Qt::CaseInsensitive) ||
                tooltip.contains(QStringLiteral("cursor"), Qt::CaseInsensitive) ||
                tooltip.contains(QStringLiteral("index"), Qt::CaseInsensitive),
            qPrintable(
                QStringLiteral("tooltip should also mention the cursor / current-match degradation: %1").arg(tooltip)
            )
        );

        // Narrowing the search (overflow gone) must not leave the
        // previous tooltip stranded on the label.
        findRecord->SetMatchInfo(1, 5, /*overflowed=*/false);
        QVERIFY2(label->toolTip().isEmpty(), "narrower search must clear the stale overflow tooltip");
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
                Q_ARG(int, static_cast<int>(loglib::LeafRule::Match::Contains))
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
                Q_ARG(int, static_cast<int>(loglib::LeafRule::Match::Contains))
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
        model->MutableLayout().SetColumnAutoDetect(static_cast<size_t>(msgCol), false);
        model->MutableLayout().SetColumnType(static_cast<size_t>(msgCol), loglib::ColumnType::Integer);
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

        model->MutableLayout().SetColumnAutoDetect(static_cast<size_t>(msgCol), false);
        model->MutableLayout().SetColumnType(static_cast<size_t>(msgCol), loglib::ColumnType::Integer);
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

        auto fileSource = std::make_unique<loglib::FileLineSource>(fixture.Path().toStdString());
        loglib::FileLineSource *fileSourcePtr = fileSource.get();
        const loglib::StopToken stopToken = model->BeginStreamingForSyncTest(std::move(fileSource));

        loglib::ParserOptions options;
        options.stopToken = stopToken;
        options.threads = 1;
        const loglib::JsonParser parser;
        parser.Parse(*fileSourcePtr, *model->Sink(), options);
        QVERIFY2(finishedSpy.count() > 0 || finishedSpy.wait(60000), "streamingFinished must arrive");
        QCOMPARE(model->rowCount(), FIXTURE_LINES);

        const int valueCol = ColumnByHeader(*model, QStringLiteral("value"));
        QVERIFY2(valueCol >= 0, "value column must exist");
        const auto &columns = model->Layout().Columns();
        QCOMPARE(columns[static_cast<size_t>(valueCol)].type, loglib::ColumnType::Integer);

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

        auto fileSource = std::make_unique<loglib::FileLineSource>(fixture.Path().toStdString());
        loglib::FileLineSource *fileSourcePtr = fileSource.get();
        const loglib::StopToken stopToken = model->BeginStreamingForSyncTest(std::move(fileSource));

        loglib::ParserOptions options;
        options.stopToken = stopToken;
        options.threads = 1;
        const loglib::JsonParser parser;
        parser.Parse(*fileSourcePtr, *model->Sink(), options);
        QVERIFY2(finishedSpy.count() > 0 || finishedSpy.wait(60000), "streamingFinished must arrive");

        const int flagCol = ColumnByHeader(*model, QStringLiteral("flag"));
        QVERIFY2(flagCol >= 0, "flag column must exist");
        const auto &columns = model->Layout().Columns();
        QCOMPARE(columns[static_cast<size_t>(flagCol)].type, loglib::ColumnType::Boolean);

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

        auto fileSource = std::make_unique<loglib::FileLineSource>(fixture.Path().toStdString());
        loglib::FileLineSource *fileSourcePtr = fileSource.get();
        const loglib::StopToken stopToken = model->BeginStreamingForSyncTest(std::move(fileSource));

        loglib::ParserOptions options;
        options.stopToken = stopToken;
        options.threads = 1;
        const loglib::JsonParser parser;
        parser.Parse(*fileSourcePtr, *model->Sink(), options);
        QVERIFY2(finishedSpy.count() > 0 || finishedSpy.wait(60000), "streamingFinished must arrive");

        const int valueCol = ColumnByHeader(*model, QStringLiteral("value"));
        QVERIFY2(valueCol >= 0, "value column must exist");

        // Serialise the saved leaf and round-trip it through glaze
        // to confirm the wire-format keys survive (mirrors the
        // lib-side tests for `Type::Number`). Leaf binds by
        // `columnKeys`; resolve the `value` column's keys once.
        const std::vector<std::string> valueKeys = model->Layout().Columns()[static_cast<size_t>(valueCol)].keys;

        loglib::LogConfiguration cfg;
        loglib::LeafRule leaf;
        leaf.type = loglib::LeafRule::Type::Number;
        leaf.columnKeys = valueKeys;
        leaf.filterMinValue = 10.0;
        leaf.filterMaxValue = 19.0;
        cfg.view.expression = WireLeavesAsExpression({leaf});

        const QTemporaryDir roundTripDir;
        QVERIFY(roundTripDir.isValid());
        const QString roundTripPath = roundTripDir.filePath(QStringLiteral("roundtrip.json"));
        loglib::SaveConfiguration(cfg, roundTripPath.toStdString(), loglib::SaveScope::Full);
        const loglib::LogConfiguration loaded = loglib::LoadConfiguration(roundTripPath.toStdString());
        const auto loadedLeaves = WireLeavesOf(loaded.view.expression);
        QCOMPARE(loadedLeaves.size(), static_cast<size_t>(1));
        QCOMPARE(loadedLeaves[0].type, loglib::LeafRule::Type::Number);
        QVERIFY(loadedLeaves[0].filterMinValue.has_value());
        QVERIFY(loadedLeaves[0].filterMaxValue.has_value());

        // Replay the round-tripped leaf through `AddFilter`. The
        // type-match check accepts (Number vs Integer column) and
        // the editor opens preloaded with the bounds.
        mWindow->statusBar()->clearMessage();
        const QString filterId = QStringLiteral("saved-numeric-roundtrip");
        QVERIFY2(
            QMetaObject::invokeMethod(
                mWindow,
                "AddFilter",
                Qt::DirectConnection,
                Q_ARG(QString, filterId),
                Q_ARG(std::optional<loglib::LeafRule>, loadedLeaves[0])
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

        auto fileSource = std::make_unique<loglib::FileLineSource>(fixture.Path().toStdString());
        loglib::FileLineSource *fileSourcePtr = fileSource.get();
        const loglib::StopToken stopToken = model->BeginStreamingForSyncTest(std::move(fileSource));

        loglib::ParserOptions options;
        options.stopToken = stopToken;
        options.threads = 1;
        const loglib::JsonParser parser;
        parser.Parse(*fileSourcePtr, *model->Sink(), options);
        QVERIFY2(finishedSpy.count() > 0 || finishedSpy.wait(60000), "streamingFinished must arrive");

        const int levelCol = ColumnByHeader(*model, QStringLiteral("category"));
        QVERIFY2(levelCol >= 0, "level column must exist");
        QCOMPARE(model->Layout().Columns()[static_cast<size_t>(levelCol)].type, loglib::ColumnType::Enumeration);

        loglib::LeafRule savedFilter;
        savedFilter.type = loglib::LeafRule::Type::Number;
        savedFilter.columnKeys = model->Layout().Columns()[static_cast<size_t>(levelCol)].keys;
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
                Q_ARG(std::optional<loglib::LeafRule>, savedFilter)
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

        auto fileSource = std::make_unique<loglib::FileLineSource>(fixture.Path().toStdString());
        loglib::FileLineSource *fileSourcePtr = fileSource.get();
        const loglib::StopToken stopToken = model->BeginStreamingForSyncTest(std::move(fileSource));

        loglib::ParserOptions options;
        options.stopToken = stopToken;
        options.threads = 1;
        const loglib::JsonParser parser;
        parser.Parse(*fileSourcePtr, *model->Sink(), options);
        QVERIFY2(finishedSpy.count() > 0 || finishedSpy.wait(60000), "streamingFinished must arrive");

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

        auto fileSource = std::make_unique<loglib::FileLineSource>(fixture.Path().toStdString());
        loglib::FileLineSource *fileSourcePtr = fileSource.get();
        const loglib::StopToken stopToken = model->BeginStreamingForSyncTest(std::move(fileSource));

        loglib::ParserOptions options;
        options.stopToken = stopToken;
        options.threads = 1;
        const loglib::JsonParser parser;
        parser.Parse(*fileSourcePtr, *model->Sink(), options);
        QVERIFY2(finishedSpy.count() > 0 || finishedSpy.wait(60000), "streamingFinished must arrive");

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

            loglib::LogConfiguration builder;
            {
                loglib::ColumnLayout builderLayout;
                builderLayout.AppendKeys({"msg"});
                builder.columns = builderLayout.Columns();
            }
            builder.source =
                (loglib::Source{.kind = loglib::Source::Kind::NetworkStream, .locators = {"tcp://127.0.0.1:5170"}});
            loglib::SaveConfiguration(builder, jsonPath.toStdString(), loglib::SaveScope::Full);

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

            loglib::LogConfiguration builder;
            {
                loglib::ColumnLayout builderLayout;
                builderLayout.AppendKeys({"msg"});
                builder.columns = builderLayout.Columns();
            }
            // Intentionally no `SetSource`.
            loglib::SaveConfiguration(builder, jsonPath.toStdString(), loglib::SaveScope::Full);

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
};

QTEST_MAIN(FiltersTest)
#include "apptest_filters_test.moc"
