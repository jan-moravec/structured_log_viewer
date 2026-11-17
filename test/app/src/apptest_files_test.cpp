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
class FilesTest : public apptest_support::AppTestBase
{
    Q_OBJECT

private slots:

    // End-to-end Stream Mode smoke test against a temp file tailed by
    // `TailingBytesProducer`. Mirrors `MainWindow::OpenLogStream`'s
    // flow but stays self-contained. Covers pre-fill, live append,
    // pause-freezes-view-with-buffer-keeps-growing, and resume-drains.
    // Retention cap is high (1000) so FIFO eviction stays inactive
    // here — the dedicated `testRetentionCap*` tests cover that.
    static void TestStreamModeOpensTailFileAndAppends()
    {
        const TempLiveTailFile fixture;

        // Pre-fill: 100 lines on disk before the source opens.
        {
            std::string blob;
            for (int i = 0; i < 100; ++i)
            {
                blob += "{\"i\":" + std::to_string(i + 1) + ",\"phase\":\"prefill\"}\n";
            }
            fixture.Append(blob);
        }

        LogModel model;
        const QSignalSpy lineCountSpy(&model, &LogModel::lineCountChanged);
        QVERIFY(lineCountSpy.isValid());
        const QSignalSpy finishedSpy(&model, &LogModel::streamingFinished);
        QVERIFY(finishedSpy.isValid());

        model.SetRetentionCap(1000);

        // Use a fast poll cadence on the source so the test wall-clock stays
        // small; the production default is 250 ms which would push the test
        // budget over a CI runner's timeout for marginal benefit here.
        loglib::TailingBytesProducer::Options sourceOptions;
        sourceOptions.disableNativeWatcher = true; // poll-only for determinism
        sourceOptions.pollInterval = std::chrono::milliseconds(25);
        sourceOptions.rotationDebounce = std::chrono::milliseconds(250);

        const std::filesystem::path filePath(fixture.Path().toStdString());
        auto source = std::make_unique<loglib::TailingBytesProducer>(filePath, /*retentionLines=*/1000, sourceOptions);
        auto streamSource = std::make_unique<loglib::StreamLineSource>(filePath, std::move(source));

        const loglib::ParserOptions options;
        // Don't pass a configuration; the auto-promote heuristics aren't the
        // focus of this test. The model's `BeginStreaming(StreamLineSource)`
        // overrides `options.stopToken` with the sink's freshly-armed token.
        const loglib::StopToken stopToken = model.BeginStreaming(std::move(streamSource), options);
        Q_UNUSED(stopToken);

        QVERIFY(model.IsStreamingActive());
        QVERIFY2(WaitForLineCount(model, 100, std::chrono::seconds(5)), "pre-fill must arrive within 5 s");
        QCOMPARE(model.rowCount(), 100);

        // Append 50 more lines while the stream is running.
        {
            std::string blob;
            for (int i = 0; i < 50; ++i)
            {
                blob += "{\"i\":" + std::to_string(101 + i) + ",\"phase\":\"running\"}\n";
            }
            fixture.Append(blob);
        }
        QVERIFY2(WaitForLineCount(model, 150, std::chrono::seconds(5)), "running-phase appends must arrive within 5 s");
        QCOMPARE(model.rowCount(), 150);

        // Pause: visible model freezes; subsequent appends land in the
        // paused buffer.
        QtStreamingLogSink *sink = model.Sink();
        QVERIFY(sink != nullptr);
        sink->Pause();
        QVERIFY(sink->IsPaused());

        {
            std::string blob;
            for (int i = 0; i < 50; ++i)
            {
                blob += "{\"i\":" + std::to_string(151 + i) + ",\"phase\":\"paused\"}\n";
            }
            fixture.Append(blob);
        }

        // Wait for the worker to drain the appended bytes into the paused
        // buffer. We can't `WaitForLineCount` here because visible row
        // count must stay frozen at 150 — the buffer is the side channel.
        // Drain Qt events too in case any lambdas slip through (they
        // shouldn't while paused).
        QVERIFY2(
            WaitUntilProcessingEvents([&] { return sink->PausedLineCount() >= 50; }),
            "the worker must drain the appended lines into the paused buffer"
        );
        QCOMPARE(model.rowCount(), 150);
        QCOMPARE(static_cast<int>(sink->PausedLineCount()), 50);

        // Resume coalesces the paused buffer into a single batch and posts
        // it once. The post-Resume row count is the cap-bounded sum.
        sink->Resume();
        QVERIFY(!sink->IsPaused());
        QVERIFY2(
            WaitForLineCount(model, 200, std::chrono::seconds(5)), "Resume must coalesce paused buffer within 5 s"
        );
        QCOMPARE(model.rowCount(), 200);
        QCOMPARE(static_cast<int>(sink->PausedLineCount()), 0);

        // Tear down cleanly. `Reset()` drives the teardown
        // sequence (Source::Stop -> sink::RequestStop -> watcher join ->
        // DropPendingBatches) so the test exits without dangling threads.
        model.Reset();
        QCoreApplication::processEvents();

        QVERIFY(!model.IsStreamingActive());
        QCOMPARE(model.rowCount(), 0);
    }

    // End-to-end smoke test for the UDP network-stream open path.
    // Mirrors `testStreamModeOpensTailFileAndAppends` but the byte
    // producer is a `UdpServerProducer` driven by a synchronous
    // `UdpLogClient` from `test_common`. We do not pop the
    // `NetworkStreamDialog` (modal -> would hang offscreen QPA);
    // instead we wire the producer directly into the model the same
    // way `MainWindow::OpenNetworkStream` does.
    static void TestStreamModeOpensUdpProducer()
    {
        loglib::UdpServerProducer::Options opts;
        opts.bindAddress = "127.0.0.1"; // ephemeral port
        auto producer = std::make_unique<loglib::UdpServerProducer>(opts);
        const uint16_t port = producer->BoundPort();
        QVERIFY(port != 0);
        const std::string display = producer->DisplayName();

        LogModel model;
        const QSignalSpy lineCountSpy(&model, &LogModel::lineCountChanged);
        QVERIFY(lineCountSpy.isValid());
        model.SetRetentionCap(1000);

        auto streamSource =
            std::make_unique<loglib::StreamLineSource>(std::filesystem::path(display), std::move(producer));
        const loglib::ParserOptions options;
        Q_UNUSED(model.BeginStreaming(std::move(streamSource), options));

        QVERIFY(model.IsStreamingActive());

        // Send 10 datagrams. Each lands as one line in the parser.
        test_common::UdpLogClient client("127.0.0.1", port);
        for (int i = 0; i < 10; ++i)
        {
            client.Send("{\"i\":" + std::to_string(i + 1) + R"(,"phase":"udp"})");
        }

        QVERIFY2(WaitForLineCount(model, 10, std::chrono::seconds(5)), "10 UDP lines must arrive within 5 s");
        QCOMPARE(model.rowCount(), 10);

        model.Reset();
        QCoreApplication::processEvents();
        QVERIFY(!model.IsStreamingActive());
    }

    // End-to-end smoke test for the TCP network-stream open path.
    // Single client, plaintext (TLS coverage lives in the lib-tree
    // `test_tcp_server_producer_tls.cpp` so the apptest does not need
    // to build OpenSSL into its link line when LOGLIB_NETWORK_TLS is
    // off on a developer build).
    static void TestStreamModeOpensTcpProducer()
    {
        loglib::TcpServerProducer::Options opts;
        opts.bindAddress = "127.0.0.1"; // ephemeral port
        auto producer = std::make_unique<loglib::TcpServerProducer>(opts);
        const uint16_t port = producer->BoundPort();
        QVERIFY(port != 0);
        const std::string display = producer->DisplayName();
        QVERIFY(display.starts_with("tcp://127.0.0.1:"));

        LogModel model;
        const QSignalSpy lineCountSpy(&model, &LogModel::lineCountChanged);
        QVERIFY(lineCountSpy.isValid());
        model.SetRetentionCap(1000);

        auto streamSource =
            std::make_unique<loglib::StreamLineSource>(std::filesystem::path(display), std::move(producer));
        const loglib::ParserOptions options;
        Q_UNUSED(model.BeginStreaming(std::move(streamSource), options));

        QVERIFY(model.IsStreamingActive());

        test_common::TcpLogClient client("127.0.0.1", port);
        for (int i = 0; i < 10; ++i)
        {
            client.Send("{\"i\":" + std::to_string(i + 1) + R"(,"phase":"tcp"})");
        }

        QVERIFY2(WaitForLineCount(model, 10, std::chrono::seconds(5)), "10 TCP lines must arrive within 5 s");
        QCOMPARE(model.rowCount(), 10);

        client.Close();
        model.Reset();
        QCoreApplication::processEvents();
        QVERIFY(!model.IsStreamingActive());
    }

    // End-to-end: a JSON `level` column with canonical names auto-
    // promotes through `Type::Enumeration` to `Type::Level`, and
    // `GetLevelForRow` then reports the canonical rank per row.
    // (Tests that use `category` instead are pinning the enum path.)
    void TestLevelColumnAutoPromotesFromCanonicalLevelKey()
    {
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

        const StreamingRun run = RunStreaming(fixture.Path());
        QCOMPARE(run.finishedCount, 1);
        QCOMPARE(run.cancelled, false);
        QVERIFY(run.model->StreamingErrors().empty());

        const int levelCol = ColumnByHeader(*run.model, QStringLiteral("level"));
        QVERIFY2(levelCol >= 0, "level column must exist");
        const auto &columns = run.model->Layout().Columns();
        QVERIFY(static_cast<size_t>(levelCol) < columns.size());
        // Every dict entry is canonical, so the tolerance trivially
        // holds and promotion must reach `Type::Level`.
        QCOMPARE(columns[static_cast<size_t>(levelCol)].type, loglib::ColumnType::Level);

        // Row-cycle pattern is `levels[i % 6]`; check the first few.
        const auto col = static_cast<size_t>(levelCol);
        QCOMPARE(run.model->Table().GetLevelForRow(0, col).value(), loglib::LogLevel::Info);
        QCOMPARE(run.model->Table().GetLevelForRow(1, col).value(), loglib::LogLevel::Warn);
        QCOMPARE(run.model->Table().GetLevelForRow(2, col).value(), loglib::LogLevel::Error);
        QCOMPARE(run.model->Table().GetLevelForRow(3, col).value(), loglib::LogLevel::Debug);
        QCOMPARE(run.model->Table().GetLevelForRow(4, col).value(), loglib::LogLevel::Trace);
        QCOMPARE(run.model->Table().GetLevelForRow(5, col).value(), loglib::LogLevel::Fatal);
    }

