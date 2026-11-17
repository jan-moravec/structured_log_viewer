#include "apptest_support.hpp"
#include "advanced_filter_editor.hpp"
#include "anchor_manager.hpp"
#include "anchors_dock.hpp"
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
class RecentsLocksTest : public apptest_support::AppTestBase
{
    Q_OBJECT

private slots:

    // With the cross-process lockfile held by a sibling, the
    // manager fails closed: `WriteSnapshot` returns an empty uuid,
    // the lock attempt is bounded so the GUI does not freeze, and
    // a retry succeeds once the lock is released.
    void TestWriteSnapshotSurvivesHeldLockFile()
    {
        const QTemporaryDir sessionsDir;
        QVERIFY(sessionsDir.isValid());

        SessionHistoryManager manager(QDir(sessionsDir.path()), std::make_unique<InMemoryRecentsIndexStorage>());

        // Force the directory so the lock path exists before
        // WriteSnapshot would create it.
        QVERIFY(QDir().mkpath(sessionsDir.path()));

        // Take the lock from a "foreign" holder. Short stale-time
        // so the manager's tryLock never treats it as dead.
        QLockFile foreign(QDir(sessionsDir.path()).filePath(QStringLiteral("recents.lock")));
        foreign.setStaleLockTime(0);
        QVERIFY(foreign.tryLock(100));

        loglib::LogConfiguration cfg;
        loglib::Source src;
        src.locators = {"C:/logs/locked.json"};
        cfg.source = src;

        // Fail-closed: empty uuid, no write, bounded duration.
        const auto start = std::chrono::steady_clock::now();
        const QString uuid = manager.WriteSnapshot(cfg);
        const auto elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();

        QVERIFY2(uuid.isEmpty(), "WriteSnapshot must fail closed when the cross-process lock cannot be acquired");
        QVERIFY2(elapsed < 3000, qPrintable(QStringLiteral("WriteSnapshot took too long: %1ms").arg(elapsed)));
        QCOMPARE(manager.List().size(), 0);

        // Releasing the foreign lock unblocks the next call.
        foreign.unlock();
        const QString retryUuid = manager.WriteSnapshot(cfg);
        QVERIFY2(!retryUuid.isEmpty(), "WriteSnapshot must succeed once contention clears");
        QCOMPARE(manager.List().size(), 1);
    }

    void TestRecentsTouchReturnsFalseOnLockContention()
    {
        const QTemporaryDir sessionsDir;
        QVERIFY(sessionsDir.isValid());
        QVERIFY(QDir(sessionsDir.path()).mkpath(QStringLiteral(".")));

        SessionHistoryManager manager(QDir(sessionsDir.path()), std::make_unique<InMemoryRecentsIndexStorage>());

        // Seed an entry so the `Touch` pre-check passes and the
        // call reaches the cross-process lock.
        loglib::LogConfiguration cfg;
        cfg.source = loglib::Source{.kind = loglib::Source::Kind::File, .locators = {"C:/logs/contended.json"}};
        const QString uuid = manager.WriteSnapshot(cfg);
        QVERIFY(!uuid.isEmpty());

        // External lock holder: `Touch` returns false so the
        // caller's publish stays gated.
        QLockFile externalLock(QDir(sessionsDir.path()).filePath(QStringLiteral("recents.lock")));
        QVERIFY(externalLock.tryLock(0));
        QVERIFY(!manager.Touch(uuid));
        externalLock.unlock();

        // Once released, `Touch` succeeds again.
        QVERIFY(manager.Touch(uuid));
    }

    // `TakeOpenWindowsAtQuit` returns empty on contention so two
    // simultaneously-launching siblings can't both fan-restore the
    // same uuids. A sibling republishes its uuid on construction
    // so a
    // missed take here is recoverable.
    void TestRecentsTakeOpenWindowsAtQuitReturnsEmptyOnContention()
    {
        const QStringList previousOpen = SessionHistoryManager::OpenWindowsAtQuitUnlocked();
        auto restoreGuard = qScopeGuard([&]() { SessionHistoryManager::SetOpenWindowsAtQuit(previousOpen); });
        SessionHistoryManager::SetOpenWindowsAtQuit({});

        // Seed the persisted set with a uuid so the read half would
        // otherwise return non-empty.
        const QString uuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
        SessionHistoryManager::SetOpenWindowsAtQuit({uuid});

        // Hold the cross-process lock so the take fails.
        QLockFile holderLock(SessionHistoryManager::DefaultSessionsDir().filePath(QStringLiteral("recents.lock")));
        if (!holderLock.tryLock(0))
        {
            QSKIP("Could not seize the cross-process lock; skipping (env-dependent)");
        }

        const QStringList taken = SessionHistoryManager::TakeOpenWindowsAtQuit();
        QCOMPARE(taken, QStringList{});
        // A refused take leaves the persisted set alone.
        QCOMPARE(SessionHistoryManager::OpenWindowsAtQuitUnlocked(), QStringList{uuid});

        holderLock.unlock();

        // After releasing the lock the take succeeds and surfaces
        // the persisted uuid.
        const QStringList retake = SessionHistoryManager::TakeOpenWindowsAtQuit();
        QCOMPARE(retake, QStringList{uuid});
    }
};

QTEST_MAIN(RecentsLocksTest)
#include "apptest_recents_locks_test.moc"
