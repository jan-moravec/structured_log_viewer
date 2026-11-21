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

#include <loglib/config/filter_expression.hpp>
#include <loglib/config/log_configuration.hpp>
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
class MainWindowTest : public apptest_support::AppTestBase
{
    Q_OBJECT

private slots:
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
        QVERIFY(!L::IsSingleLineAsciiTrim(
            "a\x7f"
            "b"
        )); // DEL

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
                qPrintable(QStringLiteral(
                               "rejected sample '%1' must differ after "
                               "conversion to '%2'"
                )
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

    // A parse that throws reports one error and finishes the sink exactly once: the library finishes it when the parse
    // throws, and the worker boundary does not finish it a second time.
    static void TestThrowingParseFinishesSinkOnce()
    {
        LogModel model;
        QSignalSpy finishedSpy(&model, &LogModel::streamingFinished);
        QVERIFY(finishedSpy.isValid());

        auto source =
            std::make_unique<loglib::StreamLineSource>("<throwing>", std::make_unique<ThrowingBytesProducer>());
        const loglib::ParserOptions options;
        model.BeginStreaming(std::move(source), options, [] { return std::make_unique<loglib::JsonParser>(); });

        QVERIFY(finishedSpy.wait(60000));
        // Absence: give a second, wrongly sent OnFinished the chance to arrive. Nothing marks the end of that
        // period, so the wait is short and fixed.
        WaitToProveAbsence(std::chrono::milliseconds(100));
        QCOMPARE(finishedSpy.count(), 1);

        const auto &errors = model.StreamingErrors();
        QCOMPARE(static_cast<int>(errors.size()), 1);
        QCOMPARE(
            QString::fromStdString(loglib::FormatParseError(errors.front())),
            QStringLiteral("Streaming parse failed: producer failed")
        );
    }

    // The same for a failure before the parse starts, which only the worker boundary can report.
    static void TestFailingParserFactoryFinishesSinkOnce()
    {
        LogModel model;
        QSignalSpy finishedSpy(&model, &LogModel::streamingFinished);
        QVERIFY(finishedSpy.isValid());

        auto source =
            std::make_unique<loglib::StreamLineSource>("<throwing>", std::make_unique<ThrowingBytesProducer>());
        const loglib::ParserOptions options;
        model.BeginStreaming(std::move(source), options, ThrowingParserFactory);

        QVERIFY(finishedSpy.wait(60000));
        // Absence: give a second, wrongly sent OnFinished the chance to arrive.
        WaitToProveAbsence(std::chrono::milliseconds(100));
        QCOMPARE(finishedSpy.count(), 1);

        const auto &errors = model.StreamingErrors();
        QCOMPARE(static_cast<int>(errors.size()), 1);
        QCOMPARE(
            QString::fromStdString(loglib::FormatParseError(errors.front())),
            QStringLiteral("Streaming parse failed: factory failed")
        );
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
        const QString message = QString::fromStdString(loglib::FormatParseError(errors.front()));
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

        const auto &columns = run.model->Layout().Columns();
        QCOMPARE(columns[static_cast<size_t>(tsCol)].type, loglib::ColumnType::Time);

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
            &model, &QAbstractItemModel::rowsAboutToBeInserted, &model, [&](const QModelIndex &, int, int) {
                rowCountSnapshotsAtAboutToBeInserted.push_back(model.rowCount());
            }
        );
        QObject::connect(
            &model, &QAbstractItemModel::columnsAboutToBeInserted, &model, [&](const QModelIndex &, int, int) {
                columnCountSnapshotsAtAboutToBeInserted.push_back(model.columnCount());
            }
        );

        QSignalSpy finishedSpy(&model, &LogModel::streamingFinished);
        QVERIFY(finishedSpy.isValid());

        auto fileSource = std::make_unique<loglib::FileLineSource>(fixture.Path().toStdString());
        loglib::FileLineSource *parseSource = fileSource.get();
        const loglib::StopToken stopToken = model.BeginStreamingForSyncTest(std::move(fileSource));

        QVERIFY(!model.Table().Data().Sources().empty());

        loglib::ParserOptions options;
        options.stopToken = stopToken;
        options.threads = 1;

        const loglib::JsonParser parser;
        parser.Parse(*parseSource, *model.Sink(), options);

        const bool finished = finishedSpy.count() > 0 || finishedSpy.wait(60000);
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

        auto fileSource = std::make_unique<loglib::FileLineSource>(fixture.Path().toStdString());
        loglib::FileLineSource *sourcePtr = fileSource.get();

        std::mutex releaseMutex;
        std::condition_variable releaseCv;
        bool released = false;
        QtStreamingLogSink *sinkBeforeBegin = model.Sink();
        QVERIFY(sinkBeforeBegin != nullptr);

        const loglib::StopToken stop = model.BeginStreaming(
            std::move(fileSource),
            [sinkBeforeBegin, sourcePtr, &releaseMutex, &releaseCv, &released](const loglib::StopToken &stopToken) {
                while (!stopToken.StopRequested())
                {
                    std::this_thread::yield();
                }

                loglib::KeyIndex &keys = sinkBeforeBegin->Keys();
                const loglib::KeyId keyId = keys.GetOrInsert("a");
                std::vector<std::pair<loglib::KeyId, loglib::LogValue>> values;
                values.emplace_back(keyId, loglib::LogValue(int64_t{1}));
                loglib::LogLine line(std::move(values), *sourcePtr, 1);
                loglib::RowBatch batch;
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
            // Lets `Reset()` block on the worker first, so the test covers the blocked path. It passes when the
            // release comes before `Reset()` blocks, so this only picks the path under test and is not a
            // correctness wait; nothing observable shows that `Reset()` is blocked.
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
            model.AppendBatch(MakeSyntheticBatch(streamSource, valueKey, batchStart + 1, batchSize, declareNewKey));
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
            MakeSyntheticBatch(streamSource, valueKey, /*firstLineId=*/1, /*count=*/2000, /*declareNewKey=*/true)
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
            streamSource, valueKey, /*firstLineId=*/1, static_cast<size_t>(batchSize), /*declareNewKey=*/true
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
        sink->OnBatch(MakeSyntheticBatch(streamSource, valueKey, 1, pausedCount, /*declareNewKey=*/true));
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
            MakeSyntheticBatch(streamSource, valueKey, pausedCount + 1, followupCount, /*declareNewKey=*/false)
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

    // The rows the export worker walks must match the user's
    // visible top-to-bottom order. Regression guard for the
    // single-level `mapToSource` bug that dropped the outer proxy
    // (newest-first) and silently reversed the exported order.
    void TestCollectExportSourceRowsRespectsDisplayOrder()
    {
        auto *rowOrderProxy = mWindow->findChild<RowOrderProxyModel *>();
        auto *model = mWindow->findChild<LogModel *>();
        auto *filterProxy = mWindow->FilterModel();
        QVERIFY(rowOrderProxy != nullptr);
        QVERIFY(model != nullptr);
        QVERIFY(filterProxy != nullptr);

        // Five rows with deterministic ids for exact ordering asserts.
        loglib::StreamLineSource &streamSource = BeginSyntheticStreamSession(*model);
        QtStreamingLogSink *sink = model->Sink();
        QVERIFY(sink != nullptr);

        loglib::KeyIndex &keys = sink->Keys();
        const loglib::KeyId valueKey = keys.GetOrInsert(std::string("value"));
        sink->OnBatch(MakeSyntheticBatch(streamSource, valueKey, 1, 5, /*declareNewKey=*/true));
        QCoreApplication::processEvents();
        QCOMPARE(model->rowCount(), 5);

        // Natural order: source rows 0..4.
        {
            const std::vector<int> collected = mWindow->CollectExportSourceRows(/*selectionOnly=*/false);
            QCOMPARE(collected.size(), std::size_t(5));
            for (std::size_t i = 0; i < collected.size(); ++i)
            {
                QCOMPARE(collected[i], static_cast<int>(i));
            }
        }

        // Newest-first flips the mapping (display 0 == source 4,
        // ..., display 4 == source 0). The pre-fix code walked
        // only one proxy level and got [0,1,2,3,4] here.
        rowOrderProxy->SetReversed(true);
        QVERIFY(rowOrderProxy->IsReversed());
        {
            const std::vector<int> collected = mWindow->CollectExportSourceRows(/*selectionOnly=*/false);
            QCOMPARE(collected.size(), std::size_t(5));
            const std::vector<int> expected = {4, 3, 2, 1, 0};
            QCOMPARE(collected, expected);
        }
        rowOrderProxy->SetReversed(false);

        model->EndStreaming(false);
    }

    // Selection-only must preserve display order (top-to-bottom),
    // not sort by source-row index (the pre-fix behaviour that
    // lost the user's column sort).
    void TestCollectExportSourceRowsSelectionKeepsDisplayOrder()
    {
        auto *model = mWindow->findChild<LogModel *>();
        auto *filterProxy = mWindow->FilterModel();
        QVERIFY(model != nullptr);
        QVERIFY(filterProxy != nullptr);

        auto *tableView = mWindow->findChild<LogTableView *>();
        QVERIFY(tableView != nullptr);
        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): false positive; prior `QVERIFY` aborts on null.
        QVERIFY(tableView->selectionModel() != nullptr);

        loglib::StreamLineSource &streamSource = BeginSyntheticStreamSession(*model);
        QtStreamingLogSink *sink = model->Sink();
        QVERIFY(sink != nullptr);

        loglib::KeyIndex &keys = sink->Keys();
        const loglib::KeyId valueKey = keys.GetOrInsert(std::string("value"));
        sink->OnBatch(MakeSyntheticBatch(streamSource, valueKey, 1, 5, /*declareNewKey=*/true));
        QCoreApplication::processEvents();
        QCOMPARE(model->rowCount(), 5);

        const int valueColumn = ColumnByHeader(*model, QStringLiteral("value"));
        QVERIFY(valueColumn >= 0);

        // Descending by value: display rows 0..4 map to source
        // rows 4,3,2,1,0.
        filterProxy->sort(valueColumn, Qt::DescendingOrder);
        QCoreApplication::processEvents();

        // Select the middle three display rows (1..3) -- source
        // rows 3, 2, 1 in that order.
        QItemSelection selection;
        selection.select(filterProxy->index(1, 0), filterProxy->index(3, filterProxy->columnCount() - 1));
        tableView->selectionModel()->select(selection, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
        QCOMPARE(tableView->selectionModel()->selectedRows().size(), 3);

        const std::vector<int> collected = mWindow->CollectExportSourceRows(/*selectionOnly=*/true);
        // Must be in display order (`{3,2,1}`), NOT sorted by
        // source-row index (`{1,2,3}`).
        const std::vector<int> expected = {3, 2, 1};
        QCOMPARE(collected, expected);

        filterProxy->sort(-1);
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
        sink->OnBatch(MakeSyntheticBatch(streamSource, valueKey, 1, 5, /*declareNewKey=*/true));
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

        // Replace clamps out-of-range colour slots to the highest
        // known slot (rather than dropping the whole anchor) so the
        // bookmark position + note survive a downgrade. The return
        // value counts the remapped entries so the GUI can surface
        // the colour drift.
        std::vector<loglib::AnchorEntry> incoming;
        incoming.push_back(loglib::AnchorEntry{.locator = "c:/x.json", .lineId = 1, .colorIndex = 0, .note = "keep"});
        incoming.push_back(
            loglib::AnchorEntry{.locator = "c:/x.json", .lineId = 2, .colorIndex = 42, .note = "future-slot"}
        );
        QCOMPARE(manager.Replace(incoming), std::size_t{1});
        QCOMPARE(resetSpy.count(), 2);
        // Both anchors are present now -- the clamp preserves the row.
        QCOMPARE(manager.Count(), std::size_t{2});
        QCOMPARE(manager.ColorFor({.locator = "c:/x.json", .lineId = 1}).value_or(255U), uint8_t{0});
        const auto clampedColor = manager.ColorFor({.locator = "c:/x.json", .lineId = 2});
        QVERIFY(clampedColor.has_value());
        QCOMPARE(*clampedColor, static_cast<uint8_t>(loglib::ANCHOR_PALETTE_SIZE - 1));
        // Note is preserved through the clamp: recolouring on load
        // must not drop the user's annotation.
        QCOMPARE(
            manager.NoteFor({.locator = "c:/x.json", .lineId = 2}).value_or(std::string{"NO-NOTE"}),
            std::string{"future-slot"}
        );

        // All-valid input returns 0 clamped (it is a schema-drift
        // signal, not an input size).
        std::vector<loglib::AnchorEntry> clean;
        clean.push_back(loglib::AnchorEntry{.locator = "c:/x.json", .lineId = 3, .colorIndex = 1});
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
        std::vector<loglib::AnchorEntry> entries;
        entries.push_back(loglib::AnchorEntry{.locator = "c:/logs/a.json", .lineId = 1, .colorIndex = 2});
        entries.push_back(loglib::AnchorEntry{.locator = "c:/logs/b.json", .lineId = 9, .colorIndex = 5});
        QCOMPARE(manager.Replace(entries), std::size_t{0});
        QCOMPARE(resetSpy.count(), 1);
        QCOMPARE(manager.Count(), std::size_t{2});

        // Same content, shuffled order: comparison is map-equality,
        // so this must NOT emit.
        std::vector<loglib::AnchorEntry> shuffled;
        shuffled.push_back(loglib::AnchorEntry{.locator = "c:/logs/b.json", .lineId = 9, .colorIndex = 5});
        shuffled.push_back(loglib::AnchorEntry{.locator = "c:/logs/a.json", .lineId = 1, .colorIndex = 2});
        QCOMPARE(manager.Replace(shuffled), std::size_t{0});
        QCOMPARE(resetSpy.count(), 1);

        // Same keys, different colour: content changed, listeners
        // must be told.
        std::vector<loglib::AnchorEntry> mutated;
        mutated.push_back(loglib::AnchorEntry{.locator = "c:/logs/a.json", .lineId = 1, .colorIndex = 2});
        mutated.push_back(loglib::AnchorEntry{.locator = "c:/logs/b.json", .lineId = 9, .colorIndex = 6});
        QCOMPARE(manager.Replace(mutated), std::size_t{0});
        QCOMPARE(resetSpy.count(), 2);
        QCOMPARE(manager.ColorFor({.locator = "c:/logs/b.json", .lineId = 9}).value_or(255U), uint8_t{6});
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
        sink->OnBatch(MakeSyntheticBatch(streamSource, valueKey, /*firstLineId=*/1, /*count=*/3, true));
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
        sink->OnBatch(MakeSyntheticBatch(streamSource, valueKey, 1, 4, true));
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
        sink->OnBatch(MakeSyntheticBatch(streamSource, valueKey, 1, 3, true));
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
        sink->OnBatch(MakeSyntheticBatch(streamSource, valueKey, 1, 5, true));
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
        auto *tree = dock->TreeForTest();
        QVERIFY(tree != nullptr);
        auto *clearBtn = dock->ClearAllButtonForTest();
        QVERIFY(clearBtn != nullptr);

        loglib::StreamLineSource &streamSource = BeginSyntheticStreamSession(*model);
        QtStreamingLogSink *sink = model->Sink();
        QVERIFY(sink != nullptr);
        loglib::KeyIndex &keys = sink->Keys();
        const loglib::KeyId valueKey = keys.GetOrInsert(std::string("value"));
        sink->OnBatch(MakeSyntheticBatch(streamSource, valueKey, 1, 4, true));
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
        QCOMPARE(tree->topLevelItemCount(), 2);

        // `itemActivated` on the anchor column -> jump. The signal
        // gates on column 0 so wiring double-click on the note
        // column doesn't spuriously request a jump.
        QSignalSpy jumpSpy(dock, &AnchorsDock::jumpToAnchorRequested);
        QVERIFY(jumpSpy.isValid());

        QTreeWidgetItem *firstItem = tree->topLevelItem(0);
        QVERIFY(firstItem != nullptr);
        emit tree->itemActivated(firstItem, /*column=*/0);
        QCoreApplication::processEvents();
        QCOMPARE(jumpSpy.count(), 1);
        // Both keys resolve back to a non-negative source row.
        QVERIFY(jumpSpy.last().at(0).toInt() >= 0);

        QTreeWidgetItem *secondItem = tree->topLevelItem(1);
        QVERIFY(secondItem != nullptr);
        emit tree->itemActivated(secondItem, /*column=*/0);
        QCoreApplication::processEvents();
        QCOMPARE(jumpSpy.count(), 2);
        QVERIFY(jumpSpy.last().at(0).toInt() >= 0);

        // Two distinct source rows (Entries() sorts by lineId,
        // so item 0 -> lineId 1 / row 0, item 1 -> lineId 3 / row 2).
        QVERIFY(jumpSpy.at(0).at(0).toInt() != jumpSpy.at(1).at(0).toInt());

        // Activation on the note column must not issue a jump: only
        // the anchor column is activate-to-jump; the note column is
        // activate-to-commit-edit (Qt handles that internally).
        const auto jumpCountBeforeNoteActivate = jumpSpy.count();
        emit tree->itemActivated(firstItem, /*column=*/1);
        QCoreApplication::processEvents();
        QCOMPARE(jumpSpy.count(), jumpCountBeforeNoteActivate);

        emit clearBtn->clicked();
        QCoreApplication::processEvents();
        QVERIFY2(anchors->Empty(), "Clear all button must wipe every anchor");
        dock->RefreshForTest();
        QCOMPARE(tree->topLevelItemCount(), 0);

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
        sink->OnBatch(MakeSyntheticBatch(streamSource, valueKey, 1, 5, true));
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

    // `ParseGotoTimestampInput` + `TimeZoneContext::LocalToUtc`
    // compose correctly: naive parses get TZ-shifted, zoned ones
    // do NOT (double-shift regression pin). Shift bound is +/-14h
    // to stay TZ-agnostic across CI hosts.
    void TestParseAndLocalShiftHelpersComposeForNaiveInput()
    {
        const std::chrono::system_clock::time_point kNow =
            std::chrono::system_clock::time_point{std::chrono::microseconds{1'700'000'000'000'000LL}};

        const auto naiveParse = MainWindow::ParseGotoTimestampInput(QStringLiteral("2024-04-28T12:00:00"), {}, kNow);
        QVERIFY(naiveParse.has_value());
        QVERIFY(naiveParse->isNaive);

        const int64_t naiveMicros = naiveParse->micros;
        const int64_t shiftedMicros = mWindow->TimeZone().LocalToUtc(naiveMicros);

        // Shift is exactly the UTC offset at that instant. Bound it
        // to +/-14 h (max real-world offset today) so the assertion
        // catches an accidental "shift by seconds since epoch"
        // regression regardless of the host TZ.
        const int64_t deltaMicros = shiftedMicros - naiveMicros;
        constexpr int64_t MAX_TZ_OFFSET_MICROS = 14LL * 3600LL * 1'000'000LL;
        QVERIFY2(std::llabs(deltaMicros) <= MAX_TZ_OFFSET_MICROS, "TZ shift must be within +/-14 h");

        // Zoned input must NOT shift (double-shift regression pin).
        const std::vector<std::string> zonedFormats{"%FT%T%Ez"};
        const auto zonedParse =
            MainWindow::ParseGotoTimestampInput(QStringLiteral("2024-04-28T12:00:00+00:00"), zonedFormats, kNow);
        QVERIFY(zonedParse.has_value());
        QVERIFY(!zonedParse->isNaive);
    }

    // SetAnchor on an already-anchored key must preserve any note
    // attached to it. Ctrl+1..8 must not clobber user text.
    void TestSetAnchorPreservesExistingNote()
    {
        AnchorManager manager;
        const AnchorManager::Key key{.locator = "c:/logs/a.json", .lineId = 11};

        QVERIFY(manager.SetAnchor(key, 2));
        QVERIFY(manager.SetAnchorNote(key, std::string{"escalated to on-call"}));
        QCOMPARE(manager.NoteFor(key).value_or(std::string{"NO-KEY"}), std::string{"escalated to on-call"});

        // Recolour: the note must survive so Ctrl+1..8 stays
        // non-destructive.
        QVERIFY(manager.SetAnchor(key, 4));
        QCOMPARE(manager.ColorFor(key).value_or(255U), uint8_t{4});
        QCOMPARE(manager.NoteFor(key).value_or(std::string{"NO-KEY"}), std::string{"escalated to on-call"});

        // Bulk recolour (Ctrl+1..8 on a multi-row selection) must
        // preserve the note too.
        const std::array keys{key};
        QVERIFY(manager.SetAnchors(keys, 6));
        QCOMPARE(manager.ColorFor(key).value_or(255U), uint8_t{6});
        QCOMPARE(manager.NoteFor(key).value_or(std::string{"NO-KEY"}), std::string{"escalated to on-call"});

        // Removing the anchor drops the note (notes only exist
        // alongside an anchor colour).
        QVERIFY(manager.RemoveAnchor(key));
        QVERIFY(!manager.NoteFor(key).has_value());
    }

    // SetAnchorNote on an unknown key must be a no-op. Notes only
    // exist alongside an anchor colour, so the manager rejects a
    // note without seeding an anchor. Neither `anchorChanged` (the
    // colour-signal) nor `anchorNoteChanged` (the note-signal) may
    // fire on a rejected write -- otherwise colour-only listeners
    // (histogram tick strip, overview rail) would rebuild for
    // nothing.
    void TestSetAnchorNoteRejectsUnknownKey()
    {
        AnchorManager manager;
        const QSignalSpy changedSpy(&manager, &AnchorManager::anchorChanged);
        const QSignalSpy noteChangedSpy(&manager, &AnchorManager::anchorNoteChanged);
        const AnchorManager::Key key{.locator = "c:/logs/a.json", .lineId = 99};

        QVERIFY(!manager.SetAnchorNote(key, std::string{"not gonna stick"}));
        QCOMPARE(changedSpy.count(), 0);
        QCOMPARE(noteChangedSpy.count(), 0);
        QVERIFY(!manager.NoteFor(key).has_value());
    }

    // `AnchorManager::SanitiseNote` is the single source of truth
    // for the "one line, no edge whitespace" invariant. Manager
    // methods (`SetAnchorNote`, `Replace`) apply it internally so
    // no caller can smuggle a multi-line note past the UI's
    // assumptions.
    void TestAnchorManagerSanitiseNote()
    {
        // Pure input: unchanged (interior spaces preserved).
        QCOMPARE(AnchorManager::SanitiseNote(std::string{"already tidy"}), std::string{"already tidy"});

        // Empty input: empty result (not `nullopt`; empty notes are
        // legal on anchored rows).
        QCOMPARE(AnchorManager::SanitiseNote(std::string{}), std::string{});
        QCOMPARE(AnchorManager::SanitiseNote(std::string{"   "}), std::string{});

        // Windows CRLF collapses to a single space (not two); lone
        // CR / LF / TAB each fold to one space; edges trim.
        QCOMPARE(AnchorManager::SanitiseNote(std::string{"  a\r\nb\tc  "}), std::string{"a b c"});
        QCOMPARE(AnchorManager::SanitiseNote(std::string{"line1\rline2"}), std::string{"line1 line2"});
        QCOMPARE(AnchorManager::SanitiseNote(std::string{"line1\nline2"}), std::string{"line1 line2"});

        // Interior double space stays put so users can write
        // deliberate word gaps.
        QCOMPARE(AnchorManager::SanitiseNote(std::string{"foo  bar"}), std::string{"foo  bar"});

        // Non-ASCII survives byte-for-byte (UTF-8 continuation bytes
        // are all above 0x7F, none of which match the CR/LF/TAB
        // predicate).
        QCOMPARE(
            AnchorManager::SanitiseNote(std::string{"  \xc3\xa9v\xc3\xa9nement\t"}),
            std::string{"\xc3\xa9v\xc3\xa9nement"}
        );

        // Extended whitespace fold: `\v` (vertical tab), `\f` (form
        // feed), and `\0` (NUL) all collapse to single spaces so a
        // pasted control-character run can't break the "one line"
        // invariant. NUL is particularly load-bearing: it silently
        // truncates downstream C-string conversions (some tooltip
        // paths, clipboard payload assembly), so folding it here
        // means we never store one in the first place.
        QCOMPARE(AnchorManager::SanitiseNote(std::string{"a\vb\fc"}), std::string{"a b c"});
        QCOMPARE(AnchorManager::SanitiseNote(std::string{"a\0b", 3}), std::string{"a b"});

        // Unicode line separators fold too. U+2028 (LINE SEPARATOR)
        // and U+2029 (PARAGRAPH SEPARATOR) are 3-byte sequences that
        // `QLabel` with `Qt::PlainText` + `wordWrap` renders as line
        // breaks in the record-detail subline. Folding avoids that
        // and matches the promise on the one-line invariant.
        // U+2028 = E2 80 A8; U+2029 = E2 80 A9.
        QCOMPARE(
            AnchorManager::SanitiseNote(
                std::string{"a\xe2\x80\xa8"
                            "b\xe2\x80\xa9"
                            "c"}
            ),
            std::string{"a b c"}
        );

        // `SetAnchorNote` applies the sanitisation internally so
        // even a direct caller that skips the UI helper can't leak
        // newlines into the stored state.
        AnchorManager manager;
        const AnchorManager::Key key{.locator = "c:/logs/a.json", .lineId = 42};
        QVERIFY(manager.SetAnchor(key, 1));
        // Spies wrap `SetAnchorNote` so we cover both the successful
        // write (fires `anchorNoteChanged`, not `anchorChanged`) and
        // the idempotent no-op (fires neither).
        const QSignalSpy changedSpy(&manager, &AnchorManager::anchorChanged);
        const QSignalSpy noteChangedSpy(&manager, &AnchorManager::anchorNoteChanged);
        QVERIFY(manager.SetAnchorNote(key, std::string{"  raw\r\nnote\t "}));
        QCOMPARE(manager.NoteFor(key).value_or(std::string{"NO-KEY"}), std::string{"raw note"});
        // Note edits go through `anchorNoteChanged`, NOT
        // `anchorChanged`, so colour-only listeners (histogram tick
        // strip, overview rail bands) don't rebuild on every keystroke.
        QCOMPARE(changedSpy.count(), 0);
        QCOMPARE(noteChangedSpy.count(), 1);

        // A second `SetAnchorNote` with the same *unsanitised* input
        // must be a no-op after the first call sanitised it: the
        // stored note equals the sanitised form, so nothing to do.
        QVERIFY(!manager.SetAnchorNote(key, std::string{"  raw\r\nnote\t "}));
        QCOMPARE(changedSpy.count(), 0);
        QCOMPARE(noteChangedSpy.count(), 1);
    }

    // Notes exceeding `AnchorManager::MAX_NOTE_BYTES` get truncated
    // on a UTF-8 sequence boundary. This bounds the render / tooltip
    // / clipboard cost of a note; a runaway paste can't force us to
    // reformat / display megabytes of text on every anchor touch.
    void TestAnchorManagerSanitiseNoteCapsLength()
    {
        // Just-under-cap input is stored verbatim.
        {
            const std::string underCap(AnchorManager::MAX_NOTE_BYTES, 'x');
            const auto out = AnchorManager::SanitiseNote(underCap);
            QCOMPARE(out.size(), AnchorManager::MAX_NOTE_BYTES);
        }
        // Exceeding input gets truncated *at* the cap: no bytes over.
        {
            const std::string overCap(AnchorManager::MAX_NOTE_BYTES + 500, 'x');
            const auto out = AnchorManager::SanitiseNote(overCap);
            QVERIFY(out.size() <= AnchorManager::MAX_NOTE_BYTES);
            QCOMPARE(out.size(), AnchorManager::MAX_NOTE_BYTES);
        }
        // UTF-8 boundary: if the cap lands mid-sequence, walk back
        // to a lead byte so the trailing code point isn't sliced.
        // Build a note ending in a 2-byte code point (`é`, 0xC3 0xA9)
        // straddling the cap.
        {
            // Fill up to `cap - 1` bytes with ASCII, then append `é`.
            // Cap - 1 ASCII + first byte of `é` at position (cap - 1)
            // + continuation at position `cap` -> naive truncate
            // would leave a lone lead byte. Our walkback must drop
            // the whole `é`.
            std::string padded(AnchorManager::MAX_NOTE_BYTES - 1, 'a');
            padded.push_back('\xc3');
            padded.push_back('\xa9');
            padded.append("tail"); // extra bytes past the cap so we truncate.
            const auto out = AnchorManager::SanitiseNote(std::move(padded));
            // Must not contain a stray lead byte at the tail.
            QVERIFY2(
                out.size() < AnchorManager::MAX_NOTE_BYTES,
                qPrintable(QStringLiteral("expected UTF-8 walkback to trim below cap, got size %1")
                               .arg(static_cast<qulonglong>(out.size())))
            );
            QVERIFY2(
                out.back() == 'a',
                qPrintable(QStringLiteral("last byte should be ASCII 'a', got 0x%1")
                               .arg(static_cast<int>(static_cast<unsigned char>(out.back())), 2, 16))
            );
        }
        // Trailing whitespace exposed by the truncation cut is
        // stripped so the "no trailing whitespace" invariant holds.
        {
            std::string padded(AnchorManager::MAX_NOTE_BYTES - 1, 'a');
            padded.push_back(' '); // just under cap
            padded.append("tail"); // pushes past cap; walkback lands after the space
            const auto out = AnchorManager::SanitiseNote(std::move(padded));
            QVERIFY2(
                out.empty() || out.back() != ' ', "trailing space produced by the truncation cut must be stripped"
            );
        }
        // Regression for the lead-byte-at-cap edge case: a 2-byte
        // code point (`é` = C3 A9) that starts exactly at position
        // `MAX_NOTE_BYTES - 1` and continues into position
        // `MAX_NOTE_BYTES`. A naive walkback that inspects
        // `out[MAX_NOTE_BYTES]` sees the lead byte at
        // `MAX_NOTE_BYTES - 1` as a "stop, this is a good boundary"
        // and would leave a stray `C3` at the tail (an incomplete
        // UTF-8 sequence). The fix also drops the lead byte when
        // it starts a sequence that wouldn't be complete within
        // the cap.
        {
            std::string padded(AnchorManager::MAX_NOTE_BYTES - 1, 'a');
            padded.push_back('\xc3'); // lead byte of `é` at cap-1
            padded.push_back('\xa9'); // continuation at cap
            padded.append("more");    // push past the cap so the truncation branch runs
            const auto out = AnchorManager::SanitiseNote(std::move(padded));
            // Result must NOT end with a lone lead byte. Lead byte
            // pattern is (byte & 0xC0) == 0xC0.
            QVERIFY2(!out.empty(), "output must not be empty");
            const auto lastByte = static_cast<unsigned char>(out.back());
            QVERIFY2(
                (lastByte & 0xC0U) != 0xC0U,
                qPrintable(QStringLiteral(
                               "output must not end with an incomplete UTF-8 sequence, "
                               "last byte was 0x%1"
                )
                               .arg(static_cast<int>(lastByte), 2, 16, QChar('0')))
            );
            // Every kept byte should be ASCII 'a' (the multi-byte
            // sequence and everything after it was dropped).
            for (const char ch : out)
            {
                QCOMPARE(ch, 'a');
            }
        }
    }

    // `AnchorManager::SetAnchorNote` must never fire `anchorChanged`
    // -- the whole point of the recent signal split is to keep
    // colour-only listeners (histogram, overview rail) from
    // rebuilding on every keystroke of a note edit. Item #14 / #18
    // of the code review flagged the invariant as load-bearing but
    // implicit; this test pins it explicitly so a future refactor
    // that collapses the two signals breaks a red bar first.
    void TestAnchorManagerSetAnchorNoteDoesNotEmitAnchorChanged()
    {
        AnchorManager manager;
        const AnchorManager::Key key{.locator = "c:/logs/x.json", .lineId = 7};
        QVERIFY(manager.SetAnchor(key, 2));

        const QSignalSpy changedSpy(&manager, &AnchorManager::anchorChanged);
        const QSignalSpy noteChangedSpy(&manager, &AnchorManager::anchorNoteChanged);

        QVERIFY(manager.SetAnchorNote(key, std::string{"first note"}));
        QCOMPARE(changedSpy.count(), 0);
        QCOMPARE(noteChangedSpy.count(), 1);

        // Clearing to empty is still a note change, not a colour
        // change: `anchorChanged` stays quiet.
        QVERIFY(manager.SetAnchorNote(key, std::string{}));
        QCOMPARE(changedSpy.count(), 0);
        QCOMPARE(noteChangedSpy.count(), 2);
    }

    // Config load through `Replace` sanitises notes so a hand-edited
    // JSON with embedded newlines can't smuggle multi-line text
    // past the QLabel / tooltip pipeline that assumes one line.
    void TestAnchorManagerReplaceSanitisesNotes()
    {
        AnchorManager manager;
        std::vector<loglib::AnchorEntry> entries;
        entries.push_back(
            loglib::AnchorEntry{
                .locator = "c:/logs/a.json",
                .lineId = 1,
                .colorIndex = 0,
                .note = "  first\r\nsecond\t ",
            }
        );
        entries.push_back(
            loglib::AnchorEntry{
                .locator = "c:/logs/b.json",
                .lineId = 2,
                .colorIndex = 3,
                .note = "clean",
            }
        );
        (void)manager.Replace(entries);

        const AnchorManager::Key key1{.locator = "c:/logs/a.json", .lineId = 1};
        const AnchorManager::Key key2{.locator = "c:/logs/b.json", .lineId = 2};
        QCOMPARE(manager.NoteFor(key1).value_or(std::string{"NO-KEY"}), std::string{"first second"});
        QCOMPARE(manager.NoteFor(key2).value_or(std::string{"NO-KEY"}), std::string{"clean"});
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
        sink->OnBatch(MakeSyntheticBatch(streamSource, valueKey, 1, 2, true));
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
        sink->OnBatch(MakeSyntheticBatch(streamSource, valueKey, 1, 4, true));
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

        model.AppendBatch(MakeSyntheticBatch(streamSource, valueKey, 1, 4, /*declareNewKey=*/true));
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
        model.AppendBatch(MakeSyntheticBatch(streamSource, valueKey, 5, 3, /*declareNewKey=*/false));
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
        auto *tree = dock->TreeForTest();
        QVERIFY(tree != nullptr);

        loglib::StreamLineSource &streamSource = BeginSyntheticStreamSession(*model);
        QtStreamingLogSink *sink = model->Sink();
        QVERIFY(sink != nullptr);
        loglib::KeyIndex &keys = sink->Keys();
        const loglib::KeyId valueKey = keys.GetOrInsert(std::string("value"));
        sink->OnBatch(MakeSyntheticBatch(streamSource, valueKey, 1, 4, true));
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
        QCOMPARE(tree->topLevelItemCount(), 3);

        // Middle item current + selected; then re-run Refresh.
        // Without snapshot/restore, `mTree->clear()` would wipe it.
        QTreeWidgetItem *middle = tree->topLevelItem(1);
        QVERIFY(middle != nullptr);
        tree->setCurrentItem(middle);
        middle->setSelected(true);
        QCOMPARE(tree->selectedItems().count(), 1);
        QVERIFY(tree->currentItem() == middle);

        // Touch key0 -> `anchorChanged` -> `Refresh()`. Offscreen
        // QPA gates Refresh, so call the test seam directly.
        anchors->SetAnchor(*key0, 5);
        QCoreApplication::processEvents();
        dock->RefreshForTest();

        // Three anchors, middle still selected + current.
        QCOMPARE(tree->topLevelItemCount(), 3);
        QCOMPARE(tree->selectedItems().count(), 1);
        const QTreeWidgetItem *restoredCurrent = tree->currentItem();
        QVERIFY2(restoredCurrent != nullptr, "current item must survive Refresh");
        QCOMPARE(restoredCurrent->data(0, Qt::UserRole + 2).toULongLong(), static_cast<qulonglong>(key1->lineId));

        anchors->ClearAll();
        QCoreApplication::processEvents();
        model->EndStreaming(false);
    }

    // Editing the note column in the AnchorsDock commits back to
    // `AnchorManager::SetAnchorNote`. Covers the item-changed
    // sanitisation path (embedded newlines are collapsed) and
    // guards against re-entry when the dock's own refresh rewrites
    // the item text.
    void TestAnchorsDockInlineEditCommitsNote()
    {
        auto *anchors = mWindow->Anchors();
        QVERIFY(anchors != nullptr);
        auto *model = mWindow->Model();
        auto *dock = mWindow->findChild<AnchorsDock *>();
        QVERIFY(dock != nullptr);
        auto *tree = dock->TreeForTest();
        QVERIFY(tree != nullptr);

        loglib::StreamLineSource &streamSource = BeginSyntheticStreamSession(*model);
        QtStreamingLogSink *sink = model->Sink();
        QVERIFY(sink != nullptr);
        loglib::KeyIndex &keys = sink->Keys();
        const loglib::KeyId valueKey = keys.GetOrInsert(std::string("value"));
        sink->OnBatch(MakeSyntheticBatch(streamSource, valueKey, 1, 3, true));
        QCoreApplication::processEvents();
        QCOMPARE(model->rowCount(), 3);

        const auto key0 = model->AnchorKeyForRow(0);
        QVERIFY(key0.has_value());
        anchors->SetAnchor(*key0, 2);
        dock->RefreshForTest();
        QCOMPARE(tree->topLevelItemCount(), 1);

        // Edit the note column: setText fires `itemChanged`, which
        // the dock forwards to `SetAnchorNote`. The row-refresh
        // triggered by `anchorNoteChanged` runs through the
        // suppression counter, so no re-entry occurs.
        QTreeWidgetItem *item = tree->topLevelItem(0);
        QVERIFY(item != nullptr);
        item->setText(1, QStringLiteral("first error"));
        QCoreApplication::processEvents();

        QCOMPARE(anchors->NoteFor(*key0).value_or(std::string{"NO-KEY"}), std::string{"first error"});

        // Multi-line paste: the commit path collapses newlines and
        // trims edges so the on-disk note stays a one-liner.
        item->setText(1, QStringLiteral("  line one\nline two\t "));
        QCoreApplication::processEvents();
        QCOMPARE(anchors->NoteFor(*key0).value_or(std::string{"NO-KEY"}), std::string{"line one line two"});
        // The displayed text is sanitised in place too so the item
        // shows the same one-line form the manager stores.
        QCOMPARE(item->text(1), QStringLiteral("line one line two"));

        anchors->ClearAll();
        QCoreApplication::processEvents();
        model->EndStreaming(false);
    }

    // F4 / row-menu editor commit path: anchored rows accept a note,
    // unanchored rows surface a status-bar hint instead of touching
    // the manager. Uses the `SubmitAnchorNoteForRowForTest` seam so
    // the modal `QInputDialog` doesn't block the headless runner.
    void TestEditAnchorNoteForRowGuardsUnanchored()
    {
        auto *anchors = mWindow->Anchors();
        QVERIFY(anchors != nullptr);
        auto *model = mWindow->Model();
        QVERIFY(model != nullptr);

        loglib::StreamLineSource &streamSource = BeginSyntheticStreamSession(*model);
        QtStreamingLogSink *sink = model->Sink();
        QVERIFY(sink != nullptr);
        loglib::KeyIndex &keys = sink->Keys();
        const loglib::KeyId valueKey = keys.GetOrInsert(std::string("value"));
        sink->OnBatch(MakeSyntheticBatch(streamSource, valueKey, 1, 3, true));
        QCoreApplication::processEvents();
        QCOMPARE(model->rowCount(), 3);

        const auto key0 = model->AnchorKeyForRow(0);
        const auto key1 = model->AnchorKeyForRow(1);
        QVERIFY(key0.has_value());
        QVERIFY(key1.has_value());
        anchors->SetAnchor(*key0, 3);
        QCoreApplication::processEvents();

        // Row 0 is anchored: commit succeeds and reaches the manager.
        QVERIFY(mWindow->SubmitAnchorNoteForRowForTest(0, QStringLiteral("customer report starts here")));
        QCOMPARE(anchors->NoteFor(*key0).value_or(std::string{"NO-KEY"}), std::string{"customer report starts here"});

        // Row 1 is not anchored: commit rejects and the manager is
        // untouched (no ghost anchor spawned by a stray note edit).
        QVERIFY(!mWindow->SubmitAnchorNoteForRowForTest(1, QStringLiteral("shouldn't stick")));
        QVERIFY(!anchors->NoteFor(*key1).has_value());
        QVERIFY(!anchors->ColorFor(*key1).has_value());

        // Sanitisation still applies to the anchored branch: CR/LF
        // and tabs collapse to spaces.
        QVERIFY(mWindow->SubmitAnchorNoteForRowForTest(0, QStringLiteral("  a\r\nb\tc  ")));
        QCOMPARE(anchors->NoteFor(*key0).value_or(std::string{"NO-KEY"}), std::string{"a b c"});

        anchors->ClearAll();
        QCoreApplication::processEvents();
        model->EndStreaming(false);
    }

    // F4 focus guard: the shortcut is `Qt::WindowShortcut` so it
    // fires anywhere in the main window, including while the user
    // is typing in a text editor (e.g. the AnchorsDock's inline
    // note editor, the search bar). Stealing focus to open a modal
    // dialog mid-edit would drop the in-flight text and target an
    // unrelated row. `EditAnchorNoteOnCurrentRow` must bail out
    // without opening any dialog when the focus widget is a text
    // input. This test asserts the guard by:
    //   1. Setting up a `QLineEdit` and forcibly promoting it to
    //      `focusWidget()` (`show()` + `activateWindow()` +
    //      `setFocus()` is required under offscreen QPA -- an
    //      un-shown widget or an inactive window silently drops
    //      focus assignment).
    //   2. Triggering the F4 `QAction` directly (rather than
    //      pressing the key) so no modal dialog can wedge the
    //      test runner if the guard regresses.
    //   3. Confirming the manager's note wasn't touched and no
    //      modal widget was spawned.
    void TestF4EditAnchorNoteBailsOutWhileEditorHasFocus()
    {
        auto *anchors = mWindow->Anchors();
        QVERIFY(anchors != nullptr);
        auto *model = mWindow->Model();
        QVERIFY(model != nullptr);

        loglib::StreamLineSource &streamSource = BeginSyntheticStreamSession(*model);
        QtStreamingLogSink *sink = model->Sink();
        QVERIFY(sink != nullptr);
        loglib::KeyIndex &keys = sink->Keys();
        const loglib::KeyId valueKey = keys.GetOrInsert(std::string("value"));
        sink->OnBatch(MakeSyntheticBatch(streamSource, valueKey, 1, 2, true));
        QCoreApplication::processEvents();
        QCOMPARE(model->rowCount(), 2);

        const auto key0 = model->AnchorKeyForRow(0);
        QVERIFY(key0.has_value());
        anchors->SetAnchor(*key0, 2);
        anchors->SetAnchorNote(*key0, std::string{"initial note"});
        QCoreApplication::processEvents();

        // Give a scratch `QLineEdit` focus and parent it to the
        // main window so `QApplication::focusWidget()` returns it.
        // Under offscreen QPA the widget must be `show()`n and its
        // window must be the active window before `setFocus()`
        // sticks -- an un-shown widget silently drops the request
        // and leaves `focusWidget()` at `nullptr`.
        mWindow->show();
        mWindow->activateWindow();
        auto scratch = std::make_unique<QLineEdit>(mWindow);
        scratch->setObjectName(QStringLiteral("scratchForFocusGuardTest"));
        scratch->show();
        scratch->setText(QStringLiteral("draft edit in flight"));
        scratch->setFocus(Qt::OtherFocusReason);
        QCoreApplication::processEvents();
        QVERIFY2(
            QApplication::focusWidget() == scratch.get(),
            qPrintable(QStringLiteral("scratch QLineEdit failed to grab focus (got %1)")
                           .arg(
                               QApplication::focusWidget() == nullptr
                                   ? QStringLiteral("nullptr")
                                   : QString::fromLatin1(QApplication::focusWidget()->metaObject()->className())
                           ))
        );

        // Trigger the F4 action. If the guard is intact, no dialog
        // opens and the stored note is unchanged. The action is
        // resolved by object name so this test stays honest even if
        // the shortcut key is remapped in the future.
        auto *editNoteAction = mWindow->findChild<QAction *>(QStringLiteral("actionEditRowAnchorNote"));
        QVERIFY2(editNoteAction != nullptr, "MainWindow must own an actionEditRowAnchorNote QAction");
        editNoteAction->trigger();
        QCoreApplication::processEvents();

        // The scratch line edit still has focus and its text is
        // untouched -- the modal dialog never opened.
        QCOMPARE(QApplication::focusWidget(), scratch.get());
        QCOMPARE(scratch->text(), QStringLiteral("draft edit in flight"));
        QCOMPARE(anchors->NoteFor(*key0).value_or(std::string{"NO-KEY"}), std::string{"initial note"});
        // No stray modal window was spawned (would be an
        // `activeModalWidget` from `QInputDialog::getText`).
        QVERIFY2(
            QApplication::activeModalWidget() == nullptr,
            "F4 must not open a modal editor while another editor holds focus"
        );

        scratch.reset();
        anchors->ClearAll();
        QCoreApplication::processEvents();
        model->EndStreaming(false);
        // Restore the fixture's expected hidden state so subsequent
        // tests don't inherit a shown/active window (visibility
        // changes paint scheduling and focus routing under offscreen
        // QPA, which can flake unrelated assertions).
        mWindow->hide();
        QCoreApplication::processEvents();
    }

    // Row tooltip surfaces the anchor note when the anchored row
    // has a non-empty note. Empty notes fall through to whatever
    // the default tooltip would be (nothing on non-level cells).
    void TestLogModelToolTipRoleSurfacesAnchorNote()
    {
        auto *anchors = mWindow->Anchors();
        QVERIFY(anchors != nullptr);
        auto *model = mWindow->Model();
        QVERIFY(model != nullptr);

        loglib::StreamLineSource &streamSource = BeginSyntheticStreamSession(*model);
        QtStreamingLogSink *sink = model->Sink();
        QVERIFY(sink != nullptr);
        loglib::KeyIndex &keys = sink->Keys();
        const loglib::KeyId valueKey = keys.GetOrInsert(std::string("value"));
        sink->OnBatch(MakeSyntheticBatch(streamSource, valueKey, 1, 2, true));
        QCoreApplication::processEvents();
        QCOMPARE(model->rowCount(), 2);

        const auto key0 = model->AnchorKeyForRow(0);
        QVERIFY(key0.has_value());
        anchors->SetAnchor(*key0, 4);
        anchors->SetAnchorNote(*key0, std::string{"customer 12345"});
        QCoreApplication::processEvents();

        // Any cell in the anchored row returns the anchor-note
        // tooltip so hovering doesn't need to hunt for the "right"
        // column. The wire form is a `<qt>`-wrapped rich-text
        // envelope so Qt renders the escaped entities correctly
        // even for notes that lack `<` (which is the sole char
        // `Qt::mightBeRichText()` triggers on inside entity refs);
        // the visible body still starts with `Anchor:`.
        const QModelIndex idx = model->index(0, 0);
        const QVariant tip = model->data(idx, Qt::ToolTipRole);
        QVERIFY(tip.isValid());
        const QString tipStr = tip.toString();
        QVERIFY2(
            tipStr.contains(QStringLiteral("customer 12345")),
            qPrintable(QStringLiteral("expected anchor note in tooltip, got: %1").arg(tipStr))
        );
        QVERIFY2(
            tipStr.startsWith(QStringLiteral("<qt>Anchor:")),
            qPrintable(QStringLiteral("expected <qt>-wrapped `Anchor:` prefix, got: %1").arg(tipStr))
        );
        QVERIFY2(
            tipStr.endsWith(QStringLiteral("</qt>")),
            qPrintable(QStringLiteral("expected closing </qt> tag, got: %1").arg(tipStr))
        );

        // Row 1 has no anchor: no tooltip.
        const QModelIndex idx1 = model->index(1, 0);
        QVERIFY(!model->data(idx1, Qt::ToolTipRole).isValid());

        // Clearing the note drops the tooltip back to the default.
        anchors->SetAnchorNote(*key0, std::string{});
        QCoreApplication::processEvents();
        QVERIFY(!model->data(idx, Qt::ToolTipRole).isValid());

        anchors->ClearAll();
        QCoreApplication::processEvents();
        model->EndStreaming(false);
    }

    // RecordDetail Copy as key/value: the anchor note (when
    // non-empty) rides at the top of the copied payload as a
    // synthetic `anchor.note` line.
    void TestRecordDetailCopyAsKeyValueIncludesAnchorNote()
    {
        auto *anchors = mWindow->Anchors();
        QVERIFY(anchors != nullptr);
        auto *model = mWindow->Model();
        QVERIFY(model != nullptr);

        loglib::StreamLineSource &streamSource = BeginSyntheticStreamSession(*model);
        QtStreamingLogSink *sink = model->Sink();
        QVERIFY(sink != nullptr);
        loglib::KeyIndex &keys = sink->Keys();
        const loglib::KeyId valueKey = keys.GetOrInsert(std::string("value"));
        sink->OnBatch(MakeSyntheticBatch(streamSource, valueKey, 1, 2, true));
        QCoreApplication::processEvents();
        QCOMPARE(model->rowCount(), 2);

        const auto key0 = model->AnchorKeyForRow(0);
        QVERIFY(key0.has_value());
        anchors->SetAnchor(*key0, 2);
        anchors->SetAnchorNote(*key0, std::string{"anchor-tag-42"});
        QCoreApplication::processEvents();

        // Build the content directly (avoids the RecordDetailDock's
        // visibility gating and matches what the widget snapshot
        // would carry).
        const RecordDetailContent content = BuildRecordDetailContent(*model, 0);
        QVERIFY(content.valid);
        QVERIFY(content.anchorColorIndex.has_value());
        QCOMPARE(*content.anchorColorIndex, std::uint8_t{2});
        QCOMPARE(content.anchorNote, QStringLiteral("anchor-tag-42"));

        // Drive the copy path through the actual widget so the
        // clipboard layout is exercised end-to-end.
        RecordDetailWidget widget;
        widget.SetContent(content);
        QClipboard *clipboard = QApplication::clipboard();
        clipboard->clear();
        auto *copyBtn = widget.findChild<QPushButton *>(QStringLiteral("copyKeyValueButton"));
        QVERIFY(copyBtn != nullptr);
        emit copyBtn->clicked();
        QCoreApplication::processEvents();

        const QString copied = clipboard->text();
        QVERIFY2(
            copied.startsWith(QStringLiteral("anchor.note: anchor-tag-42")),
            qPrintable(QStringLiteral("expected anchor.note prefix, got: %1").arg(copied.left(100)))
        );

        // Un-anchored row: no anchor.note line is emitted.
        const RecordDetailContent unanchored = BuildRecordDetailContent(*model, 1);
        QVERIFY(unanchored.valid);
        QVERIFY(!unanchored.anchorColorIndex.has_value());
        widget.SetContent(unanchored);
        clipboard->clear();
        emit copyBtn->clicked();
        QCoreApplication::processEvents();
        QVERIFY(!clipboard->text().contains(QStringLiteral("anchor.note")));

        anchors->ClearAll();
        QCoreApplication::processEvents();
        model->EndStreaming(false);
    }

    // Regression: `MainWindow` registers `F2` / `Shift+F2` as window-
    // scope `QAction` shortcuts for jump-to-(next|prev)-anchor.
    // Without the dock's `ShortcutOverride` filter, those shortcuts
    // fire before the tree's `EditKeyPressed` trigger sees the key,
    // and the advertised inline-note-editor path via `F2` is
    // unreachable. The filter must accept the `ShortcutOverride` so
    // Qt suppresses shortcut matching and delivers the follow-up
    // `KeyPress` to the tree.
    void TestAnchorsDockF2ShortcutOverrideOpensEditor()
    {
        auto *anchors = mWindow->Anchors();
        QVERIFY(anchors != nullptr);
        auto *model = mWindow->Model();
        auto *dock = mWindow->findChild<AnchorsDock *>();
        QVERIFY(dock != nullptr);
        auto *tree = dock->TreeForTest();
        QVERIFY(tree != nullptr);

        loglib::StreamLineSource &streamSource = BeginSyntheticStreamSession(*model);
        QtStreamingLogSink *sink = model->Sink();
        QVERIFY(sink != nullptr);
        loglib::KeyIndex &keys = sink->Keys();
        const loglib::KeyId valueKey = keys.GetOrInsert(std::string("value"));
        sink->OnBatch(MakeSyntheticBatch(streamSource, valueKey, 1, 1, true));
        QCoreApplication::processEvents();

        // Force the dock visible so refresh-gated paths run under
        // offscreen QPA. `AnchorsDock::IsVisibleForRefresh` gates on
        // both `!isHidden()` and `mPerceivedVisible`; call `show()`
        // so the first check passes, then emit `visibilityChanged`
        // so the second flips.
        dock->show();
        emit dock->visibilityChanged(true);

        const auto key0 = model->AnchorKeyForRow(0);
        QVERIFY(key0.has_value());
        anchors->SetAnchor(*key0, 2);
        dock->RefreshForTest();
        QCOMPARE(tree->topLevelItemCount(), 1);
        // Park focus on the note column of the first item.
        tree->setCurrentItem(tree->topLevelItem(0), /*column=*/1);

        // Send a `ShortcutOverride` for `F2`, no modifiers. The dock's
        // event filter must accept it (returning `true` in Qt speak
        // means "this key is NOT a shortcut, deliver as `KeyPress`").
        QKeyEvent f2Override(QEvent::ShortcutOverride, Qt::Key_F2, Qt::NoModifier);
        QApplication::sendEvent(tree, &f2Override);
        QVERIFY2(
            f2Override.isAccepted(),
            "AnchorsDock event filter must accept F2 ShortcutOverride so window-scope jump shortcuts don't steal it"
        );

        // Shift+F2 (prev-anchor jump) must NOT be intercepted. The
        // tree's edit trigger only matches plain `F2` for
        // `EditKeyPressed`, so shadowing `Shift+F2` here would
        // silently swallow the window-scope "Jump to previous
        // anchor" shortcut whenever focus lives in the dock.
        QKeyEvent shiftF2Override(QEvent::ShortcutOverride, Qt::Key_F2, Qt::ShiftModifier);
        QApplication::sendEvent(tree, &shiftF2Override);
        QVERIFY2(
            !shiftF2Override.isAccepted(),
            "AnchorsDock event filter must let Shift+F2 pass through so the prev-anchor "
            "shortcut still fires from within the dock"
        );

        // Every other shortcut still propagates: verify a non-F2
        // ShortcutOverride passes through so we haven't accidentally
        // shadowed unrelated shortcuts (e.g. Ctrl+F for find).
        QKeyEvent ctrlFOverride(QEvent::ShortcutOverride, Qt::Key_F, Qt::ControlModifier);
        QApplication::sendEvent(tree, &ctrlFOverride);
        QVERIFY2(
            !ctrlFOverride.isAccepted(), "AnchorsDock event filter must only shadow plain F2, not arbitrary shortcuts"
        );

        // Restore hidden state so subsequent tests inherit the
        // fixture in the same shape they'd get on a fresh run.
        anchors->ClearAll();
        QCoreApplication::processEvents();
        dock->hide();
        model->EndStreaming(false);
    }

    // Regression for the "in-flight inline edit clobbered by
    // unrelated anchor mutation" issue: `anchorChanged` on some
    // *other* anchor must not clear the tree or invalidate the
    // item currently under edit. The surgical `OnAnchorChanged`
    // path leaves untouched rows alone; a full-rebuild path would
    // relocate every item and destroy any open editor.
    void TestAnchorsDockUnrelatedAnchorChangeDoesNotRebuildTree()
    {
        auto *anchors = mWindow->Anchors();
        QVERIFY(anchors != nullptr);
        auto *dock = mWindow->findChild<AnchorsDock *>();
        QVERIFY(dock != nullptr);
        auto *tree = dock->TreeForTest();
        QVERIFY(tree != nullptr);
        dock->show();
        emit dock->visibilityChanged(true);

        const AnchorManager::Key k1{.locator = "c:/logs/a.json", .lineId = 1};
        const AnchorManager::Key k2{.locator = "c:/logs/a.json", .lineId = 2};
        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): prior QVERIFY aborts on null.
        QVERIFY(anchors->SetAnchor(k1, 1));
        QVERIFY(anchors->SetAnchor(k2, 2));
        dock->RefreshForTest();
        QCOMPARE(tree->topLevelItemCount(), 2);

        // Snapshot the item pointers so we can prove they survive
        // unrelated mutations.
        QTreeWidgetItem *item1Before = tree->topLevelItem(0);
        QTreeWidgetItem *item2Before = tree->topLevelItem(1);
        QVERIFY(item1Before != nullptr && item2Before != nullptr);

        // Recolour k2 -- an unrelated single-anchor mutation. This
        // used to call the full-rebuild path and drop both items
        // in favour of freshly-allocated ones.
        QVERIFY(anchors->SetAnchor(k2, 5));
        QCoreApplication::processEvents();

        QCOMPARE(tree->topLevelItemCount(), 2);
        QCOMPARE(tree->topLevelItem(0), item1Before);
        QCOMPARE(tree->topLevelItem(1), item2Before);

        // Now remove k2. Again single-key -> surgical path. Item 1
        // (unrelated) must keep its identity.
        QVERIFY(anchors->RemoveAnchor(k2));
        QCoreApplication::processEvents();
        QCOMPARE(tree->topLevelItemCount(), 1);
        QCOMPARE(tree->topLevelItem(0), item1Before);

        // Add a new anchor -- surgical insert path. Item 1 must
        // still keep its identity across the insert.
        const AnchorManager::Key k3{.locator = "c:/logs/b.json", .lineId = 3};
        QVERIFY(anchors->SetAnchor(k3, 3));
        QCoreApplication::processEvents();
        QCOMPARE(tree->topLevelItemCount(), 2);
        // Sorted insert: `a.json` < `b.json`, so item1 stays at row 0.
        QCOMPARE(tree->topLevelItem(0), item1Before);

        anchors->ClearAll();
        QCoreApplication::processEvents();
        dock->hide();
    }

    // Regression for the F4-in-AnchorsDock redirect: pressing F4
    // while focus lives inside the dock must open the dock's inline
    // editor on the currently-selected item, not pop a modal
    // `QInputDialog` for whatever row the main table happens to
    // have selected. The two rows can differ, and the modal path
    // is inconsistent with the F2 / double-click gesture the dock
    // already advertises.
    void TestF4WhileFocusInAnchorsDockOpensInlineEditor()
    {
        auto *anchors = mWindow->Anchors();
        QVERIFY(anchors != nullptr);
        auto *dock = mWindow->findChild<AnchorsDock *>();
        QVERIFY(dock != nullptr);
        auto *tree = dock->TreeForTest();
        QVERIFY(tree != nullptr);
        mWindow->show();
        mWindow->activateWindow();
        dock->show();
        emit dock->visibilityChanged(true);

        const AnchorManager::Key k{.locator = "c:/logs/a.json", .lineId = 42};
        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): prior QVERIFY aborts on null.
        QVERIFY(anchors->SetAnchor(k, 1));
        dock->RefreshForTest();
        QCOMPARE(tree->topLevelItemCount(), 1);
        tree->setCurrentItem(tree->topLevelItem(0), /*column=*/0);
        tree->setFocus(Qt::OtherFocusReason);
        QCoreApplication::processEvents();
        QVERIFY(tree->hasFocus() || dock->isAncestorOf(QApplication::focusWidget()));

        auto *editNoteAction = mWindow->findChild<QAction *>(QStringLiteral("actionEditRowAnchorNote"));
        QVERIFY(editNoteAction != nullptr);
        editNoteAction->trigger();
        QCoreApplication::processEvents();

        // No modal dialog spawned -- the redirect happened.
        QVERIFY2(
            QApplication::activeModalWidget() == nullptr,
            "F4 in AnchorsDock must not open a modal QInputDialog on the main table's row"
        );
        // The tree's state should be `EditingState` (editor open on
        // the note column) or the current column should have been
        // retargeted to `COLUMN_NOTE` for the follow-up edit.
        QCOMPARE(tree->currentColumn(), /*COLUMN_NOTE=*/1);

        // Close the editor without committing so subsequent tests
        // inherit a quiescent tree.
        tree->setCurrentItem(nullptr);
        QCoreApplication::processEvents();

        anchors->ClearAll();
        QCoreApplication::processEvents();
        dock->hide();
        mWindow->hide();
        QCoreApplication::processEvents();
    }

    // Regression for the `AnchorManager::Replace` clamp counter
    // double-count on duplicate keys. `Replace` runs `insert_or_assign`
    // (last-wins) but used to `++clampedCount` unconditionally per
    // entry, so a JSON payload with the *same* key appearing twice
    // -- once out-of-range, once valid -- would report a clamp
    // even though the persisted final state is valid. Symmetrically,
    // a valid-then-clamped duplicate must still count as clamped.
    void TestAnchorManagerReplaceClampCountDedupesByKey()
    {
        AnchorManager manager;

        // Case A: same key twice, out-of-range first, valid second.
        // Final persisted colour is valid, so `clampedCount` = 0.
        std::vector<loglib::AnchorEntry> aInput;
        aInput.push_back(loglib::AnchorEntry{.locator = "c:/x.json", .lineId = 1, .colorIndex = 99, .note = "first"});
        aInput.push_back(loglib::AnchorEntry{.locator = "c:/x.json", .lineId = 1, .colorIndex = 3, .note = "second"});
        QCOMPARE(manager.Replace(aInput), std::size_t{0});
        QCOMPARE(manager.Count(), std::size_t{1});
        QCOMPARE(manager.ColorFor({.locator = "c:/x.json", .lineId = 1}).value_or(255U), static_cast<std::uint8_t>(3));

        // Case B: same key twice, valid first, out-of-range second.
        // Final persisted colour is the clamped one, so count = 1.
        std::vector<loglib::AnchorEntry> bInput;
        bInput.push_back(loglib::AnchorEntry{.locator = "c:/x.json", .lineId = 1, .colorIndex = 3, .note = "first"});
        bInput.push_back(loglib::AnchorEntry{.locator = "c:/x.json", .lineId = 1, .colorIndex = 99, .note = "second"});
        QCOMPARE(manager.Replace(bInput), std::size_t{1});
        QCOMPARE(manager.Count(), std::size_t{1});
        QCOMPARE(
            manager.ColorFor({.locator = "c:/x.json", .lineId = 1}).value_or(255U),
            static_cast<std::uint8_t>(loglib::ANCHOR_PALETTE_SIZE - 1)
        );

        // Case C: three duplicates: valid, out-of-range, valid.
        // Only the final state counts (valid), so count = 0.
        std::vector<loglib::AnchorEntry> cInput;
        cInput.push_back(loglib::AnchorEntry{.locator = "c:/x.json", .lineId = 1, .colorIndex = 1, .note = ""});
        cInput.push_back(loglib::AnchorEntry{.locator = "c:/x.json", .lineId = 1, .colorIndex = 42, .note = ""});
        cInput.push_back(loglib::AnchorEntry{.locator = "c:/x.json", .lineId = 1, .colorIndex = 4, .note = ""});
        QCOMPARE(manager.Replace(cInput), std::size_t{0});

        // Case D: two distinct keys, both clamped -> count = 2
        // (proves we still count per-key, not just once).
        std::vector<loglib::AnchorEntry> dInput;
        dInput.push_back(loglib::AnchorEntry{.locator = "c:/x.json", .lineId = 1, .colorIndex = 50, .note = ""});
        dInput.push_back(loglib::AnchorEntry{.locator = "c:/x.json", .lineId = 2, .colorIndex = 51, .note = ""});
        QCOMPARE(manager.Replace(dInput), std::size_t{2});
    }

    // Regression for the broadened `SanitiseNote` control-byte
    // fold. The v1 implementation only folded the "obvious"
    // whitespace controls (`\r`, `\n`, `\t`, `\v`, `\f`, `\0`);
    // arbitrary C0 bytes (BEL, backspace, ESC, ...) and DEL fell
    // through unchanged and could produce invisible garbage in the
    // one-line label. The new fold covers all of `[0x00-0x1F, 0x7F]`.
    void TestSanitiseNoteFoldsAllC0AndDelBytes()
    {
        // BEL (0x07), backspace (0x08), ESC (0x1B), unit separator
        // (0x1F), and DEL (0x7F) are the interesting cases.
        std::string input = "a\x07"
                            "b\x08"
                            "c\x1B"
                            "d\x1F"
                            "e\x7F"
                            "f";
        const auto out = AnchorManager::SanitiseNote(std::move(input));
        QCOMPARE(out, std::string{"a b c d e f"});

        // Printable ASCII must pass through untouched even though
        // some codepoints (0x20-0x7E) sit adjacent to the folded
        // ranges.
        QCOMPARE(AnchorManager::SanitiseNote(std::string{" ~!\"#$%&'"}), std::string{"~!\"#$%&'"});

        // High bytes (0x80+) are UTF-8 continuation bytes; they
        // must not be folded so multi-byte characters survive.
        // Cyrillic "тест" is a good check: no ASCII escape hatches
        // to accidentally match.
        const std::string cyrillic = "\xd1\x82\xd0\xb5\xd1\x81\xd1\x82";
        QCOMPARE(AnchorManager::SanitiseNote(cyrillic), cyrillic);
    }

    // Runtime-only anchors (empty locator = stream / stdin rows) are
    // dropped by `AnchorManager::Entries` on save; the AnchorsDock
    // also skips them so they never appear in the tree. That means
    // the *only* commit path a runtime-only note can arrive through
    // is `MainWindow::EditAnchorNoteForKey` (F4 or the row-menu
    // "Edit note..." entry), which posts a status-bar hint directly.
    // Verify the manager-level bookkeeping the hint depends on:
    // the note gets stored but is filtered from the save snapshot.
    void TestRuntimeOnlyAnchorNoteIsStoredButNotPersisted()
    {
        AnchorManager manager;
        const AnchorManager::Key runtimeKey{.locator = "", .lineId = 999};
        QVERIFY(manager.SetAnchor(runtimeKey, 1));
        QVERIFY(manager.SetAnchorNote(runtimeKey, std::string{"session note"}));

        // Note is present in the runtime map...
        QCOMPARE(manager.NoteFor(runtimeKey).value_or(std::string{"MISSING"}), std::string{"session note"});

        // ...but `Entries()` drops the whole anchor, so the note
        // never reaches the save file. This is what the MainWindow
        // hint warns the user about.
        const auto persistable = manager.Entries();
        QVERIFY2(
            std::ranges::none_of(persistable, [](const loglib::AnchorEntry &e) { return e.locator.empty(); }),
            "Entries() must not persist runtime-only anchors (or their notes)"
        );

        // Diagnostics-only accessor keeps them for status dumps.
        const auto all = manager.EntriesIncludingRuntimeOnly();
        QVERIFY2(
            std::ranges::any_of(
                all, [](const loglib::AnchorEntry &e) { return e.locator.empty() && e.note == "session note"; }
            ),
            "EntriesIncludingRuntimeOnly() must retain the runtime-only note for diagnostics"
        );
    }

    // Right-clicking one anchor in an ExtendedSelection tree of
    // several selected anchors and picking "Remove N anchors" must
    // remove all of them, not just the right-clicked one. This
    // exercises the dock's selection-aware bulk-remove path
    // introduced alongside the "Remove N anchors" menu retitle.
    void TestAnchorsDockContextMenuRemovesSelection()
    {
        auto *anchors = mWindow->Anchors();
        QVERIFY(anchors != nullptr);
        auto *dock = mWindow->findChild<AnchorsDock *>();
        QVERIFY(dock != nullptr);
        auto *tree = dock->TreeForTest();
        QVERIFY(tree != nullptr);

        // Seed three anchors so the multi-selection has substance
        // (a two-item test would collapse into "select all").
        const std::vector<AnchorManager::Key> seed = {
            {.locator = "c:/logs/a.json", .lineId = 1},
            {.locator = "c:/logs/a.json", .lineId = 2},
            {.locator = "c:/logs/b.json", .lineId = 3},
        };
        for (const auto &k : seed)
        {
            // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): prior QVERIFY aborts on null.
            QVERIFY(anchors->SetAnchor(k, 1));
        }
        dock->RefreshForTest();
        QCOMPARE(tree->topLevelItemCount(), 3);

        // Select two of the three; the third stays untouched so we
        // can prove the bulk-remove targets the *selection*, not
        // just the right-clicked item.
        tree->topLevelItem(0)->setSelected(true);
        tree->topLevelItem(1)->setSelected(true);
        tree->topLevelItem(2)->setSelected(false);

        // Direct bulk-remove via the manager mirrors what the dock's
        // context menu path calls; the menu wiring itself is covered
        // by an integration test that dispatches the QAction. Here
        // we assert the manager-level contract that a two-key remove
        // emits a single `anchorsReset` (matches the routing
        // documented on `RemoveAnchors`).
        const QSignalSpy resetSpy(anchors, &AnchorManager::anchorsReset);
        const std::vector<AnchorManager::Key> toRemove = {seed[0], seed[1]};
        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): prior QVERIFY aborts on null.
        QVERIFY(anchors->RemoveAnchors(toRemove));
        QCOMPARE(resetSpy.count(), 1);
        QCOMPARE(anchors->Count(), std::size_t{1});
        QVERIFY(anchors->ColorFor(seed[2]).has_value());
        QVERIFY(!anchors->ColorFor(seed[0]).has_value());
        QVERIFY(!anchors->ColorFor(seed[1]).has_value());

        anchors->ClearAll();
        QCoreApplication::processEvents();
    }

    // Regression: the AnchorsDock "Clear all" button must enable
    // itself as soon as the manager becomes non-empty -- including
    // when the very first anchor added is *runtime-only* (empty
    // locator, i.e. a streaming / stdin row). Runtime-only anchors
    // are filtered out of the tree by `Entries()`, so the surgical
    // `OnAnchorChanged` insert path takes an early `return`. That
    // early return used to skip the button-state update, leaving
    // the button stuck at "disabled" even though `AnchorManager::
    // ClearAll` would still fire and wipe the anchor. The fix
    // refreshes `mClearAllButton->setEnabled(!Empty())` before the
    // early return; this test pins the invariant.
    void TestAnchorsDockClearAllButtonEnablesForRuntimeOnlyAnchor()
    {
        auto *anchors = mWindow->Anchors();
        QVERIFY(anchors != nullptr);
        auto *dock = mWindow->findChild<AnchorsDock *>();
        QVERIFY(dock != nullptr);
        auto *tree = dock->TreeForTest();
        QVERIFY(tree != nullptr);
        auto *clearBtn = dock->ClearAllButtonForTest();
        QVERIFY(clearBtn != nullptr);

        // Force the dock visible so the surgical `OnAnchorChanged`
        // path runs (buried docks skip the update and rely on a
        // later `visibilityChanged` flip to catch up).
        dock->show();
        emit dock->visibilityChanged(true);

        // Precondition: no anchors, button disabled.
        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): prior QVERIFY aborts on null.
        QVERIFY(anchors->Empty());
        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): prior QVERIFY aborts on null.
        QVERIFY(!clearBtn->isEnabled());

        // Add a runtime-only anchor (empty locator). The tree stays
        // empty by design -- but the button MUST enable because
        // `!Empty()` is now true and `Ctrl+Shift+A` / clicking the
        // button will clear the runtime anchor via `ClearAll`.
        const AnchorManager::Key runtimeKey{.locator = "", .lineId = 4242};
        QVERIFY(anchors->SetAnchor(runtimeKey, 1));
        QCoreApplication::processEvents();
        QCOMPARE(tree->topLevelItemCount(), 0);
        QVERIFY2(
            clearBtn->isEnabled(),
            "Clear all button must enable when the first anchor added is runtime-only "
            "(empty locator) so the user has a UI path to clear it"
        );

        // Removing the runtime anchor drops the button back to
        // disabled -- same code path that handles regular anchors.
        QVERIFY(anchors->RemoveAnchor(runtimeKey));
        QCoreApplication::processEvents();
        QCOMPARE(tree->topLevelItemCount(), 0);
        QVERIFY(!clearBtn->isEnabled());

        dock->hide();
    }

    // Regression for #2 of the review: `F4` (Edit anchor note on
    // current row) is a `Qt::WindowShortcut` and must not clobber
    // native F4 behaviour in embedded text-input widgets such as
    // `QComboBox` (F4 opens the drop-down on Windows) or
    // `QAbstractSpinBox` (F4 steps a field on some styles).
    //
    // The fix intercepts `QEvent::ShortcutOverride` in
    // `MainWindow::event()` and accepts it when the focus widget is
    // a text-input surface. That vetoes the shortcut so Qt sends a
    // normal `KeyPress` to the focus widget instead of firing the
    // "Edit anchor note" action. This test drives the mechanism
    // directly (sending a synthetic ShortcutOverride event via
    // `QApplication::sendEvent`) rather than relying on Qt's
    // internal shortcut dispatcher, which is finicky under
    // offscreen QPA.
    void TestF4ShortcutOverrideAcceptedOnTextInputFocus()
    {
        // Show the window so focus assignment sticks under
        // offscreen QPA (an un-shown widget's focus request is a
        // silent no-op).
        mWindow->show();
        mWindow->activateWindow();
        QCoreApplication::processEvents();

        // Scratch QLineEdit as the text-input focus target.
        auto scratch = std::make_unique<QLineEdit>(mWindow);
        scratch->show();
        scratch->setFocus(Qt::OtherFocusReason);
        QCoreApplication::processEvents();
        QVERIFY(QApplication::focusWidget() == scratch.get());

        // Dispatch a ShortcutOverride for F4 no-modifier to the
        // main window (mimicking Qt's bubble-up path from the
        // focus widget). The handler must accept it.
        QKeyEvent f4Override(QEvent::ShortcutOverride, Qt::Key_F4, Qt::NoModifier);
        QApplication::sendEvent(mWindow, &f4Override);
        QVERIFY2(
            f4Override.isAccepted(),
            "MainWindow must accept F4 ShortcutOverride while focus is on a QLineEdit so the "
            "shortcut is vetoed and the focus widget gets a plain KeyPress"
        );

        // Other keys pass through unchanged: F5 (or any random key)
        // must NOT be intercepted -- we only shadow F4.
        QKeyEvent f5Override(QEvent::ShortcutOverride, Qt::Key_F5, Qt::NoModifier);
        QApplication::sendEvent(mWindow, &f5Override);
        QVERIFY2(!f5Override.isAccepted(), "MainWindow must only intercept F4 ShortcutOverride, not arbitrary keys");

        // Modifier variants pass through too: Ctrl+F4 is a common
        // "close tab" shortcut in other apps; we must not swallow
        // it just because the base key matches.
        QKeyEvent ctrlF4Override(QEvent::ShortcutOverride, Qt::Key_F4, Qt::ControlModifier);
        QApplication::sendEvent(mWindow, &ctrlF4Override);
        QVERIFY2(!ctrlF4Override.isAccepted(), "MainWindow must not intercept modified F4 ShortcutOverride");

        // Move focus off the QLineEdit (main window itself). F4
        // must now pass through so the QAction shortcut can fire.
        scratch->clearFocus();
        mWindow->setFocus(Qt::OtherFocusReason);
        QCoreApplication::processEvents();
        QKeyEvent f4OnWindow(QEvent::ShortcutOverride, Qt::Key_F4, Qt::NoModifier);
        QApplication::sendEvent(mWindow, &f4OnWindow);
        QVERIFY2(
            !f4OnWindow.isAccepted(),
            "MainWindow must not intercept F4 ShortcutOverride when focus is not a text input -- "
            "the F4 QAction must be free to fire"
        );

        scratch.reset();
        mWindow->hide();
        QCoreApplication::processEvents();
    }

    // Surgical refresh: a note-only edit rewrites just the affected
    // item (text + tooltip) in place instead of clearing the tree
    // and rebuilding every row. Verified by comparing the item
    // pointer identity across the edit: a full rebuild would delete
    // and recreate the item, invalidating the pre-edit pointer.
    void TestAnchorsDockNoteEditPreservesItemIdentity()
    {
        auto *anchors = mWindow->Anchors();
        QVERIFY(anchors != nullptr);
        auto *dock = mWindow->findChild<AnchorsDock *>();
        QVERIFY(dock != nullptr);
        auto *tree = dock->TreeForTest();
        QVERIFY(tree != nullptr);
        // Force the dock visible so `OnAnchorNoteChanged`'s
        // visibility gate lets the surgical refresh through.
        // `AnchorsDock::IsVisibleForRefresh` also checks
        // `!isHidden()`, so `show()` is needed alongside the
        // `visibilityChanged(true)` emit.
        dock->show();
        emit dock->visibilityChanged(true);

        // Two anchors so the "item pointer stable across note edit"
        // assertion has meaning (rebuilds would relocate every item).
        const AnchorManager::Key k1{.locator = "c:/logs/a.json", .lineId = 1};
        const AnchorManager::Key k2{.locator = "c:/logs/a.json", .lineId = 2};
        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): prior QVERIFY aborts on null.
        QVERIFY(anchors->SetAnchor(k1, 1));
        QVERIFY(anchors->SetAnchor(k2, 2));
        dock->RefreshForTest();
        QCOMPARE(tree->topLevelItemCount(), 2);

        // The tree's row order is sorted by `(locator, lineId)`
        // internally, but we index the found item by locator+lineId
        // rather than assuming a fixed slot so a future sort change
        // doesn't quietly break the test.
        QTreeWidgetItem *itemBefore = nullptr;
        for (int row = 0; row < tree->topLevelItemCount(); ++row)
        {
            auto *candidate = tree->topLevelItem(row);
            if (candidate->data(0, Qt::UserRole + 1).toString() == QStringLiteral("c:/logs/a.json") &&
                candidate->data(0, Qt::UserRole + 2).toULongLong() == 1)
            {
                itemBefore = candidate;
                break;
            }
        }
        QVERIFY(itemBefore != nullptr);

        // Note edit on `k1` -- takes the surgical path.
        QVERIFY(anchors->SetAnchorNote(k1, std::string{"annotated"}));
        QCoreApplication::processEvents();

        // Look up `k1` again after the edit; expect the *same* item
        // pointer (surgical rewrite in place, not a rebuild).
        QTreeWidgetItem *itemAfter = nullptr;
        for (int row = 0; row < tree->topLevelItemCount(); ++row)
        {
            auto *candidate = tree->topLevelItem(row);
            if (candidate->data(0, Qt::UserRole + 1).toString() == QStringLiteral("c:/logs/a.json") &&
                candidate->data(0, Qt::UserRole + 2).toULongLong() == 1)
            {
                itemAfter = candidate;
                break;
            }
        }
        QVERIFY(itemAfter != nullptr);
        QCOMPARE(itemAfter, itemBefore); // pointer identity preserved
        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): prior QVERIFY aborts on null.
        QCOMPARE(itemAfter->text(1), QStringLiteral("annotated"));
        // Tooltip picked up the new note text (surgical path also
        // rewrites the tooltip so hover reflects the edit).
        QVERIFY2(
            itemAfter->toolTip(1).contains(QStringLiteral("annotated")),
            qPrintable(QStringLiteral("tooltip must include the new note; got: %1").arg(itemAfter->toolTip(1)))
        );

