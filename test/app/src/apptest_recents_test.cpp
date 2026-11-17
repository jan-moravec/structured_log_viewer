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
class RecentsTest : public apptest_support::AppTestBase
{
    Q_OBJECT

private slots:

    // ---------------------------------------------------------------
    // SessionHistoryManager
    // ---------------------------------------------------------------

    // Round-trip a snapshot: write a configuration, read the list, and
    // re-load the on-disk JSON. Asserts the per-uuid file lands under
    // the supplied sessions dir and that the metadata in the index is
    // populated from the configuration's source descriptor.
    void TestSessionHistoryWriteSnapshotRoundTrips()
    {
        const QTemporaryDir sessionsDir;
        QVERIFY(sessionsDir.isValid());

        SessionHistoryManager manager(QDir(sessionsDir.path()), std::make_unique<InMemoryRecentsIndexStorage>());

        loglib::LogConfiguration cfg;
        cfg.columns.push_back(
            loglib::Column{.header = "category", .keys = {"category"}, .type = loglib::ColumnType::Enumeration}
        );
        cfg.columns.push_back(loglib::Column{.header = "msg", .keys = {"msg"}, .type = loglib::ColumnType::String});
        cfg.source = loglib::Source{
            .kind = loglib::Source::Kind::File,
            .locators = {"C:/logs/first.json", "C:/logs/second.json"},
        };

        const QSignalSpy changedSpy(&manager, &SessionHistoryManager::changed);
        const QString uuid = manager.WriteSnapshot(cfg);

        QVERIFY(!uuid.isEmpty());
        QCOMPARE(changedSpy.count(), 1);

        const QList<RecentSessionEntry> list = manager.List();
        QCOMPARE(list.size(), 1);
        QCOMPARE(list.front().uuid, uuid);
        QCOMPARE(list.front().fileCount, 2);
        QCOMPARE(list.front().label, QStringLiteral("first.json + 1 more"));
        QCOMPARE(list.front().primaryLocator, QStringLiteral("C:/logs/first.json"));

        const QString jsonPath = manager.PathForUuid(uuid);
        QVERIFY(QFileInfo::exists(jsonPath));

        // Round-trip through the library loader.
        const loglib::LogConfiguration probe = loglib::LoadConfiguration(jsonPath.toStdString());
        QVERIFY(probe.source.has_value());
        QCOMPARE(probe.source->locators.size(), static_cast<std::size_t>(2));
        QCOMPARE(probe.columns.size(), static_cast<std::size_t>(2));
    }

    // Recents label for a `NetworkStream` source round-trips the
    // URI verbatim (no `QFileInfo::fileName()` collapse).
    void TestSessionHistoryLabelKeepsNetworkStreamLocatorIntact()
    {
        const QTemporaryDir sessionsDir;
        QVERIFY(sessionsDir.isValid());

        SessionHistoryManager manager(QDir(sessionsDir.path()), std::make_unique<InMemoryRecentsIndexStorage>());

        loglib::LogConfiguration cfg;
        cfg.source = loglib::Source{.kind = loglib::Source::Kind::NetworkStream, .locators = {"TCP 127.0.0.1:5170"}};
        const QString uuid = manager.WriteSnapshot(cfg);
        QVERIFY(!uuid.isEmpty());

        const QList<RecentSessionEntry> list = manager.List();
        QCOMPARE(list.size(), 1);
        QCOMPARE(list.front().label, QStringLiteral("TCP 127.0.0.1:5170"));
    }

    // Capacity eviction trims the index to MAX_ENTRIES and deletes the
    // matching per-uuid JSON file.
    void TestSessionHistoryEvictsOldestPastCapacity()
    {
        const QTemporaryDir sessionsDir;
        QVERIFY(sessionsDir.isValid());

        SessionHistoryManager manager(QDir(sessionsDir.path()), std::make_unique<InMemoryRecentsIndexStorage>());

        const int capacityPlus = SessionHistoryManager::MAX_ENTRIES + 3;
        QStringList writtenUuids;
        for (int i = 0; i < capacityPlus; ++i)
        {
            loglib::LogConfiguration cfg;
            cfg.source = loglib::Source{
                .kind = loglib::Source::Kind::File,
                .locators = {QStringLiteral("C:/logs/file-%1.json").arg(i).toStdString()},
            };
            const QString uuid = manager.WriteSnapshot(cfg);
            QVERIFY(!uuid.isEmpty());
            writtenUuids.append(uuid);
        }

        const QList<RecentSessionEntry> list = manager.List();
        QCOMPARE(list.size(), SessionHistoryManager::MAX_ENTRIES);

        // The newest writes survive (writtenUuids are in chronological
        // order; the last MAX_ENTRIES uuids should be in the list).
        for (int i = capacityPlus - SessionHistoryManager::MAX_ENTRIES; i < capacityPlus; ++i)
        {
            QVERIFY2(
                std::any_of(
                    list.begin(), list.end(), [&](const RecentSessionEntry &e) { return e.uuid == writtenUuids[i]; }
                ),
                qPrintable(QStringLiteral("uuid index %1 must still be in the recents list").arg(i))
            );
        }

        // The evicted uuids must not be on disk anymore.
        for (int i = 0; i < capacityPlus - SessionHistoryManager::MAX_ENTRIES; ++i)
        {
            QVERIFY2(
                !QFileInfo::exists(manager.PathForUuid(writtenUuids[i])),
                qPrintable(QStringLiteral("evicted uuid %1 must be deleted from disk").arg(i))
            );
        }
    }

    // `LastSessionPath` returns the most recent snapshot. After a
    // `Touch` on an older entry, that entry becomes the head.
    void TestSessionHistoryTouchPromotesEntry()
    {
        const QTemporaryDir sessionsDir;
        QVERIFY(sessionsDir.isValid());

        SessionHistoryManager manager(QDir(sessionsDir.path()), std::make_unique<InMemoryRecentsIndexStorage>());

        loglib::LogConfiguration cfgA;
        cfgA.source = loglib::Source{.kind = loglib::Source::Kind::File, .locators = {"C:/logs/a.json"}};
        const QString uuidA = manager.WriteSnapshot(cfgA);

        loglib::LogConfiguration cfgB;
        cfgB.source = loglib::Source{.kind = loglib::Source::Kind::File, .locators = {"C:/logs/b.json"}};
        const QString uuidB = manager.WriteSnapshot(cfgB);

        // Newest (B) is the head; LastSessionPath points at B.
        const auto lastPath = manager.LastSessionPath();
        QVERIFY(lastPath.has_value());
        QCOMPARE(*lastPath, manager.PathForUuid(uuidB));

        // Touch A -> A becomes the head.
        QVERIFY(manager.Touch(uuidA));
        const auto afterTouch = manager.LastSessionPath();
        QVERIFY(afterTouch.has_value());
        QCOMPARE(*afterTouch, manager.PathForUuid(uuidA));
        QCOMPARE(manager.List().front().uuid, uuidA);
    }

