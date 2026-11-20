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
class ColumnsTest : public apptest_support::AppTestBase
{
    Q_OBJECT

private slots:

    static void TestFixtureMixedColumns()
    {
        const FixtureFile fixture(":/fixtures/mixed_columns.jsonl");
        StreamingRun run = RunStreaming(fixture.Path());
        QCOMPARE(run.finishedCount, 1);
        QCOMPARE(run.cancelled, false);
        QCOMPARE(run.model->rowCount(), 3);
        QVERIFY(run.model->StreamingErrors().empty());

        const int colA = ColumnByHeader(*run.model, QStringLiteral("a"));
        const int colB = ColumnByHeader(*run.model, QStringLiteral("b"));
        const int colC = ColumnByHeader(*run.model, QStringLiteral("c"));
        QVERIFY(colA >= 0 && colB >= 0 && colC >= 0);
        QCOMPARE(run.model->columnCount(), 3);

        const auto displayVal = [&](int row, int col) {
            return run.model->data(run.model->index(row, col), Qt::DisplayRole).toString();
        };

        // Row 0: {a, b}; c is missing → empty.
        QCOMPARE(displayVal(0, colA), QStringLiteral("a1"));
        QCOMPARE(displayVal(0, colB), QStringLiteral("b1"));
        QVERIFY(displayVal(0, colC).isEmpty());

        // Row 1: {a, c}; b is missing.
        QCOMPARE(displayVal(1, colA), QStringLiteral("a2"));
        QVERIFY(displayVal(1, colB).isEmpty());
        QCOMPARE(displayVal(1, colC), QStringLiteral("c2"));

        // Row 2: {b, c}; a is missing.
        QVERIFY(displayVal(2, colA).isEmpty());
        QCOMPARE(displayVal(2, colB), QStringLiteral("b3"));
        QCOMPARE(displayVal(2, colC), QStringLiteral("c3"));
    }

    // Same contract with a user column sort instead of
    // newest-first: descending on `value` puts the highest
    // lineId at proxy row 0 and the export must reflect that.
    void TestCollectExportSourceRowsRespectsColumnSort()
    {
        auto *model = mWindow->findChild<LogModel *>();
        auto *filterProxy = mWindow->FilterModel();
        QVERIFY(model != nullptr);
        QVERIFY(filterProxy != nullptr);

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

        // Descending on `value` (== lineId), so proxy row 0 is
        // lineId 5 (source row 4).
        filterProxy->sort(valueColumn, Qt::DescendingOrder);
        QCoreApplication::processEvents();

        const std::vector<int> collected = mWindow->CollectExportSourceRows(/*selectionOnly=*/false);
        QCOMPARE(collected.size(), std::size_t(5));
        const std::vector<int> expected = {4, 3, 2, 1, 0};
        QCOMPARE(collected, expected);

        // Ascending: reverts to source order.
        filterProxy->sort(valueColumn, Qt::AscendingOrder);
        QCoreApplication::processEvents();
        const std::vector<int> ascending = mWindow->CollectExportSourceRows(/*selectionOnly=*/false);
        const std::vector<int> ascendingExpected = {0, 1, 2, 3, 4};
        QCOMPARE(ascending, ascendingExpected);

        // Clear the sort so the fixture teardown starts clean.
        filterProxy->sort(-1);

        model->EndStreaming(false);
    }

    // ThemeControl::AnchorBrushFor: paint-ready brush for every
    // valid slot, invalid brush for out-of-range / unknown roles.
    void TestThemeControlAnchorBrushResolver()
    {
        QVERIFY(mTheme != nullptr);

        // Every slot yields paint-ready brushes for BG and FG.
        for (uint8_t i = 0; i < loglib::ANCHOR_PALETTE_SIZE; ++i)
        {
            const QBrush bg = mTheme->AnchorBrushFor(i, Qt::BackgroundRole);
            const QBrush fg = mTheme->AnchorBrushFor(i, Qt::ForegroundRole);
            QVERIFY2(bg.style() != Qt::NoBrush, qPrintable(QStringLiteral("anchor slot %1 BG was invalid").arg(i)));
            QVERIFY2(fg.style() != Qt::NoBrush, qPrintable(QStringLiteral("anchor slot %1 FG was invalid").arg(i)));
            QVERIFY(bg.color().isValid());
            QVERIFY(fg.color().isValid());
        }

        // Distinct slots must paint distinct backgrounds.
        QVERIFY(
            mTheme->AnchorBrushFor(0, Qt::BackgroundRole).color() !=
            mTheme->AnchorBrushFor(4, Qt::BackgroundRole).color()
        );

        // Out-of-range -> invalid brush (model falls through to
        // the level branch).
        const QBrush oob = mTheme->AnchorBrushFor(loglib::ANCHOR_PALETTE_SIZE, Qt::BackgroundRole);
        QVERIFY(oob.style() == Qt::NoBrush);

        // Unhandled role -> invalid brush.
        QVERIFY(mTheme->AnchorBrushFor(0, Qt::DisplayRole).style() == Qt::NoBrush);
    }

    // Double-clicking the anchor column jumps to the anchored row.
    // Regression for the new gesture: `itemActivated` alone isn't
    // reliable across every Qt style (some route double-click via
    // `SH_ItemView_ActivateItemOnSingleClick`), so the dock wires
    // `itemDoubleClicked` explicitly. Double-clicking the note
    // column must NOT jump -- that gesture is reserved for opening
    // the inline editor.
    void TestAnchorsDockDoubleClickAnchorColumnJumps()
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
        const auto key1 = model->AnchorKeyForRow(1);
        QVERIFY(key0.has_value());
        QVERIFY(key1.has_value());
        anchors->SetAnchor(*key0, 0);
        anchors->SetAnchor(*key1, 3);
        QCoreApplication::processEvents();
        dock->RefreshForTest();
        QCOMPARE(tree->topLevelItemCount(), 2);

        QSignalSpy jumpSpy(dock, &AnchorsDock::jumpToAnchorRequested);
        QVERIFY(jumpSpy.isValid());

        // Double-click on the anchor column of the first row.
        QTreeWidgetItem *firstItem = tree->topLevelItem(0);
        QVERIFY(firstItem != nullptr);
        emit tree->itemDoubleClicked(firstItem, /*column=*/0);
        QCoreApplication::processEvents();
        QCOMPARE(jumpSpy.count(), 1);
        QVERIFY(jumpSpy.last().at(0).toInt() >= 0);

        // Double-click on the note column must NOT jump: that
        // gesture opens the inline editor.
        const auto jumpsBeforeNote = jumpSpy.count();
        emit tree->itemDoubleClicked(firstItem, /*column=*/1);
        QCoreApplication::processEvents();
        QCOMPARE(jumpSpy.count(), jumpsBeforeNote);

        // Double-clicking a second anchor row jumps to its source
        // row -- distinct from the first row's source row so we're
        // sure the item -> row resolution isn't stuck.
        QTreeWidgetItem *secondItem = tree->topLevelItem(1);
        QVERIFY(secondItem != nullptr);
        emit tree->itemDoubleClicked(secondItem, /*column=*/0);
        QCoreApplication::processEvents();
        QCOMPARE(jumpSpy.count(), jumpsBeforeNote + 1);
        QVERIFY(jumpSpy.last().at(0).toInt() >= 0);
        QVERIFY(jumpSpy.at(0).at(0).toInt() != jumpSpy.last().at(0).toInt());

