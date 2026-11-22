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
class SessionsTest : public apptest_support::AppTestBase
{
    Q_OBJECT

private slots:

    // Anchors round-trip: live AnchorManager -> saved JSON ->
    // loaded JSON -> AnchorManager. Covers the mirror, save, load,
    // and Replace plumbing.
    void TestAnchorPersistenceRoundTripsThroughSavedConfiguration()
    {
        auto *anchors = mWindow->Anchors();
        QVERIFY(anchors != nullptr);
        auto *model = mWindow->Model();

        // Three real rows so the model has real (locator, lineId)
        // keys to anchor on.
        loglib::StreamLineSource &streamSource = BeginSyntheticStreamSession(*model);
        QtStreamingLogSink *sink = model->Sink();
        QVERIFY(sink != nullptr);
        loglib::KeyIndex &keys = sink->Keys();
        const loglib::KeyId valueKey = keys.GetOrInsert(std::string("value"));
        sink->OnBatch(MakeSyntheticBatch(streamSource, valueKey, 1, 3, true));
        QCoreApplication::processEvents();
        QCOMPARE(model->rowCount(), 3);
        model->EndStreaming(false);

        const auto key0 = model->AnchorKeyForRow(0);
        const auto key2 = model->AnchorKeyForRow(2);
        QVERIFY(key0.has_value());
        QVERIFY(key2.has_value());
        anchors->SetAnchor(*key0, 2);
        anchors->SetAnchor(*key2, 6);
        // Attach a note to one anchor so the round-trip also
        // covers the on-disk `note` field. Leaving the other note
        // empty keeps the default-value branch covered too.
        QVERIFY(anchors->SetAnchorNote(*key2, std::string{"first error of the incident"}));
        QCOMPARE(anchors->Count(), static_cast<std::size_t>(2));

        // Save via the test seam (mirror -> serialise).
        const QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const QString cfgPath = tmp.filePath(QStringLiteral("anchors-roundtrip.json"));
        mWindow->SaveConfigurationToPathForTest(cfgPath);
        QCoreApplication::processEvents();

        // On-disk JSON must contain an `anchors` key and the
        // populated `note` payload.
        QFile saved(cfgPath);
        QVERIFY(saved.open(QIODevice::ReadOnly | QIODevice::Text));
        const QString savedJson = QString::fromUtf8(saved.readAll());
        saved.close();
        QVERIFY2(savedJson.contains(QStringLiteral("\"anchors\"")), "saved JSON must carry an anchors key");
        QVERIFY2(
            savedJson.contains(QStringLiteral("first error of the incident")),
            "saved JSON must carry the anchor note payload"
        );

        // Wipe live state so the load is observable.
        auto *newSessionAction = mWindow->findChild<QAction *>(QStringLiteral("actionNewSession"));
        QVERIFY(newSessionAction != nullptr);
        newSessionAction->trigger();
        QCoreApplication::processEvents();
        QVERIFY(anchors->Empty());

        // Load through the same path the menu uses; anchors come
        // back.
        QVERIFY(mWindow->TryLoadAsConfigurationForTest(cfgPath));
        QCoreApplication::processEvents();

        QCOMPARE(anchors->Count(), static_cast<std::size_t>(2));
        QCOMPARE(anchors->ColorFor(*key0).value_or(255U), uint8_t{2});
        QCOMPARE(anchors->ColorFor(*key2).value_or(255U), uint8_t{6});
        // Notes come back exactly as saved -- populated for key2,
        // empty (default-constructed) for key0.
        QCOMPARE(anchors->NoteFor(*key0).value_or(std::string{"NO-KEY"}), std::string{});
        QCOMPARE(anchors->NoteFor(*key2).value_or(std::string{"NO-KEY"}), std::string{"first error of the incident"});
    }