    // `Remove` drops the entry + its on-disk JSON and resets
    // `LastSessionPath` to the new head (or `nullopt` when the index
    // empties out).
    void TestSessionHistoryRemoveClearsLastWhenLastRemoved()
    {
        const QTemporaryDir sessionsDir;
        QVERIFY(sessionsDir.isValid());

        SessionHistoryManager manager(QDir(sessionsDir.path()), std::make_unique<InMemoryRecentsIndexStorage>());

        loglib::LogConfiguration cfg;
        cfg.source = loglib::Source{.kind = loglib::Source::Kind::File, .locators = {"C:/logs/only.json"}};
        const QString uuid = manager.WriteSnapshot(cfg);
        QVERIFY(QFileInfo::exists(manager.PathForUuid(uuid)));

        manager.Remove(uuid);
        QCOMPARE(manager.List().size(), 0);
        QVERIFY2(
            !manager.LastSessionPath().has_value(), "LastSessionPath must be nullopt after the last entry is removed"
        );
        QVERIFY(!QFileInfo::exists(manager.PathForUuid(uuid)));
    }

    // `Clear` empties the index, deletes every per-uuid JSON, and
    // resets the last-session pointer.
    void TestSessionHistoryClearWipesEverything()
    {
        const QTemporaryDir sessionsDir;
        QVERIFY(sessionsDir.isValid());

        SessionHistoryManager manager(QDir(sessionsDir.path()), std::make_unique<InMemoryRecentsIndexStorage>());

        QStringList uuids;
        for (int i = 0; i < 3; ++i)
        {
            loglib::LogConfiguration cfg;
            cfg.source = loglib::Source{
                .kind = loglib::Source::Kind::File,
                .locators = {QStringLiteral("C:/logs/cleared-%1.json").arg(i).toStdString()},
            };
            uuids.append(manager.WriteSnapshot(cfg));
        }
        QCOMPARE(manager.List().size(), 3);

        manager.Clear();
        QCOMPARE(manager.List().size(), 0);
        QVERIFY(!manager.LastSessionPath().has_value());
        for (const QString &uuid : uuids)
        {
            QVERIFY(!QFileInfo::exists(manager.PathForUuid(uuid)));
        }
    }

    // -------------------------------------------------------------------------
    // Recents hardening: defence-in-depth uuid validation across
    // the storage boundary, `PathForUuid`, and
    // `RemoveUuidFileLocked` so a hostile profile cannot escape
    // the sessions directory.
    // -------------------------------------------------------------------------

    void TestRecentsRejectsMaliciousUuid()
    {
        const QTemporaryDir sessionsDir;
        QVERIFY(sessionsDir.isValid());
        const QTemporaryDir outsideDir;
        QVERIFY(outsideDir.isValid());

        SessionHistoryManager manager(QDir(sessionsDir.path()), std::make_unique<InMemoryRecentsIndexStorage>());

        // Canary file outside the sessions dir; must survive every
        // hostile-uuid recents call.
        const QString canaryPath = outsideDir.filePath(QStringLiteral("canary.json"));
        {
            QFile canary(canaryPath);
            QVERIFY(canary.open(QIODevice::WriteOnly));
            canary.write("DO NOT DELETE");
        }
        QVERIFY(QFileInfo::exists(canaryPath));

        // `PathForUuid` returns empty for a non-uuid stem so every
        // downstream sink becomes a no-op.
        for (const QString &hostile : {
                 QStringLiteral("../canary"),
                 QStringLiteral("..\\..\\..\\canary"),
                 QStringLiteral("/etc/passwd"),
                 QStringLiteral(""),
                 QStringLiteral("not-a-uuid"),
             })
        {
            QCOMPARE(manager.PathForUuid(hostile), QString());
            manager.Remove(hostile);
            QVERIFY(!manager.Touch(hostile));
        }

        // Canary survives: no hostile uuid composed into a path
        // that reached it.
        QVERIFY2(QFileInfo::exists(canaryPath), "canary file must survive every hostile-uuid call");
    }

    void TestRecentsCorruptSizeIsCapped()
    {
        const QTemporaryDir sessionsDir;
        QVERIFY(sessionsDir.isValid());

        // Plant a bogus `size` value and assert `Read` clamps to
        // the cap instead of allocating gigabytes.
        QSettings settings;
        const QStringList previousAll = settings.allKeys();
        // Snapshot/restore so we don't poison sibling tests.
        QHash<QString, QVariant> snapshot;
        for (const QString &key : previousAll)
        {
            snapshot.insert(key, settings.value(key));
        }
        auto restoreGuard = qScopeGuard([&] {
            settings.clear();
            for (auto it = snapshot.constBegin(); it != snapshot.constEnd(); ++it)
            {
                settings.setValue(it.key(), it.value());
            }
            settings.sync();
        });

        settings.clear();
        settings.setValue(QStringLiteral("recentSessions/size"), std::numeric_limits<int>::max());
        settings.sync();

        const QSettingsRecentsIndexStorage storage;
        // No entries written; the cap bounds the read loop.
        const QList<RecentSessionEntry> entries = storage.Read();
        QVERIFY(entries.isEmpty());
    }

    void TestRecentsStorageDropsMalformedUuidSlots()
    {
        QSettings settings;
        const QStringList previousAll = settings.allKeys();
        QHash<QString, QVariant> snapshot;
        for (const QString &key : previousAll)
        {
            snapshot.insert(key, settings.value(key));
        }
        auto restoreGuard = qScopeGuard([&] {
            settings.clear();
            for (auto it = snapshot.constBegin(); it != snapshot.constEnd(); ++it)
            {
                settings.setValue(it.key(), it.value());
            }
            settings.sync();
        });

        settings.clear();
        // Three slots: real uuid + hostile path + empty string.
        // Read must surface only the real entry.
        const QString realUuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
        settings.setValue(QStringLiteral("recentSessions/size"), 3);
        settings.setValue(QStringLiteral("recentSessions/entries/0/uuid"), realUuid);
        settings.setValue(QStringLiteral("recentSessions/entries/0/label"), QStringLiteral("real"));
        settings.setValue(QStringLiteral("recentSessions/entries/1/uuid"), QStringLiteral("../bad"));
        settings.setValue(QStringLiteral("recentSessions/entries/2/uuid"), QString());
        settings.sync();

        const QSettingsRecentsIndexStorage storage;
        const QList<RecentSessionEntry> entries = storage.Read();
        QCOMPARE(entries.size(), 1);
        QCOMPARE(entries.front().uuid, realUuid);
    }

    void TestRecentsVersionKeyWrittenOnFirstSnapshot()
    {
        QSettings settings;
        const QStringList previousAll = settings.allKeys();
        QHash<QString, QVariant> snapshot;
        for (const QString &key : previousAll)
        {
            snapshot.insert(key, settings.value(key));
        }
        auto restoreGuard = qScopeGuard([&] {
            settings.clear();
            for (auto it = snapshot.constBegin(); it != snapshot.constEnd(); ++it)
            {
                settings.setValue(it.key(), it.value());
            }
            settings.sync();
        });

        settings.clear();
        QVERIFY(!settings.contains(QStringLiteral("recentSessions/version")));

        const QTemporaryDir sessionsDir;
        QVERIFY(sessionsDir.isValid());
        SessionHistoryManager manager(QDir(sessionsDir.path()), std::make_unique<QSettingsRecentsIndexStorage>());

        loglib::LogConfiguration cfg;
        cfg.source = loglib::Source{.kind = loglib::Source::Kind::File, .locators = {"C:/logs/versioned.json"}};
        QVERIFY(!manager.WriteSnapshot(cfg).isEmpty());

        const QSettings probe;
        QCOMPARE(probe.value(QStringLiteral("recentSessions/version")).toInt(), 1);
    }