        model->EndStreaming(false);
    }

    // Parser: the column's own `parseFormats` are tried first
    // (ROADMAP acceptance bar). Naive formats (no `%z` / `%Ez` /
    // `%Z`) yield `isNaive == true` so the caller knows to
    // TZ-shift the value.
    void TestParseGotoTimestampInputHonoursColumnParseFormats()
    {
        const std::chrono::system_clock::time_point kNow =
            std::chrono::system_clock::time_point{std::chrono::microseconds{1'700'000'000'000'000LL}};

        // Slash-separated custom format that would fail the ISO
        // fallbacks. `%FT%T` = "%Y-%m-%dT%H:%M:%S" and `%F %T` =
        // "%Y-%m-%d %H:%M:%S".
        const std::vector<std::string> columnFormats{"%Y/%m/%d %H:%M:%S"};
        const auto parsed =
            MainWindow::ParseGotoTimestampInput(QStringLiteral("2024/04/28 12:34:56"), columnFormats, kNow);
        QVERIFY(parsed.has_value());
        QVERIFY(parsed->micros > 0);
        QVERIFY2(parsed->isNaive, "column format has no %z, result must be flagged naive");

        // ISO fallback works even with no column formats.
        const auto isoT = MainWindow::ParseGotoTimestampInput(QStringLiteral("2024-04-28T12:34:56"), {}, kNow);
        QVERIFY(isoT.has_value());
        QVERIFY(isoT->micros > 0);
        QVERIFY2(isoT->isNaive, "ISO fallback lacks %z, result must be flagged naive");

        const auto isoSpace = MainWindow::ParseGotoTimestampInput(QStringLiteral("2024-04-28 12:34:56"), {}, kNow);
        QVERIFY(isoSpace.has_value());
        QCOMPARE(isoT->micros, isoSpace->micros);

        // A format WITH `%Ez` marks the result as TZ-aware -- the
        // slot must not double-shift.
        const std::vector<std::string> zonedFormats{"%FT%T%Ez"};
        const auto zoned =
            MainWindow::ParseGotoTimestampInput(QStringLiteral("2024-04-28T12:34:56+02:00"), zonedFormats, kNow);
        QVERIFY(zoned.has_value());
        QVERIFY2(!zoned->isNaive, "format contains %Ez, result must NOT be flagged naive");
    }

    // Seam: bail-out branch when the log has no time column.
    void TestExecuteGotoTimestampSeamHintsWhenNoTimeColumn()
    {
        auto *model = mWindow->Model();
        QVERIFY(model != nullptr);

        loglib::StreamLineSource &streamSource = BeginSyntheticStreamSession(*model);
        QtStreamingLogSink *sink = model->Sink();
        QVERIFY(sink != nullptr);
        loglib::KeyIndex &keys = sink->Keys();
        const loglib::KeyId valueKey = keys.GetOrInsert(std::string("value"));
        // Synthetic rows carry only a `value` key -- no time col.
        sink->OnBatch(MakeSyntheticBatch(streamSource, valueKey, 1, 3, true));
        QCoreApplication::processEvents();
        QCOMPARE(model->rowCount(), 3);
        QVERIFY(!loglib::FirstTimeColumnIndex(model->Layout()).has_value());

        mWindow->statusBar()->clearMessage();
        mWindow->ExecuteGotoTimestampForTest(
            QStringLiteral("2024-04-28T10:00:02+00:00"), std::chrono::system_clock::now()
        );
        QCoreApplication::processEvents();

        const QString hint = mWindow->statusBar()->currentMessage();
        QVERIFY2(
            hint.contains(QStringLiteral("no timestamp column"), Qt::CaseInsensitive),
            qUtf8Printable(QStringLiteral("Expected 'no timestamp column' hint, got: '%1'").arg(hint))
        );

        model->EndStreaming(false);
    }

    // The AnchorsDock installs a per-column edit-blocking delegate
    // on the anchor column so `F2` while focused there can't open
    // an editor for the "line N - file.json" label. Test both the
    // delegate contract (createEditor returns nullptr) and the
    // observable outcome (the tree refuses to enter edit mode via
    // `editItem` on the anchor column).
    void TestAnchorsDockAnchorColumnRefusesEditor()
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
        QCOMPARE(model->rowCount(), 1);

        const auto key0 = model->AnchorKeyForRow(0);
        QVERIFY(key0.has_value());
        anchors->SetAnchor(*key0, 3);
        dock->RefreshForTest();
        QCOMPARE(tree->topLevelItemCount(), 1);

        // Delegate on column 0 refuses to build an editor: even a
        // direct `createEditor` call returns nullptr.
        const QAbstractItemDelegate *anchorDelegate = tree->itemDelegateForColumn(0);
        QVERIFY2(anchorDelegate != nullptr, "AnchorsDock must install a delegate on the anchor column");
        const QStyleOptionViewItem opt;
        const QModelIndex anchorIdx = tree->model()->index(0, 0);
        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): prior QVERIFY2 aborts on null.
        const QWidget *editor = anchorDelegate->createEditor(tree->viewport(), opt, anchorIdx);
        QVERIFY2(editor == nullptr, "Anchor column delegate must refuse editor construction");

        // Sanity check: the note column still has an editor. `nullptr`
        // means Qt falls back to the default `QStyledItemDelegate`,
        // which builds an editor from the item flags.
        const QAbstractItemDelegate *noteDelegate = tree->itemDelegateForColumn(1);
        // Whether Qt returns the tree-wide default or a per-column
        // delegate, `createEditor` on the editable note column must
        // succeed (it produces a `QLineEdit`).
        if (noteDelegate == nullptr)
        {
            noteDelegate = tree->itemDelegate();
        }
        QVERIFY(noteDelegate != nullptr);
        const QModelIndex noteIdx = tree->model()->index(0, 1);
        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): prior QVERIFY aborts on null.
        const QWidget *noteEditor = noteDelegate->createEditor(tree->viewport(), opt, noteIdx);
        QVERIFY2(noteEditor != nullptr, "Note column must accept an editor");
        delete noteEditor;

        // Observable path: even after calling `editItem` on column 0,
        // no persistent editor is opened (no `QLineEdit` child of the
        // viewport). Qt short-circuits internally when the delegate
        // returns nullptr.
        tree->setCurrentItem(tree->topLevelItem(0), /*column=*/0);
        tree->editItem(tree->topLevelItem(0), /*column=*/0);
        QCoreApplication::processEvents();
        QVERIFY2(
            tree->viewport()->findChild<QLineEdit *>() == nullptr,
            "editItem on the anchor column must not open a QLineEdit"
        );

        // For contrast, `editItem` on the note column *does* open
        // an editor. Close it immediately so the widget doesn't leak
        // event-loop state across tests.
        tree->editItem(tree->topLevelItem(0), /*column=*/1);
        QCoreApplication::processEvents();
        auto *noteInlineEditor = tree->viewport()->findChild<QLineEdit *>();
        QVERIFY2(noteInlineEditor != nullptr, "editItem on the note column must open a QLineEdit");
        // Dismiss the editor via Escape so no stale note is committed.
        QKeyEvent escKey(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
        QApplication::sendEvent(noteInlineEditor, &escKey);
        QCoreApplication::processEvents();

        anchors->ClearAll();
        QCoreApplication::processEvents();
        model->EndStreaming(false);
    }

    // Regression for the "F2 swallowed on anchor column" issue:
    // the event filter only intercepts plain F2 when the current
    // column is the note column. On the anchor column there's no
    // editor to open (the `NoEditDelegate` refuses), so the window-
    // scope "Jump to next anchor" shortcut must be free to fire.
    void TestAnchorsDockF2ShortcutOverridePassesThroughOnAnchorColumn()
    {
        auto *anchors = mWindow->Anchors();
        QVERIFY(anchors != nullptr);
        auto *dock = mWindow->findChild<AnchorsDock *>();
        QVERIFY(dock != nullptr);
        auto *tree = dock->TreeForTest();
        QVERIFY(tree != nullptr);
        dock->show();
        emit dock->visibilityChanged(true);

        const AnchorManager::Key k{.locator = "c:/logs/a.json", .lineId = 1};
        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): prior QVERIFY aborts on null.
        QVERIFY(anchors->SetAnchor(k, 1));
        dock->RefreshForTest();
        QCOMPARE(tree->topLevelItemCount(), 1);

        // Park focus on the anchor column (0). The event filter
        // should let plain F2 propagate to the window-scope shortcut
        // rather than accepting it and shadowing the jump action.
        tree->setCurrentItem(tree->topLevelItem(0), /*column=*/0);
        QKeyEvent f2AnchorCol(QEvent::ShortcutOverride, Qt::Key_F2, Qt::NoModifier);
        QApplication::sendEvent(tree, &f2AnchorCol);
        QVERIFY2(
            !f2AnchorCol.isAccepted(),
            "F2 must NOT be intercepted on the anchor column -- the NoEditDelegate refuses to build "
            "an editor there, so shadowing the shortcut would silently swallow 'Jump to next anchor'"
        );

        // Flip to the note column and verify F2 IS intercepted so
        // the inline editor still opens. Together these two checks
        // prove the column-gated behaviour.
        tree->setCurrentItem(tree->topLevelItem(0), /*column=*/1);
        QKeyEvent f2NoteCol(QEvent::ShortcutOverride, Qt::Key_F2, Qt::NoModifier);
        QApplication::sendEvent(tree, &f2NoteCol);
        QVERIFY2(
            f2NoteCol.isAccepted(),
            "F2 must be intercepted on the note column so the tree's edit trigger runs instead of "
            "the window-scope jump shortcut"
        );

        anchors->ClearAll();
        QCoreApplication::processEvents();
        dock->hide();
    }

    /// Integration: a streamed `Type::Level` column makes
    /// `LogModel::data(Qt::BackgroundRole)` return the cached
    /// theme brush for styled levels and an empty QVariant for
    /// unstyled ones.
    void TestLogModelDataReturnsThemeBackground()
    {
        // The fixture owns a `ThemeControl` and threads it into
        // `MainWindow` + `LogModel`; force Light for determinism.
        QVERIFY(mTheme != nullptr);
        mTheme->SetActiveSelection(QStringLiteral("Light"));
        const QBrush expectedErrorBg = mTheme->BackgroundFor(loglib::LogLevel::Error);
        QVERIFY2(expectedErrorBg.style() != Qt::NoBrush, "the Light theme must define an Error background brush");
        const QBrush expectedInfoBg = mTheme->BackgroundFor(loglib::LogLevel::Info);
        QCOMPARE(expectedInfoBg.style(), Qt::NoBrush);

        // Stream a tiny fixture so `level` promotes to `Type::Level`.
        const QStringList levels{
            QStringLiteral("info"),
            QStringLiteral("warn"),
            QStringLiteral("error"),
            QStringLiteral("debug"),
            QStringLiteral("trace"),
            QStringLiteral("fatal"),
        };
        QStringList lines;
        lines.reserve(300);
        for (int i = 0; i < 300; ++i)
        {
            lines.append(QStringLiteral(R"({"level": "%1"})").arg(levels[i % levels.size()]));
        }
        const TempJsonFile fixture(lines);

        const StreamingRun run = RunStreaming(fixture.Path(), mTheme.data());
        QCOMPARE(run.finishedCount, 1);
        QCOMPARE(run.cancelled, false);

        const int levelCol = ColumnByHeader(*run.model, QStringLiteral("level"));
        QVERIFY2(levelCol >= 0, "level column must exist");

        // Row 0 = Info (unstyled) -> empty QVariant.
        const QModelIndex infoIndex = run.model->index(0, levelCol);
        const QVariant infoBg = run.model->data(infoIndex, Qt::BackgroundRole);
        QVERIFY2(!infoBg.isValid(), "Info row must defer to the palette (empty QVariant)");

        // Row 2 = Error (styled) -> theme brush.
        const QModelIndex errorIndex = run.model->index(2, levelCol);
        const QVariant errorBg = run.model->data(errorIndex, Qt::BackgroundRole);
        QVERIFY2(errorBg.isValid(), "Error row must carry the theme background brush");
        const auto actualErrorBg = qvariant_cast<QBrush>(errorBg);
        QCOMPARE(actualErrorBg.color(), expectedErrorBg.color());

        // Row 5 = Fatal -> bold font via FontRole.
        const QModelIndex fatalIndex = run.model->index(5, levelCol);
        const QVariant fatalFont = run.model->data(fatalIndex, Qt::FontRole);
        QVERIFY2(fatalFont.isValid(), "Fatal row must carry a bold font (theme sets bold=true)");
        QVERIFY(qvariant_cast<QFont>(fatalFont).bold());
    }

    // Regression: a theme that ships only a `headerIcon` (no
    // explicit `header` override) must paint the icon alone so
    // the header doesn't read "<gauge> level". Funnel / warning
    // priority and the tooltip are unchanged.
    void TestLevelHeaderIconSuppressesDisplayText()
    {
        QVERIFY(mTheme != nullptr);
        mTheme->SetActiveSelection(QStringLiteral("Light"));
        QVERIFY2(mTheme->HasLevelColumnOverride(), "shipped Light theme must opt into icon mode");
        QVERIFY2(!mTheme->LevelColumnHeaderIcon().isNull(), "shipped Light theme must ship a `headerIcon`");
        QVERIFY2(
            !mTheme->LevelColumnHeaderTextOverride().has_value(),
            "shipped Light theme must not set an explicit `header` -- this test pins the implicit-blank rule"
        );

        const QStringList lines{
            QStringLiteral(R"({"level": "info"})"),
            QStringLiteral(R"({"level": "warn"})"),
        };
        const TempJsonFile fixture(lines);
        const StreamingRun run = RunStreaming(fixture.Path(), mTheme.data());
        QCOMPARE(run.cancelled, false);
        QVERIFY(run.model != nullptr);

        const int levelCol = ColumnByHeader(*run.model, QStringLiteral("level"));
        QVERIFY2(levelCol >= 0, "level column must resolve via configured Column::header");

        const QString displayed = run.model->headerData(levelCol, Qt::Horizontal, Qt::DisplayRole).toString();
        QVERIFY2(
            displayed.isEmpty(),
            qPrintable(
                QStringLiteral("level header must render blank text when an icon is present, got '%1'").arg(displayed)
            )
        );

        // Header icon centres so it lines up with the centred
        // pills below.
        const QVariant alignment = run.model->headerData(levelCol, Qt::Horizontal, Qt::TextAlignmentRole);
        QVERIFY2(alignment.isValid(), "icon-only level header must report an explicit Qt::TextAlignmentRole");
        QCOMPARE(alignment.toInt(), static_cast<int>(Qt::AlignCenter));

        // Sibling text columns keep the default alignment.
        for (int col = 0; col < run.model->columnCount(); ++col)
        {
            if (col == levelCol)
            {
                continue;
            }
            const QVariant siblingAlignment = run.model->headerData(col, Qt::Horizontal, Qt::TextAlignmentRole);
            QVERIFY2(
                !siblingAlignment.isValid(),
                qPrintable(QStringLiteral("text column %1 must inherit the default header alignment").arg(col))
            );
        }

        // Tooltip still names the column.
        const QString tooltip = run.model->headerData(levelCol, Qt::Horizontal, Qt::ToolTipRole).toString();
        QVERIFY2(
            tooltip.contains(QStringLiteral("level")),
            qPrintable(QStringLiteral("tooltip must keep naming the column, got '%1'").arg(tooltip))
        );

        // Toggling icon mode off restores the configured header
        // text and the default alignment.
        run.model->SetShowLevelIcons(false);
        const QString restored = run.model->headerData(levelCol, Qt::Horizontal, Qt::DisplayRole).toString();
        QCOMPARE(restored, QStringLiteral("level"));
        const QVariant restoredAlignment = run.model->headerData(levelCol, Qt::Horizontal, Qt::TextAlignmentRole);
        QVERIFY2(!restoredAlignment.isValid(), "level header must drop the centre alignment when icon mode is off");
    }

    // Regression: `LevelCellDelegate::paint` must (1) actually
    // render the pill, (2) hard-clip to `option.rect` so a narrow
    // column can't bleed into the neighbour, and (3) fall back to
    // the base delegate when icon mode is off. The pixmap probe
    // pins the visual contract -- a change to the insets, clip,
    // gate, or pill-brush resolution surfaces as a pixel diff.
    void TestLevelCellDelegatePaintsPillAndClipsToRect()
    {
        QVERIFY(mTheme != nullptr);
        // Dark ships a concrete Info pill (`#1E3A5F`) to assert
        // against; pinning to a fixed theme keeps the test stable.
        mTheme->SetActiveSelection(QStringLiteral("Dark"));
        QVERIFY(mTheme->HasLevelColumnOverride());
        const QBrush expectedPill = mTheme->PillBackgroundFor(loglib::LogLevel::Info);
        QVERIFY2(expectedPill.style() != Qt::NoBrush, "Dark theme must define an Info pill background");
        const QColor expectedPillColor = expectedPill.color();
        QVERIFY(expectedPillColor.isValid());

        // Minimal fixture so the rank cache resolves `info`.
        const QStringList lines{
            QStringLiteral(R"({"level": "info"})"),
            QStringLiteral(R"({"level": "info"})"),
        };
        const TempJsonFile fixture(lines);
        const StreamingRun run = RunStreaming(fixture.Path(), mTheme.data());
        QCOMPARE(run.cancelled, false);
        QVERIFY(run.model != nullptr);
        run.model->SetShowLevelIcons(true);
        QVERIFY(run.model->IsLevelIconModeActive());

        const int levelCol = ColumnByHeader(*run.model, QStringLiteral("level"));
        QVERIFY2(levelCol >= 0, "fixture must resolve a level column");
        const QModelIndex cell = run.model->index(0, levelCol);
        QVERIFY(cell.isValid());

        const LevelCellDelegate delegate(mTheme.data());

        // Pixmap > cell rect, filled with a sentinel colour. Any
        // sentinel pixel changing after `paint` means the clip
        // leaked.
        constexpr int CELL_WIDTH = 64;
        constexpr int CELL_HEIGHT = 24;
        constexpr int OUTSIDE_MARGIN = 8;
        constexpr int PIXMAP_WIDTH = CELL_WIDTH + (2 * OUTSIDE_MARGIN);
        constexpr int PIXMAP_HEIGHT = CELL_HEIGHT + (2 * OUTSIDE_MARGIN);
        const QColor sentinel(0xFF, 0x00, 0xFF); // magenta -- not used by Dark theme.

        QPixmap pix(PIXMAP_WIDTH, PIXMAP_HEIGHT);
        pix.fill(sentinel);

        QStyleOptionViewItem option;
        option.rect = QRect(OUTSIDE_MARGIN, OUTSIDE_MARGIN, CELL_WIDTH, CELL_HEIGHT);
        option.state = QStyle::State_Enabled;
        option.widget = nullptr;

        {
            QPainter painter(&pix);
            delegate.paint(&painter, option, cell);
        }
        const QImage image = pix.toImage();

        // (1) Pill paint ran: at least one sampled pixel inside
        // the inset matches the theme's pill colour. Sampling a
        // ring avoids false negatives from AA edges and DPR
        // rounding.
        constexpr int PILL_PROBE_INSET = 6;
        const QPoint sampleCentre = option.rect.center();
        const std::array<QPoint, 5> samples = {
            sampleCentre,
            QPoint(option.rect.left() + PILL_PROBE_INSET, sampleCentre.y()),
            QPoint(option.rect.right() - PILL_PROBE_INSET, sampleCentre.y()),
            QPoint(sampleCentre.x(), option.rect.top() + PILL_PROBE_INSET),
            QPoint(sampleCentre.x(), option.rect.bottom() - PILL_PROBE_INSET)
        };
        const auto colorsMatch = [](QRgb actual, const QColor &expected) {
            // Per-channel tolerance covers AA edges + a swapped
            // compositing mode in any installed stylesheet.
            constexpr int CHANNEL_TOLERANCE = 3;
            return std::abs(qRed(actual) - expected.red()) <= CHANNEL_TOLERANCE &&
                   std::abs(qGreen(actual) - expected.green()) <= CHANNEL_TOLERANCE &&
                   std::abs(qBlue(actual) - expected.blue()) <= CHANNEL_TOLERANCE;
        };
        const bool pillVisible = std::ranges::any_of(samples, [&](const QPoint &pt) {
            return colorsMatch(image.pixel(pt), expectedPillColor);
        });
        QVERIFY2(pillVisible, "expected at least one pixel inside the cell to match the Dark Info pill brush");

        // (2) Clip held: every pixel outside the cell rect is
        // still the sentinel.
        for (int y = 0; y < PIXMAP_HEIGHT; ++y)
        {
            for (int x = 0; x < PIXMAP_WIDTH; ++x)
            {
                if (option.rect.contains(x, y))
                {
                    continue;
                }
                const QRgb actual = image.pixel(x, y);
                QVERIFY2(
                    qRed(actual) == 0xFF && qGreen(actual) == 0x00 && qBlue(actual) == 0xFF,
                    qPrintable(QStringLiteral("delegate painted outside option.rect at (%1, %2); RGBA=#%3")
                                   .arg(x)
                                   .arg(y)
                                   .arg(QString::number(actual, 16)))
                );
            }
        }

        // (3) Icon mode off: the delegate forwards to the base
        // class. Observed as "pill colour no longer at the
        // centre sample"; the exact fallback colour is palette-
        // dependent so we don't pin it.
        run.model->SetShowLevelIcons(false);
        QVERIFY(!run.model->IsLevelIconModeActive());
        pix.fill(sentinel);
        {
            QPainter painter(&pix);
            delegate.paint(&painter, option, cell);
        }
        const QImage offModeImage = pix.toImage();
        QVERIFY2(
            !colorsMatch(offModeImage.pixel(sampleCentre), expectedPillColor),
            "with icon mode disabled the delegate must not paint the pill"
        );
    }

    // Regression: a theme switch must repaint the visible rows.
    // Symptom of failure: rows keep the old level colours until
    // the user scrolls or resizes the table.
    void TestThemeSwitchRefreshesLogTableRows()
    {
        QVERIFY(mTheme != nullptr);

        // Tiny multi-level fixture to get a themed Error row.
        const QStringList lines{
            QStringLiteral(R"({"level": "info"})"),
            QStringLiteral(R"({"level": "error"})"),
            QStringLiteral(R"({"level": "warn"})"),
        };
        const TempJsonFile fixture(lines);

        mTheme->SetActiveSelection(QStringLiteral("Light"));
        const StreamingRun run = RunStreaming(fixture.Path(), mTheme.data());
        QCOMPARE(run.cancelled, false);
        QVERIFY(run.model != nullptr);

        const int levelCol = ColumnByHeader(*run.model, QStringLiteral("level"));
        QVERIFY2(levelCol >= 0, "level column must exist after streaming");

        // Locate the Error row by display value (robust to reorders).
        int errorRow = -1;
        for (int r = 0; r < run.model->rowCount(); ++r)
        {
            if (run.model->data(run.model->index(r, levelCol), Qt::DisplayRole).toString() == QStringLiteral("error"))
            {
                errorRow = r;
                break;
            }
        }
        QVERIFY2(errorRow >= 0, "fixture must include an Error-level row");

        const QModelIndex errorIndex = run.model->index(errorRow, levelCol);
        const QVariant lightErrorBg = run.model->data(errorIndex, Qt::BackgroundRole);
        QVERIFY2(lightErrorBg.isValid(), "Light theme must style the Error row background");
        const auto lightBrush = qvariant_cast<QBrush>(lightErrorBg);

        // Flip to Dark; `ThemeControl` rebuilds its cache so the
        // next `data()` call sees the new brush.
        mTheme->SetActiveSelection(QStringLiteral("Dark"));
        QCoreApplication::processEvents();

        const QVariant darkErrorBg = run.model->data(errorIndex, Qt::BackgroundRole);
        QVERIFY2(darkErrorBg.isValid(), "Dark theme must style the Error row background");
        const auto darkBrush = qvariant_cast<QBrush>(darkErrorBg);
        QVERIFY2(
            lightBrush.color() != darkBrush.color(),
            qPrintable(QStringLiteral(
                           "Error row background must differ between Light and Dark themes; "
                           "got light=%1, dark=%2"
            )
                           .arg(lightBrush.color().name(), darkBrush.color().name()))
        );

        // Inverse: Light -> Dark -> Light returns the original brush.
        mTheme->SetActiveSelection(QStringLiteral("Light"));
        QCoreApplication::processEvents();
        const auto lightBrushAgain = qvariant_cast<QBrush>(run.model->data(errorIndex, Qt::BackgroundRole));
        QCOMPARE(lightBrushAgain.color(), lightBrush.color());
    }

    // Regression: a theme switch must emit `dataChanged` on the
    // styled roles so the view drops its per-item style cache.
    // `viewport()->update()` alone doesn't reliably invalidate it.
    void TestLocalTimestampsShowAsWrittenInTheDisplayZone()
    {
        // A file with timestamps that name no zone is read in the application's zone and shown in it, so the
        // cells show the text as written, and the stored instant is that wall clock converted to UTC.
        const QStringList lines{
            QStringLiteral(R"({"timestamp":"2024-04-28 10:00:01","msg":"first"})"),
            QStringLiteral(R"({"timestamp":"2024-04-28 10:00:02","msg":"second"})"),
            QStringLiteral(R"({"timestamp":"2024-07-01 23:30:00","msg":"summer"})"),
        };
        const TempJsonFile fixture(lines);
        mWindow->OpenFilesForTest({fixture.Path()}, MainWindow::OpenMode::Replace);
        auto *model = mWindow->findChild<LogModel *>();
        QVERIFY2(model != nullptr, "MainWindow must own a LogModel");
        QSignalSpy finishedSpy(model, &LogModel::streamingFinished);
        if (finishedSpy.count() == 0)
        {
            finishedSpy.wait(60000);
        }
        QCOMPARE(model->rowCount(), 3);
        const int timeCol = ColumnByHeader(*model, QStringLiteral("timestamp"));
        QVERIFY2(timeCol >= 0, "timestamp column must exist");

        QCOMPARE(
            model->data(model->index(0, timeCol), Qt::DisplayRole).toString(), QStringLiteral("2024-04-28 10:00:01.000")
        );
        QCOMPARE(
            model->data(model->index(2, timeCol), Qt::DisplayRole).toString(), QStringLiteral("2024-07-01 23:30:00.000")
        );

        constexpr std::int64_t MICROS_2024_04_28_10_00_02 = 1'714'298'402'000'000;
        const std::int64_t expectedUtc =
            loglib::ProcessDefaultTimeZone().LocalMicrosecondsToUtc(MICROS_2024_04_28_10_00_02);
        QCOMPARE(
            static_cast<std::int64_t>(
                model->data(model->index(1, timeCol), LogModelItemDataRole::SortRole).toLongLong()
            ),
            expectedUtc
        );
    }

    void TestThemeSwitchEmitsDataChangedOnLogTable()
    {
        QVERIFY(mTheme != nullptr);
        // Stream into the live `MainWindow` to observe the
        // production-wired `LogModel`.
        const QStringList lines{
            QStringLiteral(R"({"level": "info"})"),
            QStringLiteral(R"({"level": "error"})"),
            QStringLiteral(R"({"level": "warn"})"),
        };
        const TempJsonFile fixture(lines);
        mTheme->SetActiveSelection(QStringLiteral("Light"));
        mWindow->OpenFilesForTest({fixture.Path()}, MainWindow::OpenMode::Replace);
        // Pump until `streamingFinished` arrives (queued connection).
        auto *model = mWindow->findChild<LogModel *>();
        QVERIFY2(model != nullptr, "MainWindow must own a LogModel");
        QSignalSpy finishedSpy(model, &LogModel::streamingFinished);
        if (finishedSpy.count() == 0)
        {
            finishedSpy.wait(60000);
        }
        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): false positive; prior `QVERIFY2` aborts on null.
        QVERIFY2(model->rowCount() > 0, "fixture must produce at least one streamed row");

        const QSignalSpy spy(model, &QAbstractItemModel::dataChanged);
        mTheme->SetActiveSelection(QStringLiteral("Dark"));
        QCoreApplication::processEvents();

        // At least one `dataChanged` must cover a styled role.
        // Other (e.g. anchor-related) notifications can ride along.
        bool foundStyledNotification = false;
        for (int i = 0; i < spy.count(); ++i)
        {
            const QList<QVariant> &args = spy.at(i);
            QVERIFY2(args.size() >= 3, "dataChanged must carry topLeft, bottomRight, roles");
            const auto roles = args.at(2).value<QList<int>>();
            // Empty roles = "all roles changed" in Qt; counts as styled.
            if (roles.isEmpty() || roles.contains(Qt::BackgroundRole) || roles.contains(Qt::ForegroundRole) ||
                roles.contains(Qt::FontRole))
            {
                foundStyledNotification = true;
                break;
            }
        }
        QVERIFY2(
            foundStyledNotification,
            "Theme switch must emit a dataChanged covering BackgroundRole / ForegroundRole / FontRole "
            "(or all roles) so the table view refreshes the row tints."
        );
    }

    // Regression: a theme switch must re-apply the table view's
    // stylesheet so `QStyleSheetStyle` drops its palette cache.
    // Otherwise rows with no explicit Background brush (Info /
    // Trace) keep the previous theme's default, even when the
    // rule text is unchanged.
    void TestThemeSwitchRepolishesTableStylesheet()
    {
        QVERIFY(mTheme != nullptr);

        // Prime the stylesheet via the normal app path.
        const QStringList lines{
            QStringLiteral(R"({"level": "info"})"),
            QStringLiteral(R"({"level": "error"})"),
        };
        const TempJsonFile fixture(lines);
        mTheme->SetActiveSelection(QStringLiteral("Light"));
        mWindow->OpenFilesForTest({fixture.Path()}, MainWindow::OpenMode::Replace);
        auto *model = mWindow->findChild<LogModel *>();
        QVERIFY2(model != nullptr, "MainWindow must own a LogModel");
        QSignalSpy finishedSpy(model, &LogModel::streamingFinished);
        if (finishedSpy.count() == 0)
        {
            finishedSpy.wait(60000);
        }

        auto *tableView = mWindow->findChild<QTableView *>();
        QVERIFY2(tableView != nullptr, "MainWindow must own a table view");
        const QString initialStyleSheet = tableView->styleSheet();
        QVERIFY2(
            !initialStyleSheet.isEmpty(), "After a normal load the body stylesheet must include the monospace cell rule"
        );

        // Mirror the post-optimisation state where
        // `mLastBodyStyleSheet` claims the rule is applied but the
        // widget's stylesheet is stale. Fix must re-apply anyway.
        tableView->setStyleSheet(QString{});
        QCOMPARE(tableView->styleSheet(), QString{});

        // `OnThemeChanged` resets the tracker and forces a re-write.
        mTheme->SetActiveSelection(QStringLiteral("Dark"));
        QCoreApplication::processEvents();

        QVERIFY2(
            !tableView->styleSheet().isEmpty(),
            qPrintable(QStringLiteral(
                           "Theme switch must re-apply the body stylesheet so "
                           "`QStyleSheetStyle` re-resolves the new palette; got: '%1'"
            )
                           .arg(tableView->styleSheet()))
        );
        // Re-applied stylesheet must still carry the monospace rule.
        QVERIFY2(
            tableView->styleSheet().contains(QStringLiteral("QTableView::item")),
            qPrintable(QStringLiteral("Re-applied stylesheet must carry the monospace cell rule; got: '%1'")
                           .arg(tableView->styleSheet()))
        );
    }

    // End-to-end round-trip for `Column::levelMapping`. Saved config
    // pins `Type::Level` with `NOTICE -> Info`, `PANIC -> Fatal`. A
    // filter selecting `Info` expands via the rank cache to match
    // both `NOTICE` (override) and `info` (built-in) while rejecting `PANIC`.
    void TestLevelMappingRoundTripThroughFilterEditor()
    {
        // Step 1: build the saved configuration on disk.
        loglib::LogConfiguration cfg;
        loglib::Column column;
        column.header = "lvl";
        column.keys = {"lvl"};
        column.printFormat = "{}";
        column.type = loglib::ColumnType::Level;
        column.autoDetect = false;
        column.parseFormats = {};
        column.levelMapping = {
            {"NOTICE", "Info"},
            {"PANIC", "Fatal"},
        };
        cfg.columns.push_back(column);

        const QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        const QString cfgPath = tempDir.filePath(QStringLiteral("level-mapping.json"));
        loglib::SaveConfiguration(cfg, cfgPath.toStdString(), loglib::SaveScope::Full);

        // Step 2: load through MainWindow so the filter UI rebuilds.
        QVERIFY2(mWindow->TryLoadAsConfigurationForTest(cfgPath), "TryLoadAsConfiguration must succeed");
        QCoreApplication::processEvents();

        auto *model = mWindow->findChild<LogModel *>();
        QVERIFY2(model != nullptr, "MainWindow must own a LogModel");
        const int levelCol = ColumnByHeader(*model, QStringLiteral("lvl"));
        QVERIFY2(levelCol >= 0, "lvl column must exist after configuration load");
        QCOMPARE(model->Layout().Columns()[static_cast<size_t>(levelCol)].type, loglib::ColumnType::Level);

        // Step 3: drive a stream with mixed canonical / aliased values.
        const TempJsonFile emptyFixture(QStringList{});
        auto fileSource = std::make_unique<loglib::FileLineSource>(emptyFixture.Path().toStdString());
        loglib::FileLineSource *sourcePtr = fileSource.get();
        (void)model->BeginStreamingForSyncTest(std::move(fileSource));

        loglib::KeyIndex &keys = model->Table().Keys();
        const auto makeLine = [&](const std::string &value) {
            std::vector<std::pair<loglib::KeyId, loglib::LogValue>> values;
            values.emplace_back(keys.GetOrInsert("lvl"), loglib::LogValue(value));
            return loglib::LogLine{std::move(values), *sourcePtr, 0};
        };
        {
            loglib::RowBatch batch;
            batch.firstLineNumber = 1;
            batch.lines.push_back(makeLine("NOTICE")); // override -> Info
            batch.lines.push_back(makeLine("PANIC"));  // override -> Fatal
            batch.lines.push_back(makeLine("info"));   // built-in -> Info
            batch.lines.push_back(makeLine("WARN"));   // built-in -> Warn
            batch.lines.push_back(makeLine("qux"));    // unmapped (no level)
            model->AppendBatch(std::move(batch));
        }
        QCoreApplication::processEvents();

        // Step 4: submit `Info` through the FilterEditor's slot.
        // `MainWindow::UpdateFilters` expands `"Info"` to every raw
        // dict entry resolving to `LogLevel::Info` via `LevelRankCache`.
        const QString filterId = QStringLiteral("level-mapping-info-only");
        QVERIFY2(
            QMetaObject::invokeMethod(
                mWindow,
                "FilterEnumSubmitted",
                Qt::DirectConnection,
                Q_ARG(QString, filterId),
                Q_ARG(int, levelCol),
                Q_ARG(QStringList, QStringList{QStringLiteral("Info")})
            ),
            "FilterEnumSubmitted slot must be invocable via meta-object"
        );
        QCoreApplication::processEvents();

        // Step 5: only `NOTICE` (override) and `info` (built-in)
        // resolve to `Info` and survive. `PANIC`/`WARN`/`qux` drop out.
        const LogFilterModel *filterModel = mWindow->FilterModel();
        QVERIFY2(filterModel != nullptr, "MainWindow must own a LogFilterModel proxy");
        QCOMPARE(filterModel->rowCount(), 2);

        std::vector<QString> visible;
        visible.reserve(static_cast<size_t>(filterModel->rowCount()));
        for (int i = 0; i < filterModel->rowCount(); ++i)
        {
            visible.push_back(filterModel->index(i, levelCol).data(Qt::DisplayRole).toString());
        }
        std::ranges::sort(visible);
        QCOMPARE(visible.size(), static_cast<size_t>(2));
        QCOMPARE(visible[0], QStringLiteral("NOTICE"));
        QCOMPARE(visible[1], QStringLiteral("info"));

        model->EndStreaming(false);
    }

    // A saved string filter against a now-enum column must be dropped at
    // `AddFilter` time, not silently retained as a type-mismatched rule.
    void TestSavedStringFilterDroppedOnNowEnumColumn()
    {
        // 300 rows is well past the stream-mode promotion threshold.
        const QStringList levels{QStringLiteral("info"), QStringLiteral("warn"), QStringLiteral("error")};
        QStringList lines;
        lines.reserve(300);
        for (int i = 0; i < 300; ++i)
        {
            lines.append(QStringLiteral(R"({"category": "%1"})").arg(levels[i % levels.size()]));
        }
        const TempJsonFile fixture(lines);

        // Drive streaming on the live MainWindow so wiring matches production.
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

        const bool finished = finishedSpy.count() > 0 || finishedSpy.wait(60000);
        QVERIFY2(finished, "streamingFinished must arrive within the timeout");
        QCOMPARE(model->rowCount(), 300);

        const int levelCol = ColumnByHeader(*model, QStringLiteral("category"));
        QVERIFY2(levelCol >= 0, "level column must exist");
        const auto &columns = model->Layout().Columns();
        QCOMPARE(columns[static_cast<size_t>(levelCol)].type, loglib::ColumnType::Enumeration);

        // Find the Filters-menu action whose data matches `filterId`.
        const auto findFilterMenuAction = [&](const QString &filterId) -> QAction * {
            const auto *menu = mWindow->findChild<QMenu *>(QStringLiteral("menuFilters"));
            if (menu == nullptr)
            {
                return nullptr;
            }
            for (QAction *action : menu->actions())
            {
                if (action->data().toString() == filterId)
                {
                    return action;
                }
            }
            return nullptr;
        };

        // Stage 1: install a string rule (simulates a saved pre-promotion filter).
        const QString filterId = QStringLiteral("saved-string");
        QVERIFY2(
            QMetaObject::invokeMethod(
                mWindow,
                "FilterSubmitted",
                Qt::DirectConnection,
                Q_ARG(QString, filterId),
                Q_ARG(int, levelCol),
                Q_ARG(QString, QStringLiteral("info")),
                Q_ARG(int, static_cast<int>(loglib::LeafRule::Match::Exactly))
            ),
            "FilterSubmitted slot must be invocable via meta-object"
        );
        QCoreApplication::processEvents();
        QVERIFY2(findFilterMenuAction(filterId) != nullptr, "active filter must have a menu entry before the drop");

        // 3 levels across 300 rows -> ~100 `info` rows pass. Reach the
        // proxy via `MainWindow::FilterModel()`: `findChildren<
        // QSortFilterProxyModel*>()` is similarly stranded on the Linux
        // offscreen runner.
        const LogFilterModel *filterModel = mWindow->FilterModel();
        QVERIFY2(filterModel != nullptr, "MainWindow must own a LogFilterModel proxy");
        const int filteredRowCount = filterModel->rowCount();
        QVERIFY2(
            filteredRowCount > 0 && filteredRowCount < 300,
            qPrintable(QStringLiteral("active filter must trim row count; got %1").arg(filteredRowCount))
        );

        // Stage 2: replay the saved string filter against the now-enum
        // column. The type-mismatch guard must drop the active rule.
        loglib::LeafRule savedFilter;
        savedFilter.type = loglib::LeafRule::Type::String;
        savedFilter.columnKeys = mWindow->Model()->Layout().Columns()[static_cast<size_t>(levelCol)].keys;
        savedFilter.filterString = std::string("info");
        savedFilter.matchType = loglib::LeafRule::Match::Exactly;

        mWindow->statusBar()->clearMessage();
        // `AddFilter` is private; invoke via the meta-object system.
        const bool invoked = QMetaObject::invokeMethod(
            mWindow,
            "AddFilter",
            Qt::DirectConnection,
            Q_ARG(QString, filterId),
            Q_ARG(std::optional<loglib::LeafRule>, savedFilter)
        );
        QVERIFY2(invoked, "AddFilter slot must be invocable via meta-object");
        QCoreApplication::processEvents();

        const QString message = mWindow->statusBar()->currentMessage();
        QVERIFY2(
            message.contains(QStringLiteral("was removed because the column type changed"), Qt::CaseInsensitive),
            qPrintable(QStringLiteral("expected status-bar drop message; got '%1'").arg(message))
        );

        // Active rule and menu entry are gone.
        QVERIFY2(findFilterMenuAction(filterId) == nullptr, "menu entry for the dropped filter must be removed");
        // Unfiltered total: no active rule for `filterId`.
        QCOMPARE(filterModel->rowCount(), 300);

        // Close the empty editor the drop path opens.
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

    // The bitset fast path and the string fallback must accept the same
    // rows on an enum-encoded column.
    void TestEnumFilterRuleFastPathMatchesFallbackPath()
    {
        // 4 levels rotated; selecting 2 gives a 50% pass rate.
        const QStringList levels{
            QStringLiteral("info"),
            QStringLiteral("warn"),
            QStringLiteral("error"),
            QStringLiteral("debug"),
        };
        constexpr int FIXTURE_LINES = 320;
        QStringList lines;
        lines.reserve(FIXTURE_LINES);
        for (int i = 0; i < FIXTURE_LINES; ++i)
        {
            lines.append(QStringLiteral(R"({"category": "%1", "n": %2})").arg(levels[i % levels.size()]).arg(i));
        }
        const TempJsonFile fixture(lines);

        const StreamingRun run = RunStreaming(fixture.Path());
        QCOMPARE(run.finishedCount, 1);
        QCOMPARE(run.cancelled, false);
        QVERIFY(run.model->StreamingErrors().empty());

        const int levelCol = ColumnByHeader(*run.model, QStringLiteral("category"));
        QVERIFY2(levelCol >= 0, "level column must exist");
        const auto &columns = run.model->Layout().Columns();
        QCOMPARE(columns[static_cast<size_t>(levelCol)].type, loglib::ColumnType::Enumeration);

        const loglib::EnumDictionary *dictionary = nullptr;
        QVERIFY(!columns[static_cast<size_t>(levelCol)].keys.empty());
        const loglib::KeyId canonicalKeyId =
            run.model->Table().Keys().Find(columns[static_cast<size_t>(levelCol)].keys.front());
        QVERIFY(canonicalKeyId != loglib::INVALID_KEY_ID);
        dictionary = run.model->Table().EnumDictionaries().Find(canonicalKeyId);
        QVERIFY2(dictionary != nullptr, "the enum-encoded column must have a dictionary");

        const std::vector<std::string> selectedHolders = {"info", "warn"};
        std::vector<std::string_view> selectedViews;
        selectedViews.reserve(selectedHolders.size());
        for (const auto &v : selectedHolders)
        {
            selectedViews.emplace_back(v);
        }
        const loglib::EnumRowPredicate fastRule(static_cast<size_t>(levelCol), selectedViews, dictionary);
        const loglib::EnumRowPredicate fallbackRule(
            static_cast<size_t>(levelCol), selectedViews, /*dictionary=*/nullptr
        );

        const int rows = run.model->rowCount();
        QCOMPARE(rows, FIXTURE_LINES);

        int matchedRows = 0;
        const loglib::LogTable &table = run.model->Table();
        for (int row = 0; row < rows; ++row)
        {
            const auto r = static_cast<size_t>(row);
            const bool fast = fastRule.MatchesRow(table, r);
            const bool fallback = fallbackRule.MatchesRow(table, r);

            QVERIFY2(
                fast == fallback,
                qPrintable(QStringLiteral("row %1: fast=%2 vs fallback=%3").arg(row).arg(fast).arg(fallback))
            );
            if (fast)
            {
                ++matchedRows;
            }
        }

        QCOMPARE(matchedRows, FIXTURE_LINES / 2);

        // Every enum-encoded row exposes `EnumValueRole`.
        for (int row = 0; row < rows; ++row)
        {
            const QVariant enumId =
                run.model->data(run.model->index(row, levelCol), LogModelItemDataRole::EnumValueRole);
            QVERIFY2(
                enumId.isValid(),
                qPrintable(QStringLiteral("row %1: EnumValueRole must be valid post-promotion").arg(row))
            );
        }
    }

    // Regression: `LogModel` must emit `enumColumnsChanged` on
    // demote (registry entry erased), not only on dict growth. The
    // post-batch column type is `String`, so the back-fill branch
    // can't detect it -- only the registry-shape loop can.
    void TestEnumColumnsChangedFiresOnDemote()
    {
        auto *model = mWindow->findChild<LogModel *>();
        QVERIFY2(model != nullptr, "MainWindow must own a LogModel");

        // Two batches needed: batch 1 promotes, batch 2 overflows the
        // cap. The streaming flush would collapse small fixtures, so
        // we feed `AppendBatch` directly.
        const TempJsonFile emptyFixture(QStringList{});
        auto fileSource = std::make_unique<loglib::FileLineSource>(emptyFixture.Path().toStdString());
        loglib::FileLineSource *sourcePtr = fileSource.get();
        (void)model->BeginStreamingForSyncTest(std::move(fileSource));

        // Tiny cap so the 3rd distinct value demotes to `Type::String`.
        // Set after `BeginStreamingForSyncTest` (which resets the table).
        constexpr uint16_t TEST_CAP = 2;
        model->Table().SetEnumValueCap(TEST_CAP);

        QSignalSpy enumChangedSpy(model, &LogModel::enumColumnsChanged);
        QVERIFY(enumChangedSpy.isValid());

        loglib::KeyIndex &keys = model->Table().Keys();
        const auto makeLine = [&](const std::string &value) {
            std::vector<std::pair<loglib::KeyId, loglib::LogValue>> values;
            values.emplace_back(keys.GetOrInsert("category"), loglib::LogValue(value));
            return loglib::LogLine{std::move(values), *sourcePtr, 0};
        };

        // Batch 1: 2x "info" promotes `level` (stream threshold = 2).
        // Dict size 0 -> 1.
        {
            loglib::RowBatch batch;
            batch.firstLineNumber = 1;
            batch.newKeys.emplace_back("category");
            batch.lines.push_back(makeLine("info"));
            batch.lines.push_back(makeLine("info"));
            model->AppendBatch(std::move(batch));
        }

        const int levelCol = ColumnByHeader(*model, QStringLiteral("category"));
        QVERIFY2(levelCol >= 0, "level column must exist after promoting batch");
        QCOMPARE(model->Layout().Columns()[static_cast<size_t>(levelCol)].type, loglib::ColumnType::Enumeration);
        const loglib::KeyId levelKey = keys.Find("category");
        QVERIFY(levelKey != loglib::INVALID_KEY_ID);
        const loglib::EnumDictionary *dict = model->Table().EnumDictionaries().Find(levelKey);
        QVERIFY2(dict != nullptr, "promotion must create a dictionary entry");
        QCOMPARE(static_cast<int>(dict->Size()), 1);

        // Promotion drives `enumColumnsChanged` via the back-fill
        // loop. The reason must be `Promoted` so receivers skip the
        // rank-cache flush (the cache has no entry for this key yet).
        QVERIFY2(enumChangedSpy.count() >= 1, "enumColumnsChanged must fire on promotion");
        bool sawPromotion = false;
        for (const auto &args : enumChangedSpy)
        {
            if (args.at(0).value<EnumColumnsChangeReason>() == EnumColumnsChangeReason::Promoted)
            {
                sawPromotion = true;
                break;
            }
        }
        QVERIFY2(sawPromotion, "promotion batch must emit at least one `Promoted` reason");
        enumChangedSpy.clear();

        // Batch 2: "warn" fits the cap; "error" is the cap+1th value,
        // demoting to `String` and erasing the registry entry. The
        // back-fill enum-type check sees `String` post-batch, so the
        // registry-shape path is the only signal site.
        {
            loglib::RowBatch batch;
            batch.firstLineNumber = 3;
            batch.lines.push_back(makeLine("warn"));
            batch.lines.push_back(makeLine("error"));
            model->AppendBatch(std::move(batch));
        }

        QCOMPARE(model->Layout().Columns()[static_cast<size_t>(levelCol)].type, loglib::ColumnType::String);
        QVERIFY2(
            model->Table().EnumDictionaries().Find(levelKey) == nullptr, "demotion must erase the dictionary entry"
        );

        QVERIFY2(
            enumChangedSpy.count() >= 1,
            qPrintable(QStringLiteral("expected enumColumnsChanged on demote (registry erase); got %1")
                           .arg(enumChangedSpy.count()))
        );
        bool sawDemote = false;
        for (const auto &args : enumChangedSpy)
        {
            if (args.at(0).value<EnumColumnsChangeReason>() == EnumColumnsChangeReason::Demoted)
            {
                sawDemote = true;
                break;
            }
        }
        QVERIFY2(sawDemote, "demote batch must emit at least one `Demoted` reason");

        model->EndStreaming(false);
    }

    // Regression: a `Grew` `enumColumnsChanged` must NOT flush the
    // `EnumDictRank` cache through `MainWindow`'s slot. `EnumRankFor`
    // self-heals on the next sort via its `DictSize()` check, so
    // flushing on every batch would re-rebuild ranks per batch and
    // dominate the wall-clock of a busy stream.
    void TestEnumRankCacheSurvivesGrowthSignalThroughMainWindow()
    {
        auto *model = mWindow->findChild<LogModel *>();
        QVERIFY2(model != nullptr, "MainWindow must own a LogModel");
        auto *filterModel = mWindow->FilterModel();
        QVERIFY2(filterModel != nullptr, "MainWindow must own a LogFilterModel");

        const TempJsonFile emptyFixture(QStringList{});
        auto fileSource = std::make_unique<loglib::FileLineSource>(emptyFixture.Path().toStdString());
        loglib::FileLineSource *sourcePtr = fileSource.get();
        (void)model->BeginStreamingForSyncTest(std::move(fileSource));

        loglib::KeyIndex &keys = model->Table().Keys();
        const auto makeLine = [&](const std::string &value) {
            std::vector<std::pair<loglib::KeyId, loglib::LogValue>> values;
            values.emplace_back(keys.GetOrInsert("category"), loglib::LogValue(value));
            return loglib::LogLine{std::move(values), *sourcePtr, 0};
        };

        // Batch 1 promotes "category" (stream threshold = 2).
        {
            loglib::RowBatch batch;
            batch.firstLineNumber = 1;
            batch.newKeys.emplace_back("category");
            batch.lines.push_back(makeLine("info"));
            batch.lines.push_back(makeLine("warn"));
            model->AppendBatch(std::move(batch));
        }
        const int levelCol = ColumnByHeader(*model, QStringLiteral("category"));
        QVERIFY2(levelCol >= 0, "level column must exist after promotion");

        // Warm the rank cache by sorting on the enum column.
        filterModel->sort(levelCol, Qt::AscendingOrder);
        QCoreApplication::processEvents();
        QCOMPARE(filterModel->EnumRankCacheSizeForTest(), std::size_t{1});

        // Batch 2 grows the dictionary. `Grew` is a no-op for the
        // rank cache so the entry stays.
        {
            loglib::RowBatch batch;
            batch.firstLineNumber = 3;
            batch.lines.push_back(makeLine("error"));
            batch.lines.push_back(makeLine("info"));
            model->AppendBatch(std::move(batch));
        }
        QCoreApplication::processEvents();
        QCOMPARE(filterModel->EnumRankCacheSizeForTest(), std::size_t{1});

        model->EndStreaming(false);
    }

    // Regression: a column auto-promoted to `Type::Enumeration` and
    // demoted back to `Type::String` inside the same batch (encode
    // overflows the dict cap) must still emit `Demoted` so
    // consumers can rebuild any rank-cache entry or filter rule
    // that aliased the transient dictionary.
    void TestEnumDemotedSignalFiresOnSameBatchPromoteThenDemote()
    {
        auto *model = mWindow->findChild<LogModel *>();
        QVERIFY2(model != nullptr, "MainWindow must own a LogModel");

        // Tiny dictionary cap: the promotion encode pass will trip it
        // as soon as it sees a third distinct value.
        model->Table().SetEnumValueCap(2);

        const TempJsonFile emptyFixture(QStringList{});
        auto fileSource = std::make_unique<loglib::FileLineSource>(emptyFixture.Path().toStdString());
        loglib::FileLineSource *sourcePtr = fileSource.get();
        (void)model->BeginStreamingForSyncTest(std::move(fileSource));

        const QSignalSpy enumChangedSpy(model, &LogModel::enumColumnsChanged);
        QVERIFY(enumChangedSpy.isValid());

        loglib::KeyIndex &keys = model->Table().Keys();
        const auto makeLine = [&](const std::string &value) {
            std::vector<std::pair<loglib::KeyId, loglib::LogValue>> values;
            values.emplace_back(keys.GetOrInsert("category"), loglib::LogValue(value));
            return loglib::LogLine{std::move(values), *sourcePtr, 0};
        };

        // One batch, five rows: the candidate scan early-bails after
        // two "info" rows so stream-mode promotion fires. The
        // `PromoteColumnToEnum` encode pass then trips the cap on the
        // third distinct value, demoting back to `Type::String` --
        // all inside a single `AppendBatch`.
        {
            loglib::RowBatch batch;
            batch.firstLineNumber = 1;
            batch.newKeys.emplace_back("category");
            batch.lines.push_back(makeLine("info"));
            batch.lines.push_back(makeLine("info"));
            batch.lines.push_back(makeLine("warn"));
            batch.lines.push_back(makeLine("error"));
            batch.lines.push_back(makeLine("fatal"));
            model->AppendBatch(std::move(batch));
        }
        QCoreApplication::processEvents();

        const int levelCol = ColumnByHeader(*model, QStringLiteral("category"));
        QVERIFY2(levelCol >= 0, "level column must exist post-batch");
        QCOMPARE(model->Layout().Columns()[static_cast<size_t>(levelCol)].type, loglib::ColumnType::String);
        const loglib::KeyId levelKey = model->Table().Keys().Find("category");
        QVERIFY2(
            model->Table().EnumDictionaries().Find(levelKey) == nullptr,
            "transient dictionary must have been erased by the in-batch demote"
        );

        bool sawDemote = false;
        for (const auto &args : enumChangedSpy)
        {
            if (args.at(0).value<EnumColumnsChangeReason>() == EnumColumnsChangeReason::Demoted)
            {
                sawDemote = true;
                break;
            }
        }
        QVERIFY2(sawDemote, "same-batch promote+demote must still emit a Demoted enumColumnsChanged");

        model->EndStreaming(false);
    }

    // Regression: after a demote, `MainWindow::enumColumnsChanged`
    // must rebuild every active enum filter so the predicate
    // falls back to its string-set path. A cached
    // "fully-resolved" snapshot would let a stale armed predicate
    // hide every row (post-demote `GetEnumValueId` returns nullopt).
    // Regression: a `Type::Level -> String` mid-session demote must
    // keep matching the same rows. Filter values are canonical names
    // (`"Info"`, ...), but the demoted column carries raw bytes --
    // `LogModel::AppendBatch` snapshots the level -> raw-bytes
    // mapping pre-demote and `MainWindow`'s `Demoted` handler
    // rewrites `filterValues` to the raw bytes.
    void TestLevelFilterTranslatesOnDemoteToString()
    {
        auto *model = mWindow->findChild<LogModel *>();
        QVERIFY2(model != nullptr, "MainWindow must own a LogModel");

        const TempJsonFile emptyFixture(QStringList{});
        auto fileSource = std::make_unique<loglib::FileLineSource>(emptyFixture.Path().toStdString());
        loglib::FileLineSource *sourcePtr = fileSource.get();
        (void)model->BeginStreamingForSyncTest(std::move(fileSource));

        // Cap of 3 lets batch 1 promote to Level (dict size 2). Batch 2
        // adds three new distinct values, trips the cap, demotes to String.
        constexpr uint16_t TEST_CAP = 3;
        model->Table().SetEnumValueCap(TEST_CAP);

        loglib::KeyIndex &keys = model->Table().Keys();
        const auto makeLine = [&](const std::string &value) {
            std::vector<std::pair<loglib::KeyId, loglib::LogValue>> values;
            values.emplace_back(keys.GetOrInsert("level"), loglib::LogValue(value));
            return loglib::LogLine{std::move(values), *sourcePtr, 0};
        };

        // Batch 1: 2x `info` + 1x `warn` promotes through Enumeration
        // to Level (`IsLogLevelKey` match + canonical dict entries).
        {
            loglib::RowBatch batch;
            batch.firstLineNumber = 1;
            batch.newKeys.emplace_back("level");
            batch.lines.push_back(makeLine("info"));
            batch.lines.push_back(makeLine("info"));
            batch.lines.push_back(makeLine("warn"));
            model->AppendBatch(std::move(batch));
        }

        const int levelCol = ColumnByHeader(*model, QStringLiteral("level"));
        QVERIFY2(levelCol >= 0, "level column must exist after promotion");
        QCOMPARE(model->Layout().Columns()[static_cast<size_t>(levelCol)].type, loglib::ColumnType::Level);

        // Submit `Info` through FilterEditor. Only the two `info`
        // rows out of three should survive.
        const QString filterId = QStringLiteral("level-demote-translation");
        QVERIFY2(
            QMetaObject::invokeMethod(
                mWindow,
                "FilterEnumSubmitted",
                Qt::DirectConnection,
                Q_ARG(QString, filterId),
                Q_ARG(int, levelCol),
                Q_ARG(QStringList, QStringList{QStringLiteral("Info")})
            ),
            "FilterEnumSubmitted slot must be invocable via meta-object"
        );
        QCoreApplication::processEvents();

        const LogFilterModel *filterModel = mWindow->FilterModel();
        QVERIFY2(filterModel != nullptr, "MainWindow must own a LogFilterModel proxy");
        QCOMPARE(filterModel->rowCount(), 2);

        // Batch 2: three new values overflow the cap, column demotes
        // to String. The Demoted handler expands `["Info"]` to the
        // raw entries pre-demote (`["info"]`).
        {
            loglib::RowBatch batch;
            batch.firstLineNumber = 4;
            batch.lines.push_back(makeLine("error"));
            batch.lines.push_back(makeLine("debug"));
            batch.lines.push_back(makeLine("trace"));
            model->AppendBatch(std::move(batch));
        }
        QCoreApplication::processEvents();

        QCOMPARE(model->Layout().Columns()[static_cast<size_t>(levelCol)].type, loglib::ColumnType::String);

        // Translation succeeded iff the two `info` rows still match
        // post-demote. Without it, `["Info"]` would byte-compare
        // against `info` and reject every row.
        QCOMPARE(filterModel->rowCount(), 2);
        for (int i = 0; i < filterModel->rowCount(); ++i)
        {
            QCOMPARE(filterModel->index(i, levelCol).data(Qt::DisplayRole).toString(), QStringLiteral("info"));
        }

        model->EndStreaming(false);
    }

    void TestEnumFilterRebuiltAfterDemote()
    {
        auto *model = mWindow->findChild<LogModel *>();
        QVERIFY2(model != nullptr, "MainWindow must own a LogModel");

        // Two-batch path (mirrors `TestEnumColumnsChangedFiresOnDemote`):
        // batch 2 must trip the cap, so we feed `AppendBatch` directly.
        const TempJsonFile emptyFixture(QStringList{});
        auto fileSource = std::make_unique<loglib::FileLineSource>(emptyFixture.Path().toStdString());
        loglib::FileLineSource *sourcePtr = fileSource.get();
        (void)model->BeginStreamingForSyncTest(std::move(fileSource));

        constexpr uint16_t TEST_CAP = 2;
        model->Table().SetEnumValueCap(TEST_CAP);

        loglib::KeyIndex &keys = model->Table().Keys();
        const auto makeLine = [&](const std::string &value) {
            std::vector<std::pair<loglib::KeyId, loglib::LogValue>> values;
            values.emplace_back(keys.GetOrInsert("category"), loglib::LogValue(value));
            return loglib::LogLine{std::move(values), *sourcePtr, 0};
        };

        // Batch 1: two "info" rows promote `level`. Dict size 1;
        // predicate built against it observes `mAllResolved == true`.
        {
            loglib::RowBatch batch;
            batch.firstLineNumber = 1;
            batch.newKeys.emplace_back("category");
            batch.lines.push_back(makeLine("info"));
            batch.lines.push_back(makeLine("info"));
            model->AppendBatch(std::move(batch));
        }

        const int levelCol = ColumnByHeader(*model, QStringLiteral("category"));
        QVERIFY2(levelCol >= 0, "level column must exist after promotion");
        QCOMPARE(model->Layout().Columns()[static_cast<size_t>(levelCol)].type, loglib::ColumnType::Enumeration);

        // Install an enum filter selecting the one resolved value via
        // `FilterEnumSubmitted` (the FilterEditor slot).
        const QString filterId = QStringLiteral("post-demote-rebuild");
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

        const LogFilterModel *filterModel = mWindow->FilterModel();
        QVERIFY2(filterModel != nullptr, "MainWindow must own a LogFilterModel proxy");
        QCOMPARE(filterModel->rowCount(), 2); // both info rows visible

        // Batch 2: "warn" fits, "error" overflows -> demote to String.
        // `Find` returns nullptr, `EnumFilterFullyResolved` is false,
        // and the slot rebuilds the predicate.
        {
            loglib::RowBatch batch;
            batch.firstLineNumber = 3;
            batch.lines.push_back(makeLine("warn"));
            batch.lines.push_back(makeLine("error"));
            model->AppendBatch(std::move(batch));
        }
        QCoreApplication::processEvents();

        QCOMPARE(model->Layout().Columns()[static_cast<size_t>(levelCol)].type, loglib::ColumnType::String);

        // Rebuilt predicate has no dictionary, so the string-set
        // fallback runs against the demoted column's string slots:
        // both "info" rows match; "warn" / "error" do not.
        QCOMPARE(filterModel->rowCount(), 2);

        // Confirm the visible rows are the "info" ones.
        const int levelColInProxy = levelCol; // proxy preserves column layout
        for (int i = 0; i < filterModel->rowCount(); ++i)
        {
            const QModelIndex idx = filterModel->index(i, levelColInProxy);
            QCOMPARE(idx.data(Qt::DisplayRole).toString(), QStringLiteral("info"));
        }

        model->EndStreaming(false);
    }

    // Regression: the `EnumDictRank` cache is keyed by canonical
    // `KeyId`, not column index, so a column reorder no longer
    // requires a `columnsMoved` invalidation hook -- the cached rank
    // stays attached to the same logical column.
    void TestEnumRankCacheSurvivesColumnsMoved()
    {
        auto *model = mWindow->findChild<LogModel *>();
        QVERIFY2(model != nullptr, "MainWindow must own a LogModel");

        // Two-column fixture where "category" promotes to enum (one
        // batch -- 200 lines is below the streaming threshold).
        const QStringList levels{
            QStringLiteral("info"), QStringLiteral("warn"), QStringLiteral("error"), QStringLiteral("debug")
        };
        QStringList lines;
        lines.reserve(200);
        for (int i = 0; i < 200; ++i)
        {
            lines.append(QStringLiteral(R"({"category": "%1", "msg": "m%2"})").arg(levels[i % levels.size()]).arg(i));
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

        const int levelCol = ColumnByHeader(*model, QStringLiteral("category"));
        QVERIFY2(levelCol >= 0, "level column must exist");
        QCOMPARE(model->Layout().Columns()[static_cast<size_t>(levelCol)].type, loglib::ColumnType::Enumeration);

        LogFilterModel *filterModel = mWindow->FilterModel();
        QVERIFY2(filterModel != nullptr, "MainWindow must own a LogFilterModel proxy");

        // Warm the rank cache: sort by the enum column.
        filterModel->sort(levelCol, Qt::AscendingOrder);
        QCoreApplication::processEvents();
        const std::size_t cacheSizeBefore = filterModel->EnumRankCacheSizeForTest();
        QVERIFY2(cacheSizeBefore >= std::size_t{1}, "sorting by an enum column must populate the rank cache");

        // Reorder columns; the rank cache is KeyId-keyed and
        // survives.
        QVERIFY2(model->columnCount() >= 2, "fixture must have at least two columns");
        const QSignalSpy columnsMovedSpy(model, &QAbstractItemModel::columnsMoved);
        // Pin the proxy-level forwarding contract too: `LogFilterModel`
        // must forward `columnsMoved` so downstream views see the
        // bracketed structural change during a streaming session
        // late-promoted a `Type::Time` column. Post-fix the proxy
        // forwards begin/endMoveColumns through to the view.
        const QSignalSpy filterColumnsAboutToBeMovedSpy(filterModel, &QAbstractItemModel::columnsAboutToBeMoved);
        const QSignalSpy filterColumnsMovedSpy(filterModel, &QAbstractItemModel::columnsMoved);
        QVERIFY(columnsMovedSpy.isValid());
        QVERIFY(filterColumnsAboutToBeMovedSpy.isValid());
        QVERIFY(filterColumnsMovedSpy.isValid());
        const int src = (levelCol == 0) ? 1 : 0;
        const int dest = (levelCol == 0) ? 0 : 1;
        QVERIFY2(model->MoveColumn(src, dest), "MoveColumn must succeed");
        QCoreApplication::processEvents();
        QVERIFY2(columnsMovedSpy.count() >= 1, "MoveColumn must emit columnsMoved");
        QVERIFY2(
            filterColumnsAboutToBeMovedSpy.count() >= 1,
            "LogFilterModel must forward columnsAboutToBeMoved from its source"
        );
        QVERIFY2(filterColumnsMovedSpy.count() >= 1, "LogFilterModel must forward columnsMoved from its source");

        // Cache entry survives the reorder (KeyId-keyed).
        QCOMPARE(filterModel->EnumRankCacheSizeForTest(), cacheSizeBefore);

        // Re-sort against the column's new position. The proxy
        // tracks the sort column in source-coords, so a reorder
        // shifts what that index points at -- we re-issue `sort()`.
        // The lookup hits the cache (KeyId match), no rebuild.
        const int levelColAfter = ColumnByHeader(*model, QStringLiteral("category"));
        QVERIFY2(levelColAfter >= 0, "level column must still exist after the move");
        filterModel->sort(levelColAfter, Qt::AscendingOrder);
        QCoreApplication::processEvents();
        QCOMPARE(filterModel->EnumRankCacheSizeForTest(), cacheSizeBefore);

        QStringList sortedLevelsAfter;
        sortedLevelsAfter.reserve(filterModel->rowCount());
        for (int i = 0; i < filterModel->rowCount(); ++i)
        {
            sortedLevelsAfter.append(filterModel->index(i, levelColAfter).data(Qt::DisplayRole).toString());
        }
        // Ascending alphabetical: debug < error < info < warn (50 of each).
        QCOMPARE(sortedLevelsAfter.front(), QStringLiteral("debug"));
        QCOMPARE(sortedLevelsAfter.back(), QStringLiteral("warn"));
        for (int i = 1; i < sortedLevelsAfter.size(); ++i)
        {
            QVERIFY2(
                sortedLevelsAfter[i - 1] <= sortedLevelsAfter[i],
                qPrintable(QStringLiteral("rows out of order: %1 > %2 at index %3")
                               .arg(sortedLevelsAfter[i - 1])
                               .arg(sortedLevelsAfter[i])
                               .arg(i))
            );
        }

        model->EndStreaming(false);
    }

    // `SetColumnVisible(idx, false)` flips `Column::visible` and
    // hides the matching header section.
    void TestHideColumnUpdatesHeaderAndConfiguration()
    {
        const int levelCol = StreamFixtureForColumnTests();
        QVERIFY2(levelCol >= 0, "level column must exist after streaming");

        auto *model = mWindow->Model();
        QVERIFY2(model != nullptr, "MainWindow must own a LogModel");
        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): false positive; prior `QVERIFY2` aborts on null.
        QVERIFY2(
            model->Layout().Columns()[static_cast<size_t>(levelCol)].visible,
            "column must default to visible before hide"
        );

        mWindow->SetColumnVisible(levelCol, false);

        QVERIFY2(
            !model->Layout().Columns()[static_cast<size_t>(levelCol)].visible, "Column::visible must flip to false"
        );
        const QHeaderView *header = mWindow->findChild<LogTableView *>()->horizontalHeader();
        QVERIFY2(header != nullptr, "view must own a horizontal header");
        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): false positive; prior `QVERIFY2` aborts on null.
        QVERIFY2(header->isSectionHidden(levelCol), "header section must be hidden");
    }

    // The header menu must never advertise a hidden column on a
    // visible-column right-click: re-showing hidden columns is
    // intentionally delegated to the `View` menu (the only escape
    // hatch when *every* column is hidden, since no header section
    // is left to right-click). Built against a visible clicked
    // column so the `Hide` entry is present and the contrast is
    // sharp. Pinned so a future regression that reintroduces a
    // `Show column` submenu trips here.
    void TestHeaderContextMenuOmitsShowColumnSubmenu()
    {
        const int levelCol = StreamFixtureForColumnTests();
        QVERIFY2(levelCol >= 0, "level column must exist after streaming");
        auto *model = mWindow->Model();
        const int msgCol = ColumnByHeader(*model, QStringLiteral("msg"));
        QVERIFY2(msgCol >= 0, "msg column must exist after streaming");

        mWindow->SetColumnVisible(msgCol, false);

        auto built = mWindow->BuildHeaderContextMenu(levelCol, nullptr);
        QVERIFY2(built.menu != nullptr, "BuildHeaderContextMenu must return a menu");
        const QScopeGuard menuDeleter([&built]() { built.menu->deleteLater(); });

        const QList<QAction *> topActions = built.menu->actions();
        QVERIFY2(!topActions.isEmpty(), "visible-column menu must contain at least the Hide entry");
        QVERIFY2(topActions.front()->text().startsWith("Hide"), "first action must be the Hide entry");

        for (const QAction *act : topActions)
        {
            QVERIFY2(
                act->text() != QStringLiteral("Show column"),
                "header context menu must not offer a Show column submenu (use the View menu)"
            );
            QVERIFY2(
                act->text() != QStringLiteral("msg"),
                "header context menu must not list the hidden `msg` column as an action"
            );
        }
    }

    // For a hidden column the menu must omit both `Hide` and
    // `Add filter on ...` (both would be confusing no-ops). The
    // hidden-column right-click is only reachable via the test
    // seam -- production right-clicks fire on visible sections --
    // and re-showing hidden columns is delegated to the `View`
    // menu, so the resulting menu is empty in this branch.
    void TestHeaderContextMenuOmitsHideForHiddenColumn()
    {
        const int levelCol = StreamFixtureForColumnTests();
        QVERIFY2(levelCol >= 0, "level column must exist after streaming");

        mWindow->SetColumnVisible(levelCol, false);

        auto built = mWindow->BuildHeaderContextMenu(levelCol, nullptr);
        QVERIFY2(built.menu != nullptr, "BuildHeaderContextMenu must return a menu");
        const QScopeGuard menuDeleter([&built]() { built.menu->deleteLater(); });

        for (const QAction *act : built.menu->actions())
        {
            QVERIFY2(
                !act->text().startsWith("Hide"), "menu rooted at a hidden column must not advertise a Hide action"
            );
            QVERIFY2(
                !act->text().startsWith("Add filter on"),
                "menu rooted at a hidden column must not advertise an Add-filter action"
            );
        }
    }

    // Triggering Add filter opens a `FilterEditor` with the row
    // combobox already pointed at the clicked column.
    void TestHeaderContextMenuAddFilterOpensEditorWithColumnPreselected()
    {
        const int levelCol = StreamFixtureForColumnTests();
        QVERIFY2(levelCol >= 0, "level column must exist after streaming");

        auto built = mWindow->BuildHeaderContextMenu(levelCol, nullptr);
        QVERIFY2(built.menu != nullptr, "BuildHeaderContextMenu must return a menu");
        const QScopeGuard menuDeleter([&built]() { built.menu->deleteLater(); });

        QAction *addFilterAction = nullptr;
        for (QAction *act : built.menu->actions())
        {
            if (act->text().startsWith(QStringLiteral("Add filter on")))
            {
                addFilterAction = act;
                break;
            }
        }
        QVERIFY2(addFilterAction != nullptr, "menu must contain an `Add filter on ...` action");

        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): false positive; prior `QVERIFY2` aborts on null.
        addFilterAction->trigger();
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
        QVERIFY2(editor != nullptr, "Add filter must spawn a FilterEditor");
        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): false positive; prior `QVERIFY2` aborts on null.
        QCOMPARE(editor->GetRowToFilter(), levelCol);

        editor->close();
        editor->deleteLater();
        QCoreApplication::processEvents();
    }

    // Every existing filter on the clicked column gets a per-filter
    // submenu (titled with its display value, mirroring the Filters
    // menu) carrying `Edit` and `Remove` actions.
    void TestHeaderContextMenuListsExistingFiltersForColumn()
    {
        const int levelCol = StreamFixtureForColumnTests();
        QVERIFY2(levelCol >= 0, "level column must exist after streaming");
        auto *model = mWindow->Model();
        const int msgCol = ColumnByHeader(*model, QStringLiteral("msg"));
        QVERIFY2(msgCol >= 0, "msg column must exist after streaming");

        // Two filters on `level`, one on `msg` (the latter must
        // not appear in the menu rooted at `level`).
        const QString levelFilter1 = QStringLiteral("level-filter-1");
        const QString levelFilter2 = QStringLiteral("level-filter-2");
        const QString msgFilter = QStringLiteral("msg-filter");
        QVERIFY2(
            QMetaObject::invokeMethod(
                mWindow,
                "FilterEnumSubmitted",
                Qt::DirectConnection,
                Q_ARG(QString, levelFilter1),
                Q_ARG(int, levelCol),
                Q_ARG(QStringList, QStringList{QStringLiteral("info")})
            ),
            "FilterEnumSubmitted slot must be invocable via meta-object"
        );
        QVERIFY2(
            QMetaObject::invokeMethod(
                mWindow,
                "FilterEnumSubmitted",
                Qt::DirectConnection,
                Q_ARG(QString, levelFilter2),
                Q_ARG(int, levelCol),
                Q_ARG(QStringList, QStringList{QStringLiteral("warn")})
            ),
            "FilterEnumSubmitted slot must be invocable via meta-object"
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
            "FilterSubmitted slot must be invocable via meta-object"
        );
        QCoreApplication::processEvents();
        QCOMPARE(mWindow->Filters().size(), static_cast<size_t>(3));

        auto built = mWindow->BuildHeaderContextMenu(levelCol, nullptr);
        QVERIFY2(built.menu != nullptr, "BuildHeaderContextMenu must return a menu");
        const QScopeGuard menuDeleter([&built]() { built.menu->deleteLater(); });

        const QList<QMenu *> subMenus = built.menu->findChildren<QMenu *>();
        QCOMPARE(subMenus.size(), 2);
        QVERIFY2(built.menu->findChild<QMenu *>(levelFilter1) != nullptr, "level-filter-1 submenu must be exposed");
        QVERIFY2(built.menu->findChild<QMenu *>(levelFilter2) != nullptr, "level-filter-2 submenu must be exposed");
        QVERIFY2(
            built.menu->findChild<QMenu *>(msgFilter) == nullptr,
            "filters on a different column must not appear in this header menu"
        );

        // Use MainWindow's translation context so a future
        // translation of "Edit" / "Remove" cannot silently break
        // this assertion.
        const QString editLabel = MainWindow::tr("Edit");
        const QString removeLabel = MainWindow::tr("Remove");
        for (const QMenu *subMenu : subMenus)
        {
            QVERIFY2(subMenu != nullptr, "filter sub-menu pointer must be non-null");
            const QList<QAction *> actions = subMenu->actions();
            QStringList labels;
            // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): false positive; prior `QVERIFY2` aborts on null.
            for (const QAction *act : actions)
            {
                labels.append(act->text());
            }
            QVERIFY2(labels.contains(editLabel), "filter sub-menu must contain an Edit action");
            QVERIFY2(labels.contains(removeLabel), "filter sub-menu must contain a Remove action");
        }
    }

    // Regression: the `Add filter on ...` lambda must re-resolve by
    // keys, so a column reorder between menu build and click still
    // targets the column's current index. Mirrors
    // `TestEditFilterAfterColumnReorderUsesCurrentRow`.
    void TestHeaderContextMenuAddFilterAfterColumnReorderResolvesByKeys()
    {
        const int levelCol = StreamFixtureForColumnTests();
        QVERIFY2(levelCol >= 0, "level column must exist after streaming");
        auto *model = mWindow->Model();
        const int columnCount = model->columnCount();
        QVERIFY2(columnCount >= 2, "fixture must yield at least two columns");

        auto built = mWindow->BuildHeaderContextMenu(levelCol, nullptr);
        QVERIFY2(built.menu != nullptr, "BuildHeaderContextMenu must return a menu");
        const QScopeGuard menuDeleter([&built]() { built.menu->deleteLater(); });

        QAction *addFilterAction = nullptr;
        for (QAction *act : built.menu->actions())
        {
            if (act->text().startsWith(QStringLiteral("Add filter on")))
            {
                addFilterAction = act;
                break;
            }
        }
        QVERIFY2(addFilterAction != nullptr, "menu must contain an `Add filter on ...` action");

        // Reorder `level` after the menu is built but before the
        // action is triggered; the lambda must re-resolve the index
        // from the captured keys.
        const int src = levelCol;
        const int dest = (src == 0) ? columnCount - 1 : 0;
        QVERIFY(src != dest);
        QVERIFY2(
            QMetaObject::invokeMethod(
                mWindow,
                "OnHeaderSectionMoved",
                Qt::DirectConnection,
                Q_ARG(int, src),
                Q_ARG(int, src),
                Q_ARG(int, dest)
            ),
            "OnHeaderSectionMoved slot must be invocable via meta-object"
        );
        QCoreApplication::processEvents();
        QCOMPARE(ColumnByHeader(*model, QStringLiteral("category")), dest);

        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): false positive; prior `QVERIFY2` aborts on null.
        addFilterAction->trigger();
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
        QVERIFY2(editor != nullptr, "Add filter must spawn a FilterEditor");
        // The editor's row must reflect the post-reorder index,
        // proving the lambda re-resolved by keys rather than
        // freezing on the build-time index.
        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): false positive; prior `QVERIFY2` aborts on null.
        QCOMPARE(editor->GetRowToFilter(), dest);

        editor->close();
        editor->deleteLater();
        QCoreApplication::processEvents();
    }

    // Without a `Type::Time` column the time-range actions drop out
    // but the Anchor sub-menu still keeps the row menu alive.
    void TestRowContextMenuOffersAnchorOnlyWithoutTimeColumn()
    {
        // The returned index is for `category`; used only as a
        // streaming-completed sentinel.
        const int streamedColumn = StreamFixtureForColumnTests();
        QVERIFY2(streamedColumn >= 0, "streaming must complete before the row-menu probe");

        QMenu *menu = mWindow->BuildRowContextMenu(/*sourceRow=*/0, nullptr);
        QVERIFY2(menu != nullptr, "BuildRowContextMenu must return the anchor-only menu when no time column exists");
        const QScopeGuard menuDeleter([&menu]() { menu->deleteLater(); });

        QVERIFY2(
            FindMenuActionByText(menu, MainWindow::tr("Anchor")) != nullptr,
            "anchor-only menu must still expose the Anchor sub-menu"
        );
        // No "Show only ..." entries when there is no time column.
        for (const QAction *action : menu->actions())
        {
            QVERIFY2(
                !action->text().startsWith(MainWindow::tr("Show only")),
                "menu must not expose time-range actions without a Type::Time column"
            );
        }
    }

    // Action lambdas must re-resolve the time column by its captured
    // keys at trigger time so a reorder between build and click still
    // targets the right column.
    void TestRowContextMenuAddTimeFilterAfterColumnReorderResolvesByKeys()
    {
        const int timeCol = StreamFixtureWithTimeColumnForRowMenuTests();
        QVERIFY2(timeCol >= 0, "timestamp column must exist after streaming");
        auto *model = mWindow->Model();
        const int columnCount = model->columnCount();
        QVERIFY2(columnCount >= 2, "fixture must yield at least two columns");

        QMenu *menu = mWindow->BuildRowContextMenu(/*sourceRow=*/0, nullptr);
        QVERIFY2(menu != nullptr, "BuildRowContextMenu must return a menu for a row with a timestamp");
        const QScopeGuard menuDeleter([&menu]() { menu->deleteLater(); });

        // Move the time column after building the menu, before
        // triggering. The captured keys must re-resolve to the new
        // index.
        const int src = timeCol;
        const int dest = (src == 0) ? columnCount - 1 : 0;
        QVERIFY(src != dest);
        QVERIFY2(
            QMetaObject::invokeMethod(
                mWindow,
                "OnHeaderSectionMoved",
                Qt::DirectConnection,
                Q_ARG(int, src),
                Q_ARG(int, src),
                Q_ARG(int, dest)
            ),
            "OnHeaderSectionMoved slot must be invocable via meta-object"
        );
        QCoreApplication::processEvents();
        const int timeColAfter = ColumnByHeader(*model, QStringLiteral("timestamp"));
        QCOMPARE(timeColAfter, dest);

        QAction *newerAction =
            FindMenuActionByText(menu, MainWindow::tr("Show only newer logs (%1)").arg(QStringLiteral("timestamp")));
        QVERIFY2(newerAction != nullptr, "menu must carry the newer-logs action");

        newerAction->trigger();
        QCoreApplication::processEvents();

        QCOMPARE(mWindow->Filters().size(), static_cast<size_t>(1));
        const auto &installed = mWindow->Filters().begin()->second;
        QCOMPARE(installed.columnKeys, model->Layout().Columns()[static_cast<size_t>(timeColAfter)].keys);
    }

    // `Column::visible` survives the Save / Reset / Load round-trip
    // (driven through the manager directly so the file dialogs
    // don't pop).
    void TestColumnVisibilityRoundTripsThroughSaveLoad()
    {
        const int levelCol = StreamFixtureForColumnTests();
        QVERIFY2(levelCol >= 0, "level column must exist after streaming");
        auto *model = mWindow->Model();

        mWindow->SetColumnVisible(levelCol, false);
        QVERIFY(!model->Layout().Columns()[static_cast<size_t>(levelCol)].visible);

        const QTemporaryDir savedDir;
        QVERIFY(savedDir.isValid());
        const QString savedPath = savedDir.filePath(QStringLiteral("config.json"));
        SaveLayoutForTest(*model, savedPath.toStdString());

        // `Reset` wipes the model; `Load` repopulates columns;
        // `ApplyColumnVisibility` is the production hook that pushes
        // the loaded flags onto the header.
        model->Reset();
        LoadLayoutForTest(*model, savedPath.toStdString());
        mWindow->ApplyColumnVisibility();
        QCoreApplication::processEvents();

        const auto &reloadedColumns = model->Layout().Columns();
        QVERIFY2(
            static_cast<size_t>(levelCol) < reloadedColumns.size(), "level column index must remain valid after reload"
        );
        QVERIFY2(
            !reloadedColumns[static_cast<size_t>(levelCol)].visible, "hidden flag must survive Save / Reset / Load"
        );

        const QHeaderView *header = mWindow->findChild<LogTableView *>()->horizontalHeader();
        QVERIFY2(header != nullptr, "view must own a horizontal header");
        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): false positive; prior `QVERIFY2` aborts on null.
        QVERIFY2(header->isSectionHidden(levelCol), "header section must remain hidden after reload");
    }

    // `LogModel::MoveColumn` rotates columns at the source level and
    // remaps `LogFilter::row` for every persisted filter through the
    // manager. The proxy chain forwards the move; the in-memory
    // `MainWindow::mFilters` map is updated by `OnHeaderSectionMoved`
    // when the move was initiated from a header drag, but a direct
    // `LogModel::MoveColumn` call only updates the lib-side
    // configuration. This test pins the lib-side remap on the
    // `LogConfiguration::filters` vector that `Save` would persist.
    void TestMoveColumnRemapsConfigurationFilters()
    {
        const int levelCol = StreamFixtureForColumnTests();
        QVERIFY2(levelCol >= 0, "level column must exist after streaming");
        auto *model = mWindow->Model();
        const int columnCount = model->columnCount();
        QVERIFY2(columnCount >= 2, "fixture must yield at least two columns");

        // Seed a saved filter on `level`. The public `AddFilter`
        // path opens an editor, so write through Save / Load instead
        // (the manager only exposes a const `Configuration()`).
        loglib::LogConfiguration configuration{.columns = model->Layout().Columns()};
        // Bind by column keys so the leaf survives column moves
        // without any remap step.
        QVERIFY(std::cmp_less(static_cast<size_t>(levelCol), configuration.columns.size()));
        const std::vector<std::string> levelKeys = configuration.columns[static_cast<size_t>(levelCol)].keys;
        loglib::FilterExpression::And andRoot;
        loglib::FilterExpression leafExpr;
        leafExpr.node = loglib::FilterExpression::Leaf{loglib::LeafRule{
            .type = loglib::LeafRule::Type::Enumeration,
            .columnKeys = levelKeys,
            .matchType = std::nullopt,
            .filterString = std::nullopt,
            .filterBegin = std::nullopt,
            .filterEnd = std::nullopt,
            .filterMinValue = std::nullopt,
            .filterMaxValue = std::nullopt,
            .filterValues = {"info"},
        }};
        andRoot.children.push_back(std::move(leafExpr));
        configuration.view.expression.node = std::move(andRoot);

        const QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        const QString path = tempDir.filePath(QStringLiteral("with-filter.json"));
        loglib::SaveConfiguration(configuration, path.toStdString(), loglib::SaveScope::Full);
        model->Reset();
        mWindow->SetSuppressDialogsForTest(true);
        mWindow->LoadConfigurationFromPathForTest(path);
        QCoreApplication::processEvents();

        const int levelAfterReload = ColumnByHeader(*model, QStringLiteral("category"));
        QVERIFY2(levelAfterReload >= 0, "level column must reload");

        auto extractSoleLeafKeys = [](const loglib::FilterExpression &expr) -> std::vector<std::string> {
            const auto *asAnd = std::get_if<loglib::FilterExpression::And>(&expr.node);
            if (asAnd == nullptr || asAnd->children.size() != 1)
            {
                return {};
            }
            const auto *leaf = std::get_if<loglib::FilterExpression::Leaf>(&asAnd->children[0].node);
            return leaf != nullptr ? leaf->rule.columnKeys : std::vector<std::string>{};
        };

        QCOMPARE(extractSoleLeafKeys(mWindow->activeSession()->View().expression), levelKeys);

        // Move `level`; the leaf's `columnKeys` stay the same, but
        // resolving them now yields the new column index.
        const int src = levelAfterReload;
        const int dest = (src == 0) ? model->columnCount() - 1 : 0;
        QVERIFY(src != dest);
        QVERIFY2(model->MoveColumn(src, dest), "MoveColumn must succeed");
        QCoreApplication::processEvents();

        QCOMPARE(extractSoleLeafKeys(mWindow->activeSession()->View().expression), levelKeys);
        QCOMPARE(ColumnByHeader(*model, QStringLiteral("category")), dest);
    }

    // Regression: an implicit source-side move (e.g. the streaming
    // timestamp bubble in `LogModel::AppendBatch`) used to leave
    // the runtime `mFilters` map pointing at the pre-move indices,
    // which `Save` would then mirror back over the lib-rotated
    // wire format and drop on next load. The fix wires
    // `OnSourceColumnsMoved` to `LogModel::columnsMoved`. This
    // exercises the slot through `LogModel::MoveColumn`, which
    // emits the same payload the bubble path does.
    void TestSourceColumnMoveRemapsRuntimeFilters()
    {
        const int levelCol = StreamFixtureForColumnTests();
        QVERIFY2(levelCol >= 0, "level column must exist after streaming");
        auto *model = mWindow->Model();
        const int columnCount = model->columnCount();
        QVERIFY2(columnCount >= 2, "fixture must yield at least two columns");

        // Install an enum filter on `level` via the production
        // `FilterEnumSubmitted` slot so both stores populate through
        // the supported entry point.
        const QString filterId = QStringLiteral("bubble-remap");
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

        const std::string filterKey = filterId.toStdString();
        QVERIFY2(mWindow->Filters().contains(filterKey), "filter must land in mSimpleLeaves after submit");
        const std::vector<std::string> expectedKeys = model->Layout().Columns()[static_cast<size_t>(levelCol)].keys;
        QCOMPARE(mWindow->Filters().at(filterKey).columnKeys, expectedKeys);

        // Bypass `OnHeaderSectionMoved` and emit `columnsMoved`
        // directly via `LogModel::MoveColumn` -- the streaming
        // bubble path runs the same code synchronously inside
        // `endMoveColumns()`.
        const int src = levelCol;
        const int dest = (src == 0) ? columnCount - 1 : 0;
        QVERIFY(src != dest);
        QVERIFY2(model->MoveColumn(src, dest), "MoveColumn must succeed");
        QCoreApplication::processEvents();

        // Runtime map and wire-format expression both survive the
        // move unchanged; the leaf still binds by keys.
        QVERIFY2(mWindow->Filters().contains(filterKey), "filter must survive the source-side move");
        QCOMPARE(mWindow->Filters().at(filterKey).columnKeys, expectedKeys);
        const auto &loadedExpr = mWindow->activeSession()->View().expression;
        const auto *loadedAnd = std::get_if<loglib::FilterExpression::And>(&loadedExpr.node);
        QVERIFY(loadedAnd != nullptr);
        QCOMPARE(loadedAnd->children.size(), static_cast<size_t>(1));
        const auto *loadedLeaf = std::get_if<loglib::FilterExpression::Leaf>(&loadedAnd->children[0].node);
        QVERIFY(loadedLeaf != nullptr);
        QCOMPARE(loadedLeaf->rule.columnKeys, expectedKeys);

        // The rule still targets `level`, so the proxy
        // should keep matching rows visible. A stale `row` would
        // silently zero this out.
        auto *tableView = mWindow->findChild<LogTableView *>();
        QVERIFY(tableView != nullptr);
        const QAbstractItemModel *proxy = tableView->model();
        QVERIFY(proxy != nullptr);
        QVERIFY2(proxy->rowCount() > 0, "filter on level=info must keep at least one row visible after the move");
    }

    // Regression: a source-side column move must rebind
    // `HighlightRuleSet`. Rules bind by keys, but the compiled
    // predicates cache *resolved* column indices; without a
    // rebind on `columnsMoved` those indices would drift and
    // highlights would tint the wrong field.
    void TestSourceColumnMoveRebindsHighlights()
    {
        const int categoryCol = StreamFixtureForColumnTests();
        QVERIFY2(categoryCol >= 0, "category column must exist after streaming");
        auto *model = mWindow->Model();
        auto *highlights = mWindow->Highlights();
        QVERIFY2(highlights != nullptr, "MainWindow must own a HighlightRuleSet");
        const int columnCount = model->columnCount();
        QVERIFY2(columnCount >= 2, "fixture must yield at least two columns");

        // Install a Contains rule via `SetRules` (same call path
        // as an editor Save).
        loglib::HighlightRule rule;
        rule.name = "hl-category-info";
        rule.enabled = true;
        rule.columnKeys = {"category"};
        rule.type = loglib::HighlightRule::Type::String;
        rule.matchType = loglib::HighlightRule::Match::Contains;
        rule.filterString = "info";
        rule.backgroundIndex = 1;
        highlights->SetRules({rule}, model->Layout().Columns(), &model->Table());

        const auto &resolvedBefore = highlights->ResolvedColumnsForTest();
        QCOMPARE(resolvedBefore.size(), static_cast<size_t>(1));
        QCOMPARE(resolvedBefore[0], categoryCol);
        // Sanity: at least one row matches before the move.
        bool anyMatch = false;
        for (int r = 0; r < model->rowCount() && !anyMatch; ++r)
        {
            anyMatch = highlights->LastMatchFor(static_cast<std::size_t>(r)).has_value();
        }
        QVERIFY2(anyMatch, "precondition: at least one row must match the highlight rule before the move");

        // Move `category` via the same entry point the header
        // drag / streaming bubble uses.
        const int src = categoryCol;
        const int dest = (src == 0) ? columnCount - 1 : 0;
        QVERIFY(src != dest);
        QVERIFY2(model->MoveColumn(src, dest), "MoveColumn must succeed");
        QCoreApplication::processEvents();

        // Rebind refreshed the predicate's column index.
        const auto &resolvedAfter = highlights->ResolvedColumnsForTest();
        QCOMPARE(resolvedAfter.size(), static_cast<size_t>(1));
        QCOMPARE(resolvedAfter[0], dest);
        QVERIFY(highlights->HasActiveRules());

        // Matches survive the move; a skipped rebind would zero
        // them out (predicate would read whatever now sits at src).
        bool anyMatchAfter = false;
        for (int r = 0; r < model->rowCount() && !anyMatchAfter; ++r)
        {
            anyMatchAfter = highlights->LastMatchFor(static_cast<std::size_t>(r)).has_value();
        }
        QVERIFY2(anyMatchAfter, "highlight matches must survive a source-side column move");
    }

    // Regression: a source-side column move (header drag or
    // streaming bubble) must keep `setSectionHidden` aligned with
    // `Column::visible`. Qt usually carries hidden flags through
    // `columnsMoved`, but bails on a zero-row source -- so
    // `OnSourceColumnsMoved` re-applies explicitly. Pairs with
    // `TestSourceColumnMoveRemapsRuntimeFilters` (filter remap).
    void TestSourceColumnMovePreservesHiddenColumn()
    {
        const int levelCol = StreamFixtureForColumnTests();
        QVERIFY2(levelCol >= 0, "level column must exist after streaming");
        auto *model = mWindow->Model();
        const int columnCount = model->columnCount();
        QVERIFY2(columnCount >= 2, "fixture must yield at least two columns");

        // Hide `level`; after the move it lands at `dest` and the
        // matching header section should still report hidden.
        mWindow->SetColumnVisible(levelCol, false);
        const QHeaderView *header = mWindow->findChild<LogTableView *>()->horizontalHeader();
        QVERIFY2(header != nullptr, "view must own a horizontal header");
        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): false positive; prior `QVERIFY2` aborts on null.
        QVERIFY2(header->isSectionHidden(levelCol), "precondition: section hidden before move");

        // `LogModel::MoveColumn` emits the same `columnsMoved`
        // payload the streaming bubble does, so the slot under test
        // runs the same code.
        const int src = levelCol;
        const int dest = (src == 0) ? columnCount - 1 : 0;
        QVERIFY(src != dest);
        QVERIFY2(model->MoveColumn(src, dest), "MoveColumn must succeed");
        QCoreApplication::processEvents();

        // The whole `Column` struct rotates with the move, so
        // `dest` is now the hidden index.
        QVERIFY2(
            !model->Layout().Columns()[static_cast<size_t>(dest)].visible,
            "Column::visible must follow the moved column to its new index"
        );
        QVERIFY2(
            model->Layout().Columns()[static_cast<size_t>(src)].visible,
            "the column displaced to `src` must remain visible"
        );

        // Header flags must agree with the configuration. Without
        // the `ApplyColumnVisibility()` inside `OnSourceColumnsMoved`,
        // this would only hold on the user-drag path.
        QVERIFY2(
            header->isSectionHidden(dest), "header section at the moved column's new logical index must remain hidden"
        );
        QVERIFY2(!header->isSectionHidden(src), "header section at the displaced column's index must not be hidden");
    }

    // Pin the wire format: `Save` output must contain `visible`
    // for both visible and hidden columns. A future Glaze meta
    // change that hides the field would surface here.
    void TestColumnVisibleFieldEmittedInSavedJson()
    {
        const int levelCol = StreamFixtureForColumnTests();
        QVERIFY2(levelCol >= 0, "level column must exist after streaming");
        auto *model = mWindow->Model();
        mWindow->SetColumnVisible(levelCol, false);

        const QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        const QString path = tempDir.filePath(QStringLiteral("with-visible.json"));
        SaveLayoutForTest(*model, path.toStdString());

        QFile file(path);
        QVERIFY(file.open(QFile::ReadOnly));
        const QByteArray contents = file.readAll();
        QVERIFY2(contents.contains("\"visible\": true"), "JSON must contain \"visible\": true");
        QVERIFY2(contents.contains("\"visible\": false"), "JSON must contain \"visible\": false");
    }

    // Regression: `Reset` emits `modelReset`, which clears every
    // `setSectionHidden` flag. The configuration's `Column::visible`
    // survives, and the wired `modelReset -> ApplyColumnVisibility`
    // pushes the flags back. Without it, a hidden column would be
    // silently re-shown after every reset.
    void TestVisibilityReappliedAfterModelReset()
    {
        const int levelCol = StreamFixtureForColumnTests();
        QVERIFY2(levelCol >= 0, "level column must exist after streaming");
        auto *model = mWindow->Model();

        mWindow->SetColumnVisible(levelCol, false);
        const QHeaderView *header = mWindow->findChild<LogTableView *>()->horizontalHeader();
        QVERIFY2(header != nullptr, "view must own a horizontal header");
        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): false positive; prior `QVERIFY2` aborts on null.
        QVERIFY2(header->isSectionHidden(levelCol), "precondition: section hidden");

        const QSignalSpy resetSpy(model, &QAbstractItemModel::modelReset);
        QVERIFY(resetSpy.isValid());
        model->Reset();
        QCoreApplication::processEvents();
        QVERIFY2(resetSpy.count() >= 1, "Reset must emit modelReset");

        QVERIFY2(
            !model->Layout().Columns().empty(),
            "configuration columns must survive Reset (Reset wipes data, not configuration)"
        );
        QVERIFY2(
            !model->Layout().Columns()[static_cast<size_t>(levelCol)].visible, "Column::visible flag must survive Reset"
        );
        QVERIFY2(header->isSectionHidden(levelCol), "modelReset hook must reapply Column::visible to the header");
    }

    // The `View` menu rebuilds on every `aboutToShow` and gives
    // each column a checkable action that flips `Column::visible`.
    // It stays reachable when every header section is hidden.
    void TestViewMenuTogglesColumn()
    {
        const int levelCol = StreamFixtureForColumnTests();
        QVERIFY2(levelCol >= 0, "level column must exist after streaming");
        auto *model = mWindow->Model();

        auto *viewMenu = mWindow->findChild<QMenu *>(QStringLiteral("menuView"));
        QVERIFY2(viewMenu != nullptr, "View menu must exist");

        emit viewMenu->aboutToShow();

        const QString levelHeader =
            QString::fromStdString(model->Layout().Columns()[static_cast<size_t>(levelCol)].header);
        QAction *levelAction = nullptr;
        for (QAction *act : viewMenu->actions())
        {
            if (act->text() == levelHeader)
            {
                levelAction = act;
                break;
            }
        }
        QVERIFY2(levelAction != nullptr, "View menu must contain an action for the level column");
        // NOLINTBEGIN(clang-analyzer-core.CallAndMessage): false positive; prior `QVERIFY2` aborts on null.
        QVERIFY2(levelAction->isCheckable(), "View menu action must be checkable");
        QVERIFY2(levelAction->isChecked(), "View menu action must reflect the visible state");

        levelAction->trigger();
        // NOLINTEND(clang-analyzer-core.CallAndMessage)
        QCoreApplication::processEvents();

        QVERIFY2(
            !model->Layout().Columns()[static_cast<size_t>(levelCol)].visible,
            "toggling the View menu action must flip Column::visible"
        );
        const QHeaderView *header = mWindow->findChild<LogTableView *>()->horizontalHeader();
        QVERIFY2(header != nullptr, "view must own a horizontal header");
        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): false positive; prior `QVERIFY2` aborts on null.
        QVERIFY2(header->isSectionHidden(levelCol), "header section must follow the toggle");

        // Toggle back; rebuild the menu from scratch so the action
        // state reflects the new configuration.
        emit viewMenu->aboutToShow();
        QAction *levelActionAfter = nullptr;
        for (QAction *act : viewMenu->actions())
        {
            if (act->text() == levelHeader)
            {
                levelActionAfter = act;
                break;
            }
        }
        QVERIFY(levelActionAfter != nullptr);
        QVERIFY2(!levelActionAfter->isChecked(), "rebuilt action must reflect newly-hidden state");
        levelActionAfter->trigger();
        QCoreApplication::processEvents();

        QVERIFY2(
            model->Layout().Columns()[static_cast<size_t>(levelCol)].visible,
            "second toggle must restore Column::visible"
        );
        QVERIFY2(!header->isSectionHidden(levelCol), "second toggle must un-hide the header section");
    }

    // Regression: clicking Edit after the filter's column was
    // reordered must open the editor against the column's *current*
    // index. Before the fix the lambda captured the filter by value,
    // freezing `row` at menu-build time; after a reorder, Edit
    // would mis-target the wrong column and `AddFilter`'s type-match
    // guard would silently drop the filter.
    void TestEditFilterAfterColumnReorderUsesCurrentRow()
    {
        const int levelCol = StreamFixtureForColumnTests();
        QVERIFY2(levelCol >= 0, "level column must exist after streaming");

        auto *model = mWindow->Model();
        const int columnCount = model->columnCount();
        QVERIFY2(columnCount >= 2, "fixture must yield at least two columns");

        // Install an enum filter on `level` via the production
        // submit slot (mirrors what `FilterEditor` emits on OK).
        const QString filterId = QStringLiteral("edit-after-move");
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

        const std::string filterKey = filterId.toStdString();
        QVERIFY2(mWindow->Filters().contains(filterKey), "filter must land in mSimpleLeaves after submit");
        const std::vector<std::string> levelKeysAtSubmit =
            model->Layout().Columns()[static_cast<size_t>(levelCol)].keys;
        QCOMPARE(mWindow->Filters().at(filterKey).columnKeys, levelKeysAtSubmit);

        // Reorder `level` via the production header-drag slot; the
        // source column shifts, but the leaf's `columnKeys` stay
        // the same (key-based binding).
        const int src = levelCol;
        const int dest = (src == 0) ? columnCount - 1 : 0;
        QVERIFY(src != dest);
        QVERIFY2(
            QMetaObject::invokeMethod(
                mWindow,
                "OnHeaderSectionMoved",
                Qt::DirectConnection,
                Q_ARG(int, src),
                Q_ARG(int, src),
                Q_ARG(int, dest)
            ),
            "OnHeaderSectionMoved slot must be invocable via meta-object"
        );
        QCoreApplication::processEvents();

        const int levelColAfter = ColumnByHeader(*model, QStringLiteral("category"));
        QCOMPARE(levelColAfter, dest);
        QVERIFY2(mWindow->Filters().contains(filterKey), "filter must survive the reorder");
        QCOMPARE(mWindow->Filters().at(filterKey).columnKeys, levelKeysAtSubmit);

        const auto *filtersMenu = mWindow->findChild<QMenu *>(QStringLiteral("menuFilters"));
        QVERIFY2(filtersMenu != nullptr, "MainWindow must expose its Filters menu");
        const QAction *filterMenuAction = nullptr;
        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): false positive; prior `QVERIFY2` aborts on null.
        for (const QAction *action : filtersMenu->actions())
        {
            if (action->data().toString() == filterId)
            {
                filterMenuAction = action;
                break;
            }
        }
        QVERIFY2(filterMenuAction != nullptr, "active filter must have a Filters-menu entry");

        const auto *filterSubMenu = filtersMenu->findChild<QMenu *>(filterId);
        QVERIFY2(filterSubMenu != nullptr, "filter menu entry must own an Edit/Remove sub-menu");
        QAction *editAction = nullptr;
        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): false positive; prior `QVERIFY2` aborts on null.
        for (QAction *action : filterSubMenu->actions())
        {
            if (action->text() == QStringLiteral("Edit"))
            {
                editAction = action;
                break;
            }
        }
        QVERIFY2(editAction != nullptr, "filter sub-menu must contain an Edit action");

        // Triggering Edit opens a `FilterEditor`. In the buggy build
        // the stale lambda would type-mismatch against `columns[src]`
        // and silently drop the filter from `mFilters`.
        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): false positive; prior `QVERIFY2` aborts on null.
        editAction->trigger();
        QCoreApplication::processEvents();

        QVERIFY2(
            mWindow->Filters().contains(filterKey),
            "Edit must not drop the filter; the lambda must look up the live mSimpleLeaves[id]"
        );
        QCOMPARE(mWindow->Filters().at(filterKey).columnKeys, levelKeysAtSubmit);

        // The opened `FilterEditor` should be pointing at `dest`,
        // not the stale `src`.
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
        QCOMPARE(editor->GetRowToFilter(), dest);

        editor->close();
        editor->deleteLater();
        QCoreApplication::processEvents();
    }

    // Regression: the query grammar types `category:info` as a
    // `String` / `Contains` leaf whatever the column's type is, and
    // that matches correctly at runtime because
    // `CallbackStringRowPredicate` compares the *formatted* value.
    // But `ApplyAdvancedFilterResult` used to extract every top-level
    // Leaf into `mSimpleLeaves`, and `ValidateFilterAgainstColumns`
    // rejects a String leaf on an Enumeration column (the simple
    // editor is type-driven and cannot load one). The clause worked
    // for the rest of the session and was then dropped on the next
    // load with a "column type changed" notice -- the query silently
    // lost a term between sessions. Leaves the simple editor can't
    // represent must stay in the Advanced subtree instead.
    void TestAdvancedStringLeafOnEnumColumnSurvivesReload()
    {
        const int levelCol = StreamFixtureForColumnTests();
        QVERIFY2(levelCol >= 0, "level column must exist after streaming");
        auto *model = mWindow->Model();
        const int msgCol = ColumnByHeader(*model, QStringLiteral("msg"));
        QVERIFY2(msgCol >= 0, "msg column must exist after streaming");

        const auto &columns = model->Layout().Columns();
        const auto levelType = columns[static_cast<size_t>(levelCol)].type;
        QVERIFY2(
            levelType == loglib::ColumnType::Enumeration || levelType == loglib::ColumnType::Level,
            "precondition: streaming must have promoted the level column to an enum-like type"
        );
        const std::vector<std::string> levelKeys = columns[static_cast<size_t>(levelCol)].keys;
        const std::vector<std::string> msgKeys = columns[static_cast<size_t>(msgCol)].keys;

        // What `category:info AND msg:m1` parses to: two String
        // leaves, but only the one on the String column is
        // representable in simple mode.
        std::vector<loglib::FilterExpression> andChildren;
        andChildren.push_back(
            loglib::MakeLeaf(
                loglib::LeafRule{
                    .type = loglib::LeafRule::Type::String,
                    .columnKeys = levelKeys,
                    .matchType = loglib::LeafRule::Match::Contains,
                    .filterString = std::string("info"),
                }
            )
        );
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
        mWindow->CommitAdvancedFilterForTest(loglib::MakeAnd(std::move(andChildren)));
        QCoreApplication::processEvents();

        // Only the `msg` leaf reaches the simple surface.
        QCOMPARE(static_cast<int>(mWindow->Filters().size()), 1);
        QCOMPARE(mWindow->Filters().begin()->second.columnKeys, msgKeys);

        const auto findEnumStringLeaf = [&levelKeys](const loglib::FilterExpression &expression) {
            const auto leaves = AllLeavesOf(expression);
            return std::ranges::any_of(leaves, [&levelKeys](const loglib::LeafRule &rule) {
                return rule.type == loglib::LeafRule::Type::String && rule.columnKeys == levelKeys &&
                       rule.filterString.has_value() && *rule.filterString == "info";
            });
        };
        QVERIFY2(
            findEnumStringLeaf(mWindow->activeSession()->View().expression),
            "the un-representable leaf must stay on the wire expression"
        );

        const QTemporaryDir savedDir;
        QVERIFY(savedDir.isValid());
        const QString savedPath = savedDir.filePath(QStringLiteral("string-on-enum.json"));
        mWindow->SaveConfigurationToPathForTest(savedPath);

        mWindow->activeSession()->MutableView().expression = loglib::FilterExpression{};
        QCoreApplication::processEvents();

        mWindow->SetSuppressDialogsForTest(true);
        mWindow->LoadConfigurationFromPathForTest(savedPath);
        QCoreApplication::processEvents();

        QCOMPARE(mWindow->LastDroppedFilterCountForTest(), 0);
        QVERIFY2(
            findEnumStringLeaf(mWindow->activeSession()->View().expression),
            "a String leaf on an enum column must survive Save/Load instead of being dropped"
        );
        // The representable sibling still round-trips into simple mode.
        QCOMPARE(static_cast<int>(mWindow->Filters().size()), 1);
        QCOMPARE(mWindow->Filters().begin()->second.columnKeys, msgKeys);
    }

    // Regression: the query parser records exactly one column key
    // per leaf (the identifier the user typed), but real columns
    // often carry an alias vector -- e.g. a `svc` column configured
    // as `keys = {"svc", "service", "svcname"}` so any of those
    // JSON fields feed the same logical column. Simple-mode leaves
    // built via the FilterEditor bind the *full* alias vector so
    // the rule survives future edits (rename, alias drop). Before
    // the fix, `ApplyAdvancedFilterResult` extracted the parser's
    // single-key binding verbatim into `mSimpleLeaves`, silently
    // narrowing the rule to the typed alias only. Now the
    // extractor promotes each leaf's `columnKeys` to the resolved
    // column's canonical vector, matching what the FilterEditor
    // would install.
    void TestAdvancedFilterExtractionPromotesToFullColumnKeys()
    {
        // Seed a config with a single multi-key `svc` column so
        // the promotion is observable. Loading through
        // `TryLoadAsConfigurationForTest` mirrors the on-disk load
        // path used by the production entry point.
        loglib::LogConfiguration cfg;
        loglib::Column svcCol;
        svcCol.header = "svc";
        svcCol.keys = {"svc", "service", "svcname"};
        svcCol.printFormat = "{}";
        svcCol.type = loglib::ColumnType::String;
        svcCol.parseFormats = {};
        cfg.columns.push_back(svcCol);

        const QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        const QString cfgPath = tempDir.filePath(QStringLiteral("multi-key.json"));
        loglib::SaveConfiguration(cfg, cfgPath.toStdString(), loglib::SaveScope::Full);
        QVERIFY2(mWindow->TryLoadAsConfigurationForTest(cfgPath), "TryLoadAsConfiguration must succeed");
        QCoreApplication::processEvents();

        auto *model = mWindow->Model();
        QVERIFY2(model != nullptr, "MainWindow must own a LogModel");
        const int svcColIndex = ColumnByHeader(*model, QStringLiteral("svc"));
        QVERIFY2(svcColIndex >= 0, "svc column must exist after configuration load");
        const auto &svcColumn = model->Layout().Columns()[static_cast<size_t>(svcColIndex)];
        QCOMPARE(static_cast<int>(svcColumn.keys.size()), 3);

        // Parse `svc:auth` -- ParseQuery emits a single-key
        // binding matching the identifier the user typed.
        auto parsed = loglib::ParseQuery("svc:auth", loglib::ProcessDefaultTimeZone());
        QVERIFY2(parsed.has_value(), "ParseQuery must accept a well-formed simple leaf");
        const auto *parsedLeaf = std::get_if<loglib::FilterExpression::Leaf>(&parsed->node);
        QVERIFY2(parsedLeaf != nullptr, "parsed root must be a Leaf");
        QCOMPARE(static_cast<int>(parsedLeaf->rule.columnKeys.size()), 1);
        QCOMPARE(QString::fromStdString(parsedLeaf->rule.columnKeys.front()), QStringLiteral("svc"));

        mWindow->CommitAdvancedFilterForTest(std::move(*parsed));
        QCoreApplication::processEvents();

        // One extracted simple leaf; its `columnKeys` now covers
        // the full alias vector, not just the typed alias.
        QCOMPARE(static_cast<int>(mWindow->Filters().size()), 1);
        const auto &extracted = mWindow->Filters().begin()->second;
        QCOMPARE(static_cast<int>(extracted.columnKeys.size()), 3);
        QCOMPARE(QString::fromStdString(extracted.columnKeys[0]), QStringLiteral("svc"));
        QCOMPARE(QString::fromStdString(extracted.columnKeys[1]), QStringLiteral("service"));
        QCOMPARE(QString::fromStdString(extracted.columnKeys[2]), QStringLiteral("svcname"));

        // Mirror consistency: the persisted expression carries
        // the same promoted key set, so a follow-up load / mirror
        // won't silently narrow the rule.
        const auto &mirrored = mWindow->activeSession()->View().expression;
        const auto *mirroredAnd = std::get_if<loglib::FilterExpression::And>(&mirrored.node);
        QVERIFY2(mirroredAnd != nullptr, "mirror wraps a single leaf in And so it survives future mirrors");
        QCOMPARE(static_cast<int>(mirroredAnd->children.size()), 1);
        const auto *mirroredLeaf = std::get_if<loglib::FilterExpression::Leaf>(&mirroredAnd->children.front().node);
        QVERIFY2(mirroredLeaf != nullptr, "mirrored child must be the promoted Leaf");
        QCOMPARE(static_cast<int>(mirroredLeaf->rule.columnKeys.size()), 3);
    }

    // Pinning a string-only column (`msg`) to `Integer` is the
    // canonical "user misconfiguration" case: every present value is
    // a string, no value is an integer, so `ColumnTypeHealth` reports
    // presentSlots > matchingSlots and the header should advertise a
    // mismatch via tooltip + decoration role.
    void TestColumnHealthFlagsMismatchedType()
    {
        const int levelCol = StreamFixtureForColumnTests();
        QVERIFY2(levelCol >= 0, "level column must exist after streaming");
        auto *model = mWindow->Model();
        const int msgCol = ColumnByHeader(*model, QStringLiteral("msg"));
        QVERIFY2(msgCol >= 0, "msg column must exist after streaming");

        // Streaming auto-detected `msg` to `String`; pin it to
        // `Integer` to force a mismatch and re-snapshot health.
        model->MutableLayout().SetColumnAutoDetect(static_cast<size_t>(msgCol), false);
        model->MutableLayout().SetColumnType(static_cast<size_t>(msgCol), loglib::ColumnType::Integer);

        const QSignalSpy healthSpy(model, &LogModel::columnHealthChanged);
        model->RefreshColumnHealth();
        // RefreshColumnHealth emits when health moves; the pin
        // shifts matching from N to 0, which is always a delta.
        QVERIFY2(healthSpy.count() >= 1, "RefreshColumnHealth must emit columnHealthChanged on a mismatch flip");

        const auto health = model->ColumnHealth(msgCol);
        QVERIFY2(health.has_value(), "ColumnHealth must yield a snapshot after Refresh");
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access): asserted above.
        QVERIFY2(health->presentSlots > 0, "msg column must have data after streaming");
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        QVERIFY2(health->matchingSlots < health->presentSlots, "Integer-pinned string column must report mismatches");

        // Tooltip carries the human-readable diagnostics; decoration
        // returns the standard warning icon. Both are wired through
        // the same path so a downstream UI test exercising header
        // rendering sees a consistent surface.
        const QVariant tooltip = model->headerData(msgCol, Qt::Horizontal, Qt::ToolTipRole);
        QVERIFY(tooltip.canConvert<QString>());
        QVERIFY2(
            tooltip.toString().contains(QStringLiteral("do not match the configured type")),
            "Mismatched column tooltip must explain the diagnostic"
        );

        const QVariant decoration = model->headerData(msgCol, Qt::Horizontal, Qt::DecorationRole);
        QVERIFY(decoration.canConvert<QIcon>());
        QVERIFY2(!decoration.value<QIcon>().isNull(), "Mismatched column must surface a warning icon");

        // Auto-detected columns (Type::Any + autoDetect=true) never
        // mismatch by definition. Promoted enum columns (level) carry
        // dictionary slots so their matching count equals presentSlots.
        const auto levelHealth = model->ColumnHealth(levelCol);
        QVERIFY(levelHealth.has_value());
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        QCOMPARE(levelHealth->matchingSlots, levelHealth->presentSlots);
    }

    // Renaming a column refreshes the tooltip header but keeps
    // the funnel and "Filters:" section. Pins that
    // `mColumnFilterDetails` is keyed by section, not by label.
    void TestHeaderFilterIndicatorSurvivesColumnRename()
    {
        const int levelCol = StreamFixtureForColumnTests();
        QVERIFY2(levelCol >= 0, "level column must exist after streaming");
        auto *model = mWindow->Model();
        const int msgCol = ColumnByHeader(*model, QStringLiteral("msg"));
        QVERIFY2(msgCol >= 0, "msg column must exist after streaming");

        const QString needle = QStringLiteral("m9");
        QMetaObject::invokeMethod(
            mWindow,
            "FilterSubmitted",
            Qt::DirectConnection,
            Q_ARG(QString, QStringLiteral("hf-rename")),
            Q_ARG(int, msgCol),
            Q_ARG(QString, needle),
            Q_ARG(int, static_cast<int>(loglib::LeafRule::Match::Contains))
        );
        QCoreApplication::processEvents();
        QVERIFY2(model->HasFilterForColumn(msgCol), "precondition: filter recorded on msg column");

        model->MutableLayout().SetColumnHeader(static_cast<size_t>(msgCol), std::string("message"));
        model->NotifyColumnEdited(msgCol);
        QCoreApplication::processEvents();

        QVERIFY2(
            model->HasFilterForColumn(msgCol),
            "rename must not clear the per-column filter cache (keyed by section, not label)"
        );

        const QVariant decoration = model->headerData(msgCol, Qt::Horizontal, Qt::DecorationRole);
        QVERIFY(decoration.canConvert<QIcon>());
        QVERIFY2(!decoration.value<QIcon>().isNull(), "funnel decoration must survive a column rename");

        const QString tooltip = model->headerData(msgCol, Qt::Horizontal, Qt::ToolTipRole).toString();
        QVERIFY2(tooltip.contains(QStringLiteral("message")), "tooltip header line must reflect the renamed label");
        QVERIFY2(tooltip.contains(QStringLiteral("Filters:")), "tooltip must keep its Filters section after rename");
        QVERIFY2(tooltip.contains(needle), "Filters section must still list the active filter title after rename");
    }

    // Hiding then re-showing a filtered column keeps the funnel
    // attached. Visibility doesn't change `filter.row`, so the
    // sync calls in `SetColumnVisible` / `ApplyColumnVisibility`
    // are no-ops -- but the symmetric wiring is what we pin.
    void TestHeaderFilterIndicatorSurvivesColumnHideShow()
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
            Q_ARG(QString, QStringLiteral("hf-visibility")),
            Q_ARG(int, msgCol),
            Q_ARG(QString, QStringLiteral("m4")),
            Q_ARG(int, static_cast<int>(loglib::LeafRule::Match::Contains))
        );
        QCoreApplication::processEvents();
        QVERIFY2(model->HasFilterForColumn(msgCol), "precondition: filter installed on msg column");

        mWindow->SetColumnVisible(msgCol, false);
        QCoreApplication::processEvents();
        // Hiding is a header-view concern; the filter cache stays.
        QVERIFY2(model->HasFilterForColumn(msgCol), "hiding a column must not drop the per-column filter cache entry");

        mWindow->SetColumnVisible(msgCol, true);
        QCoreApplication::processEvents();
        QVERIFY2(model->HasFilterForColumn(msgCol), "re-showing the column must surface the same cache entry");

        const QVariant decoration = model->headerData(msgCol, Qt::Horizontal, Qt::DecorationRole);
        QVERIFY(decoration.canConvert<QIcon>());
        QVERIFY2(
            !decoration.value<QIcon>().isNull(),
            "funnel decoration must reappear with the column on a hide -> show round trip"
        );
    }

    // After a mid-batch `Level -> String` demote, the "Filters:"
    // section must list the rewritten raw bytes (`info`) rather
    // than the canonical name (`Info`) the user originally
    // submitted. Pins the resync inside the demote handler.
    void TestHeaderFilterTooltipReflectsRawValuesAfterLevelDemote()
    {
        auto *model = mWindow->findChild<LogModel *>();
        QVERIFY2(model != nullptr, "MainWindow must own a LogModel");

        const TempJsonFile emptyFixture(QStringList{});
        auto fileSource = std::make_unique<loglib::FileLineSource>(emptyFixture.Path().toStdString());
        loglib::FileLineSource *sourcePtr = fileSource.get();
        (void)model->BeginStreamingForSyncTest(std::move(fileSource));

        // Cap of 3: batch 1 promotes to Level (dict size 2);
        // batch 2 trips the cap and demotes back to String.
        constexpr uint16_t TEST_CAP = 3;
        model->Table().SetEnumValueCap(TEST_CAP);

        loglib::KeyIndex &keys = model->Table().Keys();
        const auto makeLine = [&](const std::string &value) {
            std::vector<std::pair<loglib::KeyId, loglib::LogValue>> values;
            values.emplace_back(keys.GetOrInsert("level"), loglib::LogValue(value));
            return loglib::LogLine{std::move(values), *sourcePtr, 0};
        };

        {
            loglib::RowBatch batch;
            batch.firstLineNumber = 1;
            batch.newKeys.emplace_back("level");
            batch.lines.push_back(makeLine("info"));
            batch.lines.push_back(makeLine("info"));
            batch.lines.push_back(makeLine("warn"));
            model->AppendBatch(std::move(batch));
        }

        const int levelCol = ColumnByHeader(*model, QStringLiteral("level"));
        QVERIFY2(levelCol >= 0, "level column must exist after promotion");
        QCOMPARE(model->Layout().Columns()[static_cast<size_t>(levelCol)].type, loglib::ColumnType::Level);

        // Submit `Info` while the column is still Level; the
        // pre-demote tooltip lists the canonical name.
        const QString filterId = QStringLiteral("hf-level-demote");
        QVERIFY2(
            QMetaObject::invokeMethod(
                mWindow,
                "FilterEnumSubmitted",
                Qt::DirectConnection,
                Q_ARG(QString, filterId),
                Q_ARG(int, levelCol),
                Q_ARG(QStringList, QStringList{QStringLiteral("Info")})
            ),
            "FilterEnumSubmitted slot must be invocable via meta-object"
        );
        QCoreApplication::processEvents();

        const QString tooltipBefore = model->headerData(levelCol, Qt::Horizontal, Qt::ToolTipRole).toString();
        QVERIFY2(
            tooltipBefore.contains(QStringLiteral("Info")),
            "tooltip pre-demote must list the canonical Level name the user submitted"
        );

        // Batch 2 overflows the cap and demotes `level` to String.
        // The `Demoted` handler rewrites `["Info"] -> ["info"]`
        // and resyncs the tooltip cache.
        {
            loglib::RowBatch batch;
            batch.firstLineNumber = 4;
            batch.lines.push_back(makeLine("error"));
            batch.lines.push_back(makeLine("debug"));
            batch.lines.push_back(makeLine("trace"));
            model->AppendBatch(std::move(batch));
        }
        QCoreApplication::processEvents();

        QCOMPARE(model->Layout().Columns()[static_cast<size_t>(levelCol)].type, loglib::ColumnType::String);

        const QString tooltipAfter = model->headerData(levelCol, Qt::Horizontal, Qt::ToolTipRole).toString();
        QVERIFY2(
            tooltipAfter.contains(QStringLiteral("Filters:")), "tooltip post-demote must keep its Filters section"
        );
        QVERIFY2(
            tooltipAfter.contains(QStringLiteral("info")), "tooltip post-demote must list the rewritten raw value"
        );
        // The canonical-name bullet `&bull; Info` must be gone.
        // We can't just `!contains("Info")` because raw `info`
        // is a substring of `Info`, so we look for the bullet.
        QVERIFY2(
            !tooltipAfter.contains(QStringLiteral("&bull; Info")),
            "tooltip post-demote must not still list the pre-demote canonical name"
        );

        model->EndStreaming(false);
    }

    // The other tests only cover `BuildFilterTitle`'s String
    // branch. Pin the Enumeration and numeric-range branches so
    // their tooltip rendering stays correct.
    void TestHeaderFilterTooltipForEnumAndNumericFilters()
    {
        const int levelCol = StreamFixtureForColumnTests();
        QVERIFY2(levelCol >= 0, "category column must exist after streaming");
        auto *model = mWindow->Model();
        QCOMPARE(model->Layout().Columns()[static_cast<size_t>(levelCol)].type, loglib::ColumnType::Enumeration);

        // Enum branch: assert each selected value is mentioned;
        // the exact separator is an implementation detail. The
        // local lifts the `{a, b}` list out of the `Q_ARG` macro,
        // which would mis-parse the embedded comma.
        const QStringList enumValues{QStringLiteral("info"), QStringLiteral("warn")};
        QVERIFY2(
            QMetaObject::invokeMethod(
                mWindow,
                "FilterEnumSubmitted",
                Qt::DirectConnection,
                Q_ARG(QString, QStringLiteral("hf-enum")),
                Q_ARG(int, levelCol),
                Q_ARG(QStringList, enumValues)
            ),
            "FilterEnumSubmitted slot must be invocable via meta-object"
        );
        QCoreApplication::processEvents();

        const QString enumTooltip = model->headerData(levelCol, Qt::Horizontal, Qt::ToolTipRole).toString();
        QVERIFY2(enumTooltip.contains(QStringLiteral("Filters:")), "enum filter must show in the Filters section");
        QVERIFY2(
            enumTooltip.contains(QStringLiteral("info")) && enumTooltip.contains(QStringLiteral("warn")),
            "enum tooltip must list every selected value"
        );

        // Wipe the enum filter so the numeric branch starts clean
        // and the two filter bullets don't collide.
        QMetaObject::invokeMethod(mWindow, "ClearAllFilters", Qt::DirectConnection);
        QCoreApplication::processEvents();
        QVERIFY2(mWindow->Filters().empty(), "ClearAllFilters must drain mFilters between branches");

        // Numeric branch: `BuildFilterTitle` formats from
        // `LogFilter::Type`, not the column's declared type, so a
        // numeric filter on the (string) `msg` column still
        // exercises this branch.
        const int msgCol = ColumnByHeader(*model, QStringLiteral("msg"));
        QVERIFY2(msgCol >= 0, "msg column must exist after streaming");
        QVERIFY2(
            QMetaObject::invokeMethod(
                mWindow,
                "FilterNumericRangeSubmitted",
                Qt::DirectConnection,
                Q_ARG(QString, QStringLiteral("hf-numeric")),
                Q_ARG(int, msgCol),
                Q_ARG(std::optional<double>, std::optional<double>{1.5}),
                Q_ARG(std::optional<double>, std::optional<double>{42.0})
            ),
            "FilterNumericRangeSubmitted slot must be invocable via meta-object"
        );
        QCoreApplication::processEvents();

        const QString numericTooltip = model->headerData(msgCol, Qt::Horizontal, Qt::ToolTipRole).toString();
        QVERIFY2(
            numericTooltip.contains(QStringLiteral("Filters:")), "numeric filter must show in the Filters section"
        );
        // Both bounds must appear; the separator (`>=`, `<=`,
        // dash) is an implementation detail.
        QVERIFY2(
            numericTooltip.contains(QStringLiteral("1.5")) && numericTooltip.contains(QStringLiteral("42")),
            "numeric tooltip must list both bounds"
        );
    }

    // Status-bar diagnostics button + dialog summary are driven by
    // `ConfigurationDiagnosticsDialog::MismatchedColumnCount`, so they
    // both flip together. Verify the aggregate stays in lockstep with
    // the per-column health (no double-counting, no stale reads).
    void TestDiagnosticsButtonSurfacesMismatchCount()
    {
        const int levelCol = StreamFixtureForColumnTests();
        QVERIFY2(levelCol >= 0, "level column must exist after streaming");
        auto *model = mWindow->Model();
        const int msgCol = ColumnByHeader(*model, QStringLiteral("msg"));
        QVERIFY2(msgCol >= 0, "msg column must exist after streaming");

        // Steady state after streamingFinished: every column either
        // auto-detected to a matching type or stayed Any; zero
        // mismatches.
        QCOMPARE(ConfigurationDiagnosticsDialog::MismatchedColumnCount(*model), 0);
        auto *button = mWindow->findChild<QPushButton *>(QStringLiteral("diagnosticsButton"));
        QVERIFY2(button != nullptr, "MainWindow must own the diagnostics status-bar button");
        // Offscreen-QPA leaves the parent window hidden, which would
        // collapse `isVisible` to false regardless of the slot's
        // intent; check `isHidden` so we exercise only the state the
        // slot actually drives.
        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): prior QVERIFY2 aborts on null.
        QVERIFY2(button->isHidden(), "Diagnostics button must be hidden when no mismatches are present");

        // Force a mismatch by pinning `msg` to Integer.
        model->MutableLayout().SetColumnAutoDetect(static_cast<size_t>(msgCol), false);
        model->MutableLayout().SetColumnType(static_cast<size_t>(msgCol), loglib::ColumnType::Integer);
        model->RefreshColumnHealth();
        QCoreApplication::processEvents();

        QCOMPARE(ConfigurationDiagnosticsDialog::MismatchedColumnCount(*model), 1);
        // `isVisible` is gated by the parent window's visibility,
        // which the offscreen-QPA test harness keeps hidden; check
        // the inverse (`isHidden`) instead so the assertion exercises
        // the only state the slot controls.
        QVERIFY2(!button->isHidden(), "Diagnostics button must un-hide when a column mismatches");
        QVERIFY2(button->text().contains(QStringLiteral("1")), "Button label must surface the mismatch count");
    }

    // The diagnostics dialog walks the same per-column snapshot as
    // the status-bar aggregate; opening it must list exactly one
    // mismatched row (matching the count surfaced by the button).
    void TestDiagnosticsDialogListsMismatchedColumns()
    {
        const int levelCol = StreamFixtureForColumnTests();
        QVERIFY2(levelCol >= 0, "level column must exist after streaming");
        auto *model = mWindow->Model();
        const int msgCol = ColumnByHeader(*model, QStringLiteral("msg"));
        QVERIFY2(msgCol >= 0, "msg column must exist after streaming");

        model->MutableLayout().SetColumnAutoDetect(static_cast<size_t>(msgCol), false);
        model->MutableLayout().SetColumnType(static_cast<size_t>(msgCol), loglib::ColumnType::Integer);
        model->RefreshColumnHealth();

        const ConfigurationDiagnosticsDialog dialog(model);
        const auto *table = dialog.findChild<QTableWidget *>();
        QVERIFY2(table != nullptr, "Dialog must own a diagnosticsTable widget");
        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): prior QVERIFY2 aborts on null.
        QCOMPARE(table->rowCount(), static_cast<int>(model->Layout().Columns().size()));

        // Find the mismatched row by header text and assert the
        // counter columns. Sorting is enabled, so look up by item
        // rather than fixed row index.
        int matchedRowIndex = -1;
        for (int row = 0; row < table->rowCount(); ++row)
        {
            const QTableWidgetItem *headerItem = table->item(row, 0);
            if (headerItem != nullptr && headerItem->text() == QStringLiteral("msg"))
            {
                matchedRowIndex = row;
                break;
            }
        }
        QVERIFY2(matchedRowIndex >= 0, "Dialog must include a row for the `msg` column");

        // Column 6 is the "Mismatched" counter (see the header layout
        // in `ConfigurationDiagnosticsDialog`'s constructor).
        constexpr int MISMATCHED_COLUMN = 6;
        const QTableWidgetItem *mismatchedItem = table->item(matchedRowIndex, MISMATCHED_COLUMN);
        QVERIFY(mismatchedItem != nullptr);
        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): prior QVERIFY aborts on null.
        QVERIFY2(mismatchedItem->text().toInt() > 0, "Dialog must report a non-zero mismatch count for `msg`");

        // Regression: the mismatched row must pin both halves of the
        // contrast pair; bg alone leaves fg on the palette default,
        // which is near-white on a dark theme (row text invisible).
        for (int c = 0; c < table->columnCount(); ++c)
        {
            const QTableWidgetItem *item = table->item(matchedRowIndex, c);
            QVERIFY2(item != nullptr, qPrintable(QStringLiteral("mismatched row column %1 must have an item").arg(c)));
            // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): prior QVERIFY2 aborts on null.
            const QBrush bg = item->background();
            const QBrush fg = item->foreground();
            QVERIFY2(
                bg.style() != Qt::NoBrush,
                qPrintable(QStringLiteral("mismatched row column %1 must paint an explicit background").arg(c))
            );
            QVERIFY2(
                fg.style() != Qt::NoBrush,
                qPrintable(QStringLiteral(
                               "mismatched row column %1 must paint an explicit foreground "
                               "so it stays legible on dark themes"
                )
                               .arg(c))
            );
            // ITU-R BT.601 luma: foreground must be visibly darker
            // than the background, or we're back to the original bug.
            const QColor fgColor = fg.color();
            const QColor bgColor = bg.color();
            constexpr double R_LUMA = 0.299;
            constexpr double G_LUMA = 0.587;
            constexpr double B_LUMA = 0.114;
            const double fgLuma = (R_LUMA * fgColor.red()) + (G_LUMA * fgColor.green()) + (B_LUMA * fgColor.blue());
            const double bgLuma = (R_LUMA * bgColor.red()) + (G_LUMA * bgColor.green()) + (B_LUMA * bgColor.blue());
            constexpr double MIN_LUMA_GAP = 80.0;
            QVERIFY2(
                bgLuma - fgLuma >= MIN_LUMA_GAP,
                qPrintable(QStringLiteral(
                               "mismatched row column %1 must keep a high contrast between fg (%2) and bg (%3); "
                               "got fgLuma=%4, bgLuma=%5"
                )
                               .arg(c)
                               .arg(fgColor.name(), bgColor.name())
                               .arg(fgLuma)
                               .arg(bgLuma))
            );
        }
    }

    // Regression: the mismatched-row highlight must adapt to the
    // active palette. The original fix pinned a pale-pink bg + dark
    // fg which read as a glaringly bright row in Dark themes.
    // Flipping to Dark must hand the dialog a darker bg + lighter
    // fg while keeping the same luma gap.
    void TestDiagnosticsDialogHighlightAdaptsToDarkTheme()
    {
        QVERIFY(mTheme != nullptr);
        const int levelCol = StreamFixtureForColumnTests();
        QVERIFY2(levelCol >= 0, "level column must exist after streaming");
        auto *model = mWindow->Model();
        const int msgCol = ColumnByHeader(*model, QStringLiteral("msg"));
        QVERIFY2(msgCol >= 0, "msg column must exist after streaming");

        // Force `msg` into a mismatch so the dialog paints a warning row.
        model->MutableLayout().SetColumnAutoDetect(static_cast<size_t>(msgCol), false);
        model->MutableLayout().SetColumnType(static_cast<size_t>(msgCol), loglib::ColumnType::Integer);
        model->RefreshColumnHealth();

        // Snapshot the Light-theme brushes for the `msg` row.
        mTheme->SetActiveSelection(QStringLiteral("Light"));
        QCoreApplication::processEvents();

        const ConfigurationDiagnosticsDialog dialog(model);
        auto *table = dialog.findChild<QTableWidget *>();
        QVERIFY2(table != nullptr, "Dialog must own a diagnosticsTable widget");

        auto findMsgRow = [table]() {
            // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): false positive; prior `QVERIFY2` aborts on null.
            for (int row = 0; row < table->rowCount(); ++row)
            {
                const QTableWidgetItem *headerItem = table->item(row, 0);
                if (headerItem != nullptr && headerItem->text() == QStringLiteral("msg"))
                {
                    return row;
                }
            }
            return -1;
        };

        const int lightRow = findMsgRow();
        QVERIFY2(lightRow >= 0, "Light-theme dialog must include a row for the `msg` column");
        const QTableWidgetItem *lightItem = table->item(lightRow, 0);
        QVERIFY(lightItem != nullptr);
        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): prior QVERIFY aborts on null.
        const QColor lightBg = lightItem->background().color();
        const QColor lightFg = lightItem->foreground().color();

        // Flip to Dark. The palette change fires `ApplicationPaletteChange`
        // on the dialog; the override re-runs `Refresh()`.
        mTheme->SetActiveSelection(QStringLiteral("Dark"));
        QCoreApplication::processEvents();

        const int darkRow = findMsgRow();
        QVERIFY2(darkRow >= 0, "Dark-theme dialog must still include a row for the `msg` column");
        const QTableWidgetItem *darkItem = table->item(darkRow, 0);
        QVERIFY(darkItem != nullptr);
        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): prior QVERIFY aborts on null.
        const QColor darkBg = darkItem->background().color();
        const QColor darkFg = darkItem->foreground().color();

        QVERIFY2(
            lightBg != darkBg,
            qPrintable(QStringLiteral("Highlight bg must differ between Light and Dark; got light=%1, dark=%2")
                           .arg(lightBg.name(), darkBg.name()))
        );
        QVERIFY2(
            lightFg != darkFg,
            qPrintable(QStringLiteral("Highlight fg must differ between Light and Dark; got light=%1, dark=%2")
                           .arg(lightFg.name(), darkFg.name()))
        );

        constexpr double R_LUMA = 0.299;
        constexpr double G_LUMA = 0.587;
        constexpr double B_LUMA = 0.114;
        auto luma = [](const QColor &c) { return (R_LUMA * c.red()) + (G_LUMA * c.green()) + (B_LUMA * c.blue()); };

        // Dark-mode highlight bg must actually be dark; otherwise the
        // row punches through the dialog as a near-white slab.
        constexpr double DARK_BG_MAX_LUMA = 110.0;
        QVERIFY2(
            luma(darkBg) <= DARK_BG_MAX_LUMA,
            qPrintable(QStringLiteral("Dark-theme highlight bg must be dark; got %1 (luma=%2)")
                           .arg(darkBg.name())
                           .arg(luma(darkBg)))
        );

        // Dark mode reverses the pair: fg must be lighter than bg,
        // by the same gap as the light-mode highlight.
        constexpr double MIN_LUMA_GAP = 80.0;
        QVERIFY2(
            luma(darkFg) - luma(darkBg) >= MIN_LUMA_GAP,
            qPrintable(QStringLiteral(
                           "Dark-theme highlight must keep high contrast (fg lighter than bg); "
                           "got fg=%1 (luma=%2), bg=%3 (luma=%4)"
            )
                           .arg(darkFg.name())
                           .arg(luma(darkFg))
                           .arg(darkBg.name())
                           .arg(luma(darkBg)))
        );

        // Restore the default before the next test starts.
        mTheme->SetActiveSelection(QStringLiteral("Light"));
        QCoreApplication::processEvents();
    }

    // Live preview + rollback: toggling Preferences checkboxes
    // updates the model + theme immediately, and Cancel/X-close
    // rewinds to the pre-dialog state.
    void TestPreferencesEditorLevelIconsLivePreviewAndRevert()
    {
        QVERIFY(mTheme != nullptr);

        // Baseline showLevelIcons=true so toggling off is a real
        // change. Built-in themes ship `levelColumnOverride`.
        {
            QSettings settings;
            settings.setValue(QStringLiteral("ui/showLevelIcons"), true);
        }
        mTheme->SetActiveSelection(QStringLiteral("Dark"));
        QCoreApplication::processEvents();
        QVERIFY(mTheme->HasLevelColumnOverride());
        // Drive the model into icon mode so we can observe the
        // live-preview flip back to text mode.
        auto *model = mWindow->Model();
        QVERIFY(model != nullptr);
        model->SetShowLevelIcons(true);
        QVERIFY(model->IsLevelIconModeActive());

        auto *editor = mWindow->findChild<PreferencesEditor *>();
        QVERIFY2(editor != nullptr, "MainWindow must own a PreferencesEditor");
        // `UpdateFields` seeds `mInitial*` from QSettings.
        editor->UpdateFields();
        // Find the checkbox by its label rather than `objectName`
        // so the test doesn't need production-only metadata.
        QCheckBox *checkbox = nullptr;
        for (auto *cb : editor->findChildren<QCheckBox *>())
        {
            if (cb->text() == QStringLiteral("Show level icons"))
            {
                checkbox = cb;
                break;
            }
        }
        QVERIFY2(checkbox != nullptr, "Preferences must expose a `Show level icons` checkbox");
        // Explicit null-check for clang-analyzer (it doesn't model
        // QVERIFY2's early return).
        if (checkbox == nullptr)
        {
            return;
        }
        QVERIFY(checkbox->isChecked());

        // Live preview: untoggle -> icon mode flips off
        // synchronously via `MainWindow`'s handler.
        checkbox->setChecked(false);
        QCoreApplication::processEvents();
        QVERIFY2(!model->IsLevelIconModeActive(), "Untoggling the checkbox must live-preview icon mode OFF");

        // QSettings is untouched until Ok.
        {
            const QSettings settings;
            QVERIFY(settings.value(QStringLiteral("ui/showLevelIcons")).toBool());
        }

        // X-close triggers `closeEvent` revert.
        editor->close();
        QCoreApplication::processEvents();

        QVERIFY2(
            model->IsLevelIconModeActive(),
            "Closing the dialog without Ok must revert the level-icons live preview to the initial state"
        );
        {
            const QSettings settings;
            QVERIFY(settings.value(QStringLiteral("ui/showLevelIcons")).toBool());
        }
    }

    // Mirror of the level-icons test for the high-contrast toggle.
    // Observed on `ThemeControl::IsHighContrast` because the live
    // preview goes through `SetHighContrast`.
    void TestPreferencesEditorHighContrastLivePreviewAndRevert()
    {
        QVERIFY(mTheme != nullptr);

        // Baseline highContrast=false on a theme with a
        // `levelsHighContrast` block so the checkbox is enabled.
        {
            QSettings settings;
            settings.setValue(QStringLiteral("ui/highContrastLevels"), false);
        }
        mTheme->SetActiveSelection(QStringLiteral("Dark"));
        QCoreApplication::processEvents();
        QVERIFY(mTheme->HasLevelsHighContrast());
        mTheme->SetHighContrast(false);
        QVERIFY(!mTheme->IsHighContrast());

        auto *editor = mWindow->findChild<PreferencesEditor *>();
        QVERIFY2(editor != nullptr, "MainWindow must own a PreferencesEditor");
        editor->UpdateFields();

        QCheckBox *checkbox = nullptr;
        for (auto *cb : editor->findChildren<QCheckBox *>())
        {
            if (cb->text() == QStringLiteral("High contrast levels"))
            {
                checkbox = cb;
                break;
            }
        }
        QVERIFY2(checkbox != nullptr, "Preferences must expose a `High contrast levels` checkbox");
        // Explicit null-check for clang-analyzer (see sibling test).
        if (checkbox == nullptr)
        {
            return;
        }
        QVERIFY(!checkbox->isChecked());

        // Live preview: toggle on -> controller flag flips
        // immediately, `themeChanged` fires via existing wiring.
        checkbox->setChecked(true);
        QCoreApplication::processEvents();
        QVERIFY2(mTheme->IsHighContrast(), "Toggling the checkbox must live-preview high contrast ON");

        // QSettings still false until Ok.
        {
            const QSettings settings;
            QVERIFY(!settings.value(QStringLiteral("ui/highContrastLevels")).toBool());
        }

        // X-close reverts via `closeEvent`.
        editor->close();
        QCoreApplication::processEvents();

        QVERIFY2(!mTheme->IsHighContrast(), "Closing the dialog without Ok must revert the high-contrast live preview");
    }

    // The Column Editor writes back every user-controllable Column
    // field on Apply: header, type, autoDetect, visible. Verify each
    // surface lands in the live configuration and the health cache
    // re-snapshots so the diagnostics surfaces flip in lockstep.
    void TestColumnEditorAppliesEveryField()
    {
        const int levelCol = StreamFixtureForColumnTests();
        QVERIFY2(levelCol >= 0, "level column must exist after streaming");
        auto *model = mWindow->Model();
        const int msgCol = ColumnByHeader(*model, QStringLiteral("msg"));
        QVERIFY2(msgCol >= 0, "msg column must exist after streaming");

        // Baseline: msg auto-detected to String, visible, header "msg".
        QVERIFY(model->Layout().Columns()[static_cast<size_t>(msgCol)].visible);
        QCOMPARE(model->Layout().Columns()[static_cast<size_t>(msgCol)].type, loglib::ColumnType::String);

        ColumnEditor editor(model, msgCol);

        // Direct accessors instead of `findChild` -- Linux Qt 6.8 +
        // offscreen QPA strands the lookup for child widgets the same
        // way it does for QActions (see `FindActionByObjectName`).
        auto *headerEdit = editor.findChild<QLineEdit *>();
        auto *typeCombo = editor.findChild<QComboBox *>();
        auto *visibleCheck = editor.findChild<QCheckBox *>();
        QVERIFY(headerEdit != nullptr);
        QVERIFY(typeCombo != nullptr);
        QVERIFY(visibleCheck != nullptr);
        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): prior QVERIFYs abort on null.
        QCOMPARE(headerEdit->text(), QStringLiteral("msg"));

        // Drive the form: rename, pin to Integer, and hide.
        headerEdit->setText(QStringLiteral("message"));
        // Index 4 in the combo is "Integer" (see TypeChoices() in
        // column_editor.cpp); the choice list is the single source of
        // truth and the order is documented inline.
        constexpr int INTEGER_CHOICE_INDEX = 4;
        typeCombo->setCurrentIndex(INTEGER_CHOICE_INDEX);
        visibleCheck->setChecked(false);

        const QSignalSpy healthSpy(model, &LogModel::columnHealthChanged);
        editor.Apply();

        const auto &updated = model->Layout().Columns()[static_cast<size_t>(msgCol)];
        QCOMPARE(QString::fromStdString(updated.header), QStringLiteral("message"));
        QCOMPARE(updated.type, loglib::ColumnType::Integer);
        QVERIFY2(!updated.autoDetect, "Pinning a concrete type must flip autoDetect off");
        QVERIFY2(!updated.visible, "Visible checkbox must round-trip to Column::visible");

        // The diagnostics cache picked up the new type immediately;
        // pinning a string column to Integer must reveal mismatches.
        QVERIFY2(healthSpy.count() >= 1, "Apply must trigger RefreshColumnHealth and emit columnHealthChanged");
        const auto health = model->ColumnHealth(msgCol);
        QVERIFY(health.has_value());
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        QVERIFY2(health->matchingSlots < health->presentSlots, "Pinned-Integer string column must report mismatches");
    }

    // Regression: editor-driven type edits that cross the
    // Enumeration/Level boundary must emit `enumColumnsChanged`. The
    // streaming auto-detect path emits the signal from
    // `LogModel::AppendBatch`'s snapshot diff; before
    // `LogModel::ApplyColumnTypeEdit` was introduced the editor path
    // silently skipped it, leaving any active enum filter on the
    // column wired to a now-stale bitset and the proxy's
    // `EnumDictRank` cache uninvalidated.
    //
    // We drive both directions on the `category` column (4 unique
    // values, well within `DEFAULT_ENUM_VALUE_CAP`) returned by
    // `StreamFixtureForColumnTests`:
    //   1. Enumeration -> String: should emit `Demoted`.
    //   2. String -> Enumeration: should emit `Promoted`.
    void TestColumnEditorTypeEditEmitsEnumColumnsChanged()
    {
        const int levelCol = StreamFixtureForColumnTests();
        QVERIFY2(levelCol >= 0, "category column must exist after streaming");
        auto *model = mWindow->Model();
        // The streaming detector auto-promotes `category` to
        // `Enumeration` (the key is not a recognized level name, so
        // it stops short of `Type::Level`). Verify the precondition
        // so a future detector change doesn't silently disarm the
        // test.
        QCOMPARE(model->Layout().Columns()[static_cast<size_t>(levelCol)].type, loglib::ColumnType::Enumeration);

        // (1) Enumeration -> String: editor demotes the column. The
        // signal must fire so any active enum filter rebuilds onto
        // the string-set fallback and the cached rank entry is dropped.
        {
            const QSignalSpy enumSpy(model, &LogModel::enumColumnsChanged);
            QVERIFY(enumSpy.isValid());
            ColumnEditor editor(model, levelCol);
            auto *typeCombo = editor.findChild<QComboBox *>();
            QVERIFY(typeCombo != nullptr);
            // Index 2 is "String" -- mirrors TypeChoices() in
            // column_editor.cpp; same convention as the other editor
            // tests in this file.
            constexpr int STRING_CHOICE_INDEX = 2;
            // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): prior QVERIFY aborts on null.
            typeCombo->setCurrentIndex(STRING_CHOICE_INDEX);
            editor.Apply();

            QCOMPARE(model->Layout().Columns()[static_cast<size_t>(levelCol)].type, loglib::ColumnType::String);
            bool sawDemote = false;
            for (int i = 0; i < enumSpy.count(); ++i)
            {
                const QList<QVariant> &args = enumSpy.at(i);
                const auto reason = args.at(0).value<EnumColumnsChangeReason>();
                const int columnIndex = args.at(1).toInt();
                if (reason == EnumColumnsChangeReason::Demoted && columnIndex == levelCol)
                {
                    sawDemote = true;
                    break;
                }
            }
            QVERIFY2(sawDemote, "Editor-driven Enumeration->String must emit enumColumnsChanged(Demoted, levelCol)");
        }

        // (2) String -> Enumeration: editor re-promotes the column.
        // The signal must fire so any pre-existing string-fallback
        // enum filter upgrades onto the new bitset fast path and the
        // proxy refreshes its rank cache.
        {
            const QSignalSpy enumSpy(model, &LogModel::enumColumnsChanged);
            QVERIFY(enumSpy.isValid());
            ColumnEditor editor(model, levelCol);
            auto *typeCombo = editor.findChild<QComboBox *>();
            QVERIFY(typeCombo != nullptr);
            constexpr int ENUMERATION_CHOICE_INDEX = 8;
            // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): prior QVERIFY aborts on null.
            typeCombo->setCurrentIndex(ENUMERATION_CHOICE_INDEX);
            editor.Apply();

            QCOMPARE(model->Layout().Columns()[static_cast<size_t>(levelCol)].type, loglib::ColumnType::Enumeration);
            bool sawPromote = false;
            for (int i = 0; i < enumSpy.count(); ++i)
            {
                const QList<QVariant> &args = enumSpy.at(i);
                const auto reason = args.at(0).value<EnumColumnsChangeReason>();
                const int columnIndex = args.at(1).toInt();
                if (reason == EnumColumnsChangeReason::Promoted && columnIndex == levelCol)
                {
                    sawPromote = true;
                    break;
                }
            }
            QVERIFY2(sawPromote, "Editor-driven String->Enumeration must emit enumColumnsChanged(Promoted, levelCol)");
        }
    }

    // Regression: a no-op editor accept (user presses OK without
    // touching the type combo) must not emit `enumColumnsChanged`.
    // The signal triggers an O(n) filter rebuild downstream, so the
    // editor path should short-circuit when nothing changed.
    void TestColumnEditorNoTypeChangeIsSilent()
    {
        const int levelCol = StreamFixtureForColumnTests();
        QVERIFY2(levelCol >= 0, "level column must exist after streaming");
        auto *model = mWindow->Model();
        const int msgCol = ColumnByHeader(*model, QStringLiteral("msg"));
        QVERIFY2(msgCol >= 0, "msg column must exist after streaming");

        const QSignalSpy enumSpy(model, &LogModel::enumColumnsChanged);
        QVERIFY(enumSpy.isValid());
        ColumnEditor editor(model, msgCol);
        editor.Apply(); // Combo untouched -- still on the same choice.

        QCOMPARE(enumSpy.count(), 0);
    }

    // Regression: an auto-promoted column (e.g. `category` ->
    // `Enumeration` via the streaming detector) carries
    // `(type=Enumeration, autoDetect=true)` because the pipeline
    // flips only `type` through `SetColumnType`. The Type combo
    // must therefore:
    //   1. Surface the resolved type in the combo (the user must
    //      see "Enumeration", not a fallback to "Auto-detect").
    //   2. Round-trip cleanly on accept-without-change: the
    //      column's `(type, autoDetect)` pair stays intact (no
    //      silent `(Enumeration, false)` pin, no destructive
    //      `(Any, true)` rewrite via the "Auto-detect" entry).
    void TestColumnEditorPreservesAutoDetectFlagOnPromotedColumn()
    {
        const int categoryCol = StreamFixtureForColumnTests();
        QVERIFY2(categoryCol >= 0, "category column must exist after streaming");
        auto *model = mWindow->Model();

        // Precondition: streaming auto-promoted `category` to
        // `Enumeration` with `autoDetect = true`: the detector
        // never clears the flag. `FindTypeChoiceIndex` must surface
        // this as "Enumeration", not as index 0 ("Auto-detect").
        const auto &preEdit = model->Layout().Columns()[static_cast<size_t>(categoryCol)];
        QCOMPARE(preEdit.type, loglib::ColumnType::Enumeration);
        QVERIFY2(preEdit.autoDetect, "streaming auto-promotion must leave autoDetect=true");

        ColumnEditor editor(model, categoryCol);
        auto *typeCombo = editor.findChild<QComboBox *>();
        QVERIFY(typeCombo != nullptr);
        // (1) Combo must show the resolved type. Index 8 is
        // "Enumeration" in `TypeChoices()`. Index 0 (the auto-detect
        // fallback) would mislead the user.
        constexpr int ENUMERATION_CHOICE_INDEX = 8;
        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): prior QVERIFY aborts on null.
        QCOMPARE(typeCombo->currentIndex(), ENUMERATION_CHOICE_INDEX);

        // (2) Accept-without-change must preserve both fields.
        // No `enumColumnsChanged` either: the column did not cross
        // the enum boundary, and downstream filter rebuilds are
        // expensive.
        const QSignalSpy enumSpy(model, &LogModel::enumColumnsChanged);
        QVERIFY(enumSpy.isValid());
        editor.Apply();

        const auto &postEdit = model->Layout().Columns()[static_cast<size_t>(categoryCol)];
        QCOMPARE(postEdit.type, loglib::ColumnType::Enumeration);
        QVERIFY2(postEdit.autoDetect, "Accept-without-change must preserve the auto-detector's autoDetect=true flag");
        QCOMPARE(enumSpy.count(), 0);
    }

    // Companion to the above for the *enum-promotion* branch of
    // the rescan: pick a column whose data forms a small set of
    // repeated values (`category` -> info/warn/error/debug). After
    // pinning to a wrong terminal type and flipping to
    // "Auto-detect", the rescan must promote the column to
    // `Enumeration` (or `Level`, since the alias matches and the
    // dictionary tolerance holds). Validates the integrated
    // ApplyColumnTypeEdit -> RescanColumnForAutoDetection ->
    // PromoteColumnToEnum chain through the actual GUI seam.
    void TestColumnEditorAutoDetectPromotesEnumColumn()
    {
        const int categoryCol = StreamFixtureForColumnTests();
        QVERIFY2(categoryCol >= 0, "category column must exist after streaming");
        auto *model = mWindow->Model();

        // `category` was already promoted by streaming. Demote it
        // to Integer so picking Auto-detect is a real transition.
        model->MutableLayout().SetColumnType(static_cast<size_t>(categoryCol), loglib::ColumnType::Integer);
        model->MutableLayout().SetColumnAutoDetect(static_cast<size_t>(categoryCol), false);

        // Pump the model through the same edit path the user would
        // hit to make sure `OnUserChangedColumnType` is invoked
        // (which clears dict state) before the rescan runs.
        model->ApplyColumnTypeEdit(categoryCol, loglib::ColumnType::Integer, false);
        QCOMPARE(model->Layout().Columns()[static_cast<size_t>(categoryCol)].type, loglib::ColumnType::Integer);

        ColumnEditor editor(model, categoryCol);
        auto *typeCombo = editor.findChild<QComboBox *>();
        QVERIFY(typeCombo != nullptr);
        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): prior QVERIFY aborts on null.
        typeCombo->setCurrentIndex(0); // "Auto-detect"
        editor.Apply();

        const auto promotedType = model->Layout().Columns()[static_cast<size_t>(categoryCol)].type;
        QVERIFY2(
            promotedType == loglib::ColumnType::Enumeration || promotedType == loglib::ColumnType::Level,
            "RescanColumnForAutoDetection must promote a small-cardinality column out of Any"
        );
    }

    // Double-clicking a diagnostics row emits `editColumnRequested`
    // with the source-table column index. Verifying the signal
    // (rather than reaching into the editor it eventually pops) keeps
    // the dialog test layer-agnostic and lets `MainWindow` own the
    // editor wiring.
    void TestDiagnosticsDialogDoubleClickEmitsEditRequest()
    {
        const int levelCol = StreamFixtureForColumnTests();
        QVERIFY2(levelCol >= 0, "level column must exist after streaming");
        auto *model = mWindow->Model();
        const int msgCol = ColumnByHeader(*model, QStringLiteral("msg"));
        QVERIFY2(msgCol >= 0, "msg column must exist after streaming");

        const ConfigurationDiagnosticsDialog dialog(model);
        QSignalSpy editSpy(&dialog, &ConfigurationDiagnosticsDialog::editColumnRequested);

        auto *table = dialog.findChild<QTableWidget *>();
        QVERIFY(table != nullptr);

        // Find the row corresponding to `msg` (sorting may rearrange
        // the table independently of source-column order) and
        // simulate a double-click.
        int msgRow = -1;
        for (int row = 0; row < table->rowCount(); ++row)
        {
            const QTableWidgetItem *item = table->item(row, 0);
            if (item != nullptr && item->text() == QStringLiteral("msg"))
            {
                msgRow = row;
                break;
            }
        }
        QVERIFY2(msgRow >= 0, "Dialog must surface a row for `msg`");
        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): prior QVERIFY aborts on null.
        emit table->cellDoubleClicked(msgRow, 0);

        QCOMPARE(editSpy.count(), 1);
        QCOMPARE(editSpy.takeFirst().at(0).toInt(), msgCol);
    }

    // The columns manager mirrors every `Column` row in the live
    // configuration -- one table row per column, with the visible
    // checkbox in the rightmost cell. This is the smoke test that
    // the dialog is wired to the live model rather than a stale
    // snapshot, and that the Visible cell carries an interactable
    // check state.
    void TestColumnsManagerListsEveryColumn()
    {
        const int levelCol = StreamFixtureForColumnTests();
        QVERIFY2(levelCol >= 0, "level column must exist after streaming");
        auto *model = mWindow->Model();
        const int msgCol = ColumnByHeader(*model, QStringLiteral("msg"));
        QVERIFY2(msgCol >= 0, "msg column must exist after streaming");

        const ColumnsManagerDialog dialog(model, mWindow);

        auto *table = dialog.findChild<QTableWidget *>();
        QVERIFY(table != nullptr);
        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): prior QVERIFY aborts on null.
        QCOMPARE(table->rowCount(), static_cast<int>(model->Layout().Columns().size()));

        // The Visible cell sits in column 4 (see COL_VISIBLE in
        // columns_manager_dialog.cpp); a row whose underlying
        // column is visible must surface as Qt::Checked.
        constexpr int VISIBLE_COL = 4;
        const QTableWidgetItem *msgVisible = table->item(msgCol, VISIBLE_COL);
        QVERIFY(msgVisible != nullptr);
        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): prior QVERIFY aborts on null.
        QCOMPARE(msgVisible->checkState(), Qt::Checked);
        QVERIFY2(
            (msgVisible->flags() & Qt::ItemIsUserCheckable) != 0,
            "Visible cell must be user-checkable so the in-place toggle works"
        );
    }

    // Toggling the Visible checkbox in the manager must propagate
    // all the way to the live `Column::visible` flag and to the
    // header (i.e. go through `MainWindow::SetColumnVisible`, not
    // straight to the lib mutator). Doing the round-trip in the
    // test avoids the bug where the manager flipped only its own
    // table-widget state while the underlying header still showed
    // the column.
    void TestColumnsManagerVisibilityToggleHidesColumn()
    {
        const int levelCol = StreamFixtureForColumnTests();
        QVERIFY2(levelCol >= 0, "level column must exist after streaming");
        auto *model = mWindow->Model();
        const int msgCol = ColumnByHeader(*model, QStringLiteral("msg"));
        QVERIFY2(msgCol >= 0, "msg column must exist after streaming");
        QVERIFY(model->Layout().Columns()[static_cast<size_t>(msgCol)].visible);

        const ColumnsManagerDialog dialog(model, mWindow);
        auto *table = dialog.findChild<QTableWidget *>();
        QVERIFY(table != nullptr);

        constexpr int VISIBLE_COL = 4;
        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): prior QVERIFY aborts on null.
        QTableWidgetItem *item = table->item(msgCol, VISIBLE_COL);
        QVERIFY(item != nullptr);
        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): prior QVERIFY aborts on null.
        item->setCheckState(Qt::Unchecked);

        QVERIFY2(
            !model->Layout().Columns()[static_cast<size_t>(msgCol)].visible,
            "Unchecking the Visible cell must flip Column::visible to false"
        );

        auto *tableView = mWindow->findChild<LogTableView *>();
        QVERIFY2(tableView != nullptr, "MainWindow must own a LogTableView");
        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): prior QVERIFY aborts on null.
        QVERIFY2(
            tableView->horizontalHeader()->isSectionHidden(msgCol),
            "Toggle must reach the header (i.e. go via MainWindow::SetColumnVisible, not just the lib mutator)"
        );
    }

    // `MoveSelectedDown` calls `LogModel::MoveColumn`, which both
    // rotates `Configuration().columns` and re-emits header /
    // visibility state to the view. The test verifies the order
    // shift end-to-end: pick the first row, push it one slot down,
    // and assert the two columns swap positions in both the model
    // and the manager table itself.
    void TestColumnsManagerMoveDownReordersColumns()
    {
        const int levelCol = StreamFixtureForColumnTests();
        QVERIFY2(levelCol >= 0, "level column must exist after streaming");
        auto *model = mWindow->Model();
        QVERIFY2(model->Layout().Columns().size() >= 2, "fixture must yield at least two columns");

        const std::string firstKeys = model->Layout().Columns().front().keys.empty()
                                          ? std::string{}
                                          : model->Layout().Columns().front().keys.front();
        const std::string secondKeys =
            model->Layout().Columns()[1].keys.empty() ? std::string{} : model->Layout().Columns()[1].keys[0];
        QVERIFY2(!firstKeys.empty() && !secondKeys.empty(), "fixture columns must carry stable keys");

        ColumnsManagerDialog dialog(model, mWindow);
        auto *table = dialog.findChild<QTableWidget *>();
        QVERIFY(table != nullptr);
        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): prior QVERIFY aborts on null.
        table->selectRow(0);

        dialog.MoveSelectedDown();

        const auto &columns = model->Layout().Columns();
        QVERIFY2(columns.size() >= 2, "Move must not lose columns");
        QCOMPARE(columns[0].keys.empty() ? std::string{} : columns[0].keys.front(), secondKeys);
        QCOMPARE(columns[1].keys.empty() ? std::string{} : columns[1].keys.front(), firstKeys);
        // The manager moves the selection along with the row so the
        // user can drag the same column further down with repeated
        // clicks. (Verifying the selection prevents a regression
        // where a stable index lookup would re-pick the *new* top
        // row by mistake.)
        QCOMPARE(table->currentRow(), 1);
    }

    // Edge case: Move up on the top row, Move down on the bottom
    // row. Either would push the index out of [0, columnCount());
    // the dialog must clamp instead of asserting or wrapping.
    void TestColumnsManagerMoveAtBoundariesIsNoOp()
    {
        const int levelCol = StreamFixtureForColumnTests();
        QVERIFY2(levelCol >= 0, "level column must exist after streaming");
        auto *model = mWindow->Model();
        const auto initialColumns = model->Layout().Columns();
        QVERIFY2(initialColumns.size() >= 2, "fixture must yield at least two columns");

        ColumnsManagerDialog dialog(model, mWindow);
        auto *table = dialog.findChild<QTableWidget *>();
        QVERIFY(table != nullptr);

        // Top row: Move up is a no-op.
        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): prior QVERIFY aborts on null.
        table->selectRow(0);
        dialog.MoveSelectedUp();
        for (size_t i = 0; i < initialColumns.size(); ++i)
        {
            QCOMPARE(model->Layout().Columns()[i].keys, initialColumns[i].keys);
        }

        // Bottom row: Move down is a no-op.
        const int lastRow = static_cast<int>(initialColumns.size()) - 1;
        table->selectRow(lastRow);
        dialog.MoveSelectedDown();
        for (size_t i = 0; i < initialColumns.size(); ++i)
        {
            QCOMPARE(model->Layout().Columns()[i].keys, initialColumns[i].keys);
        }
    }

    // Regression for bug 1: the manager only listened to
    // `modelReset`, `headerDataChanged`, and `columnHealthChanged`.
    // A header-drag (or the streaming timestamp-bubble) reorders
    // columns via `MoveColumn`, which emits
    // `beginMoveColumns`/`endMoveColumns` only -- no
    // `headerDataChanged` follows because the column data is
    // unchanged. Before the fix, the manager's table widget kept
    // its pre-move row order until something else (e.g. a sort
    // toggle) happened to trigger a refresh.
    void TestColumnsManagerRefreshesAfterExternalColumnsMoved()
    {
        const int levelCol = StreamFixtureForColumnTests();
        QVERIFY2(levelCol >= 0, "level column must exist after streaming");
        auto *model = mWindow->Model();
        QVERIFY2(model->Layout().Columns().size() >= 2, "fixture must yield at least two columns");

        ColumnsManagerDialog dialog(model, mWindow);
        dialog.show();
        auto *table = dialog.findChild<QTableWidget *>();
        QVERIFY(table != nullptr);

        // Snapshot the pre-move dialog row 0 / row 1 headers so the
        // assertion below sees the actual swap rather than any
        // canonical ordering.
        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): prior QVERIFY aborts on null.
        const QTableWidgetItem *row0Before = table->item(0, 0);
        const QTableWidgetItem *row1Before = table->item(1, 0);
        QVERIFY(row0Before != nullptr && row1Before != nullptr);
        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): prior QVERIFY aborts on null.
        const QString headerAt0Before = row0Before->text();
        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): prior QVERIFY aborts on null.
        const QString headerAt1Before = row1Before->text();
        QVERIFY(headerAt0Before != headerAt1Before);

        // Reorder via the model layer -- the same seam a header drag
        // (`OnSourceColumnsMoved`) goes through. The fix adds a
        // `columnsMoved` -> `Refresh` connection in the dialog, so
        // the manager's rows must now follow this move without any
        // intervening UI event.
        QVERIFY2(model->MoveColumn(0, 1), "MoveColumn must succeed for a 2+ column model");

        // Pump the event loop so the queued-connection (if any) fires
        // before we sample the table widget.
        QCoreApplication::processEvents();

        const QTableWidgetItem *row0After = table->item(0, 0);
        const QTableWidgetItem *row1After = table->item(1, 0);
        QVERIFY(row0After != nullptr && row1After != nullptr);
        QCOMPARE(row0After->text(), headerAt1Before);
        QCOMPARE(row1After->text(), headerAt0Before);
        dialog.close();
    }

    // The View menu's "Manage columns\u2026" entry is the only
    // production entry point into the manager. Verifying it both
    // exists and triggers the dialog catches the case where a menu
    // rebuild forgets to re-add the action after a column shape
    // change.
    void TestViewMenuManageColumnsActionOpensDialog()
    {
        const int levelCol = StreamFixtureForColumnTests();
        QVERIFY2(levelCol >= 0, "level column must exist after streaming");

        auto *viewMenu = mWindow->findChild<QMenu *>(QStringLiteral("menuView"));
        QVERIFY2(viewMenu != nullptr, "MainWindow must expose its View menu");
        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): prior QVERIFY aborts on null.
        emit viewMenu->aboutToShow();

        QAction *manage = nullptr;
        const auto actions = viewMenu->actions();
        for (QAction *action : actions)
        {
            if (action->objectName() == QStringLiteral("actionManageColumns"))
            {
                manage = action;
                break;
            }
        }
        QVERIFY2(manage != nullptr, "View menu must carry the Manage columns... entry");
        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): prior QVERIFY aborts on null.
        manage->trigger();

        auto *dialog = mWindow->findChild<ColumnsManagerDialog *>(QStringLiteral("ColumnsManagerDialog"));
        QVERIFY2(dialog != nullptr, "Triggering Manage columns... must construct a ColumnsManagerDialog");
        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): prior QVERIFY aborts on null.
        QVERIFY2(!dialog->isHidden(), "Manager dialog must be shown after triggering its action");
        dialog->close();
    }

    // Find must skip hidden columns. The fixture has `level`
    // (info/warn/...) and `msg` (m0..m199); "info" only matches
    // `level`, so hiding `level` yields an empty match list.
    void TestFindSkipsHiddenColumns()
    {
        const int levelCol = StreamFixtureForColumnTests();
        QVERIFY2(levelCol >= 0, "level column must exist after streaming");
        auto *model = mWindow->Model();

        const LogFilterModel *proxy = mWindow->FilterModel();
        QVERIFY2(proxy != nullptr, "MainWindow must own a LogFilterModel");
        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): false positive; prior `QVERIFY2` aborts on null.
        QVERIFY2(proxy->rowCount() > 0, "fixture must yield at least one row");

        const QModelIndex start = proxy->index(0, 0);
        const QVariant needle = QVariant::fromValue(QStringLiteral("info"));
        const Qt::MatchFlags flags = Qt::MatchContains | Qt::MatchWrap | Qt::MatchRecursive;

        // Sanity: with `level` visible, "info" matches at least one row.
        const QModelIndexList beforeHide = proxy->MatchRow(start, Qt::DisplayRole, needle, 1, flags, true, 0);
        QVERIFY2(!beforeHide.isEmpty(), "precondition: 'info' must match while level is visible");
        QCOMPARE(beforeHide.front().column(), levelCol);

        mWindow->SetColumnVisible(levelCol, false);
        QVERIFY(!model->Layout().Columns()[static_cast<size_t>(levelCol)].visible);

        const QModelIndexList afterHide = proxy->MatchRow(start, Qt::DisplayRole, needle, 1, flags, true, 0);
        QVERIFY2(
            afterHide.isEmpty(), "Find must skip hidden columns; hiding the only matching column must yield no result"
        );
    }

    // Hiding the sorted-by column would leave the sort active
    // with no UI glyph to clear it (the indicator lives on a
    // zero-width section). `SetColumnVisible` resets the sort.
    void TestHidingSortedColumnClearsSort()
    {
        const int levelCol = StreamFixtureForColumnTests();
        QVERIFY2(levelCol >= 0, "level column must exist after streaming");

        auto *tableView = mWindow->findChild<LogTableView *>();
        QVERIFY2(tableView != nullptr, "MainWindow must own a LogTableView");
        const QHeaderView *header = tableView->horizontalHeader();
        QVERIFY2(header != nullptr, "view must own a horizontal header");

        // Sort by `level` so the indicator points at it.
        tableView->sortByColumn(levelCol, Qt::AscendingOrder);
        QCoreApplication::processEvents();
        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): false positive; prior `QVERIFY2` aborts on null.
        QVERIFY2(header->isSortIndicatorShown(), "precondition: sort indicator must be shown after sortByColumn");
        QCOMPARE(header->sortIndicatorSection(), levelCol);

        mWindow->SetColumnVisible(levelCol, false);

        QVERIFY2(
            !header->isSortIndicatorShown() || header->sortIndicatorSection() != levelCol,
            "hiding the sorted column must clear the sort indicator"
        );
    }

    // Hiding a non-sorted column must NOT touch the sort indicator
    // -- only the case where the user actively hides the column they
    // sorted by needs the reset.
    void TestHidingUnsortedColumnLeavesSortAlone()
    {
        const int levelCol = StreamFixtureForColumnTests();
        QVERIFY2(levelCol >= 0, "level column must exist after streaming");
        auto *model = mWindow->Model();
        const int msgCol = ColumnByHeader(*model, QStringLiteral("msg"));
        QVERIFY2(msgCol >= 0, "msg column must exist after streaming");

        auto *tableView = mWindow->findChild<LogTableView *>();
        const QHeaderView *header = tableView->horizontalHeader();

        tableView->sortByColumn(levelCol, Qt::AscendingOrder);
        QCoreApplication::processEvents();
        QCOMPARE(header->sortIndicatorSection(), levelCol);

        mWindow->SetColumnVisible(msgCol, false);

        QVERIFY2(
            header->isSortIndicatorShown() && header->sortIndicatorSection() == levelCol,
            "hiding an unrelated column must not change the sort indicator"
        );
    }

    // Duplicate-header disambiguation in the View / Show menus.
    // Qt lets two columns share a header; the stable id is `keys`.
    // Each duplicate gets `header [keys]` so the user can tell
    // them apart.
    void TestDuplicateHeaderDisambiguation()
    {
        QVERIFY(StreamFixtureForColumnTests() >= 0);
        auto *model = mWindow->Model();

        // Make column 1's header collide with column 0's so we
        // have a real duplicate. The configuration is read-only
        // through the manager, so we round-trip via Save / Load.
        const auto &columns = model->Layout().Columns();
        QVERIFY2(columns.size() >= 2, "fixture must have at least two columns");

        loglib::LogConfiguration mutated;
        mutated.columns = columns;
        const QString sharedHeader = QString::fromStdString(mutated.columns[0].header);
        mutated.columns[1].header = mutated.columns[0].header;

        const QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        const QString cfgPath = tempDir.filePath(QStringLiteral("dupes.json"));
        loglib::SaveConfiguration(mutated, cfgPath.toStdString(), loglib::SaveScope::Full);
        QVERIFY(mWindow->TryLoadAsConfigurationForTest(cfgPath));
        QCoreApplication::processEvents();

        auto *viewMenu = mWindow->findChild<QMenu *>(QStringLiteral("menuView"));
        QVERIFY2(viewMenu != nullptr, "View menu must exist");
        emit viewMenu->aboutToShow();

        QStringList labels;
        for (const QAction *act : viewMenu->actions())
        {
            labels.append(act->text());
        }
        // Both colliding entries must include a `[...]` disambiguator;
        // a non-colliding entry (any other column) must not.
        int withDisambiguator = 0;
        for (const QString &label : labels)
        {
            if (label.startsWith(sharedHeader) && label.contains(QLatin1Char('[')))
            {
                ++withDisambiguator;
            }
        }
        QVERIFY2(withDisambiguator == 2, "both duplicate-headered columns must be disambiguated in View menu");
    }

    // Regression: `EnumRankFor` self-heals when the live dictionary
    // has grown or its address changed since the cached rank was
    // built. Production also has a `MainWindow` invalidation tick;
    // this test wires the proxy directly (no `MainWindow`) so the
    // self-heal is the only defence.
    void TestEnumRankCacheSelfHealsOnDictionaryGrowth()
    {
        LogModel model;
        LogFilterModel filterModel;
        filterModel.setSourceModel(&model);
        filterModel.SetLogModel(&model);

        // Direct `AppendBatch` driving. Stream mode needs
        // `BeginStreaming` first; an empty fixture satisfies it.
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

        // Batch 1: dict_v1 = {"info" -> 0, "warn" -> 1}.
        {
            loglib::RowBatch batch;
            batch.firstLineNumber = 1;
            batch.newKeys.emplace_back("category");
            batch.lines.push_back(makeLine("info"));
            batch.lines.push_back(makeLine("warn"));
            model.AppendBatch(std::move(batch));
        }

        const int levelCol = ColumnByHeader(model, QStringLiteral("category"));
        QVERIFY2(levelCol >= 0, "level column must exist after promotion");
        QCOMPARE(model.Layout().Columns()[static_cast<size_t>(levelCol)].type, loglib::ColumnType::Enumeration);

        // Warm the cache.
        filterModel.sort(levelCol, Qt::AscendingOrder);
        QVERIFY2(filterModel.EnumRankCacheSizeForTest() == std::size_t{1}, "first sort must populate the rank cache");

        // Snapshot pre-growth order. Without the self-heal, ids past
        // the cached bitset all collide at the past-the-end rank.
        QStringList orderedBeforeGrowth;
        orderedBeforeGrowth.reserve(filterModel.rowCount());
        for (int i = 0; i < filterModel.rowCount(); ++i)
        {
            orderedBeforeGrowth.append(filterModel.index(i, levelCol).data(Qt::DisplayRole).toString());
        }
        QCOMPARE(orderedBeforeGrowth, (QStringList{QStringLiteral("info"), QStringLiteral("warn")}));

        // Batch 2: dictionary grows from 2 to 4 entries; cached rank
        // is now too small.
        {
            loglib::RowBatch batch;
            batch.firstLineNumber = 3;
            batch.lines.push_back(makeLine("error"));
            batch.lines.push_back(makeLine("debug"));
            model.AppendBatch(std::move(batch));
        }
        const loglib::KeyId levelKey = keys.Find("category");
        QVERIFY(levelKey != loglib::INVALID_KEY_ID);
        const loglib::EnumDictionary *dict = model.Table().EnumDictionaries().Find(levelKey);
        QVERIFY2(dict != nullptr, "level dictionary must still exist after batch 2");
        QCOMPARE(static_cast<int>(dict->Size()), 4);

        // Re-sort. `rank.DictSize() < live.Size()` trips and
        // `EnumRankFor` rebuilds; the cache stays at one row (same
        // `KeyId`) but its `mIdToRank` now covers all 4 ids.
        filterModel.sort(levelCol, Qt::AscendingOrder);
        QCOMPARE(filterModel.EnumRankCacheSizeForTest(), std::size_t{1});

        QStringList orderedAfterGrowth;
        orderedAfterGrowth.reserve(filterModel.rowCount());
        for (int i = 0; i < filterModel.rowCount(); ++i)
        {
            orderedAfterGrowth.append(filterModel.index(i, levelCol).data(Qt::DisplayRole).toString());
        }
        // Alphabetical: debug < error < info < warn. Without the
        // self-heal, "debug" / "error" would tie at past-the-end and
        // appear at the tail in source order.
        QCOMPARE(
            orderedAfterGrowth,
            (QStringList{
                QStringLiteral("debug"), QStringLiteral("error"), QStringLiteral("info"), QStringLiteral("warn")
            })
        );

        model.EndStreaming(false);
    }

    // Regression: `MatchRow` skips hidden columns, but column-
    // visibility flips don't emit any of the proxy signals the find
    // cache hooks. Without explicit invalidation the "*i* of *N*"
    // indicator would strand a count that includes hits in
    // now-hidden columns. `SetColumnVisible` /
    // `ApplyColumnVisibility` now call `OnFindCacheInvalidated`.
    //
    // The main window must be realised so the find dock is visible
    // (the bump gate skips recounts when the bar is hidden).
    void TestHidingColumnInvalidatesFindCache()
    {
        const int categoryCol = StreamFixtureForColumnTests();
        QVERIFY2(categoryCol >= 0, "category column must exist after streaming");

        auto *findRecord = mWindow->findChild<FindRecordWidget *>();
        QVERIFY2(findRecord != nullptr, "MainWindow must own a FindRecordWidget");
        auto *findDock = mWindow->findChild<FindDock *>();
        QVERIFY2(findDock != nullptr, "MainWindow must own a FindDock");
        auto *edit = findRecord->findChild<QLineEdit *>(QStringLiteral("findEdit"));
        QVERIFY2(edit != nullptr, "FindRecordWidget must expose its line edit");

        // Realise the window so the bump gate lets through.
        mWindow->show();
        QVERIFY(QTest::qWaitForWindowExposed(mWindow, 60000));
        findDock->show();
        QCoreApplication::processEvents();
        QVERIFY2(findDock->isVisible(), "find dock must be visible for the bump gate to pass");

        edit->setText(QStringLiteral("info"));
        QSignalSpy spy(findRecord, &FindRecordWidget::MatchCountRequested);
        QVERIFY(spy.isValid());
        // Drain the textChanged debounce so the next emit is
        // unambiguously caused by the hide.
        QVERIFY2(
            spy.count() > 0 || spy.wait(60000), "initial textChanged must trigger a debounced MatchCountRequested"
        );
        spy.clear();

        mWindow->SetColumnVisible(categoryCol, false);
        QVERIFY2(
            spy.count() > 0 || spy.wait(60000),
            "Hiding a column must invalidate the find cache and trigger a fresh MatchCountRequested"
        );
    }

    // Item 5: status-bar "*n* of *m* shown" indicator + inline
    // Clear-filters button. Three branches:
    //   - no filter             -> "N lines",   button hidden
    //   - filter hides rows     -> "M of N shown", button shown
    //   - filter matches every row -> "N lines", button still shown
    //     (the comment in `UpdateRowsShownStatus` calls this out
    //      explicitly; left untested in the original change).
    void TestFilterProblemLabelReportsUnknownColumn()
    {
        const int categoryCol = StreamFixtureForColumnTests();
        QVERIFY2(categoryCol >= 0, "category column must exist after streaming");
        QCoreApplication::processEvents();

        auto *label = mWindow->findChild<QLabel *>(QStringLiteral("filterProblemLabel"));
        QVERIFY2(label != nullptr, "MainWindow must own the filter problem label");
        QVERIFY(label->isHidden());

        loglib::LeafRule leaf;
        leaf.type = loglib::LeafRule::Type::String;
        leaf.columnKeys = {"no_such_column"};
        leaf.matchType = loglib::LeafRule::Match::Contains;
        leaf.filterString = "x";
        mWindow->CommitAdvancedFilterForTest(WireLeavesAsExpression({leaf}));
        QVERIFY(!label->isHidden());
        QVERIFY(label->text().contains(QStringLiteral("no_such_column")));

        mWindow->CommitAdvancedFilterForTest(loglib::FilterExpression{});
        QVERIFY(label->isHidden());
    }

    void TestRowsShownStatusReflectsFilter()
    {
        const int categoryCol = StreamFixtureForColumnTests();
        QVERIFY2(categoryCol >= 0, "category column must exist after streaming");

        // The fixture leaves `mSessionMode` at Idle, but the
        // rows-shown slot now gates on `mModel->rowCount() > 0`,
        // so no `SetSessionModeForTest` dance is needed.
        QCoreApplication::processEvents();

        auto *label = mWindow->findChild<QLabel *>(QStringLiteral("rowsShownLabel"));
        QVERIFY2(label != nullptr, "MainWindow must own the rows-shown status label");
        auto *button = mWindow->findChild<QPushButton *>(QStringLiteral("clearFiltersStatusButton"));
        QVERIFY2(button != nullptr, "MainWindow must own the clear-filters status button");

        // Locale-formatted total used in every "no filter" / "vacuous
        // filter" assertion below. Matches the slot's own
        // `QLocale::system()` + `qlonglong` round-trip.
        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): prior QVERIFY2 aborts on null.
        auto *model = mWindow->Model();
        QVERIFY2(model != nullptr, "MainWindow must own a LogModel");
        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): prior QVERIFY2 aborts on null.
        const int totalRows = model->rowCount();
        QVERIFY2(totalRows > 1, "fixture must stream more than one row for the plural branch");
        const QString totalText = QLocale::system().toString(static_cast<qlonglong>(totalRows));
        const QString unfilteredText = QStringLiteral("%1 lines").arg(totalText);

        // Branch 1: no filter. Label reads "<N> lines"; button is
        // hidden. `isHidden()` (not `isVisible()`) so the assertion
        // exercises the slot's intent -- offscreen-QPA keeps the
        // parent hidden, collapsing `isVisible()` regardless.
        QCOMPARE(label->text(), unfilteredText);
        QVERIFY2(!label->isHidden(), "rows-shown label must show with a non-empty source");
        QVERIFY2(button->isHidden(), "clear-filters button must hide when no filter is active");

        // Branch 2: filter hides most rows. The fixture streams 200
        // lines across 4 categories, so "info" alone leaves ~50.
        const QString hideFilterId = QStringLiteral("rows-shown-test-filter");
        QVERIFY2(
            QMetaObject::invokeMethod(
                mWindow,
                "FilterEnumSubmitted",
                Qt::DirectConnection,
                Q_ARG(QString, hideFilterId),
                Q_ARG(int, categoryCol),
                Q_ARG(QStringList, QStringList{QStringLiteral("info")})
            ),
            "FilterEnumSubmitted must be invocable via meta-object"
        );
        QCoreApplication::processEvents();

        QVERIFY2(!button->isHidden(), "clear-filters button must un-hide when at least one filter is active");
        auto *filterModel = mWindow->FilterModel();
        QVERIFY2(filterModel != nullptr, "MainWindow must own a LogFilterModel");
        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): prior QVERIFY2 aborts on null.
        const int visibleRows = filterModel->rowCount();
        QVERIFY2(visibleRows > 0 && visibleRows < totalRows, "fixture filter must hide some but not all rows");
        const QString expectedFiltered =
            QStringLiteral("%1 of %2 shown")
                .arg(QLocale::system().toString(static_cast<qlonglong>(visibleRows)), totalText);
        QCOMPARE(label->text(), expectedFiltered);

        // Branch 3: vacuous filter -- every category selected, so
        // proxyRows == sourceRows but `mFilters` is still populated.
        // Label drops the "of" form; button stays visible.
        // Build the selection outside `Q_ARG`: brace-initializer commas
        // are not protected from the macro's argument splitter.
        const QStringList allCategories{
            QStringLiteral("info"), QStringLiteral("warn"), QStringLiteral("error"), QStringLiteral("debug")
        };
        QVERIFY2(
            QMetaObject::invokeMethod(
                mWindow,
                "FilterEnumSubmitted",
                Qt::DirectConnection,
                Q_ARG(QString, hideFilterId),
                Q_ARG(int, categoryCol),
                Q_ARG(QStringList, allCategories)
            ),
            "FilterEnumSubmitted (vacuous) must be invocable via meta-object"
        );
        QCoreApplication::processEvents();

        QCOMPARE(filterModel->rowCount(), totalRows);
        QVERIFY2(!button->isHidden(), "clear-filters button must stay visible even when the filter matches every row");
        QCOMPARE(label->text(), unfilteredText);

        // Clicking the status button must route through
        // `actionClearAllFilters`. Triggering the action directly
        // is the same end-state and avoids needing a real click on
        // a hidden parent window. The action also exercises the
        // signal wiring on the button itself (`clicked ->
        // actionClearAllFilters::trigger`).
        auto *clearAction = mWindow->findChild<QAction *>(QStringLiteral("actionClearAllFilters"));
        QVERIFY2(clearAction != nullptr, "MainWindow must own actionClearAllFilters");
        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): prior QVERIFY2 aborts on null.
        clearAction->trigger();
        QCoreApplication::processEvents();

        QVERIFY2(button->isHidden(), "clear-filters button must re-hide after clearing all filters");
        QCOMPARE(label->text(), unfilteredText);
    }

    // The Add-filter dropdown lists every *visible* column under
    // `Add filter on "<col>"…`. Verifies both the population
    // (one entry per visible column, in column order) and the
    // hidden-column filter (toggling a column hidden must drop
    // its entry on the next open, mirroring the header
    // right-click which refuses to expose hidden columns).
    void TestAddFilterDropdownListsVisibleColumns()
    {
        const int levelCol = StreamFixtureForColumnTests();
        QVERIFY2(levelCol >= 0, "fixture must produce columns");

        auto *menu = mWindow->findChild<QMenu *>(QStringLiteral("addFilterSplitMenu"));
        QVERIFY2(menu != nullptr, "add-filter dropdown must exist");
        // NOLINTBEGIN(clang-analyzer-core.CallAndMessage): prior QVERIFY2 aborts on null.
        // `aboutToShow` is the production rebuild trigger; firing
        // it directly is what every other rebuild-on-show test
        // does (`TestMainToolbarToggleAppearsInViewMenu`,
        // `RebuildViewMenu` tests).
        emit menu->aboutToShow();

        QStringList entryTexts;
        for (const QAction *act : menu->actions())
        {
            entryTexts.append(act != nullptr ? act->text() : QStringLiteral("<null>"));
        }
        QVERIFY2(!entryTexts.isEmpty(), "dropdown must list at least one column after streaming a fixture");
        for (const QString &text : entryTexts)
        {
            QVERIFY2(
                text.startsWith(QStringLiteral("Add filter on")),
                "every dropdown entry must follow the `Add filter on \"<col>\"...` shape"
            );
        }

        // Hide the `level` column and re-open: its entry must
        // drop. Cross-checks the visible-only filter.
        auto *model = mWindow->Model();
        const int msgCol = ColumnByHeader(*model, QStringLiteral("msg"));
        QVERIFY2(msgCol >= 0, "fixture must expose `msg` column");
        mWindow->SetColumnVisible(msgCol, false);
        emit menu->aboutToShow();

        for (const QAction *act : menu->actions())
        {
            QVERIFY2(
                !act->text().contains(QStringLiteral("\"msg\"")),
                "hidden column must not surface in the dropdown after hide"
            );
        }
        // NOLINTEND(clang-analyzer-core.CallAndMessage)
    }

    // Triggering an `Add filter on "<col>"…` dropdown entry opens
    // a `FilterEditor` with the row combobox pre-pointed at the
    // clicked column -- same contract the header right-click
    // `TestHeaderContextMenuAddFilterOpensEditorWithColumnPreselected`
    // pins, replicated for the toolbar entry point so a refactor
    // that changes the `AddFilter` signature can't silently break
    // the new shortcut without tripping a test.
    void TestAddFilterDropdownTriggerPreselectsColumn()
    {
        const int levelCol = StreamFixtureForColumnTests();
        QVERIFY2(levelCol >= 0, "fixture must produce columns");

        auto *menu = mWindow->findChild<QMenu *>(QStringLiteral("addFilterSplitMenu"));
        QVERIFY2(menu != nullptr, "add-filter dropdown must exist");
        // NOLINTBEGIN(clang-analyzer-core.CallAndMessage): prior QVERIFY2 aborts on null.
        emit menu->aboutToShow();

        QAction *categoryEntry = nullptr;
        for (QAction *act : menu->actions())
        {
            if (act != nullptr && act->text().contains(QStringLiteral("\"category\"")))
            {
                categoryEntry = act;
                break;
            }
        }
        QVERIFY2(categoryEntry != nullptr, "dropdown must offer an entry for the `category` column");

        categoryEntry->trigger();
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
        QVERIFY2(editor != nullptr, "triggering a dropdown entry must spawn a FilterEditor");
        QCOMPARE(editor->GetRowToFilter(), levelCol);
        editor->close();
        editor->deleteLater();
        QCoreApplication::processEvents();
        // NOLINTEND(clang-analyzer-core.CallAndMessage)
    }

    // The Sort dropdown lists every visible column as two
    // checkable rows: ▲ asc and ▼ desc, each followed by the
    // disambiguated column label in quotes. Hiding a column
    // drops its rows on the next open.
    void TestSortByDropdownListsVisibleColumns()
    {
        // The fixture helper returns the `category` column
        // index; bind to a clearer name.
        const int categoryCol = StreamFixtureForColumnTests();
        QVERIFY2(categoryCol >= 0, "fixture must produce columns");

        auto *menu = mWindow->findChild<QMenu *>(QStringLiteral("sortBySplitMenu"));
        QVERIFY2(menu != nullptr, "sort dropdown must exist");
        // NOLINTBEGIN(clang-analyzer-core.CallAndMessage): prior QVERIFY2 aborts on null.
        emit menu->aboutToShow();

        const QList<QAction *> entries = menu->actions();
        QVERIFY2(!entries.isEmpty(), "dropdown must list at least one column after streaming a fixture");
        int ascCount = 0;
        int descCount = 0;
        for (const QAction *act : entries)
        {
            QVERIFY2(act != nullptr, "dropdown entry must not be null");
            QVERIFY2(act->menu() == nullptr, "flat entries must not carry submenus");
            QVERIFY2(act->isCheckable(), "every dropdown row must be checkable");
            const QString text = act->text();
            QVERIFY2(text.contains(QStringLiteral("\"")), "every dropdown row must carry the column label in quotes");
            QVERIFY2(
                !text.contains(QStringLiteral("Sort by")),
                "row text is just glyph + quoted column name; the host menu carries the verb"
            );
            if (text.startsWith(QString::fromUtf8(u8"\u25B2")))
            {
                ++ascCount;
            }
            else if (text.startsWith(QString::fromUtf8(u8"\u25BC")))
            {
                ++descCount;
            }
            else
            {
                QFAIL("every dropdown row must start with the asc / desc triangle glyph");
            }
        }
        QVERIFY2(ascCount > 0 && ascCount == descCount, "every column must contribute both an Asc and a Desc row");

        // Hide `msg` and re-open: its rows must drop.
        auto *model = mWindow->Model();
        const int msgCol = ColumnByHeader(*model, QStringLiteral("msg"));
        QVERIFY2(msgCol >= 0, "fixture must expose `msg` column");
        mWindow->SetColumnVisible(msgCol, false);
        emit menu->aboutToShow();

        for (const QAction *act : menu->actions())
        {
            QVERIFY2(
                !act->text().contains(QStringLiteral("\"msg\"")),
                "hidden column must not surface in the dropdown after hide"
            );
        }
        // NOLINTEND(clang-analyzer-core.CallAndMessage)
    }

    // Triggering a desc row installs the matching sort on the
    // proxy.
    void TestSortByDropdownTriggerSortsByColumn()
    {
        const int categoryCol = StreamFixtureForColumnTests();
        QVERIFY2(categoryCol >= 0, "fixture must expose `category` column");

        auto *menu = mWindow->findChild<QMenu *>(QStringLiteral("sortBySplitMenu"));
        QVERIFY2(menu != nullptr, "sort dropdown must exist");
        // NOLINTBEGIN(clang-analyzer-core.CallAndMessage): prior QVERIFY2 aborts on null.
        emit menu->aboutToShow();

        QAction *categoryDesc = nullptr;
        for (QAction *act : menu->actions())
        {
            if (act != nullptr && act->text().contains(QStringLiteral("\"category\"")) &&
                act->text().startsWith(QString::fromUtf8(u8"\u25BC")))
            {
                categoryDesc = act;
                break;
            }
        }
        QVERIFY2(categoryDesc != nullptr, "dropdown must offer a desc row for the `category` column");

        categoryDesc->trigger();
        QCoreApplication::processEvents();

        QCOMPARE(mWindow->FilterModel()->SortColumn(), categoryCol);
        QCOMPARE(mWindow->FilterModel()->SortOrder(), Qt::DescendingOrder);
        // NOLINTEND(clang-analyzer-core.CallAndMessage)
    }

    // When every column is hidden both Sort surfaces must
    // surface the `(every column is hidden ...)` placeholder,
    // not leave the menu empty.
    void TestSortDropdownPlaceholderWhenAllColumnsHidden()
    {
        const int categoryCol = StreamFixtureForColumnTests();
        QVERIFY2(categoryCol >= 0, "fixture must expose `category` column");
        auto *model = mWindow->Model();
        const int msgCol = ColumnByHeader(*model, QStringLiteral("msg"));
        QVERIFY2(msgCol >= 0, "fixture must expose `msg` column");

        // Hide every visible column. `SetColumnVisible`
        // clears the sort if the sorted column gets hidden.
        mWindow->SetColumnVisible(categoryCol, false);
        mWindow->SetColumnVisible(msgCol, false);
        QCoreApplication::processEvents();

        // Toolbar dropdown: a single placeholder, disabled.
        auto *splitMenu = mWindow->findChild<QMenu *>(QStringLiteral("sortBySplitMenu"));
        QVERIFY2(splitMenu != nullptr, "sort split menu must exist");
        // NOLINTBEGIN(clang-analyzer-core.CallAndMessage): prior QVERIFY2 aborts on null.
        emit splitMenu->aboutToShow();
        const QList<QAction *> splitActions = splitMenu->actions();
        QCOMPARE(splitActions.size(), 1);
        QVERIFY2(splitActions[0] != nullptr, "placeholder action must be present");
        QVERIFY2(!splitActions[0]->isEnabled(), "placeholder must be disabled");
        QVERIFY2(
            splitActions[0]->text().contains(QStringLiteral("every column is hidden")),
            "placeholder must explain why the dropdown is empty"
        );

        // Top-level Sort menu: Clear-sort + separator +
        // placeholder.
        auto *sortMenu = mWindow->findChild<QMenu *>(QStringLiteral("menuSort"));
        QVERIFY2(sortMenu != nullptr, "Sort menu must exist");
        emit sortMenu->aboutToShow();
        const QList<QAction *> sortActions = sortMenu->actions();
        QCOMPARE(sortActions.size(), 3);
        QCOMPARE(sortActions[0]->objectName(), QStringLiteral("actionClearSort"));
        QVERIFY2(sortActions[1]->isSeparator(), "second slot must be the separator");
        QVERIFY2(!sortActions[2]->isEnabled(), "placeholder must be disabled");
        QVERIFY2(
            sortActions[2]->text().contains(QStringLiteral("every column is hidden")),
            "placeholder must explain why the menu has no per-column entries"
        );
        // NOLINTEND(clang-analyzer-core.CallAndMessage)
    }

    // The status-bar tooltip must rebuild when the sorted
    // column is renamed - `NotifyColumnEdited` emits only
    // `headerDataChanged`, so without the dedicated hook the
    // tooltip would freeze on the old label until the next
    // sort / filter event.
    void TestSortStatusTooltipUpdatesOnColumnRename()
    {
        const int categoryCol = StreamFixtureForColumnTests();
        QVERIFY2(categoryCol >= 0, "fixture must expose `category` column");

        // Sort so the status-bar button surfaces; its tooltip
        // resolves the live column label.
        mWindow->findChild<LogTableView *>()->sortByColumn(categoryCol, Qt::AscendingOrder);
        QCoreApplication::processEvents();

        auto *button = mWindow->findChild<QPushButton *>(QStringLiteral("clearSortStatusButton"));
        QVERIFY2(button != nullptr, "status bar must host the clear-sort indicator");
        // NOLINTBEGIN(clang-analyzer-core.CallAndMessage): prior QVERIFY2 aborts on null.
        QVERIFY2(!button->isHidden(), "indicator must surface while a sort is active");
        QVERIFY2(
            button->toolTip().contains(QStringLiteral("\"category\"")), "initial tooltip must name the sorted column"
        );

        // Rename the column and emit the same
        // `NotifyColumnEdited` the column editor would fire.
        // The tooltip must catch the new label without any
        // sort / filter event in between.
        auto *model = mWindow->Model();
        model->MutableLayout().SetColumnHeader(static_cast<size_t>(categoryCol), std::string("renamed-cat"));
        model->NotifyColumnEdited(categoryCol);
        QCoreApplication::processEvents();

        QVERIFY2(
            button->toolTip().contains(QStringLiteral("\"renamed-cat\"")),
            "tooltip must reflect the renamed column without waiting for a sort/filter event"
        );
        QVERIFY2(
            !button->toolTip().contains(QStringLiteral("\"category\"")),
            "tooltip must not still carry the stale column label"
        );
        // NOLINTEND(clang-analyzer-core.CallAndMessage)
    }

    // An enum column with an empty dictionary must show the placeholder
    // and disable OK so a "hide everything" filter cannot be submitted.
    void TestFilterEditorEmptyEnumPickerDisablesOk()
    {
        // Configure `level` as enum without streaming so the dict stays empty.
        auto *model = mWindow->findChild<LogModel *>();
        QVERIFY2(model != nullptr, "MainWindow must own a LogModel");

        const QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString cfgPath = dir.filePath("empty_enum.json");
        // Save through the manager so the JSON matches the live schema,
        // then load it back into the model.
        {
            loglib::ColumnLayout scratch;
            scratch.AppendKeys({"category"});
            scratch.SetColumnTypePair(0, loglib::ColumnType::Enumeration, false);
            loglib::SaveConfiguration(
                loglib::LogConfiguration{.columns = scratch.Columns()}, cfgPath.toStdString(), loglib::SaveScope::Full
            );
        }
        LoadLayoutForTest(*model, cfgPath.toStdString());

        QCOMPARE(model->Layout().Columns().size(), static_cast<size_t>(1));
        QCOMPARE(model->Layout().Columns()[0].type, loglib::ColumnType::Enumeration);

        const FilterEditor editor(*model, QStringLiteral("test-empty-enum"));
        // `UpdateSelectedColumn(0)` settles the OK / placeholder state.

        const auto *okButton = editor.findChild<QPushButton *>(QStringLiteral("okButton"));
        QVERIFY2(okButton != nullptr, "FilterEditor must expose an OK button");
        // clang-analyzer does not model `QVERIFY2`'s test-aborting behaviour,
        // so it still considers `okButton` potentially null on the next line.
        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage)
        QVERIFY2(!okButton->isEnabled(), "OK must be disabled when the picker dictionary is empty");

        const auto *placeholder = editor.findChild<QLabel *>(QStringLiteral("enumEmptyPlaceholder"));
        QVERIFY2(placeholder != nullptr, "FilterEditor must expose the empty-picker placeholder");
        QVERIFY2(
            placeholder->text().contains(QStringLiteral("No values observed"), Qt::CaseInsensitive),
            "placeholder text must explain why the picker is empty"
        );
        // Use `!isHidden()`: `isVisible()` requires shown ancestors.
        QVERIFY2(!placeholder->isHidden(), "placeholder must not be explicitly hidden when the picker is empty");
    }

    // A saved enum filter with empty `filterValues` is "hide everything";
    // drop it at AddFilter time rather than opening a doomed editor.
    void TestSavedEmptyEnumFilterIsDropped()
    {
        // Promote `level` so the filter is type-compatible.
        const QStringList levels{
            QStringLiteral("info"), QStringLiteral("warn"), QStringLiteral("error"), QStringLiteral("debug")
        };
        QStringList lines;
        lines.reserve(320);
        for (int i = 0; i < 320; ++i)
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

        const bool finished = finishedSpy.count() > 0 || finishedSpy.wait(60000);
        QVERIFY2(finished, "streamingFinished must arrive within the timeout");
        QCOMPARE(model->rowCount(), 320);

        const int levelCol = ColumnByHeader(*model, QStringLiteral("category"));
        QVERIFY2(levelCol >= 0, "level column must exist");
        QCOMPARE(model->Layout().Columns()[static_cast<size_t>(levelCol)].type, loglib::ColumnType::Enumeration);

        loglib::LeafRule savedFilter;
        savedFilter.type = loglib::LeafRule::Type::Enumeration;
        savedFilter.columnKeys = model->Layout().Columns()[static_cast<size_t>(levelCol)].keys;

        mWindow->statusBar()->clearMessage();
        const QString filterId = QStringLiteral("saved-empty-enum");
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
            message.contains(QStringLiteral("had no values selected"), Qt::CaseInsensitive),
            qPrintable(QStringLiteral("expected status-bar 'no values selected' message; got '%1'").arg(message))
        );

        // Unfiltered total: the empty enum filter never became active.
        const LogFilterModel *filterModel = mWindow->FilterModel();
        QVERIFY(filterModel != nullptr);
        QCOMPARE(filterModel->rowCount(), 320);

        const auto *menu = mWindow->findChild<QMenu *>(QStringLiteral("menuFilters"));
        QVERIFY(menu != nullptr);
        for (const QAction *action : menu->actions())
        {
            QVERIFY2(action->data().toString() != filterId, "dropped enum filter must not have a menu entry");
        }

        // No editor opens; filter is dropped before construction.
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

    // `enumColumnsChanged` must fire when a column auto-promotes mid-stream.
    void TestEnumColumnsChangedFiresOnPromotion()
    {
        // 4 levels across 320 rows; well past the promotion threshold.
        const QStringList levels{
            QStringLiteral("info"), QStringLiteral("warn"), QStringLiteral("error"), QStringLiteral("debug")
        };
        QStringList lines;
        lines.reserve(320);
        for (int i = 0; i < 320; ++i)
        {
            lines.append(QStringLiteral(R"({"category": "%1"})").arg(levels[i % levels.size()]));
        }
        const TempJsonFile fixture(lines);

        auto *model = mWindow->findChild<LogModel *>();
        QVERIFY2(model != nullptr, "MainWindow must own a LogModel");

        const QSignalSpy enumSpy(model, &LogModel::enumColumnsChanged);
        QVERIFY(enumSpy.isValid());
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

        const bool finished = finishedSpy.count() > 0 || finishedSpy.wait(60000);
        QVERIFY2(finished, "streamingFinished must arrive within the timeout");
        QCOMPARE(model->rowCount(), 320);

        const int levelCol = ColumnByHeader(*model, QStringLiteral("category"));
        QVERIFY2(levelCol >= 0, "level column must exist");
        const auto &columns = model->Layout().Columns();
        QCOMPARE(columns[static_cast<size_t>(levelCol)].type, loglib::ColumnType::Enumeration);

        // Don't pin an exact count: dictionary growth may re-fire.
        QVERIFY2(
            enumSpy.count() >= 1,
            qPrintable(
                QStringLiteral("enumColumnsChanged should have fired at least once; got %1").arg(enumSpy.count())
            )
        );

        model->EndStreaming(false);
    }

    // Regression: a saved `Type::String` filter against a now-
    // `Boolean` column drops at `AddFilter` time -- same way it would
    // for an enum-promoted column. Covers the type-match branch
    // added with `Boolean`.
    void TestSavedStringFilterDroppedOnNowBooleanColumn()
    {
        QStringList lines;
        lines.reserve(80);
        for (int i = 0; i < 80; ++i)
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
        QCOMPARE(model->Layout().Columns()[static_cast<size_t>(flagCol)].type, loglib::ColumnType::Boolean);

        loglib::LeafRule savedFilter;
        savedFilter.type = loglib::LeafRule::Type::String;
        savedFilter.columnKeys = model->Layout().Columns()[static_cast<size_t>(flagCol)].keys;
        savedFilter.filterString = "true";
        savedFilter.matchType = loglib::LeafRule::Match::Exactly;

        mWindow->statusBar()->clearMessage();
        const QString filterId = QStringLiteral("saved-string-on-bool");
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

        const LogFilterModel *filterModel = mWindow->FilterModel();
        QVERIFY(filterModel != nullptr);
        QCOMPARE(filterModel->rowCount(), 80);

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

    // Regression: a saved `Type::String` filter against a numeric
    // column drops at `AddFilter` time. Same shape as the now-Boolean
    // / now-Enum variants; covers the numeric type-match branch.
    void TestSavedStringFilterDroppedOnNowNumericColumn()
    {
        QStringList lines;
        lines.reserve(100);
        for (int i = 0; i < 100; ++i)
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
        QCOMPARE(model->Layout().Columns()[static_cast<size_t>(valueCol)].type, loglib::ColumnType::Integer);

        loglib::LeafRule savedFilter;
        savedFilter.type = loglib::LeafRule::Type::String;
        savedFilter.columnKeys = model->Layout().Columns()[static_cast<size_t>(valueCol)].keys;
        savedFilter.filterString = "42";
        savedFilter.matchType = loglib::LeafRule::Match::Exactly;

        mWindow->statusBar()->clearMessage();
        const QString filterId = QStringLiteral("saved-string-on-numeric");
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

        const LogFilterModel *filterModel = mWindow->FilterModel();
        QVERIFY(filterModel != nullptr);
        QCOMPARE(filterModel->rowCount(), 100);

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

    void TestEnumFilterUpgradesToFastPathOnColumnPromotion()
    {
        auto *model = mWindow->findChild<LogModel *>();
        QVERIFY2(model != nullptr, "MainWindow must own a LogModel");
        auto *filterModel = mWindow->FilterModel();
        QVERIFY2(filterModel != nullptr, "MainWindow must own a LogFilterModel");

        const TempJsonFile emptyFixture(QStringList{});
        auto fileSource = std::make_unique<loglib::FileLineSource>(emptyFixture.Path().toStdString());
        loglib::FileLineSource *sourcePtr = fileSource.get();
        (void)model->BeginStreamingForSyncTest(std::move(fileSource));

        loglib::KeyIndex &keys = model->Table().Keys();
        const auto makeLine = [&](const std::string &value) {
            std::vector<std::pair<loglib::KeyId, loglib::LogValue>> values;
            values.emplace_back(keys.GetOrInsert("mycol"), loglib::LogValue(value));
            return loglib::LogLine{std::move(values), *sourcePtr, 0};
        };

        // Batch 1: one row keeps `mycol` below
        // STREAM_PROMOTION_MIN_ROWS (== 2), so the column stays a
        // candidate (`Type::Any + autoDetect`) and
        // `ResolveEnumDictionary` returns nullptr at filter-install
        // time.
        {
            loglib::RowBatch batch;
            batch.firstLineNumber = 1;
            batch.newKeys.emplace_back("mycol");
            batch.lines.push_back(makeLine("alpha"));
            model->AppendBatch(std::move(batch));
        }
        QCoreApplication::processEvents();

        const int col = ColumnByHeader(*model, QStringLiteral("mycol"));
        QVERIFY2(col >= 0, "mycol column must exist after the first batch");
        QCOMPARE(model->Layout().Columns()[static_cast<size_t>(col)].type, loglib::ColumnType::Any);
        QVERIFY(model->Layout().Columns()[static_cast<size_t>(col)].autoDetect);

        // Install an enum filter while the column is still a candidate:
        // predicate built with `dictionary = nullptr`, only the slow
        // string-set fallback path is available.
        const QString filterId = QStringLiteral("upgrade-on-promote");
        QVERIFY2(
            QMetaObject::invokeMethod(
                mWindow,
                "FilterEnumSubmitted",
                Qt::DirectConnection,
                Q_ARG(QString, filterId),
                Q_ARG(int, col),
                Q_ARG(QStringList, QStringList{QStringLiteral("alpha")})
            ),
            "FilterEnumSubmitted slot must be invocable via meta-object"
        );
        QCoreApplication::processEvents();
        QCOMPARE(filterModel->rowCount(), 1); // matched via the string-set fallback.

        // Spy after install so only post-install rebuilds count. The
        // only `layoutChanged` path on `LogFilterModel` is
        // `RebuildAcceptedRows` (via `SetFilterRules` / sort), so
        // any post-promote emission means `UpdateFilters` re-armed
        // the predicate.
        const QSignalSpy layoutChangedSpy(filterModel, &QAbstractItemModel::layoutChanged);
        QVERIFY(layoutChangedSpy.isValid());
        const QSignalSpy enumChangedSpy(model, &LogModel::enumColumnsChanged);
        QVERIFY(enumChangedSpy.isValid());

        // Batch 2: a second row pushes `presenceCount` to 2 and
        // trips streaming promotion. The slot must rebuild the
        // predicate to upgrade it to the bitset path.
        {
            loglib::RowBatch batch;
            batch.firstLineNumber = 2;
            batch.lines.push_back(makeLine("alpha"));
            model->AppendBatch(std::move(batch));
        }
        QCoreApplication::processEvents();

        QCOMPARE(model->Layout().Columns()[static_cast<size_t>(col)].type, loglib::ColumnType::Enumeration);
        bool sawPromoted = false;
        for (const auto &args : enumChangedSpy)
        {
            if (args.at(0).value<EnumColumnsChangeReason>() == EnumColumnsChangeReason::Promoted)
            {
                sawPromoted = true;
                break;
            }
        }
        QVERIFY2(sawPromoted, "promotion batch must emit a `Promoted` reason");

        QVERIFY2(
            layoutChangedSpy.count() >= 1,
            qPrintable(QStringLiteral(
                           "filter must be rebuilt on Promoted to upgrade to the fast path; "
                           "expected layoutChanged >= 1, got %1"
            )
                           .arg(layoutChangedSpy.count()))
        );

        // Sanity: the rebuilt fast-path predicate still matches both rows.
        QCOMPARE(filterModel->rowCount(), 2);

        model->EndStreaming(false);
    }

    // `enumColumnsChanged(Promoted, columnIndex)` only rebuilds
    // filters that target the promoted column.
    void TestEnumPromotedOnUnrelatedColumnDoesNotRebuildFilters()
    {
        auto *model = mWindow->findChild<LogModel *>();
        QVERIFY2(model != nullptr, "MainWindow must own a LogModel");
        auto *filterModel = mWindow->FilterModel();
        QVERIFY2(filterModel != nullptr, "MainWindow must own a LogFilterModel");

        const TempJsonFile emptyFixture(QStringList{});
        auto fileSource = std::make_unique<loglib::FileLineSource>(emptyFixture.Path().toStdString());
        loglib::FileLineSource *sourcePtr = fileSource.get();
        (void)model->BeginStreamingForSyncTest(std::move(fileSource));

        loglib::KeyIndex &keys = model->Table().Keys();
        const auto makeColARow = [&](const std::string &value) {
            std::vector<std::pair<loglib::KeyId, loglib::LogValue>> values;
            values.emplace_back(keys.GetOrInsert("colA"), loglib::LogValue(value));
            return loglib::LogLine{std::move(values), *sourcePtr, 0};
        };
        const auto makeColBRow = [&](const std::string &value) {
            std::vector<std::pair<loglib::KeyId, loglib::LogValue>> values;
            values.emplace_back(keys.GetOrInsert("colB"), loglib::LogValue(value));
            return loglib::LogLine{std::move(values), *sourcePtr, 0};
        };

        // Batch 1: promote colA to Enumeration via the streaming
        // threshold (`presenceCount >= 2`). colB doesn't appear so
        // it stays a candidate (`Type::Any + autoDetect`) until
        // batch 2 introduces it.
        {
            loglib::RowBatch batch;
            batch.firstLineNumber = 1;
            batch.newKeys.emplace_back("colA");
            batch.lines.push_back(makeColARow("alpha"));
            batch.lines.push_back(makeColARow("alpha"));
            model->AppendBatch(std::move(batch));
        }
        QCoreApplication::processEvents();

        const int colA = ColumnByHeader(*model, QStringLiteral("colA"));
        QVERIFY2(colA >= 0, "colA must exist after batch 1");
        QCOMPARE(model->Layout().Columns()[static_cast<size_t>(colA)].type, loglib::ColumnType::Enumeration);

        // Install an enum filter on colA only. Predicate is built
        // with the live dictionary so it's already on the fast path.
        const QString filterId = QStringLiteral("scoped-promote-test");
        QVERIFY2(
            QMetaObject::invokeMethod(
                mWindow,
                "FilterEnumSubmitted",
                Qt::DirectConnection,
                Q_ARG(QString, filterId),
                Q_ARG(int, colA),
                Q_ARG(QStringList, QStringList{QStringLiteral("alpha")})
            ),
            "FilterEnumSubmitted slot must be invocable via meta-object"
        );
        QCoreApplication::processEvents();

        // Spy after install so only post-install rebuilds count.
        const QSignalSpy layoutChangedSpy(filterModel, &QAbstractItemModel::layoutChanged);
        QVERIFY(layoutChangedSpy.isValid());

        // Batch 2: introduce colB with 2 same-value rows so it
        // promotes via the streaming threshold. colA does not
        // appear in this batch so its dictionary is untouched.
        {
            loglib::RowBatch batch;
            batch.firstLineNumber = 3;
            batch.newKeys.emplace_back("colB");
            batch.lines.push_back(makeColBRow("x"));
            batch.lines.push_back(makeColBRow("x"));
            model->AppendBatch(std::move(batch));
        }
        QCoreApplication::processEvents();

        const int colB = ColumnByHeader(*model, QStringLiteral("colB"));
        QVERIFY2(colB >= 0, "colB must exist after batch 2");
        QCOMPARE(model->Layout().Columns()[static_cast<size_t>(colB)].type, loglib::ColumnType::Enumeration);
        QVERIFY2(colA != colB, "colA and colB must be distinct columns for the scoping test");

        // The filter on colA must NOT have been rebuilt. The only
        // `layoutChanged` source on `LogFilterModel` is
        // `RebuildAcceptedRows` (via `SetFilterRules`), so a count
        // of 0 confirms the slot short-circuited.
        QVERIFY2(
            layoutChangedSpy.count() == 0,
            qPrintable(
                QStringLiteral(
                    "filter on colA must not be rebuilt when an unrelated column promotes; got layoutChanged count %1"
                )
                    .arg(layoutChangedSpy.count())
            )
        );

        model->EndStreaming(false);
    }

    // Regression: a pre-existing enum column whose dict only grew in
    // a batch emits `Grew` exactly once and never `Promoted`.
    // the batch result backfill range is a min/max over every back-filled
    // column, so a grow-only column can fall inside it; the
    // back-fill loop must filter rather than firing `Promoted`
    // for every enum column in the range.
    //
    // Setup:
    //   * batch 1 leaves `colA` as a candidate (`Type::Any +
    //     autoDetect`) and promotes `colB`.
    //   * batch 2 promotes `colA` (low index, back-filled), grows
    //     `colB`'s dict (no back-fill), and promotes a new `colC`
    //     (high index, back-filled). Range spans `[colA, colC]`
    //     with `colB` sandwiched in the middle.
    void TestEnumGrewSignalNotDoubledAsPromotedWhenSandwichedInBackfillRange()
    {
        auto *model = mWindow->findChild<LogModel *>();
        QVERIFY2(model != nullptr, "MainWindow must own a LogModel");

        const TempJsonFile emptyFixture(QStringList{});
        auto fileSource = std::make_unique<loglib::FileLineSource>(emptyFixture.Path().toStdString());
        loglib::FileLineSource *sourcePtr = fileSource.get();
        (void)model->BeginStreamingForSyncTest(std::move(fileSource));

        loglib::KeyIndex &keys = model->Table().Keys();
        const auto makeRow = [&](const std::vector<std::pair<std::string, std::string>> &kvs) {
            std::vector<std::pair<loglib::KeyId, loglib::LogValue>> values;
            values.reserve(kvs.size());
            for (const auto &[k, v] : kvs)
            {
                values.emplace_back(keys.GetOrInsert(k), loglib::LogValue(v));
            }
            return loglib::LogLine{std::move(values), *sourcePtr, 0};
        };

        // Batch 1: `colA` gets exactly one presence (stays a
        // candidate, tracker carries 1 presence forward). `colB`
        // gets two identical presences and promotes to Enumeration
        // with dict size 1. `colA` is appended first so it lands at
        // column 0.
        {
            loglib::RowBatch batch;
            batch.firstLineNumber = 1;
            batch.newKeys.emplace_back("colA");
            batch.newKeys.emplace_back("colB");
            batch.lines.push_back(makeRow({{"colA", "x"}, {"colB", "y"}}));
            batch.lines.push_back(makeRow({{"colB", "y"}}));
            model->AppendBatch(std::move(batch));
        }
        QCoreApplication::processEvents();

        const int colA = ColumnByHeader(*model, QStringLiteral("colA"));
        const int colB = ColumnByHeader(*model, QStringLiteral("colB"));
        QVERIFY2(colA == 0, "colA must land at column 0 (first appended new key)");
        QVERIFY2(colB == 1, "colB must land at column 1 (second appended new key)");
        QCOMPARE(model->Layout().Columns()[static_cast<size_t>(colA)].type, loglib::ColumnType::Any);
        QVERIFY(model->Layout().Columns()[static_cast<size_t>(colA)].autoDetect);
        QCOMPARE(model->Layout().Columns()[static_cast<size_t>(colB)].type, loglib::ColumnType::Enumeration);
        const loglib::KeyId colBKey = keys.Find("colB");
        QVERIFY(colBKey != loglib::INVALID_KEY_ID);
        const loglib::EnumDictionary *colBDict = model->Table().EnumDictionaries().Find(colBKey);
        QVERIFY2(colBDict != nullptr, "colB must have a registry entry after batch 1");
        QCOMPARE(static_cast<int>(colBDict->Size()), 1);

        // Spy after batch 1 so only batch-2 emissions count.
        QSignalSpy enumChangedSpy(model, &LogModel::enumColumnsChanged);
        QVERIFY(enumChangedSpy.isValid());

        // Batch 2 sandwiches `colB` between two recorded back-fills:
        //   * `colA` promotes (presence count crosses 2): recordBackfill(0).
        //   * `colB` dict grows from 1 to 2 (`y` -> `y`, `w`): no back-fill.
        //   * `colC` promotes (2 same-value presences): recordBackfill(2).
        // the batch result backfill range becomes `[0, 2]` with `colB` at column 1.
        {
            loglib::RowBatch batch;
            batch.firstLineNumber = 3;
            batch.newKeys.emplace_back("colC");
            batch.lines.push_back(makeRow({{"colA", "z"}, {"colB", "w"}, {"colC", "m"}}));
            batch.lines.push_back(makeRow({{"colA", "z"}, {"colB", "w"}, {"colC", "m"}}));
            model->AppendBatch(std::move(batch));
        }
        QCoreApplication::processEvents();

        const int colC = ColumnByHeader(*model, QStringLiteral("colC"));
        QVERIFY2(colC == 2, "colC must land at column 2 (only new key in batch 2)");
        QCOMPARE(model->Layout().Columns()[static_cast<size_t>(colA)].type, loglib::ColumnType::Enumeration);
        QCOMPARE(model->Layout().Columns()[static_cast<size_t>(colB)].type, loglib::ColumnType::Enumeration);
        QCOMPARE(model->Layout().Columns()[static_cast<size_t>(colC)].type, loglib::ColumnType::Enumeration);
        const loglib::EnumDictionary *colBDictAfter = model->Table().EnumDictionaries().Find(colBKey);
        QVERIFY2(colBDictAfter != nullptr, "colB must remain enum across batch 2");
        QCOMPARE(static_cast<int>(colBDictAfter->Size()), 2);

        // colB is the sandwiched pre-existing enum: exactly one
        // `Grew`, zero `Promoted`.
        int colBGrewCount = 0;
        int colBPromotedCount = 0;
        int colBDemotedCount = 0;
        for (const auto &args : enumChangedSpy)
        {
            const auto reason = args.at(0).value<EnumColumnsChangeReason>();
            const int columnIndex = args.at(1).toInt();
            if (columnIndex != colB)
            {
                continue;
            }
            switch (reason)
            {
            case EnumColumnsChangeReason::Grew:
                ++colBGrewCount;
                break;
            case EnumColumnsChangeReason::Promoted:
                ++colBPromotedCount;
                break;
            case EnumColumnsChangeReason::Demoted:
                ++colBDemotedCount;
                break;
            }
        }
        QCOMPARE(colBGrewCount, 1);
        QVERIFY2(
            colBPromotedCount == 0,
            qPrintable(QStringLiteral("colB (already enum at snapshot) must NOT receive a spurious `Promoted`; got %1")
                           .arg(colBPromotedCount))
        );
        QCOMPARE(colBDemotedCount, 0);

        // Genuinely new enum columns still get exactly one
        // `Promoted` each, so receivers can rebuild their filters.
        const auto countReasonForColumn = [&](int columnIndex, EnumColumnsChangeReason wanted) {
            int count = 0;
            for (const auto &args : enumChangedSpy)
            {
                if (args.at(1).toInt() == columnIndex && args.at(0).value<EnumColumnsChangeReason>() == wanted)
                {
                    ++count;
                }
            }
            return count;
        };
        QCOMPARE(countReasonForColumn(colA, EnumColumnsChangeReason::Promoted), 1);
        QCOMPARE(countReasonForColumn(colC, EnumColumnsChangeReason::Promoted), 1);

        model->EndStreaming(false);
    }

    // The View-menu action and the dock's title-bar X stay in sync via
    // `QDockWidget::visibilityChanged`.
    void TestRecordDetailDockViewMenuTogglesVisibility()
    {
        QAction *toggleAction = FindActionByObjectName(mWindow, QStringLiteral("actionToggleRecordDetails"));
        QVERIFY2(toggleAction != nullptr, "actionToggleRecordDetails must be wired");
        QVERIFY(toggleAction->isCheckable());

        auto *dock = mWindow->findChild<RecordDetailDock *>();
        QVERIFY2(dock != nullptr, "Record Details dock must be owned by MainWindow");
        // clang-analyzer doesn't model `QVERIFY2`'s fail-fast return.
        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage)
        QVERIFY2(dock->isHidden(), "dock starts hidden");
        QVERIFY(!toggleAction->isChecked());

        // Action -> dock: when the host window is not realised
        // (offscreen QPA), the handler reverts the action so a
        // checked toggle always means a visible dock.
        toggleAction->setChecked(true);
        QVERIFY2(
            !toggleAction->isChecked(),
            "setChecked(true) must be reverted when host window is not realised (offscreen QPA)"
        );

        // Dock -> action direction: drive `visibilityChanged`
        // synthetically (see the namespace note above for why).
        emit dock->visibilityChanged(false);
        QVERIFY2(!toggleAction->isChecked(), "visibilityChanged(false) must un-check the action");

        emit dock->visibilityChanged(true);
        QVERIFY2(toggleAction->isChecked(), "visibilityChanged(true) must re-check the action");
    }

    // `columnsMoved` (header drag, streaming `Time` bubble) refreshes
    // the Field/Value list. Driven via the public `MoveColumn` -- in
    // Qt 6 `columnsMoved` is `QPrivateSignal`, so external `emit`
    // doesn't compile. `columnsInserted` shares the same lambda and
    // is exercised indirectly by the streaming tests.
    void TestRecordDetailDockRefreshesOnColumnsMoved()
    {
        // Three keys so we can move two non-zero columns and keep the
        // pinned column-0 persistent index trivially valid.
        const TempJsonFile fixture(
            QStringList{
                QStringLiteral(R"({"a": "alpha", "b": "1", "c": "x"})"),
                QStringLiteral(R"({"a": "beta",  "b": "2", "c": "y"})"),
            }
        );
        const StreamingRun run = RunStreaming(fixture.Path());
        QCOMPARE(run.model->rowCount(), 2);
        QVERIFY(run.model->columnCount() >= 3);

        RecordDetailDock dock(run.model.get());
        // Drive the visibility gate via the signal (see namespace note).
        emit dock.visibilityChanged(true);
        dock.ShowSourceRow(0);
        QVERIFY(dock.Widget()->Content().valid);
        const int refreshesAfterPin = dock.RefreshCountForTest();

        const int cols = run.model->columnCount();
        const int src = cols - 1;
        const int dest = cols - 2;
        QVERIFY(src != dest);
        QVERIFY2(src >= 1 && dest >= 1, "Move must skip column 0 so the pin stays trivially valid");
        QVERIFY(run.model->MoveColumn(src, dest));
        QCoreApplication::processEvents();
        QCOMPARE(dock.RefreshCountForTest(), refreshesAfterPin + 1);

        // Hidden gate applies here too -- invisible dock skips the rebuild.
        emit dock.visibilityChanged(false);
        const int refreshesAfterHide = dock.RefreshCountForTest();
        // NOLINTNEXTLINE(readability-suspicious-call-argument): intentional swap to restore the original column order.
        QVERIFY(run.model->MoveColumn(dest, src));
        QCoreApplication::processEvents();
        QCOMPARE(dock.RefreshCountForTest(), refreshesAfterHide);
    }

    // End-to-end: `QAction::trigger` (what the shortcut map calls
    // after resolving Ctrl+I) toggles the dock's visibility. A
    // regression that severs the `toggled -> setVisible` connect
    // would slip past the cold-launch test but fail this one. We
    // don't post a `QKeyEvent` because `WindowShortcut` requires
    // window activation, which offscreen QPA doesn't realise.
    void TestRecordDetailToggleActionTogglesDockVisibility()
    {
        QAction *toggleAction = FindActionByObjectName(mWindow, QStringLiteral("actionToggleRecordDetails"));
        QVERIFY2(toggleAction != nullptr, "actionToggleRecordDetails must be wired");
        QVERIFY(toggleAction->isCheckable());

        auto *dock = mWindow->findChild<RecordDetailDock *>();
        QVERIFY(dock != nullptr);
        QVERIFY2(dock->isHidden(), "dock starts hidden");
        QVERIFY(!toggleAction->isChecked());

        // The handler reverts under offscreen QPA, so the action
        // stays unchecked across triggers. Live visibility is
        // covered by manual QA; this guards against the action
        // settling "checked while dock hidden".
        toggleAction->trigger();
        QVERIFY2(
            !toggleAction->isChecked(),
            "trigger() must auto-revert under offscreen QPA: action cannot settle checked while dock cannot show"
        );

        toggleAction->trigger();
        QVERIFY2(
            !toggleAction->isChecked(),
            "second trigger must also revert under offscreen QPA -- the action stays unchecked until the host is "
            "realised"
        );
    }
};

QTEST_MAIN(ColumnsTest)
#include "apptest_columns_test.moc"