    // `OpenMode::Append` keeps the active static session's rows / filters /
    // source intact and queues the new file via `AppendStreaming`. Row
    // count after the second open must equal the sum of both fixtures
    // and `mCurrentSource->locators` must list both file paths in load
    // order.
    void TestOpenFilesAppendsToActiveStaticSession()
    {
        auto *model = mWindow->Model();
        QVERIFY(model != nullptr);

        const QStringList fixtureLinesA{
            QStringLiteral(R"({"category": "info", "msg": "a-0"})"),
            QStringLiteral(R"({"category": "warn", "msg": "a-1"})"),
        };
        const QStringList fixtureLinesB{
            QStringLiteral(R"({"category": "error", "msg": "b-0"})"),
            QStringLiteral(R"({"category": "debug", "msg": "b-1"})"),
            QStringLiteral(R"({"category": "info", "msg": "b-2"})"),
        };
        const TempJsonFile fixtureA(fixtureLinesA);
        const TempJsonFile fixtureB(fixtureLinesB);

        QSignalSpy finishedSpy(model, &LogModel::streamingFinished);
        QVERIFY(finishedSpy.isValid());

        mWindow->OpenFilesForTest({fixtureA.Path()}, MainWindow::OpenMode::Append);
        QVERIFY2(finishedSpy.wait(60000), "first open must finish");
        QCoreApplication::processEvents();
        QCOMPARE(model->rowCount(), fixtureLinesA.size());
        QVERIFY(mWindow->Model() == model);

        finishedSpy.clear();
        mWindow->OpenFilesForTest({fixtureB.Path()}, MainWindow::OpenMode::Append);
        QVERIFY2(finishedSpy.wait(60000), "appended open must finish");
        QCoreApplication::processEvents();

        QCOMPARE(model->rowCount(), fixtureLinesA.size() + fixtureLinesB.size());

        // Source descriptor lists every appended file in load order.
        const QTemporaryDir saved;
        QVERIFY(saved.isValid());
        const QString sessionPath = saved.filePath(QStringLiteral("appended.json"));
        mWindow->SaveConfigurationToPathForTest(sessionPath, loglib::SaveScope::Full);

        const loglib::LogConfiguration probe = loglib::LoadConfiguration(sessionPath.toStdString());
        QVERIFY(probe.source.has_value());
        QCOMPARE(probe.source->locators.size(), static_cast<std::size_t>(2));
        // `locators` stores case-preserved display paths; the
        // lower-cased dedup form is parallel-indexed under
        // `locatorDedupKeys`. Assert both stay populated.
        QCOMPARE(
            QString::fromStdString(probe.source->locators[0].displayPath), logapp::CanonicalDisplayPath(fixtureA.Path())
        );
        QCOMPARE(
            QString::fromStdString(probe.source->locators[1].displayPath), logapp::CanonicalDisplayPath(fixtureB.Path())
        );
        QCOMPARE(probe.source->locators.size(), static_cast<std::size_t>(2));
        QCOMPARE(QString::fromStdString(probe.source->locators[0].dedupKey), logapp::CanonicalLocator(fixtureA.Path()));
        QCOMPARE(QString::fromStdString(probe.source->locators[1].dedupKey), logapp::CanonicalLocator(fixtureB.Path()));
    }

    // Opening a primary prepends every sibling in oldest-first order.
    void TestOpenFilesExpandsRotationSiblings()
    {
        auto *model = mWindow->Model();
        QVERIFY(model != nullptr);

        const QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString primaryPath = dir.filePath(QStringLiteral("app.log"));
        const QString oldPath = dir.filePath(QStringLiteral("app.log.1"));
        const QString olderPath = dir.filePath(QStringLiteral("app.log.2"));
        const auto write = [](const QString &p, const QString &content) {
            std::ofstream stream(p.toStdString(), std::ios::binary);
            QVERIFY2(stream.is_open(), "fixture write must succeed");
            stream << content.toStdString();
        };
        write(
            primaryPath,
            R"({"msg":"primary-0"})"
            "\n"
            R"({"msg":"primary-1"})"
            "\n"
        );
        write(
            oldPath,
            R"({"msg":"gen1-0"})"
            "\n"
        );
        write(
            olderPath,
            R"({"msg":"gen2-0"})"
            "\n"
            R"({"msg":"gen2-1"})"
            "\n"
        );

        QSignalSpy finishedSpy(model, &LogModel::streamingFinished);
        QVERIFY(finishedSpy.isValid());

        // The mixed dispatcher is required to exercise expansion.
        (void)mWindow->OpenMixedFilesForTest({primaryPath}, MainWindow::OpenMode::Replace);
        while (finishedSpy.count() < 3)
        {
            QVERIFY2(finishedSpy.wait(60000), "three per-file streamingFinished emits expected");
        }
        QCoreApplication::processEvents();

        QCOMPARE(model->rowCount(), 5);
    }

    // A caller-listed rotation family is normalized to oldest-first order.
    void TestMultiFileDropSmartSortsRotationFamily()
    {
        auto *model = mWindow->Model();
        QVERIFY(model != nullptr);

        const QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString primaryPath = dir.filePath(QStringLiteral("app.log"));
        const QString oldPath = dir.filePath(QStringLiteral("app.log.1"));
        const QString olderPath = dir.filePath(QStringLiteral("app.log.2"));
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
            oldPath,
            R"({"msg":"g1"})"
            "\n"
        );
        write(
            olderPath,
            R"({"msg":"g2"})"
            "\n"
        );

        QSignalSpy finishedSpy(model, &LogModel::streamingFinished);
        QVERIFY(finishedSpy.isValid());

        // The mixed dispatcher is required to exercise expansion.
        (void)mWindow->OpenMixedFilesForTest({oldPath, primaryPath, olderPath}, MainWindow::OpenMode::Replace);
        while (finishedSpy.count() < 3)
        {
            QVERIFY2(finishedSpy.wait(60000), "three per-file streamingFinished emits expected");
        }
        QCoreApplication::processEvents();

        QCOMPARE(model->rowCount(), 3);

        const QTemporaryDir savedDir;
        QVERIFY(savedDir.isValid());
        const QString sessionPath = savedDir.filePath(QStringLiteral("rotation-sorted.json"));
        mWindow->SaveConfigurationToPathForTest(sessionPath, loglib::SaveScope::Full);