    void TestRecentsCleanupSkipsNonUuidFiles()
    {
        const QTemporaryDir sessionsDir;
        QVERIFY(sessionsDir.isValid());
        const QDir dir(sessionsDir.path());

        SessionHistoryManager manager(dir, std::make_unique<InMemoryRecentsIndexStorage>());

        // The orphan sweeper must leave non-uuid files alone.
        const QString notesPath = dir.filePath(QStringLiteral("notes.json"));
        {
            QFile notes(notesPath);
            QVERIFY(notes.open(QIODevice::WriteOnly));
            notes.write("hand-written notes");
        }

        (void)manager.CleanupOrphanFiles();

        QVERIFY2(QFileInfo::exists(notesPath), "non-uuid file in the sessions directory must survive cleanup");
    }

    void TestRecentsCleanupRemovesOrphanedUuidJson()
    {
        const QTemporaryDir sessionsDir;
        QVERIFY(sessionsDir.isValid());
        const QDir dir(sessionsDir.path());

        SessionHistoryManager manager(dir, std::make_unique<InMemoryRecentsIndexStorage>());

        // Plant a uuid-shaped file that the index does not reference.
        const QString orphanUuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const QString orphanPath = dir.filePath(orphanUuid + QStringLiteral(".json"));
        {
            QFile orphan(orphanPath);
            QVERIFY(orphan.open(QIODevice::WriteOnly));
            orphan.write("{}");
        }
        QVERIFY(QFileInfo::exists(orphanPath));

        (void)manager.CleanupOrphanFiles();
        QVERIFY2(!QFileInfo::exists(orphanPath), "orphan uuid JSON must be deleted by CleanupOrphanFiles");
    }

    // Lock order: cross-process first, `mMutex` second. A writer
    // blocked on the file lock no longer holds `mMutex`, so
    // concurrent `List()` readers stay responsive.
    void TestRecentsListReadsAreNotBlockedByCrossProcessLock()
    {
        const QTemporaryDir sessionsDir;
        QVERIFY(sessionsDir.isValid());
        QVERIFY(QDir(sessionsDir.path()).mkpath(QStringLiteral(".")));

        const SessionHistoryManager manager(QDir(sessionsDir.path()), std::make_unique<InMemoryRecentsIndexStorage>());

        // Hold the cross-process lock from a sibling thread so the
        // manager's own mutators can't acquire it. The thread waits
        // a beat before unlocking, simulating a slow sibling writer.
        std::atomic<bool> startSignal{false};
        std::atomic<bool> stopSignal{false};
        std::thread holder([&] {
            QLockFile holderLock(QDir(sessionsDir.path()).filePath(QStringLiteral("recents.lock")));
            QVERIFY(holderLock.tryLock(2000));
            startSignal.store(true, std::memory_order_release);
            static_cast<void>(test_common::WaitUntil([&] { return stopSignal.load(std::memory_order_acquire); }));
            holderLock.unlock();
        });
        QVERIFY(test_common::WaitUntil([&] { return startSignal.load(std::memory_order_acquire); }));

        // `List()` skips the cross-process lock by contract and
        // must return promptly even with the lock file held.
        QElapsedTimer timer;
        timer.start();
        const QList<RecentSessionEntry> result = manager.List();
        QVERIFY2(
            timer.elapsed() < 200,
            qPrintable(QStringLiteral("List() must not stall on cross-process lock; took %1ms").arg(timer.elapsed()))
        );
        Q_UNUSED(result);

        stopSignal.store(true, std::memory_order_release);
        holder.join();
    }

    // Regression: `New Session` must detach the window from the
    // previous uuid so opening another file mints a fresh entry
    // instead of rewriting uuidA's JSON in place.
    void TestNewSessionDoesNotOverwritePreviousRecentsEntry()
    {
        const QTemporaryDir sessionsDir;
        QVERIFY(sessionsDir.isValid());

        SessionHistoryManager manager(QDir(sessionsDir.path()), std::make_unique<InMemoryRecentsIndexStorage>());
        auto wired = std::make_unique<MainWindow>(mTheme.data(), &manager, nullptr);

        const TempJsonFile fixtureA({QStringLiteral(R"({"msg": "alpha"})")});
        const TempJsonFile fixtureB({QStringLiteral(R"({"msg": "beta"})")});

        QSignalSpy finishedSpy(wired->Model(), &LogModel::streamingFinished);
        QVERIFY(finishedSpy.isValid());

        wired->OpenFilesForTest({fixtureA.Path()}, MainWindow::OpenMode::Append);
        QVERIFY(finishedSpy.wait(60000));
        QCoreApplication::processEvents();
        QCOMPARE(manager.List().size(), 1);
        const QString uuidA = manager.List().front().uuid;
        const QString pathA = manager.PathForUuid(uuidA);
        QVERIFY(QFileInfo::exists(pathA));

        // Discard the active session. After this, opening a new file
        // must NOT reuse uuidA.
        wired->findChild<QAction *>(QStringLiteral("actionNewSession"))->trigger();
        QCoreApplication::processEvents();

        finishedSpy.clear();
        wired->OpenFilesForTest({fixtureB.Path()}, MainWindow::OpenMode::Append);
        QVERIFY(finishedSpy.wait(60000));
        QCoreApplication::processEvents();

        const QList<RecentSessionEntry> entries = manager.List();
        QCOMPARE(entries.size(), 2);
        const bool uuidAStillPresent = (entries.front().uuid == uuidA) || (entries.back().uuid == uuidA);
        QVERIFY2(uuidAStillPresent, "uuidA must still be in the recents index");
        QVERIFY2(entries.front().uuid != entries.back().uuid, "the two sessions must have distinct uuids");

        // Both JSON files exist on disk and decode independently.
        QVERIFY(QFileInfo::exists(pathA));
        const loglib::LogConfiguration probeA = loglib::LoadConfiguration(pathA.toStdString());
        QVERIFY(probeA.source.has_value());
        QCOMPARE(probeA.source->locators.size(), static_cast<std::size_t>(1));
        QCOMPARE(
            QString::fromStdString(probeA.source->locators.front().displayPath),
            logapp::CanonicalDisplayPath(fixtureA.Path())
        );
    }