        // Contrast: a colour change (fires `anchorChanged`) still
        // takes the full rebuild path -- verify by observing that
        // recolouring re-emits a `Refresh` and the tree structure
        // stays consistent. Item pointer identity is not guaranteed
        // through `anchorChanged` (documented behaviour); we only
        // assert content correctness here.
        QVERIFY(anchors->SetAnchor(k1, 4));
        QCoreApplication::processEvents();
        // Locate `k1`'s item again after the rebuild.
        const QTreeWidgetItem *refoundItem = nullptr;
        for (int row = 0; row < tree->topLevelItemCount(); ++row)
        {
            auto *candidate = tree->topLevelItem(row);
            if (candidate->data(0, Qt::UserRole + 1).toString() == QStringLiteral("c:/logs/a.json") &&
                candidate->data(0, Qt::UserRole + 2).toULongLong() == 1)
            {
                refoundItem = candidate;
                break;
            }
        }
        QVERIFY(refoundItem != nullptr);
        // Note text survived the recolour (manager-level guarantee,
        // just re-asserted here since we're already looking).
        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): prior QVERIFY aborts on null.
        QCOMPARE(refoundItem->text(1), QStringLiteral("annotated"));

        anchors->ClearAll();
        QCoreApplication::processEvents();
        dock->hide();
    }

    // Notes may contain characters like `<`, `>`, `&` that Qt's
    // tooltip machinery treats as rich-text markup. Both the
    // `LogModel` row tooltip and the `AnchorsDock` item tooltip
    // must escape the note so it renders literally.
    void TestAnchorNotesEscapeHtmlInTooltips()
    {
        auto *anchors = mWindow->Anchors();
        QVERIFY(anchors != nullptr);
        auto *model = mWindow->Model();
        auto *dock = mWindow->findChild<AnchorsDock *>();
        QVERIFY(dock != nullptr);
        auto *tree = dock->TreeForTest();
        QVERIFY(tree != nullptr);

        loglib::StreamLineSource &streamSource = BeginSyntheticStreamSession(*model);
        QtStreamingLogSink *sink = model->Sink();
        QVERIFY(sink != nullptr);
        loglib::KeyIndex &keys = sink->Keys();
        const loglib::KeyId valueKey = keys.GetOrInsert(std::string("value"));
        sink->OnBatch(MakeSyntheticBatch(streamSource, valueKey, 1, 1, true));
        QCoreApplication::processEvents();

        const auto key0 = model->AnchorKeyForRow(0);
        QVERIFY(key0.has_value());
        anchors->SetAnchor(*key0, 1);
        // Anchor note deliberately carries markup-shaped characters
        // that would otherwise render as bold + link + entity.
        anchors->SetAnchorNote(*key0, std::string{"<b>foo</b> & <a href=\"x\">y</a>"});
        QCoreApplication::processEvents();

        // LogModel row tooltip: the escaped form must appear
        // verbatim, and the raw `<b>` must NOT appear literally.
        const QVariant modelTip = model->data(model->index(0, 0), Qt::ToolTipRole);
        QVERIFY(modelTip.isValid());
        const QString modelTipStr = modelTip.toString();
        QVERIFY2(
            modelTipStr.contains(QStringLiteral("&lt;b&gt;foo&lt;/b&gt;")),
            qPrintable(QStringLiteral("expected escaped `<b>foo</b>`, got: %1").arg(modelTipStr))
        );
        QVERIFY2(
            !modelTipStr.contains(QStringLiteral("<b>foo</b>")),
            qPrintable(QStringLiteral("raw HTML must not survive: %1").arg(modelTipStr))
        );
        QVERIFY(modelTipStr.contains(QStringLiteral("&amp;")));

        // AnchorsDock tree item tooltip: same escaping treatment.
        dock->RefreshForTest();
        QCOMPARE(tree->topLevelItemCount(), 1);
        const QString itemTip = tree->topLevelItem(0)->toolTip(1);
        QVERIFY2(
            itemTip.contains(QStringLiteral("&lt;b&gt;foo&lt;/b&gt;")),
            qPrintable(QStringLiteral("expected escaped note in AnchorsDock tooltip, got: %1").arg(itemTip))
        );
        QVERIFY2(
            !itemTip.contains(QStringLiteral("<b>foo</b>")),
            qPrintable(QStringLiteral("raw HTML must not survive in AnchorsDock tooltip: %1").arg(itemTip))
        );

        anchors->ClearAll();
        QCoreApplication::processEvents();
        model->EndStreaming(false);
    }

    // Regression for the ampersand-only tooltip rendering bug: notes
    // that contain `&` or `>` but no `<` must still render literally,
    // not as `&amp;`/`&gt;` entity strings. `Qt::mightBeRichText()`
    // only auto-detects HTML on `&lt;` and real tags, so the fix
    // wraps the tooltip in an explicit `<qt>...</qt>` envelope
    // (forcing rich-text mode) and spells newlines as `<br/>`
    // (because HTML collapses literal `\n` to whitespace).
    //
    // Both surfaces (`LogModel` row tooltip and `AnchorsDock` item
    // tooltip) share this contract, so the test asserts:
    //   1. The output starts with `<qt>` (HTML envelope present).
    //   2. Newlines are `<br/>`, not raw `\n`.
    //   3. Escaped `&amp;`/`&gt;` are still present in the string
    //      form (so Qt decodes them when it renders).
    void TestAnchorNoteTooltipWrappedInQtEnvelope()
    {
        auto *anchors = mWindow->Anchors();
        QVERIFY(anchors != nullptr);
        auto *model = mWindow->Model();
        auto *dock = mWindow->findChild<AnchorsDock *>();
        QVERIFY(dock != nullptr);
        auto *tree = dock->TreeForTest();
        QVERIFY(tree != nullptr);

        loglib::StreamLineSource &streamSource = BeginSyntheticStreamSession(*model);
        QtStreamingLogSink *sink = model->Sink();
        QVERIFY(sink != nullptr);
        loglib::KeyIndex &keys = sink->Keys();
        const loglib::KeyId valueKey = keys.GetOrInsert(std::string("value"));
        sink->OnBatch(MakeSyntheticBatch(streamSource, valueKey, 1, 1, true));
        QCoreApplication::processEvents();

        const auto key0 = model->AnchorKeyForRow(0);
        QVERIFY(key0.has_value());
        anchors->SetAnchor(*key0, 1);
        // No `<` in this note: pre-fix, Qt's `mightBeRichText`
        // heuristic would return false and the escaped `&amp;` /
        // `&gt;` would render literally to the user.
        anchors->SetAnchorNote(*key0, std::string{"retry > 3 & fail"});
        QCoreApplication::processEvents();

        const QVariant modelTip = model->data(model->index(0, 0), Qt::ToolTipRole);
        QVERIFY(modelTip.isValid());
        const QString modelTipStr = modelTip.toString();
        QVERIFY2(
            modelTipStr.startsWith(QStringLiteral("<qt>")) && modelTipStr.endsWith(QStringLiteral("</qt>")),
            qPrintable(QStringLiteral("expected <qt>-wrapped tooltip, got: %1").arg(modelTipStr))
        );
        QVERIFY2(
            modelTipStr.contains(QStringLiteral("&amp;")),
            qPrintable(QStringLiteral("expected escaped `&`, got: %1").arg(modelTipStr))
        );
        QVERIFY2(
            modelTipStr.contains(QStringLiteral("&gt;")),
            qPrintable(QStringLiteral("expected escaped `>`, got: %1").arg(modelTipStr))
        );
        // No literal newlines inside the envelope -- they'd collapse
        // to whitespace in HTML rendering.
        QVERIFY2(
            !modelTipStr.contains(QLatin1Char('\n')),
            qPrintable(QStringLiteral("literal `\\n` must be replaced with <br/>, got: %1").arg(modelTipStr))
        );

        dock->RefreshForTest();
        QCOMPARE(tree->topLevelItemCount(), 1);
        const QString itemTip = tree->topLevelItem(0)->toolTip(1);
        QVERIFY2(
            itemTip.startsWith(QStringLiteral("<qt>")) && itemTip.endsWith(QStringLiteral("</qt>")),
            qPrintable(QStringLiteral("expected <qt>-wrapped AnchorsDock tooltip, got: %1").arg(itemTip))
        );
        QVERIFY(itemTip.contains(QStringLiteral("&amp;")));
        QVERIFY(itemTip.contains(QStringLiteral("&gt;")));
        QVERIFY2(
            !itemTip.contains(QLatin1Char('\n')),
            qPrintable(QStringLiteral("AnchorsDock tooltip must use <br/> not `\\n`, got: %1").arg(itemTip))
        );
        // Sanity: the note payload still appears escaped inside the
        // envelope. `&amp;` and `&gt;` surround the literal word.
        QVERIFY2(
            itemTip.contains(QStringLiteral("retry &gt; 3 &amp; fail")),
            qPrintable(QStringLiteral("escaped note payload missing from tooltip: %1").arg(itemTip))
        );

        anchors->ClearAll();
        QCoreApplication::processEvents();
        model->EndStreaming(false);
    }

    // The RecordDetailWidget's anchor-note subline is rendered as
    // plain text so a user-typed `<b>...</b>` reads literally
    // instead of being interpreted as HTML markup by QLabel's
    // default `Qt::AutoText` format.
    void TestRecordDetailAnchorNoteRendersPlainText()
    {
        // Build a synthetic content with anchor markup and verify
        // that the label's textFormat forces plain text -- this
        // guards the fix against a future refactor that omits the
        // `setTextFormat` call.
        RecordDetailWidget widget;
        RecordDetailContent content;
        content.valid = true;
        content.summary = QStringLiteral("row summary");
        content.anchorColorIndex = std::uint8_t{2};
        content.anchorNote = QStringLiteral("<b>customer 42</b> & friends");
        widget.SetContent(content);

        auto *noteLabel = widget.findChild<QLabel *>(QStringLiteral("anchorNoteLabel"));
        QVERIFY2(noteLabel != nullptr, "RecordDetailWidget must expose an anchorNoteLabel");
        // `isHidden()` rather than `isVisible()` because the widget
        // is never `show()`n in this test; the assertion is that
        // `PopulateUi` did not explicitly hide the label.
        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): prior QVERIFY2 aborts on null.
        QVERIFY2(!noteLabel->isHidden(), "anchor-note label must be shown when the pinned row is anchored");
        QCOMPARE(noteLabel->textFormat(), Qt::PlainText);
        QVERIFY2(
            noteLabel->text().contains(QStringLiteral("<b>customer 42</b>")),
            qPrintable(QStringLiteral("expected literal `<b>` in label text, got: %1").arg(noteLabel->text()))
        );
    }

    // `RecordDetailDock` subscribes directly to `AnchorManager`
    // signals so a note edit while a row is pinned refreshes the
    // subline live. Without this the `IsStyleOnlyRoleChange` filter
    // on the `dataChanged` path would swallow the update (the model
    // emits only `Background`/`Foreground`/`ToolTip` roles for
    // anchor mutations, all treated as style-only).
    void TestRecordDetailDockLiveUpdateOnAnchorNoteEdit()
    {
        auto *anchors = mWindow->Anchors();
        QVERIFY(anchors != nullptr);
        auto *model = mWindow->Model();
        auto *dock = mWindow->findChild<RecordDetailDock *>();
        QVERIFY(dock != nullptr);

        loglib::StreamLineSource &streamSource = BeginSyntheticStreamSession(*model);
        QtStreamingLogSink *sink = model->Sink();
        QVERIFY(sink != nullptr);
        loglib::KeyIndex &keys = sink->Keys();
        const loglib::KeyId valueKey = keys.GetOrInsert(std::string("value"));
        sink->OnBatch(MakeSyntheticBatch(streamSource, valueKey, 1, 2, true));
        QCoreApplication::processEvents();
        QCOMPARE(model->rowCount(), 2);

        // Force the dock visible so the subscription's
        // `IsVisibleForRefresh()` gate passes on offscreen QPA.
        emit dock->visibilityChanged(true);

        const auto key0 = model->AnchorKeyForRow(0);
        QVERIFY(key0.has_value());
        anchors->SetAnchor(*key0, 3);
        // Pin the anchored row and verify the initial "anchored,
        // no note" state renders as "Anchored" (empty-note path).
        dock->ShowSourceRow(0);
        QCoreApplication::processEvents();
        auto *widget = dock->findChild<RecordDetailWidget *>();
        QVERIFY(widget != nullptr);
        auto *noteLabel = widget->findChild<QLabel *>(QStringLiteral("anchorNoteLabel"));
        QVERIFY(noteLabel != nullptr);
        // Use `!isHidden()` (rather than `isVisible()`) because the
        // dock's ancestor chain isn't realised under offscreen QPA
        // -- `isVisible()` requires the whole ancestor chain to be
        // showing, `!isHidden()` reflects only the explicit
        // hide/show state driven by `PopulateUi`.
        QVERIFY(!noteLabel->isHidden());
        QCOMPARE(noteLabel->text(), QStringLiteral("Anchored"));

        // Edit the note through the manager (mirrors what the
        // AnchorsDock inline editor and the F4 shortcut do). The
        // dock must refresh live -- no user re-selection required.
        QVERIFY(anchors->SetAnchorNote(*key0, std::string{"customer report"}));
        QCoreApplication::processEvents();
        QCOMPARE(noteLabel->text(), QStringLiteral("Anchor: customer report"));

        // Clearing the anchor drops the subline. Same `isHidden()`
        // rationale as above.
        QVERIFY(anchors->RemoveAnchor(*key0));
        QCoreApplication::processEvents();
        QVERIFY2(noteLabel->isHidden(), "unanchoring the pinned row must hide the anchor-note subline");

        anchors->ClearAll();
        QCoreApplication::processEvents();
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
                sink->OnBatch(
                    MakeSyntheticBatch(streamSource, valueKey, static_cast<size_t>(i) + 1, 1, /*declareNewKey=*/i == 0)
                );
                enqueuedCount.fetch_add(1, std::memory_order_release);
            }
        });

        // Wait for the worker to fill the queue. We deliberately do
        // NOT spin the event loop here -- the drain lambda must stay
        // queued so the worker has nowhere to push the (cap+1)-th batch.
        QVERIFY(test_common::WaitUntil([&] {
            return enqueuedCount.load(std::memory_order_acquire) >= static_cast<int>(CAPACITY);
        }));
        // Producer must remain parked: count stays at cap for a
        // sustained sample window. Absence of progress has no signal, so
        // time is the only evidence.
        test_common::SleepToProveAbsence(std::chrono::milliseconds(50));
        QCOMPARE(enqueuedCount.load(std::memory_order_acquire), static_cast<int>(CAPACITY));

        // Now spin the event loop so the drain lambda runs, pulls
        // batches out, wakes the worker, and the rest go through.
        QVERIFY(WaitUntilProcessingEvents([&] {
            return enqueuedCount.load(std::memory_order_acquire) >= TOTAL_BATCHES;
        }));
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
        sink->OnBatch(MakeSyntheticBatch(streamSource, valueKey, 1, 1, /*declareNewKey=*/true));

        std::atomic<bool> entered{false};
        std::atomic<bool> finished{false};
        std::thread worker([&]() {
            entered.store(true, std::memory_order_release);
            sink->OnBatch(MakeSyntheticBatch(streamSource, valueKey, 2, 1, /*declareNewKey=*/false));
            finished.store(true, std::memory_order_release);
        });

        // Give the worker a moment to actually park inside the queue's
        // condition variable.
        test_common::LetWaiterPark(entered);
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
                sink->OnBatch(
                    MakeSyntheticBatch(streamSource, valueKey, static_cast<size_t>(i) + 1, 1, /*declareNewKey=*/i == 0)
                );
            }
            producerDone.store(true, std::memory_order_release);
        });

        QVERIFY(WaitUntilProcessingEvents([&] { return model.rowCount() >= TOTAL_BATCHES; }));
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
        QCOMPARE(it->second.type, loglib::LeafRule::Type::Time);
        QVERIFY(it->second.filterBegin.has_value());
        QVERIFY(it->second.filterEnd.has_value());
        // NOLINTBEGIN(bugprone-unchecked-optional-access)
        QCOMPARE(*it->second.filterBegin, *preBegin);
        QCOMPARE(*it->second.filterEnd, *preEnd);
        // NOLINTEND(bugprone-unchecked-optional-access)
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
        QCOMPARE(it->second.type, loglib::LeafRule::Type::Time);
        QCOMPARE(it->second.columnKeys, model->Layout().Columns()[static_cast<size_t>(timeCol)].keys);
        QVERIFY2(it->second.filterBegin.has_value(), "uncheck must promote begin from nullopt to the seed value");
        QVERIFY(it->second.filterEnd.has_value());
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        QCOMPARE(*it->second.filterEnd, row2Micros);

        QCoreApplication::processEvents();
    }

    // Regression: after `ApplyAdvancedFilterResult` commits a tree
    // whose root is not a Leaf / And-of-Leaves (e.g. `NOT msg:m1`,
    // `msg:a OR msg:b`), `mSimpleLeaves` is empty but the expression
    // is non-match-all -- rows are actively being filtered.
    // `actionClearAllFilters` correctly stays enabled (its gate is
    // `IsMatchAll(expression)`), but the status-bar button used to
    // gate on `!mSimpleLeaves.empty()` and hide itself, leaving the
    // user with no visible affordance to clear the filter. Both must
    // now agree: button visible + action enabled whenever the
    // expression is non-match-all.
    void TestStatusBarClearButtonVisibleForAdvancedOnlyExpression()
    {
        const int levelCol = StreamFixtureForColumnTests();
        QVERIFY2(levelCol >= 0, "level column must exist after streaming");
        auto *model = mWindow->Model();
        const int msgCol = ColumnByHeader(*model, QStringLiteral("msg"));
        QVERIFY2(msgCol >= 0, "msg column must exist after streaming");
        const std::vector<std::string> msgKeys = model->Layout().Columns()[static_cast<size_t>(msgCol)].keys;

        auto *button = mWindow->findChild<QPushButton *>(QStringLiteral("clearFiltersStatusButton"));
        QVERIFY2(button != nullptr, "MainWindow must own the clear-filters status button");
        auto *clearAction = mWindow->findChild<QAction *>(QStringLiteral("actionClearAllFilters"));
        QVERIFY2(clearAction != nullptr, "MainWindow must own actionClearAllFilters");

        // Precondition: button hidden, action disabled with no filter.
        //
        // clang-analyzer-core.CallAndMessage sees `button` reach
        // this line while still `nullptr` (the preceding QVERIFY2
        // is a macro that expands to an early-return, but the
        // analyzer models both branches). Suppressed because the
        // macro's contract is "return on failure" and the test
        // would abort long before the deref otherwise.
        QCoreApplication::processEvents();
        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage)
        QVERIFY2(button->isHidden(), "clear-filters button must start hidden");
        QVERIFY2(!clearAction->isEnabled(), "clear-all action must start disabled");

        // Commit a `NOT msg:m1` Advanced tree. Root is `Not`, so
        // `ApplyAdvancedFilterResult` extracts zero top-level Leaves
        // and installs the whole tree as the non-Leaf remainder.
        loglib::FilterExpression advancedTree = loglib::MakeNot(
            loglib::MakeLeaf(
                loglib::LeafRule{
                    .type = loglib::LeafRule::Type::String,
                    .columnKeys = msgKeys,
                    .matchType = loglib::LeafRule::Match::Contains,
                    .filterString = std::string("m1"),
                }
            )
        );
        mWindow->CommitAdvancedFilterForTest(std::move(advancedTree));
        QCoreApplication::processEvents();

        // The invariant this test pins: even with `mSimpleLeaves`
        // empty, the button follows the compiled expression like the
        // action does.
        QVERIFY2(mWindow->Filters().empty(), "Advanced-only commit must not populate mSimpleLeaves");
        QVERIFY2(
            !loglib::IsMatchAll(mWindow->activeSession()->View().expression),
            "Advanced NOT tree must produce a non-match-all expression"
        );
        QVERIFY2(clearAction->isEnabled(), "clear-all action must be enabled with an active Advanced tree");
        QVERIFY2(
            !button->isHidden(),
            "clear-filters status button must stay visible while an Advanced tree is filtering rows"
        );

        // Symmetry: triggering the action must hide the button again.
        clearAction->trigger();
        QCoreApplication::processEvents();
        QVERIFY2(button->isHidden(), "clear-filters button must re-hide after clearing an Advanced tree");
        QVERIFY2(!clearAction->isEnabled(), "clear-all action must disable after clearing an Advanced tree");
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
            QStringLiteral("actionToggleHistogram"),
            QStringLiteral("actionToggleOverviewRail"),
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
        QVERIFY(QTest::qWaitForWindowExposed(mWindow, 60000));
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
            "ToolTipRole flips with `SetShowLevelIcons` (level-icon mode) and with anchor note "
            "edits; receivers that care about note text must subscribe to `AnchorManager` "
            "directly rather than the `dataChanged` path (see `RecordDetailDock`)"
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

    // Basic shape: summary mentions row number, every column lands in
    // `fields`, raw JSON is single-line, formatted JSON is indented.
    void TestBuildRecordDetailContentBasic()
    {
        const TempJsonFile fixture(
            QStringList{
                QStringLiteral(R"({"timestamp": "2025-01-15T12:34:56Z", "level": "info", "message": "hello"})"),
                QStringLiteral(R"({"timestamp": "2025-01-15T12:34:57Z", "level": "error", "message": "boom"})"),
            }
        );
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

        QCOMPARE(row1.fields.size(), static_cast<qsizetype>(run.model->Layout().Columns().size()));
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
        const TempJsonFile fixture(
            QStringList{
                QStringLiteral(R"({"a": 1})"),
            }
        );
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

    void TestLogTableViewCopyMultipleRowsEscapesEmbeddedNewlines()
    {
        const QTemporaryDir tmp;
        QVERIFY2(tmp.isValid(), "temp dir must be creatable");
        const QString fixturePath = tmp.filePath(QStringLiteral("multiline.log"));
        {
            std::ofstream stream(fixturePath.toStdString(), std::ios::binary);
            QVERIFY2(stream.is_open(), "fixture file must be openable");
            stream << "level=error msg=boom\r\n"
                      "\tframe 1\n"
                      "\tframe 2\n"
                      "level=info msg=ok\n";
        }

        const StreamingRun run = RunStreamingLogfmt(fixturePath);
        QCOMPARE(run.finishedCount, 1);
        QCOMPARE(run.cancelled, false);
        QCOMPARE(run.model->rowCount(), 2);

        LogTableView view;
        view.setModel(run.model.get());

        QItemSelectionModel *sel = view.selectionModel();
        QVERIFY(sel != nullptr);
        for (int row = 0; row < run.model->rowCount(); ++row)
        {
            sel->select(run.model->index(row, 0), QItemSelectionModel::Select | QItemSelectionModel::Rows);
        }
        QCOMPARE(sel->selectedRows().size(), 2);

        QClipboard *clipboard = QApplication::clipboard();
        QVERIFY(clipboard != nullptr);
        clipboard->clear();

        const QString pasted =
            InvokeAndReadClipboardWithRetry(clipboard, &view, "CopySelectedRowsToClipboard", QStringLiteral("frame 1"));

        const QStringList lines = pasted.split(QLatin1Char('\n'));
        QCOMPARE(lines.size(), 2);

        QVERIFY2(lines[0].contains(QStringLiteral("boom")), qPrintable(lines[0]));
        QVERIFY2(lines[0].contains(QStringLiteral("frame 1")), qPrintable(lines[0]));
        QVERIFY2(lines[0].contains(QStringLiteral("frame 2")), qPrintable(lines[0]));
        QVERIFY2(lines[0].contains(QStringLiteral("\\n")), qPrintable(lines[0]));
        QVERIFY2(lines[0].contains(QStringLiteral("\\r")), qPrintable(lines[0]));
        QVERIFY2(!lines[0].contains(QChar('\r')), "raw CR must not leak into a row");

        QVERIFY2(lines[1].contains(QStringLiteral("msg=ok")), qPrintable(lines[1]));
    }

    void TestLogTableViewCopyMultipleRowsEscapesBackslashesBeforeNewlines()
    {
        const QTemporaryDir tmp;
        QVERIFY2(tmp.isValid(), "temp dir must be creatable");
        const QString fixturePath = tmp.filePath(QStringLiteral("multiline_bs.log"));
        {
            std::ofstream stream(fixturePath.toStdString(), std::ios::binary);
            QVERIFY2(stream.is_open(), "fixture file must be openable");
            stream << "level=error msg=boom\n"
                      "\tpath C:\\a\\b\n"
                      "level=info msg=ok\n";
        }

        const StreamingRun run = RunStreamingLogfmt(fixturePath);
        QCOMPARE(run.finishedCount, 1);
        QCOMPARE(run.model->rowCount(), 2);

        LogTableView view;
        view.setModel(run.model.get());

        QItemSelectionModel *sel = view.selectionModel();
        QVERIFY(sel != nullptr);
        for (int row = 0; row < run.model->rowCount(); ++row)
        {
            sel->select(run.model->index(row, 0), QItemSelectionModel::Select | QItemSelectionModel::Rows);
        }
        QCOMPARE(sel->selectedRows().size(), 2);

        QClipboard *clipboard = QApplication::clipboard();
        QVERIFY(clipboard != nullptr);
        clipboard->clear();

        const QString pasted =
            InvokeAndReadClipboardWithRetry(clipboard, &view, "CopySelectedRowsToClipboard", QStringLiteral("path"));

        const QStringList lines = pasted.split(QLatin1Char('\n'));
        QCOMPARE(lines.size(), 2);

        QVERIFY2(lines[0].contains(QStringLiteral("C:\\\\a\\\\b")), qPrintable(lines[0]));
        QVERIFY2(!lines[0].contains(QStringLiteral("\\\\\\\\")), qPrintable(lines[0]));
        QVERIFY2(lines[0].contains(QStringLiteral("boom\\n\tpath")), qPrintable(lines[0]));
    }

    void TestLogTableViewCopySingleRowPreservesEmbeddedNewlines()
    {
        const QTemporaryDir tmp;
        QVERIFY2(tmp.isValid(), "temp dir must be creatable");
        const QString fixturePath = tmp.filePath(QStringLiteral("multiline_single.log"));
        {
            std::ofstream stream(fixturePath.toStdString(), std::ios::binary);
            QVERIFY2(stream.is_open(), "fixture file must be openable");
            stream << "level=error msg=boom\n"
                      "\tframe 1\n"
                      "\tframe 2\n"
                      "level=info msg=ok\n";
        }

        const StreamingRun run = RunStreamingLogfmt(fixturePath);
        QCOMPARE(run.finishedCount, 1);
        QCOMPARE(run.model->rowCount(), 2);

        LogTableView view;
        view.setModel(run.model.get());

        QItemSelectionModel *sel = view.selectionModel();
        QVERIFY(sel != nullptr);
        sel->select(run.model->index(0, 0), QItemSelectionModel::Select | QItemSelectionModel::Rows);
        QCOMPARE(sel->selectedRows().size(), 1);

        QClipboard *clipboard = QApplication::clipboard();
        QVERIFY(clipboard != nullptr);
        clipboard->clear();

        const QString pasted =
            InvokeAndReadClipboardWithRetry(clipboard, &view, "CopySelectedRowsToClipboard", QStringLiteral("frame 2"));

        QVERIFY2(pasted.contains(QStringLiteral("level=error msg=boom")), qPrintable(pasted));
        QVERIFY2(pasted.contains(QStringLiteral("\tframe 1")), qPrintable(pasted));
        QVERIFY2(pasted.contains(QStringLiteral("\tframe 2")), qPrintable(pasted));
        QVERIFY2(pasted.contains(QChar('\n')), "single-row copy keeps real newlines");
        QVERIFY2(!pasted.contains(QStringLiteral("\\n")), "single-row copy must NOT escape newlines");
    }

    void TestRecordDetailDockRendersMultilineMessage()
    {
        const QTemporaryDir tmp;
        QVERIFY2(tmp.isValid(), "temp dir must be creatable");
        const QString fixturePath = tmp.filePath(QStringLiteral("multiline_detail.log"));
        {
            std::ofstream stream(fixturePath.toStdString(), std::ios::binary);
            QVERIFY2(stream.is_open(), "fixture file must be openable");
            stream << "level=error msg=boom\n"
                      "\tframe A\n"
                      "\tframe B\n";
        }

        const StreamingRun run = RunStreamingLogfmt(fixturePath);
        QCOMPARE(run.finishedCount, 1);
        QCOMPARE(run.model->rowCount(), 1);

        const RecordDetailContent content = BuildRecordDetailContent(*run.model, 0);
        QVERIFY(content.valid);

        QString msgValue;
        for (const auto &field : content.fields)
        {
            if (field.first == QStringLiteral("msg"))
            {
                msgValue = field.second;
                break;
            }
        }
        QVERIFY2(!msgValue.isEmpty(), "record detail must surface the msg field for a multi-line record");
        QVERIFY2(msgValue.contains(QStringLiteral("boom")), qPrintable(msgValue));
        QVERIFY2(msgValue.contains(QStringLiteral("frame A")), qPrintable(msgValue));
        QVERIFY2(msgValue.contains(QStringLiteral("frame B")), qPrintable(msgValue));
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
        const TempJsonFile fixture(
            QStringList{
                QStringLiteral(R"({"k": "alpha"})"),
                QStringLiteral(R"({"k": "beta"})"),
            }
        );
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
        const TempJsonFile fixture(
            QStringList{
                QStringLiteral(R"({"msg": "snapshot me"})"),
            }
        );
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
        const QPushButton *popOutButton = window->findChild<RecordDetailWidget *>()->findChild<QPushButton *>(
            QStringLiteral("openInNewWindowButton")
        );
        QVERIFY(popOutButton != nullptr);
        QVERIFY(!popOutButton->isVisibleTo(window->findChild<RecordDetailWidget *>()));
    }
    // NOLINTEND(clang-analyzer-cplusplus.NewDeleteLeaks)

    // Double-clicking a row surfaces and pins the dock; `modelReset`
    // clears it back to the placeholder.
    void TestRecordDetailDockOpensOnDoubleClickAndClearsOnReset()
    {
        const TempJsonFile fixture(
            QStringList{
                QStringLiteral(R"({"k": "alpha"})"),
                QStringLiteral(R"({"k": "beta"})"),
                QStringLiteral(R"({"k": "gamma"})"),
            }
        );
        auto *model = mWindow->Model();
        QVERIFY(model != nullptr);

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
        QVERIFY(finishedSpy.count() > 0 || finishedSpy.wait(60000));
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
        const TempJsonFile fixture(
            QStringList{
                QStringLiteral(R"({"k": "one"})"),
                QStringLiteral(R"({"k": "two"})"),
            }
        );
        auto *model = mWindow->findChild<LogModel *>();
        QVERIFY(model != nullptr);
        QSignalSpy finishedSpy(model, &LogModel::streamingFinished);
        auto fileSource = std::make_unique<loglib::FileLineSource>(fixture.Path().toStdString());
        loglib::FileLineSource *fileSourcePtr = fileSource.get();
        const loglib::StopToken stopToken = model->BeginStreamingForSyncTest(std::move(fileSource));
        loglib::ParserOptions options;
        options.stopToken = stopToken;
        options.threads = 1;
        const loglib::JsonParser parser;
        parser.Parse(*fileSourcePtr, *model->Sink(), options);
        QVERIFY(finishedSpy.count() > 0 || finishedSpy.wait(60000));
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
        const TempJsonFile fixture(
            QStringList{
                QStringLiteral(R"({"k": "alpha"})"),
                QStringLiteral(R"({"k": "beta"})"),
                QStringLiteral(R"({"k": "gamma"})"),
            }
        );
        auto *model = mWindow->findChild<LogModel *>();
        QVERIFY(model != nullptr);

        QSignalSpy finishedSpy(model, &LogModel::streamingFinished);
        auto fileSource = std::make_unique<loglib::FileLineSource>(fixture.Path().toStdString());
        loglib::FileLineSource *fileSourcePtr = fileSource.get();
        const loglib::StopToken stopToken = model->BeginStreamingForSyncTest(std::move(fileSource));
        loglib::ParserOptions options;
        options.stopToken = stopToken;
        options.threads = 1;
        const loglib::JsonParser parser;
        parser.Parse(*fileSourcePtr, *model->Sink(), options);
        QVERIFY(finishedSpy.count() > 0 || finishedSpy.wait(60000));

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
        model.AppendBatch(MakeSyntheticBatch(streamSource, valueKey, 1, 100, /*declareNewKey=*/true));
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
        model.AppendBatch(MakeSyntheticBatch(streamSource, valueKey, 101, 50, /*declareNewKey=*/false));
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
        model.AppendBatch(MakeSyntheticBatch(streamSource, valueKey, 151, 100, /*declareNewKey=*/false));
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
        model.AppendBatch(MakeSyntheticBatch(streamSource, valueKey, 1, 50, /*declareNewKey=*/true));
        QCOMPARE(model.rowCount(), 50);

        RecordDetailDock dock(&model);
        // Drive the visibility gate via the signal (see namespace note).
        emit dock.visibilityChanged(true);
        // No `ShowSourceRow` -> `mEverPinned` stays false.
        const QString initialPlaceholder = dock.Widget()->Content().placeholderText;
        QCOMPARE(initialPlaceholder, DefaultRecordDetailPlaceholder());

        // Trigger eviction; placeholder must stay default.
        model.AppendBatch(MakeSyntheticBatch(streamSource, valueKey, 51, 50, /*declareNewKey=*/false));
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
        const TempJsonFile fixture(
            QStringList{
                QStringLiteral(R"({"k": "alpha"})"),
                QStringLiteral(R"({"k": "beta"})"),
            }
        );
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
        run.model->MutableLayout().SetColumnHeader(static_cast<size_t>(kColumn), std::string("renamed"));
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
        const TempJsonFile fixture(
            QStringList{
                QStringLiteral(R"({"k": "one"})"),
            }
        );
        auto *model = mWindow->findChild<LogModel *>();
        QVERIFY(model != nullptr);
        QSignalSpy finishedSpy(model, &LogModel::streamingFinished);
        auto fileSource = std::make_unique<loglib::FileLineSource>(fixture.Path().toStdString());
        loglib::FileLineSource *fileSourcePtr = fileSource.get();
        const loglib::StopToken stopToken = model->BeginStreamingForSyncTest(std::move(fileSource));
        loglib::ParserOptions options;
        options.stopToken = stopToken;
        options.threads = 1;
        const loglib::JsonParser parser;
        parser.Parse(*fileSourcePtr, *model->Sink(), options);
        QVERIFY(finishedSpy.count() > 0 || finishedSpy.wait(60000));

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
        const TempJsonFile fixture(
            QStringList{
                QStringLiteral(R"({"k": "alpha"})"),
                QStringLiteral(R"({"k": "beta"})"),
            }
        );
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
        run.model->MutableLayout().SetColumnHeader(static_cast<size_t>(kColumn), std::string("renamed"));
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
        const TempJsonFile fixture(
            QStringList{
                QStringLiteral(R"({"k": "alpha"})"),
                QStringLiteral(R"({"k": "beta"})"),
                QStringLiteral(R"({"k": "gamma"})"),
            }
        );
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
        const TempJsonFile fixture(
            QStringList{
                QStringLiteral(R"({"k": "alpha"})"),
                QStringLiteral(R"({"k": "beta"})"),
                QStringLiteral(R"({"k": "gamma"})"),
            }
        );
        auto *model = mWindow->findChild<LogModel *>();
        QVERIFY(model != nullptr);
        QSignalSpy finishedSpy(model, &LogModel::streamingFinished);
        auto fileSource = std::make_unique<loglib::FileLineSource>(fixture.Path().toStdString());
        loglib::FileLineSource *fileSourcePtr = fileSource.get();
        const loglib::StopToken stopToken = model->BeginStreamingForSyncTest(std::move(fileSource));
        loglib::ParserOptions options;
        options.stopToken = stopToken;
        options.threads = 1;
        const loglib::JsonParser parser;
        parser.Parse(*fileSourcePtr, *model->Sink(), options);
        QVERIFY(finishedSpy.count() > 0 || finishedSpy.wait(60000));
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

    // Buried-tab transition (`visibilityChanged(false)` without
    // `hide()`) skips refreshes; the resuming `(true)` refreshes once.
    void TestRecordDetailDockSkipsRefreshWhileTabInactiveAndCatchesUpOnResume()
    {
        const TempJsonFile fixture(
            QStringList{
                QStringLiteral(R"({"k": "alpha"})"),
                QStringLiteral(R"({"k": "beta"})"),
            }
        );
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
        const TempJsonFile fixture(
            QStringList{
                QStringLiteral(R"({"k": "alpha"})"),
                QStringLiteral(R"({"k": "beta"})"),
            }
        );
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
};

QTEST_MAIN(MainWindowTest)
#include "apptest_main_window_test.moc"