        const loglib::LogConfiguration probe = loglib::LoadConfiguration(sessionPath.toStdString());
        QVERIFY(probe.source.has_value());
        const auto &locators = probe.source->locators;
        QCOMPARE(locators.size(), static_cast<std::size_t>(3));
        QCOMPARE(QString::fromStdString(locators[0].displayPath), logapp::CanonicalDisplayPath(olderPath));
        QCOMPARE(QString::fromStdString(locators[1].displayPath), logapp::CanonicalDisplayPath(oldPath));
        QCOMPARE(QString::fromStdString(locators[2].displayPath), logapp::CanonicalDisplayPath(primaryPath));
    }

    // The sibling toast names the expanded family, not the last input.
    void TestSiblingToastAnchorsOnExpandedFamilyPrimary()
    {
        auto *model = mWindow->Model();
        QVERIFY(model != nullptr);

        const QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString appPrimary = dir.filePath(QStringLiteral("app.log"));
        const QString appOlder = dir.filePath(QStringLiteral("app.log.1"));
        // Keep an unrelated primary last to distinguish the toast anchor.
        const QString otherPrimary = dir.filePath(QStringLiteral("other.log"));
        const auto write = [](const QString &p, const QString &content) {
            std::ofstream stream(p.toStdString(), std::ios::binary);
            QVERIFY2(stream.is_open(), "fixture write must succeed");
            stream << content.toStdString();
        };
        write(
            appPrimary,
            R"({"msg":"a"})"
            "\n"
        );
        write(
            appOlder,
            R"({"msg":"a-old"})"
            "\n"
        );
        write(
            otherPrimary,
            R"({"msg":"o"})"
            "\n"
        );

        QSignalSpy finishedSpy(model, &LogModel::streamingFinished);
        QVERIFY(finishedSpy.isValid());

        mWindow->statusBar()->clearMessage();
        (void)mWindow->OpenMixedFilesForTest({appPrimary, otherPrimary}, MainWindow::OpenMode::Replace);
        while (finishedSpy.count() < 3)
        {
            QVERIFY2(finishedSpy.wait(60000), "three per-file streamingFinished emits expected");
        }
        QCoreApplication::processEvents();

        const QString message = mWindow->statusBar()->currentMessage();
        QVERIFY2(
            message.contains(QStringLiteral("app.log")),
            qPrintable(QStringLiteral("toast must name the expanded family; got: '%1'").arg(message))
        );
        QVERIFY2(
            !message.contains(QStringLiteral("other.log")),
            qPrintable(QStringLiteral("toast must not name the unrelated caller input; got: '%1'").arg(message))
        );
    }

    // Caller-listed family members do not arm rotation-expansion Undo.
    void TestExpanderDoesNotArmUndoWhenCallerListedEverySibling()
    {
        auto *model = mWindow->Model();
        QVERIFY(model != nullptr);

        const QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString primaryPath = dir.filePath(QStringLiteral("app.log"));
        const QString oldPath = dir.filePath(QStringLiteral("app.log.1"));
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
            oldPath,
            R"({"msg":"g1"})"
            "\n"
        );

        QSignalSpy finishedSpy(model, &LogModel::streamingFinished);
        QVERIFY(finishedSpy.isValid());

        mWindow->statusBar()->clearMessage();
        (void)mWindow->OpenMixedFilesForTest({oldPath, primaryPath}, MainWindow::OpenMode::Replace);
        while (finishedSpy.count() < 2)
        {
            QVERIFY2(finishedSpy.wait(60000), "two per-file streamingFinished emits expected");
        }
        QCoreApplication::processEvents();

        QCOMPARE(model->rowCount(), 2);
        QVERIFY2(
            !mWindow->IsUndoRotationExpansionEnabledForTest(),
            "Undo must not arm when every 'sibling' was in the caller's own selection"
        );
        const QString message = mWindow->statusBar()->currentMessage();
        QVERIFY2(
            !message.contains(QStringLiteral("rotated companion")),
            qPrintable(QStringLiteral("no sibling toast expected; got: '%1'").arg(message))
        );
    }

    // The launch override suppresses sibling expansion.
    void TestRotationSiblingsOptOutViaLaunchOverride()
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

        mWindow->SetRotationHistoryLaunchOverride(true);

        QSignalSpy finishedSpy(model, &LogModel::streamingFinished);
        QVERIFY(finishedSpy.isValid());

        (void)mWindow->OpenMixedFilesForTest({primaryPath}, MainWindow::OpenMode::Replace);
        QVERIFY2(finishedSpy.wait(60000), "single-file open must finish");
        QCoreApplication::processEvents();

        QCOMPARE(model->rowCount(), 1);
        mWindow->SetRotationHistoryLaunchOverride(false);
    }

    // Re-enabling rotation history clears the launch override.
    void TestRotationHistoryMenuToggleClearsCliOverride()
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

        mWindow->SetRotationHistoryLaunchOverride(true);
        QVERIFY(mWindow->RotationHistoryLaunchOverrideForTest());

        mWindow->SimulateRotationHistoryMenuToggleForTest(true);
        QVERIFY2(
            !mWindow->RotationHistoryLaunchOverrideForTest(), "menu toggle must clear the launch-time CLI override"
        );

        QSignalSpy finishedSpy(model, &LogModel::streamingFinished);
        QVERIFY(finishedSpy.isValid());

        (void)mWindow->OpenMixedFilesForTest({primaryPath}, MainWindow::OpenMode::Replace);
        for (int i = 0; i < 2; ++i)
        {
            QVERIFY2(finishedSpy.wait(60000), "two per-file streamingFinished emits expected");
        }
        QCoreApplication::processEvents();
        QCOMPARE(model->rowCount(), 2);
    }

    // Menu changes mirror to the current source in both directions.
    void TestRotationHistoryMenuTogglePersistsOntoCurrentSource()
    {
        auto *model = mWindow->Model();
        QVERIFY(model != nullptr);

        const QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString primaryPath = dir.filePath(QStringLiteral("app.log"));
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

        QSignalSpy finishedSpy(model, &LogModel::streamingFinished);
        QVERIFY(finishedSpy.isValid());
        (void)mWindow->OpenMixedFilesForTest({primaryPath}, MainWindow::OpenMode::Replace);
        QVERIFY2(finishedSpy.wait(60000), "single-file open must finish");
        QCoreApplication::processEvents();

        {
            const auto &source = mWindow->CurrentSourceForTest();
            QVERIFY(source.has_value());
            QVERIFY(source->followRotationSiblings);
        }

        mWindow->SimulateRotationHistoryMenuToggleForTest(false);
        {
            const auto &source = mWindow->CurrentSourceForTest();
            QVERIFY(source.has_value());
            QVERIFY2(!source->followRotationSiblings, "menu toggle OFF must mirror onto the session source");
        }

        mWindow->SimulateRotationHistoryMenuToggleForTest(true);
        {
            const auto &source = mWindow->CurrentSourceForTest();
            QVERIFY(source.has_value());
            QVERIFY2(source->followRotationSiblings, "menu toggle ON must re-enable the session-scope flag");
        }
    }

    // Windows and macOS match sibling names case-insensitively.
    // Keep the slot declared on Linux so moc still discovers it.
    void TestRotationSiblingsAreCaseInsensitiveOnWindowsAndMac()
    {
#if defined(_WIN32) || defined(__APPLE__)
        auto *model = mWindow->Model();
        QVERIFY(model != nullptr);

        const QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString primaryPath = dir.filePath(QStringLiteral("app.log"));
        const QString upperPath = dir.filePath(QStringLiteral("App.LOG.1"));
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
            upperPath,
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
        QCOMPARE(model->rowCount(), 2);
#else
        QSKIP("Case-insensitive sibling matching is Windows/macOS only.");
#endif
    }

    // A new file stream clears stale sibling-prefix promotion state.
    void TestOpenLogStreamFromPathClearsStalePendingLiveTail()
    {
        auto *model = mWindow->Model();
        QVERIFY(model != nullptr);

        const QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString fakeStalePrimary = dir.filePath(QStringLiteral("stale-primary.log"));
        const QString freshLog = dir.filePath(QStringLiteral("fresh.log"));
        const auto write = [](const QString &p, const QString &content) {
            std::ofstream stream(p.toStdString(), std::ios::binary);
            QVERIFY2(stream.is_open(), "fixture write must succeed");
            stream << content.toStdString();
        };
        write(
            freshLog,
            R"({"msg":"fresh"})"
            "\n"
        );

        // Seed the unresolved state that a cancelled sibling drain can leave.
        mWindow->SeedPendingLiveTailForTest(fakeStalePrimary, /*retention=*/128);
        QVERIFY(mWindow->HasPendingLiveTailForTest());

        QSignalSpy finishedSpy(model, &LogModel::streamingFinished);
        QVERIFY(finishedSpy.isValid());
        mWindow->OpenLogStreamForTest(freshLog);

        QVERIFY2(!mWindow->HasPendingLiveTailForTest(), "destructive open must scrub stale pending live-tail state");

        // A live tail never finishes by itself; leave no worker for the next slot.
        model->StopAndKeepRows();
        QCoreApplication::processEvents();
    }

    // Replace reopens ignore the outgoing session's deduplication keys.
    void TestReplaceReopenOfSameRotationFamilyStreamsAllFiles()
    {
        auto *model = mWindow->Model();
        QVERIFY(model != nullptr);

        const QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString primaryPath = dir.filePath(QStringLiteral("app.log"));
        const QString oldPath = dir.filePath(QStringLiteral("app.log.1"));
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
            oldPath,
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
        QCOMPARE(model->rowCount(), 2);
        {
            const auto &source = mWindow->CurrentSourceForTest();
            QVERIFY(source.has_value());
            QCOMPARE(source->locators.size(), static_cast<std::size_t>(2));
        }

        finishedSpy.clear();
        (void)mWindow->OpenMixedFilesForTest({primaryPath}, MainWindow::OpenMode::Replace);
        for (int i = 0; i < 2; ++i)
        {
            QVERIFY2(finishedSpy.wait(60000), "second Replace must stream both files again");
        }
        QCoreApplication::processEvents();
        QCOMPARE(model->rowCount(), 2);
    }

    // Replace opens honor the current session's effective opt-out.
    void TestReplaceOpenHonoursSessionOptOutEffectivePreference()
    {
        auto *model = mWindow->Model();
        QVERIFY(model != nullptr);

        const QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString primaryPath = dir.filePath(QStringLiteral("app.log"));
        const QString oldPath = dir.filePath(QStringLiteral("app.log.1"));
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
            oldPath,
            R"({"msg":"g1"})"
            "\n"
        );

        // Use an unrelated source so deduplication cannot mask the opt-out.
        loglib::Source spoof;
        spoof.kind = loglib::Source::Kind::File;
        spoof.format = loglib::Source::Format::Json;
        spoof.locators = {dir.filePath(QStringLiteral("unrelated.log")).toStdString()};
        spoof.followRotationSiblings = false;
        mWindow->SetCurrentSourceForTest(spoof);

        QSignalSpy finishedSpy(model, &LogModel::streamingFinished);
        QVERIFY(finishedSpy.isValid());

        (void)mWindow->OpenMixedFilesForTest({primaryPath}, MainWindow::OpenMode::Replace);
        QVERIFY2(finishedSpy.wait(60000), "single-file open must finish");
        QCoreApplication::processEvents();
        QCOMPARE(model->rowCount(), 1);
        QVERIFY2(
            !mWindow->IsUndoRotationExpansionEnabledForTest(),
            "session opt-out must suppress the Undo affordance the same way an unchecked box does"
        );
    }

    // Stopping a sibling-prefix drain still promotes the requested live tail.
    void TestSiblingPrefixLiveTailSurvivesStopMidDrain()
    {
        auto *model = mWindow->Model();
        QVERIFY(model != nullptr);

        const QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString primaryPath = dir.filePath(QStringLiteral("app.log"));
        const QString oldPath = dir.filePath(QStringLiteral("app.log.1"));
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
        // Keep the sibling drain active until StopAndKeepRows runs.
        QString bigContent;
        bigContent.reserve(200000 * 20);
        for (int i = 0; i < 200000; ++i)
        {
            bigContent += QStringLiteral(
                              R"({"msg":"g%1"})"
                              "\n"
            )
                              .arg(i);
        }
        write(oldPath, bigContent);

        const QSignalSpy finishedSpy(model, &LogModel::streamingFinished);
        QVERIFY(finishedSpy.isValid());

        mWindow->OpenLogStreamForTest(primaryPath);

        QTRY_VERIFY_WITH_TIMEOUT(model->IsStreamingActive(), 60000);
        QCOMPARE(mWindow->SessionModeForTest(), MainWindow::TestSessionMode::Static);
        QVERIFY(mWindow->HasPendingLiveTailForTest());

        model->StopAndKeepRows();
        QCoreApplication::processEvents();

        QCOMPARE(mWindow->SessionModeForTest(), MainWindow::TestSessionMode::LiveTail);
        QVERIFY2(!mWindow->HasPendingLiveTailForTest(), "pending live-tail slots must be consumed by the promotion");
        const auto &source = mWindow->CurrentSourceForTest();
        QVERIFY(source.has_value());
        const std::string primaryKey = logapp::CanonicalLocator(primaryPath).toStdString();
        const bool sawPrimary = loglib::ContainsDedupKey(*source, primaryKey);
        QVERIFY2(sawPrimary, "promoted live tail must append primary to the source locators");

        mWindow->NewSessionForTest();
        QCoreApplication::processEvents();
    }

    // A queued sibling Success must promote only after Stop teardown unwinds.
    // Synchronous promotion is reentrant and lets teardown clobber the new tail.
    void TestSiblingPrefixLiveTailSurvivesStopAfterPreQueuedSuccess()
    {
        auto *model = mWindow->Model();
        QVERIFY(model != nullptr);

        const QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString primaryPath = dir.filePath(QStringLiteral("app.log"));
        const QString oldPath = dir.filePath(QStringLiteral("app.log.1"));
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
        // Keep parsing short enough to queue Success before Stop.
        write(
            oldPath,
            R"({"msg":"g1"})"
            "\n"
            R"({"msg":"g2"})"
            "\n"
            R"({"msg":"g3"})"
            "\n"
        );

        const QSignalSpy finishedSpy(model, &LogModel::streamingFinished);
        QVERIFY(finishedSpy.isValid());

        mWindow->OpenLogStreamForTest(primaryPath);

        QTRY_VERIFY_WITH_TIMEOUT(model->IsStreamingActive(), 60000);
        QCOMPARE(mWindow->SessionModeForTest(), MainWindow::TestSessionMode::Static);
        QVERIFY(mWindow->HasPendingLiveTailForTest());

        // Do not pump GUI events; worker completion must remain queued for Stop. The worker's completion is
        // only visible through the event queue this test must leave alone, so time is the only signal here. The
        // test passes whether or not the worker has finished, and the pause only lets it reach the queued state.
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        model->StopAndKeepRows();
        QCoreApplication::processEvents();

        QCOMPARE(mWindow->SessionModeForTest(), MainWindow::TestSessionMode::LiveTail);
        QVERIFY2(
            !mWindow->HasPendingLiveTailForTest(), "pending live-tail slots must be consumed by the deferred promotion"
        );
        QVERIFY2(
            model->IsStreamingActive(),
            "live-tail worker must remain active after teardown; a pre-fix reentrancy "
            "would have clobbered mStreamingActive via a spurious streamingFinished(Cancelled)"
        );

        const auto &source = mWindow->CurrentSourceForTest();
        QVERIFY(source.has_value());
        const std::string primaryKey = logapp::CanonicalLocator(primaryPath).toStdString();
        const bool sawPrimary = loglib::ContainsDedupKey(*source, primaryKey);
        QVERIFY2(sawPrimary, "promoted live tail must append primary to the source locators");

        mWindow->NewSessionForTest();
        QCoreApplication::processEvents();
    }

    // Rotation-expansion Undo restores every caller-listed path.
    void TestUndoRotationExpansionResurrectsMultiFileSelection()
    {
        auto *model = mWindow->Model();
        QVERIFY(model != nullptr);

        const QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString primaryPath = dir.filePath(QStringLiteral("app.log"));
        const QString oldPath = dir.filePath(QStringLiteral("app.log.1"));
        const QString otherPath = dir.filePath(QStringLiteral("other.log"));
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
            oldPath,
            R"({"msg":"g1"})"
            "\n"
        );
        write(
            otherPath,
            R"({"msg":"o"})"
            "\n"
        );

        QSignalSpy finishedSpy(model, &LogModel::streamingFinished);
        QVERIFY(finishedSpy.isValid());

        (void)mWindow->OpenMixedFilesForTest({primaryPath, otherPath}, MainWindow::OpenMode::Replace);
        for (int i = 0; i < 3; ++i)
        {
            QVERIFY2(finishedSpy.wait(60000), "three per-file streamingFinished emits expected");
        }
        QCoreApplication::processEvents();
        QCOMPARE(model->rowCount(), 3);

        const QStringList captured = mWindow->LastRotationExpansionOriginalInputsForTest();
        QCOMPARE(captured.size(), 2);
        QCOMPARE(captured[0], primaryPath);
        QCOMPARE(captured[1], otherPath);
        QVERIFY(mWindow->IsUndoRotationExpansionEnabledForTest());

        finishedSpy.clear();
        mWindow->UndoRotationExpansionForTest();
        for (int i = 0; i < 2; ++i)
        {
            QVERIFY2(finishedSpy.wait(60000), "two per-file streamingFinished emits expected on undo");
        }
        QCoreApplication::processEvents();
        QCOMPARE(model->rowCount(), 2);

        const auto &source = mWindow->CurrentSourceForTest();
        QVERIFY(source.has_value());
        const std::string primaryKey = logapp::CanonicalLocator(primaryPath).toStdString();
        const std::string otherKey = logapp::CanonicalLocator(otherPath).toStdString();
        const std::string siblingKey = logapp::CanonicalLocator(oldPath).toStdString();
        const auto hasKey = [&source](const std::string &k) { return loglib::ContainsDedupKey(*source, k); };
        QVERIFY2(hasKey(primaryKey), "undo must resurrect the app.log primary");
        QVERIFY2(hasKey(otherKey), "undo must resurrect the other.log primary");
        QVERIFY2(!hasKey(siblingKey), "undo must not re-attach the sibling this time");

        QVERIFY(mWindow->LastRotationExpansionOriginalInputsForTest().isEmpty());
        QVERIFY(!mWindow->IsUndoRotationExpansionEnabledForTest());
    }

    // Rescue-path live-tail promotion disables configuration UI first.
    void TestRescueBranchLiveTailDisablesConfigurationUi()
    {
        auto *tableView = mWindow->findChild<LogTableView *>();
        QVERIFY(tableView != nullptr);
        const QHeaderView *header = tableView->horizontalHeader();
        QVERIFY(header != nullptr);

        const QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString primaryPath = dir.filePath(QStringLiteral("app.log"));
        {
            std::ofstream stream(primaryPath.toStdString(), std::ios::binary);
            QVERIFY2(stream.is_open(), "fixture write must succeed");
            stream << R"({"msg":"p"})"
                      "\n";
        }

        // Model the rescue path where no sibling disabled the UI.
        mWindow->SetConfigurationUiEnabledForTest(true);
        QVERIFY2(header->sectionsMovable(), "precondition: header drag must be armed before the rescue");
        QCOMPARE(header->contextMenuPolicy(), Qt::CustomContextMenu);

        // Trigger the otherwise timing-dependent rescue branch directly.
        mWindow->TriggerRescueLiveTailForTest(primaryPath, /*retention=*/0);
        QCoreApplication::processEvents();

        QVERIFY2(
            !header->sectionsMovable(), "rescue-branch live tail must disable header drag before arming the parser"
        );
        QCOMPARE(header->contextMenuPolicy(), Qt::NoContextMenu);

        mWindow->NewSessionForTest();
        QCoreApplication::processEvents();
    }

    // Loading a configuration clears rotation Undo and pending promotion state.
    void TestApplyLoadedConfigurationClearsRotationExpansionState()
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
        QCOMPARE(mWindow->LastRotationExpansionOriginalInputsForTest().size(), 1);

        // Seed the second class of outgoing state cleared by a load.
        mWindow->SeedPendingLiveTailForTest(primaryPath, /*retention=*/64);
        QVERIFY(mWindow->HasPendingLiveTailForTest());

        const QTemporaryDir savedDir;
        QVERIFY(savedDir.isValid());
        const QString sessionPath = savedDir.filePath(QStringLiteral("saved.json"));
        mWindow->SaveConfigurationToPathForTest(sessionPath, loglib::SaveScope::Full);

        mWindow->LoadConfigurationFromPathForTest(sessionPath);
        QCoreApplication::processEvents();

        QVERIFY2(
            !mWindow->IsUndoRotationExpansionEnabledForTest(),
            "loading a config must disable Undo Rotated History Expansion"
        );
        QVERIFY2(
            mWindow->LastRotationExpansionOriginalInputsForTest().isEmpty(),
            "loading a config must clear the stored original-inputs list"
        );
        QVERIFY2(
            !mWindow->HasPendingLiveTailForTest(),
            "loading a config must scrub any half-drained sibling-prefix live-tail state"
        );
    }

    // App and library canonical keys agree for case and Unicode.
    // Canonicalize the root first to avoid macOS `/tmp` symlink differences.
    void TestCanonicalKeyAndCanonicalLocatorAgree()
    {
        const QTemporaryDir dir;
        QVERIFY(dir.isValid());
        std::error_code ec;
        // Preserve Unicode temporary roots on Windows.
        const std::filesystem::path rootFs = std::filesystem::canonical(logapp::QStringToFsPath(dir.path()), ec);
        QVERIFY2(!ec, "canonical(root) must succeed on a valid QTemporaryDir");
        // Avoid active-code-page narrowing on Windows.
#ifdef Q_OS_WIN
        const QString root = QString::fromStdWString(rootFs.generic_wstring());
#else
        const QString root = QString::fromStdString(rootFs.generic_string());
#endif

        // Cover mixed case and non-ASCII path components.
        const QStringList relatives{
            QStringLiteral("App.log"),
            QStringLiteral("dir/Nested/File.LOG"),
            QStringLiteral("subdir/app.log.1"),
            QStringLiteral("app-2025-08-01.log.gz"),
            QStringLiteral(u"\u65e5\u5fd7/app.log"), // 日志/app.log
        };
        for (const QString &rel : relatives)
        {
            const QString p = root + QLatin1Char('/') + rel;
            const std::string appKey = logapp::CanonicalLocator(p).toStdString();
            const std::string libKey = loglib::CanonicalKeyForPath(logapp::QStringToFsPath(p));
            QVERIFY2(
                appKey == libKey,
                qPrintable(QStringLiteral(
                               "CanonicalLocator vs CanonicalKeyForPath mismatch for %1:\n"
                               "  app: %2\n  lib: %3"
                )
                               .arg(p)
                               .arg(QString::fromStdString(appKey))
                               .arg(QString::fromStdString(libKey)))
            );
        }
    }

    // Explicitly selected siblings are not counted as auto-added.
    void TestExplicitlySelectedSiblingsAreNotCountedAsAutoAdded()
    {
        auto *model = mWindow->Model();
        QVERIFY(model != nullptr);

        const QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString primaryPath = dir.filePath(QStringLiteral("app.log"));
        const QString oldPath = dir.filePath(QStringLiteral("app.log.1"));
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
            oldPath,
            R"({"msg":"g1"})"
            "\n"
        );

        QSignalSpy finishedSpy(model, &LogModel::streamingFinished);
        QVERIFY(finishedSpy.isValid());

        (void)mWindow->OpenMixedFilesForTest({primaryPath, oldPath}, MainWindow::OpenMode::Replace);
        for (int i = 0; i < 2; ++i)
        {
            QVERIFY2(finishedSpy.wait(60000), "both hand-picked files must stream to completion");
        }
        QCoreApplication::processEvents();
        QCOMPARE(model->rowCount(), 2);

        QVERIFY2(
            !mWindow->IsUndoRotationExpansionEnabledForTest(),
            "Undo action must stay disabled when nothing was auto-discovered"
        );
        QVERIFY2(
            mWindow->LastRotationExpansionOriginalInputsForTest().isEmpty(),
            "originals list must stay empty when nothing was auto-discovered"
        );
    }

    // Sibling expansion round-trips non-ASCII paths without narrowing.
    void TestNonAsciiSiblingPathRoundTripsThroughExpansion()
    {
        auto *model = mWindow->Model();
        QVERIFY(model != nullptr);

        const QTemporaryDir dir;
        QVERIFY(dir.isValid());
        // Exercise non-ASCII parent and basename components.
        const QString subdir = dir.filePath(QStringLiteral(u"\u65e5\u5fd7")); // 日志
        std::error_code createEc;
        // Avoid a filesystem exception aborting later QtTest slots.
        std::filesystem::create_directory(logapp::QStringToFsPath(subdir), createEc);
        if (createEc)
        {
            QSKIP("could not create non-ASCII subdirectory under QTemporaryDir");
        }
        const QString primaryPath =
            subdir + QLatin1Char('/') + QStringLiteral(u"\u041f\u0440\u0438\u043c\u0435\u0440.log"); // Пример.log
        const QString oldPath = primaryPath + QStringLiteral(".1");
        const auto write = [](const QString &p, const QString &content) -> bool {
            // The filesystem-path overload preserves wide paths on Windows.
            std::ofstream stream(logapp::QStringToFsPath(p), std::ios::binary);
            if (!stream.is_open())
            {
                return false;
            }
            stream << content.toStdString();
            return static_cast<bool>(stream);
        };
        if (!write(
                primaryPath,
                R"({"msg":"p"})"
                "\n"
            ) ||
            !write(
                oldPath,
                R"({"msg":"g1"})"
                "\n"
            ))
        {
            QSKIP("could not write non-ASCII log fixtures under QTemporaryDir");
        }

        QSignalSpy finishedSpy(model, &LogModel::streamingFinished);
        QVERIFY(finishedSpy.isValid());

        // Static Replace exercises both expansion and queue reopening.
        (void)mWindow->OpenMixedFilesForTest({primaryPath}, MainWindow::OpenMode::Replace);
        for (int i = 0; i < 2; ++i)
        {
            QVERIFY2(
                finishedSpy.wait(60000),
                "primary + auto-detected sibling must both stream (round-trip must not mangle non-ASCII)"
            );
        }
        QCoreApplication::processEvents();
        QCOMPARE(model->rowCount(), 2);
        QVERIFY2(
            mWindow->IsUndoRotationExpansionEnabledForTest(),
            "the sibling really was auto-discovered -- Undo affordance must arm"
        );
    }

    // Config-then-log opens ignore locator keys loaded from the config.
    void TestConfigThenLogsIgnoresStaleLocatorDedup()
    {
        auto *model = mWindow->Model();
        QVERIFY(model != nullptr);

        const QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString primaryPath = dir.filePath(QStringLiteral("app.log"));
        const auto write = [](const QString &p, const QString &content) {
            std::ofstream stream(p.toStdString(), std::ios::binary);
            QVERIFY2(stream.is_open(), "fixture write must succeed");
            stream << content.toStdString();
        };
        write(
            primaryPath,
            R"({"msg":"row-A"})"
            "\n"
        );

        QSignalSpy finishedSpy(model, &LogModel::streamingFinished);
        QVERIFY(finishedSpy.isValid());
        (void)mWindow->OpenMixedFilesForTest({primaryPath}, MainWindow::OpenMode::Replace);
        QVERIFY2(finishedSpy.wait(60000), "primary must stream to completion");
        QCoreApplication::processEvents();
        QCOMPARE(model->rowCount(), 1);

        const QTemporaryDir savedDir;
        QVERIFY(savedDir.isValid());
        const QString sessionPath = savedDir.filePath(QStringLiteral("saved.json"));
        mWindow->SaveConfigurationToPathForTest(sessionPath, loglib::SaveScope::Full);

        mWindow->NewSessionForTest();
        QCoreApplication::processEvents();
        QCOMPARE(model->rowCount(), 0);
        QVERIFY(!mWindow->CurrentSourceForTest().has_value());

        finishedSpy.clear();
        const MainWindow::MixedInputDispatch outcome =
            mWindow->OpenMixedFilesForTest({sessionPath, primaryPath}, MainWindow::OpenMode::Replace);
        QCOMPARE(outcome, MainWindow::MixedInputDispatch::AppliedConfigThenLogs);
        QVERIFY2(finishedSpy.wait(60000), "post-config log must stream to completion");
        QCoreApplication::processEvents();

        QVERIFY2(
            model->rowCount() >= 1,
            qPrintable(QStringLiteral(
                           "post-config `app.log` open produced %1 rows -- "
                           "stale locatorDedupKeys must not filter fresh selections"
            )
                           .arg(model->rowCount()))
        );
    }

    // Opening stdin clears pending file-tail promotion and rotation Undo.
    void TestOpenStdinStreamClearsPendingLiveTailPromotion()
    {
        auto *model = mWindow->Model();
        QVERIFY(model != nullptr);

        const QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString stalePrimary = dir.filePath(QStringLiteral("stale.log"));
        // Seed both outgoing bookkeeping surfaces.
        mWindow->SeedPendingLiveTailForTest(stalePrimary, /*retention=*/64);
        mWindow->SeedLastRotationExpansionForTest(
            /*originalInputs=*/QStringList{stalePrimary}, /*wasLiveTail=*/true
        );
        QVERIFY(mWindow->HasPendingLiveTailForTest());
        QVERIFY(!mWindow->LastRotationExpansionOriginalInputsForTest().isEmpty());

        // The stub reaches terminal EOF after one JSON line.
        class StubProducer final : public loglib::BytesProducer
        {
        public:
            explicit StubProducer(std::string bytes)
                : mBytes(std::move(bytes))
            {
            }
            size_t Read(std::span<char> buffer) override
            {
                if (mCursor >= mBytes.size())
                {
                    mClosed = true;
                    return 0;
                }
                const size_t available = mBytes.size() - mCursor;
                const size_t n = std::min(available, buffer.size());
                std::memcpy(buffer.data(), mBytes.data() + mCursor, n);
                mCursor += n;
                if (mCursor >= mBytes.size())
                {
                    mClosed = true;
                }
                return n;
            }
            void WaitForBytes(std::chrono::milliseconds /*timeout*/) override
            {
            }
            void Stop() noexcept override
            {
                mClosed = true;
            }
            [[nodiscard]] bool IsClosed() const noexcept override
            {
                return mClosed;
            }
            [[nodiscard]] std::string DisplayName() const override
            {
                return "test-stdin";
            }

        private:
            std::string mBytes;
            size_t mCursor = 0;
            bool mClosed = false;
        };

        QSignalSpy finishedSpy(model, &LogModel::streamingFinished);
        QVERIFY(finishedSpy.isValid());

        const std::string peek(
            R"({"msg":"stdin"})"
            "\n"
        );
        auto producer = std::make_unique<StubProducer>(peek);
        mWindow->OpenStdinStreamForTest(std::move(producer), peek);

        QVERIFY2(
            !mWindow->HasPendingLiveTailForTest(),
            "OpenStdinStreamFromProducer must scrub stale pending live-tail state"
        );
        QVERIFY2(
            mWindow->LastRotationExpansionOriginalInputsForTest().isEmpty(),
            "OpenStdinStreamFromProducer must scrub outgoing Undo-rotation affordance"
        );

        // A live tail never finishes by itself; leave no worker for the next slot.
        model->StopAndKeepRows();
        QCoreApplication::processEvents();
    }

    // Undo from Stream Mode reopens as a live tail, not a static view.
    void TestUndoRotationExpansionOnLiveTailStaysLiveTail()
    {
        auto *model = mWindow->Model();
        QVERIFY(model != nullptr);

        const QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString primaryPath = dir.filePath(QStringLiteral("app.log"));
        const QString oldPath = dir.filePath(QStringLiteral("app.log.1"));
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
            oldPath,
            R"({"msg":"g1"})"
            "\n"
        );

        QSignalSpy finishedSpy(model, &LogModel::streamingFinished);
        QVERIFY(finishedSpy.isValid());

        mWindow->OpenLogStreamForTest(primaryPath);
        QVERIFY2(finishedSpy.wait(60000), "sibling drain must finish before the tail attaches");
        QCoreApplication::processEvents();
        QCOMPARE(mWindow->SessionModeForTest(), MainWindow::TestSessionMode::LiveTail);
        QVERIFY(mWindow->IsUndoRotationExpansionEnabledForTest());

        finishedSpy.clear();
        mWindow->UndoRotationExpansionForTest();
        QCoreApplication::processEvents();

        QCOMPARE(mWindow->SessionModeForTest(), MainWindow::TestSessionMode::LiveTail);
        QVERIFY(!mWindow->IsUndoRotationExpansionEnabledForTest());
        QVERIFY(!mWindow->HasPendingLiveTailForTest());

        mWindow->NewSessionForTest();
        QCoreApplication::processEvents();
    }

    // Stream Mode derotates a picked segment to the active primary.
    void TestStreamModeDerotatesRotatedSegmentToActivePrimary()
    {
        auto *model = mWindow->Model();
        QVERIFY(model != nullptr);

        const QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString primaryPath = dir.filePath(QStringLiteral("app.log"));
        const QString oldPath = dir.filePath(QStringLiteral("app.log.1"));
        const QString olderPath = dir.filePath(QStringLiteral("app.log.2"));
        const auto write = [](const QString &p, const QString &content) {
            std::ofstream stream(p.toStdString(), std::ios::binary);
            QVERIFY2(stream.is_open(), "fixture write must succeed");
            stream << content.toStdString();
        };
        write(
            primaryPath,
            R"({"msg":"primary"})"
            "\n"
        );
        write(
            oldPath,
            R"({"msg":"gen1"})"
            "\n"
        );
        write(
            olderPath,
            R"({"msg":"gen2"})"
            "\n"
        );

        QSignalSpy finishedSpy(model, &LogModel::streamingFinished);
        QVERIFY(finishedSpy.isValid());

        mWindow->OpenLogStreamForTest(olderPath);
        // Both prefix completions precede the queued live-tail promotion.
        for (int i = 0; i < 2; ++i)
        {
            QVERIFY2(finishedSpy.wait(60000), "two sibling-prefix streamingFinished emits expected");
        }
        QCoreApplication::processEvents();

        QCOMPARE(mWindow->SessionModeForTest(), MainWindow::TestSessionMode::LiveTail);
        QCOMPARE(mWindow->StreamingFileNameForTest(), QStringLiteral("app.log"));

        const auto &source = mWindow->CurrentSourceForTest();
        QVERIFY(source.has_value());
        const std::string primaryKey = logapp::CanonicalLocator(primaryPath).toStdString();
        const std::string olderKey = logapp::CanonicalLocator(olderPath).toStdString();
        const std::string oldKey = logapp::CanonicalLocator(oldPath).toStdString();
        const auto hasKey = [&source](const std::string &key) { return loglib::ContainsDedupKey(*source, key); };
        QVERIFY2(hasKey(primaryKey), "derotated live-tail must include the active primary");
        QVERIFY2(hasKey(olderKey), "picked segment must still appear in the historical prefix");
        QVERIFY2(hasKey(oldKey), "intermediate numbered sibling must be loaded");
        QCOMPARE(source->locators.size(), static_cast<std::size_t>(3));
        QCOMPARE(
            QString::fromStdString(source->locators.back().displayPath), logapp::CanonicalDisplayPath(primaryPath)
        );

        mWindow->NewSessionForTest();
        QCoreApplication::processEvents();
    }

    // Stream Mode honors the bound session's effective opt-out.
    void TestStreamModeHonoursSessionOptOutEffectivePreference()
    {
        auto *model = mWindow->Model();
        QVERIFY(model != nullptr);

        const QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString primaryPath = dir.filePath(QStringLiteral("app.log"));
        const QString oldPath = dir.filePath(QStringLiteral("app.log.1"));
        const QString otherPath = dir.filePath(QStringLiteral("other.log"));
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
            oldPath,
            R"({"msg":"g1"})"
            "\n"
        );
        write(
            otherPath,
            R"({"msg":"o"})"
            "\n"
        );

        // Keep the global preference on while the bound source opts out.
        mWindow->SimulateRotationHistoryMenuToggleForTest(true);
        loglib::Source optedOut{
            .kind = loglib::Source::Kind::File,
            .format = loglib::Source::Format::Json,
            .locators =
                {{logapp::CanonicalDisplayPath(otherPath).toStdString(),
                  logapp::CanonicalLocator(otherPath).toStdString()}},
            .followRotationSiblings = false,
        };
        mWindow->SetCurrentSourceForTest(optedOut);

        mWindow->OpenLogStreamForTest(primaryPath);
        QCoreApplication::processEvents();
        QCOMPARE(mWindow->SessionModeForTest(), MainWindow::TestSessionMode::LiveTail);
        QVERIFY2(
            !mWindow->IsUndoRotationExpansionEnabledForTest(),
            "session opt-out must suppress Stream Mode sibling expansion"
        );

        const auto &source = mWindow->CurrentSourceForTest();
        QVERIFY(source.has_value());
        QCOMPARE(source->locators.size(), static_cast<std::size_t>(1));
        QCOMPARE(QString::fromStdString(source->locators.front().dedupKey), logapp::CanonicalLocator(primaryPath));

        mWindow->NewSessionForTest();
        QCoreApplication::processEvents();
    }

    // Regression: a multi-file open must sniff each queued file
    // independently, not reuse the first file's format for the rest.
    // Before the fix, a JSON Lines + logfmt queue tried to parse the
    // logfmt file with `JsonParser`, turning every line into a parse
    // error. Detection is content-based, so the extension is irrelevant.
    void TestMultiFileOpenSniffsFormatPerFile()
    {
        auto *model = mWindow->Model();
        QVERIFY(model != nullptr);

        const QStringList jsonLines{
            QStringLiteral(R"({"level": "info", "msg": "json-0"})"),
            QStringLiteral(R"({"level": "warn", "msg": "json-1"})"),
        };
        // logfmt bytes written through the JSON temp-file helper.
        // Detection is content-based, so the `.json` suffix is
        // exactly what we want the test to defeat.
        const QStringList logfmtLines{
            QStringLiteral(R"(level=info msg="logfmt-0")"),
            QStringLiteral(R"(level=warn msg="logfmt-1")"),
            QStringLiteral(R"(level=error msg="logfmt-2")"),
        };
        const TempJsonFile jsonFixture(jsonLines);
        const TempJsonFile logfmtFixture(logfmtLines);

        QSignalSpy finishedSpy(model, &LogModel::streamingFinished);
        QVERIFY(finishedSpy.isValid());

        // One open, two queued files: first uses `BeginStreaming`,
        // second uses `AppendStreaming` after the first finishes
        // -> two `streamingFinished` emits.
        mWindow->OpenFilesForTest({jsonFixture.Path(), logfmtFixture.Path()}, MainWindow::OpenMode::Replace);
        while (finishedSpy.count() < 2)
        {
            QVERIFY2(finishedSpy.wait(60000), "both queued files must finish parsing");
        }
        QCoreApplication::processEvents();

        // Without per-file sniffing the logfmt lines would be parse
        // errors and never become rows.
        QVERIFY2(model->StreamingErrors().empty(), "mixed-format queue must not produce parse errors");
        QCOMPARE(model->rowCount(), jsonLines.size() + logfmtLines.size());
    }

    // Regression for the Append-while-streaming crash: Append while
    // a static streaming worker was in flight used to call
    // `AppendStreaming` and assert `!isRunning()`. The fix defers
    // the new files into `mPendingOpenFiles` so the next
    // `streamingFinished` picks them up.
    void TestAppendDuringActiveStreamingDefersInsteadOfCrashing()
    {
        auto *model = mWindow->Model();
        QVERIFY(model != nullptr);

        // Inflate fixture A so the first parse is still in flight
        // when the second `OpenFilesForTest` lands. Short fixtures
        // would race the GUI thread on fast machines.
        QStringList fixtureLinesA;
        fixtureLinesA.reserve(500);
        for (int i = 0; i < 500; ++i)
        {
            fixtureLinesA.append(QStringLiteral(R"({"msg": "a-%1"})").arg(i));
        }
        const QStringList fixtureLinesB{
            QStringLiteral(R"({"msg": "b-0"})"),
            QStringLiteral(R"({"msg": "b-1"})"),
        };
        const TempJsonFile fixtureA(fixtureLinesA);
        const TempJsonFile fixtureB(fixtureLinesB);

        QSignalSpy finishedSpy(model, &LogModel::streamingFinished);
        QVERIFY(finishedSpy.isValid());

        // First open arms the worker but does NOT wait. No events are
        // pumped before the second open: the session only ends on the
        // GUI thread, so it is still active however fast the worker is.
        mWindow->OpenFilesForTest({fixtureA.Path()}, MainWindow::OpenMode::Append);
        QVERIFY(model->IsStreamingActive());

        // Second open is the regression case: with an in-flight
        // streaming worker, the file must defer onto
        // `mPendingOpenFiles` instead of tripping `AppendStreaming`'s
        // `!isRunning()` assert.
        mWindow->OpenFilesForTest({fixtureB.Path()}, MainWindow::OpenMode::Append);

        // Drain both `streamingFinished` events; the deferred file
        // produces a second one after the first worker terminates.
        while (finishedSpy.count() < 2)
        {
            QVERIFY2(finishedSpy.wait(60000), "both streaming sessions must finish");
        }
        QCoreApplication::processEvents();

        QCOMPARE(model->rowCount(), fixtureLinesA.size() + fixtureLinesB.size());

        // Both files land on the source descriptor in load order
        // despite the second open being deferred.
        const QTemporaryDir saved;
        QVERIFY(saved.isValid());
        const QString sessionPath = saved.filePath(QStringLiteral("appended_during_stream.json"));
        mWindow->SaveConfigurationToPathForTest(sessionPath, loglib::SaveScope::Full);

        const loglib::LogConfiguration probe = loglib::LoadConfiguration(sessionPath.toStdString());
        QVERIFY(probe.source.has_value());
        QCOMPARE(probe.source->locators.size(), static_cast<std::size_t>(2));
        QCOMPARE(
            QString::fromStdString(probe.source->locators[0].displayPath), logapp::CanonicalDisplayPath(fixtureA.Path())
        );
        QCOMPARE(
            QString::fromStdString(probe.source->locators[1].displayPath), logapp::CanonicalDisplayPath(fixtureB.Path())
        );
    }

    // Switching to live-tail mid-append-queue used to silently drop
    // the remaining queued files via the cancel-path `clear()`. The
    // fix snapshots the queue size, posts a status-bar hint, and
    // clears the queue explicitly before the reset.
    void TestOpenLogStreamSurfacesDiscardedQueuedFiles()
    {
        auto *model = mWindow->Model();
        QVERIFY(model != nullptr);

        // No events are pumped between the opens, so the second Append
        // queues into `mPendingOpenFiles`: the first session only ends
        // on the GUI thread, however fast its worker is.
        QStringList fixtureLinesA;
        fixtureLinesA.reserve(500);
        for (int i = 0; i < 500; ++i)
        {
            fixtureLinesA.append(QStringLiteral(R"({"msg": "discard-a-%1"})").arg(i));
        }
        const TempJsonFile fixtureA(fixtureLinesA);
        const TempJsonFile fixtureB({QStringLiteral(R"({"msg": "discard-b"})")});
        const TempJsonFile streamFixture({QStringLiteral(R"({"msg": "discard-stream"})")});

        const QSignalSpy finishedSpy(model, &LogModel::streamingFinished);
        QVERIFY(finishedSpy.isValid());

        mWindow->OpenFilesForTest({fixtureA.Path()}, MainWindow::OpenMode::Append);
        QVERIFY(model->IsStreamingActive());
        // Second open lands in `mPendingOpenFiles` because the
        // first session is still active.
        mWindow->OpenFilesForTest({fixtureB.Path()}, MainWindow::OpenMode::Append);

        mWindow->statusBar()->clearMessage();

        // Switch to live-tail while the append queue is primed.
        // The fix surfaces a discard hint; without it the queue is
        // silently cleared via the cancel-path inside Reset().
        mWindow->OpenLogStreamForTest(streamFixture.Path());
        QCoreApplication::processEvents();

        const QString message = mWindow->statusBar()->currentMessage();
        QVERIFY2(
            message.contains(QStringLiteral("queued file")),
            qPrintable(QStringLiteral("expected discard hint in status bar; got: '%1'").arg(message))
        );

        // fixtureB never streamed (discarded before the live-tail
        // took over). We only check the queue drop, not the
        // live-tail row count.
        const bool seesFixtureB = [&]() {
            const int rows = model->rowCount();
            for (int r = 0; r < rows; ++r)
            {
                const QModelIndex idx = model->index(r, 0);
                if (model->data(idx, Qt::DisplayRole).toString().contains(QStringLiteral("discard-b")))
                {
                    return true;
                }
            }
            return false;
        }();
        QVERIFY2(!seesFixtureB, "discarded queued file's rows must not be in the model");
    }

    // `OpenMode::Replace` is the destructive path: rows wiped, filters
    // cleared, source descriptor reset to just the new file.
    void TestOpenFilesReplaceWipesPreviousSession()
    {
        auto *model = mWindow->Model();
        QVERIFY(model != nullptr);

        const QStringList fixtureLinesA{
            QStringLiteral(R"({"msg": "a-0"})"),
            QStringLiteral(R"({"msg": "a-1"})"),
        };
        const QStringList fixtureLinesB{
            QStringLiteral(R"({"msg": "b-0"})"),
        };
        const TempJsonFile fixtureA(fixtureLinesA);
        const TempJsonFile fixtureB(fixtureLinesB);

        QSignalSpy finishedSpy(model, &LogModel::streamingFinished);
        QVERIFY(finishedSpy.isValid());

        mWindow->OpenFilesForTest({fixtureA.Path()}, MainWindow::OpenMode::Append);
        QTRY_VERIFY_WITH_TIMEOUT(finishedSpy.count() >= 1, 60000);
        QCoreApplication::processEvents();
        QCOMPARE(model->rowCount(), fixtureLinesA.size());

        finishedSpy.clear();
        mWindow->OpenFilesForTest({fixtureB.Path()}, MainWindow::OpenMode::Replace);
        QVERIFY(finishedSpy.wait(60000));
        QCoreApplication::processEvents();

        QCOMPARE(model->rowCount(), fixtureLinesB.size());

        const QTemporaryDir saved;
        QVERIFY(saved.isValid());
        const QString sessionPath = saved.filePath(QStringLiteral("replaced.json"));
        mWindow->SaveConfigurationToPathForTest(sessionPath, loglib::SaveScope::Full);

        const loglib::LogConfiguration probe = loglib::LoadConfiguration(sessionPath.toStdString());
        QVERIFY(probe.source.has_value());
        QCOMPARE(probe.source->locators.size(), static_cast<std::size_t>(1));
        QCOMPARE(
            QString::fromStdString(probe.source->locators.front().displayPath),
            logapp::CanonicalDisplayPath(fixtureB.Path())
        );
    }

    // Transparent decompression happy path: a gzip JSONL fixture
    // streams through the full open pipeline (sniff -> decompress
    // -> continue -> parse). Pins row count, the source locator
    // (original .gz path, not the temp file), and detected format.
    void TestOpenCompressedFileDecodesAndStreams()
    {
        auto *model = mWindow->Model();
        QVERIFY(model != nullptr);
        // Skip the potential progress dialog on slow runners.
        mWindow->SetSuppressDialogsForTest(true);

        // 500 lines: well below `DECOMPRESSION_DIALOG_DEFER_MS` on
        // any runner but still large enough that decompression is
        // genuinely async (worker + queued `finished` callout).
        QStringList fixtureLines;
        fixtureLines.reserve(500);
        for (int i = 0; i < 500; ++i)
        {
            fixtureLines.append(QStringLiteral(R"({"idx": %1, "msg": "compressed-happy-%1"})").arg(i));
        }
        QByteArray uncompressed;
        for (const QString &line : fixtureLines)
        {
            uncompressed.append(line.toUtf8());
            uncompressed.append('\n');
        }
        const QByteArray gzipped = GzipCompressForTest(uncompressed);

        const QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        const QString path = tempDir.filePath(QStringLiteral("happy.jsonl.gz"));
        WriteBinaryForTest(path, gzipped);

        QSignalSpy finishedSpy(model, &LogModel::streamingFinished);
        QVERIFY(finishedSpy.isValid());

        mWindow->OpenFilesForTest({path}, MainWindow::OpenMode::Replace);
        // The decompression flag must be set synchronously (before
        // any events pump). Same precondition as the Append-during-
        // decompression regression; fixture size ensures it holds.
        QVERIFY2(
            mWindow->IsDecompressionInFlightForTest(),
            "gzip open must arm the async decompression worker synchronously; without this precondition the "
            "append-during-decompression regression below is silently vacuous"
        );

        while (finishedSpy.count() < 1)
        {
            QVERIFY2(finishedSpy.wait(60000), "compressed open must produce a streamingFinished event");
        }
        QCoreApplication::processEvents();

        QCOMPARE(model->rowCount(), fixtureLines.size());

        // Session locator must be the ORIGINAL compressed path --
        // the temp file is a per-open implementation detail and must
        // never appear in a persisted descriptor.
        const QTemporaryDir savedDir;
        QVERIFY(savedDir.isValid());
        const QString sessionPath = savedDir.filePath(QStringLiteral("compressed_happy.json"));
        mWindow->SaveConfigurationToPathForTest(sessionPath, loglib::SaveScope::Full);

        const loglib::LogConfiguration probe = loglib::LoadConfiguration(sessionPath.toStdString());
        QVERIFY(probe.source.has_value());
        QCOMPARE(probe.source->locators.size(), static_cast<std::size_t>(1));
        QCOMPARE(
            QString::fromStdString(probe.source->locators.front().displayPath), logapp::CanonicalDisplayPath(path)
        );
        // Format sniff runs on the DECOMPRESSED bytes -- extension
        // (`.jsonl.gz`) must not fool the classifier.
        QCOMPARE(probe.source->format, loglib::Source::Format::Json);
    }

    // Regression: appending during an in-flight decompression must
    // queue behind the worker instead of racing it. Pre-fix, this
    // either wiped the Append's rows (decompression's `BeginStreaming`
    // finished second) or tripped `AppendStreaming`'s assert
    // (`!mStreamingWatcher->isRunning()`) when it finished first. The
    // append lands after the decompressed file; total = sum of both.
    void TestAppendDuringDecompressionQueuesBehindWorker()
    {
        auto *model = mWindow->Model();
        QVERIFY(model != nullptr);
        mWindow->SetSuppressDialogsForTest(true);

        // Same 500-line gzip fixture as the happy-path test above.
        QStringList gzipLines;
        gzipLines.reserve(500);
        for (int i = 0; i < 500; ++i)
        {
            gzipLines.append(QStringLiteral(R"({"idx": %1, "msg": "gz-%1"})").arg(i));
        }
        QByteArray uncompressed;
        for (const QString &line : gzipLines)
        {
            uncompressed.append(line.toUtf8());
            uncompressed.append('\n');
        }
        const QByteArray gzipped = GzipCompressForTest(uncompressed);

        const QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        const QString gzipPath = tempDir.filePath(QStringLiteral("first.jsonl.gz"));
        WriteBinaryForTest(gzipPath, gzipped);

        const QStringList appendLines{
            QStringLiteral(R"({"idx": 0, "msg": "append-0"})"),
            QStringLiteral(R"({"idx": 1, "msg": "append-1"})"),
            QStringLiteral(R"({"idx": 2, "msg": "append-2"})"),
        };
        const TempJsonFile appendFixture(appendLines);

        QSignalSpy finishedSpy(model, &LogModel::streamingFinished);
        QVERIFY(finishedSpy.isValid());

        // Step 1: arm the compressed open WITHOUT pumping events so
        // the append races a synchronously-armed decompression worker
        // (deterministic timing).
        mWindow->OpenFilesForTest({gzipPath}, MainWindow::OpenMode::Replace);
        QVERIFY2(
            mWindow->IsDecompressionInFlightForTest(),
            "test precondition: gzip decompression must be in flight before the append call lands"
        );

        // Step 2: append while the guard above still holds. Pre-fix,
        // this would either clobber `mPendingOpenFiles` and race
        // `BeginStreaming` (row loss when decompression resets the
        // model), or fall through and dispatch a second worker that
        // tripped `AppendStreaming`'s assert.
        mWindow->OpenFilesForTest({appendFixture.Path()}, MainWindow::OpenMode::Append);
        // Append must have taken the "queue" branch; only the
        // finished / cancel slots clear the flag, and neither runs
        // synchronously here.
        QVERIFY2(
            mWindow->IsDecompressionInFlightForTest(),
            "Append during in-flight decompression must not cancel the pending worker; the guard should defer "
            "the append into mPendingOpenFiles and leave the decompression running"
        );

        // Two files -> two `streamingFinished` events (decompressed
        // first, then the uncompressed append).
        while (finishedSpy.count() < 2)
        {
            QVERIFY2(finishedSpy.wait(60000), "both compressed + appended sessions must finish");
        }
        QCoreApplication::processEvents();

        // Row-count == sum of both files (row-loss would leave only one).
        QCOMPARE(model->rowCount(), gzipLines.size() + appendLines.size());

        // Locators must preserve load order and be the ORIGINAL
        // user-facing paths (not the decompressed temp file).
        const QTemporaryDir savedDir;
        QVERIFY(savedDir.isValid());
        const QString sessionPath = savedDir.filePath(QStringLiteral("append_during_decompression.json"));
        mWindow->SaveConfigurationToPathForTest(sessionPath, loglib::SaveScope::Full);

        const loglib::LogConfiguration probe = loglib::LoadConfiguration(sessionPath.toStdString());
        QVERIFY(probe.source.has_value());
        QCOMPARE(probe.source->locators.size(), static_cast<std::size_t>(2));
        QCOMPARE(QString::fromStdString(probe.source->locators[0].displayPath), logapp::CanonicalDisplayPath(gzipPath));
        QCOMPARE(
            QString::fromStdString(probe.source->locators[1].displayPath),
            logapp::CanonicalDisplayPath(appendFixture.Path())
        );
    }

    // Regression: a compressed file queued behind an uncompressed
    // one used to wipe the prior file's rows. Pre-fix flow:
    // `OnStreamingFinished(Success)` chained to
    // `StreamNextPendingFile`, which dispatched decompression and
    // returned; the "streaming active?" guard was then false (only
    // a decompression worker was running), so teardown ran and
    // `mSessionMode` went Idle. On finish,
    // `ContinueOpenAfterPrepared` saw `!IsSessionActive()` and took
    // `BeginStreaming` instead of `AppendStreaming`, wiping the
    // uncompressed file's rows. The fix adds
    // `|| mDecompressionInFlight` to the guard.
    void TestQueuedCompressedFollowUpKeepsPriorRows()
    {
        auto *model = mWindow->Model();
        QVERIFY(model != nullptr);
        mWindow->SetSuppressDialogsForTest(true);

        // Uncompressed fixture large enough that its parse is still
        // running when the Append lands (same precondition as
        // `TestAppendDuringActiveStreamingDefersInsteadOfCrashing`).
        QStringList fixtureLinesA;
        fixtureLinesA.reserve(500);
        for (int i = 0; i < 500; ++i)
        {
            fixtureLinesA.append(QStringLiteral(R"({"idx": %1, "msg": "chain-a-%1"})").arg(i));
        }
        const TempJsonFile uncompressedFixture(fixtureLinesA);

        // Compressed follow-up: any size works, keep it short.
        const QStringList gzipLines{
            QStringLiteral(R"({"idx": 0, "msg": "chain-b-0"})"),
            QStringLiteral(R"({"idx": 1, "msg": "chain-b-1"})"),
            QStringLiteral(R"({"idx": 2, "msg": "chain-b-2"})"),
        };
        QByteArray uncompressedGz;
        for (const QString &line : gzipLines)
        {
            uncompressedGz.append(line.toUtf8());
            uncompressedGz.append('\n');
        }
        const QByteArray gzipped = GzipCompressForTest(uncompressedGz);

        const QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        const QString gzipPath = tempDir.filePath(QStringLiteral("second.jsonl.gz"));
        WriteBinaryForTest(gzipPath, gzipped);

        QSignalSpy finishedSpy(model, &LogModel::streamingFinished);
        QVERIFY(finishedSpy.isValid());

        // Step 1: arm the uncompressed open and pump so the parse
        // worker actually starts.
        mWindow->OpenFilesForTest({uncompressedFixture.Path()}, MainWindow::OpenMode::Append);
        QCoreApplication::processEvents();

        // Step 2: append the gzip while the parse is still running;
        // it queues in `mPendingOpenFiles`.
        mWindow->OpenFilesForTest({gzipPath}, MainWindow::OpenMode::Append);

        // Both files land in load order. Pre-fix, the gzip's
        // `BeginStreaming` (taken because teardown had set Idle)
        // would wipe the uncompressed rows.
        while (finishedSpy.count() < 2)
        {
            QVERIFY2(finishedSpy.wait(60000), "both queued sessions must finish");
        }
        QCoreApplication::processEvents();

        QCOMPARE(model->rowCount(), fixtureLinesA.size() + gzipLines.size());

        // Locators in load order; both must be the ORIGINAL paths
        // (not the decompressed temp file).
        const QTemporaryDir savedDir;
        QVERIFY(savedDir.isValid());
        const QString sessionPath = savedDir.filePath(QStringLiteral("queued_compressed_follow_up.json"));
        mWindow->SaveConfigurationToPathForTest(sessionPath, loglib::SaveScope::Full);

        const loglib::LogConfiguration probe = loglib::LoadConfiguration(sessionPath.toStdString());
        QVERIFY(probe.source.has_value());
        QCOMPARE(probe.source->locators.size(), static_cast<std::size_t>(2));
        QCOMPARE(
            QString::fromStdString(probe.source->locators[0].displayPath),
            logapp::CanonicalDisplayPath(uncompressedFixture.Path())
        );
        QCOMPARE(QString::fromStdString(probe.source->locators[1].displayPath), logapp::CanonicalDisplayPath(gzipPath));
    }

    // Cancel-path regression. Verifies a stop mid-decompression
    // unwinds cleanly:
    //   - flag flips back to false;
    //   - no `streamingFinished` (nothing was ever armed on the
    //     model; a spurious event would mean cancel accidentally
    //     took the BeginStreaming path);
    //   - no rows in the model;
    //   - a subsequent open still works.
    //
    // Uses `RequestDecompressionCancelForTest` because the
    // production `QProgressDialog::canceled` wiring is unreachable
    // under `SetSuppressDialogsForTest`.
    void TestCancelDecompressionUnwindsCleanly()
    {
        auto *model = mWindow->Model();
        QVERIFY(model != nullptr);
        mWindow->SetSuppressDialogsForTest(true);

        // Big enough that the worker is still running when cancel
        // lands. Small fixtures race the cancel and pass vacuously.
        // 20K padded lines -> ~2 MiB, plenty of `ObservePoll`
        // checkpoints for the stop token.
        QByteArray uncompressed;
        uncompressed.reserve(3'000'000);
        const QString pad =
            QStringLiteral("padding-payload-so-the-input-stream-is-big-enough-that-cancel-lands-before-completion");
        for (int i = 0; i < 20000; ++i)
        {
            uncompressed.append(QStringLiteral(R"({"idx": %1, "msg": "%2-%1"})").arg(i).arg(pad).toUtf8());
            uncompressed.append('\n');
        }
        const QByteArray gzipped = GzipCompressForTest(uncompressed);

        const QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        const QString gzipPath = tempDir.filePath(QStringLiteral("cancel.jsonl.gz"));
        WriteBinaryForTest(gzipPath, gzipped);

        QSignalSpy finishedSpy(model, &LogModel::streamingFinished);
        QVERIFY(finishedSpy.isValid());

        mWindow->OpenFilesForTest({gzipPath}, MainWindow::OpenMode::Replace);
        QVERIFY2(
            mWindow->IsDecompressionInFlightForTest(),
            "precondition: decompression worker must be armed synchronously so the cancel has a target"
        );

        // Trip the stop token. Worker unwinds on its next
        // `ObservePoll`; finished slot posts the toast, returns idle.
        mWindow->RequestDecompressionCancelForTest();

        // Pump until the flag clears (10 s headroom).
        QTRY_VERIFY_WITH_TIMEOUT(!mWindow->IsDecompressionInFlightForTest(), 60000);

        // Cancel must NOT surface as `streamingFinished`: nothing
        // ever armed the model watcher. A non-zero count means the
        // cancel path is accidentally starting a parse session.
        QCOMPARE(finishedSpy.count(), 0);
        QCOMPARE(model->rowCount(), 0);

        // Pipeline must remain usable. Follow-up on a different
        // file exercises the full open path from a clean slate, so
        // residual state (dangling watcher, stale mCurrentSource,
        // wrong mSessionMode) surfaces as either no finish event
        // or rows leaking into the cancelled session.
        const QStringList followUpLines{
            QStringLiteral(R"({"idx": 0, "msg": "after-cancel-0"})"),
            QStringLiteral(R"({"idx": 1, "msg": "after-cancel-1"})"),
        };
        const TempJsonFile followUp(followUpLines);
        mWindow->OpenFilesForTest({followUp.Path()}, MainWindow::OpenMode::Replace);
        QVERIFY2(finishedSpy.wait(60000), "follow-up open after cancel must produce a streamingFinished event");
        QCoreApplication::processEvents();
        QCOMPARE(model->rowCount(), followUpLines.size());
    }

    // Regression: a decompression failure at the end of a multi-file
    // queue used to be silently lost. Pre-fix, opening `[good.log,
    // corrupt.gz]` in Append ended with the error entry stuck in
    // `mPendingDecompressionErrors`, `mSessionMode` still `Static`,
    // and no live worker to drain it -- user sees a wedged
    // "Parsing corrupt.gz" state.
    //
    // Fix: `FinalizeAfterDecompressionIfChainTerminal` drains both
    // error buckets under their own titles when no successor worker
    // was armed, and settles the UI back to Idle. Prior rows survive.
    void TestChainTerminalDecompressionFailureSurfacesError()
    {
        auto *model = mWindow->Model();
        QVERIFY(model != nullptr);
        mWindow->SetSuppressDialogsForTest(true);

        auto *dock = mWindow->findChild<ParseErrorsDock *>();
        QVERIFY2(dock != nullptr, "MainWindow must own a ParseErrorsDock");
        dock->ResetSessionState();
        const int startingErrorCount = dock->Count();

        // 500-line uncompressed fixture so its parse is still
        // running when the corrupt .gz Append lands (short fixtures
        // drain first and hit the fresh-open path, not the chain
        // path this regression targets).
        QStringList uncompressedLines;
        uncompressedLines.reserve(500);
        for (int i = 0; i < 500; ++i)
        {
            uncompressedLines.append(QStringLiteral(R"({"idx": %1, "msg": "good-%1"})").arg(i));
        }
        const TempJsonFile uncompressedFixture(uncompressedLines);

        // Valid gzip magic + header, truncated deflate stream ->
        // sniff routes to gzip, decoder errors on premature EOF.
        const QByteArray validGzip =
            GzipCompressForTest(QByteArrayLiteral(R"({"idx": 0, "msg": "will-not-survive-truncation"})"));
        QVERIFY2(validGzip.size() > 12, "gzip helper must produce a real stream (magic + header + payload + footer)");
        const QByteArray corruptGzip = validGzip.left(12);

        const QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        const QString gzipPath = tempDir.filePath(QStringLiteral("chain_terminal_corrupt.jsonl.gz"));
        WriteBinaryForTest(gzipPath, corruptGzip);

        QSignalSpy finishedSpy(model, &LogModel::streamingFinished);
        QVERIFY(finishedSpy.isValid());

        // Arm the uncompressed open and pump so the parse starts.
        mWindow->OpenFilesForTest({uncompressedFixture.Path()}, MainWindow::OpenMode::Append);
        QCoreApplication::processEvents();

        // Append the corrupt .gz while the parse is running --
        // queues in `mPendingOpenFiles`.
        mWindow->OpenFilesForTest({gzipPath}, MainWindow::OpenMode::Append);

        // Wait for `good.log` to finish (chains into decompression). A
        // fast parse may already have finished while events were pumped
        // above, so check the count rather than wait for a new signal.
        QTRY_VERIFY2_WITH_TIMEOUT(
            finishedSpy.count() >= 1, "first file must reach terminal state before the chain kicks in", 10000
        );
        // Drive events until the chained decompression fails and the
        // pipeline settles. `IsStreamingActive` clears when
        // `good.log` finishes, so also gate on the decompression flag.
        QTRY_VERIFY_WITH_TIMEOUT(!mWindow->IsDecompressionInFlightForTest(), 60000);
        QTRY_VERIFY_WITH_TIMEOUT(!model->IsStreamingActive(), 60000);
        QCoreApplication::processEvents();

        // Prior rows survive (the bug was silent-error-loss, not
        // row-loss; assertion here for intent).
        QCOMPARE(model->rowCount(), uncompressedLines.size());

        // The decompression failure surfaces in the dock.
        QCOMPARE(dock->Count(), startingErrorCount + 1);

        // Entry must land under `Error Decompressing File`, not
        // `Error Opening File`. The whole point of the second error
        // bucket is correct categorisation.
        auto *list = dock->findChild<QListWidget *>(QStringLiteral("parseErrorsList"));
        QVERIFY2(list != nullptr, "ParseErrorsDock must expose its internal QListWidget");
        bool foundDecompressionHeader = false;
        bool foundCorruptEntry = false;
        for (int i = 0; i < list->count(); ++i)
        {
            const QListWidgetItem *item = list->item(i);
            if (item == nullptr)
            {
                continue;
            }
            const QString text = item->text();
            if (text.contains(QStringLiteral("Error Decompressing File")))
            {
                foundDecompressionHeader = true;
            }
            if (text.contains(QStringLiteral("chain_terminal_corrupt.jsonl.gz")))
            {
                foundCorruptEntry = true;
            }
        }
        QVERIFY2(
            foundDecompressionHeader,
            "the chain-terminal decompression failure must be surfaced under an "
            "`Error Decompressing File` header, not lumped into `Error Opening File`"
        );
        QVERIFY2(
            foundCorruptEntry, "the failure entry must name the original .gz path so the user can locate the culprit"
        );

        // Regression assertion #3: the pipeline must be usable
        // again after the chain-terminal failure. Pre-fix,
        // `mSessionMode` was stuck at `Static` with no live worker
        // and the configuration UI stayed disabled. Re-arming a
        // fresh session via Append (which follows the "no active
        // session" branch in `StartStreamingOpenQueue`) would
        // succeed either way, but the *observable* stuckness is
        // the streaming label / session mode -- so we exercise
        // that indirectly by opening a follow-up and confirming
        // its rows land on top (append onto the surviving
        // session, not a wedged Replace that clears them).
        const QStringList followUpLines{QStringLiteral(R"({"idx": 0, "msg": "after-chain-failure"})")};
        const TempJsonFile followUp(followUpLines);
        mWindow->OpenFilesForTest({followUp.Path()}, MainWindow::OpenMode::Append);
        QVERIFY2(finishedSpy.wait(60000), "follow-up open after chain-terminal failure must produce streamingFinished");
        QCoreApplication::processEvents();
        QCOMPARE(model->rowCount(), uncompressedLines.size() + followUpLines.size());
    }

    // `actionNewSession` clears rows, runtime filters, the persisted
    // column configuration, and session mode. Reached through
    // `findChild<QAction*>("actionNewSession")` so the action wiring
    // is covered too.
    void TestNewSessionActionResetsState()
    {
        auto *model = mWindow->Model();
        QVERIFY(model != nullptr);

        // Two distinct keys so the auto-detector materialises at
        // least two columns; NewSession must wipe both.
        const QStringList fixtureLines{
            QStringLiteral(R"({"category": "info", "msg": "x-0"})"),
            QStringLiteral(R"({"category": "warn", "msg": "x-1"})"),
        };
        const TempJsonFile fixture(fixtureLines);

        QSignalSpy finishedSpy(model, &LogModel::streamingFinished);
        QVERIFY(finishedSpy.isValid());
        mWindow->OpenFilesForTest({fixture.Path()}, MainWindow::OpenMode::Append);
        QVERIFY(finishedSpy.wait(60000));
        QCoreApplication::processEvents();
        QCOMPARE(model->rowCount(), fixtureLines.size());
        QVERIFY2(model->columnCount() >= 2, "fixture must auto-detect at least the two JSON keys as columns");

        // Apply a sort so NewSession has something to clear --
        // otherwise the post-condition holds trivially.
        const int categoryCol = ColumnByHeader(*model, QStringLiteral("category"));
        QVERIFY2(categoryCol >= 0, "category column must exist after streaming");
        mWindow->findChild<LogTableView *>()->sortByColumn(categoryCol, Qt::DescendingOrder);
        QCoreApplication::processEvents();
        QCOMPARE(mWindow->FilterModel()->SortColumn(), categoryCol);

        auto *action = mWindow->findChild<QAction *>(QStringLiteral("actionNewSession"));
        QVERIFY2(action != nullptr, "actionNewSession must be reachable via objectName");
        action->trigger();
        QCoreApplication::processEvents();

        // Rows, columns, runtime filters gone; sort cleared.
        QCOMPARE(model->rowCount(), 0);
        QCOMPARE(model->columnCount(), 0);
        QVERIFY2(model->Layout().Columns().empty(), "New Session must clear the persisted column configuration");
        QVERIFY2(mWindow->Filters().empty(), "New Session must clear the runtime filter map");
        QCOMPARE(mWindow->FilterModel()->SortColumn(), -1);

        // SaveSession on the cleared state writes no source, empty
        // columns / filters arrays, and the default sort.
        const QTemporaryDir saved;
        QVERIFY(saved.isValid());
        const QString sessionPath = saved.filePath(QStringLiteral("new-session.json"));
        mWindow->SaveConfigurationToPathForTest(sessionPath, loglib::SaveScope::Full);
        const loglib::LogConfiguration probe = loglib::LoadConfiguration(sessionPath.toStdString());
        QVERIFY2(!probe.source.has_value(), "New Session must clear the source descriptor");
        QVERIFY2(probe.columns.empty(), "New Session saved JSON must have no columns");
        QVERIFY2(loglib::IsMatchAll(probe.view.expression), "New Session saved JSON must have no filter expression");
        QVERIFY(probe.view.sort.columnKeys.empty());
    }

    // Regression: a Boolean filter with both sides selected renders
    // its menu title in canonical `"true, false"` order, even when
    // `filter.filterValues` is in non-canonical order (e.g. a hand-
    // edited `["false", "true"]` payload).
    void TestBooleanFilterMenuTitleCanonicalOrder()
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

        const QString filterId = QStringLiteral("bool-canonical-title");
        QVERIFY2(
            QMetaObject::invokeMethod(
                mWindow,
                "FilterBooleanSubmitted",
                Qt::DirectConnection,
                Q_ARG(QString, filterId),
                Q_ARG(int, flagCol),
                Q_ARG(bool, true),
                Q_ARG(bool, true)
            ),
            "FilterBooleanSubmitted slot must be invocable via meta-object"
        );
        QCoreApplication::processEvents();

        const auto findFilterMenuAction = [&](const QString &id) -> QAction * {
            const auto *menu = mWindow->findChild<QMenu *>(QStringLiteral("menuFilters"));
            if (menu == nullptr)
            {
                return nullptr;
            }
            for (QAction *action : menu->actions())
            {
                if (action->data().toString() == id)
                {
                    return action;
                }
            }
            return nullptr;
        };

        const QAction *action = findFilterMenuAction(filterId);
        QVERIFY2(action != nullptr, "Boolean filter must have a menu entry");
        QCOMPARE(action->text(), QStringLiteral("true, false"));

        // Single-side sanity: "false only" must render as just
        // "false" (no leftover "true, " from a stale code path).
        const QString falseOnlyId = QStringLiteral("bool-canonical-title-false");
        QVERIFY2(
            QMetaObject::invokeMethod(
                mWindow,
                "FilterBooleanSubmitted",
                Qt::DirectConnection,
                Q_ARG(QString, falseOnlyId),
                Q_ARG(int, flagCol),
                Q_ARG(bool, false),
                Q_ARG(bool, true)
            ),
            "FilterBooleanSubmitted slot must be invocable via meta-object"
        );
        QCoreApplication::processEvents();
        const QAction *falseAction = findFilterMenuAction(falseOnlyId);
        QVERIFY2(falseAction != nullptr, "false-only Boolean filter must have a menu entry");
        QCOMPARE(falseAction->text(), QStringLiteral("false"));

        model->EndStreaming(false);
    }

    // Switching from a static session to a live-tail stream must
    // flush unsaved post-`streamingFinished` edits to the recents
    // JSON before the destructive `mModel->Reset()`. Witnessed via
    // a `changed` signal tick on the pre-reset flush.
    void TestOpenLogStreamFlushesPriorStaticSessionEdits()
    {
        const QTemporaryDir sessionsDir;
        QVERIFY(sessionsDir.isValid());

        SessionHistoryManager manager(QDir(sessionsDir.path()), std::make_unique<InMemoryRecentsIndexStorage>());
        auto wired = std::make_unique<MainWindow>(mTheme.data(), &manager, nullptr);

        // Stage 1: open a static session and let it auto-save.
        // Drain `changed` before the assertion window.
        const TempJsonFile staticFixture({QStringLiteral(R"({"msg": "static"})")});
        QSignalSpy finishedSpy(wired->Model(), &LogModel::streamingFinished);
        wired->OpenFilesForTest({staticFixture.Path()}, MainWindow::OpenMode::Append);
        QVERIFY(finishedSpy.wait(60000));
        QCoreApplication::processEvents();

        QCOMPARE(manager.List().size(), 1);
        const QString staticUuid = wired->ActiveSessionUuid();
        QVERIFY(!staticUuid.isEmpty());

        const QSignalSpy changedSpy(&manager, &SessionHistoryManager::changed);
        QVERIFY(changedSpy.isValid());

        // Stage 2: switch to live-tail. The fix calls
        // `AutoSaveSessionSnapshot(false)` on the outgoing session
        // before the reset, firing `changed` once.
        const TempJsonFile streamFixture({QStringLiteral(R"({"msg": "stream-seed"})")});
        wired->OpenLogStreamForTest(streamFixture.Path());
        QCoreApplication::processEvents();

        QVERIFY2(
            changedSpy.count() >= 1, "OpenLogStream must flush the outgoing static session before resetting the model"
        );

        // Static entry survives (rewritten with the latest state);
        // the new live-tail session is intentionally not saved.
        QCOMPARE(manager.List().size(), 1);
        QCOMPARE(manager.List().front().uuid, staticUuid);

        // Post-reset: pin cleared so a follow-up flush does not
        // silently rewrite the static entry.
        QVERIFY(wired->ActiveSessionUuid().isEmpty());
    }
};

QTEST_MAIN(FilesTest)
#include "apptest_files_test.moc"