    // Companion: `OpenMode::Replace` must detach the window from
    // the previous uuid so the replaced session gets a fresh entry.
    void TestReplaceOpenDoesNotOverwritePreviousRecentsEntry()
    {
        const QTemporaryDir sessionsDir;
        QVERIFY(sessionsDir.isValid());

        SessionHistoryManager manager(QDir(sessionsDir.path()), std::make_unique<InMemoryRecentsIndexStorage>());
        auto wired = std::make_unique<MainWindow>(mTheme.data(), &manager, nullptr);

        const TempJsonFile fixtureA({QStringLiteral(R"({"msg": "alpha"})")});
        const TempJsonFile fixtureB({QStringLiteral(R"({"msg": "beta"})")});

        QSignalSpy finishedSpy(wired->Model(), &LogModel::streamingFinished);
        QVERIFY(finishedSpy.isValid());

        wired->OpenFilesForTest({fixtureA.Path()}, MainWindow::OpenMode::Append);
        QVERIFY(finishedSpy.wait(60000));
        QCoreApplication::processEvents();
        QCOMPARE(manager.List().size(), 1);
        const QString uuidA = manager.List().front().uuid;

        // Shift-Open (destructive Replace) must produce a NEW uuid.
        finishedSpy.clear();
        wired->OpenFilesForTest({fixtureB.Path()}, MainWindow::OpenMode::Replace);
        QVERIFY(finishedSpy.wait(60000));
        QCoreApplication::processEvents();

        const QList<RecentSessionEntry> entries = manager.List();
        QCOMPARE(entries.size(), 2);
        QVERIFY2(
            entries.front().uuid != entries.back().uuid, "Replace mode must not reuse the previous session's uuid"
        );

        const QString pathA = manager.PathForUuid(uuidA);
        QVERIFY2(QFileInfo::exists(pathA), "previous session's JSON must survive a Replace open");
    }

    // `LoadConfiguration` must detach the previous recents pin so
    // the next AutoSave doesn't rewrite the prior session in place.
    void TestLoadConfigurationDetachesPreviousRecentsEntry()
    {
        const QTemporaryDir sessionsDir;
        QVERIFY(sessionsDir.isValid());

        SessionHistoryManager manager(QDir(sessionsDir.path()), std::make_unique<InMemoryRecentsIndexStorage>());
        auto wired = std::make_unique<MainWindow>(mTheme.data(), &manager, nullptr);

        const TempJsonFile fixtureA({QStringLiteral(R"({"msg": "alpha"})")});
        const TempJsonFile fixtureB({QStringLiteral(R"({"msg": "beta"})")});

        QSignalSpy finishedSpy(wired->Model(), &LogModel::streamingFinished);
        QVERIFY(finishedSpy.isValid());

        // Establish session A.
        wired->OpenFilesForTest({fixtureA.Path()}, MainWindow::OpenMode::Append);
        QVERIFY(finishedSpy.wait(60000));
        QCoreApplication::processEvents();
        QCOMPARE(manager.List().size(), 1);
        const QString uuidA = manager.List().front().uuid;
        const QString pathA = manager.PathForUuid(uuidA);
        QVERIFY(QFileInfo::exists(pathA));

        // The menu `LoadConfiguration` path does not pre-detach via
        // `NewSession`; the pin only clears if `DoLoadConfiguration`
        // detaches itself.
        const QTemporaryDir configDir;
        QVERIFY(configDir.isValid());
        const QString configPath = QDir(configDir.path()).filePath(QStringLiteral("columns_only.json"));
        wired->SaveConfigurationToPathForTest(configPath, loglib::SaveScope::Columns);
        wired->LoadConfigurationFromPathForTest(configPath);
        QCoreApplication::processEvents();

        // No pinned uuid after the load so the next AutoSave forks
        // rather than overwriting session A.
        QVERIFY2(wired->ActiveSessionUuid().isEmpty(), "LoadConfiguration must detach the previous recents pin");

        finishedSpy.clear();
        wired->OpenFilesForTest({fixtureB.Path()}, MainWindow::OpenMode::Append);
        QVERIFY(finishedSpy.wait(60000));
        QCoreApplication::processEvents();

        const QList<RecentSessionEntry> entries = manager.List();
        QCOMPARE(entries.size(), 2);
        QVERIFY2(
            entries.front().uuid != entries.back().uuid,
            "loading a configuration and opening new files must yield distinct uuids"
        );

        // Session A's JSON is untouched.
        QVERIFY(QFileInfo::exists(pathA));
        const loglib::LogConfiguration probeA = loglib::LoadConfiguration(pathA.toStdString());
        QVERIFY(probeA.source.has_value());
        QCOMPARE(probeA.source->locators.size(), static_cast<std::size_t>(1));
        QCOMPARE(
            QString::fromStdString(probeA.source->locators.front().displayPath),
            logapp::CanonicalDisplayPath(fixtureA.Path())
        );
    }

    // Single-file probe path: `TryLoadAsConfiguration` succeeding
    // is a session boundary and must detach the previous recents
    // pin -- otherwise a follow-up AutoSave would rewrite the
    // prior session's JSON under the stale uuid.
    //
    // Uses a `SaveScope::Full` session JSON (real-world repro:
    // dropping a recent session JSON onto an active window); a
    // columns-only config would wipe `mCurrentSource` and mask
    // the auto-save corruption.
    void TestTryLoadAsConfigurationDetachesPreviousRecentsEntry()
    {
        const QTemporaryDir sessionsDir;
        QVERIFY(sessionsDir.isValid());

        SessionHistoryManager manager(QDir(sessionsDir.path()), std::make_unique<InMemoryRecentsIndexStorage>());
        auto wired = std::make_unique<MainWindow>(mTheme.data(), &manager, nullptr);

        const TempJsonFile fixtureA({QStringLiteral(R"({"msg": "alpha"})")});
        const TempJsonFile fixtureB({QStringLiteral(R"({"msg": "beta"})")});

        QSignalSpy finishedSpy(wired->Model(), &LogModel::streamingFinished);
        QVERIFY(finishedSpy.isValid());

        // Establish session A and pin its uuid into the window.
        wired->OpenFilesForTest({fixtureA.Path()}, MainWindow::OpenMode::Append);
        QVERIFY(finishedSpy.wait(60000));
        QCoreApplication::processEvents();
        QCOMPARE(manager.List().size(), 1);
        const QString uuidA = manager.List().front().uuid;
        QCOMPARE(wired->ActiveSessionUuid(), uuidA);
        const QString pathA = manager.PathForUuid(uuidA);
        QVERIFY(QFileInfo::exists(pathA));

        // Drive the `DispatchMixedOpenInput` lone-config probe with
        // a Full-scope snapshot of session A's own state (the
        // source points at fixtureA) so the load lands with a
        // valid `mCurrentSource` and the follow-up Append fires.
        const QTemporaryDir configDir;
        QVERIFY(configDir.isValid());
        const QString sessionPath = QDir(configDir.path()).filePath(QStringLiteral("session_full.json"));
        wired->SaveConfigurationToPathForTest(sessionPath, loglib::SaveScope::Full);
        QVERIFY2(
            wired->TryLoadAsConfigurationForTest(sessionPath),
            "TryLoadAsConfiguration must succeed on a saved session JSON"
        );
        QCoreApplication::processEvents();

        // No pinned uuid after a successful probe.
        QVERIFY2(
            wired->ActiveSessionUuid().isEmpty(),
            "TryLoadAsConfiguration must detach the previous recents pin on a successful load"
        );

        // Append fires AutoSave; with the detach, a brand-new uuid
        // is minted instead of clobbering session A.
        finishedSpy.clear();
        wired->OpenFilesForTest({fixtureB.Path()}, MainWindow::OpenMode::Append);
        QVERIFY(finishedSpy.wait(60000));
        QCoreApplication::processEvents();

        const QList<RecentSessionEntry> entries = manager.List();
        QCOMPARE(entries.size(), 2);
        QVERIFY2(
            entries.front().uuid != entries.back().uuid,
            "loading a session JSON via the probe path and appending new files must yield distinct uuids"
        );
        QVERIFY2(wired->ActiveSessionUuid() != uuidA, "follow-up AutoSave must not reuse the prior pin");

        // Session A's JSON is untouched: single-locator File
        // source still points at fixtureA.
        QVERIFY(QFileInfo::exists(pathA));
        const loglib::LogConfiguration probeA = loglib::LoadConfiguration(pathA.toStdString());
        QVERIFY(probeA.source.has_value());
        QCOMPARE(probeA.source->locators.size(), static_cast<std::size_t>(1));
        QCOMPARE(
            QString::fromStdString(probeA.source->locators.front().displayPath),
            logapp::CanonicalDisplayPath(fixtureA.Path())
        );
    }