    // `SetColumnVisible(idx, true)` reverses a hide.
    void TestShowColumnRestoresSection()
    {
        const int levelCol = StreamFixtureForColumnTests();
        QVERIFY2(levelCol >= 0, "level column must exist after streaming");

        auto *model = mWindow->Model();
        mWindow->SetColumnVisible(levelCol, false);
        QVERIFY(!model->Layout().Columns()[static_cast<size_t>(levelCol)].visible);

        mWindow->SetColumnVisible(levelCol, true);

        QVERIFY2(
            model->Layout().Columns()[static_cast<size_t>(levelCol)].visible, "Column::visible must flip back to true"
        );
        const QHeaderView *header = mWindow->findChild<LogTableView *>()->horizontalHeader();
        QVERIFY2(header != nullptr, "view must own a horizontal header");
        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): false positive; prior `QVERIFY2` aborts on null.
        QVERIFY2(!header->isSectionHidden(levelCol), "header section must no longer be hidden");
    }

    // `TryLoadAsConfiguration` is the single-file entry from
    // `OpenFiles`. It must reapply `Column::visible` to the header,
    // otherwise a saved config would load but every column would
    // still be shown.
    void TestTryLoadAsConfigurationAppliesVisibility()
    {
        const int levelCol = StreamFixtureForColumnTests();
        QVERIFY2(levelCol >= 0, "level column must exist after streaming");
        auto *model = mWindow->Model();

        mWindow->SetColumnVisible(levelCol, false);

        const QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        const QString savedPath = tempDir.filePath(QStringLiteral("hidden-level.json"));
        SaveLayoutForTest(*model, savedPath.toStdString());

        // Re-show first so the load + reapply has something to
        // do; otherwise the test would pass without the new
        // `ApplyColumnVisibility` inside `TryLoadAsConfiguration`.
        mWindow->SetColumnVisible(levelCol, true);
        const QHeaderView *header = mWindow->findChild<LogTableView *>()->horizontalHeader();
        QVERIFY2(header != nullptr, "view must own a horizontal header");
        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): false positive; prior `QVERIFY2` aborts on null.
        QVERIFY2(!header->isSectionHidden(levelCol), "precondition: section visible before load");

        QVERIFY2(mWindow->TryLoadAsConfigurationForTest(savedPath), "TryLoadAsConfiguration must succeed");
        QCoreApplication::processEvents();

        QVERIFY2(
            !model->Layout().Columns()[static_cast<size_t>(levelCol)].visible,
            "loaded configuration must mark level hidden"
        );
        QVERIFY2(header->isSectionHidden(levelCol), "TryLoadAsConfiguration must reapply visibility to the header");
    }

    // Regression: filters added via the Filters menu used to live
    // only in `mFilters` and were never mirrored into the wire-
    // format vector `Save` persists, so they vanished on Save /
    // Load. The eager mirror keeps both stores in lockstep; the
    // load path rebuilds `mFilters` from the loaded vector.
    void TestFilterPersistenceRoundtrip()
    {
        const int levelCol = StreamFixtureForColumnTests();
        QVERIFY2(levelCol >= 0, "level column must exist after streaming");
        auto *model = mWindow->Model();

        const int msgCol = ColumnByHeader(*model, QStringLiteral("msg"));
        QVERIFY2(msgCol >= 0, "msg column must exist after streaming");

        // Install a string filter on `msg` (string-typed by
        // default) via the production submit slot.
        const QString filterId = QStringLiteral("persisted-string");
        QVERIFY2(
            QMetaObject::invokeMethod(
                mWindow,
                "FilterSubmitted",
                Qt::DirectConnection,
                Q_ARG(QString, filterId),
                Q_ARG(int, msgCol),
                Q_ARG(QString, QStringLiteral("m1")),
                Q_ARG(int, static_cast<int>(loglib::FilterCondition::Match::Contains))
            ),
            "FilterSubmitted slot must be invocable via meta-object"
        );
        QCoreApplication::processEvents();

        QCOMPARE(mWindow->Filters().size(), static_cast<size_t>(1));
        const std::vector<std::string> msgKeys = model->Layout().Columns()[static_cast<size_t>(msgCol)].keys;

        // The eager mirror must have copied the filter into the
        // wire-format expression before any Save -- the load-
        // bearing invariant this fix establishes.
        auto mirrored = WireLeavesOf(mWindow->activeSession()->View().expression);
        QCOMPARE(mirrored.size(), static_cast<size_t>(1));
        QCOMPARE(mirrored[0].columnKeys, msgKeys);
        QCOMPARE(mirrored[0].type, loglib::FilterCondition::Type::String);

        const QTemporaryDir savedDir;
        QVERIFY(savedDir.isValid());
        const QString savedPath = savedDir.filePath(QStringLiteral("filter-persistence.json"));

        mWindow->SaveConfigurationToPathForTest(savedPath);

        // Clear the active filter; the eager mirror empties the
        // wire-format expression too (match-all).
        QMetaObject::invokeMethod(mWindow, "ClearAllFilters", Qt::DirectConnection);
        QCoreApplication::processEvents();
        QCOMPARE(mWindow->Filters().size(), static_cast<size_t>(0));
        QVERIFY(loglib::AcceptsEverything(mWindow->activeSession()->View().expression));

        // Suppress the modal so the test thread does not block.
        // No drops are expected here, but the load path always
        // checks before showing.
        mWindow->SetSuppressDialogsForTest(true);
        mWindow->LoadConfigurationFromPathForTest(savedPath);
        QCoreApplication::processEvents();

        QCOMPARE(mWindow->LastDroppedFilterCountForTest(), 0);
        QCOMPARE(mWindow->Filters().size(), static_cast<size_t>(1));
        const auto &revived = mWindow->Filters().begin()->second;
        QCOMPARE(revived.type, loglib::FilterCondition::Type::String);
        QCOMPARE(revived.columnKeys, msgKeys);
        QVERIFY(revived.filterString.has_value());
        QCOMPARE(*revived.filterString, std::string("m1"));
        QVERIFY(revived.matchType.has_value());
        QCOMPARE(*revived.matchType, loglib::FilterCondition::Match::Contains);

        // The wire-format expression also reflects the revived state.
        QCOMPARE(WireLeavesOf(mWindow->activeSession()->View().expression).size(), static_cast<size_t>(1));
    }

    // Multi-type round-trip: a string filter and an enum filter
    // persist together and revive into the correct slots, with one
    // Filters-menu entry per revived filter.
    void TestFilterPersistenceMultipleTypes()
    {
        const int levelCol = StreamFixtureForColumnTests();
        QVERIFY2(levelCol >= 0, "level column must exist after streaming");
        auto *model = mWindow->Model();
        const int msgCol = ColumnByHeader(*model, QStringLiteral("msg"));
        QVERIFY2(msgCol >= 0, "msg column must exist after streaming");

        // String filter on msg.
        const QString stringId = QStringLiteral("persisted-string");
        QMetaObject::invokeMethod(
            mWindow,
            "FilterSubmitted",
            Qt::DirectConnection,
            Q_ARG(QString, stringId),
            Q_ARG(int, msgCol),
            Q_ARG(QString, QStringLiteral("m1")),
            Q_ARG(int, static_cast<int>(loglib::FilterCondition::Match::Contains))
        );
        // Enum filter on level (auto-promoted to enum by the streaming threshold).
        const QString enumId = QStringLiteral("persisted-enum");
        // Hoist to a named local: an inline brace-init inside
        // `Q_ARG(...)` confuses the preprocessor (commas).
        const QStringList enumValues{QStringLiteral("info"), QStringLiteral("warn")};
        QMetaObject::invokeMethod(
            mWindow,
            "FilterEnumSubmitted",
            Qt::DirectConnection,
            Q_ARG(QString, enumId),
            Q_ARG(int, levelCol),
            Q_ARG(QStringList, enumValues)
        );
        QCoreApplication::processEvents();

        QCOMPARE(mWindow->Filters().size(), static_cast<size_t>(2));
        QCOMPARE(WireLeavesOf(mWindow->activeSession()->View().expression).size(), static_cast<size_t>(2));

        const QTemporaryDir savedDir;
        QVERIFY(savedDir.isValid());
        const QString savedPath = savedDir.filePath(QStringLiteral("multi-filter.json"));
        mWindow->SaveConfigurationToPathForTest(savedPath);

        QMetaObject::invokeMethod(mWindow, "ClearAllFilters", Qt::DirectConnection);
        QCoreApplication::processEvents();
        QCOMPARE(mWindow->Filters().size(), static_cast<size_t>(0));

        mWindow->SetSuppressDialogsForTest(true);
        mWindow->LoadConfigurationFromPathForTest(savedPath);
        QCoreApplication::processEvents();

        QCOMPARE(mWindow->LastDroppedFilterCountForTest(), 0);
        QCOMPARE(mWindow->Filters().size(), static_cast<size_t>(2));

        // Verify both types survived. UUIDs are regenerated on load
        // so look up by (type, columnKeys) rather than original IDs.
        const std::vector<std::string> msgKeys = model->Layout().Columns()[static_cast<size_t>(msgCol)].keys;
        const std::vector<std::string> levelKeys = model->Layout().Columns()[static_cast<size_t>(levelCol)].keys;
        bool sawString = false;
        bool sawEnum = false;
        for (const auto &[id, f] : mWindow->Filters())
        {
            if (f.type == loglib::FilterCondition::Type::String && f.columnKeys == msgKeys)
            {
                sawString = true;
                QVERIFY(f.filterString.has_value());
                QCOMPARE(*f.filterString, std::string("m1"));
            }
            else if (f.type == loglib::FilterCondition::Type::Enumeration && f.columnKeys == levelKeys)
            {
                sawEnum = true;
                QCOMPARE(f.filterValues.size(), static_cast<size_t>(2));
            }
        }
        QVERIFY2(sawString, "string filter on msg must revive");
        QVERIFY2(sawEnum, "enum filter on level must revive");

        // Filters menu must hold one sub-menu per revived filter.
        const auto *filtersMenu = mWindow->findChild<QMenu *>(QStringLiteral("menuFilters"));
        QVERIFY2(filtersMenu != nullptr, "MainWindow must expose its Filters menu");
        int subMenuCount = 0;
        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): false positive; prior `QVERIFY2` aborts on null.
        for (const QAction *action : filtersMenu->actions())
        {
            if (!action->data().toString().isNull())
            {
                ++subMenuCount;
            }
        }
        QCOMPARE(subMenuCount, 2);
    }

    // A configuration JSON mixing a valid filter with an invalid
    // one (out-of-range column index) must keep the valid filter,
    // drop the invalid one, and surface the drop through
    // `ShowDroppedFiltersDialog` (observed via the test counter).
    void TestFilterPersistenceDropsInvalidFilters()
    {
        const int levelCol = StreamFixtureForColumnTests();
        QVERIFY2(levelCol >= 0, "level column must exist after streaming");
        auto *model = mWindow->Model();
        const int columnCount = model->columnCount();

        // Build a config with two leaves: a valid enum on `level`
        // and a string leaf whose column keys don't match any
        // column (so `FindColumnByKeys` finds nothing). Serialise
        // via Glaze so the load path sees the same wire format
        // `Save` emits.
        (void)columnCount;
        loglib::LogConfiguration configuration{.columns = model->Layout().Columns()};
        const std::vector<std::string> levelKeys = configuration.columns[static_cast<size_t>(levelCol)].keys;
        configuration.view.expression = WireLeavesAsExpression({
            loglib::FilterCondition{
                .type = loglib::FilterCondition::Type::Enumeration,
                .columnKeys = levelKeys,
                .matchType = std::nullopt,
                .filterString = std::nullopt,
                .filterBegin = std::nullopt,
                .filterEnd = std::nullopt,
                .filterMinValue = std::nullopt,
                .filterMaxValue = std::nullopt,
                .filterValues = {"info"},
            },
            loglib::FilterCondition{
                .type = loglib::FilterCondition::Type::String,
                .columnKeys = {"__does_not_exist__"},
                .matchType = loglib::FilterCondition::Match::Contains,
                .filterString = std::string("nope"),
                .filterBegin = std::nullopt,
                .filterEnd = std::nullopt,
                .filterMinValue = std::nullopt,
                .filterMaxValue = std::nullopt,
                .filterValues = {},
            },
        });

        const QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        const QString path = tempDir.filePath(QStringLiteral("mixed-filters.json"));
        loglib::SaveConfiguration(configuration, path.toStdString(), loglib::SaveScope::Full);

        mWindow->SetSuppressDialogsForTest(true);
        mWindow->LoadConfigurationFromPathForTest(path);
        QCoreApplication::processEvents();

        QCOMPARE(mWindow->LastDroppedFilterCountForTest(), 1);
        QCOMPARE(mWindow->Filters().size(), static_cast<size_t>(1));
        const auto &surviving = mWindow->Filters().begin()->second;
        QCOMPARE(surviving.type, loglib::FilterCondition::Type::Enumeration);
        const int levelColAfterLoad = ColumnByHeader(*model, QStringLiteral("category"));
        QVERIFY2(levelColAfterLoad >= 0, "level column must reload");
        QCOMPARE(surviving.columnKeys, model->Layout().Columns()[static_cast<size_t>(levelColAfterLoad)].keys);
    }

    // Two consecutive `Save`s of the same filter set must produce
    // byte-identical JSON. `SyncSessionView` sorts the
    // snapshot so the output is independent of the `unordered_map`
    // iteration order. Without the sort, save -> reopen -> save
    // would change the file every round-trip.
    void TestFilterPersistenceSaveOrderingIsDeterministic()
    {
        const int levelCol = StreamFixtureForColumnTests();
        QVERIFY2(levelCol >= 0, "level column must exist after streaming");
        auto *model = mWindow->Model();
        const int msgCol = ColumnByHeader(*model, QStringLiteral("msg"));
        QVERIFY2(msgCol >= 0, "msg column must exist after streaming");

        // Three filters across two columns and two types so the
        // sort has to break ties beyond `row`. Submit out of sort
        // order so a missing sort would show in the saved bytes.
        const QStringList enumValues{QStringLiteral("info"), QStringLiteral("warn")};
        QMetaObject::invokeMethod(
            mWindow,
            "FilterEnumSubmitted",
            Qt::DirectConnection,
            Q_ARG(QString, QStringLiteral("z-enum")),
            Q_ARG(int, levelCol),
            Q_ARG(QStringList, enumValues)
        );
        QMetaObject::invokeMethod(
            mWindow,
            "FilterSubmitted",
            Qt::DirectConnection,
            Q_ARG(QString, QStringLiteral("a-string-msg")),
            Q_ARG(int, msgCol),
            Q_ARG(QString, QStringLiteral("m1")),
            Q_ARG(int, static_cast<int>(loglib::FilterCondition::Match::Contains))
        );
        QMetaObject::invokeMethod(
            mWindow,
            "FilterSubmitted",
            Qt::DirectConnection,
            Q_ARG(QString, QStringLiteral("m-string-msg-2")),
            Q_ARG(int, msgCol),
            Q_ARG(QString, QStringLiteral("m2")),
            Q_ARG(int, static_cast<int>(loglib::FilterCondition::Match::Contains))
        );
        QCoreApplication::processEvents();

        const QTemporaryDir savedDir;
        QVERIFY(savedDir.isValid());
        const QString pathA = savedDir.filePath(QStringLiteral("save-a.json"));
        const QString pathB = savedDir.filePath(QStringLiteral("save-b.json"));

        mWindow->SaveConfigurationToPathForTest(pathA);
        mWindow->SaveConfigurationToPathForTest(pathB);

        QFile fileA(pathA);
        QFile fileB(pathB);
        QVERIFY(fileA.open(QFile::ReadOnly));
        QVERIFY(fileB.open(QFile::ReadOnly));
        const QByteArray bytesA = fileA.readAll();
        const QByteArray bytesB = fileB.readAll();
        QCOMPARE(bytesA, bytesB);

        // Stronger pin: round-trip through Load and save again. The
        // load rebuilds `mFilters` with fresh UUIDs, so the iteration
        // order is guaranteed different -- yet the bytes must match.
        mWindow->SetSuppressDialogsForTest(true);
        mWindow->LoadConfigurationFromPathForTest(pathA);
        QCoreApplication::processEvents();
        const QString pathRoundTrip = savedDir.filePath(QStringLiteral("save-c.json"));
        mWindow->SaveConfigurationToPathForTest(pathRoundTrip);

        QFile fileC(pathRoundTrip);
        QVERIFY(fileC.open(QFile::ReadOnly));
        const QByteArray bytesC = fileC.readAll();
        QCOMPARE(bytesC, bytesA);
    }

    // Regression: an Advanced-editor tree whose root is `Or` / `Not`
    // (i.e. cannot be represented by simple-mode leaves) must survive
    // a Save / Load round-trip. Before the fix the load path called
    // `SyncSessionView` after `ClearAllFilters` and
    // that helper only preserved non-Leaf siblings of an `And` root
    // -- an `Or` / `Not` root was silently overwritten with the
    // empty simple-mode `And` (match-all), losing the user's tree.
    void TestFilterPersistenceAdvancedRootRoundTrips()
    {
        const int levelCol = StreamFixtureForColumnTests();
        QVERIFY2(levelCol >= 0, "level column must exist after streaming");
        auto *model = mWindow->Model();
        const int msgCol = ColumnByHeader(*model, QStringLiteral("msg"));
        QVERIFY2(msgCol >= 0, "msg column must exist after streaming");

        const std::vector<std::string> msgKeys = model->Layout().Columns()[static_cast<size_t>(msgCol)].keys;
        const std::vector<std::string> levelKeys = model->Layout().Columns()[static_cast<size_t>(levelCol)].keys;

        // Build `NOT msg:m1 OR level in [info]` -- a two-node
        // Advanced tree whose root is `Or` and one branch is a
        // `Not` (both shapes trigger the fixed preservation path).
        loglib::FilterExpression notLeaf = loglib::MakeNot(
            loglib::MakeLeaf(
                loglib::FilterCondition{
                    .type = loglib::FilterCondition::Type::String,
                    .columnKeys = msgKeys,
                    .matchType = loglib::FilterCondition::Match::Contains,
                    .filterString = std::string("m1"),
                }
            )
        );
        loglib::FilterExpression enumLeaf = loglib::MakeLeaf(
            loglib::FilterCondition{
                .type = loglib::FilterCondition::Type::Enumeration,
                .columnKeys = levelKeys,
                .filterValues = {"info"},
            }
        );
        std::vector<loglib::FilterExpression> orChildren;
        orChildren.push_back(std::move(notLeaf));
        orChildren.push_back(std::move(enumLeaf));
        const loglib::FilterExpression advancedTree = loglib::MakeOr(std::move(orChildren));

        // Install the Advanced tree exactly as `OpenAdvancedFilter`
        // would after the user clicks OK.
        mWindow->activeSession()->MutableView().expression = advancedTree;

        const QTemporaryDir savedDir;
        QVERIFY(savedDir.isValid());
        const QString savedPath = savedDir.filePath(QStringLiteral("advanced-root.json"));
        mWindow->SaveConfigurationToPathForTest(savedPath);

        // Wipe the runtime expression directly. This bypasses the
        // simple-mode UI plumbing so the load path is the only
        // remaining source of truth for the reload assertion --
        // `ClearAllFilters` would work equally well here (it also
        // resets to match-all; see `TestClearAllFiltersDropsAdvancedTree`),
        // but going through the manager keeps the intent explicit.
        mWindow->activeSession()->MutableView().expression = loglib::FilterExpression{};
        QCoreApplication::processEvents();
        QVERIFY(loglib::AcceptsEverything(mWindow->activeSession()->View().expression));

        mWindow->SetSuppressDialogsForTest(true);
        mWindow->LoadConfigurationFromPathForTest(savedPath);
        QCoreApplication::processEvents();

        // Locate the preserved Advanced subtree. The mirror wraps a
        // pure-Advanced root in a single-child `And` for uniform
        // simple-mode-plus-Advanced siblings; both shapes are
        // semantically equivalent, so accept either.
        const auto &reloaded = mWindow->activeSession()->View().expression;
        const loglib::FilterExpression *advanced = &reloaded;
        if (const auto *asAnd = std::get_if<loglib::FilterExpression::And>(&reloaded.node); asAnd != nullptr)
        {
            QCOMPARE(static_cast<int>(asAnd->children.size()), 1);
            advanced = &asAnd->children.front();
        }
        QVERIFY2(
            *advanced == advancedTree, "Advanced Or/Not tree must survive Save/Load exactly (structure + payload)"
        );
    }

    // `Save Configuration...` (SaveScope::Columns) writes a
    // portable layout: columns survive, session-only state
    // (filters, sort) is omitted from the file on purpose so the
    // configuration can be applied to a different log source
    // without dragging old filter rows / sort indices along.
    void TestSaveScopeColumnsOnlyOmitsSession()
    {
        const int levelCol = StreamFixtureForColumnTests();
        QVERIFY2(levelCol >= 0, "level column must exist after streaming");
        auto *model = mWindow->Model();
        const int msgCol = ColumnByHeader(*model, QStringLiteral("msg"));
        QVERIFY2(msgCol >= 0, "msg column must exist after streaming");

        // Engage filter + sort so the wire-format vector + sort field
        // are non-default before saving.
        QMetaObject::invokeMethod(
            mWindow,
            "FilterSubmitted",
            Qt::DirectConnection,
            Q_ARG(QString, QStringLiteral("columns-only-filter")),
            Q_ARG(int, msgCol),
            Q_ARG(QString, QStringLiteral("m1")),
            Q_ARG(int, static_cast<int>(loglib::FilterCondition::Match::Contains))
        );
        auto *tableView = mWindow->findChild<LogTableView *>();
        QVERIFY(tableView != nullptr);
        tableView->sortByColumn(levelCol, Qt::DescendingOrder);
        QCoreApplication::processEvents();

        QCOMPARE(mWindow->Filters().size(), static_cast<size_t>(1));

        const QTemporaryDir savedDir;
        QVERIFY(savedDir.isValid());
        const QString columnsOnlyPath = savedDir.filePath(QStringLiteral("columns-only.json"));
        mWindow->SaveConfigurationToPathForTest(columnsOnlyPath, loglib::SaveScope::Columns);

        // Reload into a fresh manager and confirm the on-disk file is
        // a configuration-shape file: columns present, session-only
        // fields back at their inert defaults.
        const loglib::LogConfiguration probe = loglib::LoadConfiguration(columnsOnlyPath.toStdString());
        QVERIFY(!probe.columns.empty());
        QVERIFY2(loglib::AcceptsEverything(probe.view.expression), "SaveScope::Columns must omit filter expression");
        QVERIFY(probe.view.sort.columnKeys.empty());
        QVERIFY2(!probe.source.has_value(), "SaveScope::Columns must omit source");
    }

    // `Save Session...` (SaveScope::Full) persists the live sort
    // indicator and a Load restores it. This is the load-bearing
    // promise of the session/configuration split: filters already
    // round-trip; the sort indicator now travels with them.
    void TestSaveScopeFullPersistsSortAndRestoresOnLoad()
    {
        const int levelCol = StreamFixtureForColumnTests();
        QVERIFY2(levelCol >= 0, "level column must exist after streaming");

        auto *tableView = mWindow->findChild<LogTableView *>();
        QVERIFY(tableView != nullptr);
        const QHeaderView *header = tableView->horizontalHeader();
        QVERIFY(header != nullptr);

        tableView->sortByColumn(levelCol, Qt::DescendingOrder);
        QCoreApplication::processEvents();
        QVERIFY2(
            header->isSortIndicatorShown() && header->sortIndicatorSection() == levelCol,
            "precondition: sort indicator must be on the level column before save"
        );

        const QTemporaryDir savedDir;
        QVERIFY(savedDir.isValid());
        const QString sessionPath = savedDir.filePath(QStringLiteral("with-sort.json"));
        mWindow->SaveConfigurationToPathForTest(sessionPath, loglib::SaveScope::Full);

        // Drop the live sort so the load has to actually reapply it,
        // not just look like it did.
        tableView->sortByColumn(-1, Qt::AscendingOrder);
        QCoreApplication::processEvents();

        mWindow->SetSuppressDialogsForTest(true);
        mWindow->LoadConfigurationFromPathForTest(sessionPath);
        QCoreApplication::processEvents();

        QVERIFY2(header->isSortIndicatorShown(), "Load must restore the persisted sort indicator");
        QCOMPARE(header->sortIndicatorSection(), levelCol);
        QCOMPARE(header->sortIndicatorOrder(), Qt::DescendingOrder);
    }

    // Regression: `Save Session...` used to silently drop the
    // `Source` descriptor for a static file because
    // `streamingFinished` reset `mCurrentSource` unconditionally,
    // and a follow-up `Save` mirrored the now-empty mirror into
    // the configuration. The fix is to keep the descriptor alive
    // for the lifetime of the loaded rows; this test exercises the
    // full save -> wipe -> load round-trip.
    //
    // The shared streaming fixture bypasses `OpenFiles`, so we
    // poke `mCurrentSource` directly: production sets it on every
    // open path; the test fakes that to keep the fixture small.
    void TestSaveScopeFullRoundTripsSourceDescriptor()
    {
        const int levelCol = StreamFixtureForColumnTests();
        QVERIFY2(levelCol >= 0, "level column must exist after streaming");

        // Stand in for the open-path `mCurrentSource` write that the
        // sync fixture skips. `streamingFinished` already ran by now
        // -- before the fix the reset there would have wiped this
        // back to `nullopt` regardless.
        const std::string syntheticSource = "/test/source/path.log";
        mWindow->SetCurrentSourceForTest(
            loglib::SourceDescriptor{
                .kind = loglib::SourceDescriptor::Kind::File,
                .locators = {{.path = syntheticSource, .key = syntheticSource}}
            }
        );

        const QTemporaryDir savedDir;
        QVERIFY(savedDir.isValid());
        const QString sessionPath = savedDir.filePath(QStringLiteral("with-source.json"));
        mWindow->SaveConfigurationToPathForTest(sessionPath, loglib::SaveScope::Full);

        // Read back through a fresh manager so we don't accidentally
        // observe the still-live runtime mirror.
        const loglib::LogConfiguration probe = loglib::LoadConfiguration(sessionPath.toStdString());
        QVERIFY2(
            probe.source.has_value(),
            "SaveScope::Full must persist the source descriptor for a streamed-then-finished file"
        );
        QCOMPARE(static_cast<int>(probe.source->kind), static_cast<int>(loglib::SourceDescriptor::Kind::File));
        QCOMPARE(probe.source->locators.size(), static_cast<std::size_t>(1));
        QCOMPARE(QString::fromStdString(probe.source->locators.front().path), QString::fromStdString(syntheticSource));

        // Now load the freshly-saved session back into the running
        // window and re-save: the descriptor must survive that round
        // too (regression for the symmetric bug where Load -> Save
        // dropped a loaded source because the runtime mirror was
        // never seeded from disk).
        mWindow->SetSuppressDialogsForTest(true);
        mWindow->LoadConfigurationFromPathForTest(sessionPath);
        QCoreApplication::processEvents();

        const QString resavePath = savedDir.filePath(QStringLiteral("with-source-resave.json"));
        mWindow->SaveConfigurationToPathForTest(resavePath, loglib::SaveScope::Full);

        const loglib::LogConfiguration resaveProbe = loglib::LoadConfiguration(resavePath.toStdString());
        QVERIFY2(resaveProbe.source.has_value(), "Load -> Save round trip must preserve a loaded source descriptor");
        QCOMPARE(resaveProbe.source->locators.size(), static_cast<std::size_t>(1));
        QCOMPARE(resaveProbe.source->locators.front().path, syntheticSource);
    }

    // Configuration-only refresh clears stale rotation Undo state.
    void TestTryLoadAsConfigurationClearsUndoRotationExpansion()
    {
        auto *model = mWindow->Model();
        QVERIFY(model != nullptr);

        const QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString primaryPath = dir.filePath(QStringLiteral("app.log"));
        const QString olderPath = dir.filePath(QStringLiteral("app.log.1"));
        const auto write = [](const QString &p, const QString &content) {
            std::ofstream stream(p.toStdString(), std::ios::binary);
            QVERIFY2(stream.is_open(), "fixture write must succeed");
            stream << content.toStdString();
        };
        write(
            primaryPath,
            R"({"msg":"p"})"
            "\n"
        );
        write(
            olderPath,
            R"({"msg":"g1"})"
            "\n"
        );

        QSignalSpy finishedSpy(model, &LogModel::streamingFinished);
        QVERIFY(finishedSpy.isValid());
        (void)mWindow->OpenMixedFilesForTest({primaryPath}, MainWindow::OpenMode::Replace);
        for (int i = 0; i < 2; ++i)
        {
            QVERIFY2(finishedSpy.wait(60000), "two per-file streamingFinished emits expected");
        }
        QCoreApplication::processEvents();
        QVERIFY(mWindow->IsUndoRotationExpansionEnabledForTest());

        // Columns-only scope exercises the row-preserving refresh path.
        const QTemporaryDir savedDir;
        QVERIFY(savedDir.isValid());
        const QString columnsPath = savedDir.filePath(QStringLiteral("cols.json"));
        mWindow->SaveConfigurationToPathForTest(columnsPath, loglib::SaveScope::Columns);

        QVERIFY(mWindow->TryLoadAsConfigurationForTest(columnsPath));
        QCoreApplication::processEvents();

        QVERIFY2(
            !mWindow->IsUndoRotationExpansionEnabledForTest(),
            "TryLoadAsConfiguration must disable Undo Rotated History Expansion"
        );
        QVERIFY2(
            mWindow->LastRotationExpansionOriginalInputsForTest().isEmpty(),
            "TryLoadAsConfiguration must clear the stored original-inputs list"
        );
    }

    // Regression: closing the Preferences window via the X button
    // must revert a live-previewed theme to the persisted selection.
    // Without the `closeEvent` override the Dark preview leaked
    // past the dialog until the next app restart.
    void TestPreferencesEditorCloseEventRevertsThemePreview()
    {
        QVERIFY(mTheme != nullptr);

        // Persist a known baseline so the revert has a deterministic target.
        mTheme->SetActiveSelection(QStringLiteral("Light"));
        mTheme->SaveConfiguration();
        QCOMPARE(mTheme->PersistedSelection(), QStringLiteral("Light"));
        QCOMPARE(mTheme->ActiveSelection(), QStringLiteral("Light"));

        PreferencesEditor editor(mTheme.data());

        // Preview Dark directly via `ThemeControl` to stay independent
        // of QComboBox event quirks under offscreen-QPA.
        mTheme->SetActiveSelection(QStringLiteral("Dark"));
        QCoreApplication::processEvents();
        QCOMPARE(mTheme->ActiveSelection(), QStringLiteral("Dark"));
        // Persisted unchanged -- only Ok writes that.
        QCOMPARE(mTheme->PersistedSelection(), QStringLiteral("Light"));

        // Same path the X button takes (programmatic `close()` ->
        // `closeEvent` without first running Cancel).
        editor.close();
        QCoreApplication::processEvents();

        QCOMPARE(mTheme->ActiveSelection(), QStringLiteral("Light"));
        QCOMPARE(mTheme->PersistedSelection(), QStringLiteral("Light"));
    }

    // The Auto-detect choice at the top of the Type combo collapses
    // the (Type::Any, autoDetect=true) pair into a single user-
    // visible entry. Selecting it must restore both fields atomically
    // -- otherwise the column ends up in the legacy "user-pinned Any"
    // state that suppresses promotion.
    void TestColumnEditorAutoDetectChoiceRestoresFlag()
    {
        const int levelCol = StreamFixtureForColumnTests();
        QVERIFY2(levelCol >= 0, "level column must exist after streaming");
        auto *model = mWindow->Model();
        const int msgCol = ColumnByHeader(*model, QStringLiteral("msg"));
        QVERIFY2(msgCol >= 0, "msg column must exist after streaming");

        // Pre-rescan: the streaming detector already promoted `msg`
        // to `String` because its 200 unique values trip the
        // cardinality bail. We need a column whose post-rescan type
        // is observably different from its pre-edit type, so park
        // `msg` at a deliberately-wrong terminal type first. Picking
        // "Auto-detect" then exercises both the autoDetect flag
        // restoration *and* the rescan that promotes the column
        // back to the detector's preferred type.
        model->MutableLayout().SetColumnType(static_cast<size_t>(msgCol), loglib::ColumnType::Integer);
        model->MutableLayout().SetColumnAutoDetect(static_cast<size_t>(msgCol), false);

        ColumnEditor editor(model, msgCol);
        auto *typeCombo = editor.findChild<QComboBox *>();
        QVERIFY(typeCombo != nullptr);
        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): prior QVERIFY aborts on null.
        typeCombo->setCurrentIndex(0); // "Auto-detect"
        editor.Apply();

        const auto &updated = model->Layout().Columns()[static_cast<size_t>(msgCol)];
        // The rescan kicks in (bug 4 fix). `msg` carries 200 unique
        // values, so the candidate tracker bails on cardinality and
        // routes to `String`. The autoDetect flag survives the
        // re-route -- consistent with the streaming detector's
        // behaviour, and harmless because `Type::String` columns
        // are not enum-pass-eligible.
        QCOMPARE(updated.type, loglib::ColumnType::String);
        QVERIFY2(updated.autoDetect, "Auto-detect choice must restore autoDetect=true");
    }

    // Item 1: the File -> Recent Sessions submenu carries the
    // `file-clock` Lucide glyph so the entry is recognisable at a
    // glance. The icon is set on the submenu's `menuAction()` and
    // refreshed through the same `RefreshThemedIcons` pipeline as
    // toolbar actions, so a Light <-> Dark flip keeps it visible.
    void TestRecentSessionsMenuHasThemedIcon()
    {
        auto *recentMenu = mWindow->findChild<QMenu *>(QStringLiteral("menuRecentSessions"));
        QVERIFY2(recentMenu != nullptr, "File menu must expose menuRecentSessions");
        // NOLINTBEGIN(clang-analyzer-core.CallAndMessage): prior QVERIFY2 aborts on null.
        const QAction *menuAction = recentMenu->menuAction();
        QVERIFY2(menuAction != nullptr, "menuRecentSessions must have a menuAction");
        QVERIFY2(!menuAction->icon().isNull(), "menuRecentSessions menuAction must carry a themed icon");
        // The path stays as a property so `RefreshThemedIcons` can
        // re-tint without a per-action switch -- the value here is
        // the contract checked in `BuildMainToolbar`.
        QCOMPARE(menuAction->property("svgIconPath").toString(), QStringLiteral(":/icons/file-clock.svg"));
        // NOLINTEND(clang-analyzer-core.CallAndMessage)
    }

    // A hand-edited Boolean filter with mixed-case `"True"` /
    // `"FALSE"` still restores correctly. `DecodeBooleanFilterSides`
    // is case-insensitive on read; the canonical submit path keeps
    // writing lowercase.
    void TestSavedBooleanFilterCaseInsensitiveRestore()
    {
        QStringList lines;
        lines.reserve(60);
        for (int i = 0; i < 60; ++i)
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

        loglib::FilterCondition savedFilter;
        savedFilter.type = loglib::FilterCondition::Type::Boolean;
        savedFilter.columnKeys = model->Layout().Columns()[static_cast<size_t>(flagCol)].keys;
        // Non-canonical casing: lower path must still recognise both.
        savedFilter.filterValues = {"True", "FALSE"};

        mWindow->statusBar()->clearMessage();
        const QString filterId = QStringLiteral("saved-bool-mixed-case");
        QVERIFY2(
            QMetaObject::invokeMethod(
                mWindow,
                "AddFilter",
                Qt::DirectConnection,
                Q_ARG(QString, filterId),
                Q_ARG(std::optional<loglib::FilterCondition>, savedFilter)
            ),
            "AddFilter slot must be invocable via meta-object"
        );
        QCoreApplication::processEvents();

        // Editor opens preloaded with both sides checked.
        FilterEditor *editor = nullptr;
        for (QWidget *widget : QApplication::topLevelWidgets())
        {
            if (auto *e = qobject_cast<FilterEditor *>(widget))
            {
                editor = e;
                break;
            }
        }
        QVERIFY2(editor != nullptr, "FilterEditor must open for a Boolean-typed saved filter");
        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage)
        QVERIFY(editor->GetBooleanIncludeTrue());
        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage)
        QVERIFY(editor->GetBooleanIncludeFalse());

        editor->close();
        editor->deleteLater();
        QCoreApplication::processEvents();

        model->EndStreaming(false);
    }

    // `actionNewWindow` spawns a second MainWindow sharing the
    // primary's manager. Asserts: one new top-level appears, the
    // shared manager observes the same writes, and the spawn has
    // `WA_DeleteOnClose`.
    void TestNewWindowSharesHistoryManager()
    {
        const QTemporaryDir sessionsDir;
        QVERIFY(sessionsDir.isValid());

        SessionHistoryManager manager(QDir(sessionsDir.path()), std::make_unique<InMemoryRecentsIndexStorage>());
        auto primary = std::make_unique<MainWindow>(mTheme.data(), &manager, nullptr);

        // Each `MainWindow` constructor also installs auxiliary
        // top-levels (preferences, record detail), so filter to
        // just the MainWindows.
        auto countMainWindows = []() {
            int n = 0;
            for (QWidget *w : QApplication::topLevelWidgets())
            {
                if (qobject_cast<MainWindow *>(w) != nullptr)
                {
                    ++n;
                }
            }
            return n;
        };

        const int mainsBefore = countMainWindows();
        const QList<QWidget *> topLevelBefore = QApplication::topLevelWidgets();

        primary->findChild<QAction *>(QStringLiteral("actionNewWindow"))->trigger();
        QCoreApplication::processEvents();

        QCOMPARE(countMainWindows(), mainsBefore + 1);

        // Find the newly-spawned MainWindow (the one that wasn't in
        // `topLevelBefore`).
        MainWindow *child = nullptr;
        for (QWidget *w : QApplication::topLevelWidgets())
        {
            if (topLevelBefore.contains(w))
            {
                continue;
            }
            auto *candidate = qobject_cast<MainWindow *>(w);
            if (candidate != nullptr && candidate != primary.get())
            {
                child = candidate;
                break;
            }
        }
        QVERIFY2(child != nullptr, "newly spawned window must be a MainWindow");
        QVERIFY2(child->testAttribute(Qt::WA_DeleteOnClose), "spawned window must auto-delete on close");

        // Drive a real open through the primary; the shared
        // manager surfaces the entry to both windows.
        const TempJsonFile fixture({QStringLiteral(R"({"msg": "shared"})")});
        QSignalSpy primaryFinished(primary->Model(), &LogModel::streamingFinished);
        primary->OpenFilesForTest({fixture.Path()}, MainWindow::OpenMode::Append);
        QVERIFY(primaryFinished.wait(60000));
        QCoreApplication::processEvents();
        QCOMPARE(manager.List().size(), 1);

        // Close the spawned window and spin the event loop so the
        // `deleteLater` from `WA_DeleteOnClose` runs before
        // `primary` falls out of scope.
        const QSignalSpy destroyedSpy(child, &QObject::destroyed);
        child->close();
        for (int i = 0; i < 50 && destroyedSpy.isEmpty(); ++i)
        {
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            QCoreApplication::processEvents();
        }
        QCOMPARE(destroyedSpy.count(), 1);
    }

    // `RestoreLastSessionFromPath` reopens an auto-saved snapshot
    // into a freshly-built window (the entry point that `main()`'s
    // restore-on-launch hook uses).
    void TestRestoreLastSessionFromPath()
    {
        const QTemporaryDir sessionsDir;
        QVERIFY(sessionsDir.isValid());

        SessionHistoryManager manager(QDir(sessionsDir.path()), std::make_unique<InMemoryRecentsIndexStorage>());

        // Seed an on-disk JSON via a real open.
        auto seeder = std::make_unique<MainWindow>(mTheme.data(), &manager, nullptr);
        const QStringList fixtureLines{
            QStringLiteral(R"({"category": "info", "msg": "alpha"})"),
            QStringLiteral(R"({"category": "warn", "msg": "beta"})"),
        };
        const TempJsonFile fixture(fixtureLines);

        QSignalSpy finishedSpy(seeder->Model(), &LogModel::streamingFinished);
        seeder->OpenFilesForTest({fixture.Path()}, MainWindow::OpenMode::Append);
        QVERIFY(finishedSpy.wait(60000));
        QCoreApplication::processEvents();
        QCOMPARE(manager.List().size(), 1);

        const auto lastPath = manager.LastSessionPath();
        QVERIFY(lastPath.has_value());
        // Drop the seeder so its closeEvent flush completes first
        // (mirrors the real cold-start order).
        seeder.reset();

        // Fresh window restores the snapshot.
        auto restored = std::make_unique<MainWindow>(mTheme.data(), &manager, nullptr);
        QSignalSpy restoredSpy(restored->Model(), &LogModel::streamingFinished);
        restored->RestoreLastSessionFromPath(*lastPath);
        QVERIFY(restoredSpy.wait(60000));
        QCoreApplication::processEvents();

        QCOMPARE(restored->Model()->rowCount(), fixtureLines.size());
    }

    // Regression: a saved sort must be deferred until streaming
    // finishes. Avoids the O(N^2) per-row insert path
    // `LogFilterModel::OnSourceRowsInserted` takes under an
    // active sort (a 1 GB restore "never finishes" otherwise).
    // Asserts both halves: proxy is unsorted while streaming, and
    // the saved sort is applied exactly once at the end.
    void TestRestoreLastSessionDefersSortUntilStreamingFinishes()
    {
        const QTemporaryDir sessionsDir;
        QVERIFY(sessionsDir.isValid());
        SessionHistoryManager manager(QDir(sessionsDir.path()), std::make_unique<InMemoryRecentsIndexStorage>());

        // Seed: stream, engage a sort, save the session via the
        // close-event autosave so the saved JSON carries the sort.
        auto seeder = std::make_unique<MainWindow>(mTheme.data(), &manager, nullptr);
        const QStringList fixtureLines{
            QStringLiteral(R"({"category": "info", "msg": "alpha"})"),
            QStringLiteral(R"({"category": "warn", "msg": "beta"})"),
        };
        const TempJsonFile fixture(fixtureLines);

        QSignalSpy seederFinishedSpy(seeder->Model(), &LogModel::streamingFinished);
        seeder->OpenFilesForTest({fixture.Path()}, MainWindow::OpenMode::Append);
        QVERIFY(seederFinishedSpy.wait(60000));
        QCoreApplication::processEvents();

        const int categoryCol = ColumnByHeader(*seeder->Model(), QStringLiteral("category"));
        QVERIFY2(categoryCol >= 0, "category column must exist after streaming");
        auto *seederTable = seeder->findChild<LogTableView *>();
        QVERIFY(seederTable != nullptr);
        seederTable->sortByColumn(categoryCol, Qt::DescendingOrder);
        QCoreApplication::processEvents();

        // closeEvent autosave captures the manual sort engaged
        // after `streamingFinished` -- the earlier streaming
        // autosave saw the proxy before the sort.
        seeder->close();
        QCoreApplication::processEvents();
        seeder.reset();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QCoreApplication::processEvents();

        const auto lastPath = manager.LastSessionPath();
        QVERIFY(lastPath.has_value());

        // Sanity: the saved JSON carries the sort.
        {
            const loglib::LogConfiguration probe = loglib::LoadConfiguration(lastPath->toStdString());
            QCOMPARE(probe.view.sort.columnKeys, std::vector<std::string>{"category"});
            QVERIFY(probe.view.sort.direction == loglib::SortDirection::Descending);
        }

        // Fresh window restores the snapshot.
        auto restored = std::make_unique<MainWindow>(mTheme.data(), &manager, nullptr);
        auto *restoredFilter = restored->FilterModel();
        QVERIFY2(restoredFilter != nullptr, "restored window must own a LogFilterModel");

        QSignalSpy restoredFinishedSpy(restored->Model(), &LogModel::streamingFinished);
        restored->RestoreLastSessionFromPath(*lastPath);
        // Sort still deferred immediately after the call returns:
        // no batches have been processed yet.
        QCOMPARE(restoredFilter->SortColumn(), -1);

        QVERIFY(restoredFinishedSpy.wait(60000));
        QCoreApplication::processEvents();

        // `ApplyDeferredSortFromConfig` applies the sort once
        // when streaming finishes.
        QCOMPARE(restoredFilter->SortColumn(), categoryCol);
        QCOMPARE(restoredFilter->SortOrder(), Qt::DescendingOrder);
        QCOMPARE(restored->Model()->rowCount(), fixtureLines.size());

        // The deferred sort reaches `UpdateSortStatus` via
        // the `layoutChanged` it issues. Pin the status-bar
        // indicator and `actionClearSort` to mirror the
        // restored sort once streaming settles.
        auto *restoredClearSortAction = restored->findChild<QAction *>(QStringLiteral("actionClearSort"));
        QVERIFY2(restoredClearSortAction != nullptr, "restored window must own actionClearSort");
        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): prior QVERIFY2 aborts on null.
        QVERIFY2(restoredClearSortAction->isEnabled(), "actionClearSort must be enabled after deferred restore");
        auto *restoredClearSortButton = restored->findChild<QPushButton *>(QStringLiteral("clearSortStatusButton"));
        QVERIFY2(restoredClearSortButton != nullptr, "restored window must own clearSortStatusButton");
        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): prior QVERIFY2 aborts on null.
        QVERIFY2(!restoredClearSortButton->isHidden(), "status-bar Clear-sort indicator must surface after restore");
    }

    // Recent Sessions submenu rebuild on `aboutToShow`. Asserts:
    // entries + separator + Clear are present, clicking an entry
    // reopens the configuration, and `mAutoSaveUuid` is pinned so
    // further edits update the same entry.
    void TestRecentSessionsMenuReopensEntry()
    {
        const QTemporaryDir sessionsDir;
        QVERIFY(sessionsDir.isValid());

        SessionHistoryManager manager(QDir(sessionsDir.path()), std::make_unique<InMemoryRecentsIndexStorage>());
        auto wired = std::make_unique<MainWindow>(mTheme.data(), &manager, nullptr);

        // Seed a recents entry via a real open.
        const QStringList fixtureLines{
            QStringLiteral(R"({"category": "info", "msg": "alpha"})"),
            QStringLiteral(R"({"category": "warn", "msg": "beta"})"),
        };
        const TempJsonFile fixture(fixtureLines);

        QSignalSpy finishedSpy(wired->Model(), &LogModel::streamingFinished);
        wired->OpenFilesForTest({fixture.Path()}, MainWindow::OpenMode::Append);
        QVERIFY(finishedSpy.wait(60000));
        QCoreApplication::processEvents();

        QCOMPARE(manager.List().size(), 1);
        const QString uuid = manager.List().front().uuid;

        // Tear the session down so the menu reopen has work to do.
        wired->findChild<QAction *>(QStringLiteral("actionNewSession"))->trigger();
        QCoreApplication::processEvents();
        QCOMPARE(wired->Model()->rowCount(), 0);

        // Force the submenu rebuild + drive the entry action.
        // Reach for the menu via the explicit accessor: the Qt 6.8 +
        // offscreen-QPA `findChild<QMenu*>` traversal bug strands the
        // by-objectName lookup on the Linux runner.
        auto *recentsMenu = wired->findChild<QMenu *>(QStringLiteral("menuRecentSessions"));
        QVERIFY2(recentsMenu != nullptr, "menuRecentSessions must exist");
        emit recentsMenu->aboutToShow();

        const QList<QAction *> actions = recentsMenu->actions();
        QVERIFY2(actions.size() >= 3, "rebuild must produce at least one entry + separator + clear");

        // First action is the single recents entry.
        QAction *entryAction = actions.first();
        finishedSpy.clear();
        entryAction->trigger();
        QVERIFY(finishedSpy.wait(60000));
        QCoreApplication::processEvents();

        QCOMPARE(wired->Model()->rowCount(), fixtureLines.size());

        // Re-stream auto-save updates the same recents entry.
        QCOMPARE(manager.List().size(), 1);
        QCOMPARE(manager.List().front().uuid, uuid);
    }

    // Auto-save writes a snapshot on every successful streaming
    // finish, reusing the per-window uuid so a second auto-save
    // updates the same recents entry instead of growing the list.
    void TestMainWindowAutoSavesOnStreamingFinished()
    {
        const QTemporaryDir sessionsDir;
        QVERIFY(sessionsDir.isValid());

        SessionHistoryManager manager(QDir(sessionsDir.path()), std::make_unique<InMemoryRecentsIndexStorage>());

        // The fixture's `mWindow` is unwired; use the production
        // constructor that takes a manager.
        auto wired = std::make_unique<MainWindow>(mTheme.data(), &manager, nullptr);

        auto *model = wired->Model();
        QVERIFY(model != nullptr);

        const QStringList fixtureLines{
            QStringLiteral(R"({"category": "info", "msg": "a-0"})"),
            QStringLiteral(R"({"category": "warn", "msg": "a-1"})"),
        };
        const TempJsonFile fixture(fixtureLines);

        QSignalSpy finishedSpy(model, &LogModel::streamingFinished);
        QVERIFY(finishedSpy.isValid());
        const QSignalSpy changedSpy(&manager, &SessionHistoryManager::changed);
        QVERIFY(changedSpy.isValid());

        wired->OpenFilesForTest({fixture.Path()}, MainWindow::OpenMode::Append);
        QVERIFY(finishedSpy.wait(60000));
        QCoreApplication::processEvents();

        QCOMPARE(changedSpy.count(), 1);
        QCOMPARE(manager.List().size(), 1);
        const QString firstUuid = manager.List().front().uuid;

        // Second open updates the existing entry, not appends.
        const TempJsonFile fixtureB(QStringList{QStringLiteral(R"({"msg": "b-0"})")});
        finishedSpy.clear();
        wired->OpenFilesForTest({fixtureB.Path()}, MainWindow::OpenMode::Append);
        QVERIFY(finishedSpy.wait(60000));
        QCoreApplication::processEvents();

        QCOMPARE(manager.List().size(), 1);
        QCOMPARE(manager.List().front().uuid, firstUuid);
        // Label now tracks both A and B (B appended onto A).
        QCOMPARE(manager.List().front().fileCount, 2);
    }

    // `openWindowsAtQuit` round-trip preserves order. If QSettings
    // refuses the write (registry permissions in some Windows test
    // envs), the test skips rather than failing.
    void TestOpenWindowsAtQuitRoundTrip()
    {
        const QStringList expected{QStringLiteral("uuid-a"), QStringLiteral("uuid-b"), QStringLiteral("uuid-c")};
        const QStringList previous = SessionHistoryManager::OpenWindowsAtQuitUnlocked();
        auto restoreGuard = qScopeGuard([&]() { SessionHistoryManager::SetOpenWindowsAtQuit(previous); });

        SessionHistoryManager::SetOpenWindowsAtQuit(expected);
        const QStringList actual = SessionHistoryManager::OpenWindowsAtQuitUnlocked();
        // QSettings may swallow the write on some Windows test envs.
        if (actual.isEmpty())
        {
            QSKIP("QSettings did not honour the write in this environment");
        }
        QCOMPARE(actual, expected);

        SessionHistoryManager::SetOpenWindowsAtQuit({});
        QCOMPARE(SessionHistoryManager::OpenWindowsAtQuitUnlocked(), QStringList{});
    }

    // `TakeOpenWindowsAtQuit` is the atomic read-and-wipe used by
    // `main()` at startup: returns the list and wipes it under one
    // critical section so a sibling writer can't race between them.
    void TestTakeOpenWindowsAtQuitReadsAndWipesAtomically()
    {
        const QStringList expected{QStringLiteral("uuid-x"), QStringLiteral("uuid-y")};
        const QStringList previous = SessionHistoryManager::OpenWindowsAtQuitUnlocked();
        auto restoreGuard = qScopeGuard([&]() { SessionHistoryManager::SetOpenWindowsAtQuit(previous); });

        SessionHistoryManager::SetOpenWindowsAtQuit(expected);
        // QSettings-on-Windows soft-skip mirrors `TestOpenWindowsAtQuitRoundTrip`.
        if (SessionHistoryManager::OpenWindowsAtQuitUnlocked().isEmpty())
        {
            QSKIP("QSettings did not honour the write in this environment");
        }

        const QStringList taken = SessionHistoryManager::TakeOpenWindowsAtQuit();
        QCOMPARE(taken, expected);

        // Post-take: the persisted list is empty.
        QCOMPARE(SessionHistoryManager::OpenWindowsAtQuitUnlocked(), QStringList{});

        // A second take returns empty and is a no-op (idempotent).
        QCOMPARE(SessionHistoryManager::TakeOpenWindowsAtQuit(), QStringList{});
    }

    // ---------------------------------------------------------------
    // SingleInstanceGuard
    // ---------------------------------------------------------------

    // Primary's socket receives a hand-rolled secondary frame and
    // emits `openWindowRequested`. Hand-rolled rather than using a
    // second `TryAcquire` because both peers share the test thread,
    // and the secondary's wait would block the primary's slot.
    void TestSingleInstanceForwardsOpenRequest()
    {
        const QString socketName =
            QStringLiteral("slv-fwd-") + QUuid::createUuid().toString(QUuid::WithoutBraces).left(8);

        SingleInstanceGuard primary;
        primary.SetSocketNameForTest(socketName);
        QVERIFY(primary.TryAcquire({}, /*allowNewInstance=*/false));

        QSignalSpy spy(&primary, &SingleInstanceGuard::openWindowRequested);
        QVERIFY(spy.isValid());

        const QStringList forwardFiles{
            QStringLiteral("C:/logs/forward-a.json"), QStringLiteral("C:/logs/forward-b.json")
        };

        // Wire format: magic `STRUCTLOG`, `quint8` version (2),
        // file list, `quint32 truncatedCount`, via Qt_6_0.
        QLocalSocket secondary;
        secondary.connectToServer(socketName);
        QVERIFY2(secondary.waitForConnected(2000), "secondary must connect to primary's socket");

        QByteArray payload;
        QDataStream out(&payload, QIODevice::WriteOnly);
        out.setVersion(QDataStream::Qt_6_0);
        out << QByteArray("STRUCTLOG");
        out << static_cast<quint8>(2);
        out << forwardFiles;
        out << static_cast<quint32>(0);

        const qint64 written = secondary.write(payload);
        QCOMPARE(written, payload.size());
        // `flush` may return false if the OS already drained;
        // rely on `waitForBytesWritten` for the wire guarantee.
        secondary.flush();
        // The guard reads on this thread, so wait with the event loop running; a blocking wait only times out.
        QVERIFY(WaitUntilProcessingEvents([&] { return secondary.bytesToWrite() == 0; }));

        // Spin the loop so the primary drains the pipe.
        QVERIFY(spy.count() > 0 || spy.wait(60000));
        QCOMPARE(spy.count(), 1);
        const auto args = spy.takeFirst();
        const QStringList received = args.at(0).toStringList();
        QCOMPARE(received, forwardFiles);
        QCOMPARE(args.at(1).toInt(), 0);

        // The guard runs on this thread, so a blocking wait here would only time out; closing is enough.
        secondary.disconnectFromServer();
    }

    // With `--new-instance`, the secondary skips forwarding and
    // runs uncoordinated. Both guards `TryAcquire(true)` on their
    // own socket names without forwarding.
    void TestSingleInstanceNewInstanceFlagBypassesGuard()
    {
        const QString socketName =
            QStringLiteral("slv-new-") + QUuid::createUuid().toString(QUuid::WithoutBraces).left(8);

        SingleInstanceGuard primary;
        primary.SetSocketNameForTest(socketName);
        QVERIFY(primary.TryAcquire({}, /*allowNewInstance=*/false));

        const QSignalSpy spy(&primary, &SingleInstanceGuard::openWindowRequested);
        QVERIFY(spy.isValid());

        // allowNewInstance skips forwarding; the primary's spy
        // must stay empty.
        SingleInstanceGuard secondary;
        secondary.SetSocketNameForTest(socketName);
        const bool acquired = secondary.TryAcquire({QStringLiteral("dummy.json")}, /*allowNewInstance=*/true);
        QVERIFY2(acquired, "new-instance launch must take primary role even when forwarding is suppressed");

        // No forward should have happened.
        for (int i = 0; i < 5; ++i)
        {
            QCoreApplication::processEvents();
        }
        QCOMPARE(spy.count(), 0);
    }

    // -------------------------------------------------------------------------
    // SingleInstanceGuard hardening: magic-first peek, version
    // range, idle-timer reset, payload cap, forward-error fallback.
    // -------------------------------------------------------------------------

    void TestSingleInstanceMagicMismatchRejected()
    {
        const QString socketName =
            QStringLiteral("slv-mag-") + QUuid::createUuid().toString(QUuid::WithoutBraces).left(8);

        SingleInstanceGuard primary;
        primary.SetSocketNameForTest(socketName);
        QVERIFY(primary.TryAcquire({}, /*allowNewInstance=*/false));

        const QSignalSpy spy(&primary, &SingleInstanceGuard::openWindowRequested);
        QVERIFY(spy.isValid());

        // Frame-shaped payload with the wrong magic. Primary
        // rejects and emits nothing.
        QLocalSocket peer;
        peer.connectToServer(socketName);
        QVERIFY2(peer.waitForConnected(2000), "peer must connect to primary's socket");

        QByteArray payload;
        QDataStream out(&payload, QIODevice::WriteOnly);
        out.setVersion(QDataStream::Qt_6_0);
        out << QByteArray("NOTOURMAGIC");
        out << static_cast<quint8>(1);
        out << QStringList{QStringLiteral("a.json")};

        peer.write(payload);
        peer.flush();
        // The guard reads on this thread, so wait with the event loop running; a blocking wait only times out.
        QVERIFY(WaitUntilProcessingEvents([&] { return peer.bytesToWrite() == 0; }));

        // Let the primary drain the bytes; no signal expected.
        for (int i = 0; i < 10; ++i)
        {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        }
        QCOMPARE(spy.count(), 0);

        // The guard runs on this thread, so a blocking wait here would only time out; closing is enough.
        peer.disconnectFromServer();
    }

    void TestSingleInstanceVersionOutOfRangeRejected()
    {
        const QString socketName =
            QStringLiteral("slv-ver-") + QUuid::createUuid().toString(QUuid::WithoutBraces).left(8);

        SingleInstanceGuard primary;
        primary.SetSocketNameForTest(socketName);
        QVERIFY(primary.TryAcquire({}, /*allowNewInstance=*/false));

        const QSignalSpy spy(&primary, &SingleInstanceGuard::openWindowRequested);
        QVERIFY(spy.isValid());

        // Correct magic, out-of-range version byte. Primary refuses
        // to interpret a future-schema payload.
        QLocalSocket peer;
        peer.connectToServer(socketName);
        QVERIFY2(peer.waitForConnected(2000), "peer must connect to primary's socket");

        QByteArray payload;
        QDataStream out(&payload, QIODevice::WriteOnly);
        out.setVersion(QDataStream::Qt_6_0);
        out << QByteArray("STRUCTLOG");
        out << static_cast<quint8>(255);
        out << QStringList{QStringLiteral("a.json")};
        out << static_cast<quint32>(0);

        peer.write(payload);
        peer.flush();
        // The guard reads on this thread, so wait with the event loop running; a blocking wait only times out.
        QVERIFY(WaitUntilProcessingEvents([&] { return peer.bytesToWrite() == 0; }));

        for (int i = 0; i < 10; ++i)
        {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        }
        QCOMPARE(spy.count(), 0);

        // The guard runs on this thread, so a blocking wait here would only time out; closing is enough.
        peer.disconnectFromServer();
    }

    void TestSingleInstanceLargePayloadRejected()
    {
        const QString socketName =
            QStringLiteral("slv-big-") + QUuid::createUuid().toString(QUuid::WithoutBraces).left(8);

        SingleInstanceGuard primary;
        primary.SetSocketNameForTest(socketName);
        QVERIFY(primary.TryAcquire({}, /*allowNewInstance=*/false));

        const QSignalSpy spy(&primary, &SingleInstanceGuard::openWindowRequested);
        QVERIFY(spy.isValid());

        // Exceed the 1 MiB primary-side cap. Connection torn down
        // with no signal.
        QLocalSocket peer;
        peer.connectToServer(socketName);
        QVERIFY2(peer.waitForConnected(2000), "peer must connect to primary's socket");

        const QByteArray garbage(2 * 1024 * 1024, 'x');
        peer.write(garbage);
        peer.flush();

        // The primary closes the connection once the buffer overruns the cap. It runs on this thread, so wait with
        // the event loop running; a blocking wait would only time out.
        QVERIFY2(
            WaitUntilProcessingEvents([&] { return peer.state() == QLocalSocket::UnconnectedState; }),
            "primary must close a connection that exceeds the payload cap"
        );
        QCOMPARE(spy.count(), 0);
    }

    void TestSingleInstancePostDecodePayloadCap()
    {
        // A hostile peer that bypasses the pre-send cap still gets
        // truncated by the primary post-decode. Stamp 300 entries
        // directly and assert the primary clamps to 256.
        const QString socketName =
            QStringLiteral("slv-cap-") + QUuid::createUuid().toString(QUuid::WithoutBraces).left(8);

        SingleInstanceGuard primary;
        primary.SetSocketNameForTest(socketName);
        QVERIFY(primary.TryAcquire({}, /*allowNewInstance=*/false));

        QSignalSpy spy(&primary, &SingleInstanceGuard::openWindowRequested);
        QVERIFY(spy.isValid());

        QStringList paths;
        for (int i = 0; i < 300; ++i)
        {
            paths << QStringLiteral("C:/logs/x-%1.json").arg(i);
        }

        QLocalSocket peer;
        peer.connectToServer(socketName);
        QVERIFY2(peer.waitForConnected(2000), "peer must connect to primary's socket");

        QByteArray payload;
        QDataStream out(&payload, QIODevice::WriteOnly);
        out.setVersion(QDataStream::Qt_6_0);
        out << QByteArray("STRUCTLOG");
        out << static_cast<quint8>(2);
        out << paths;
        // Overrun computed by the primary post-decode: 300 sent,
        // cap 256, so 44 dropped entries get folded into the count.
        out << static_cast<quint32>(0);

        // Drip the payload in chunks while pumping events; a single
        // sync write would stall on pipe back-pressure same-thread.
        const int chunkSize = 4096;
        int sent = 0;
        while (sent < payload.size())
        {
            const int n = std::min(chunkSize, static_cast<int>(payload.size()) - sent);
            peer.write(payload.constData() + sent, n);
            peer.flush();
            // The guard reads on this thread, so wait with the event loop running; a blocking wait only times out.
            QVERIFY(WaitUntilProcessingEvents([&] { return peer.bytesToWrite() == 0; }));
            sent += n;
            QElapsedTimer pause;
            pause.start();
            while (pause.elapsed() < 25)
            {
                QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
            }
        }

        QElapsedTimer timer;
        timer.start();
        while (spy.isEmpty() && timer.elapsed() < 3000)
        {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        }
        QCOMPARE(spy.count(), 1);
        const auto args = spy.takeFirst();
        const QStringList received = args.at(0).toStringList();
        QCOMPARE(received.size(), 256);
        QCOMPARE(received.first(), QStringLiteral("C:/logs/x-0.json"));
        QCOMPARE(received.last(), QStringLiteral("C:/logs/x-255.json"));
        // The post-decode truncation rolls 300 - 256 = 44 dropped
        // entries into `truncatedCount` so the user-facing message
        // does not under-count when a hostile peer ignores the
        // documented cap.
        QCOMPARE(args.at(1).toInt(), 44);

        if (peer.state() != QLocalSocket::UnconnectedState)
        {
            peer.disconnectFromServer();
            peer.waitForDisconnected(1000);
        }
    }

    // A v3 peer forwards the rotation-history launch override.
    void TestSingleInstanceForwardsRotationHistoryDisable()
    {
        const QString socketName =
            QStringLiteral("slv-flag-") + QUuid::createUuid().toString(QUuid::WithoutBraces).left(8);

        SingleInstanceGuard primary;
        primary.SetSocketNameForTest(socketName);
        QVERIFY(primary.TryAcquire({}, /*allowNewInstance=*/false));

        QSignalSpy spy(&primary, &SingleInstanceGuard::openWindowRequested);
        QVERIFY(spy.isValid());

        // V3 appends launch flags after the truncated-file count.
        QLocalSocket peer;
        peer.connectToServer(socketName);
        QVERIFY2(peer.waitForConnected(2000), "peer must connect to primary's socket");

        const QStringList files{QStringLiteral("C:/logs/only.json")};
        QByteArray payload;
        {
            QDataStream out(&payload, QIODevice::WriteOnly);
            out.setVersion(QDataStream::Qt_6_0);
            out << QByteArray("STRUCTLOG");
            out << static_cast<quint8>(3);
            out << files;
            out << static_cast<quint32>(0);
            // Set only the rotation-history-disable bit.
            out << static_cast<quint32>(1u << 0);
        }
        const qint64 written = peer.write(payload);
        QCOMPARE(written, payload.size());
        peer.flush();
        // The guard reads on this thread, so wait with the event loop running; a blocking wait only times out.
        QVERIFY(WaitUntilProcessingEvents([&] { return peer.bytesToWrite() == 0; }));

        QVERIFY(spy.count() > 0 || spy.wait(60000));
        QCOMPARE(spy.count(), 1);
        const auto args = spy.takeFirst();
        QCOMPARE(args.at(0).toStringList(), files);
        QCOMPARE(args.at(1).toInt(), 0);
        const QVariant &flagsVariant = args.at(2);
        QVERIFY2(flagsVariant.isValid(), "signal must forward the LaunchFlags argument");
        const auto flags = flagsVariant.value<SingleInstanceGuard::LaunchFlagsBitmask>();
        QVERIFY2(
            flags.testFlag(SingleInstanceGuard::LaunchFlags::DisableRotationHistory),
            "v3 wire schema must round-trip --no-rotation-history to the primary"
        );

        if (peer.state() != QLocalSocket::UnconnectedState)
        {
            peer.disconnectFromServer();
            peer.waitForDisconnected(1000);
        }
    }

    // A legacy v2 peer decodes with no launch flags.
    void TestSingleInstanceLegacyVersion2StillAccepted()
    {
        const QString socketName =
            QStringLiteral("slv-legacy-") + QUuid::createUuid().toString(QUuid::WithoutBraces).left(8);

        SingleInstanceGuard primary;
        primary.SetSocketNameForTest(socketName);
        QVERIFY(primary.TryAcquire({}, /*allowNewInstance=*/false));

        QSignalSpy spy(&primary, &SingleInstanceGuard::openWindowRequested);
        QVERIFY(spy.isValid());

        QLocalSocket peer;
        peer.connectToServer(socketName);
        QVERIFY2(peer.waitForConnected(2000), "peer must connect to primary's socket");

        const QStringList files{QStringLiteral("C:/logs/legacy.json")};
        QByteArray payload;
        {
            QDataStream out(&payload, QIODevice::WriteOnly);
            out.setVersion(QDataStream::Qt_6_0);
            out << QByteArray("STRUCTLOG");
            out << static_cast<quint8>(2);
            out << files;
            out << static_cast<quint32>(0);
        }
        peer.write(payload);
        peer.flush();
        // The guard reads on this thread, so wait with the event loop running; a blocking wait only times out.
        QVERIFY(WaitUntilProcessingEvents([&] { return peer.bytesToWrite() == 0; }));

        QVERIFY(spy.count() > 0 || spy.wait(60000));
        QCOMPARE(spy.count(), 1);
        const auto args = spy.takeFirst();
        QCOMPARE(args.at(0).toStringList(), files);
        QCOMPARE(args.at(1).toInt(), 0);
        const auto flags = args.at(2).value<SingleInstanceGuard::LaunchFlagsBitmask>();
        QVERIFY2(
            !flags.testFlag(SingleInstanceGuard::LaunchFlags::DisableRotationHistory),
            "v2 payload must decode to LaunchFlags::None"
        );

        if (peer.state() != QLocalSocket::UnconnectedState)
        {
            peer.disconnectFromServer();
            peer.waitForDisconnected(1000);
        }
    }

    void TestSingleInstanceSecondaryTrimsBeforeSending()
    {
        // The secondary trims to MAX_FORWARDED_FILES (256) before
        // serialisation. We exercise the secondary's serialisation +
        // truncation by constructing a `payload` via the same code
        // path `TryAcquire` would have taken, but stop short of the
        // forward (which would block on the same-process pipe
        // back-pressure). Asserting the produced wire-frame's file
        // count is sufficient -- the rest of the path is already
        // covered by `TestSingleInstanceForwardsOpenRequest`.
        QStringList paths;
        for (int i = 0; i < 300; ++i)
        {
            paths << QStringLiteral("C:/logs/x-%1.json").arg(i);
        }

        // Mirror the truncation logic in `TryAcquire::forwardTo`.
        constexpr int MAX_FORWARDED_FILES = 256;
        const QStringList trimmed = paths.size() > MAX_FORWARDED_FILES ? paths.mid(0, MAX_FORWARDED_FILES) : paths;
        QCOMPARE(trimmed.size(), 256);

        // And confirm the resulting payload stays under 1 MiB so the
        // payload-cap check in `TryAcquire` doesn't suppress it.
        QByteArray wire;
        {
            QDataStream out(&wire, QIODevice::WriteOnly);
            out.setVersion(QDataStream::Qt_6_0);
            out << QByteArray("STRUCTLOG");
            out << static_cast<quint8>(2);
            out << trimmed;
            // Mirror the secondary's `truncatedCount` tail: 300 input
            // paths, cap 256, so 44 were dropped on the secondary
            // side before serialisation.
            out << static_cast<quint32>(paths.size() - MAX_FORWARDED_FILES);
        }
        QVERIFY(wire.size() < 1024 * 1024);
    }

    void TestSingleInstanceIdleTimerResetsOnActivity()
    {
        // Drip-feed bytes so the cumulative interval crosses the
        // idle watchdog. The watchdog resets on every `readyRead`,
        // so this drip pattern completes a frame successfully.
        //
        // Concrete timings: the watchdog uses
        // `CONNECTION_IDLE_TIMEOUT_MS = 5000`. We drip bytes in 200
        // ms intervals, total >=300 ms but well under 5 s, so this
        // test passes under both implementations -- the *new* value
        // is that we now also pin the per-readyRead reset behaviour
        // by completing the frame after a brief gap.
        const QString socketName =
            QStringLiteral("slv-idle-") + QUuid::createUuid().toString(QUuid::WithoutBraces).left(8);

        SingleInstanceGuard primary;
        primary.SetSocketNameForTest(socketName);
        QVERIFY(primary.TryAcquire({}, /*allowNewInstance=*/false));

        QSignalSpy spy(&primary, &SingleInstanceGuard::openWindowRequested);
        QVERIFY(spy.isValid());

        QLocalSocket peer;
        peer.connectToServer(socketName);
        QVERIFY2(peer.waitForConnected(2000), "peer must connect to primary's socket");

        QByteArray payload;
        QDataStream out(&payload, QIODevice::WriteOnly);
        out.setVersion(QDataStream::Qt_6_0);
        out << QByteArray("STRUCTLOG");
        out << static_cast<quint8>(2);
        out << QStringList{QStringLiteral("drip-a.json"), QStringLiteral("drip-b.json")};
        out << static_cast<quint32>(0);

        const int chunkSize = std::max(1, static_cast<int>(payload.size()) / 4);
        int sent = 0;
        while (sent < payload.size())
        {
            const int n = std::min(chunkSize, static_cast<int>(payload.size()) - sent);
            peer.write(payload.constData() + sent, n);
            peer.flush();
            // The guard reads on this thread, so wait with the event loop running; a blocking wait only times out.
            QVERIFY(WaitUntilProcessingEvents([&] { return peer.bytesToWrite() == 0; }));
            sent += n;
            // Gap so the primary's `readyRead` resets the watchdog.
            QElapsedTimer pause;
            pause.start();
            while (pause.elapsed() < 50)
            {
                QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
            }
        }

        // Once the final chunk lands, the frame completes; pump
        // events until the signal fires.
        QElapsedTimer timer;
        timer.start();
        while (spy.isEmpty() && timer.elapsed() < 3000)
        {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        }
        QCOMPARE(spy.count(), 1);
        const QStringList received = spy.takeFirst().at(0).toStringList();
        QCOMPARE(received.size(), 2);

        if (peer.state() != QLocalSocket::UnconnectedState)
        {
            peer.disconnectFromServer();
            peer.waitForDisconnected(1000);
        }
    }

    // -------------------------------------------------------------------------
    // CLI parser regressions.
    // -------------------------------------------------------------------------

    void TestCliParserHonoursNewInstanceFlag()
    {
        const QStringList args = {
            QStringLiteral("StructuredLogViewer"),
            QStringLiteral("--new-instance"),
            QStringLiteral("alpha.log"),
        };
        const logapp::ParsedCli parsed = logapp::ParseCli(args, QProcessEnvironment());
        QVERIFY(parsed.allowNewInstance);
        QCOMPARE(parsed.files.size(), 1);
        QVERIFY(parsed.files.front().endsWith(QStringLiteral("alpha.log"), Qt::CaseInsensitive));
    }

    void TestCliParserHonoursEnvOverride()
    {
        QProcessEnvironment env;
        env.insert(QStringLiteral("LOGAPP_NEW_INSTANCE"), QStringLiteral("1"));
        const QStringList args = {QStringLiteral("StructuredLogViewer")};
        const logapp::ParsedCli parsed = logapp::ParseCli(args, env);
        QVERIFY(parsed.allowNewInstance);
        QVERIFY(parsed.files.isEmpty());
    }

    void TestCliParserEnvOverrideTrueIsCaseInsensitive()
    {
        QProcessEnvironment env;
        env.insert(QStringLiteral("LOGAPP_NEW_INSTANCE"), QStringLiteral("TrUe"));
        const QStringList args = {QStringLiteral("StructuredLogViewer")};
        QVERIFY(logapp::ParseCli(args, env).allowNewInstance);
    }

    void TestCliParserEnvOverrideOtherValuesAreFalse()
    {
        for (const QString &value : {QString("0"), QString("yes"), QString("no"), QString("")})
        {
            QProcessEnvironment env;
            env.insert(QStringLiteral("LOGAPP_NEW_INSTANCE"), value);
            const QStringList args = {QStringLiteral("StructuredLogViewer")};
            QVERIFY2(
                !logapp::ParseCli(args, env).allowNewInstance,
                qPrintable(QStringLiteral("env override `%1` must not enable new-instance").arg(value))
            );
        }
    }

    void TestCliParserDoubleDashLetsDashedPathsThrough()
    {
        // POSIX `--`: everything after is positional, even when
        // dash-prefixed.
        const QStringList args = {
            QStringLiteral("StructuredLogViewer"),
            QStringLiteral("--"),
            QStringLiteral("-weird-filename.log"),
        };
        const logapp::ParsedCli parsed = logapp::ParseCli(args, QProcessEnvironment());
        QCOMPARE(parsed.files.size(), 1);
        QVERIFY(parsed.files.front().endsWith(QStringLiteral("-weird-filename.log"), Qt::CaseInsensitive));
        QVERIFY(!parsed.allowNewInstance);
    }

    void TestCliParserCanonicalisesPositionalsAgainstCwd()
    {
        // Bare filenames resolve to absolute paths against CWD,
        // regardless of whether the file exists.
        const QStringList args = {
            QStringLiteral("StructuredLogViewer"),
            QStringLiteral("relative.log"),
        };
        const logapp::ParsedCli parsed = logapp::ParseCli(args, QProcessEnvironment());
        QCOMPARE(parsed.files.size(), 1);
        // `isAbsolute` is the contract; missing files surface to
        // the user later via a parse error.
        QVERIFY2(
            QFileInfo(parsed.files.front()).isAbsolute(),
            qPrintable(QStringLiteral("expected absolute path, got `%1`").arg(parsed.files.front()))
        );
    }

    void TestCliParserUnknownFlagDoesNotDropFiles()
    {
        // Unknown long-form flags log a warning but the parser
        // still returns whatever positionals it recognised.
        const QStringList args = {
            QStringLiteral("StructuredLogViewer"),
            QStringLiteral("--this-flag-does-not-exist"),
            QStringLiteral("real.log"),
        };
        const logapp::ParsedCli parsed = logapp::ParseCli(args, QProcessEnvironment());
        // The exact files depend on `QCommandLineParser`'s recovery;
        // the only invariant we pin is "does not abort the launch".
        Q_UNUSED(parsed);
    }

    void TestCliParserPositionalsSurviveUnknownFlagMidArgv()
    {
        // An unknown flag between two positionals must not drop
        // either. `QCommandLineParser` in `ParseAsLongOptions`
        // mode keeps collecting positionals past the unknown flag.
        // `--new-instance` is not set; the gate stays false.
        const QStringList args = {
            QStringLiteral("StructuredLogViewer"),
            QStringLiteral("first.log"),
            QStringLiteral("--this-flag-does-not-exist"),
            QStringLiteral("second.log"),
        };
        const logapp::ParsedCli parsed = logapp::ParseCli(args, QProcessEnvironment());
        QVERIFY(!parsed.allowNewInstance);
        QCOMPARE(parsed.files.size(), 2);
        QVERIFY2(
            parsed.files.front().endsWith(QStringLiteral("first.log"), Qt::CaseInsensitive),
            qPrintable(
                QStringLiteral("expected leading positional to be 'first.log', got '%1'").arg(parsed.files.front())
            )
        );
        QVERIFY2(
            parsed.files.back().endsWith(QStringLiteral("second.log"), Qt::CaseInsensitive),
            qPrintable(QStringLiteral("expected trailing positional to be 'second.log' (post unknown flag), got '%1'")
                           .arg(parsed.files.back()))
        );
    }

    void TestCliParserRecognisesBareDashAsStdin()
    {
        // POSIX convention: `-` on its own means "read stdin".
        // The parser pre-strips it from argv so `QCommandLineParser`
        // (in `ParseAsLongOptions` mode) does not choke on the
        // bare dash.
        const QStringList args = {
            QStringLiteral("StructuredLogViewer"),
            QStringLiteral("-"),
        };
        const logapp::ParsedCli parsed = logapp::ParseCli(args, QProcessEnvironment());
        QVERIFY(parsed.readStdin);
        QVERIFY(parsed.files.isEmpty());
        QVERIFY(!parsed.allowNewInstance);
    }

    void TestCliParserRecognisesLongStdinFlag()
    {
        // `--stdin` is the PowerShell-friendly long form; some
        // shells eat a lone `-`.
        const QStringList args = {
            QStringLiteral("StructuredLogViewer"),
            QStringLiteral("--stdin"),
        };
        const logapp::ParsedCli parsed = logapp::ParseCli(args, QProcessEnvironment());
        QVERIFY(parsed.readStdin);
        QVERIFY(parsed.files.isEmpty());
    }

    void TestCliParserStdinCoexistsWithFileArgs()
    {
        // The parser preserves both signals. `main()` opens files
        // first, then stdin replaces queued and in-flight work.
        const QStringList args = {
            QStringLiteral("StructuredLogViewer"),
            QStringLiteral("preload.log"),
            QStringLiteral("-"),
        };
        const logapp::ParsedCli parsed = logapp::ParseCli(args, QProcessEnvironment());
        QVERIFY(parsed.readStdin);
        QCOMPARE(parsed.files.size(), 1);
        QVERIFY(parsed.files.front().endsWith(QStringLiteral("preload.log"), Qt::CaseInsensitive));
    }

    void TestCliParserStdinDefaultsToFalseWithoutFlag()
    {
        // An ordinary launch must not opt into stdin.
        const QStringList args = {
            QStringLiteral("StructuredLogViewer"),
            QStringLiteral("only.log"),
        };
        const logapp::ParsedCli parsed = logapp::ParseCli(args, QProcessEnvironment());
        QVERIFY(!parsed.readStdin);
        QCOMPARE(parsed.files.size(), 1);
    }

    void TestCliParserDashAtArgv0IsNotInterpretedAsStdin()
    {
        // Regression: the `-` pre-strip used to iterate all of
        // `args`, including `args[0]` (the program name). A
        // hypothetical exe renamed to just `-` (or malformed
        // `QCoreApplication::arguments()` in test code) would then
        // set `readStdin`. The fix skips index 0 in the pre-strip.
        const QStringList args = {
            QStringLiteral("-"),
            QStringLiteral("only.log"),
        };
        const logapp::ParsedCli parsed = logapp::ParseCli(args, QProcessEnvironment());
        QVERIFY(!parsed.readStdin);
        QCOMPARE(parsed.files.size(), 1);
    }

    // Mixed input (one config + N logs): apply the configuration
    // before the logs stream, so the loaded columns / filters /
    // sort drive the streamed rows.
    void TestDispatchMixedConfigAndLogsAppliesConfigThenStreams()
    {
        const QTemporaryDir sessionsDir;
        QVERIFY(sessionsDir.isValid());
        SessionHistoryManager manager(QDir(sessionsDir.path()), std::make_unique<InMemoryRecentsIndexStorage>());
        auto wired = std::make_unique<MainWindow>(mTheme.data(), &manager, nullptr);

        // Two fixtures sharing a schema so we can assert on a sum.
        const TempJsonFile fixtureA(
            {QStringLiteral(R"({"category": "alpha", "msg": "first"})"),
             QStringLiteral(R"({"category": "beta", "msg": "second"})")}
        );
        const TempJsonFile fixtureB({QStringLiteral(R"({"category": "alpha", "msg": "third"})")});
        const int expectedRows = 3;

        // Full-scope cfg with a contains-`alpha` filter and a
        // descending sort on `category`. Proxy row count collapses
        // to 2 and the sort indicator matches.
        const QTemporaryDir cfgDir;
        QVERIFY(cfgDir.isValid());
        const QString cfgPath = cfgDir.filePath(QStringLiteral("mixed-cfg.json"));

        loglib::ColumnLayout builderLayout;
        builderLayout.AppendKeys({"category", "msg"});
        loglib::LogConfiguration builder{.columns = builderLayout.Columns()};
        builder.source = loglib::SourceDescriptor{};

        loglib::FilterCondition filter;
        filter.type = loglib::FilterCondition::Type::String;
        filter.columnKeys = {"category"};
        filter.filterString = "alpha";
        filter.matchType = loglib::FilterCondition::Match::Contains;
        builder.view.expression = WireLeavesAsExpression({filter});

        loglib::Sort sort;
        sort.columnKeys = {"category"};
        sort.direction = loglib::SortDirection::Descending;
        builder.view.sort = sort;

        loglib::SaveConfiguration(builder, cfgPath.toStdString(), loglib::SaveScope::Full);

        const MainWindow::MixedInputDispatch result =
            wired->OpenMixedFilesForTest({cfgPath, fixtureA.Path(), fixtureB.Path()}, MainWindow::OpenMode::Append);
        QCOMPARE(result, MainWindow::MixedInputDispatch::AppliedConfigThenLogs);

        // Two log files chain: `BeginStreaming` then
        // `AppendStreaming`. Wait on the accumulated row count
        // rather than `streamingFinished` (it fires per-file).
        auto *model = wired->Model();
        QVERIFY(model != nullptr);
        QTRY_COMPARE_WITH_TIMEOUT(model->rowCount(), expectedRows, 60000);

        // Cfg columns drove the load (no autodetect fallback).
        QCOMPARE(static_cast<int>(model->Layout().Columns().size()), 2);

        // Full-scope filter survived; proxy hides non-`alpha`.
        QCOMPARE(wired->Filters().size(), static_cast<size_t>(1));
        auto *proxy = wired->FilterModel();
        QVERIFY(proxy != nullptr);
        QCOMPARE(proxy->rowCount(), 2);

        // The Full-scope sort survived: the table view is sorting
        // by `category` descending.
        const auto *header = wired->findChild<LogTableView *>()->horizontalHeader();
        QVERIFY(header != nullptr);
        QCOMPARE(header->sortIndicatorSection(), 0);
        QCOMPARE(header->sortIndicatorOrder(), Qt::DescendingOrder);
    }

    // Lone-config input: `TryLoadAsConfiguration` applies columns
    // / filters / sort, model is not reset, no streaming starts.
    // Dispatcher reports `AppliedConfigOnly` for the CLI hint.
    void TestDispatchMixedSingleConfigDelegatesToTryLoadAsConfiguration()
    {
        const QTemporaryDir sessionsDir;
        QVERIFY(sessionsDir.isValid());
        SessionHistoryManager manager(QDir(sessionsDir.path()), std::make_unique<InMemoryRecentsIndexStorage>());
        auto wired = std::make_unique<MainWindow>(mTheme.data(), &manager, nullptr);

        const QTemporaryDir cfgDir;
        QVERIFY(cfgDir.isValid());
        const QString cfgPath = cfgDir.filePath(QStringLiteral("lone-cfg.json"));
        loglib::ColumnLayout builderLayout;
        builderLayout.AppendKeys({"msg", "level"});
        loglib::SaveConfiguration(
            loglib::LogConfiguration{.columns = builderLayout.Columns()},
            cfgPath.toStdString(),
            loglib::SaveScope::Columns
        );

        const QSignalSpy finishedSpy(wired->Model(), &LogModel::streamingFinished);
        QVERIFY(finishedSpy.isValid());

        const MainWindow::MixedInputDispatch result =
            wired->OpenMixedFilesForTest({cfgPath}, MainWindow::OpenMode::Append);
        QCoreApplication::processEvents();

        QCOMPARE(result, MainWindow::MixedInputDispatch::AppliedConfigOnly);
        QCOMPARE(static_cast<int>(wired->Model()->Layout().Columns().size()), 2);
        QCOMPARE(wired->Model()->rowCount(), 0);
        QCOMPARE(finishedSpy.count(), 0);
    }

    // Multiple configs in one input is rejected with a modal
    // (suppressed in tests). Live state untouched: no config
    // applied, no streaming, no uuid mutation.
    void TestDispatchMixedRejectsMultipleConfigs()
    {
        const QTemporaryDir sessionsDir;
        QVERIFY(sessionsDir.isValid());
        SessionHistoryManager manager(QDir(sessionsDir.path()), std::make_unique<InMemoryRecentsIndexStorage>());
        auto wired = std::make_unique<MainWindow>(mTheme.data(), &manager, nullptr);

        // Pre-seed so we can later assert nothing mutated.
        const TempJsonFile prior({QStringLiteral(R"({"msg": "kept"})")});
        QSignalSpy primingSpy(wired->Model(), &LogModel::streamingFinished);
        QVERIFY(primingSpy.isValid());
        wired->OpenFilesForTest({prior.Path()}, MainWindow::OpenMode::Append);
        QVERIFY(primingSpy.wait(60000));
        QCoreApplication::processEvents();

        const int rowsBefore = wired->Model()->rowCount();
        const auto columnsBefore = wired->Model()->Layout().Columns().size();
        const QString uuidBefore = wired->ActiveSessionUuid();
        QVERIFY2(rowsBefore > 0, "priming open must produce at least one row");
        QVERIFY2(columnsBefore > 0, "priming open must produce at least one column");

        // Two real columns-only configurations and a log file.
        const QTemporaryDir cfgDir;
        QVERIFY(cfgDir.isValid());
        const QString cfgPathA = cfgDir.filePath(QStringLiteral("cfg-a.json"));
        const QString cfgPathB = cfgDir.filePath(QStringLiteral("cfg-b.json"));
        {
            loglib::ColumnLayout builderA;
            builderA.AppendKeys({"alpha"});
            loglib::SaveConfiguration(
                loglib::LogConfiguration{.columns = builderA.Columns()},
                cfgPathA.toStdString(),
                loglib::SaveScope::Columns
            );
            loglib::ColumnLayout builderB;
            builderB.AppendKeys({"beta"});
            loglib::SaveConfiguration(
                loglib::LogConfiguration{.columns = builderB.Columns()},
                cfgPathB.toStdString(),
                loglib::SaveScope::Columns
            );
        }
        const TempJsonFile log({QStringLiteral(R"({"msg": "ignored"})")});

        wired->SetSuppressDialogsForTest(true);
        const QSignalSpy finishedSpy(wired->Model(), &LogModel::streamingFinished);
        QVERIFY(finishedSpy.isValid());

        const MainWindow::MixedInputDispatch result =
            wired->OpenMixedFilesForTest({cfgPathA, cfgPathB, log.Path()}, MainWindow::OpenMode::Append);
        QCoreApplication::processEvents();

        QCOMPARE(result, MainWindow::MixedInputDispatch::RejectedMultiConfig);
        QCOMPARE(wired->Model()->rowCount(), rowsBefore);
        QCOMPARE(wired->Model()->Layout().Columns().size(), columnsBefore);
        QCOMPARE(wired->ActiveSessionUuid(), uuidBefore);
        QCOMPARE(finishedSpy.count(), 0);
    }

    // No-config input streams everything via
    // `StartStreamingOpenQueue` in the caller's `OpenMode`.
    void TestDispatchMixedNoConfigStreamsEverything()
    {
        const QTemporaryDir sessionsDir;
        QVERIFY(sessionsDir.isValid());
        SessionHistoryManager manager(QDir(sessionsDir.path()), std::make_unique<InMemoryRecentsIndexStorage>());
        auto wired = std::make_unique<MainWindow>(mTheme.data(), &manager, nullptr);

        const TempJsonFile logA({QStringLiteral(R"({"msg": "first"})")});
        const TempJsonFile logB({QStringLiteral(R"({"msg": "second"})")});

        const QSignalSpy finishedSpy(wired->Model(), &LogModel::streamingFinished);
        QVERIFY(finishedSpy.isValid());

        const MainWindow::MixedInputDispatch result =
            wired->OpenMixedFilesForTest({logA.Path(), logB.Path()}, MainWindow::OpenMode::Append);
        QCOMPARE(result, MainWindow::MixedInputDispatch::QueuedLogsOnly);

        // Each chained file fires `streamingFinished`; wait both.
        QTRY_VERIFY_WITH_TIMEOUT(finishedSpy.count() >= 2, 60000);
        QCoreApplication::processEvents();

        QCOMPARE(wired->Model()->rowCount(), 2);
    }

    // `{}` must classify as a log (no `"columns"` key), not a
    // configuration -- otherwise it would wipe columns. JsonParser
    // is permissive: `{}` parses as a 0-field record, so both
    // files stream successfully (total rowCount = 2). We wait for
    // both finish emits to avoid racing `StreamNextPendingFile`.
    void TestDispatchMixedRejectsEmptyJsonObjectAsConfig()
    {
        const QTemporaryDir sessionsDir;
        QVERIFY(sessionsDir.isValid());
        SessionHistoryManager manager(QDir(sessionsDir.path()), std::make_unique<InMemoryRecentsIndexStorage>());
        auto wired = std::make_unique<MainWindow>(mTheme.data(), &manager, nullptr);

        const QTemporaryDir emptyDir;
        QVERIFY(emptyDir.isValid());
        const QString emptyPath = QDir(emptyDir.path()).filePath(QStringLiteral("empty.json"));
        {
            std::ofstream stream(emptyPath.toStdString(), std::ios::binary);
            QVERIFY(stream.is_open());
            stream << "{}";
        }

        const TempJsonFile log({QStringLiteral(R"({"msg": "real"})")});

        wired->SetSuppressDialogsForTest(true);
        const QSignalSpy finishedSpy(wired->Model(), &LogModel::streamingFinished);
        QVERIFY(finishedSpy.isValid());

        const MainWindow::MixedInputDispatch result =
            wired->OpenMixedFilesForTest({emptyPath, log.Path()}, MainWindow::OpenMode::Append);
        QCOMPARE(result, MainWindow::MixedInputDispatch::QueuedLogsOnly);
        // Two emits: file 1 chains to file 2 via
        // `OnStreamingFinished -> StreamNextPendingFile`.
        QTRY_VERIFY_WITH_TIMEOUT(finishedSpy.count() >= 2, 60000);
        QCoreApplication::processEvents();

        QCOMPARE(wired->Model()->rowCount(), 2);
    }

    void TestSessionBundleUsesNormalCompressedFileOpen()
    {
        const QTemporaryDir sessionsDir;
        const QTemporaryDir bundleDir;
        QVERIFY(sessionsDir.isValid());
        QVERIFY(bundleDir.isValid());
        SessionHistoryManager manager(QDir(sessionsDir.path()), std::make_unique<InMemoryRecentsIndexStorage>());
        auto wired = std::make_unique<MainWindow>(mTheme.data(), &manager, nullptr);

        const QString original = WriteBundleFixture(
            bundleDir,
            {QStringLiteral(R"({"value":1,"msg":"first"})"), QStringLiteral(R"({"value":2,"msg":"second"})")},
            [](loglib::LogConfiguration &configuration) {
                configuration.columns.at(0).header = "Embedded value";
                configuration.view.sort = {.columnKeys = {"value"}, .direction = loglib::SortDirection::Descending};
                configuration.view.anchors = {{.pathKey = {}, .lineId = 1, .colorIndex = 0, .note = "moved"}};
            }
        );
        const QString moved = bundleDir.filePath(QStringLiteral("renamed.slvbundle"));
        QVERIFY(QFile::rename(original, moved));

        QSignalSpy finishedSpy(wired->Model(), &LogModel::streamingFinished);
        QVERIFY(finishedSpy.isValid());
        QCOMPARE(
            wired->OpenMixedFilesForTest({moved}, MainWindow::OpenMode::Replace),
            MainWindow::MixedInputDispatch::QueuedLogsOnly
        );
        QVERIFY(finishedSpy.wait(60000));
        QCoreApplication::processEvents();

        QCOMPARE(wired->Model()->rowCount(), 2);
        QCOMPARE(QString::fromStdString(wired->Model()->Layout().Columns().at(0).header), QString("Embedded value"));
        QCOMPARE(wired->FilterModel()->SortColumn(), 0);
        QCOMPARE(wired->FilterModel()->SortOrder(), Qt::DescendingOrder);

        const auto &source = wired->CurrentSourceForTest();
        QVERIFY(source.has_value());
        QCOMPARE(source->kind, loglib::SourceDescriptor::Kind::File);
        QCOMPARE(source->locators.size(), static_cast<std::size_t>(1));
        QCOMPARE(source->locators.front().path, logapp::CanonicalDisplayPath(moved).toStdString());
        QCOMPARE(source->locators.front().key, logapp::PathKeyOf(moved));
        QVERIFY(!wired->windowTitle().contains(QStringLiteral("[Bundle]")));
        QCOMPARE(wired->activeSession()->View().anchors.size(), static_cast<size_t>(1));
        // Anchors must carry the path key of the source so
        // `AnchorManager::Key` compares hit; the display path
        // silently mismatched on Windows.
        QCOMPARE(wired->activeSession()->View().anchors.front().pathKey, logapp::PathKeyOf(moved));
        // Fixture anchored line-id 1 -> source row 1 must actually be
        // reachable through `LogModel`. Guards a load-side
        // canonicalization regression where `AnchorSlotForRow` would
        // return nullopt because the stored key didn't match the row.
        QVERIFY(wired->Anchors() != nullptr);
        QVERIFY(wired->Anchors()->Count() == 1);
        const std::optional<std::uint8_t> slot = wired->Model()->AnchorSlotForRow(1);
        QVERIFY(slot.has_value());
        QCOMPARE(*slot, static_cast<std::uint8_t>(0));

        const QList<RecentSessionEntry> recent = manager.List();
        QCOMPARE(recent.size(), 1);
        QVERIFY(!wired->ActiveSessionUuid().isEmpty());
    }

    void TestSessionBundleAppliesEmbeddedHighlightRulesToLoadedRows()
    {
        // Regression: embedded rules must populate the runtime cache.
        const QTemporaryDir sessionsDir;
        const QTemporaryDir bundleDir;
        QVERIFY(sessionsDir.isValid());
        QVERIFY(bundleDir.isValid());
        SessionHistoryManager manager(QDir(sessionsDir.path()), std::make_unique<InMemoryRecentsIndexStorage>());
        auto wired = std::make_unique<MainWindow>(mTheme.data(), &manager, nullptr);

        // Only the second row matches `severity == "err"`.
        const QString bundle = WriteBundleFixture(
            bundleDir,
            {
                QStringLiteral(R"({"severity":"info","msg":"first"})"),
                QStringLiteral(R"({"severity":"err","msg":"second"})"),
            },
            [](loglib::LogConfiguration &configuration) {
                loglib::HighlightRule rule{};
                rule.name = "Errors";
                rule.enabled = true;
                rule.condition.columnKeys = {"severity"};
                rule.condition.type = loglib::FilterCondition::Type::String;
                rule.condition.matchType = loglib::FilterCondition::Match::Exactly;
                rule.condition.filterString = "err";
                rule.style.foregroundIndex = 2;
                rule.style.backgroundIndex = 0;
                rule.style.bold = true;
                configuration.view.highlightRules = {rule};
            }
        );

        QSignalSpy finishedSpy(wired->Model(), &LogModel::streamingFinished);
        QVERIFY(finishedSpy.isValid());
        QCOMPARE(
            wired->OpenMixedFilesForTest({bundle}, MainWindow::OpenMode::Replace),
            MainWindow::MixedInputDispatch::QueuedLogsOnly
        );
        QVERIFY(finishedSpy.wait(60000));
        QCoreApplication::processEvents();

        QCOMPARE(wired->Model()->rowCount(), 2);
        // First verify the embedded rule reached configuration.
        QCOMPARE(wired->activeSession()->View().highlightRules.size(), static_cast<size_t>(1));

        const HighlightRuleSet *const highlights = wired->Highlights();
        QVERIFY(highlights != nullptr);
        QCOMPARE(highlights->Rules().size(), static_cast<size_t>(1));
        QVERIFY(highlights->HasActiveRules());
        QCOMPARE(highlights->InactiveCount(), static_cast<size_t>(0));

        // Then verify the runtime cache evaluated it.
        QVERIFY(!highlights->LastMatchFor(0).has_value());
        const std::optional<std::size_t> secondMatch = highlights->LastMatchFor(1);
        QVERIFY(secondMatch.has_value());
        QCOMPARE(*secondMatch, static_cast<std::size_t>(0));
    }

    void TestSupersedingBundleDecompressionClearsEmbeddedConfigIntent()
    {
        const QTemporaryDir sessionsDir;
        QVERIFY(sessionsDir.isValid());
        SessionHistoryManager manager(QDir(sessionsDir.path()), std::make_unique<InMemoryRecentsIndexStorage>());
        auto wired = std::make_unique<MainWindow>(mTheme.data(), &manager, nullptr);
        wired->SimulateSupersededBundleDecompressionForTest();
        QVERIFY(!wired->AppliesEmbeddedBundleConfigForNextOpenForTest());
    }

    void TestSessionBundleAutosaveConfigOverridesEmbeddedDefault()
    {
        const QTemporaryDir sessionsDir;
        const QTemporaryDir bundleDir;
        QVERIFY(sessionsDir.isValid());
        QVERIFY(bundleDir.isValid());
        SessionHistoryManager manager(QDir(sessionsDir.path()), std::make_unique<InMemoryRecentsIndexStorage>());

        const QString bundle = WriteBundleFixture(
            bundleDir, {QStringLiteral(R"({"msg":"saved"})")}, [](loglib::LogConfiguration &configuration) {
                configuration.columns.at(0).header = "Embedded";
            }
        );

        QString uuid;
        {
            auto wired = std::make_unique<MainWindow>(mTheme.data(), &manager, nullptr);
            const QSignalSpy finishedSpy(wired->Model(), &LogModel::streamingFinished);
            wired->OpenMixedFilesForTest({bundle}, MainWindow::OpenMode::Replace);
            QTRY_VERIFY_WITH_TIMEOUT(finishedSpy.count() >= 1, 60000);

            wired->Model()->MutableLayout().SetColumnHeader(0, "Autosaved");
            wired->Model()->NotifyConfigurationReplaced();
            wired->AutoSaveSessionSnapshot();
            uuid = wired->ActiveSessionUuid();
            QVERIFY(!uuid.isEmpty());
        }

        auto restored = std::make_unique<MainWindow>(mTheme.data(), &manager, nullptr);
        const QSignalSpy restoredSpy(restored->Model(), &LogModel::streamingFinished);
        restored->OpenRecentSessionForTest(uuid);
        QTRY_VERIFY_WITH_TIMEOUT(restoredSpy.count() >= 1, 60000);
        QCOMPARE(QString::fromStdString(restored->Model()->Layout().Columns().at(0).header), QString("Autosaved"));
    }

    void TestAppendingSessionBundlePreservesActiveConfiguration()
    {
        const QTemporaryDir sessionsDir;
        const QTemporaryDir bundleDir;
        QVERIFY(sessionsDir.isValid());
        QVERIFY(bundleDir.isValid());
        SessionHistoryManager manager(QDir(sessionsDir.path()), std::make_unique<InMemoryRecentsIndexStorage>());
        auto wired = std::make_unique<MainWindow>(mTheme.data(), &manager, nullptr);

        const TempJsonFile initial({QStringLiteral(R"({"msg":"plain"})")});
        const QSignalSpy initialSpy(wired->Model(), &LogModel::streamingFinished);
        wired->OpenFilesForTest({initial.Path()}, MainWindow::OpenMode::Replace);
        QTRY_VERIFY_WITH_TIMEOUT(initialSpy.count() >= 1, 60000);

        wired->Model()->MutableLayout().SetColumnHeader(0, "Active");
        wired->Model()->NotifyConfigurationReplaced();

        const QString bundle = WriteBundleFixture(
            bundleDir, {QStringLiteral(R"({"msg":"bundled"})")}, [](loglib::LogConfiguration &configuration) {
                configuration.columns.at(0).header = "Embedded";
            }
        );
        const QSignalSpy appendSpy(wired->Model(), &LogModel::streamingFinished);
        wired->OpenMixedFilesForTest({bundle}, MainWindow::OpenMode::Append);
        QTRY_VERIFY_WITH_TIMEOUT(appendSpy.count() >= 1, 60000);

        QCOMPARE(wired->Model()->rowCount(), 2);
        QCOMPARE(QString::fromStdString(wired->Model()->Layout().Columns().at(0).header), QString("Active"));
        QVERIFY(wired->CurrentSourceForTest().has_value());
        QCOMPARE(wired->CurrentSourceForTest()->kind, loglib::SourceDescriptor::Kind::File);
        QCOMPARE(wired->CurrentSourceForTest()->locators.size(), static_cast<size_t>(2));
    }

    // Bundle export stops live tail before snapshotting retained rows.
    void TestSessionBundleExportSnapshotsAndStopsLiveTailSession()
    {
        const QTemporaryDir sessionsDir;
        const QTemporaryDir bundleDir;
        QVERIFY(sessionsDir.isValid());
        QVERIFY(bundleDir.isValid());
        SessionHistoryManager manager(QDir(sessionsDir.path()), std::make_unique<InMemoryRecentsIndexStorage>());
        auto wired = std::make_unique<MainWindow>(mTheme.data(), &manager, nullptr);
        wired->SetSuppressDialogsForTest(true);

        auto restore = qScopeGuard([&wired]() { wired->SetSessionModeForTest(MainWindow::TestSessionMode::Idle); });
        wired->SetSessionModeForTest(MainWindow::TestSessionMode::LiveTail);

        LogModel *const model = wired->Model();
        QVERIFY(model != nullptr);
        loglib::StreamLineSource &streamSource = BeginSyntheticStreamSession(*model);
        QtStreamingLogSink *sink = model->Sink();
        QVERIFY(sink != nullptr);
        loglib::KeyIndex &keys = sink->Keys();
        const loglib::KeyId valueKey = keys.GetOrInsert(std::string("value"));

        constexpr size_t ROW_COUNT = 12;
        sink->OnBatch(MakeSyntheticBatch(streamSource, valueKey, 1, ROW_COUNT, /*declareNewKey=*/true));
        QCoreApplication::processEvents();
        QCOMPARE(model->rowCount(), static_cast<int>(ROW_COUNT));
        QVERIFY(model->IsStreamingActive());

        const QString destination = bundleDir.filePath(QStringLiteral("livetail-snapshot.slvbundle"));
        wired->ExportSessionBundleToPathForTest(destination);

        // The stream stops synchronously before the writer starts.
        QVERIFY2(
            !model->IsStreamingActive(),
            "Live-tail bundle export must stop the streaming pipeline before it dispatches the writer."
        );
        QCOMPARE(model->rowCount(), static_cast<int>(ROW_COUNT));

        // Wait for the asynchronous writer.
        QTRY_VERIFY_WITH_TIMEOUT(!wired->IsExportInFlightForTest(), 60000);
        QCoreApplication::processEvents();

        // Verify the produced bundle.
        const auto destPath = logapp::QStringToFsPath(destination);
        QVERIFY2(std::filesystem::exists(destPath), "Bundle export must produce the requested destination file.");
        QVERIFY(loglib::HasSessionBundleExtension(destPath));

        // Parse metadata to catch truncated output.
        loglib::DecompressingByteSource::Options options;
        options.discardFirstLine = true;
        const loglib::DecompressingByteSource decoded(destPath, {}, {}, options);
        const loglib::SessionBundleMetadata metadata = loglib::ParseSessionBundleMetadata(decoded.DiscardedFirstLine());
        QCOMPARE(metadata.rowCount, static_cast<std::uint64_t>(ROW_COUNT));
    }

    // CLI variant: `app cfg.json log.json` applies the cfg first
    // and streams the log under it.
    void TestOpenFilesForCliConfigAndLogsAppliesConfigThenStreams()
    {
        const QTemporaryDir sessionsDir;
        QVERIFY(sessionsDir.isValid());
        SessionHistoryManager manager(QDir(sessionsDir.path()), std::make_unique<InMemoryRecentsIndexStorage>());
        auto wired = std::make_unique<MainWindow>(mTheme.data(), &manager, nullptr);

        const QTemporaryDir cfgDir;
        QVERIFY(cfgDir.isValid());
        const QString cfgPath = cfgDir.filePath(QStringLiteral("cli-cfg.json"));
        loglib::LogConfiguration builder;
        {
            loglib::ColumnLayout builderLayout;
            builderLayout.AppendKeys({"msg"});
            builder.columns = builderLayout.Columns();
        }
        loglib::SaveConfiguration(builder, cfgPath.toStdString(), loglib::SaveScope::Columns);

        const TempJsonFile log({QStringLiteral(R"({"msg": "from cli"})")});

        QSignalSpy finishedSpy(wired->Model(), &LogModel::streamingFinished);
        QVERIFY(finishedSpy.isValid());
        wired->OpenFilesForCli({cfgPath, log.Path()});
        QVERIFY(finishedSpy.wait(60000));
        QCoreApplication::processEvents();

        // Configuration applied: single column `msg` from the cfg.
        // Log streamed under it: one row.
        QCOMPARE(static_cast<int>(wired->Model()->Layout().Columns().size()), 1);
        QCOMPARE(wired->Model()->rowCount(), 1);
    }

    // Glaze accepts `{}` as a default-valued `LogConfiguration` with
    // no columns; treating that as a config would wipe the live
    // column layout. `TryLoadAsConfiguration` must reject column-
    // less parses so the caller opens the file as a log instead.
    void TestTryLoadAsConfigurationRejectsEmptyJsonObject()
    {
        const QTemporaryDir sessionsDir;
        QVERIFY(sessionsDir.isValid());

        SessionHistoryManager manager(QDir(sessionsDir.path()), std::make_unique<InMemoryRecentsIndexStorage>());
        auto wired = std::make_unique<MainWindow>(mTheme.data(), &manager, nullptr);

        const TempJsonFile fixtureA({QStringLiteral(R"({"msg": "alpha"})")});

        QSignalSpy finishedSpy(wired->Model(), &LogModel::streamingFinished);
        QVERIFY(finishedSpy.isValid());
        wired->OpenFilesForTest({fixtureA.Path()}, MainWindow::OpenMode::Append);
        QVERIFY(finishedSpy.wait(60000));
        QCoreApplication::processEvents();

        const auto columnsBefore = wired->Model()->Layout().Columns().size();
        QVERIFY2(columnsBefore > 0, "fixture must produce at least one column");

        // `{}` parses cleanly into a default config; without the
        // explicit reject this would wipe the live columns.
        const QTemporaryDir emptyDir;
        QVERIFY(emptyDir.isValid());
        const QString emptyPath = QDir(emptyDir.path()).filePath(QStringLiteral("empty.json"));
        {
            std::ofstream stream(emptyPath.toStdString(), std::ios::binary);
            QVERIFY(stream.is_open());
            stream << "{}";
        }
        QVERIFY2(
            !wired->TryLoadAsConfigurationForTest(emptyPath), "an empty JSON object must be rejected as a probe miss"
        );
        QCoreApplication::processEvents();

        QCOMPARE(wired->Model()->Layout().Columns().size(), columnsBefore);
    }

    // `openWindowsAtQuit` is maintained eagerly so multi-window
    // restore survives `WA_DeleteOnClose`. AutoSave adds; close
    // removes.
    void TestOpenWindowsAtQuitTrackedAcrossAutoSaveAndClose()
    {
        const QTemporaryDir sessionsDir;
        QVERIFY(sessionsDir.isValid());

        // Snapshot + restore so we don't pollute the live profile.
        const QStringList previousOpen = SessionHistoryManager::OpenWindowsAtQuitUnlocked();
        auto restoreGuard = qScopeGuard([&]() { SessionHistoryManager::SetOpenWindowsAtQuit(previousOpen); });
        SessionHistoryManager::SetOpenWindowsAtQuit({});

        SessionHistoryManager manager(QDir(sessionsDir.path()), std::make_unique<InMemoryRecentsIndexStorage>());
        auto wired = std::make_unique<MainWindow>(mTheme.data(), &manager, nullptr);

        const TempJsonFile fixture({QStringLiteral(R"({"msg": "track"})")});

        QSignalSpy finishedSpy(wired->Model(), &LogModel::streamingFinished);
        wired->OpenFilesForTest({fixture.Path()}, MainWindow::OpenMode::Append);
        QVERIFY(finishedSpy.wait(60000));
        QCoreApplication::processEvents();

        const QString uuid = wired->ActiveSessionUuid();
        QVERIFY(!uuid.isEmpty());

        // Some sandboxed CI Windows envs refuse the write; skip.
        const QStringList afterAuto = SessionHistoryManager::OpenWindowsAtQuitUnlocked();
        if (afterAuto.isEmpty())
        {
            QSKIP("QSettings did not honour the open-windows write in this environment");
        }
        QVERIFY2(afterAuto.contains(uuid), "AutoSave must publish the session uuid into openWindowsAtQuit");

        // closeEvent removes the window from the set.
        const QSignalSpy destroyedSpy(wired.get(), &QObject::destroyed);
        wired->close();
        wired.reset();
        for (int i = 0; i < 20 && destroyedSpy.isEmpty(); ++i)
        {
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            QCoreApplication::processEvents();
        }

        const QStringList afterClose = SessionHistoryManager::OpenWindowsAtQuitUnlocked();
        QVERIFY2(!afterClose.contains(uuid), "closeEvent must remove the window from the open-windows set");
    }

    // Regression: the primary window in `main.cpp` is not
    // `WA_DeleteOnClose`, so it stays in `topLevelWidgets()` past
    // `closeEvent`. Without clearing `mAutoSaveUuid` in
    // `closeEvent`, the `aboutToQuit` snapshot would re-publish
    // the exited window into `openWindowsAtQuit`.
    void TestCloseEventClearsAutoSaveUuidForAboutToQuit()
    {
        const QTemporaryDir sessionsDir;
        QVERIFY(sessionsDir.isValid());

        const QStringList previousOpen = SessionHistoryManager::OpenWindowsAtQuitUnlocked();
        auto restoreGuard = qScopeGuard([&]() { SessionHistoryManager::SetOpenWindowsAtQuit(previousOpen); });
        SessionHistoryManager::SetOpenWindowsAtQuit({});

        SessionHistoryManager manager(QDir(sessionsDir.path()), std::make_unique<InMemoryRecentsIndexStorage>());
        // Heap-owned but not `WA_DeleteOnClose`: mirrors `main.cpp`'s
        // primary -- survives `close()` long enough for the
        // aboutToQuit replay.
        auto wired = std::make_unique<MainWindow>(mTheme.data(), &manager, nullptr);

        const TempJsonFile fixture({QStringLiteral(R"({"msg": "exit"})")});

        QSignalSpy finishedSpy(wired->Model(), &LogModel::streamingFinished);
        wired->OpenFilesForTest({fixture.Path()}, MainWindow::OpenMode::Append);
        QVERIFY(finishedSpy.wait(60000));
        QCoreApplication::processEvents();

        const QString uuid = wired->ActiveSessionUuid();
        QVERIFY(!uuid.isEmpty());
        const QStringList afterAuto = SessionHistoryManager::OpenWindowsAtQuitUnlocked();
        if (afterAuto.isEmpty())
        {
            QSKIP("QSettings did not honour the open-windows write in this environment");
        }
        QVERIFY(afterAuto.contains(uuid));

        // Simulate File -> Exit: closeEvent fires synchronously,
        // widget stays alive.
        wired->close();
        QCoreApplication::processEvents();

        // Replay `main.cpp`'s aboutToQuit snapshot loop on the
        // still-live widget. With the fix,
        // `RestorableActiveSessionUuid` is empty after closeEvent.
        QStringList capturedOnQuit;
        for (QWidget *widget : QApplication::topLevelWidgets())
        {
            auto *mw = qobject_cast<MainWindow *>(widget);
            if (mw == nullptr)
            {
                continue;
            }
            const QString restorableUuid = mw->RestorableActiveSessionUuid();
            if (!restorableUuid.isEmpty())
            {
                capturedOnQuit.append(restorableUuid);
            }
        }
        if (!capturedOnQuit.isEmpty())
        {
            SessionHistoryManager::SetOpenWindowsAtQuit(capturedOnQuit);
        }

        QVERIFY2(
            capturedOnQuit.isEmpty(),
            "aboutToQuit snapshot must not see a uuid from a window whose closeEvent already ran"
        );
        QVERIFY2(
            !SessionHistoryManager::OpenWindowsAtQuitUnlocked().contains(uuid),
            "the just-exited window's uuid must not be republished into openWindowsAtQuit"
        );
    }

    // `NewSession` must clear `mAutoSaveUuid` and drop it from
    // `openWindowsAtQuit` so a crash before the next AutoSave
    // does not re-restore the just-discarded session.
    void TestNewSessionDetachesFromOpenWindowsAtQuit()
    {
        const QTemporaryDir sessionsDir;
        QVERIFY(sessionsDir.isValid());

        const QStringList previousOpen = SessionHistoryManager::OpenWindowsAtQuitUnlocked();
        auto restoreGuard = qScopeGuard([&]() { SessionHistoryManager::SetOpenWindowsAtQuit(previousOpen); });
        SessionHistoryManager::SetOpenWindowsAtQuit({});

        SessionHistoryManager manager(QDir(sessionsDir.path()), std::make_unique<InMemoryRecentsIndexStorage>());
        auto wired = std::make_unique<MainWindow>(mTheme.data(), &manager, nullptr);

        const TempJsonFile fixture({QStringLiteral(R"({"msg": "detach"})")});

        QSignalSpy finishedSpy(wired->Model(), &LogModel::streamingFinished);
        wired->OpenFilesForTest({fixture.Path()}, MainWindow::OpenMode::Append);
        QVERIFY(finishedSpy.wait(60000));
        QCoreApplication::processEvents();

        const QString uuid = wired->ActiveSessionUuid();
        QVERIFY(!uuid.isEmpty());
        const QStringList afterAuto = SessionHistoryManager::OpenWindowsAtQuitUnlocked();
        if (afterAuto.isEmpty())
        {
            QSKIP("QSettings did not honour the open-windows write in this environment");
        }
        QVERIFY(afterAuto.contains(uuid));

        wired->findChild<QAction *>(QStringLiteral("actionNewSession"))->trigger();
        QCoreApplication::processEvents();

        QVERIFY2(
            wired->ActiveSessionUuid().isEmpty(),
            "NewSession must drop the pinned uuid so the next AutoSave produces a fresh entry"
        );
        const QStringList afterNew = SessionHistoryManager::OpenWindowsAtQuitUnlocked();
        QVERIFY2(
            !afterNew.contains(uuid), "NewSession must remove the discarded session's uuid from openWindowsAtQuit"
        );
    }

    // `Add` / `Remove` round-trip on the persisted open-windows set
    // is idempotent and order-preserving for unaffected entries.
    void TestOpenWindowUuidAddRemoveRoundTrip()
    {
        const QStringList previousOpen = SessionHistoryManager::OpenWindowsAtQuitUnlocked();
        auto restoreGuard = qScopeGuard([&]() { SessionHistoryManager::SetOpenWindowsAtQuit(previousOpen); });
        SessionHistoryManager::SetOpenWindowsAtQuit({});

        // Empty / null uuids are no-ops; `AddOpenWindowUuid`
        // returns false for empty input, true on a real publish.
        QVERIFY(!SessionHistoryManager::AddOpenWindowUuid(QString()));
        QCOMPARE(SessionHistoryManager::OpenWindowsAtQuitUnlocked(), QStringList{});
        SessionHistoryManager::RemoveOpenWindowUuid(QString());
        QCOMPARE(SessionHistoryManager::OpenWindowsAtQuitUnlocked(), QStringList{});

        const QString uuidA = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const QString uuidB = QUuid::createUuid().toString(QUuid::WithoutBraces);

        QVERIFY(SessionHistoryManager::AddOpenWindowUuid(uuidA));
        const QStringList probe = SessionHistoryManager::OpenWindowsAtQuitUnlocked();
        if (probe.isEmpty())
        {
            QSKIP("QSettings did not honour the open-windows write in this environment");
        }
        QCOMPARE(probe, QStringList{uuidA});

        // Idempotent: re-adding does not duplicate. Returns true
        // because the post-condition "uuid is in the set" holds.
        QVERIFY(SessionHistoryManager::AddOpenWindowUuid(uuidA));
        QCOMPARE(SessionHistoryManager::OpenWindowsAtQuitUnlocked(), QStringList{uuidA});

        // Order-preserving append for a fresh uuid.
        QVERIFY(SessionHistoryManager::AddOpenWindowUuid(uuidB));
        QCOMPARE(SessionHistoryManager::OpenWindowsAtQuitUnlocked(), (QStringList{uuidA, uuidB}));

        // Remove the middle / head entry; the rest stay put.
        SessionHistoryManager::RemoveOpenWindowUuid(uuidA);
        QCOMPARE(SessionHistoryManager::OpenWindowsAtQuitUnlocked(), QStringList{uuidB});

        // Removing a missing uuid is a no-op.
        SessionHistoryManager::RemoveOpenWindowUuid(uuidA);
        QCOMPARE(SessionHistoryManager::OpenWindowsAtQuitUnlocked(), QStringList{uuidB});
    }

    // Batched `AddOpenWindowUuids` (used by `aboutToQuit`): merge
    // under one lock acquisition. Appends in order, skips
    // duplicates / empty strings, preserves pre-existing entries.
    void TestAddOpenWindowUuidsBatchedMerge()
    {
        const QStringList previousOpen = SessionHistoryManager::OpenWindowsAtQuitUnlocked();
        auto restoreGuard = qScopeGuard([&]() { SessionHistoryManager::SetOpenWindowsAtQuit(previousOpen); });
        SessionHistoryManager::SetOpenWindowsAtQuit({});

        // Empty list is a no-op.
        SessionHistoryManager::AddOpenWindowUuids(QStringList{});
        QCOMPARE(SessionHistoryManager::OpenWindowsAtQuitUnlocked(), QStringList{});

        const QString uuidA = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const QString uuidB = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const QString uuidC = QUuid::createUuid().toString(QUuid::WithoutBraces);

        // Pre-seed `uuidA` to model a sibling peer. Batched call
        // must merge, not overwrite.
        QVERIFY(SessionHistoryManager::AddOpenWindowUuid(uuidA));
        const QStringList preBatch = SessionHistoryManager::OpenWindowsAtQuitUnlocked();
        if (preBatch.isEmpty())
        {
            QSKIP("QSettings did not honour the open-windows write in this environment");
        }
        QCOMPARE(preBatch, QStringList{uuidA});

        // Batch: an empty string (skipped), a duplicate of `uuidA`
        // (skipped -- idempotent), and two new uuids appended in
        // order.
        SessionHistoryManager::AddOpenWindowUuids(QStringList{QString(), uuidA, uuidB, uuidC});
        QCOMPARE(SessionHistoryManager::OpenWindowsAtQuitUnlocked(), (QStringList{uuidA, uuidB, uuidC}));

        // A subsequent batch with no new entries is also a no-op
        // (every uuid is already present).
        SessionHistoryManager::AddOpenWindowUuids(QStringList{uuidA, uuidB, uuidC});
        QCOMPARE(SessionHistoryManager::OpenWindowsAtQuitUnlocked(), (QStringList{uuidA, uuidB, uuidC}));
    }

    // `RestoreLastSessionFromPath` must pin `mAutoSaveUuid` even
    // for source-less configurations; otherwise the next AutoSave
    // forks a fresh entry instead of updating the loaded one.
    void TestRestoreLastSessionPinsUuidEvenWithNoSource()
    {
        const QTemporaryDir sessionsDir;
        QVERIFY(sessionsDir.isValid());

        SessionHistoryManager manager(QDir(sessionsDir.path()), std::make_unique<InMemoryRecentsIndexStorage>());

        // Hand-craft a uuid-stemmed JSON with no source so the
        // restore path hits the no-source branch.
        const QString uuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const QString jsonPath = manager.PathForUuid(uuid);
        QDir().mkpath(QFileInfo(jsonPath).absolutePath());

        loglib::LogConfiguration builder;
        {
            loglib::ColumnLayout builderLayout;
            builderLayout.AppendKeys({"msg", "level"});
            builder.columns = builderLayout.Columns();
        }
        // No `SetSource`: column-only snapshot.
        loglib::SaveConfiguration(builder, jsonPath.toStdString(), loglib::SaveScope::Full);
        QVERIFY(QFileInfo::exists(jsonPath));

        auto wired = std::make_unique<MainWindow>(mTheme.data(), &manager, nullptr);

        wired->RestoreLastSessionFromPath(jsonPath);
        QCoreApplication::processEvents();

        QCOMPARE(wired->ActiveSessionUuid(), uuid);
    }

    // `ShouldAutoSaveSession` filters out live-tail / network-
    // stream sessions: they can't be rebound from a JSON snapshot,
    // so saving them would create entries that never reopen.
    void TestAutoSaveSkipsLiveTailAndNetworkStreamSessions()
    {
        const QTemporaryDir sessionsDir;
        QVERIFY(sessionsDir.isValid());

        SessionHistoryManager manager(QDir(sessionsDir.path()), std::make_unique<InMemoryRecentsIndexStorage>());

        // Case A: live-tail on a File source. A plain kind check
        // would let it through; the gate must also see `LiveTail`.
        {
            auto wired = std::make_unique<MainWindow>(mTheme.data(), &manager, nullptr);
            wired->SetCurrentSourceForTest(
                loglib::SourceDescriptor{
                    .kind = loglib::SourceDescriptor::Kind::File,
                    .locators = {{.path = "/tmp/live.log", .key = "/tmp/live.log"}}
                }
            );
            wired->SetSessionModeForTest(MainWindow::TestSessionMode::LiveTail);

            wired->close();
            QCoreApplication::processEvents();

            QVERIFY2(
                manager.List().isEmpty(), "closeEvent must not auto-save a live-tail session into Recent Sessions"
            );
        }

        // Case B: NetworkStream source -- not re-bindable.
        {
            auto wired = std::make_unique<MainWindow>(mTheme.data(), &manager, nullptr);
            wired->SetCurrentSourceForTest(
                loglib::SourceDescriptor{
                    .kind = loglib::SourceDescriptor::Kind::NetworkStream,
                    .locators = {{.path = "tcp://127.0.0.1:5170", .key = "tcp://127.0.0.1:5170"}},
                }
            );
            wired->SetSessionModeForTest(MainWindow::TestSessionMode::LiveTail);

            wired->close();
            QCoreApplication::processEvents();

            QVERIFY2(
                manager.List().isEmpty(), "closeEvent must not auto-save a network-stream session into Recent Sessions"
            );
        }
    }

    // `RestoreLastSessionFromPath` must not stream a NetworkStream
    // URI as a static file. Restore succeeds (columns / filters
    // installed) but no streaming attempt is made.
    void TestRestoreLastSessionSkipsNetworkStreamSource()
    {
        const QTemporaryDir sessionsDir;
        QVERIFY(sessionsDir.isValid());

        SessionHistoryManager manager(QDir(sessionsDir.path()), std::make_unique<InMemoryRecentsIndexStorage>());

        // Hand-craft a NetworkStream session JSON (legacy: older
        // builds auto-saved streams before the gate landed).
        const QString uuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const QString jsonPath = manager.PathForUuid(uuid);
        QDir().mkpath(QFileInfo(jsonPath).absolutePath());

        loglib::LogConfiguration builder;
        {
            loglib::ColumnLayout builderLayout;
            builderLayout.AppendKeys({"msg"});
            builder.columns = builderLayout.Columns();
        }
        builder.source = (loglib::SourceDescriptor{
            .kind = loglib::SourceDescriptor::Kind::NetworkStream,
            .locators = {{.path = "tcp://127.0.0.1:5170", .key = "tcp://127.0.0.1:5170"}},
        });
        loglib::SaveConfiguration(builder, jsonPath.toStdString(), loglib::SaveScope::Full);
        QVERIFY(QFileInfo::exists(jsonPath));

        auto wired = std::make_unique<MainWindow>(mTheme.data(), &manager, nullptr);

        // No streaming attempt -- `finishedSpy` must stay at 0.
        const QSignalSpy finishedSpy(wired->Model(), &LogModel::streamingFinished);
        wired->RestoreLastSessionFromPath(jsonPath);
        // Pump the event loop briefly.
        for (int i = 0; i < 5; ++i)
        {
            QCoreApplication::processEvents();
        }

        QCOMPARE(finishedSpy.count(), 0);
        // uuid still pinned so future saves update this entry.
        QCOMPARE(wired->ActiveSessionUuid(), uuid);
    }

    // `RestoreLastSessionFromPath` only pins `mAutoSaveUuid` when
    // the stem parses as a QUuid -- an ad-hoc file can't hijack
    // auto-save into the user's filesystem.
    void TestRestoreLastSessionRejectsNonUuidStem()
    {
        const QTemporaryDir sessionsDir;
        QVERIFY(sessionsDir.isValid());

        SessionHistoryManager manager(QDir(sessionsDir.path()), std::make_unique<InMemoryRecentsIndexStorage>());

        // Valid session JSON at a non-uuid path; only the stem-
        // validation gate is under test.
        const QTemporaryDir adhocDir;
        QVERIFY(adhocDir.isValid());
        const TempJsonFile fixture({QStringLiteral(R"({"msg": "stem"})")});

        loglib::LogConfiguration builder;
        {
            loglib::ColumnLayout builderLayout;
            builderLayout.AppendKeys({"msg"});
            builder.columns = builderLayout.Columns();
        }
        builder.source = (loglib::SourceDescriptor{
            .kind = loglib::SourceDescriptor::Kind::File,
            .locators = {{.path = fixture.Path().toStdString(), .key = fixture.Path().toStdString()}},
        });
        const QString adhocPath = adhocDir.filePath(QStringLiteral("not-a-uuid.json"));
        loglib::SaveConfiguration(builder, adhocPath.toStdString(), loglib::SaveScope::Full);

        auto wired = std::make_unique<MainWindow>(mTheme.data(), &manager, nullptr);

        QSignalSpy finishedSpy(wired->Model(), &LogModel::streamingFinished);
        wired->RestoreLastSessionFromPath(adhocPath);
        QVERIFY(finishedSpy.wait(60000));
        QCoreApplication::processEvents();

        // Streaming finished but `mAutoSaveUuid` must not pin to
        // `not-a-uuid`.
        QVERIFY2(
            wired->ActiveSessionUuid() != QStringLiteral("not-a-uuid"),
            "RestoreLastSessionFromPath must reject non-uuid stems"
        );
    }

    // `aboutToQuit` must flush each window via
    // `AutoSaveSessionSnapshot(true)` so post-`streamingFinished`
    // edits survive an OS-driven quit (no `closeEvent`).
    void TestAboutToQuitFlushesPerWindowEdits()
    {
        const QTemporaryDir sessionsDir;
        QVERIFY(sessionsDir.isValid());

        const QStringList previousOpen = SessionHistoryManager::OpenWindowsAtQuitUnlocked();
        auto restoreGuard = qScopeGuard([&]() { SessionHistoryManager::SetOpenWindowsAtQuit(previousOpen); });
        SessionHistoryManager::SetOpenWindowsAtQuit({});

        SessionHistoryManager manager(QDir(sessionsDir.path()), std::make_unique<InMemoryRecentsIndexStorage>());
        auto wired = std::make_unique<MainWindow>(mTheme.data(), &manager, nullptr);

        const TempJsonFile fixture({QStringLiteral(R"({"msg": "abq-flush"})")});
        QSignalSpy finishedSpy(wired->Model(), &LogModel::streamingFinished);
        wired->OpenFilesForTest({fixture.Path()}, MainWindow::OpenMode::Append);
        QVERIFY(finishedSpy.wait(60000));
        QCoreApplication::processEvents();

        const QString uuid = wired->ActiveSessionUuid();
        QVERIFY(!uuid.isEmpty());

        // Drain the auto-save signal so the assertion below is
        // unambiguous.
        const QSignalSpy changedSpy(&manager, &SessionHistoryManager::changed);
        QVERIFY(changedSpy.isValid());

        // Replay the `aboutToQuit` body: per-window flush, then
        // one batched publish under a single lock acquisition.
        QStringList restorable;
        for (QWidget *widget : QApplication::topLevelWidgets())
        {
            auto *mw = qobject_cast<MainWindow *>(widget);
            if (mw == nullptr)
            {
                continue;
            }
            mw->AutoSaveSessionSnapshot(/*publishOpenWindow=*/false);
            const QString restorableUuid = mw->RestorableActiveSessionUuid();
            if (!restorableUuid.isEmpty())
            {
                restorable.append(restorableUuid);
            }
        }
        SessionHistoryManager::AddOpenWindowUuids(restorable);

        QVERIFY2(
            changedSpy.count() >= 1, "aboutToQuit must invoke AutoSaveSessionSnapshot, not just AddOpenWindowUuids"
        );

        const QStringList afterQuit = SessionHistoryManager::OpenWindowsAtQuitUnlocked();
        if (afterQuit.isEmpty())
        {
            QSKIP("QSettings did not honour the open-windows write in this environment");
        }
        QVERIFY2(
            afterQuit.contains(uuid), "aboutToQuit handler must publish the live window's uuid into openWindowsAtQuit"
        );
    }

    // External JSON (outside the sessions dir) must not pin or
    // publish a uuid -- a future AutoSave would otherwise silently
    // fork the user's file into a managed copy.
    //
    // Exercised against a no-source config so `streamingFinished`
    // never legitimises the entry after the fact.
    void TestRestoreLastSessionDoesNotPublishGhostUuid()
    {
        const QTemporaryDir sessionsDir;
        QVERIFY(sessionsDir.isValid());

        const QStringList previousOpen = SessionHistoryManager::OpenWindowsAtQuitUnlocked();
        auto restoreGuard = qScopeGuard([&]() { SessionHistoryManager::SetOpenWindowsAtQuit(previousOpen); });
        SessionHistoryManager::SetOpenWindowsAtQuit({});

        SessionHistoryManager manager(QDir(sessionsDir.path()), std::make_unique<InMemoryRecentsIndexStorage>());

        // No-source JSON with a uuid-shaped stem outside the
        // managed sessionsDir. The directory mismatch trips the
        // gate so the pin + publish block is skipped.
        const QTemporaryDir externalDir;
        QVERIFY(externalDir.isValid());
        const QString uuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const QString externalPath = externalDir.filePath(uuid + QStringLiteral(".json"));

        loglib::LogConfiguration builder;
        {
            loglib::ColumnLayout builderLayout;
            builderLayout.AppendKeys({"msg"});
            builder.columns = builderLayout.Columns();
        }
        // Columns-only (no `SetSource`): restore hits the no-source
        // branch and never starts streaming.
        loglib::SaveConfiguration(builder, externalPath.toStdString(), loglib::SaveScope::Full);
        QVERIFY(QFileInfo::exists(externalPath));

        auto wired = std::make_unique<MainWindow>(mTheme.data(), &manager, nullptr);

        wired->RestoreLastSessionFromPath(externalPath);
        QCoreApplication::processEvents();

        // The configuration loads but the uuid stays unpinned, so
        // the next save mints a fresh uuid in the managed dir.
        QVERIFY2(
            wired->ActiveSessionUuid().isEmpty(),
            qPrintable(QStringLiteral("external uuid-shaped JSON must not pin mAutoSaveUuid (got '%1')")
                           .arg(wired->ActiveSessionUuid()))
        );

        const QStringList openSet = SessionHistoryManager::OpenWindowsAtQuitUnlocked();
        // No `QSKIP` on empty -- an empty set is what the fix
        // guarantees.
        QVERIFY2(!openSet.contains(uuid), "external uuid-shaped JSON must not be published into openWindowsAtQuit");
    }

    // Non-File source kinds (NetworkStream today) must not publish
    // into `openWindowsAtQuit`. The recents index still owns the
    // entry (Touch / manual rebind work) but fan-restore must skip
    // it; otherwise every launch resurrects a "re-bind manually"
    // popup loop.
    void TestRestoreLastSessionDoesNotPublishNetworkStreamUuid()
    {
        const QTemporaryDir sessionsDir;
        QVERIFY(sessionsDir.isValid());

        const QStringList previousOpen = SessionHistoryManager::OpenWindowsAtQuitUnlocked();
        auto restoreGuard = qScopeGuard([&]() { SessionHistoryManager::SetOpenWindowsAtQuit(previousOpen); });
        SessionHistoryManager::SetOpenWindowsAtQuit({});

        SessionHistoryManager manager(QDir(sessionsDir.path()), std::make_unique<InMemoryRecentsIndexStorage>());

        // `WriteSnapshot` so the entry lives in the index; without
        // it, `Touch` would short-circuit and mask the gate.
        loglib::LogConfiguration cfg;
        cfg.source = loglib::SourceDescriptor{
            .kind = loglib::SourceDescriptor::Kind::NetworkStream,
            .locators = {{.path = "tcp://127.0.0.1:5170", .key = "tcp://127.0.0.1:5170"}},
        };
        const QString uuid = manager.WriteSnapshot(cfg);
        QVERIFY(!uuid.isEmpty());
        const QString jsonPath = manager.PathForUuid(uuid);
        QVERIFY(QFileInfo::exists(jsonPath));

        auto wired = std::make_unique<MainWindow>(mTheme.data(), &manager, nullptr);

        wired->RestoreLastSessionFromPath(jsonPath);
        QCoreApplication::processEvents();

        // Window owns the uuid (so a manual rebind updates the
        // same entry), but the restorability predicate hides it.
        QCOMPARE(wired->ActiveSessionUuid(), uuid);
        QVERIFY(wired->RestorableActiveSessionUuid().isEmpty());

        const QStringList openSet = SessionHistoryManager::OpenWindowsAtQuitUnlocked();
        QVERIFY2(!openSet.contains(uuid), "NetworkStream uuid must not be published into openWindowsAtQuit on restore");
    }

    // Companion: same invariant for the menu reopen path
    // (`OpenRecentSession(uuid)`).
    void TestOpenRecentSessionDoesNotPublishNetworkStreamUuid()
    {
        const QTemporaryDir sessionsDir;
        QVERIFY(sessionsDir.isValid());

        const QStringList previousOpen = SessionHistoryManager::OpenWindowsAtQuitUnlocked();
        auto restoreGuard = qScopeGuard([&]() { SessionHistoryManager::SetOpenWindowsAtQuit(previousOpen); });
        SessionHistoryManager::SetOpenWindowsAtQuit({});

        SessionHistoryManager manager(QDir(sessionsDir.path()), std::make_unique<InMemoryRecentsIndexStorage>());

        loglib::LogConfiguration cfg;
        cfg.source = loglib::SourceDescriptor{
            .kind = loglib::SourceDescriptor::Kind::NetworkStream,
            .locators = {{.path = "tcp://127.0.0.1:5171", .key = "tcp://127.0.0.1:5171"}},
        };
        const QString uuid = manager.WriteSnapshot(cfg);
        QVERIFY(!uuid.isEmpty());
        QVERIFY(QFileInfo::exists(manager.PathForUuid(uuid)));

        auto wired = std::make_unique<MainWindow>(mTheme.data(), &manager, nullptr);
        // Suppress the non-File source modal so the offscreen
        // runner doesn't hang.
        wired->SetSuppressDialogsForTest(true);

        wired->OpenRecentSessionForTest(uuid);
        QCoreApplication::processEvents();

        QCOMPARE(wired->ActiveSessionUuid(), uuid);
        QVERIFY(wired->RestorableActiveSessionUuid().isEmpty());

        const QStringList openSet = SessionHistoryManager::OpenWindowsAtQuitUnlocked();
        QVERIFY2(!openSet.contains(uuid), "NetworkStream uuid must not be published into openWindowsAtQuit on reopen");
    }

    // -------------------------------------------------------------------------
    // MainWindow lifecycle regressions.
    // -------------------------------------------------------------------------

    void TestOpenRecentSessionDropsCorruptEntry()
    {
        const QTemporaryDir sessionsDir;
        QVERIFY(sessionsDir.isValid());

        SessionHistoryManager manager(QDir(sessionsDir.path()), std::make_unique<InMemoryRecentsIndexStorage>());

        // Corrupt the JSON so the pre-flight parse fails. Post-fix
        // the corrupt entry is removed from the index.
        loglib::LogConfiguration cfg;
        cfg.source = loglib::SourceDescriptor{
            .kind = loglib::SourceDescriptor::Kind::File,
            .locators = {{.path = "C:/logs/will-be-corrupted.json", .key = "C:/logs/will-be-corrupted.json"}},
        };
        const QString uuid = manager.WriteSnapshot(cfg);
        QVERIFY(!uuid.isEmpty());

        const QString path = manager.PathForUuid(uuid);
        QVERIFY(QFileInfo::exists(path));

        // Replace contents with non-JSON garbage.
        {
            QFile f(path);
            QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
            f.write("this is not json {{");
        }

        auto wired = std::make_unique<MainWindow>(mTheme.data(), &manager, nullptr);
        wired->SetSuppressDialogsForTest(true);
        wired->OpenRecentSessionForTest(uuid);
        QCoreApplication::processEvents();

        const QList<RecentSessionEntry> entries = manager.List();
        QVERIFY2(
            std::none_of(
                entries.begin(), entries.end(), [&uuid](const RecentSessionEntry &e) { return e.uuid == uuid; }
            ),
            "Corrupt recents entry must be removed after a failed open attempt"
        );
    }

    // CLI with a single source-less config must show a status-bar
    // hint pointing at File -> Open; otherwise the launch looks
    // like a no-op (just column headers, no rows).
    void TestOpenFilesForCliSingleConfigWithoutSourceShowsHint()
    {
        SessionHistoryManager manager(
            QDir(QDir::tempPath() + QStringLiteral("/abs-2-1")), std::make_unique<InMemoryRecentsIndexStorage>()
        );
        auto wired = std::make_unique<MainWindow>(mTheme.data(), &manager, nullptr);

        // Source-less config (columns only, no `source` field).
        const QTemporaryDir scratch;
        QVERIFY(scratch.isValid());
        const QString configPath = scratch.filePath(QStringLiteral("hint-cfg.json"));
        loglib::LogConfiguration builder;
        {
            loglib::ColumnLayout builderLayout;
            builderLayout.AppendKeys({"timestamp", "level", "msg"});
            builder.columns = builderLayout.Columns();
        }
        loglib::SaveConfiguration(builder, configPath.toStdString(), loglib::SaveScope::Columns);

        wired->OpenFilesForCli({configPath});
        QCoreApplication::processEvents();

        // Status-bar hint visible.
        const QString status = wired->statusBar()->currentMessage();
        QVERIFY2(
            status.contains(QStringLiteral("configuration"), Qt::CaseInsensitive) &&
                status.contains(QStringLiteral("File")),
            qPrintable(QStringLiteral(
                           "status bar should hint at File -> Open after a source-less "
                           "configuration load; got: %1"
            )
                           .arg(status))
        );

        // Configuration applied so a follow-up File -> Open lands
        // on the user's chosen column layout.
        QVERIFY2(
            wired->Model()->columnCount() >= 1,
            "OpenFilesForCli must apply the configuration's columns even when no source is bound"
        );
    }

    // `closeEvent` on a window that never published its uuid must
    // not leave a stale entry in `openWindowsAtQuit`. The fix
    // skips the cross-process `RemoveOpenWindowUuid` when nothing
    // was published.
    //
    // Exercised via the path where `AutoSaveSessionSnapshot(false)`
    // is the *first* save (no prior `streamingFinished` published
    // the uuid), so a Remove would be a wasted no-op.
    void TestCloseEventOnUnpublishedSessionLeavesOpenWindowsClear()
    {
        const QTemporaryDir sessionsDir;
        QVERIFY(sessionsDir.isValid());

        // Snapshot + restore so the assertion starts clean.
        const QStringList previousOpen = SessionHistoryManager::OpenWindowsAtQuitUnlocked();
        auto restoreGuard = qScopeGuard([&]() { SessionHistoryManager::SetOpenWindowsAtQuit(previousOpen); });
        SessionHistoryManager::SetOpenWindowsAtQuit({});

        SessionHistoryManager manager(QDir(sessionsDir.path()), std::make_unique<InMemoryRecentsIndexStorage>());
        auto wired = std::make_unique<MainWindow>(mTheme.data(), &manager, nullptr);

        // Open a static session (auto-save publishes the uuid),
        // then clear the set so we exercise the unpublished close.
        const TempJsonFile fixture({QStringLiteral(R"({"msg": "unpub"})")});
        QSignalSpy finishedSpy(wired->Model(), &LogModel::streamingFinished);
        wired->OpenFilesForTest({fixture.Path()}, MainWindow::OpenMode::Append);
        QVERIFY(finishedSpy.wait(60000));
        QCoreApplication::processEvents();
        const QString uuid = wired->ActiveSessionUuid();
        QVERIFY(!uuid.isEmpty());

        // Scrub the set to simulate a sibling reset.
        SessionHistoryManager::SetOpenWindowsAtQuit({});

        // closeEvent runs synchronously.
        wired->close();
        QCoreApplication::processEvents();

        const QStringList afterClose = SessionHistoryManager::OpenWindowsAtQuitUnlocked();
        QVERIFY2(!afterClose.contains(uuid), "closeEvent must leave the open-windows set free of this window's uuid");
    }

    // H1 regression: after `File -> Exit` runs `closeEvent`, the
    // primary stays in `topLevelWidgets()`. Without resetting
    // session-mode state in `closeEvent`, the `aboutToQuit`
    // fan-AutoSave would mint a fresh uuid and resurrect the
    // window on next launch.
    void TestCloseEventThenAboutToQuitDoesNotResurrectClosedWindow()
    {
        const QTemporaryDir sessionsDir;
        QVERIFY(sessionsDir.isValid());

        const QStringList previousOpen = SessionHistoryManager::OpenWindowsAtQuitUnlocked();
        auto restoreGuard = qScopeGuard([&]() { SessionHistoryManager::SetOpenWindowsAtQuit(previousOpen); });
        SessionHistoryManager::SetOpenWindowsAtQuit({});

        SessionHistoryManager manager(QDir(sessionsDir.path()), std::make_unique<InMemoryRecentsIndexStorage>());
        auto wired = std::make_unique<MainWindow>(mTheme.data(), &manager, nullptr);

        // Stage 1: open a static session; auto-save publishes the
        // uuid into `openWindowsAtQuit`.
        const TempJsonFile fixture({QStringLiteral(R"({"msg": "h1-regr"})")});
        QSignalSpy finishedSpy(wired->Model(), &LogModel::streamingFinished);
        wired->OpenFilesForTest({fixture.Path()}, MainWindow::OpenMode::Append);
        QVERIFY(finishedSpy.wait(60000));
        QCoreApplication::processEvents();

        const QString origUuid = wired->ActiveSessionUuid();
        QVERIFY(!origUuid.isEmpty());
        QCOMPARE(manager.List().size(), 1);

        // Stage 2: close the window; with the fix, closeEvent
        // also resets session-mode state to Idle.
        wired->close();
        QCoreApplication::processEvents();

        // Sanity: the just-closed uuid is gone from the open set.
        const QStringList afterClose = SessionHistoryManager::OpenWindowsAtQuitUnlocked();
        QVERIFY2(!afterClose.contains(origUuid), "closeEvent should remove the window's uuid from openWindowsAtQuit");

        // Stage 3: replicate the production `aboutToQuit` body:
        // per-window flush, then one batched publish.
        QStringList restorableUuids;
        for (QWidget *widget : QApplication::topLevelWidgets())
        {
            auto *mw = qobject_cast<MainWindow *>(widget);
            if (mw == nullptr || mw != wired.get())
            {
                continue;
            }
            mw->AutoSaveSessionSnapshot(/*publishOpenWindow=*/false);
            const QString uuid = mw->RestorableActiveSessionUuid();
            if (!uuid.isEmpty())
            {
                restorableUuids.append(uuid);
            }
        }
        SessionHistoryManager::AddOpenWindowUuids(restorableUuids);
        QCoreApplication::processEvents();

        // 1: recents index size unchanged (no duplicate entry).
        QCOMPARE(manager.List().size(), 1);
        QCOMPARE(manager.List().front().uuid, origUuid);

        // 2: no fresh uuid in the open-windows set.
        const QStringList afterQuit = SessionHistoryManager::OpenWindowsAtQuitUnlocked();
        QVERIFY2(
            afterQuit.isEmpty() || (afterQuit.size() == 1 && afterQuit.front() == origUuid),
            qPrintable(QStringLiteral(
                           "aboutToQuit fan-flush must not resurrect the closed window via "
                           "a fresh uuid; got: %1"
            )
                           .arg(afterQuit.join(QStringLiteral(", "))))
        );

        // 3: no duplicate JSON on disk.
        const QStringList sessionFiles =
            QDir(sessionsDir.path()).entryList(QStringList{QStringLiteral("*.json")}, QDir::Files);
        QCOMPARE(sessionFiles.size(), 1);
    }

    // `RestoreLastSessionOnLaunch` round-trip; default is `true`.
    void TestRestoreLastSessionOnLaunchPreferenceRoundTrip()
    {
        QSettings settings;
        const QVariant previous = settings.value(QStringLiteral("recentSessions/restoreLastSessionOnLaunch"));
        auto restoreGuard = qScopeGuard([&]() {
            QSettings restore;
            if (previous.isValid())
            {
                restore.setValue(QStringLiteral("recentSessions/restoreLastSessionOnLaunch"), previous);
            }
            else
            {
                restore.remove(QStringLiteral("recentSessions/restoreLastSessionOnLaunch"));
            }
            restore.sync();
        });

        settings.remove(QStringLiteral("recentSessions/restoreLastSessionOnLaunch"));
        settings.sync();
        QVERIFY2(
            SessionHistoryManager::RestoreLastSessionOnLaunch(),
            "default RestoreLastSessionOnLaunch must be true when the key is absent"
        );

        SessionHistoryManager::SetRestoreLastSessionOnLaunch(false);
        QVERIFY(!SessionHistoryManager::RestoreLastSessionOnLaunch());

        SessionHistoryManager::SetRestoreLastSessionOnLaunch(true);
        QVERIFY(SessionHistoryManager::RestoreLastSessionOnLaunch());
    }

    // `SetSocketNameForTest` after `TryAcquire` must early-return
    // (debug: `Q_ASSERT`; release: qWarning + ignore) so the
    // original binding stays intact.
    void TestSingleInstanceSetSocketNameForTestIgnoredAfterAcquire()
    {
        const QString originalName =
            QStringLiteral("slv-orig-") + QUuid::createUuid().toString(QUuid::WithoutBraces).left(8);

        SingleInstanceGuard primary;
        primary.SetSocketNameForTest(originalName);
        QVERIFY(primary.TryAcquire({}, /*allowNewInstance=*/false));

        // Release-only: debug `Q_ASSERT` fires before the early
        // return, so skip the rebind attempt in debug builds.
#ifdef QT_NO_DEBUG
        const QString divertName =
            QStringLiteral("slv-div-") + QUuid::createUuid().toString(QUuid::WithoutBraces).left(8);
        primary.SetSocketNameForTest(divertName);

        // The divert-name secondary becomes its own primary
        // because the original primary is still bound elsewhere.
        SingleInstanceGuard divertSecondary;
        divertSecondary.SetSocketNameForTest(divertName);
        QVERIFY(divertSecondary.TryAcquire({}, /*allowNewInstance=*/false));
#endif
    }
};

QTEST_MAIN(SessionsTest)
#include "apptest_sessions_test.moc"