    // Companion: on a probe *failure* the pin survives so the
    // subsequent Append extends the same entry. The detach lives
    // after a successful `Load` for exactly this reason. Also
    // pins that filters / sort survive a failed probe (a prior
    // implementation cleared them pre-Load with no rollback).
    void TestTryLoadAsConfigurationFailurePreservesPreviousRecentsEntry()
    {
        const QTemporaryDir sessionsDir;
        QVERIFY(sessionsDir.isValid());

        SessionHistoryManager manager(QDir(sessionsDir.path()), std::make_unique<InMemoryRecentsIndexStorage>());
        auto wired = std::make_unique<MainWindow>(mTheme.data(), &manager, nullptr);

        // One matching + one non-matching row so the proxy row
        // count is a clean signal for "filter still active".
        const TempJsonFile fixtureA({QStringLiteral(R"({"msg": "alpha"})"), QStringLiteral(R"({"msg": "beta"})")});

        QSignalSpy finishedSpy(wired->Model(), &LogModel::streamingFinished);
        QVERIFY(finishedSpy.isValid());

        wired->OpenFilesForTest({fixtureA.Path()}, MainWindow::OpenMode::Append);
        QVERIFY(finishedSpy.wait(60000));
        QCoreApplication::processEvents();
        QCOMPARE(manager.List().size(), 1);
        const QString uuidA = manager.List().front().uuid;
        QCOMPARE(wired->ActiveSessionUuid(), uuidA);

        auto *model = wired->Model();
        const int msgCol = ColumnByHeader(*model, QStringLiteral("msg"));
        QVERIFY2(msgCol >= 0, "msg column must exist after streaming");
        auto *filterModel = wired->FilterModel();
        QVERIFY(filterModel != nullptr);
        QCOMPARE(filterModel->rowCount(), 2);

        // Substring filter that drops `beta`; rowCount falls to 1.
        // A leaked-clear regression would bump it back to 2.
        QVERIFY2(
            QMetaObject::invokeMethod(
                wired.get(),
                "FilterSubmitted",
                Qt::DirectConnection,
                Q_ARG(QString, QStringLiteral("survives-failed-probe")),
                Q_ARG(int, msgCol),
                Q_ARG(QString, QStringLiteral("alpha")),
                Q_ARG(int, static_cast<int>(loglib::LeafRule::Match::Contains))
            ),
            "FilterSubmitted slot must be invocable via meta-object"
        );
        QCoreApplication::processEvents();
        QCOMPARE(filterModel->rowCount(), 1);
        QCOMPARE(wired->Filters().size(), static_cast<size_t>(1));

        // Pin a deterministic sort so the indicator preservation is
        // also observable.
        wired->findChild<LogTableView *>()->sortByColumn(msgCol, Qt::DescendingOrder);
        QCoreApplication::processEvents();
        QCOMPARE(filterModel->SortColumn(), msgCol);
        QCOMPARE(filterModel->SortOrder(), Qt::DescendingOrder);

        // Write a deliberately-non-JSON payload so `TryLoadAsConfiguration`
        // fails inside `Load(...)`.
        const QTemporaryDir garbageDir;
        QVERIFY(garbageDir.isValid());
        const QString garbagePath = QDir(garbageDir.path()).filePath(QStringLiteral("not_a_config.txt"));
        {
            std::ofstream stream(garbagePath.toStdString(), std::ios::binary);
            QVERIFY(stream.is_open());
            stream << "this is not json";
        }
        QVERIFY2(
            !wired->TryLoadAsConfigurationForTest(garbagePath),
            "TryLoadAsConfiguration must reject a non-configuration file"
        );
        QCoreApplication::processEvents();

        QVERIFY2(
            wired->ActiveSessionUuid() == uuidA,
            "failed probe must preserve the prior recents pin so an Append open extends the same entry"
        );

        // Filters and sort survive the failed probe.
        QCOMPARE(wired->Filters().size(), static_cast<size_t>(1));
        QCOMPARE(filterModel->rowCount(), 1);
        QCOMPARE(filterModel->SortColumn(), msgCol);
        QCOMPARE(filterModel->SortOrder(), Qt::DescendingOrder);
    }

    // `CleanupOrphanFiles` deletes per-uuid JSONs that are not in the
    // index. Index-referenced files survive; the lock file is left
    // alone.
    void TestCleanupOrphanFilesRemovesStaleJsons()
    {
        const QTemporaryDir sessionsDir;
        QVERIFY(sessionsDir.isValid());

        SessionHistoryManager manager(QDir(sessionsDir.path()), std::make_unique<InMemoryRecentsIndexStorage>());

        // Seed one valid entry.
        loglib::LogConfiguration cfg;
        cfg.source = loglib::Source{.kind = loglib::Source::Kind::File, .locators = {"C:/logs/kept.json"}};
        const QString keptUuid = manager.WriteSnapshot(cfg);
        QVERIFY(!keptUuid.isEmpty());
        const QString keptPath = manager.PathForUuid(keptUuid);
        QVERIFY(QFileInfo::exists(keptPath));

        // Orphan JSON (simulates a crash between file-write and
        // index-update). Stem must be uuid-shaped -- the sweeper
        // gates deletion on a strict uuid check.
        const QString orphanPath =
            QDir(sessionsDir.path())
                .filePath(QUuid::createUuid().toString(QUuid::WithoutBraces) + QStringLiteral(".json"));
        {
            QFile orphan(orphanPath);
            QVERIFY(orphan.open(QIODevice::WriteOnly));
            orphan.write("{\"columns\": []}\n");
        }
        QVERIFY(QFileInfo::exists(orphanPath));

        // Leftover `.json.tmp` (atomic-write crash). Same uuid-
        // stem requirement applies to `*.json.tmp` as to `*.json`.
        const QString staleTempPath =
            QDir(sessionsDir.path())
                .filePath(QUuid::createUuid().toString(QUuid::WithoutBraces) + QStringLiteral(".json.tmp"));
        {
            QFile staleTemp(staleTempPath);
            QVERIFY(staleTemp.open(QIODevice::WriteOnly));
            staleTemp.write("{\"columns\": [");
        }
        QVERIFY(QFileInfo::exists(staleTempPath));

        (void)manager.CleanupOrphanFiles();

        QVERIFY2(QFileInfo::exists(keptPath), "indexed entries must survive cleanup");
        QVERIFY2(!QFileInfo::exists(orphanPath), "orphan JSON must be removed");
        QVERIFY2(!QFileInfo::exists(staleTempPath), "stale .json.tmp must be removed");
    }

    // The orphan sweeper must NOT delete files whose stem isn't a
    // QUuid -- silently wiping a user's `notes.json` would be a
    // foot-gun.
    void TestCleanupOrphanFilesPreservesNonUuidStems()
    {
        const QTemporaryDir sessionsDir;
        QVERIFY(sessionsDir.isValid());

        SessionHistoryManager manager(QDir(sessionsDir.path()), std::make_unique<InMemoryRecentsIndexStorage>());

        // Seed an entry to mkpath the directory.
        loglib::LogConfiguration cfg;
        cfg.source = loglib::Source{.kind = loglib::Source::Kind::File, .locators = {"C:/logs/seed.json"}};
        const QString seedUuid = manager.WriteSnapshot(cfg);
        QVERIFY(!seedUuid.isEmpty());

        // Plant a foreign file with a non-uuid stem.
        const QString foreignPath = QDir(sessionsDir.path()).filePath(QStringLiteral("notes.json"));
        {
            QFile foreign(foreignPath);
            QVERIFY(foreign.open(QIODevice::WriteOnly));
            foreign.write("user notes -- not ours");
        }
        QVERIFY(QFileInfo::exists(foreignPath));

        // Symmetric check for `.json.tmp` stems.
        const QString foreignTempPath = QDir(sessionsDir.path()).filePath(QStringLiteral("scratch.json.tmp"));
        {
            QFile foreignTemp(foreignTempPath);
            QVERIFY(foreignTemp.open(QIODevice::WriteOnly));
            foreignTemp.write("scratch space");
        }
        QVERIFY(QFileInfo::exists(foreignTempPath));

        (void)manager.CleanupOrphanFiles();

        QVERIFY2(QFileInfo::exists(foreignPath), "non-uuid `notes.json` must survive cleanup");
        QVERIFY2(QFileInfo::exists(foreignTempPath), "non-uuid `scratch.json.tmp` must survive cleanup");
        QVERIFY2(QFileInfo::exists(manager.PathForUuid(seedUuid)), "indexed entry must still be present");
    }

    // Regression: closeEvent used to write a phantom recents
    // entry for a *finished* live-tail session because
    // `streamingFinished` resets `mSessionMode` to `Idle`.
    // `mLastTerminalSessionMode` retains the real mode so the
    // auto-save gate bails.
    void TestCloseAfterFinishedLiveTailDoesNotCreatePhantomRecentsEntry()
    {
        const QTemporaryDir sessionsDir;
        QVERIFY(sessionsDir.isValid());

        SessionHistoryManager manager(QDir(sessionsDir.path()), std::make_unique<InMemoryRecentsIndexStorage>());
        auto wired = std::make_unique<MainWindow>(mTheme.data(), &manager, nullptr);

        wired->SetCurrentSourceForTest(
            loglib::Source{.kind = loglib::Source::Kind::File, .locators = {"/tmp/livetail-finished.log"}}
        );
        wired->SetSessionModeForTest(MainWindow::TestSessionMode::LiveTail);

        // Drive a real `streamingFinished` so the MainWindow lambda
        // captures `mLastTerminalSessionMode = LiveTail` before
        // resetting `mSessionMode = Idle`.
        auto *model = wired->Model();
        QVERIFY(model != nullptr);
        const QSignalSpy finishedSpy(model, &LogModel::streamingFinished);
        QVERIFY(finishedSpy.isValid());
        BeginSyntheticStreamSession(*model);
        QVERIFY(model->IsStreamingActive());
        model->StopAndKeepRows();
        QCoreApplication::processEvents();
        QVERIFY2(finishedSpy.count() >= 1, "StopAndKeepRows must emit streamingFinished");

        QVERIFY2(manager.List().isEmpty(), "preconditions: no recents entries before close");

        wired->close();
        QCoreApplication::processEvents();

        QVERIFY2(
            manager.List().isEmpty(),
            "closeEvent must not write a Recent Sessions entry for a finished live-tail session"
        );
    }

    void TestLoadFromStringParsesValidConfiguration()
    {
        // `LoadFromString` is the in-memory pre-flight that
        // `FileLooksLikeConfiguration` uses to bound disk IO.
        const QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("seed.json"));
        {
            loglib::LogConfiguration cfg;
            loglib::Column col;
            col.header = "msg";
            col.keys = {"msg"};
            cfg.columns.push_back(col);
            loglib::SaveConfiguration(cfg, path.toStdString(), loglib::SaveScope::Full);
        }
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QByteArray content = file.readAll();

        const loglib::LogConfiguration parsed =
            loglib::ParseConfiguration(std::string_view(content.constData(), static_cast<size_t>(content.size())));
        QCOMPARE(parsed.columns.size(), static_cast<size_t>(1));
        QCOMPARE(QString::fromStdString(parsed.columns.front().header), QStringLiteral("msg"));
    }

    void TestLoadFromStringRejectsNonJsonGarbage()
    {
        bool threw = false;
        try
        {
            static_cast<void>(loglib::ParseConfiguration("definitely not json {{"));
        }
        catch (const std::exception &)
        {
            // Latch the throw so `bugprone-empty-catch` is happy.
            threw = true;
        }
        QVERIFY2(threw, "LoadFromString must throw on garbage input");
    }

    // `EvictLocked` writes the index before unlinking so a crash
    // between the two leaves an orphan JSON (recovered by
    // `CleanupOrphanFiles`) rather than a dangling index entry.
    // Steady-state observable invariant is unchanged.
    void TestEvictionKeepsIndexConsistentWithDisk()
    {
        const QTemporaryDir sessionsDir;
        QVERIFY(sessionsDir.isValid());

        SessionHistoryManager manager(QDir(sessionsDir.path()), std::make_unique<InMemoryRecentsIndexStorage>());

        const int capacityPlus = SessionHistoryManager::MAX_ENTRIES + 1;
        QStringList writtenUuids;
        for (int i = 0; i < capacityPlus; ++i)
        {
            loglib::LogConfiguration cfg;
            cfg.source = loglib::Source{
                .kind = loglib::Source::Kind::File,
                .locators = {QStringLiteral("C:/logs/evict-%1.json").arg(i).toStdString()},
            };
            const QString uuid = manager.WriteSnapshot(cfg);
            QVERIFY(!uuid.isEmpty());
            writtenUuids.append(uuid);
        }

        // Every indexed entry has its JSON on disk.
        const QList<RecentSessionEntry> list = manager.List();
        QCOMPARE(list.size(), SessionHistoryManager::MAX_ENTRIES);
        for (const RecentSessionEntry &e : list)
        {
            QVERIFY2(
                QFileInfo::exists(manager.PathForUuid(e.uuid)),
                qPrintable(QStringLiteral("indexed entry %1 must have backing JSON on disk").arg(e.uuid))
            );
        }

        // The evicted (oldest) uuid has no backing file.
        QVERIFY2(
            !QFileInfo::exists(manager.PathForUuid(writtenUuids.first())),
            "evicted entry's JSON must be removed from disk"
        );
    }

    // Atomic temp+rename: any temp-file failure must throw and
    // leave the destination untouched. We can't fault-inject a
    // close failure portably; instead exercise an open failure
    // (parent dir missing), which honours the same contract.
    void TestSaveTempFailureDoesNotClobberDestination()
    {
        const QTemporaryDir scratchDir;
        QVERIFY(scratchDir.isValid());

        // Pre-seed an existing JSON we can detect modifications on.
        const QString destPath = scratchDir.filePath(QStringLiteral("seed.json"));
        loglib::LogConfiguration seeder;
        {
            loglib::ColumnLayout seederLayout;
            seederLayout.AppendKeys({"a", "b"});
            seeder.columns = seederLayout.Columns();
        }
        loglib::SaveConfiguration(seeder, destPath.toStdString(), loglib::SaveScope::Columns);
        QVERIFY(QFileInfo::exists(destPath));
        QFile destFile(destPath);
        QVERIFY(destFile.open(QIODevice::ReadOnly));
        const QByteArray seedBytes = destFile.readAll();
        destFile.close();
        QVERIFY(!seedBytes.isEmpty());

        // Save into a path whose parent dir does not exist; the
        // temp-file open inside the atomic write must fail.
        const QString unreachablePath =
            QDir(scratchDir.path()).filePath(QStringLiteral("does/not/exist/save-target.json"));

        loglib::LogConfiguration doomed;
        {
            loglib::ColumnLayout doomedLayout;
            doomedLayout.AppendKeys({"x", "y"});
            doomed.columns = doomedLayout.Columns();
        }
        bool threw = false;
        try
        {
            loglib::SaveConfiguration(doomed, unreachablePath.toStdString(), loglib::SaveScope::Columns);
        }
        catch (const std::exception &)
        {
            threw = true;
        }
        QVERIFY2(threw, "Save must throw when the temp file cannot be opened");
        QVERIFY2(
            !QFileInfo::exists(unreachablePath), "Save must not have created a partial file at the unreachable path"
        );

        // The pre-existing destination is untouched.
        QVERIFY(destFile.open(QIODevice::ReadOnly));
        const QByteArray afterBytes = destFile.readAll();
        destFile.close();
        QCOMPARE(afterBytes, seedBytes);
    }

    // `MaxEntries` / `SetMaxEntries` round-trip and default fallback
    // (absent key -> `MAX_ENTRIES`).
    void TestMaxEntriesPreferenceRoundTrip()
    {
        QSettings settings;
        const QVariant previous = settings.value(QStringLiteral("recentSessions/maxEntries"));
        auto restoreGuard = qScopeGuard([&] {
            QSettings restore;
            if (previous.isValid())
            {
                restore.setValue(QStringLiteral("recentSessions/maxEntries"), previous);
            }
            else
            {
                restore.remove(QStringLiteral("recentSessions/maxEntries"));
            }
            restore.sync();
        });

        settings.remove(QStringLiteral("recentSessions/maxEntries"));
        settings.sync();
        QCOMPARE(SessionHistoryManager::MaxEntries(), SessionHistoryManager::MAX_ENTRIES);

        SessionHistoryManager::SetMaxEntries(42);
        QCOMPARE(SessionHistoryManager::MaxEntries(), 42);
    }

    // Out-of-range values clamp into
    // `[MAX_ENTRIES_LOWER_BOUND, MAX_ENTRIES_UPPER_BOUND]`.
    void TestMaxEntriesClampedToValidRange()
    {
        const QSettings settings;
        const QVariant previous = settings.value(QStringLiteral("recentSessions/maxEntries"));
        auto restoreGuard = qScopeGuard([&] {
            QSettings restore;
            if (previous.isValid())
            {
                restore.setValue(QStringLiteral("recentSessions/maxEntries"), previous);
            }
            else
            {
                restore.remove(QStringLiteral("recentSessions/maxEntries"));
            }
            restore.sync();
        });

        SessionHistoryManager::SetMaxEntries(0);
        QCOMPARE(SessionHistoryManager::MaxEntries(), SessionHistoryManager::MAX_ENTRIES_LOWER_BOUND);

        SessionHistoryManager::SetMaxEntries(100000);
        QCOMPARE(SessionHistoryManager::MaxEntries(), SessionHistoryManager::MAX_ENTRIES_UPPER_BOUND);

        SessionHistoryManager::SetMaxEntries(-1);
        QCOMPARE(SessionHistoryManager::MaxEntries(), SessionHistoryManager::MAX_ENTRIES_LOWER_BOUND);
    }

    // The runtime cap drives `EvictLocked`: a Preferences edit
    // takes effect on the next `WriteSnapshot`.
    void TestMaxEntriesPreferenceDrivesEviction()
    {
        const QSettings settings;
        const QVariant previous = settings.value(QStringLiteral("recentSessions/maxEntries"));
        auto restoreGuard = qScopeGuard([&] {
            QSettings restore;
            if (previous.isValid())
            {
                restore.setValue(QStringLiteral("recentSessions/maxEntries"), previous);
            }
            else
            {
                restore.remove(QStringLiteral("recentSessions/maxEntries"));
            }
            restore.sync();
        });

        SessionHistoryManager::SetMaxEntries(3);

        const QTemporaryDir sessionsDir;
        QVERIFY(sessionsDir.isValid());
        SessionHistoryManager manager(QDir(sessionsDir.path()), std::make_unique<InMemoryRecentsIndexStorage>());

        for (int i = 0; i < 6; ++i)
        {
            loglib::LogConfiguration cfg;
            cfg.source = loglib::Source{
                .kind = loglib::Source::Kind::File,
                .locators = {QStringLiteral("C:/logs/file-%1.json").arg(i).toStdString()},
            };
            QVERIFY(!manager.WriteSnapshot(cfg).isEmpty());
        }

        const auto entries = manager.List();
        QCOMPARE(entries.size(), 3);
    }

    // `WriteSnapshotAndPublish(true)` performs the JSON write and
    // `openWindowsAtQuit` publish under one cross-process lock.
    void TestWriteSnapshotAndPublishUpdatesOpenWindowsAtomically()
    {
        const QStringList previousOpen = SessionHistoryManager::OpenWindowsAtQuitUnlocked();
        auto restoreGuard = qScopeGuard([&] { SessionHistoryManager::SetOpenWindowsAtQuit(previousOpen); });
        SessionHistoryManager::SetOpenWindowsAtQuit({});

        const QTemporaryDir sessionsDir;
        QVERIFY(sessionsDir.isValid());

        SessionHistoryManager manager(QDir(sessionsDir.path()), std::make_unique<InMemoryRecentsIndexStorage>());

        loglib::LogConfiguration cfg;
        cfg.source = loglib::Source{.kind = loglib::Source::Kind::File, .locators = {"C:/logs/atomic.json"}};

        const QString uuid = manager.WriteSnapshotAndPublish(cfg, QString(), /*publishOpenWindow=*/true);
        QVERIFY(!uuid.isEmpty());

        // uuid visible in both the recents index and the open set.
        const auto entries = manager.List();
        QCOMPARE(entries.size(), 1);
        QCOMPARE(entries.front().uuid, uuid);

        const QStringList open = SessionHistoryManager::OpenWindowsAtQuitUnlocked();
        QVERIFY2(
            open.contains(uuid),
            qPrintable(QStringLiteral("openWindowsAtQuit must contain %1, got: %2").arg(uuid, open.join(", ")))
        );
    }

    // `WriteSnapshotAndPublish(false)` matches plain `WriteSnapshot`:
    // JSON written, open-windows list untouched.
    void TestWriteSnapshotAndPublishSkipsPublishWhenFlagFalse()
    {
        const QStringList previousOpen = SessionHistoryManager::OpenWindowsAtQuitUnlocked();
        auto restoreGuard = qScopeGuard([&] { SessionHistoryManager::SetOpenWindowsAtQuit(previousOpen); });
        SessionHistoryManager::SetOpenWindowsAtQuit({});

        const QTemporaryDir sessionsDir;
        QVERIFY(sessionsDir.isValid());

        SessionHistoryManager manager(QDir(sessionsDir.path()), std::make_unique<InMemoryRecentsIndexStorage>());

        loglib::LogConfiguration cfg;
        cfg.source = loglib::Source{.kind = loglib::Source::Kind::File, .locators = {"C:/logs/noopen.json"}};

        const QString uuid = manager.WriteSnapshotAndPublish(cfg, QString(), /*publishOpenWindow=*/false);
        QVERIFY(!uuid.isEmpty());

        QCOMPARE(manager.List().size(), 1);
        QCOMPARE(SessionHistoryManager::OpenWindowsAtQuitUnlocked(), QStringList{});
    }

    // Reuse-uuid fast path: re-saving with identical metadata
    // keeps the count and head-of-list ordering stable; only the
    // JSON on disk is rewritten.
    void TestWriteSnapshotReuseUuidFastPathIsStable()
    {
        const QTemporaryDir sessionsDir;
        QVERIFY(sessionsDir.isValid());

        SessionHistoryManager manager(QDir(sessionsDir.path()), std::make_unique<InMemoryRecentsIndexStorage>());

        loglib::LogConfiguration cfg;
        cfg.source = loglib::Source{.kind = loglib::Source::Kind::File, .locators = {"C:/logs/reuse.json"}};

        const QString uuid = manager.WriteSnapshot(cfg);
        QVERIFY(!uuid.isEmpty());
        QCOMPARE(manager.List().size(), 1);

        // Re-save with identical metadata; fast path triggers.
        const QString reuse = manager.WriteSnapshot(cfg, uuid);
        QCOMPARE(reuse, uuid);
        const auto entries = manager.List();
        QCOMPARE(entries.size(), 1);
        QCOMPARE(entries.front().uuid, uuid);
        // JSON still exists; only the entries-group rewrite is
        // skipped.
        QVERIFY(QFileInfo::exists(manager.PathForUuid(uuid)));
    }

    // Reuse-uuid with changed metadata takes the slow path so the
    // menu label reflects the new label / locator / fileCount.
    void TestWriteSnapshotReuseUuidPicksSlowPathOnMetadataChange()
    {
        const QTemporaryDir sessionsDir;
        QVERIFY(sessionsDir.isValid());

        SessionHistoryManager manager(QDir(sessionsDir.path()), std::make_unique<InMemoryRecentsIndexStorage>());

        loglib::LogConfiguration cfg;
        cfg.source = loglib::Source{.kind = loglib::Source::Kind::File, .locators = {"C:/logs/initial.json"}};
        const QString uuid = manager.WriteSnapshot(cfg);
        QVERIFY(!uuid.isEmpty());

        // Mutate the source so `BuildLabel` differs; fast-path
        // equality check rejects and rewrites the entry slot.
        cfg.source = loglib::Source{
            .kind = loglib::Source::Kind::File,
            .locators = {"C:/logs/initial.json", "C:/logs/added.json"},
        };
        const QString reuse = manager.WriteSnapshot(cfg, uuid);
        QCOMPARE(reuse, uuid);

        const auto entries = manager.List();
        QCOMPARE(entries.size(), 1);
        QCOMPARE(entries.front().fileCount, 2);
    }

    // Concurrent stress: many threads through `WriteSnapshot` /
    // `Touch` / `Remove` / `List` must not corrupt the index or
    // deadlock. Pins the `mMutex` thread-safety contract.
    void TestSessionHistoryConcurrentStress()
    {
        const QTemporaryDir sessionsDir;
        QVERIFY(sessionsDir.isValid());

        SessionHistoryManager manager(QDir(sessionsDir.path()), std::make_unique<InMemoryRecentsIndexStorage>());

        constexpr int WRITER_THREADS = 3;
        constexpr int OPS_PER_WRITER = 25;
        std::atomic<bool> stop{false};
        std::atomic<int> writes{0};

        auto writer = [&](int seed) {
            for (int i = 0; i < OPS_PER_WRITER; ++i)
            {
                loglib::LogConfiguration cfg;
                cfg.source = loglib::Source{
                    .kind = loglib::Source::Kind::File,
                    .locators = {QStringLiteral("C:/logs/stress-%1-%2.json").arg(seed).arg(i).toStdString()},
                };
                if (!manager.WriteSnapshot(cfg).isEmpty())
                {
                    writes.fetch_add(1);
                }
            }
        };

        auto reader = [&] {
            while (!stop.load())
            {
                // `List()` always returns a self-consistent snapshot.
                const auto entries = manager.List();
                for (const auto &e : entries)
                {
                    QVERIFY2(!e.uuid.isEmpty(), "List entry must never have an empty uuid under stress");
                }
            }
        };

        std::vector<std::thread> writers;
        writers.reserve(WRITER_THREADS);
        std::thread readerThread(reader);
        for (int i = 0; i < WRITER_THREADS; ++i)
        {
            writers.emplace_back(writer, i);
        }
        for (auto &t : writers)
        {
            t.join();
        }
        stop = true;
        readerThread.join();

        // Some writes landed; eviction caps the visible count.
        QVERIFY2(writes.load() > 0, "writer threads must produce at least one successful WriteSnapshot");
        QVERIFY(manager.List().size() <= SessionHistoryManager::MaxEntries());
    }
};

QTEST_MAIN(RecentsTest)
#include "apptest_recents_test.moc"
