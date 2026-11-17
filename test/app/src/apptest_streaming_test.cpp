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
class StreamingTest : public apptest_support::AppTestBase
{
    Q_OBJECT

private slots:

    // Regression: `Reset()` must emit a compensating `streamingFinished` based
    // on a GUI-thread flag (`mStreamingActive`), not `isRunning()` — the
    // latter races with the queued `OnFinished` and would silently drop the
    // signal, leaving configuration menus disabled.
    static void TestResetAfterBeginStreamingEmitsCompensatingFinished()
    {
        // Use a tiny in-memory file so BeginStreaming has a valid FileLineSource
        // to install on the model.
        const TempJsonFile fixture(QStringList{QStringLiteral(R"({"a": 1})")});

        LogModel model;
        QSignalSpy finishedSpy(&model, &LogModel::streamingFinished);
        QVERIFY(finishedSpy.isValid());

        auto fileSource = std::make_unique<loglib::FileLineSource>(fixture.Path().toStdString());
        static_cast<void>(model.BeginStreamingForSyncTest(std::move(fileSource)));

        // No parser is started, so `EndStreaming` will never fire on its own.
        // `Reset()` must emit `streamingFinished(true)` itself based on the
        // GUI-thread flag set by `BeginStreaming`.
        model.Reset();

        QCOMPARE(finishedSpy.count(), 1);
        QCOMPARE(finishedSpy.takeFirst().value(0).value<StreamingResult>(), StreamingResult::Cancelled);

        // A second `Reset()` after the first one already cleared the flag
        // must not emit a spurious `streamingFinished` (otherwise repeated
        // user-driven file opens would emit duplicate signals).
        model.Reset();
        QCOMPARE(finishedSpy.count(), 0);
    }

    // Regression: the sink-generation bump must happen in
    // `DropPendingBatches()` (called *after* `waitForFinished()`), not in
    // `RequestStop()`. Otherwise drain-phase queued lambdas pass the
    // mismatch check and run after `Reset()` has destroyed `mLogTable`
    // (use-after-free on dangling `FileLineSource*` + spurious second `streamingFinished`).
    static void TestResetDuringStreamingDropsDrainPhaseBatch()
    {
        const TempJsonFile fixture(QStringList{QStringLiteral(R"({"a": 1})")});

        LogModel model;
        QSignalSpy finishedSpy(&model, &LogModel::streamingFinished);
        QVERIFY(finishedSpy.isValid());

        auto fileSource = std::make_unique<loglib::FileLineSource>(fixture.Path().toStdString());
        loglib::FileLineSource *sourcePtr = fileSource.get();

        // The parseCallable parks on a CV until the test thread releases it,
        // which happens AFTER the worker has emitted drain-phase
        // `OnBatch` + `OnFinished` (the racy queued lambdas the fix invalidates).
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

        // `Reset()` blocks on `waitForFinished()`; release the worker from a
        // separate thread so `Reset()` can return.
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

        model.Reset();
        releaser.join();

        // Drain the queued lambdas; with the fix both observe a stale
        // generation and short-circuit.
        QCoreApplication::processEvents();

        QCOMPARE(finishedSpy.count(), 1);
        QCOMPARE(finishedSpy.takeFirst().value(0).value<StreamingResult>(), StreamingResult::Cancelled);
        QCOMPARE(model.rowCount(), 0);
    }

    // Sink Pause/Resume: while paused, `OnBatch` redirects into the paused
    // buffer instead of posting per-batch QueuedConnection lambdas; on
    // Resume the buffer is coalesced into a single batch and posted to
    // `LogModel::AppendBatch`.
    static void TestSinkPauseResumeCoalescesBufferedBatches()
    {
        LogModel model;
        const QSignalSpy lineCountSpy(&model, &LogModel::lineCountChanged);
        QVERIFY(lineCountSpy.isValid());

        loglib::StreamLineSource &streamSource = BeginSyntheticStreamSession(model);
        QtStreamingLogSink *sink = model.Sink();
        QVERIFY(sink != nullptr);
        QVERIFY(sink->IsActive());

        loglib::KeyIndex &keys = sink->Keys();
        const loglib::KeyId valueKey = keys.GetOrInsert(std::string("value"));

        sink->Pause();
        QVERIFY(sink->IsPaused());

        // Push three batches into the sink while paused — they must NOT
        // reach the model (no `lineCountChanged` ticks observed).
        const int batchesWhilePaused = 3;
        const int linesPerBatch = 5;
        for (int b = 0; b < batchesWhilePaused; ++b)
        {
            const auto firstLineId = (static_cast<size_t>(b) * static_cast<size_t>(linesPerBatch)) + size_t{1};
            const bool declareNewKey = (b == 0);
            // Direct call to OnBatch from this thread mirrors the worker.
            sink->OnBatch(MakeSyntheticBatch(
                streamSource, valueKey, firstLineId, static_cast<size_t>(linesPerBatch), declareNewKey
            ));
        }

        // Drain the event loop just to be sure no QueuedConnection lambdas
        // slipped through.
        QCoreApplication::processEvents();
        QCOMPARE(model.rowCount(), 0);
        QCOMPARE(static_cast<int>(sink->PausedLineCount()), batchesWhilePaused * linesPerBatch);

        // Resume coalesces the three batches into a single batch and posts
        // it via QueuedConnection; `processEvents` runs the lambda.
        sink->Resume();
        QVERIFY(!sink->IsPaused());
        QCoreApplication::processEvents();

        QCOMPARE(model.rowCount(), batchesWhilePaused * linesPerBatch);
        QCOMPARE(static_cast<int>(sink->PausedLineCount()), 0);

        model.EndStreaming(false);
    }

    // Regression: pausing a *static-streaming* session (the **Stream** menu
    // is reachable from both paths because `TogglePauseStream` only gates
    // on `IsStreamingActive()`) must not silently drop the buffered rows
    // on Resume. The bug was `CoalesceLocked` only moving `streamLines` /
    // `errors` / `newKeys`, dropping the `lines` and `localLineOffsets`
    // payload that the static path emits, and `PausedLineCountLocked`
    // only summing `streamLines.size()` (so the status-bar `K buffered`
    // would read 0 even with rows queued).
    static void TestSinkPauseResumePreservesStaticLineBatches()
    {
        // Need a real file because the `FileLineSource` (and the
        // model's `LogTable::AppendBatch` path that consumes `lines`) hold
        // pointers into the file. The file's contents don't have to match
        // what we synthesise -- we never call `LogLine::GetValue()` on them
        // through the table -- but they must outlive the test.
        const TempJsonFile fixture(
            QStringList{
                QStringLiteral(R"({"a": 1})"),
                QStringLiteral(R"({"a": 2})"),
                QStringLiteral(R"({"a": 3})"),
            }
        );

        LogModel model;
        const QSignalSpy lineCountSpy(&model, &LogModel::lineCountChanged);
        QVERIFY(lineCountSpy.isValid());

        auto fileSource = std::make_unique<loglib::FileLineSource>(fixture.Path().toStdString());
        loglib::FileLineSource *sourcePtr = fileSource.get();
        static_cast<void>(model.BeginStreamingForSyncTest(std::move(fileSource)));

        QtStreamingLogSink *sink = model.Sink();
        QVERIFY(sink != nullptr);
        QVERIFY(sink->IsActive());

        loglib::KeyIndex &keys = sink->Keys();
        const loglib::KeyId keyId = keys.GetOrInsert(std::string("a"));

        sink->Pause();
        QVERIFY(sink->IsPaused());

        // Push three static-path batches (`lines` + paired
        // `localLineOffsets`) into the sink while paused.
        const int batchesWhilePaused = 3;
        const int linesPerBatch = 4;
        size_t cursorOffset = 0;
        for (int b = 0; b < batchesWhilePaused; ++b)
        {
            loglib::RowBatch batch;
            batch.firstLineNumber = (static_cast<size_t>(b) * static_cast<size_t>(linesPerBatch)) + size_t{1};
            batch.lines.reserve(linesPerBatch);
            batch.localLineOffsets.reserve(linesPerBatch);
            for (int i = 0; i < linesPerBatch; ++i)
            {
                const auto lineNumber =
                    (static_cast<size_t>(b) * static_cast<size_t>(linesPerBatch)) + static_cast<size_t>(i) + size_t{1};
                std::vector<std::pair<loglib::KeyId, loglib::LogValue>> values;
                values.emplace_back(keyId, loglib::LogValue(static_cast<int64_t>(lineNumber)));
                batch.lines.emplace_back(std::move(values), *sourcePtr, lineNumber);
                cursorOffset += 16;
                batch.localLineOffsets.push_back(static_cast<uint64_t>(cursorOffset));
            }
            sink->OnBatch(std::move(batch));
        }

        QCoreApplication::processEvents();
        // No queued lambdas fired (we're paused); the buffered
        // count reflects the `lines` payload.
        QCOMPARE(model.rowCount(), 0);
        QCOMPARE(static_cast<int>(sink->PausedLineCount()), batchesWhilePaused * linesPerBatch);

        // Resume coalesces all three batches and posts once.
        sink->Resume();
        QVERIFY(!sink->IsPaused());
        QCoreApplication::processEvents();

        QCOMPARE(model.rowCount(), batchesWhilePaused * linesPerBatch);
        QCOMPARE(static_cast<int>(sink->PausedLineCount()), 0);

        model.EndStreaming(false);
    }

    // Pause + cap-shrink interaction: while paused, lowering
    // the retention cap must trim the paused buffer to `cap - visible`
    // (preserving the visible rows). Verified via PausedLineCount().
    static void TestPauseCapShrinkTrimsPausedBuffer()
    {
        LogModel model;
        loglib::StreamLineSource &streamSource = BeginSyntheticStreamSession(model);
        QtStreamingLogSink *sink = model.Sink();

        loglib::KeyIndex &keys = sink->Keys();
        const loglib::KeyId valueKey = keys.GetOrInsert(std::string("value"));

        model.SetRetentionCap(1000);
        model.AppendBatch(MakeSyntheticBatch(streamSource, valueKey, 1, 100, /*declareNewKey=*/true));
        QCOMPARE(model.rowCount(), 100);

        sink->Pause();
        // 500 paused-buffer rows arrive while paused (visible stays at 100).
        sink->OnBatch(MakeSyntheticBatch(streamSource, valueKey, 101, 500, /*declareNewKey=*/false));
        QCOMPARE(model.rowCount(), 100);
        QCOMPARE(static_cast<int>(sink->PausedLineCount()), 500);

        // Lower cap to (visible + 100). Visible rows must stay; paused buffer
        // must trim down to 100 so visible+buffered <= cap.
        model.SetRetentionCap(100 + 100);
        QCOMPARE(model.rowCount(), 100);
        QCOMPARE(static_cast<int>(sink->PausedLineCount()), 100);

        // Resume drains the trimmed buffer, FIFO-evicting the oldest visible
        // rows so the post-Resume visible count stays within the cap.
        sink->Resume();
        QCoreApplication::processEvents();
        QCOMPARE(model.rowCount(), 200);

        model.EndStreaming(false);
    }

    // The `actionOpenNetworkStream` UI entry must be reachable by
    // `objectName` so apptest harnesses (and any future tooling that
    // walks the menu tree) can locate it without poking `ui->`.
    static void TestActionOpenNetworkStreamIsExposed()
    {
        // Local variable name avoids the `MainWindowTest::window`
        // member-shadow C4458 warning under MSVC.
        const MainWindow mainWindow;
        const auto *action = mainWindow.findChild<QAction *>(QStringLiteral("actionOpenNetworkStream"));
        QVERIFY2(action != nullptr, "actionOpenNetworkStream must be reachable via objectName");
        // Ctrl+Shift+N was reassigned to File -> New Window; the
        // network-stream action moved to Ctrl+Shift+L.
        QCOMPARE(action->shortcut(), QKeySequence(QStringLiteral("Ctrl+Shift+L")));
    }

    //  regression: when the paused-buffer cap forces the
    // sink to drop its oldest entries, the count of lost lines must be
    // observable so the user knows rows were silently discarded during
    // the pause. This test pauses, feeds enough live-tail batches to
    // overflow the cap, and asserts both that `PausedDropCount()` > 0
    // and that the visible+buffered total stayed within the cap.
    static void TestPausedDropCountIsObservable()
    {
        LogModel model;
        loglib::StreamLineSource &streamSource = BeginSyntheticStreamSession(model);
        QtStreamingLogSink *sink = model.Sink();
        QVERIFY(sink != nullptr);

        const size_t cap = 100;
        model.SetRetentionCap(cap);

        QCOMPARE(static_cast<qulonglong>(sink->PausedDropCount()), qulonglong(0));

        loglib::KeyIndex &keys = sink->Keys();
        const loglib::KeyId valueKey = keys.GetOrInsert(std::string("value"));

        sink->Pause();
        QVERIFY(sink->IsPaused());

        // Push three 100-line batches while paused. The second and
        // third will each tip the buffer over the cap, evicting the
        // oldest 100 lines per batch. After the third batch the paused
        // buffer holds the most-recent 100 lines and the counter should
        // reflect ~200 evictions.
        const size_t batchLines = 100;
        const int batchesPushed = 3;
        for (int b = 0; b < batchesPushed; ++b)
        {
            const bool firstBatch = (b == 0);
            sink->OnBatch(MakeSyntheticBatch(
                streamSource, valueKey, (static_cast<size_t>(b) * batchLines) + 1, batchLines, firstBatch
            ));
        }

        QCOMPARE(static_cast<int>(sink->PausedLineCount()), static_cast<int>(cap));
        // Each overflow batch evicted 100 lines; two of three batches
        // overflowed → at least 200 lines dropped. (Exactly 200 in
        // practice; assert >= to avoid pinning the exact count against
        // future eviction-policy tweaks.)
        QVERIFY2(
            sink->PausedDropCount() >= batchLines * (batchesPushed - 1),
            qPrintable(QStringLiteral("PausedDropCount=%1 must be >= %2")
                           .arg(static_cast<qulonglong>(sink->PausedDropCount()))
                           .arg(static_cast<qulonglong>(batchLines * (batchesPushed - 1))))
        );

        // Resume and Detach: the counter survives Resume (it's a
        // session-scoped history of drops). Only `Arm()` /
        // `DropPendingBatches()` reset it.
        const uint64_t droppedBeforeResume = sink->PausedDropCount();
        sink->Resume();
        QCoreApplication::processEvents();
        QCOMPARE(sink->PausedDropCount(), droppedBeforeResume);

        model.StopAndKeepRows();
        QCoreApplication::processEvents();
    }

    // Regression: pausing during a live-tail session, feeding N
    // synthetic batches into the paused buffer, then `StopAndKeepRows`
    // must flush every still-buffered row into the visible model so
    // the user sees the final state of the parse rather than losing
    // the last-emitted rows. Asserts that
    // `visibleRows == sumOfBatchedRows - PausedDropCount` so the
    // accounting closes: any row that left the paused buffer either
    // landed in the visible model or was counted in the cap-eviction
    // counter, but never silently disappeared. Counterpart to
    // `testPausedDropCountIsObservable` (which exercises overflow);
    // this one exercises the under-cap normal-stop path.
    static void TestStopAfterPauseFlushesPausedBufferToModel()
    {
        LogModel model;
        loglib::StreamLineSource &streamSource = BeginSyntheticStreamSession(model);
        QtStreamingLogSink *sink = model.Sink();
        QVERIFY(sink != nullptr);

        // Cap chosen high enough to never trigger paused-buffer
        // eviction so this test exercises the no-loss path. Five
        // batches of 50 lines = 250 rows, well under cap.
        const size_t cap = 1000;
        model.SetRetentionCap(cap);
        QCOMPARE(static_cast<qulonglong>(sink->PausedDropCount()), qulonglong(0));

        loglib::KeyIndex &keys = sink->Keys();
        const loglib::KeyId valueKey = keys.GetOrInsert(std::string("value"));

        sink->Pause();
        QVERIFY(sink->IsPaused());

        const size_t batchLines = 50;
        const int batchesPushed = 5;
        const size_t totalLines = batchLines * batchesPushed;
        for (int b = 0; b < batchesPushed; ++b)
        {
            const bool firstBatch = (b == 0);
            sink->OnBatch(MakeSyntheticBatch(
                streamSource, valueKey, (static_cast<size_t>(b) * batchLines) + 1, batchLines, firstBatch
            ));
        }

        QCOMPARE(static_cast<int>(sink->PausedLineCount()), static_cast<int>(totalLines));
        QCOMPARE(model.rowCount(), 0); // still paused, nothing visible

        // StopAndKeepRows must drain the paused buffer into the
        // visible model BEFORE bumping the generation that would
        // short-circuit drain-phase callbacks. The teardown sequence
        // (TeardownStreamingSessionInternal) is responsible for
        // calling `TakePausedBuffer` and feeding it through
        // `AppendBatch` -- this test fails loudly if that step is
        // ever removed.
        model.StopAndKeepRows();
        QCoreApplication::processEvents();

        const qsizetype visible = model.rowCount();
        const auto dropped = static_cast<qulonglong>(sink->PausedDropCount());
        QVERIFY2(
            static_cast<qulonglong>(visible) + dropped == totalLines,
            qPrintable(QStringLiteral("rows accounting failed: visible=%1 + dropped=%2 != totalLines=%3")
                           .arg(visible)
                           .arg(dropped)
                           .arg(static_cast<qulonglong>(totalLines)))
        );
        QCOMPARE(static_cast<int>(visible), static_cast<int>(totalLines));
    }

    // Regression for the `linesDropped` snapshot bug.
    // When the head of `mPausedBatches` is a *static-content* batch
    // (`lines` + `localLineOffsets`, reachable when **Pause** is invoked
    // during a static-streaming parse from the **Stream** menu), the
    // eviction loop in `OnBatch` evicts the whole batch atomically even
    // when its row count exceeds the requested `toDrop` — the
    // `localLineOffsets` may be longer than `lines`, so a partial
    // prefix trim would break `LogTable::AppendBatch`. The counter
    // accumulates the actual lines evicted as the loop runs (not a
    // pre-loop snapshot).
    static void TestPausedDropCountReflectsStaticBatchOverEviction()
    {
        const TempJsonFile fixture(
            QStringList{
                QStringLiteral(R"({"a": 1})"),
                QStringLiteral(R"({"a": 2})"),
                QStringLiteral(R"({"a": 3})"),
            }
        );

        LogModel model;
        auto fileSource = std::make_unique<loglib::FileLineSource>(fixture.Path().toStdString());
        loglib::FileLineSource *sourcePtr = fileSource.get();
        static_cast<void>(model.BeginStreamingForSyncTest(std::move(fileSource)));

        QtStreamingLogSink *sink = model.Sink();
        QVERIFY(sink != nullptr);

        const size_t cap = 100;
        model.SetRetentionCap(cap);
        QCOMPARE(static_cast<qulonglong>(sink->PausedDropCount()), qulonglong(0));

        loglib::KeyIndex &keys = sink->Keys();
        const loglib::KeyId valueKey = keys.GetOrInsert(std::string("value"));

        sink->Pause();
        QVERIFY(sink->IsPaused());

        // Step 1 — fill the paused buffer to the cap with a single
        // *static-content* batch (`lines` + `localLineOffsets`).
        const size_t staticBatchRows = cap; // 100
        {
            loglib::RowBatch batch;
            batch.firstLineNumber = 1;
            batch.newKeys.emplace_back("value");
            batch.lines.reserve(staticBatchRows);
            batch.localLineOffsets.reserve(staticBatchRows);
            for (size_t i = 0; i < staticBatchRows; ++i)
            {
                const size_t lineNumber = i + 1;
                std::vector<std::pair<loglib::KeyId, loglib::LogValue>> values;
                values.emplace_back(valueKey, loglib::LogValue{static_cast<int64_t>(lineNumber)});
                batch.lines.emplace_back(std::move(values), *sourcePtr, lineNumber);
                batch.localLineOffsets.push_back(lineNumber * 16);
            }
            sink->OnBatch(std::move(batch));
        }
        QCOMPARE(static_cast<int>(sink->PausedLineCount()), static_cast<int>(staticBatchRows));
        QCOMPARE(static_cast<qulonglong>(sink->PausedDropCount()), qulonglong(0));

        // Step 2: push a small live-tail batch that overflows the
        // cap by a fraction of the static head. The entire 100-row
        // static head is evicted atomically; the counter must
        // reflect the actual eviction count. Build the live-tail
        // rows as
        // `LogLine`s referencing a no-producer `StreamLineSource` we
        // wire up directly (not via the model — the model is mid-static-
        // session here).
        const size_t overflowRows = 30;
        loglib::StreamLineSource liveTailSource(std::filesystem::path("synthetic"), nullptr);
        {
            loglib::RowBatch batch;
            batch.firstLineNumber = staticBatchRows + 1;
            batch.lines.reserve(overflowRows);
            for (size_t i = 0; i < overflowRows; ++i)
            {
                const size_t lineId = staticBatchRows + 1 + i;
                static_cast<void>(liveTailSource.AppendLine("synthetic", std::string{}));
                std::vector<loglib::CompactLogValue> compactValues;
                compactValues.push_back(
                    loglib::CompactLogValue::MakeInt64(static_cast<int64_t>(lineId)).WithKey(valueKey)
                );
                batch.lines.emplace_back(std::move(compactValues), liveTailSource, lineId);
            }
            sink->OnBatch(std::move(batch));
        }

        // Whole 100-row static head was evicted; only the 30-row
        // live-tail batch survives. Counter reports the 100 lines
        // that actually left the buffer.
        QCOMPARE(static_cast<int>(sink->PausedLineCount()), static_cast<int>(overflowRows));
        QCOMPARE(static_cast<qulonglong>(sink->PausedDropCount()), static_cast<qulonglong>(staticBatchRows));

        sink->Resume();
        QCoreApplication::processEvents();
        model.StopAndKeepRows();
        QCoreApplication::processEvents();
    }

    // Bug 2 regression: `actionPauseStream` is checkable and lives in
    // both the toolbar and the **Stream** menu. The menu stays reachable
    // even with the toolbar hidden. Without an explicit disable while
    // idle, a menu click would flip the action's checked state before
    // `TogglePauseStream`'s `IsStreamingActive()` early-return runs;
    // the toggle then survives into the next session — and the teardown
    // path *cannot* unstick it because `LogModel::Reset()` only emits
    // `streamingFinished` when the prior session was still active.
    //
    // The fix disables the three Stream actions whenever the toolbar
    // is hidden, and force-unchecks `actionPauseStream` defensively so
    // a stuck checked state from any other path can never persist.
    void TestStreamMenuActionsDisabledWhileIdle()
    {
        QAction *pauseAction = FindActionByObjectName(mWindow, QStringLiteral("actionPauseStream"));
        const QAction *followAction = FindActionByObjectName(mWindow, QStringLiteral("actionFollowTail"));
        const QAction *stopAction = FindActionByObjectName(mWindow, QStringLiteral("actionStopStream"));
        QVERIFY(pauseAction != nullptr);
        QVERIFY(followAction != nullptr);
        QVERIFY(stopAction != nullptr);

        // Idle at startup: no stream has been opened yet, the toolbar is
        // hidden, and the Stream menu actions must be disabled so a
        // user click on the menu cannot flip the checked state.
        // NOLINTBEGIN(clang-analyzer-core.CallAndMessage): false positive; prior `QVERIFY` aborts on null.
        QVERIFY(!pauseAction->isEnabled());
        QVERIFY(!followAction->isEnabled());
        QVERIFY(!stopAction->isEnabled());
        QVERIFY(!pauseAction->isChecked());
        // NOLINTEND(clang-analyzer-core.CallAndMessage)

        // `trigger()` ignores disabled actions (Qt won't deliver the
        // `triggered` / `toggled` signals on a disabled action), so even
        // a programmatic activation while idle leaves the checked state
        // alone. This is the user-facing guarantee.
        pauseAction->trigger();
        QVERIFY(!pauseAction->isChecked());
    }

    // Companion to `testStreamMenuActionsDisabledWhileIdle`: even if
    // some external path manages to flip `actionPauseStream` checked
    // (a future caller that programmatically re-enables and checks
    // the action; a test that pokes the QAction directly), the
    // `streamingFinished` slot must clear that stale state at the
    // next session boundary. This validates that my new disable-gate
    // in `UpdateStreamToolbarVisibility` did not interfere with the
    // existing Pause-toggle reset in the slot.
    void TestStaleCheckedPauseClearedOnTeardown()
    {
        QAction *pauseAction = FindActionByObjectName(mWindow, QStringLiteral("actionPauseStream"));
        QVERIFY(pauseAction != nullptr);

        // Simulate the bug condition: action enabled + checked
        // while idle.
        pauseAction->setEnabled(true);
        pauseAction->setChecked(true);
        QVERIFY(pauseAction->isChecked());

        // Drive a streaming session and tear it down. `StopAndKeepRows()` emits
        // a compensating `streamingFinished(Cancelled)` whose slot
        // (in `MainWindow`) resets the Pause toggle and refreshes the
        // toolbar gating. This is the existing reset path that must
        // keep working alongside the new disabled-while-idle invariant.
        auto *model = mWindow->findChild<LogModel *>();
        QVERIFY(model != nullptr);

        static_cast<void>(model->BeginStreamingForSyncTest(std::unique_ptr<loglib::LineSource>{}));
        model->StopAndKeepRows();
        QCoreApplication::processEvents();

        // After teardown the action is back to its idle baseline:
        // unchecked (cleared by the `streamingFinished` slot) and
        // disabled (by `UpdateStreamToolbarVisibility` since the
        // toolbar is no longer visible). The next legitimate session
        // will re-enable it from a clean baseline.
        QVERIFY(!pauseAction->isChecked());
        QVERIFY(!pauseAction->isEnabled());
    }

    // Regression: hover / layout-driven scrollbar updates must not
    // auto-disengage **Follow newest**. Earlier `LogTableView`
    // connected `valueChanged` directly to the edge-trigger lambda,
    // so any programmatic value change (`endInsertRows` clamp,
    // hover/repaint scroll adjustment,
    // a viewport-size update) flipped `mAtTailEdge` from true to false
    // and emitted `userScrolledAwayFromTail`, which the `MainWindow`
    // then dutifully translated into "uncheck Follow newest" — even
    // though the user never scrolled.
    //
    // The fix gates `userScrolledAwayFromTail` /
    // `userScrolledToTail` emissions on a user-input depth counter
    // that's only non-zero during real wheel / keyboard / scrollbar-
    // mouse events. A bare programmatic `setValue` to the middle of
    // the range — the closest in-process simulation of a hover-
    // triggered internal scroll update — must therefore leave the
    // toggle alone.
    void TestFollowTailIgnoresProgrammaticScrollbarChanges()
    {
        QAction *followAction = FindActionByObjectName(mWindow, QStringLiteral("actionFollowTail"));
        const auto *tableView = mWindow->findChild<LogTableView *>();
        QVERIFY(followAction != nullptr);
        QVERIFY(tableView != nullptr);

        // Enable + check the toggle from the simulated streaming-active
        // baseline (the production path enables it when a stream opens
        // via `UpdateStreamToolbarVisibility`). We bypass the gate here
        // because the test only needs the toggle's checked state to
        // observe the scrollbar→toggle wiring in isolation.
        followAction->setEnabled(true);
        followAction->setChecked(true);
        QVERIFY(followAction->isChecked());

        // Force the scrollbar into a valid range so `setValue` can push
        // the slider somewhere genuinely "not at bottom". Without this
        // setup the maximum stays at 0 and the at-bottom check is
        // trivially true regardless of value.
        QScrollBar *scrollBar = tableView->verticalScrollBar();
        QVERIFY(scrollBar != nullptr);
        scrollBar->setRange(0, 1000);
        scrollBar->setValue(scrollBar->maximum());

        const QSignalSpy awaySpy(tableView, &LogTableView::userScrolledAwayFromTail);
        QVERIFY(awaySpy.isValid());

        // Programmatic value change: mimics a layout-driven scroll
        // adjustment Qt fires on hover / repaint.
        scrollBar->setValue(scrollBar->maximum() / 2);
        QCoreApplication::processEvents();

        QCOMPARE(awaySpy.count(), 0);
        QVERIFY(followAction->isChecked());
    }

    // The **Show newest lines first** preference flips the
    // `RowOrderProxyModel` into reversed mode: the highest source
    // row index (the most-recently-appended streamed line) lands at
    // proxy row 0 and the oldest at the bottom of the visible model.
    // Drives the proxy directly (no Preferences dialog round-trip)
    // because the dialog's Ok handler is just a thin wrapper around
    // `RowOrderProxyModel::SetReversed` plus the persisted
    // `streaming/newestFirst` setting — exercising the proxy here
    // covers the contract that the GUI relies on.
    void TestNewestFirstReversesProxyOrder()
    {
        auto *rowOrderProxy = mWindow->findChild<RowOrderProxyModel *>();
        auto *model = mWindow->findChild<LogModel *>();
        QVERIFY(rowOrderProxy != nullptr);
        QVERIFY(model != nullptr);
        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): false positive; prior `QVERIFY` aborts on null.
        QVERIFY(!rowOrderProxy->IsReversed());

        // Drive a tiny streaming session with three rows so the proxy
        // has something to reorder. `valueKey` plus a per-line integer
        // gives us a deterministic identifier we can read back via
        // `data(SortRole)` to assert the visible row order.
        loglib::StreamLineSource &streamSource = BeginSyntheticStreamSession(*model);
        QtStreamingLogSink *sink = model->Sink();
        QVERIFY(sink != nullptr);

        loglib::KeyIndex &keys = sink->Keys();
        const loglib::KeyId valueKey = keys.GetOrInsert(std::string("value"));
        sink->OnBatch(MakeSyntheticBatch(streamSource, valueKey, 1, 3, /*declareNewKey=*/true));
        QCoreApplication::processEvents();
        QCOMPARE(model->rowCount(), 3);

        const int valueColumn = ColumnByHeader(*model, QStringLiteral("value"));
        QVERIFY(valueColumn >= 0);

        // Default (oldest-first) order: proxy row 0 == source row 0
        // == lineId 1, proxy row 2 == source row 2 == lineId 3.
        QCOMPARE(
            rowOrderProxy->data(rowOrderProxy->index(0, valueColumn), LogModelItemDataRole::SortRole).toLongLong(),
            qint64(1)
        );
        QCOMPARE(
            rowOrderProxy->data(rowOrderProxy->index(2, valueColumn), LogModelItemDataRole::SortRole).toLongLong(),
            qint64(3)
        );

        rowOrderProxy->SetReversed(true);
        QVERIFY(rowOrderProxy->IsReversed());

        // Reversed order: proxy row 0 carries the *most-recently-
        // appended* line (lineId 3), and proxy row 2 the oldest
        // (lineId 1). The source model (`model`) is untouched — only
        // the proxy mapping flips, so the underlying append-order
        // contract continues to hold.
        QCOMPARE(
            rowOrderProxy->data(rowOrderProxy->index(0, valueColumn), LogModelItemDataRole::SortRole).toLongLong(),
            qint64(3)
        );
        QCOMPARE(
            rowOrderProxy->data(rowOrderProxy->index(2, valueColumn), LogModelItemDataRole::SortRole).toLongLong(),
            qint64(1)
        );

        // Toggling back leaves the source order intact and restores
        // the identity mapping (idempotency contract for
        // `SetReversed`).
        rowOrderProxy->SetReversed(false);
        QVERIFY(!rowOrderProxy->IsReversed());
        QCOMPARE(
            rowOrderProxy->data(rowOrderProxy->index(0, valueColumn), LogModelItemDataRole::SortRole).toLongLong(),
            qint64(1)
        );

        model->EndStreaming(false);
    }

    // Regression for incremental streaming: `QSortFilterProxyModel`
    // does not reliably keep descending-by-insertion-order stable
    // across successive `rowsInserted` unless we queue an explicit
    // `sort()` after each structural change. **Show newest lines
    // first** enables reversed mode before any data (matching session
    // startup); the second batch must land with its newest line at
    // proxy row 0, not stuck under the first batch.
    void TestNewestFirstIncrementalBatchesKeepNewestAtTop()
    {
        auto *rowOrderProxy = mWindow->findChild<RowOrderProxyModel *>();
        auto *model = mWindow->findChild<LogModel *>();
        QVERIFY(rowOrderProxy != nullptr);
        QVERIFY(model != nullptr);

        loglib::StreamLineSource &streamSource = BeginSyntheticStreamSession(*model);
        QtStreamingLogSink *sink = model->Sink();
        QVERIFY(sink != nullptr);

        rowOrderProxy->SetReversed(true);
        QVERIFY(rowOrderProxy->IsReversed());

        loglib::KeyIndex &keys = sink->Keys();
        const loglib::KeyId valueKey = keys.GetOrInsert(std::string("value"));

        sink->OnBatch(MakeSyntheticBatch(streamSource, valueKey, 1, 3, /*declareNewKey=*/true));
        QCoreApplication::processEvents();
        QCOMPARE(model->rowCount(), 3);

        const int valueColumn = ColumnByHeader(*model, QStringLiteral("value"));
        QVERIFY(valueColumn >= 0);

        QCOMPARE(
            rowOrderProxy->data(rowOrderProxy->index(0, valueColumn), LogModelItemDataRole::SortRole).toLongLong(),
            qint64(3)
        );

        sink->OnBatch(MakeSyntheticBatch(streamSource, valueKey, 4, 2, /*declareNewKey=*/false));
        QCoreApplication::processEvents();
        QCOMPARE(model->rowCount(), 5);

        QCOMPARE(
            rowOrderProxy->data(rowOrderProxy->index(0, valueColumn), LogModelItemDataRole::SortRole).toLongLong(),
            qint64(5)
        );

        model->EndStreaming(false);
    }

    // Guard test: toggling `RowOrderProxyModel::SetReversed` must be
    // O(persistent indices), not O(rows). Regression here would mean
    // we accidentally re-introduced a `QSortFilterProxyModel`-style
    // full mapping rebuild — which is the exact O(N²) trap Phase 3
    // was written to fix.
    //
    // Method: install a single persistent index, fill the model with
    // 10'000 rows (one order of magnitude past anything Qt's painter
    // visits per frame), toggle 100 times, and assert the wall-time
    // stays well under a budget that a full per-row rebuild would
    // blow. Set deliberately loose to absorb CI noise; the failing
    // implementation routinely lands at multiple seconds, while the
    // current one lands in single-digit ms.
    void TestNewestFirstToggleIsOPersistentIndices()
    {
        auto *rowOrderProxy = mWindow->findChild<RowOrderProxyModel *>();
        auto *model = mWindow->findChild<LogModel *>();
        QVERIFY(rowOrderProxy != nullptr);
        QVERIFY(model != nullptr);

        loglib::StreamLineSource &streamSource = BeginSyntheticStreamSession(*model);
        QtStreamingLogSink *sink = model->Sink();
        QVERIFY(sink != nullptr);

        constexpr int K_ROWS = 10'000;
        loglib::KeyIndex &keys = sink->Keys();
        const loglib::KeyId valueKey = keys.GetOrInsert(std::string("value"));
        sink->OnBatch(MakeSyntheticBatch(streamSource, valueKey, 1, K_ROWS, /*declareNewKey=*/true));
        QCoreApplication::processEvents();
        QCOMPARE(model->rowCount(), K_ROWS);

        // Hold a single persistent index from somewhere in the
        // middle of the proxy. Toggling SetReversed must remap *this
        // one* via `changePersistentIndexList`; nothing else.
        const QPersistentModelIndex pinned(rowOrderProxy->index(K_ROWS / 2, 0));
        QVERIFY(pinned.isValid());

        rowOrderProxy->SetReversed(false);
        QVERIFY(!rowOrderProxy->IsReversed());

        QElapsedTimer timer;
        timer.start();
        constexpr int K_TOGGLES = 100;
        for (int i = 0; i < K_TOGGLES; ++i)
        {
            rowOrderProxy->SetReversed(i % 2 == 1);
        }
        const qint64 elapsedMs = timer.elapsed();

        // Budget: 200 ms for 100 toggles over 10'000 rows. The
        // QSortFilterProxyModel-based predecessor in newest-first
        // mode was O(N log N) per toggle and timed in seconds for
        // this row count.
        QVERIFY2(
            elapsedMs < 200,
            qPrintable(QStringLiteral(
                           "RowOrderProxyModel::SetReversed must be O(persistent indices), "
                           "not O(rows); 100 toggles over %1 rows took %2 ms (budget 200 ms)"
            )
                           .arg(K_ROWS)
                           .arg(elapsedMs))
        );

        // Sanity check: the persistent index correctly tracks the
        // mapping flip. After the last toggle (kToggles-1=99 -> odd
        // -> reversed=true), the pinned source row (kRows/2 in
        // identity, which is what the constructor saw) should sit at
        // proxy row (kRows - 1 - kRows/2).
        QVERIFY(pinned.isValid());
        QCOMPARE(pinned.row(), K_ROWS - 1 - (K_ROWS / 2));

        rowOrderProxy->SetReversed(false);
        model->EndStreaming(false);
    }

    // Regression for `LogFilterModel`'s persistent-index handling
    // across a *source-side* layout change. Toggling
    // `RowOrderProxyModel::SetReversed` is the production trigger:
    // it flips its mapping (source row K now points at a different
    // entity), emits `layoutChanged`, and the `LogFilterModel` above
    // must remap its own persistent indices to follow the entity,
    // not the bare row number.
    //
    // Earlier the snapshot stored an integer source row, which
    // under a no-filter pass-through stayed numerically the same
    // after the flip; the persistent index ended up pointing at
    // the row that moved into the slot -- a silent selection swap.
    // The
    // fix anchors each snapshot on a `QPersistentModelIndex` of the
    // source-mapped index, which the source's own
    // `changePersistentIndexList` remaps to the entity's new row.
    void TestNewestFirstReversalPreservesFilterModelSelection()
    {
        auto *rowOrderProxy = mWindow->findChild<RowOrderProxyModel *>();
        auto *model = mWindow->findChild<LogModel *>();
        auto *filterModel = mWindow->FilterModel();
        QVERIFY(rowOrderProxy != nullptr);
        QVERIFY(model != nullptr);
        QVERIFY(filterModel != nullptr);

        loglib::StreamLineSource &streamSource = BeginSyntheticStreamSession(*model);
        QtStreamingLogSink *sink = model->Sink();
        QVERIFY(sink != nullptr);

        constexpr int K_ROWS = 16;
        loglib::KeyIndex &keys = sink->Keys();
        const loglib::KeyId valueKey = keys.GetOrInsert(std::string("value"));
        sink->OnBatch(MakeSyntheticBatch(streamSource, valueKey, 1, K_ROWS, /*declareNewKey=*/true));
        QCoreApplication::processEvents();
        QCOMPARE(filterModel->rowCount(), K_ROWS);

        const int valueColumn = ColumnByHeader(*model, QStringLiteral("value"));
        QVERIFY(valueColumn >= 0);

        // Pin a persistent index on the filter model (not the
        // RowOrderProxyModel) at proxy row 5. In default
        // (oldest-first) mode that's lineId 6.
        constexpr int K_PINNED_PROXY_ROW = 5;
        const QPersistentModelIndex pinned(filterModel->index(K_PINNED_PROXY_ROW, valueColumn));
        QVERIFY(pinned.isValid());
        const qint64 originalLineId = filterModel->data(pinned, LogModelItemDataRole::SortRole).toLongLong();
        QCOMPARE(originalLineId, qint64(K_PINNED_PROXY_ROW + 1));

        // Flip the source layout via SetReversed. The persistent
        // index must follow the entity (lineId K_PINNED_PROXY_ROW + 1),
        // which after reversal lives at proxy row
        // `K_ROWS - 1 - K_PINNED_PROXY_ROW`.
        rowOrderProxy->SetReversed(true);
        QCoreApplication::processEvents();
        QVERIFY(rowOrderProxy->IsReversed());

        QVERIFY(pinned.isValid());
        const qint64 lineIdAfterFlip = filterModel->data(pinned, LogModelItemDataRole::SortRole).toLongLong();
        QCOMPARE(lineIdAfterFlip, originalLineId);
        QCOMPARE(pinned.row(), K_ROWS - 1 - K_PINNED_PROXY_ROW);

        // Flip back; the persistent index returns to its original row.
        rowOrderProxy->SetReversed(false);
        QCoreApplication::processEvents();
        QVERIFY(pinned.isValid());
        QCOMPARE(pinned.row(), K_PINNED_PROXY_ROW);
        QCOMPARE(filterModel->data(pinned, LogModelItemDataRole::SortRole).toLongLong(), originalLineId);

        model->EndStreaming(false);
    }

    // Companion to `testNewestFirstReversesProxyOrder`: the user-input
    // gate on the `LogTableView` must follow the configured tail edge
    // when **Show newest lines first** is enabled. Specifically:
    //
    //   - With `TailEdge::Top`, "at tail" is `value <= minimum` (the
    //     newest row sits at proxy row 0, which is at the top of the
    //     scroll viewport).
    //   - A user scrolling down (away from the top) must edge-trigger
    //     `userScrolledAwayFromTail` so `MainWindow` disengages
    //     Follow newest.
    //   - A user scrolling back to the top must edge-trigger
    //     `userScrolledToTail` so Follow newest re-engages.
    //
    // The companion `testFollowTailIgnoresProgrammaticScrollbarChanges`
    // already covers the bottom edge; this test pins down the top edge
    // contract introduced by the newest-first feature.
    void TestTailEdgeTopFollowsScrollbarMinimum()
    {
        auto *tableView = mWindow->findChild<LogTableView *>();
        QVERIFY(tableView != nullptr);

        tableView->SetTailEdge(LogTableView::TailEdge::Top);
        QCOMPARE(tableView->GetTailEdge(), LogTableView::TailEdge::Top);

        QScrollBar *scrollBar = tableView->verticalScrollBar();
        QVERIFY(scrollBar != nullptr);
        scrollBar->setRange(0, 1000);
        scrollBar->setValue(scrollBar->minimum());

        // Programmatic value change to the middle: must not flip the
        // toggle (mirrors the bottom-edge programmatic-scroll test).
        const QSignalSpy awaySpy(tableView, &LogTableView::userScrolledAwayFromTail);
        const QSignalSpy toSpy(tableView, &LogTableView::userScrolledToTail);
        QVERIFY(awaySpy.isValid());
        QVERIFY(toSpy.isValid());

        scrollBar->setValue(scrollBar->maximum() / 2);
        QCoreApplication::processEvents();
        QCOMPARE(awaySpy.count(), 0);
        QCOMPARE(toSpy.count(), 0);

        // Reset the view to "at tail" (top) so the next user-initiated
        // event can flip the edge tracking. We bypass the user-input
        // gate by re-seeding via `SetTailEdge`, which is the production
        // path the `MainWindow` takes when `ApplyDisplayOrder`
        // re-orients the view.
        scrollBar->setValue(scrollBar->minimum());
        QCoreApplication::processEvents();
        tableView->SetTailEdge(LogTableView::TailEdge::Top);

        // Simulate a real user wheel-scroll down. `wheelEvent` on the
        // viewport sets `mNextValueChangeIsUser`, so the resulting
        // `valueChanged` *is* attributed to the user — and because
        // the tail edge is Top, leaving the minimum must emit
        // `userScrolledAwayFromTail`.
        QWheelEvent wheelDown(
            QPointF(10, 10), // position in viewport
            tableView->viewport()->mapToGlobal(QPointF(10, 10)),
            QPoint(0, -120), // pixelDelta (downward)
            QPoint(0, -120), // angleDelta (downward)
            Qt::NoButton,
            Qt::NoModifier,
            Qt::NoScrollPhase,
            false
        );
        QCoreApplication::sendEvent(tableView->viewport(), &wheelDown);
        QCoreApplication::processEvents();

        QCOMPARE(awaySpy.count(), 1);
        QCOMPARE(toSpy.count(), 0);
    }

    // Regression for the user-reported "Follow newest never auto-
    // disables when I scroll manually" bug: the previous
    // `installEventFilter`-based user-input gate ran *before* the
    // scrollbar processed the event, so direct interactions with the
    // scrollbar widget (drag, arrow / track click, Page Up / Down,
    // Home / End) saw `mUserInteractionDepth` flip back to zero
    // before `valueChanged` actually fired — and the edge transition
    // was therefore treated as programmatic, not user-initiated.
    //
    // The fix replaces the filter with a connection to
    // `QAbstractSlider::actionTriggered`, which fires synchronously
    // *before* the slider value is updated. `triggerAction(...)` is
    // the public API the same internal handlers (mouse press on the
    // arrows, key press on the slider) ultimately call, so driving
    // it directly here is the closest in-process equivalent of the
    // user clicking the scrollbar.
    void TestFollowNewestDisengagesOnScrollbarAction()
    {
        QAction *followAction = FindActionByObjectName(mWindow, QStringLiteral("actionFollowTail"));
        const auto *tableView = mWindow->findChild<LogTableView *>();
        QVERIFY(followAction != nullptr);
        QVERIFY(tableView != nullptr);

        followAction->setEnabled(true);
        followAction->setChecked(true);
        QVERIFY(followAction->isChecked());

        QScrollBar *scrollBar = tableView->verticalScrollBar();
        QVERIFY(scrollBar != nullptr);
        scrollBar->setRange(0, 1000);
        scrollBar->setValue(scrollBar->maximum());

        const QSignalSpy awaySpy(tableView, &LogTableView::userScrolledAwayFromTail);
        QVERIFY(awaySpy.isValid());

        // `triggerAction(SliderToMinimum)` mirrors a Home keypress on
        // the scrollbar: the slider emits `actionTriggered`, our slot
        // sets `mNextValueChangeIsUser`, then the action proper
        // updates the slider value and fires `valueChanged` — which
        // now sees the user flag and emits `userScrolledAwayFromTail`.
        scrollBar->triggerAction(QAbstractSlider::SliderToMinimum);
        QCoreApplication::processEvents();

        QCOMPARE(awaySpy.count(), 1);
        QVERIFY(!followAction->isChecked());
    }

    // Companion to `testFollowNewestDisengagesOnScrollbarAction`:
    // **Show newest lines first** mode preserves the user's reading
    // position when new lines arrive. Without preservation each
    // incoming batch grows the proxy's row count above the user's
    // current viewport row, the scrollbar value stays where it was,
    // and the visible content visually shifts down by the new rows'
    // height. With preservation the view records the topmost visible
    // row + its pixel offset before the structural change and
    // re-aligns the scrollbar after, so the row stays at the same
    // pixel position from the user's perspective (chat-app pattern).
    void TestNewestFirstPreservesReadingPositionAcrossBatches()
    {
        auto *rowOrderProxy = mWindow->findChild<RowOrderProxyModel *>();
        auto *tableView = mWindow->findChild<LogTableView *>();
        auto *model = mWindow->findChild<LogModel *>();
        QAction *followAction = FindActionByObjectName(mWindow, QStringLiteral("actionFollowTail"));
        QVERIFY(rowOrderProxy != nullptr);
        QVERIFY(tableView != nullptr);
        QVERIFY(model != nullptr);
        QVERIFY(followAction != nullptr);

        // The preservation hook is the **Follow newest OFF, user is
        // reading** path. In production a real scroll-away would
        // disengage Follow newest via `userScrolledAwayFromTail`; the
        // test bypasses that path by writing the scrollbar value
        // programmatically below, so we have to uncheck the action
        // up front to match the same end state.
        followAction->setEnabled(true);
        followAction->setChecked(false);

        // Force a known viewport size so `visualRect` / `indexAt`
        // produce meaningful pixel coordinates and the scrollbar
        // reaches a non-zero range below.
        tableView->resize(400, 200);
        tableView->show();
        QCoreApplication::processEvents();

        // Reverse the proxy and orient the table view's tail edge so
        // the preservation hook (which only fires for `TailEdge::Top`)
        // is in the right configuration. Mirrors what
        // `MainWindow::ApplyDisplayOrder` does in production.
        rowOrderProxy->SetReversed(true);
        tableView->SetTailEdge(LogTableView::TailEdge::Top);

        loglib::StreamLineSource &streamSource = BeginSyntheticStreamSession(*model);
        QtStreamingLogSink *sink = model->Sink();
        QVERIFY(sink != nullptr);

        loglib::KeyIndex &keys = sink->Keys();
        const loglib::KeyId valueKey = keys.GetOrInsert(std::string("value"));

        // Seed the model with enough rows that the viewport is
        // genuinely scrollable.
        sink->OnBatch(MakeSyntheticBatch(streamSource, valueKey, 1, 50, /*declareNewKey=*/true));
        QCoreApplication::processEvents();
        QCOMPARE(model->rowCount(), 50);

        QScrollBar *vbar = tableView->verticalScrollBar();
        QVERIFY(vbar != nullptr);
        if (vbar->maximum() == 0)
        {
            QSKIP("offscreen layout did not produce a scrollable viewport for the seeded rows");
        }

        // Scroll to the middle so the view is *not* at the
        // configured tail edge — preservation only kicks in here.
        const int midValue = vbar->maximum() / 2;
        vbar->setValue(midValue);
        QCoreApplication::processEvents();
        QVERIFY(vbar->value() > 0);

        // Capture the topmost visible row (mapped through both
        // proxies down to the source model) and the scrollbar value
        // at the start of the structural change.
        const QModelIndex topProxyBefore = tableView->indexAt(QPoint(0, 1));
        QVERIFY2(topProxyBefore.isValid(), "indexAt(0,1) must land inside a row");
        const int valueBefore = vbar->value();

        // Push another batch. In reversed mode the new rows land at
        // the visual top of the proxy; without preservation the
        // scrollbar value stays at `valueBefore` and the visible
        // content slides down by the new rows' total height. With
        // preservation the scrollbar advances by that same amount so
        // the previously-visible content stays put.
        sink->OnBatch(MakeSyntheticBatch(streamSource, valueKey, 51, 5, /*declareNewKey=*/false));
        QCoreApplication::processEvents();
        QCOMPARE(model->rowCount(), 55);

        const int valueAfter = vbar->value();
        QVERIFY2(
            valueAfter > valueBefore,
            qPrintable(QStringLiteral(
                           "scrollbar value should advance to compensate for the new top rows; "
                           "before=%1 after=%2"
            )
                           .arg(valueBefore)
                           .arg(valueAfter))
        );

        model->EndStreaming(false);
    }

    // Pill is hidden on a fresh idle window: no rows, no
    // scroll-away, nothing to announce. Pins the resting state
    // so a future change to the construction order is caught.
    //
    // Note: this group uses `isHidden()` instead of `!isVisible()`
    // because under the offscreen QPA the test window is never
    // mapped, so `isVisible()` is always false. `isHidden()`
    // reflects the explicit `setVisible(false)` the pill ctor
    // issues -- the production "would show if mapped" property.
    void TestJumpToTailPillHiddenOnIdle()
    {
        const auto *tableView = mWindow->findChild<LogTableView *>();
        QVERIFY(tableView != nullptr);
        const JumpToTailPill *pill = tableView->TailPillForTest();
        QVERIFY2(pill != nullptr, "log table view must construct the jump-to-tail pill");
        QVERIFY2(pill->isHidden(), "pill must be hidden on a fresh idle window");
        QCOMPARE(pill->Count(), 0);
        QCOMPARE(tableView->PendingNewRowsForTest(), 0);
    }

    // While the user is at the tail edge, inserted rows must not
    // raise the pill -- they're already current.
    void TestJumpToTailPillStaysHiddenAtTail()
    {
        auto *tableView = mWindow->findChild<LogTableView *>();
        auto *model = mWindow->findChild<LogModel *>();
        QVERIFY(tableView != nullptr);
        QVERIFY(model != nullptr);
        const JumpToTailPill *pill = tableView->TailPillForTest();
        QVERIFY(pill != nullptr);

        loglib::StreamLineSource &streamSource = BeginSyntheticStreamSession(*model);
        QtStreamingLogSink *sink = model->Sink();
        QVERIFY(sink != nullptr);
        loglib::KeyIndex &keys = sink->Keys();
        const loglib::KeyId valueKey = keys.GetOrInsert(std::string("value"));

        // Default `TailEdge::Bottom` + empty scrollbar starts us
        // at the tail edge.
        QVERIFY(tableView->verticalScrollBar()->value() >= tableView->verticalScrollBar()->maximum());

        sink->OnBatch(MakeSyntheticBatch(streamSource, valueKey, 1, 10, /*declareNewKey=*/true));
        QCoreApplication::processEvents();
        QCOMPARE(model->rowCount(), 10);

        QCOMPARE(tableView->PendingNewRowsForTest(), 0);
        QCOMPARE(pill->Count(), 0);
        QVERIFY2(pill->isHidden(), "pill must stay hidden while the user sits at the tail edge");

        model->EndStreaming(false);
    }

    // After a user scroll-away from the tail, the next batch
    // raises the pill with a matching count. Uses
    // `triggerAction(SliderToMinimum)` so the scroll-edge state
    // machine sees a user-flagged change; a plain `setValue`
    // would be filtered out as programmatic.
    void TestJumpToTailPillSurfacesAfterScrollAway()
    {
        auto *tableView = mWindow->findChild<LogTableView *>();
        auto *model = mWindow->findChild<LogModel *>();
        QVERIFY(tableView != nullptr);
        QVERIFY(model != nullptr);
        const JumpToTailPill *pill = tableView->TailPillForTest();
        QVERIFY(pill != nullptr);

        loglib::StreamLineSource &streamSource = BeginSyntheticStreamSession(*model);
        QtStreamingLogSink *sink = model->Sink();
        QVERIFY(sink != nullptr);
        loglib::KeyIndex &keys = sink->Keys();
        const loglib::KeyId valueKey = keys.GetOrInsert(std::string("value"));

        // Seed scrollable rows, then simulate a user Home press
        // to scroll away from the bottom tail.
        sink->OnBatch(MakeSyntheticBatch(streamSource, valueKey, 1, 30, /*declareNewKey=*/true));
        QCoreApplication::processEvents();

        QScrollBar *vbar = tableView->verticalScrollBar();
        vbar->setRange(0, 1000);
        vbar->setValue(vbar->maximum());
        // Re-seed the at-tail flag: the prior programmatic
        // `setValue` may have transitioned it silently. Production
        // reaches the same state via `SetTailEdge` in
        // `ApplyDisplayOrder`.
        tableView->SetTailEdge(LogTableView::TailEdge::Bottom);
        vbar->triggerAction(QAbstractSlider::SliderToMinimum);
        QCoreApplication::processEvents();

        sink->OnBatch(MakeSyntheticBatch(streamSource, valueKey, 31, 7, /*declareNewKey=*/false));
        QCoreApplication::processEvents();

        QCOMPARE(tableView->PendingNewRowsForTest(), 7);
        QCOMPARE(pill->Count(), 7);
        QVERIFY2(!pill->isHidden(), "pill must be un-hidden after a scroll-away + batch");

        model->EndStreaming(false);
    }

    // The pill count accumulates across batches: it tracks "rows
    // since you scrolled away", not "rows in the last batch".
    void TestJumpToTailPillAccumulatesAcrossBatches()
    {
        auto *tableView = mWindow->findChild<LogTableView *>();
        auto *model = mWindow->findChild<LogModel *>();
        QVERIFY(tableView != nullptr);
        QVERIFY(model != nullptr);
        const JumpToTailPill *pill = tableView->TailPillForTest();
        QVERIFY(pill != nullptr);

        loglib::StreamLineSource &streamSource = BeginSyntheticStreamSession(*model);
        QtStreamingLogSink *sink = model->Sink();
        QVERIFY(sink != nullptr);
        loglib::KeyIndex &keys = sink->Keys();
        const loglib::KeyId valueKey = keys.GetOrInsert(std::string("value"));

        sink->OnBatch(MakeSyntheticBatch(streamSource, valueKey, 1, 20, /*declareNewKey=*/true));
        QCoreApplication::processEvents();

        QScrollBar *vbar = tableView->verticalScrollBar();
        vbar->setRange(0, 1000);
        vbar->setValue(vbar->maximum());
        tableView->SetTailEdge(LogTableView::TailEdge::Bottom);
        vbar->triggerAction(QAbstractSlider::SliderToMinimum);
        QCoreApplication::processEvents();

        sink->OnBatch(MakeSyntheticBatch(streamSource, valueKey, 21, 3, /*declareNewKey=*/false));
        QCoreApplication::processEvents();
        QCOMPARE(tableView->PendingNewRowsForTest(), 3);
        QCOMPARE(pill->Count(), 3);

        sink->OnBatch(MakeSyntheticBatch(streamSource, valueKey, 24, 5, /*declareNewKey=*/false));
        QCoreApplication::processEvents();
        QCOMPARE(tableView->PendingNewRowsForTest(), 8);
        QCOMPARE(pill->Count(), 8);

        model->EndStreaming(false);
    }

    // Returning to the tail edge zeroes the counter and hides
    // the pill: a single user scroll back catches them up.
    void TestJumpToTailPillResetsOnTailReturn()
    {
        auto *tableView = mWindow->findChild<LogTableView *>();
        auto *model = mWindow->findChild<LogModel *>();
        QVERIFY(tableView != nullptr);
        QVERIFY(model != nullptr);
        const JumpToTailPill *pill = tableView->TailPillForTest();
        QVERIFY(pill != nullptr);

        loglib::StreamLineSource &streamSource = BeginSyntheticStreamSession(*model);
        QtStreamingLogSink *sink = model->Sink();
        QVERIFY(sink != nullptr);
        loglib::KeyIndex &keys = sink->Keys();
        const loglib::KeyId valueKey = keys.GetOrInsert(std::string("value"));

        sink->OnBatch(MakeSyntheticBatch(streamSource, valueKey, 1, 25, /*declareNewKey=*/true));
        QCoreApplication::processEvents();

        QScrollBar *vbar = tableView->verticalScrollBar();
        vbar->setRange(0, 1000);
        vbar->setValue(vbar->maximum());
        tableView->SetTailEdge(LogTableView::TailEdge::Bottom);
        vbar->triggerAction(QAbstractSlider::SliderToMinimum);
        QCoreApplication::processEvents();

        sink->OnBatch(MakeSyntheticBatch(streamSource, valueKey, 26, 4, /*declareNewKey=*/false));
        QCoreApplication::processEvents();
        QCOMPARE(pill->Count(), 4);

        // User returns to the tail. `triggerAction` flags the
        // change as user-initiated, so `OnVerticalScrollValueChanged`
        // emits `userScrolledToTail` and drops the pending count.
        vbar->triggerAction(QAbstractSlider::SliderToMaximum);
        QCoreApplication::processEvents();

        QCOMPARE(tableView->PendingNewRowsForTest(), 0);
        QCOMPARE(pill->Count(), 0);

        model->EndStreaming(false);
    }

    // Clicking the pill emits `jumpToTailRequested`, which the
    // host turns into a scroll back to tail (zeroing the counter)
    // and a Follow-newest re-engage. Without the re-engage the
    // next batch would push the user behind the tail again.
    void TestJumpToTailPillClickJumpsAndResets()
    {
        auto *tableView = mWindow->findChild<LogTableView *>();
        auto *model = mWindow->findChild<LogModel *>();
        QAction *followAction = FindActionByObjectName(mWindow, QStringLiteral("actionFollowTail"));
        QVERIFY(tableView != nullptr);
        QVERIFY(model != nullptr);
        QVERIFY(followAction != nullptr);
        JumpToTailPill *pill = tableView->TailPillForTest();
        QVERIFY(pill != nullptr);

        // The host slot gates the Follow re-engage on
        // `IsLiveTailSession()`; production sets the mode in
        // `OpenLogStream`, so tests must opt in explicitly.
        auto restore = qScopeGuard([this]() { mWindow->SetSessionModeForTest(MainWindow::TestSessionMode::Idle); });
        mWindow->SetSessionModeForTest(MainWindow::TestSessionMode::LiveTail);

        loglib::StreamLineSource &streamSource = BeginSyntheticStreamSession(*model);
        QtStreamingLogSink *sink = model->Sink();
        QVERIFY(sink != nullptr);
        loglib::KeyIndex &keys = sink->Keys();
        const loglib::KeyId valueKey = keys.GetOrInsert(std::string("value"));

        sink->OnBatch(MakeSyntheticBatch(streamSource, valueKey, 1, 40, /*declareNewKey=*/true));
        QCoreApplication::processEvents();

        // Pin Follow on so the scroll-away assertion below is
        // meaningful (Follow already off would mask a missing
        // re-engage).
        followAction->setEnabled(true);
        followAction->setChecked(true);

        QScrollBar *vbar = tableView->verticalScrollBar();
        vbar->setRange(0, 1000);
        vbar->setValue(vbar->maximum());
        tableView->SetTailEdge(LogTableView::TailEdge::Bottom);
        vbar->triggerAction(QAbstractSlider::SliderToMinimum);
        QCoreApplication::processEvents();

        // Sanity-check the scroll-away disengaged Follow before
        // we exercise the re-engage path.
        QVERIFY2(
            !followAction->isChecked(),
            "scroll-away must disengage Follow newest before the click test exercises re-engage"
        );

        sink->OnBatch(MakeSyntheticBatch(streamSource, valueKey, 41, 6, /*declareNewKey=*/false));
        QCoreApplication::processEvents();
        QCOMPARE(pill->Count(), 6);

        const QSignalSpy jumpSpy(tableView, &LogTableView::jumpToTailRequested);
        QVERIFY(jumpSpy.isValid());

        pill->click();
        QCoreApplication::processEvents();

        QCOMPARE(jumpSpy.count(), 1);
        // `JumpToNewestRow` scrolls back to the bottom; the
        // at-tail transition fires `ResetPendingNewRows`.
        QCOMPARE(tableView->PendingNewRowsForTest(), 0);
        QCOMPARE(pill->Count(), 0);
        // Follow newest must be re-engaged: the pill click is
        // "catch me up", not "show the tail once and then fall
        // behind again".
        QVERIFY2(followAction->isChecked(), "pill click must re-engage Follow newest in a live-tail session");

        model->EndStreaming(false);
    }

    // `SetTailEdge(Top)` must flip the pill arrow to Up so the
    // glyph reads correctly in newest-first mode.
    void TestJumpToTailPillArrowFollowsTailEdge()
    {
        auto *tableView = mWindow->findChild<LogTableView *>();
        QVERIFY(tableView != nullptr);
        const JumpToTailPill *pill = tableView->TailPillForTest();
        QVERIFY(pill != nullptr);

        QCOMPARE(tableView->GetTailEdge(), LogTableView::TailEdge::Bottom);
        QCOMPARE(pill->Direction(), JumpToTailPill::ArrowDirection::Down);

        tableView->SetTailEdge(LogTableView::TailEdge::Top);
        QCOMPARE(pill->Direction(), JumpToTailPill::ArrowDirection::Up);

        tableView->SetTailEdge(LogTableView::TailEdge::Bottom);
        QCOMPARE(pill->Direction(), JumpToTailPill::ArrowDirection::Down);
    }

    // Regression: a tail-edge flip while the user was mid-scroll
    // used to leave the pill showing the old direction's tally
    // under the new arrow ("5 rows below" becoming "5 rows
    // above"). The fix gates the reset on the orientation flip
    // itself, not just on `mAtTailEdge`.
    void TestJumpToTailPillResetsOnEdgeFlipMidScroll()
    {
        auto *tableView = mWindow->findChild<LogTableView *>();
        auto *model = mWindow->findChild<LogModel *>();
        QVERIFY(tableView != nullptr);
        QVERIFY(model != nullptr);
        const JumpToTailPill *pill = tableView->TailPillForTest();
        QVERIFY(pill != nullptr);

        loglib::StreamLineSource &streamSource = BeginSyntheticStreamSession(*model);
        QtStreamingLogSink *sink = model->Sink();
        QVERIFY(sink != nullptr);
        loglib::KeyIndex &keys = sink->Keys();
        const loglib::KeyId valueKey = keys.GetOrInsert(std::string("value"));

        sink->OnBatch(MakeSyntheticBatch(streamSource, valueKey, 1, 30, /*declareNewKey=*/true));
        QCoreApplication::processEvents();

        // Park at the (Bottom) tail, scroll away, then take a
        // batch so the pill ends up with a non-zero count under
        // Bottom orientation.
        QScrollBar *vbar = tableView->verticalScrollBar();
        vbar->setRange(0, 1000);
        vbar->setValue(vbar->maximum());
        tableView->SetTailEdge(LogTableView::TailEdge::Bottom);
        vbar->triggerAction(QAbstractSlider::SliderToMinimum);
        QCoreApplication::processEvents();

        sink->OnBatch(MakeSyntheticBatch(streamSource, valueKey, 31, 5, /*declareNewKey=*/false));
        QCoreApplication::processEvents();
        QCOMPARE(tableView->PendingNewRowsForTest(), 5);
        QCOMPARE(pill->Count(), 5);

        // Park the scrollbar in the middle so neither tail edge
        // computes as true -- the exact mid-scroll condition
        // where the old `if (mAtTailEdge)` gate would skip the
        // reset. `setRange` is needed because `SliderToMinimum`
        // clamped the range to Qt's computed layout.
        vbar->setRange(0, 1000);
        vbar->setValue(500);
        QVERIFY2(
            !tableView->AtTailEdgeForTest(),
            "precondition: mid-scroll viewport must not be at the Bottom tail edge before the flip"
        );

        tableView->SetTailEdge(LogTableView::TailEdge::Top);

        QCOMPARE(pill->Direction(), JumpToTailPill::ArrowDirection::Up);
        QVERIFY2(
            !tableView->AtTailEdgeForTest(),
            "sanity: mid-scroll viewport must not be at the Top tail edge either, so the at-tail "
            "branch of the reset gate cannot mask a missing edge-flip branch"
        );
        QCOMPARE(tableView->PendingNewRowsForTest(), 0);
        QCOMPARE(pill->Count(), 0);

        model->EndStreaming(false);
    }

    // `JumpToNewestRow` used to bail (and the pill click do
    // nothing) when the source's newest row was filtered out --
    // a common live-tail + level filter case. The fallback now
    // walks back to the first visible source row and ultimately
    // snaps to the proxy's visual tail, so the click always
    // moves the viewport.
    //
    // Setup: push values 1..30, exclude `value >= 26`, scroll
    // off tail, and check that `JumpToNewestRow` advances the
    // scrollbar (vs. the pre-fix early-return that left it put).
    void TestJumpToNewestRowFallbackToVisualTailWhenFiltered()
    {
        auto *model = mWindow->findChild<LogModel *>();
        auto *tableView = mWindow->findChild<LogTableView *>();
        LogFilterModel *filterModel = mWindow->FilterModel();
        QVERIFY(model != nullptr);
        QVERIFY(tableView != nullptr);
        QVERIFY(filterModel != nullptr);

        loglib::StreamLineSource &streamSource = BeginSyntheticStreamSession(*model);
        QtStreamingLogSink *sink = model->Sink();
        QVERIFY(sink != nullptr);
        loglib::KeyIndex &keys = sink->Keys();
        const loglib::KeyId valueKey = keys.GetOrInsert(std::string("value"));

        sink->OnBatch(MakeSyntheticBatch(streamSource, valueKey, 1, 30, /*declareNewKey=*/true));
        QCoreApplication::processEvents();
        QCOMPARE(model->rowCount(), 30);

        const int valueCol = ColumnByHeader(*model, QStringLiteral("value"));
        QVERIFY2(valueCol >= 0, "MakeSyntheticBatch must produce a 'value' column for the filter to target");

        // Exclude `value >= 26` so the source-newest row
        // (value=30) is filtered out -- the bug condition.
        {
            std::vector<loglib::RowPredicate> rules;
            rules.emplace_back(
                std::in_place_type<loglib::NumericRangeRowPredicate>,
                static_cast<size_t>(valueCol),
                std::optional<double>{},
                std::optional<double>{25.5}
            );
            filterModel->SetFilterRules(std::move(rules));
        }
        QCoreApplication::processEvents();
        QCOMPARE(model->rowCount(), 30);
        QCOMPARE(filterModel->rowCount(), 25);

        // Park the scrollbar off tail; this test exercises
        // `JumpToNewestRow`'s mapping path, not the scroll-edge
        // state machine, so a plain `setRange` + `setValue` is
        // enough.
        QScrollBar *vbar = tableView->verticalScrollBar();
        vbar->setRange(0, 1000);
        vbar->setValue(0);
        const int valueBefore = vbar->value();
        QCOMPARE(valueBefore, 0);

        mWindow->JumpToNewestRowForTest();
        QCoreApplication::processEvents();

        // Pre-fix: stays at 0 (source-newest mapping was
        // invalid). With the fallback we land near the proxy's
        // visual tail.
        QVERIFY2(
            vbar->value() > valueBefore,
            qPrintable(QStringLiteral(
                           "scrollbar must move toward the visual tail when the newest source row is "
                           "filtered out; value stayed at %1"
            )
                           .arg(vbar->value()))
        );

        model->EndStreaming(false);
    }

    // A row insert that grows `maximum` without moving `value`
    // used to leave `mAtTailEdge` stale (`OnVerticalScrollValueChanged`
    // only fires when `value` changes), so a user at the previous
    // tail would silently drift behind. The `rangeChanged`
    // listener refreshes the flag without emitting user-scroll
    // signals (a layout change must not toggle Follow newest).
    //
    // Drives `setRange` directly and observes via
    // `AtTailEdgeForTest()`: a row insert under the offscreen QPA
    // can re-call `setRange(0, 0)` during geometry and mask the
    // bug; the seam pins the moment the listener fires.
    void TestRangeChangeFreshensAtTailFlag()
    {
        auto *tableView = mWindow->findChild<LogTableView *>();
        QVERIFY(tableView != nullptr);

        QScrollBar *vbar = tableView->verticalScrollBar();
        vbar->setRange(0, 0);
        vbar->setValue(0);
        tableView->SetTailEdge(LogTableView::TailEdge::Bottom);
        QVERIFY2(tableView->AtTailEdgeForTest(), "empty range + value 0 must be at-tail after re-seed");

        const QSignalSpy awaySpy(tableView, &LogTableView::userScrolledAwayFromTail);
        const QSignalSpy toSpy(tableView, &LogTableView::userScrolledToTail);
        QVERIFY(awaySpy.isValid());
        QVERIFY(toSpy.isValid());

        // Grow the range without moving `value` -- the bug
        // condition. The listener must flip the flag to false.
        vbar->setRange(0, 500);
        QVERIFY2(
            !tableView->AtTailEdgeForTest(),
            "rangeChanged growing max must flip mAtTailEdge to false when value < newMax"
        );

        // Shrinking the range back so `value >= max` must restore
        // the flag (the rare "rows removed past value" branch).
        vbar->setRange(0, 0);
        QVERIFY2(tableView->AtTailEdgeForTest(), "rangeChanged shrinking max back below value must restore at-tail");

        // Neither transition is a user scroll, so no
        // `userScrolled*` signal must fire (a stray emission
        // would auto-toggle Follow newest).
        QCOMPARE(awaySpy.count(), 0);
        QCOMPARE(toSpy.count(), 0);
    }

    // Regression: under live-tail with Follow newest engaged,
    // signal ordering during a row insert (`rangeChanged` ->
    // `mAtTailEdge` to false -> our `OnRowsInserted` increments
    // -> `lineCountChanged` -> scroll-back -> reset) used to
    // briefly leak a count between steps 3 and 5. The fix
    // mirrors `actionFollowTail` into `mPendingNewRowsSuppressed`
    // so the increment short-circuits.
    //
    // Pin: with Follow engaged, no batch may push the pill above
    // zero regardless of signal ordering.
    void TestJumpToTailPillStaysZeroUnderFollowNewest()
    {
        auto *tableView = mWindow->findChild<LogTableView *>();
        auto *model = mWindow->findChild<LogModel *>();
        QAction *followAction = FindActionByObjectName(mWindow, QStringLiteral("actionFollowTail"));
        QVERIFY(tableView != nullptr);
        QVERIFY(model != nullptr);
        QVERIFY(followAction != nullptr);
        const JumpToTailPill *pill = tableView->TailPillForTest();
        QVERIFY(pill != nullptr);

        auto restore = qScopeGuard([this]() { mWindow->SetSessionModeForTest(MainWindow::TestSessionMode::Idle); });
        mWindow->SetSessionModeForTest(MainWindow::TestSessionMode::LiveTail);

        loglib::StreamLineSource &streamSource = BeginSyntheticStreamSession(*model);
        QtStreamingLogSink *sink = model->Sink();
        QVERIFY(sink != nullptr);
        loglib::KeyIndex &keys = sink->Keys();
        const loglib::KeyId valueKey = keys.GetOrInsert(std::string("value"));

        // Engage Follow newest and verify the suppression flag
        // followed. Suppression must be active *before* the
        // batch arrives or the window we're closing would still
        // leak a count.
        followAction->setEnabled(true);
        followAction->setChecked(true);
        QVERIFY2(
            tableView->PendingNewRowsSuppressedForTest(),
            "Follow newest engaging must mirror into the view's pending-new-rows suppression flag"
        );

        sink->OnBatch(MakeSyntheticBatch(streamSource, valueKey, 1, 25, /*declareNewKey=*/true));
        QCoreApplication::processEvents();
        QCOMPARE(model->rowCount(), 25);

        QCOMPARE(tableView->PendingNewRowsForTest(), 0);
        QCOMPARE(pill->Count(), 0);
        QVERIFY2(pill->isHidden(), "pill must stay hidden across a batch in the live-tail Follow-newest steady state");

        // A second batch must also stay silent -- the
        // suppression flag is sticky, not one-shot.
        sink->OnBatch(MakeSyntheticBatch(streamSource, valueKey, 26, 13, /*declareNewKey=*/false));
        QCoreApplication::processEvents();
        QCOMPARE(tableView->PendingNewRowsForTest(), 0);
        QCOMPARE(pill->Count(), 0);

        model->EndStreaming(false);
    }

    // Toggling Follow newest back on while scrolled away with a
    // pending tally must drain the count -- engaging suppression
    // implies the user has acknowledged the announcement.
    void TestFollowNewestEngagingClearsPendingTally()
    {
        auto *tableView = mWindow->findChild<LogTableView *>();
        auto *model = mWindow->findChild<LogModel *>();
        QAction *followAction = FindActionByObjectName(mWindow, QStringLiteral("actionFollowTail"));
        QVERIFY(tableView != nullptr);
        QVERIFY(model != nullptr);
        QVERIFY(followAction != nullptr);
        const JumpToTailPill *pill = tableView->TailPillForTest();
        QVERIFY(pill != nullptr);

        auto restore = qScopeGuard([this]() { mWindow->SetSessionModeForTest(MainWindow::TestSessionMode::Idle); });
        mWindow->SetSessionModeForTest(MainWindow::TestSessionMode::LiveTail);

        loglib::StreamLineSource &streamSource = BeginSyntheticStreamSession(*model);
        QtStreamingLogSink *sink = model->Sink();
        QVERIFY(sink != nullptr);
        loglib::KeyIndex &keys = sink->Keys();
        const loglib::KeyId valueKey = keys.GetOrInsert(std::string("value"));

        // Start with Follow off so the scroll-away + batch path
        // accumulates a tally (same path as
        // `TestJumpToTailPillSurfaces*`, just under live-tail).
        followAction->setEnabled(true);
        followAction->setChecked(false);

        sink->OnBatch(MakeSyntheticBatch(streamSource, valueKey, 1, 30, /*declareNewKey=*/true));
        QCoreApplication::processEvents();

        QScrollBar *vbar = tableView->verticalScrollBar();
        vbar->setRange(0, 1000);
        vbar->setValue(vbar->maximum());
        tableView->SetTailEdge(LogTableView::TailEdge::Bottom);
        vbar->triggerAction(QAbstractSlider::SliderToMinimum);
        QCoreApplication::processEvents();

        sink->OnBatch(MakeSyntheticBatch(streamSource, valueKey, 31, 4, /*declareNewKey=*/false));
        QCoreApplication::processEvents();
        QCOMPARE(tableView->PendingNewRowsForTest(), 4);
        QCOMPARE(pill->Count(), 4);

        // Toggle Follow back on. Engaging suppression must drain
        // the tally: "I trust auto-follow more than the catch-up
        // affordance".
        followAction->setChecked(true);

        QVERIFY2(
            tableView->PendingNewRowsSuppressedForTest(),
            "engaging Follow newest must mirror into the view's pending-new-rows suppression flag"
        );
        QCOMPARE(tableView->PendingNewRowsForTest(), 0);
        QCOMPARE(pill->Count(), 0);

        model->EndStreaming(false);
    }

    // Regression: under a custom sort the pill-click scroll can
    // land short of the visual tail (the source-newest row sits
    // in the middle of the proxy), so `mAtTailEdge` never
    // transitions and `ResetPendingNewRows` would not fire. The
    // host's `jumpToTailRequested` handler now acknowledges
    // up-front so the count clears regardless of where the
    // scroll lands.
    void TestJumpToTailPillClickAcknowledgesUnderCustomSort()
    {
        auto *tableView = mWindow->findChild<LogTableView *>();
        auto *model = mWindow->findChild<LogModel *>();
        const QAction *followAction = FindActionByObjectName(mWindow, QStringLiteral("actionFollowTail"));
        QVERIFY(tableView != nullptr);
        QVERIFY(model != nullptr);
        QVERIFY(followAction != nullptr);
        JumpToTailPill *pill = tableView->TailPillForTest();
        QVERIFY(pill != nullptr);

        // Static session keeps Follow newest out of the picture:
        // live-tail's auto re-engage would drain the tally as a
        // side effect and mask the regression.
        auto restore = qScopeGuard([this]() { mWindow->SetSessionModeForTest(MainWindow::TestSessionMode::Idle); });
        mWindow->SetSessionModeForTest(MainWindow::TestSessionMode::Static);

        loglib::StreamLineSource &streamSource = BeginSyntheticStaticSession(*model);
        QtStreamingLogSink *sink = model->Sink();
        QVERIFY(sink != nullptr);
        loglib::KeyIndex &keys = sink->Keys();
        const loglib::KeyId valueKey = keys.GetOrInsert(std::string("value"));

        sink->OnBatch(MakeSyntheticBatch(streamSource, valueKey, 1, 30, /*declareNewKey=*/true));
        QCoreApplication::processEvents();

        // Sort descending so source-newest (value=30) lands at
        // proxy row 0 -- the visual top under Bottom orientation.
        // `JumpToNewestRow`'s `scrollTo(PositionAtBottom)` then
        // can't move that row to the visual bottom, leaving the
        // scrollbar short of `maximum()`.
        const int valueCol = ColumnByHeader(*model, QStringLiteral("value"));
        QVERIFY2(valueCol >= 0, "MakeSyntheticBatch must produce a 'value' column for the sort to target");
        tableView->sortByColumn(valueCol, Qt::DescendingOrder);
        QCoreApplication::processEvents();

        // Park scrolled away and seed a pending tally via a
        // second batch. The count tracks "rows since I scrolled
        // away" regardless of where they land visually.
        QScrollBar *vbar = tableView->verticalScrollBar();
        vbar->setRange(0, 1000);
        vbar->setValue(vbar->maximum());
        tableView->SetTailEdge(LogTableView::TailEdge::Bottom);
        vbar->triggerAction(QAbstractSlider::SliderToMinimum);
        QCoreApplication::processEvents();

        sink->OnBatch(MakeSyntheticBatch(streamSource, valueKey, 31, 8, /*declareNewKey=*/false));
        QCoreApplication::processEvents();
        QVERIFY2(
            tableView->PendingNewRowsForTest() > 0,
            qPrintable(QStringLiteral("precondition: pill must have non-zero tally before the click; got %1")
                           .arg(tableView->PendingNewRowsForTest()))
        );

        // Click the pill. The host handler calls
        // `AcknowledgePendingNewRows` up-front, so the count
        // clears regardless of where the scroll lands.
        pill->click();
        QCoreApplication::processEvents();

        QCOMPARE(tableView->PendingNewRowsForTest(), 0);
        QCOMPARE(pill->Count(), 0);

        model->EndStreaming(false);
    }

    // Accessibility contract: the pill must be keyboard-reachable
    // (`Qt::TabFocus`) and its `accessibleName` must include the
    // running count so AT users hear updates (Qt's bridge prefers
    // `accessibleName` over the visible `text`). Substring match
    // on the digit keeps the assertion robust to locale wording.
    void TestJumpToTailPillAccessibilityContract()
    {
        auto *tableView = mWindow->findChild<LogTableView *>();
        auto *model = mWindow->findChild<LogModel *>();
        QVERIFY(tableView != nullptr);
        QVERIFY(model != nullptr);
        const JumpToTailPill *pill = tableView->TailPillForTest();
        QVERIFY(pill != nullptr);

        QCOMPARE(pill->focusPolicy(), Qt::TabFocus);
        QVERIFY2(
            pill->accessibleName().contains(QStringLiteral("Jump to newest row")),
            "pill accessibleName must reference the action while the count is zero"
        );

        loglib::StreamLineSource &streamSource = BeginSyntheticStreamSession(*model);
        QtStreamingLogSink *sink = model->Sink();
        QVERIFY(sink != nullptr);
        loglib::KeyIndex &keys = sink->Keys();
        const loglib::KeyId valueKey = keys.GetOrInsert(std::string("value"));

        sink->OnBatch(MakeSyntheticBatch(streamSource, valueKey, 1, 30, /*declareNewKey=*/true));
        QCoreApplication::processEvents();

        QScrollBar *vbar = tableView->verticalScrollBar();
        vbar->setRange(0, 1000);
        vbar->setValue(vbar->maximum());
        tableView->SetTailEdge(LogTableView::TailEdge::Bottom);
        vbar->triggerAction(QAbstractSlider::SliderToMinimum);
        QCoreApplication::processEvents();

        sink->OnBatch(MakeSyntheticBatch(streamSource, valueKey, 31, 7, /*declareNewKey=*/false));
        QCoreApplication::processEvents();
        QCOMPARE(pill->Count(), 7);

        // Live-update path: with a non-zero count, the
        // accessible name must include the count so screen
        // readers convey *how many* rows are pending.
        // Substring match: locales rephrase the sentence (and
        // some embed `7` differently), the contract is that
        // the digit is present in the announced name.
        QVERIFY2(
            pill->accessibleName().contains(QStringLiteral("7")),
            qPrintable(QStringLiteral("pill accessibleName must include the running count for AT users; got: %1")
                           .arg(pill->accessibleName()))
        );

        model->EndStreaming(false);
    }

    // When every row is filtered out, `JumpToNewestRow` has
    // nothing to scroll to and used to leave the pill stranded.
    // The empty-proxy branch now acknowledges so a pending tally
    // doesn't linger across the swallow.
    void TestJumpToNewestRowAcknowledgesWhenProxyEmpty()
    {
        auto *tableView = mWindow->findChild<LogTableView *>();
        auto *model = mWindow->findChild<LogModel *>();
        LogFilterModel *filterModel = mWindow->FilterModel();
        QVERIFY(tableView != nullptr);
        QVERIFY(model != nullptr);
        QVERIFY(filterModel != nullptr);
        const JumpToTailPill *pill = tableView->TailPillForTest();
        QVERIFY(pill != nullptr);

        loglib::StreamLineSource &streamSource = BeginSyntheticStreamSession(*model);
        QtStreamingLogSink *sink = model->Sink();
        QVERIFY(sink != nullptr);
        loglib::KeyIndex &keys = sink->Keys();
        const loglib::KeyId valueKey = keys.GetOrInsert(std::string("value"));

        sink->OnBatch(MakeSyntheticBatch(streamSource, valueKey, 1, 30, /*declareNewKey=*/true));
        QCoreApplication::processEvents();

        // Park scrolled away and accumulate a pending count.
        QScrollBar *vbar = tableView->verticalScrollBar();
        vbar->setRange(0, 1000);
        vbar->setValue(vbar->maximum());
        tableView->SetTailEdge(LogTableView::TailEdge::Bottom);
        vbar->triggerAction(QAbstractSlider::SliderToMinimum);
        QCoreApplication::processEvents();

        sink->OnBatch(MakeSyntheticBatch(streamSource, valueKey, 31, 4, /*declareNewKey=*/false));
        QCoreApplication::processEvents();
        QCOMPARE(pill->Count(), 4);

        // Tighten the filter to swallow every row. Pre-fix the
        // empty-proxy branch left the pill alone.
        const int valueCol = ColumnByHeader(*model, QStringLiteral("value"));
        QVERIFY2(valueCol >= 0, "MakeSyntheticBatch must produce a 'value' column");
        {
            std::vector<loglib::RowPredicate> rules;
            // Range that excludes every value (max < min).
            rules.emplace_back(
                std::in_place_type<loglib::NumericRangeRowPredicate>,
                static_cast<size_t>(valueCol),
                std::optional<double>{1000.0},
                std::optional<double>{0.0}
            );
            filterModel->SetFilterRules(std::move(rules));
        }
        QCoreApplication::processEvents();
        QCOMPARE(filterModel->rowCount(), 0);

        mWindow->JumpToNewestRowForTest();
        QCoreApplication::processEvents();

        QCOMPARE(tableView->PendingNewRowsForTest(), 0);
        QCOMPARE(pill->Count(), 0);

        model->EndStreaming(false);
    }

    // Alternating row colours stay off on the log table regardless
    // of newest-first orientation. Per-level theme colours already
    // partition rows; toggling alternation also used to flicker
    // every batch in newest-first mode. The test guards against a
    // regression that re-enables alternation.
    void TestAlternatingRowColoursDisabledInNewestFirstMode()
    {
        const auto *tableView = mWindow->findChild<LogTableView *>();
        auto *model = mWindow->findChild<LogModel *>();
        QVERIFY(tableView != nullptr);
        QVERIFY(model != nullptr);

        const bool originalNewestFirst = StreamingControl::IsNewestFirst();
        auto restoreNewestFirst = qScopeGuard([this, originalNewestFirst]() {
            StreamingControl::SetNewestFirst(originalNewestFirst);
            mWindow->SetSessionModeForTest(MainWindow::TestSessionMode::Idle);
        });

        // Stage a stream-mode session so `ApplyDisplayOrder` consults
        // `IsNewestFirst()` (and not `IsStaticNewestFirst()`).
        static_cast<void>(BeginSyntheticStreamSession(*model));
        mWindow->SetSessionModeForTest(MainWindow::TestSessionMode::LiveTail);

        StreamingControl::SetNewestFirst(false);
        mWindow->ApplyDisplayOrder();
        QVERIFY2(
            !tableView->alternatingRowColors(),
            "log table must keep alternating row colours off regardless of display order"
        );

        StreamingControl::SetNewestFirst(true);
        mWindow->ApplyDisplayOrder();
        QVERIFY2(
            !tableView->alternatingRowColors(), "log table must keep alternating row colours off in newest-first mode"
        );

        StreamingControl::SetNewestFirst(false);
        mWindow->ApplyDisplayOrder();
        QVERIFY2(
            !tableView->alternatingRowColors(),
            "log table must keep alternating row colours off after toggling newest-first back"
        );

        model->EndStreaming(false);
    }

    // JumpToAnchor visits anchored rows in ascending visible order
    // and wraps. With anchors on rows 0 and 2, the forward sequence
    // is 0 -> 2 -> 0; one step back from the wrap lands on 2.
    void TestMainWindowJumpToAnchorVisits()
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
        sink->OnBatch(MakeSyntheticBatch(streamSource, valueKey, 1, 4, true));
        QCoreApplication::processEvents();
        QCOMPARE(model->rowCount(), 4);

        const auto key0 = model->AnchorKeyForRow(0);
        const auto key2 = model->AnchorKeyForRow(2);
        QVERIFY(key0.has_value());
        QVERIFY(key2.has_value());
        anchors->SetAnchor(*key0, 0);
        anchors->SetAnchor(*key2, 1);
        QCoreApplication::processEvents();

        view->selectionModel()->clearSelection();
        view->setCurrentIndex(QModelIndex{});

        mWindow->JumpToAnchor(/*forward=*/true);
        QCoreApplication::processEvents();
        QCOMPARE(view->currentIndex().row(), 0);

        mWindow->JumpToAnchor(/*forward=*/true);
        QCoreApplication::processEvents();
        QCOMPARE(view->currentIndex().row(), 2);

        // Wrap to row 0.
        mWindow->JumpToAnchor(/*forward=*/true);
        QCoreApplication::processEvents();
        QCOMPARE(view->currentIndex().row(), 0);

        // Step back to row 2.
        mWindow->JumpToAnchor(/*forward=*/false);
        QCoreApplication::processEvents();
        QCOMPARE(view->currentIndex().row(), 2);

        model->EndStreaming(false);
    }

    // Regression pin: newest-first display reversal is a mid-
    // proxy concern; the binary search still returns the source
    // row in file order. `SelectSourceRow` handles the mapping
    // to the reversed display position.
    void TestFindFirstRowAtOrAfterHonoursNewestFirstDisplay()
    {
        const int timeCol = StreamFixtureWithTimeColumnForRowMenuTests();
        QVERIFY2(timeCol >= 0, "timestamp column must exist after streaming");
        auto *model = mWindow->Model();
        auto *rowOrder = mWindow->findChild<RowOrderProxyModel *>();
        auto *view = mWindow->findChild<LogTableView *>();
        QVERIFY(model != nullptr);
        QVERIFY(rowOrder != nullptr);
        QVERIFY(view != nullptr);
        QCOMPARE(model->rowCount(), 3);

        // Flip into newest-first mode. Save the original so we can
        // restore it (this is process-wide global state).
        const bool wasReversed = rowOrder->IsReversed();
        rowOrder->SetReversed(true);
        QCoreApplication::processEvents();
        QVERIFY(rowOrder->IsReversed());

        // Fast path must still return the earliest-in-file row that
        // qualifies (source row 1 for the middle timestamp).
        const auto *table = &model->Table();
        const auto midMicros = loglib::AsEpochMicroseconds(table->GetValue(1, static_cast<std::size_t>(timeCol)));
        QVERIFY(midMicros.has_value());
        QCOMPARE(mWindow->FindFirstRowAtOrAfter(timeCol, *midMicros), 1);

        // And `SelectSourceRow` must land the view on the reversed
        // proxy position (source row 1 in a 3-row reversed view
        // is proxy row `3 - 1 - 1 == 1`; middle stays middle).
        view->selectionModel()->clearSelection();
        view->setCurrentIndex(QModelIndex{});
        mWindow->SelectSourceRow(1);
        QCoreApplication::processEvents();
        QCOMPARE(view->currentIndex().row(), 1);

        // Source row 0 (earliest) -> reversed proxy row 2 (bottom).
        mWindow->SelectSourceRow(0);
        QCoreApplication::processEvents();
        QCOMPARE(view->currentIndex().row(), 2);

        rowOrder->SetReversed(wasReversed);
        QCoreApplication::processEvents();
    }

    // Static-mode parallel of `testAlternatingRowColoursDisabledInNewestFirstMode`:
    // when the active session is static the apply path must read the
    // **static** preference (`StreamingControl::IsStaticNewestFirst`),
    // not the stream-mode one. Toggling only the stream-mode flag must
    // be a no-op while a static session is active.
    void TestStaticNewestFirstReversesProxyOrder()
    {
        const auto *rowOrderProxy = mWindow->findChild<RowOrderProxyModel *>();
        const auto *tableView = mWindow->findChild<LogTableView *>();
        auto *model = mWindow->findChild<LogModel *>();
        QVERIFY(rowOrderProxy != nullptr);
        QVERIFY(tableView != nullptr);
        QVERIFY(model != nullptr);

        const bool originalStream = StreamingControl::IsNewestFirst();
        const bool originalStatic = StreamingControl::IsStaticNewestFirst();
        auto restore = qScopeGuard([this, originalStream, originalStatic]() {
            StreamingControl::SetNewestFirst(originalStream);
            StreamingControl::SetStaticNewestFirst(originalStatic);
            mWindow->SetSessionModeForTest(MainWindow::TestSessionMode::Idle);
        });

        BeginSyntheticStaticSession(*model);
        mWindow->SetSessionModeForTest(MainWindow::TestSessionMode::Static);

        // Stream-mode flag has no effect on a static session: with
        // the stream flag ON and the static flag OFF, the proxy
        // stays in the identity orientation. Alternation stays off
        // (per-level theme colours own the row partitioning).
        StreamingControl::SetNewestFirst(true);
        StreamingControl::SetStaticNewestFirst(false);
        mWindow->ApplyDisplayOrder();
        QVERIFY2(!rowOrderProxy->IsReversed(), "static session must ignore the stream-mode newest-first flag");
        QVERIFY(!tableView->alternatingRowColors());

        // Flipping the static-mode flag drives the same proxy reversal
        // as the stream-mode flag does for live-tail sessions.
        StreamingControl::SetStaticNewestFirst(true);
        mWindow->ApplyDisplayOrder();
        QVERIFY(rowOrderProxy->IsReversed());
        QVERIFY(!tableView->alternatingRowColors());

        StreamingControl::SetStaticNewestFirst(false);
        mWindow->ApplyDisplayOrder();
        QVERIFY(!rowOrderProxy->IsReversed());
        QVERIFY(!tableView->alternatingRowColors());

        model->EndStreaming(false);
    }

    // Regression for the user-reported "static file mode keeps
    // following the newest data" bug: while a large static file is
    // being parsed, every batch fires `LogModel::lineCountChanged`,
    // which in turn called `ScrollToNewestRowIfFollowing()` whenever
    // `mModel->IsStreamingActive()` -- a flag that's true for *both*
    // live-tail and static sessions. With `actionFollowTail` defaulting
    // to checked at startup (and on every `streamingFinished` reset),
    // a static session would silently chase the newest parsed row and
    // yank the viewport away from wherever the user had scrolled.
    //
    // The fix narrows the gate from `IsStreamingActive()` to
    // `IsLiveTailSession()` (in both `lineCountChanged` and
    // `userScrolledToTail`), and adds a defensive early-return inside
    // `ScrollToNewestRowIfFollowing()` itself so any future caller
    // of the method honours the same contract.
    //
    // The test simulates a static-mode batch arrival after the user
    // has scrolled away from the bottom and asserts the scrollbar
    // value does not change.
    void TestStaticSessionDoesNotFollowNewestRows()
    {
        auto *tableView = mWindow->findChild<LogTableView *>();
        auto *model = mWindow->findChild<LogModel *>();
        const QAction *followAction = FindActionByObjectName(mWindow, QStringLiteral("actionFollowTail"));
        QVERIFY(tableView != nullptr);
        QVERIFY(model != nullptr);
        QVERIFY(followAction != nullptr);

        auto restore = qScopeGuard([this]() { mWindow->SetSessionModeForTest(MainWindow::TestSessionMode::Idle); });

        // Static session, with `actionFollowTail` left in its startup
        // baseline (checked). Production users never see this toggle
        // in static mode (the toolbar is hidden, the action is
        // disabled), but the *value* persists across sessions and is
        // what the buggy auto-scroll path was reading.
        loglib::StreamLineSource &streamSource = BeginSyntheticStaticSession(*model);
        mWindow->SetSessionModeForTest(MainWindow::TestSessionMode::Static);
        QVERIFY(followAction->isChecked());

        QtStreamingLogSink *sink = model->Sink();
        QVERIFY(sink != nullptr);
        loglib::KeyIndex &keys = sink->Keys();
        const loglib::KeyId valueKey = keys.GetOrInsert(std::string("value"));

        // Force a known viewport size so the seeded rows actually
        // produce a scrollable range.
        tableView->resize(400, 200);
        tableView->show();
        QCoreApplication::processEvents();

        sink->OnBatch(MakeSyntheticBatch(streamSource, valueKey, 1, 200, /*declareNewKey=*/true));
        QCoreApplication::processEvents();
        QCOMPARE(model->rowCount(), 200);

        QScrollBar *vbar = tableView->verticalScrollBar();
        QVERIFY(vbar != nullptr);
        if (vbar->maximum() == 0)
        {
            QSKIP("offscreen layout did not produce a scrollable viewport for the seeded rows");
        }

        // Park the user mid-file. A live-tail session would re-yank
        // the viewport to the bottom on the next batch (see
        // `testFollowNewestDisengagesOnScrollbarAction` for the
        // production scroll-away behaviour); a static session must
        // leave the value alone.
        const int parkedValue = vbar->maximum() / 2;
        vbar->setValue(parkedValue);
        QCoreApplication::processEvents();
        QCOMPARE(vbar->value(), parkedValue);

        // Push another batch; the slot must not snap the viewport
        // to the bottom (only live-tail sessions follow).
        sink->OnBatch(MakeSyntheticBatch(streamSource, valueKey, 201, 200, /*declareNewKey=*/false));
        QCoreApplication::processEvents();
        QCOMPARE(model->rowCount(), 400);

        // Post-fix: the scrollbar's range can grow because rows were
        // appended, but the *value* must not have been programmatic-
        // ally snapped to the new maximum. Allow at most a small
        // delta from any layout / viewport recalc that genuinely
        // affected the parked row's pixel position.
        const int valueAfter = vbar->value();
        QVERIFY2(
            qAbs(valueAfter - parkedValue) <= 4,
            qPrintable(QStringLiteral(
                           "static-mode batch must not move the scrollbar; "
                           "parked=%1 after=%2 max=%3"
            )
                           .arg(parkedValue)
                           .arg(valueAfter)
                           .arg(vbar->maximum()))
        );
        QVERIFY2(
            valueAfter < vbar->maximum(),
            qPrintable(QStringLiteral(
                           "static-mode batch must not snap the scrollbar to the bottom; "
                           "after=%1 max=%2"
            )
                           .arg(valueAfter)
                           .arg(vbar->maximum()))
        );

        model->EndStreaming(false);
    }

    // Companion to `testStaticSessionDoesNotFollowNewestRows`:
    // user scroll-to-tail must not silently re-arm
    // `actionFollowTail` in a static session. The slot must gate
    // on session mode, not on `IsStreamingActive()` (which is true
    // for static sessions too).
    // partially-parsed static file would have the next incoming
    // batch yank the viewport away.
    void TestStaticSessionDoesNotReArmFollowOnScrollToTail()
    {
        auto *tableView = mWindow->findChild<LogTableView *>();
        auto *model = mWindow->findChild<LogModel *>();
        QAction *followAction = FindActionByObjectName(mWindow, QStringLiteral("actionFollowTail"));
        QVERIFY(tableView != nullptr);
        QVERIFY(model != nullptr);
        QVERIFY(followAction != nullptr);

        auto restore = qScopeGuard([this]() { mWindow->SetSessionModeForTest(MainWindow::TestSessionMode::Idle); });

        BeginSyntheticStaticSession(*model);
        mWindow->SetSessionModeForTest(MainWindow::TestSessionMode::Static);

        // Force the action *unchecked* so the re-arm path has a
        // visible state transition to attempt.
        followAction->setChecked(false);
        QVERIFY(!followAction->isChecked());

        const QSignalSpy toSpy(tableView, &LogTableView::userScrolledToTail);
        QVERIFY(toSpy.isValid());

        // Synthesise the same `userScrolledToTail` emission the
        // production path produces when the user drags the scrollbar
        // to the bottom edge. Going through `Q_EMIT` here is the
        // narrowest possible exercise of the slot we just narrowed.
        Q_EMIT tableView->userScrolledToTail();
        QCoreApplication::processEvents();

        QCOMPARE(toSpy.count(), 1);
        QVERIFY2(!followAction->isChecked(), "scroll-to-tail in static mode must not re-arm the Follow newest toggle");

        model->EndStreaming(false);
    }

    // Mode-transition guard: with both flags ON, switching from a
    // static session to a stream session must keep the proxy reversed
    // because both modes' preferences agree. Then with only the
    // *static* flag ON, the proxy must reverse on a static session and
    // de-reverse on a stream session, proving the mode dispatch in
    // `ApplyDisplayOrder` actually picks per-mode (and isn't reading
    // either flag's value unconditionally).
    void TestNewestFirstFollowsSessionModeOnTransition()
    {
        const auto *rowOrderProxy = mWindow->findChild<RowOrderProxyModel *>();
        auto *model = mWindow->findChild<LogModel *>();
        QVERIFY(rowOrderProxy != nullptr);
        QVERIFY(model != nullptr);

        const bool originalStream = StreamingControl::IsNewestFirst();
        const bool originalStatic = StreamingControl::IsStaticNewestFirst();
        auto restore = qScopeGuard([this, originalStream, originalStatic]() {
            StreamingControl::SetNewestFirst(originalStream);
            StreamingControl::SetStaticNewestFirst(originalStatic);
            mWindow->SetSessionModeForTest(MainWindow::TestSessionMode::Idle);
        });

        // Both ON: every session orientation should land reversed.
        StreamingControl::SetNewestFirst(true);
        StreamingControl::SetStaticNewestFirst(true);

        BeginSyntheticStaticSession(*model);
        mWindow->SetSessionModeForTest(MainWindow::TestSessionMode::Static);
        mWindow->ApplyDisplayOrder();
        QVERIFY(rowOrderProxy->IsReversed());

        // End the static session and start a stream one; the proxy
        // must stay reversed because the stream-mode flag is also ON.
        model->EndStreaming(false);
        static_cast<void>(BeginSyntheticStreamSession(*model));
        mWindow->SetSessionModeForTest(MainWindow::TestSessionMode::LiveTail);
        mWindow->ApplyDisplayOrder();
        QVERIFY(rowOrderProxy->IsReversed());

        // Asymmetric: only the static-mode flag is ON. The proxy must
        // de-reverse for the stream session (still active) and only
        // re-reverse once we transition back to a static session.
        StreamingControl::SetNewestFirst(false);
        mWindow->ApplyDisplayOrder();
        QVERIFY2(
            !rowOrderProxy->IsReversed(), "stream session must follow the stream-mode flag, not the static-mode one"
        );

        model->EndStreaming(false);
        BeginSyntheticStaticSession(*model);
        mWindow->SetSessionModeForTest(MainWindow::TestSessionMode::Static);
        mWindow->ApplyDisplayOrder();
        QVERIFY2(
            rowOrderProxy->IsReversed(),
            "static session must follow the static-mode flag even when the stream-mode flag is OFF"
        );

        model->EndStreaming(false);
    }

    /// Regression: an auto-promoted `Type::Level` column with
    /// non-canonical columns ahead of it must bubble to
    /// `CANONICAL_LEVEL_COLUMN_INDEX` via `columnsMoved`. The
    /// pre-fix code rotated silently inside `mLogTable.AppendBatch`,
    /// leaving Qt's column-keyed view state on the wrong section.
    void TestStreamingLevelBubbleEmitsColumnsMoved()
    {
        // Fixture: `body, scope, level`. Once `level` promotes,
        // it bubbles to index 1, pushing `scope` to index 2.
        const QStringList levels{
            QStringLiteral("info"),
            QStringLiteral("warn"),
            QStringLiteral("error"),
        };
        QStringList lines;
        lines.reserve(200);
        for (int i = 0; i < 200; ++i)
        {
            lines.append(QStringLiteral(R"({"body": "msg %1", "scope": "core", "level": "%2"})")
                             .arg(i)
                             .arg(levels[i % levels.size()]));
        }
        const TempJsonFile fixture(lines);

        auto model = std::make_unique<LogModel>(nullptr, nullptr);
        QSignalSpy columnsMovedSpy(model.get(), &QAbstractItemModel::columnsMoved);
        const QSignalSpy columnsInsertedSpy(model.get(), &QAbstractItemModel::columnsInserted);
        QSignalSpy finishedSpy(model.get(), &LogModel::streamingFinished);
        QVERIFY(columnsMovedSpy.isValid());
        QVERIFY(columnsInsertedSpy.isValid());
        QVERIFY(finishedSpy.isValid());

        auto fileSource = std::make_unique<loglib::FileLineSource>(fixture.Path().toStdString());
        loglib::FileLineSource *fileSourcePtr = fileSource.get();
        const loglib::StopToken stopToken = model->BeginStreamingForSyncTest(std::move(fileSource));
        {
            loglib::ParserOptions options;
            options.stopToken = stopToken;
            options.threads = 1;
            const loglib::JsonParser parser;
            parser.Parse(*fileSourcePtr, *model->Sink(), options);
        }
        if (finishedSpy.count() == 0)
        {
            QVERIFY2(finishedSpy.wait(60000), "streamingFinished must arrive");
        }

        // Post-bubble column layout: `body, level, scope`.
        const int levelCol = ColumnByHeader(*model, QStringLiteral("level"));
        QVERIFY2(levelCol >= 0, "level column must exist after streaming");
        QCOMPARE(levelCol, static_cast<int>(loglib::CANONICAL_LEVEL_COLUMN_INDEX));
        const auto &columns = model->Layout().Columns();
        QCOMPARE(columns.size(), static_cast<size_t>(3));
        QCOMPARE(columns[0].header, std::string{"body"});
        QCOMPARE(columns[1].header, std::string{"level"});
        QCOMPARE(columns[1].type, loglib::ColumnType::Level);
        QCOMPARE(columns[2].header, std::string{"scope"});

        // Match any single-column `columnsMoved` whose destination
        // is canonical. We don't pin the last emit because a Time
        // fixture would interleave its own bubble. Qt's
        // `destinationColumn` equals the final index for leftward
        // moves. Param `signalArgs` because `emit` is a Qt macro.
        const auto matchesLevelBubble = [](const QList<QVariant> &signalArgs) {
            const int first = signalArgs.value(1).toInt();
            const int last = signalArgs.value(2).toInt();
            const int destColumn = signalArgs.value(4).toInt();
            return first == last && first >= 0 && std::cmp_equal(destColumn, loglib::CANONICAL_LEVEL_COLUMN_INDEX);
        };
        const bool sawLevelBubble = std::any_of(columnsMovedSpy.begin(), columnsMovedSpy.end(), matchesLevelBubble);
        QVERIFY2(
            sawLevelBubble,
            qPrintable(QStringLiteral("expected a columnsMoved emit with destination=%1; got %2 emit(s)")
                           .arg(static_cast<int>(loglib::CANONICAL_LEVEL_COLUMN_INDEX))
                           .arg(columnsMovedSpy.count()))
        );

        // Sanity: rank cache survived the bubble.
        QCOMPARE(model->Table().GetLevelForRow(0, static_cast<size_t>(levelCol)).value(), loglib::LogLevel::Info);

        model->EndStreaming(false);
    }

    /// Regression: a single batch carrying both `Level` and
    /// `Time` columns must end up with `[time, level, ...]`. The
    /// Time bubble shifts the just-promoted Level out of place,
    /// and the Level bubble's re-check rebalances it.
    ///
    /// Pre-fix bug: `[time, body, level]` -- level stranded.
    /// Post-fix:    `[time, level, body]`.
    void TestStreamingLevelBubblesAfterTimeBubbleSameBatch()
    {
        // Fixture order matters: `body, level, time` so level
        // transiently lands at canonical (index 1) before the
        // Time bubble shifts it out.
        QStringList lines;
        lines.reserve(200);
        const QStringList levels{
            QStringLiteral("info"),
            QStringLiteral("warn"),
            QStringLiteral("error"),
        };
        for (int i = 0; i < 200; ++i)
        {
            lines.append(QStringLiteral(R"({"body": "msg %1", "level": "%2", "time": "2025-01-15T12:34:%3Z"})")
                             .arg(i)
                             .arg(levels[i % levels.size()])
                             .arg(i % 60, 2, 10, QLatin1Char('0')));
        }
        const TempJsonFile fixture(lines);

        const StreamingRun run = RunStreaming(fixture.Path());
        QCOMPARE(run.finishedCount, 1);
        QCOMPARE(run.cancelled, false);

        const auto &columns = run.model->Layout().Columns();
        QCOMPARE(columns.size(), static_cast<size_t>(3));
        QCOMPARE(columns[0].header, std::string{"time"});
        QCOMPARE(columns[0].type, loglib::ColumnType::Time);
        // Level rejoined canonical index 1 after the Time bubble.
        QCOMPARE(columns[1].header, std::string{"level"});
        QCOMPARE(columns[1].type, loglib::ColumnType::Level);
        QCOMPARE(columns[2].header, std::string{"body"});

        const int levelCol = ColumnByHeader(*run.model, QStringLiteral("level"));
        QCOMPARE(levelCol, static_cast<int>(loglib::CANONICAL_LEVEL_COLUMN_INDEX));
        // Sanity: rank cache survived both bubbles.
        QCOMPARE(run.model->Table().GetLevelForRow(0, static_cast<size_t>(levelCol)).value(), loglib::LogLevel::Info);
    }

    // Records every `enumColumnsChanged`, `dataChanged` and `columnsMoved`
    // signal over a scripted stream and compares the whole sequence with a
    // fixed expectation, so the order of the signals cannot drift.
    void TestStreamSignalSequenceIsStable()
    {
        auto *model = mWindow->findChild<LogModel *>();
        QVERIFY2(model != nullptr, "MainWindow must own a LogModel");

        const TempJsonFile emptyFixture(QStringList{});
        auto fileSource = std::make_unique<loglib::FileLineSource>(emptyFixture.Path().toStdString());
        loglib::FileLineSource *sourcePtr = fileSource.get();
        (void)model->BeginStreamingForSyncTest(std::move(fileSource));
        model->Table().SetEnumValueCap(3);

        QStringList recorded;
        const auto reasonName = [](EnumColumnsChangeReason reason) {
            switch (reason)
            {
            case EnumColumnsChangeReason::Grew:
                return QStringLiteral("Grew");
            case EnumColumnsChangeReason::Promoted:
                return QStringLiteral("Promoted");
            case EnumColumnsChangeReason::Demoted:
                return QStringLiteral("Demoted");
            }
            return QStringLiteral("?");
        };
        QObject context;
        connect(model, &LogModel::enumColumnsChanged, &context, [&](EnumColumnsChangeReason reason, int column) {
            QString line = QStringLiteral("enum %1 %2").arg(reasonName(reason)).arg(column);
            if (reason == EnumColumnsChangeReason::Demoted && model->LastBatchLevelDemoteMappingFor(column) != nullptr)
            {
                line += QStringLiteral(" with level mapping");
            }
            recorded.append(line);
        });
        connect(
            model,
            &QAbstractItemModel::dataChanged,
            &context,
            [&](const QModelIndex &topLeft, const QModelIndex &bottomRight, const QList<int> &roles) {
                recorded.append(QStringLiteral("data r%1-%2 c%3-%4 roles=%5")
                                    .arg(topLeft.row())
                                    .arg(bottomRight.row())
                                    .arg(topLeft.column())
                                    .arg(bottomRight.column())
                                    .arg(roles.size()));
            }
        );
        connect(
            model,
            &QAbstractItemModel::columnsMoved,
            &context,
            [&](const QModelIndex &, int start, int end, const QModelIndex &, int destination) {
                recorded.append(QStringLiteral("moved %1-%2 to %3").arg(start).arg(end).arg(destination));
            }
        );

        loglib::KeyIndex &keys = model->Table().Keys();
        using Fields = std::vector<std::pair<std::string, std::string>>;
        const auto makeBatch = [&](size_t firstLine, const std::vector<Fields> &rows) {
            loglib::RowBatch batch;
            batch.firstLineNumber = firstLine;
            for (const Fields &row : rows)
            {
                std::vector<std::pair<loglib::KeyId, loglib::LogValue>> values;
                for (const auto &[key, value] : row)
                {
                    if (keys.Find(key) == loglib::INVALID_KEY_ID)
                    {
                        batch.newKeys.push_back(key);
                    }
                    values.emplace_back(keys.GetOrInsert(key), loglib::LogValue(value));
                }
                batch.lines.emplace_back(std::move(values), *sourcePtr, 0);
            }
            return batch;
        };

        // Batch 1: "category" becomes an enum.
        model->AppendBatch(makeBatch(1, {{{"msg", "a"}, {"category", "info"}}, {{"msg", "b"}, {"category", "info"}}}));
        recorded.append(QStringLiteral("--- batch 2"));
        // Batch 2: "category" grows; "kind" arrives and is back-filled.
        model->AppendBatch(makeBatch(
            3,
            {
                {{"msg", "c"}, {"category", "warn"}, {"kind", "x"}},
                {{"msg", "d"}, {"category", "warn"}, {"kind", "x"}},
                {{"msg", "e"}, {"category", "info"}, {"kind", "x"}},
            }
        ));
        recorded.append(QStringLiteral("--- batch 3"));
        // Batch 3: a level column arrives and moves to its slot.
        model->AppendBatch(makeBatch(
            6,
            {
                {{"msg", "f"}, {"category", "info"}, {"kind", "x"}, {"level", "info"}},
                {{"msg", "g"}, {"category", "info"}, {"kind", "x"}, {"level", "warn"}},
                {{"msg", "h"}, {"category", "info"}, {"kind", "x"}, {"level", "info"}},
            }
        ));
        recorded.append(QStringLiteral("--- batch 4"));
        // Batch 4: the level column overflows the cap and demotes.
        model->AppendBatch(makeBatch(
            9,
            {
                {{"msg", "i"}, {"category", "info"}, {"kind", "x"}, {"level", "error"}},
                {{"msg", "j"}, {"category", "info"}, {"kind", "x"}, {"level", "debug"}},
                {{"msg", "k"}, {"category", "info"}, {"kind", "x"}, {"level", "trace"}},
            }
        ));
        recorded.append(QStringLiteral("--- batch 5"));
        // Batch 5: two time columns arrive in one batch.
        model->AppendBatch(makeBatch(
            12,
            {
                {{"msg", "l"}, {"timestamp", "2024-01-01T00:00:00Z"}, {"time", "2024-01-01T00:00:01Z"}},
                {{"msg", "m"}, {"timestamp", "2024-01-01T00:00:02Z"}, {"time", "2024-01-01T00:00:03Z"}},
            }
        ));
        recorded.append(QStringLiteral("--- finish"));
        model->EndStreaming(false);
        QCoreApplication::processEvents();

        const QStringList expected{
            QStringLiteral("data r0-1 c0-1 roles=3"),
            QStringLiteral("enum Promoted 0"),
            QStringLiteral("enum Promoted 1"),
            QStringLiteral("--- batch 2"),
            QStringLiteral("enum Demoted 0"),
            QStringLiteral("enum Grew 1"),
            QStringLiteral("data r0-4 c0-2 roles=3"),
            QStringLiteral("enum Promoted 2"),
            QStringLiteral("--- batch 3"),
            QStringLiteral("data r0-7 c3-3 roles=3"),
            QStringLiteral("enum Promoted 3"),
            QStringLiteral("moved 3-3 to 1"),
            QStringLiteral("--- batch 4"),
            QStringLiteral("enum Demoted 1 with level mapping"),
            QStringLiteral("data r0-10 c1-1 roles=3"),
            QStringLiteral("--- batch 5"),
            QStringLiteral("data r0-12 c4-5 roles=3"),
            QStringLiteral("moved 4-4 to 0"),
            QStringLiteral("moved 5-5 to 0"),
            QStringLiteral("--- finish"),
        };
        QCOMPARE(recorded.join(QLatin1Char('\n')), expected.join(QLatin1Char('\n')));
    }

    // Regression: an Advanced-editor commit whose parsed result is a
    // bare `Leaf` (user typed a single `service:auth`-style query in
    // the empty editor) must not be silently dropped by a subsequent
    // mirror. Before the fix, `OpenAdvancedFilter` wrote the bare
    // Leaf onto the configuration and cleared `mSimpleLeaves`. Any
    // later trigger of `SyncSessionView`
    // (`AddLogFilter`, `ClearFilter`, `AutoSaveSessionSnapshot` on
    // close) rebuilt the expression from the empty simple set, saw
    // the existing bare Leaf, matched neither the And / Or / Not
    // preservation branch, and wrote back match-all -- the user's
    // rule disappeared without warning. The fix extracts the Leaf
    // into `mSimpleLeaves` on commit so the mirror's "existing And
    // with Leaf children skipped" branch handles it uniformly.
    void TestAdvancedFilterBareLeafSurvivesFollowUpMirror()
    {
        const int levelCol = StreamFixtureForColumnTests();
        QVERIFY2(levelCol >= 0, "level column must exist after streaming");
        auto *model = mWindow->Model();
        const int msgCol = ColumnByHeader(*model, QStringLiteral("msg"));
        QVERIFY2(msgCol >= 0, "msg column must exist after streaming");
        const std::vector<std::string> msgKeys = model->Layout().Columns()[static_cast<size_t>(msgCol)].keys;

        // Simulate the user typing `msg:m1` and clicking OK. `Result`
        // is a bare `Leaf` -- the shape the pre-fix branch dropped.
        const loglib::FilterExpression bareLeaf = loglib::MakeLeaf(
            loglib::LeafRule{
                .type = loglib::LeafRule::Type::String,
                .columnKeys = msgKeys,
                .matchType = loglib::LeafRule::Match::Contains,
                .filterString = std::string("m1"),
            }
        );
        mWindow->CommitAdvancedFilterForTest(bareLeaf);
        QCoreApplication::processEvents();

        // After the commit, the leaf must be visible in the simple-mode
        // surface (Filters menu entry + `mSimpleLeaves`), and the wire
        // expression must be `And{[Leaf]}` -- not match-all.
        QCOMPARE(static_cast<int>(mWindow->Filters().size()), 1);
        {
            const auto &current = mWindow->activeSession()->View().expression;
            const auto *asAnd = std::get_if<loglib::FilterExpression::And>(&current.node);
            QVERIFY2(asAnd != nullptr, "root must be And after Advanced commit of a bare Leaf");
            QCOMPARE(static_cast<int>(asAnd->children.size()), 1);
            const auto *leaf = std::get_if<loglib::FilterExpression::Leaf>(&asAnd->children.front().node);
            QVERIFY2(leaf != nullptr, "the sole child must be the leaf we just committed");
            QVERIFY(leaf->rule.filterString.has_value());
            QCOMPARE(*leaf->rule.filterString, std::string("m1"));
        }

        // Now trigger a follow-up mirror through `FilterSubmitted`
        // (identical to any other simple-mode edit path). The
        // Advanced leaf must survive alongside the new simple leaf.
        QMetaObject::invokeMethod(
            mWindow,
            "FilterSubmitted",
            Qt::DirectConnection,
            Q_ARG(QString, QStringLiteral("second-simple")),
            Q_ARG(int, msgCol),
            Q_ARG(QString, QStringLiteral("m2")),
            Q_ARG(int, static_cast<int>(loglib::LeafRule::Match::Contains))
        );
        QCoreApplication::processEvents();

        const auto &current = mWindow->activeSession()->View().expression;
        const auto *asAnd = std::get_if<loglib::FilterExpression::And>(&current.node);
        QVERIFY2(asAnd != nullptr, "root must remain And after the follow-up simple edit");
        QCOMPARE(static_cast<int>(asAnd->children.size()), 2);
        bool sawM1 = false;
        bool sawM2 = false;
        for (const auto &child : asAnd->children)
        {
            const auto *leaf = std::get_if<loglib::FilterExpression::Leaf>(&child.node);
            QVERIFY2(leaf != nullptr, "both children must remain Leaves");
            QVERIFY(leaf->rule.filterString.has_value());
            if (*leaf->rule.filterString == "m1")
            {
                sawM1 = true;
            }
            if (*leaf->rule.filterString == "m2")
            {
                sawM2 = true;
            }
        }
        QVERIFY2(sawM1, "the Advanced-committed bare-Leaf rule must survive the mirror");
        QVERIFY2(sawM2, "the follow-up simple-mode rule must be present");
    }

    // Moving a filtered column re-aligns the funnel with the new
    // section index. Pins the `SyncColumnFilterIndicators` call
    // reached via `OnSourceColumnsMoved -> ApplyColumnVisibility`.
    void TestHeaderFilterIndicatorFollowsColumnMove()
    {
        const int levelColBefore = StreamFixtureForColumnTests();
        QVERIFY2(levelColBefore >= 0, "category column must exist after streaming");
        auto *model = mWindow->Model();
        QVERIFY2(model->columnCount() >= 2, "fixture must have at least two columns for a move");

        const int msgCol = ColumnByHeader(*model, QStringLiteral("msg"));
        QVERIFY2(msgCol >= 0, "msg column must exist after streaming");

        // Filter on `msg`; the funnel must follow the column to
        // its new section index after the move below.
        const QString filterId = QStringLiteral("hf-move");
        QMetaObject::invokeMethod(
            mWindow,
            "FilterSubmitted",
            Qt::DirectConnection,
            Q_ARG(QString, filterId),
            Q_ARG(int, msgCol),
            Q_ARG(QString, QStringLiteral("m1")),
            Q_ARG(int, static_cast<int>(loglib::LeafRule::Match::Contains))
        );
        QCoreApplication::processEvents();
        QVERIFY2(model->HasFilterForColumn(msgCol), "precondition: filter installed on msg column");

        // Move `msg` past `category` (or vice versa). The filter
        // follows the column by its keys.
        const int src = msgCol;
        const int dest = (msgCol == 0) ? model->columnCount() - 1 : 0;
        QVERIFY2(model->MoveColumn(src, dest), "MoveColumn must succeed");
        QCoreApplication::processEvents();

        const int msgColAfter = ColumnByHeader(*model, QStringLiteral("msg"));
        QVERIFY2(msgColAfter >= 0, "msg column must still exist after move");
        QVERIFY2(msgColAfter != src, "fixture sanity: msg must have actually moved sections");

        QVERIFY2(
            model->HasFilterForColumn(msgColAfter), "funnel cache must follow the moved column to its new section index"
        );
        QVERIFY2(!model->HasFilterForColumn(src), "funnel must not be left stranded at the pre-move section");

        const QVariant decoration = model->headerData(msgColAfter, Qt::Horizontal, Qt::DecorationRole);
        QVERIFY(decoration.canConvert<QIcon>());
        QVERIFY2(!decoration.value<QIcon>().isNull(), "funnel decoration must render at the post-move section index");
    }

    // `SetConfigurationUiEnabled(false)` must lock down both
    // column-management entry points (header drag + right-click
    // menu). Otherwise a drag mid-stream would race with
    // `AppendKeys` mutating `columns`. `enabled = true` restores
    // both.
    void TestStreamingDisablesColumnReorderAndContextMenu()
    {
        QVERIFY(StreamFixtureForColumnTests() >= 0);
        auto *tableView = mWindow->findChild<LogTableView *>();
        const QHeaderView *header = tableView->horizontalHeader();
        QVERIFY(header != nullptr);

        // Idle baseline: drag + custom context menu armed.
        QVERIFY2(header->sectionsMovable(), "precondition: header must allow drag in idle state");
        QCOMPARE(header->contextMenuPolicy(), Qt::CustomContextMenu);

        // Streaming-armed: gate fires on every open path.
        mWindow->SetConfigurationUiEnabledForTest(false);
        QVERIFY2(!header->sectionsMovable(), "drag must be disabled while configuration UI is gated");
        QCOMPARE(header->contextMenuPolicy(), Qt::NoContextMenu);

        // Streaming finished: the same gate flips both back on.
        mWindow->SetConfigurationUiEnabledForTest(true);
        QVERIFY2(header->sectionsMovable(), "drag must re-enable when configuration UI is ungated");
        QCOMPARE(header->contextMenuPolicy(), Qt::CustomContextMenu);
    }

    // `ParseErrorsDock::AppendErrors` must:
    //   - track Count() / DroppedCount() correctly across batches,
    //   - emit `countChanged(count, droppedCount)` on every change,
    //   - emit `firstBatchArrived()` exactly once per session (i.e.
    //     not on subsequent batches after the first).
    void TestParseErrorsDockTracksCountsAndFirstBatchSignal()
    {
        auto *dock = mWindow->findChild<ParseErrorsDock *>();
        QVERIFY2(dock != nullptr, "MainWindow must own a ParseErrorsDock");
        // `ResetSessionState` (not `ClearErrors`) re-arms the
        // first-batch latch, so this test can observe
        // `firstBatchArrived` regardless of prior test state.
        dock->ResetSessionState();

        QSignalSpy countSpy(dock, &ParseErrorsDock::countChanged);
        const QSignalSpy firstBatchSpy(dock, &ParseErrorsDock::firstBatchArrived);
        QVERIFY(countSpy.isValid());
        QVERIFY(firstBatchSpy.isValid());

        QCOMPARE(dock->Count(), 0);
        QCOMPARE(dock->DroppedCount(), 0);

        dock->AppendErrors(QStringLiteral("First batch"), {"a", "b", "c"});
        QCOMPARE(dock->Count(), 3);
        QCOMPARE(dock->DroppedCount(), 0);
        QCOMPARE(firstBatchSpy.count(), 1);
        // countChanged carries (count, droppedCount) -- two ints.
        QVERIFY2(countSpy.count() >= 1, "countChanged must fire on append");
        QCOMPARE(countSpy.last().at(0).toInt(), 3);
        QCOMPARE(countSpy.last().at(1).toInt(), 0);

        dock->AppendErrors(QStringLiteral("Second batch"), {"d", "e"});
        QCOMPARE(dock->Count(), 5);
        QCOMPARE(dock->DroppedCount(), 0);
        QCOMPARE(firstBatchSpy.count(),
                 1); // No second emit until a session boundary re-arms the latch.
        QCOMPARE(countSpy.last().at(0).toInt(), 5);

        // Clear zeroes the counters but leaves the first-batch latch
        // SET, so a streaming hiccup right after a manual Clear can't
        // re-pop the dock the user just dismissed.
        dock->ClearErrors();
        QCOMPARE(dock->Count(), 0);
        QCOMPARE(dock->DroppedCount(), 0);
        QCOMPARE(countSpy.last().at(0).toInt(), 0);
        QCOMPARE(countSpy.last().at(1).toInt(), 0);

        dock->AppendErrors(QStringLiteral("After clear"), {"x"});
        QCOMPARE(dock->Count(), 1);
        QCOMPARE(firstBatchSpy.count(), 1); // ClearErrors does NOT re-arm.

        // `ResetSessionState` is the canonical session boundary
        // and DOES re-arm the latch.
        dock->ResetSessionState();
        QCOMPARE(dock->Count(), 0);
        QCOMPARE(dock->DroppedCount(), 0);
        dock->AppendErrors(QStringLiteral("New session"), {"y"});
        QCOMPARE(dock->Count(), 1);
        QCOMPARE(firstBatchSpy.count(), 2); // Re-armed -> second emit.
    }

    // Regression: pre-fix, clicking Clear reset the
    // `firstBatchOfSession` heuristic (it keyed on the counters
    // being zero), so the next streamed batch fired
    // `firstBatchArrived` again and re-raised the dock the user
    // had just dismissed.
    void TestParseErrorsDockClearDoesNotReArmFirstBatchSignal()
    {
        auto *dock = mWindow->findChild<ParseErrorsDock *>();
        QVERIFY2(dock != nullptr, "MainWindow must own a ParseErrorsDock");
        dock->ResetSessionState();

        const QSignalSpy firstBatchSpy(dock, &ParseErrorsDock::firstBatchArrived);
        QVERIFY(firstBatchSpy.isValid());

        dock->AppendErrors(QStringLiteral("Initial batch"), {"a"});
        QCOMPARE(firstBatchSpy.count(), 1);

        dock->ClearErrors();
        QCOMPARE(dock->Count(), 0);
        QCOMPARE(dock->DroppedCount(), 0);

        // Simulated streaming hiccup after a manual Clear. The latch
        // must stay set so MainWindow does not auto-raise the dock.
        dock->AppendErrors(QStringLiteral("Post-clear hiccup"), {"b"});
        QCOMPARE(dock->Count(), 1);
        QCOMPARE(firstBatchSpy.count(), 1); // Still 1 -- Clear did NOT re-arm.

        // Inverse: a real session boundary DOES re-arm.
        dock->ResetSessionState();
        dock->AppendErrors(QStringLiteral("Real new session"), {"c"});
        QCOMPARE(firstBatchSpy.count(), 2);
    }

    // A single batch larger than the cap must pre-trim the input
    // and still surface the in-list overflow footer.
    //
    // Regression: when `mErrorCount` landed exactly at the cap
    // after pre-trim, the old `TrimToCap` short-circuited and
    // skipped appending the footer, leaving the "N dropped"
    // summary and the list view out of sync.
    void TestParseErrorsDockTrimsToCapAndAddsOverflowFooter()
    {
        auto *dock = mWindow->findChild<ParseErrorsDock *>();
        QVERIFY2(dock != nullptr, "MainWindow must own a ParseErrorsDock");
        dock->ClearErrors();

        const int cap = ParseErrorsDock::MAX_DISPLAYED_ERRORS;
        std::vector<std::string> huge;
        huge.reserve(static_cast<size_t>(cap) + 25);
        for (int i = 0; i < cap + 25; ++i)
        {
            huge.emplace_back(std::string("err ") + std::to_string(i));
        }
        dock->AppendErrors(QStringLiteral("Huge"), huge);
        QCOMPARE(dock->Count(), cap);
        QCOMPARE(dock->DroppedCount(), 25);

        auto *list = dock->findChild<QListWidget *>(QStringLiteral("parseErrorsList"));
        QVERIFY2(list != nullptr, "ParseErrorsDock must expose its internal QListWidget");

        // Footer must be the last item and the only one flagged
        // with `Qt::UserRole + 1` (so we'd also catch a stale
        // footer left somewhere in the middle).
        const QListWidgetItem *footer = nullptr;
        int footerIndex = -1;
        int footerHits = 0;
        const int footerRole = Qt::UserRole + 1;
        for (int i = 0; i < list->count(); ++i)
        {
            const QListWidgetItem *item = list->item(i);
            if (item != nullptr && item->data(footerRole).toBool())
            {
                ++footerHits;
                footer = item;
                footerIndex = i;
            }
        }
        QCOMPARE(footerHits, 1);
        QCOMPARE(footerIndex, list->count() - 1);
        QVERIFY2(footer != nullptr, "footer item lookup must succeed");
        QVERIFY2(
            footer->text().contains(QStringLiteral("dropped"), Qt::CaseInsensitive),
            qPrintable(QStringLiteral("footer must read like an overflow notice; got: %1").arg(footer->text()))
        );
        // Non-selectable so arrow-key navigation and Ctrl+A don't
        // pull the footer into the user's selection.
        QVERIFY2(!footer->flags().testFlag(Qt::ItemIsSelectable), "overflow footer must not be user-selectable");
    }

    // Streaming-style overflow: many small batches sum past the
    // cap. Exercises `TrimToCap`'s walk-from-the-top path (the
    // single-batch test above covers the pre-trim path); the
    // footer must surface here too and track `mDroppedCount`.
    void TestParseErrorsDockOverflowFooterTracksMultipleBatches()
    {
        auto *dock = mWindow->findChild<ParseErrorsDock *>();
        QVERIFY2(dock != nullptr, "MainWindow must own a ParseErrorsDock");
        dock->ClearErrors();

        auto *list = dock->findChild<QListWidget *>(QStringLiteral("parseErrorsList"));
        QVERIFY2(list != nullptr, "ParseErrorsDock must expose its internal QListWidget");
        const int footerRole = Qt::UserRole + 1;

        const int cap = ParseErrorsDock::MAX_DISPLAYED_ERRORS;
        // Fill exactly to the cap -- no overflow yet, no footer.
        std::vector<std::string> first;
        first.reserve(static_cast<size_t>(cap));
        for (int i = 0; i < cap; ++i)
        {
            first.emplace_back(std::string("a-") + std::to_string(i));
        }
        dock->AppendErrors(QStringLiteral("A"), first);
        QCOMPARE(dock->DroppedCount(), 0);
        for (int i = 0; i < list->count(); ++i)
        {
            const QListWidgetItem *item = list->item(i);
            QVERIFY2(item == nullptr || !item->data(footerRole).toBool(), "no footer when nothing has been dropped");
        }

        // Tip past the cap; walk eviction kicks in, footer surfaces.
        const int overflow = 50;
        std::vector<std::string> second;
        second.reserve(static_cast<size_t>(overflow));
        for (int i = 0; i < overflow; ++i)
        {
            second.emplace_back(std::string("b-") + std::to_string(i));
        }
        dock->AppendErrors(QStringLiteral("B"), second);
        QCOMPARE(dock->Count(), cap);
        QCOMPARE(dock->DroppedCount(), overflow);
        const QListWidgetItem *tail = list->item(list->count() - 1);
        QVERIFY2(
            tail != nullptr && tail->data(footerRole).toBool(),
            "overflow footer must be the trailing item after eviction"
        );

        // Third batch grows the drop count; footer text must update
        // and the dock must not accumulate stale footers.
        std::vector<std::string> third;
        third.reserve(static_cast<size_t>(overflow));
        for (int i = 0; i < overflow; ++i)
        {
            third.emplace_back(std::string("c-") + std::to_string(i));
        }
        dock->AppendErrors(QStringLiteral("C"), third);
        QCOMPARE(dock->DroppedCount(), overflow * 2);
        int footerHits = 0;
        const QListWidgetItem *latestFooter = nullptr;
        for (int i = 0; i < list->count(); ++i)
        {
            const QListWidgetItem *item = list->item(i);
            if (item != nullptr && item->data(footerRole).toBool())
            {
                ++footerHits;
                latestFooter = item;
            }
        }
        QCOMPARE(footerHits, 1);
        QVERIFY2(latestFooter != nullptr, "footer lookup must succeed after third batch");
        QVERIFY2(
            latestFooter->text().contains(QString::number(overflow * 2)) ||
                latestFooter->text().contains(QLocale::system().toString(static_cast<qlonglong>(overflow * 2))),
            qPrintable(QStringLiteral("footer must surface the running drop count; got: %1").arg(latestFooter->text()))
        );
    }

    // `CopySelection` must synthesise group headers and the
    // overflow footer around the selection so the pasted block
    // reads coherently. Headers / footer are non-selectable.
    void TestParseErrorsDockCopySelectionIncludesHeaderAndFooter()
    {
        auto *dock = mWindow->findChild<ParseErrorsDock *>();
        QVERIFY2(dock != nullptr, "MainWindow must own a ParseErrorsDock");
        dock->ClearErrors();

        // Two batches -> two headers; cap-overflow forces a footer.
        const int cap = ParseErrorsDock::MAX_DISPLAYED_ERRORS;
        std::vector<std::string> batchA;
        batchA.reserve(static_cast<size_t>(cap - 5));
        for (int i = 0; i < cap - 5; ++i)
        {
            batchA.emplace_back("A-" + std::to_string(i));
        }
        const std::vector<std::string> batchB{"B-0", "B-1", "B-2", "B-3", "B-4", "B-5", "B-6"};

        dock->AppendErrors(QStringLiteral("Batch A"), batchA);
        dock->AppendErrors(QStringLiteral("Batch B"), batchB);
        QVERIFY2(dock->DroppedCount() > 0, "cap overflow must mint an overflow footer");

        auto *list = dock->findChild<QListWidget *>(QStringLiteral("parseErrorsList"));
        QVERIFY2(list != nullptr, "ParseErrorsDock must expose its internal QListWidget");

        // Find Batch B's header + first error row.
        int batchBHeaderRow = -1;
        int firstBRow = -1;
        for (int i = 0; i < list->count(); ++i)
        {
            const QListWidgetItem *item = list->item(i);
            if (item == nullptr)
            {
                continue;
            }
            if (item->text() == QStringLiteral("Batch B") && !item->flags().testFlag(Qt::ItemIsSelectable))
            {
                batchBHeaderRow = i;
            }
            if (item->text().startsWith(QStringLiteral("B-")) && item->flags().testFlag(Qt::ItemIsSelectable))
            {
                if (firstBRow < 0)
                {
                    firstBRow = i;
                }
            }
        }
        QVERIFY2(batchBHeaderRow >= 0, "Batch B header must survive trimming (its rows did)");
        QVERIFY2(firstBRow > batchBHeaderRow, "Batch B's first error row must follow its header");

        list->clearSelection();
        list->item(firstBRow)->setSelected(true);

        auto *clipboard = QGuiApplication::clipboard();
        clipboard->clear();

        // `CopySelection` is private; trigger the Ctrl+C shortcut
        // instead. It's parented on the dock content widget (so it
        // stays live even when focus is on the Clear button), so
        // search the whole dock subtree.
        QShortcut *copyShortcut = nullptr;
        const QList<QShortcut *> shortcuts = dock->findChildren<QShortcut *>();
        for (QShortcut *s : shortcuts)
        {
            if (s->key() == QKeySequence(QKeySequence::Copy))
            {
                copyShortcut = s;
                break;
            }
        }
        QVERIFY2(copyShortcut != nullptr, "ParseErrorsDock must wire Ctrl+C somewhere in its subtree");
        emit copyShortcut->activated();
        QCoreApplication::processEvents();

        const QString clip = clipboard->text();
        QVERIFY2(!clip.isEmpty(), "CopySelection must populate the clipboard");
        QVERIFY2(
            clip.contains(QStringLiteral("Batch B")),
            qPrintable(QStringLiteral("clipboard must include the group header; got:\n%1").arg(clip))
        );
        QVERIFY2(
            clip.contains(QStringLiteral("B-0")),
            qPrintable(QStringLiteral("clipboard must include the selected error row; got:\n%1").arg(clip))
        );
        // Footer is `tr()`-rendered, so match the locale-stable
        // "dropped" hint rather than the full string.
        QVERIFY2(
            clip.contains(QStringLiteral("dropped"), Qt::CaseInsensitive),
            qPrintable(QStringLiteral("clipboard must include the overflow footer; got:\n%1").arg(clip))
        );
    }

    // Regression: trimming evicted the first batch's group header
    // along with some rows, leaving the surviving rows headerless
    // above the next batch's title. Fix re-mints the evicted header
    // above the survivors.
    void TestParseErrorsDockTrimPreservesSurvivingBatchHeader()
    {
        auto *dock = mWindow->findChild<ParseErrorsDock *>();
        QVERIFY2(dock != nullptr, "MainWindow must own a ParseErrorsDock");
        dock->ClearErrors();

        auto *list = dock->findChild<QListWidget *>(QStringLiteral("parseErrorsList"));
        QVERIFY2(list != nullptr, "ParseErrorsDock must expose its internal QListWidget");

        const int cap = ParseErrorsDock::MAX_DISPLAYED_ERRORS;
        // Batch A fills most of the cap; batch B tips us over,
        // evicting A's header plus a chunk of its rows.
        const int batchAStart = cap - 200;
        std::vector<std::string> batchA;
        batchA.reserve(static_cast<size_t>(batchAStart));
        for (int i = 0; i < batchAStart; ++i)
        {
            batchA.emplace_back("A-" + std::to_string(i));
        }
        std::vector<std::string> batchB;
        batchB.reserve(300);
        for (int i = 0; i < 300; ++i)
        {
            batchB.emplace_back("B-" + std::to_string(i));
        }
        dock->AppendErrors(QStringLiteral("Batch A"), batchA);
        dock->AppendErrors(QStringLiteral("Batch B"), batchB);

        QVERIFY2(dock->DroppedCount() > 0, "second batch must have evicted some of batch A");

        // First surviving row must be an `A-...` entry, with a
        // "Batch A" header immediately above it (not "Batch B"
        // floating above orphan A rows).
        int firstSelectableRow = -1;
        for (int i = 0; i < list->count(); ++i)
        {
            const QListWidgetItem *item = list->item(i);
            if (item != nullptr && item->flags().testFlag(Qt::ItemIsSelectable))
            {
                firstSelectableRow = i;
                break;
            }
        }
        QVERIFY2(firstSelectableRow > 0, "surviving error rows must follow at least one header");
        const QListWidgetItem *firstError = list->item(firstSelectableRow);
        QVERIFY2(firstError != nullptr, "first selectable row lookup must succeed");
        QVERIFY2(
            firstError->text().startsWith(QStringLiteral("A-")),
            qPrintable(QStringLiteral("first survivor must be from batch A; got: %1").arg(firstError->text()))
        );
        const QListWidgetItem *headerAbove = list->item(firstSelectableRow - 1);
        QVERIFY2(headerAbove != nullptr, "row above first survivor must exist");
        QVERIFY2(
            !headerAbove->flags().testFlag(Qt::ItemIsSelectable),
            "row above first survivor must be a group header (non-selectable)"
        );
        QCOMPARE(headerAbove->text(), QStringLiteral("Batch A"));
    }

    // Regression: `AppendErrors` used to `scrollToBottom()` on every
    // append, yanking the user back to the tail mid-read. Chat/log
    // convention: auto-follow only when already at the tail.
    void TestParseErrorsDockAutoFollowOnlyWhenAtTail()
    {
        auto *dock = mWindow->findChild<ParseErrorsDock *>();
        QVERIFY2(dock != nullptr, "MainWindow must own a ParseErrorsDock");
        dock->ClearErrors();

        auto *list = dock->findChild<QListWidget *>(QStringLiteral("parseErrorsList"));
        QVERIFY2(list != nullptr, "ParseErrorsDock must expose its internal QListWidget");

        // Realise the dock so the scrollbar has a real viewport.
        // Offscreen QPA still pumps min/max/value updates as long
        // as the widget tree is shown.
        mWindow->show();
        QVERIFY(QTest::qWaitForWindowExposed(mWindow, 60000));
        dock->show();
        dock->raise();
        QCoreApplication::processEvents();

        // Seed enough entries that the list scrolls. We start at
        // the tail (list was empty), so auto-follow pins us there.
        std::vector<std::string> seed;
        seed.reserve(200);
        for (int i = 0; i < 200; ++i)
        {
            seed.emplace_back(std::string("seed-") + std::to_string(i));
        }
        dock->AppendErrors(QStringLiteral("Seed"), seed);
        QCoreApplication::processEvents();

        QScrollBar *vBar = list->verticalScrollBar();
        QVERIFY2(vBar != nullptr, "QListWidget must have a vertical scrollbar");
        // If offscreen geometry leaves `maximum() == minimum()`
        // we can't distinguish "still at tail" from "moved by us",
        // so the test is meaningless on this platform.
        if (vBar->maximum() <= vBar->minimum())
        {
            QSKIP("offscreen QPA produced a degenerate scrollbar -- can't exercise auto-follow");
        }

        // Park mid-list -- comfortably outside the 4 px tail slack.
        const int parkedPosition = vBar->minimum() + ((vBar->maximum() - vBar->minimum()) / 2);
        vBar->setValue(parkedPosition);
        QCoreApplication::processEvents();
        QCOMPARE(vBar->value(), parkedPosition);

        // New batch mid-read must leave the scrollbar parked.
        const std::vector<std::string> later{"late-0", "late-1", "late-2", "late-3", "late-4"};
        dock->AppendErrors(QStringLiteral("Later"), later);
        QCoreApplication::processEvents();
        QVERIFY2(
            vBar->value() == parkedPosition,
            qPrintable(QStringLiteral(
                           "Scroll position must be preserved when the user was not at the tail "
                           "(parked=%1, after=%2, max=%3)"
            )
                           .arg(parkedPosition)
                           .arg(vBar->value())
                           .arg(vBar->maximum()))
        );

        // Scroll back to tail; auto-follow must re-engage.
        vBar->setValue(vBar->maximum());
        QCoreApplication::processEvents();
        QCOMPARE(vBar->value(), vBar->maximum());

        const std::vector<std::string> trailing{"tail-0", "tail-1", "tail-2"};
        dock->AppendErrors(QStringLiteral("Trailing"), trailing);
        QCoreApplication::processEvents();
        // After more items land `maximum()` grew; auto-follow
        // must have moved `value` along with it.
        QVERIFY2(
            vBar->value() >= vBar->maximum() - 4,
            qPrintable(QStringLiteral(
                           "Auto-follow must re-pin to the tail when the user was at the tail "
                           "(value=%1, max=%2)"
            )
                           .arg(vBar->value())
                           .arg(vBar->maximum()))
        );
    }

    // Regression: the status button used to total `count +
    // droppedCount`, which didn't match the dock summary ("X
    // errors; Y earlier dropped"). Now the button renders the
    // dropped count inline when non-zero.
    void TestParseErrorsStatusButtonShowsDroppedHint()
    {
        auto *dock = mWindow->findChild<ParseErrorsDock *>();
        QVERIFY2(dock != nullptr, "MainWindow must own a ParseErrorsDock");
        auto *statusButton = mWindow->findChild<QPushButton *>(QStringLiteral("parseErrorsStatusButton"));
        QVERIFY2(statusButton != nullptr, "MainWindow must own the parse-errors status button");
        dock->ClearErrors();

        // No drops yet: button uses the plain "%n errors" form.
        dock->AppendErrors(QStringLiteral("First"), {"a", "b", "c"});
        QCoreApplication::processEvents();
        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): false positive; prior `QVERIFY2` aborts on null.
        QVERIFY2(
            !statusButton->text().contains(QStringLiteral("dropped"), Qt::CaseInsensitive),
            qPrintable(QStringLiteral("status button must not mention 'dropped' when nothing was dropped: %1")
                           .arg(statusButton->text()))
        );

        // Force an overflow so `droppedCount > 0`. A batch larger
        // than the cap pre-trims to mint a dropped count.
        const int cap = ParseErrorsDock::MAX_DISPLAYED_ERRORS;
        std::vector<std::string> huge;
        huge.reserve(static_cast<size_t>(cap) + 50);
        for (int i = 0; i < cap + 50; ++i)
        {
            huge.emplace_back(std::string("err-") + std::to_string(i));
        }
        dock->AppendErrors(QStringLiteral("Huge"), huge);
        QCoreApplication::processEvents();
        QVERIFY2(dock->DroppedCount() > 0, "fixture must produce a non-zero dropped count");

        // Button must now surface the dropped hint so the user can
        // tell from the status bar that the visible total isn't
        // the whole story.
        QVERIFY2(
            statusButton->text().contains(QStringLiteral("dropped"), Qt::CaseInsensitive),
            qPrintable(QStringLiteral("status button must surface dropped count in its label; got: %1")
                           .arg(statusButton->text()))
        );
        // And the tooltip continues to spell out the breakdown.
        QVERIFY2(
            statusButton->toolTip().contains(QStringLiteral("dropped"), Qt::CaseInsensitive),
            qPrintable(QStringLiteral("status button tooltip must still spell out the breakdown; got: %1")
                           .arg(statusButton->toolTip()))
        );
    }

    // Item 9 (partial): the stream toolbar is movable and may be
    // docked to any of the four `QMainWindow` toolbar areas.
    void TestStreamToolbarIsMovable()
    {
        auto *toolbar = mWindow->findChild<QToolBar *>(QStringLiteral("streamToolbar"));
        QVERIFY2(toolbar != nullptr, "MainWindow must own the stream toolbar");
        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): prior QVERIFY2 aborts on null.
        QVERIFY2(toolbar->isMovable(), "stream toolbar must be user-movable");
        QCOMPARE(toolbar->allowedAreas(), Qt::ToolBarAreas(Qt::AllToolBarAreas));
    }

    // Item 1: `insertToolBar(mStreamToolbar, mMainToolbar)` puts the
    // primary toolbar ahead of the stream toolbar in the top dock
    // area, so the combined strip reads "Main | Stream" left to
    // right when streaming. Regressed shape would be "Stream | Main"
    // which looks broken when a live-tail session starts.
    void TestMainToolbarPrecedesStreamToolbar()
    {
        auto *mainBar = mWindow->findChild<QToolBar *>(QStringLiteral("mainToolbar"));
        auto *streamBar = mWindow->findChild<QToolBar *>(QStringLiteral("streamToolbar"));
        QVERIFY2(mainBar != nullptr, "MainWindow must own the primary toolbar");
        QVERIFY2(streamBar != nullptr, "MainWindow must own the stream toolbar");
        // NOLINTBEGIN(clang-analyzer-core.CallAndMessage): prior QVERIFY2 aborts on null.
        QCOMPARE(mWindow->toolBarArea(mainBar), Qt::TopToolBarArea);
        QCOMPARE(mWindow->toolBarArea(streamBar), Qt::TopToolBarArea);

        // Walk top-level toolbars in the window's child order; the
        // primary toolbar must appear before the stream toolbar.
        // `findChildren<QToolBar*>` returns children in the order
        // they were `addChild`-ed (which is the `insertToolBar` /
        // `addToolBar` order). The pre-existing `streamToolbar`
        // child was added first, but `insertToolBar` re-parents the
        // new bar ahead of it in the dock area, which is what
        // `QMainWindow` queries to lay them out.
        const QList<QToolBar *> bars = mWindow->findChildren<QToolBar *>(QString{}, Qt::FindDirectChildrenOnly);
        const int mainIdx = static_cast<int>(bars.indexOf(mainBar));
        const int streamIdx = static_cast<int>(bars.indexOf(streamBar));
        QVERIFY2(mainIdx >= 0, "primary toolbar must be a direct child of MainWindow");
        QVERIFY2(streamIdx >= 0, "stream toolbar must be a direct child of MainWindow");

        // X coordinates (in window space) are the most reliable
        // visual-order witness because both bars share the top
        // dock area row. The stream toolbar starts hidden, so we
        // force it visible for the geometry probe and restore
        // afterwards. Showing the window + `qWaitForWindowExposed`
        // is what makes the layout deterministic across hosts --
        // an offscreen-platform fixture that never enters the
        // event loop has been observed to leave both bars at x=0
        // until something exposes the window.
        const bool windowWasVisible = mWindow->isVisible();
        const bool streamWasVisible = streamBar->isVisible();
        streamBar->setVisible(true);
        mWindow->show();
        QVERIFY(QTest::qWaitForWindowExposed(mWindow, 60000));
        mWindow->adjustSize();
        QCoreApplication::processEvents();
        QVERIFY2(
            mainBar->x() < streamBar->x(),
            qPrintable(QStringLiteral("main toolbar (x=%1) must precede stream toolbar (x=%2)")
                           .arg(mainBar->x())
                           .arg(streamBar->x()))
        );
        streamBar->setVisible(streamWasVisible);
        if (!windowWasVisible)
        {
            mWindow->hide();
        }
        // NOLINTEND(clang-analyzer-core.CallAndMessage)
    }

    // Item 1: the "Open Stream" split button surfaces the log-stream
    // action on its primary face and the network-stream action in
    // its dropdown. Validates both the default-action wiring and the
    // popup-menu population so a `setDefaultAction` regression (no
    // icon on the button) or a missing menu entry would be caught
    // separately.
    void TestOpenStreamSplitButtonPopup()
    {
        auto *button = mWindow->findChild<QToolButton *>(QStringLiteral("openStreamSplitButton"));
        QVERIFY2(button != nullptr, "primary toolbar must host the open-stream split button");
        // NOLINTBEGIN(clang-analyzer-core.CallAndMessage): prior QVERIFY2 aborts on null.
        QCOMPARE(button->popupMode(), QToolButton::MenuButtonPopup);

        const QAction *defaultAction = button->defaultAction();
        QVERIFY2(defaultAction != nullptr, "split button must have a default action");
        QCOMPARE(defaultAction->objectName(), QStringLiteral("actionOpenLogStream"));

        const QMenu *menu = button->menu();
        QVERIFY2(menu != nullptr, "split button must have a popup menu");
        QStringList menuNames;
        for (const QAction *act : menu->actions())
        {
            menuNames.append(act != nullptr ? act->objectName() : QStringLiteral("<null>"));
        }
        QCOMPARE(
            menuNames, (QStringList{QStringLiteral("actionOpenLogStream"), QStringLiteral("actionOpenNetworkStream")})
        );
        // NOLINTEND(clang-analyzer-core.CallAndMessage)
    }

    // Regression for the split-button blank-icon bug: the split
    // button was added to the toolbar via `addWidget`, which wraps
    // it in an internal `QWidgetAction` that does NOT appear in
    // `mMainToolbar->actions()`. The first cut of `RefreshThemedIcons`
    // iterated only the toolbar's `actions()` list, so
    // `actionOpenLogStream` (tagged with `svgIconPath`) was never
    // reached; the split button's `setDefaultAction` sync therefore
    // pulled an empty icon and the button rendered as a blank box.
    // The corresponding `actionOpenNetworkStream` entry in the
    // dropdown menu had the same problem.
    //
    // We now route every themed action through `mThemedActions`
    // (anchor-driven, not toolbar-iteration-driven). Three asserts
    // pin the contract: both underlying actions carry a non-empty
    // icon, AND the button face (synced from the default action)
    // renders one too. Any future refactor that reverts to the
    // toolbar-iteration shape would lose one or more of these.
    void TestOpenStreamSplitButtonAndMenuHaveThemedIcons()
    {
        auto *button = mWindow->findChild<QToolButton *>(QStringLiteral("openStreamSplitButton"));
        auto *openLog = mWindow->findChild<QAction *>(QStringLiteral("actionOpenLogStream"));
        auto *openNet = mWindow->findChild<QAction *>(QStringLiteral("actionOpenNetworkStream"));
        QVERIFY2(button != nullptr, "primary toolbar must host the open-stream split button");
        QVERIFY2(openLog != nullptr, "actionOpenLogStream must exist");
        QVERIFY2(openNet != nullptr, "actionOpenNetworkStream must exist");
        // NOLINTBEGIN(clang-analyzer-core.CallAndMessage): prior QVERIFY2 aborts on null.
        QVERIFY2(
            !openLog->icon().isNull(),
            "actionOpenLogStream must carry a themed icon (split button's default-action icon source)"
        );
        QVERIFY2(
            !openNet->icon().isNull(),
            "actionOpenNetworkStream must carry a themed icon (popup-menu entry next to Log Stream)"
        );
        // QToolButton::setDefaultAction syncs the action's icon
        // onto the button; an empty action icon would land an
        // empty button icon. Asserting on the button face directly
        // catches the visual regression even if the sync is ever
        // re-routed.
        QVERIFY2(!button->icon().isNull(), "split button face must render a non-empty icon");
        // NOLINTEND(clang-analyzer-core.CallAndMessage)
    }

    // Companion regression for the split-button blank-icon bug:
    // it is not enough that the icons are non-null at construction.
    // The original bug also meant a Light <-> Dark flip would
    // leave the (initial-tint) icon stale while every other
    // toolbar action repainted. Pinning palette tracking on
    // `actionOpenLogStream` specifically guards against a future
    // refactor that keeps the constructor-time tint but drops the
    // refresh hook for this action.
    void TestOpenStreamActionsTrackPalette()
    {
        auto *toolbar = mWindow->findChild<QToolBar *>(QStringLiteral("mainToolbar"));
        auto *openLog = mWindow->findChild<QAction *>(QStringLiteral("actionOpenLogStream"));
        QVERIFY2(toolbar != nullptr, "MainWindow must own the primary toolbar");
        QVERIFY2(openLog != nullptr, "actionOpenLogStream must exist");

        // Match the AA-tolerant pixel walk used by
        // `TestMainToolbarIconsTrackPalette`; kept inline rather
        // than hoisted so a future cleanup that drops one test
        // doesn't leave the other one referencing a vanished helper.
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
        QPalette greenPalette = toolbar->palette();
        greenPalette.setColor(QPalette::Active, QPalette::WindowText, QColor(0, 200, 0));
        greenPalette.setColor(QPalette::Inactive, QPalette::WindowText, QColor(0, 200, 0));
        toolbar->setPalette(greenPalette);
        mWindow->setPalette(greenPalette);
        QCoreApplication::processEvents();
        QVERIFY2(
            opaquePixelCount(openLog->icon(), qRgb(0, 200, 0)) > 0,
            "actionOpenLogStream icon must repaint in the palette WindowText colour"
        );
        // NOLINTEND(clang-analyzer-core.CallAndMessage)
    }

    // Status-bar Clear-sort indicator must be hidden by default,
    // surface when a sort is installed, and hide again when the
    // sort is dropped. Mirrors the existing Clear-filters
    // status-button behaviour.
    void TestClearSortStatusButtonVisibilityFollowsSort()
    {
        const int categoryCol = StreamFixtureForColumnTests();
        QVERIFY2(categoryCol >= 0, "fixture must expose `category` column");

        auto *button = mWindow->findChild<QPushButton *>(QStringLiteral("clearSortStatusButton"));
        QVERIFY2(button != nullptr, "status bar must host the clear-sort indicator");
        // NOLINTBEGIN(clang-analyzer-core.CallAndMessage): prior QVERIFY2 aborts on null.
        QVERIFY2(button->isHidden(), "indicator must be hidden when no sort is active");

        mWindow->findChild<LogTableView *>()->sortByColumn(categoryCol, Qt::AscendingOrder);
        QCoreApplication::processEvents();
        QVERIFY2(!button->isHidden(), "indicator must surface when a sort becomes active");

        // Clicking triggers `actionClearSort`; the next
        // `layoutChanged` must hide the indicator again.
        button->click();
        QCoreApplication::processEvents();
        QCOMPARE(mWindow->FilterModel()->SortColumn(), -1);
        QVERIFY2(button->isHidden(), "indicator must hide again after a clear");
        // NOLINTEND(clang-analyzer-core.CallAndMessage)
    }

    // `actionClearSort` enable state must follow the proxy's sort
    // column. Mirrors `actionClearAllFilters`'s gating.
    void TestActionClearSortEnableStateFollowsSort()
    {
        const int categoryCol = StreamFixtureForColumnTests();
        QVERIFY2(categoryCol >= 0, "fixture must expose `category` column");

        auto *action = mWindow->findChild<QAction *>(QStringLiteral("actionClearSort"));
        QVERIFY2(action != nullptr, "actionClearSort must exist");
        // NOLINTBEGIN(clang-analyzer-core.CallAndMessage): prior QVERIFY2 aborts on null.
        QVERIFY2(!action->isEnabled(), "actionClearSort must start disabled");

        mWindow->findChild<LogTableView *>()->sortByColumn(categoryCol, Qt::DescendingOrder);
        QCoreApplication::processEvents();
        QVERIFY2(action->isEnabled(), "actionClearSort must enable while a sort is active");

        mWindow->findChild<LogTableView *>()->sortByColumn(-1, Qt::AscendingOrder);
        QCoreApplication::processEvents();
        QVERIFY2(!action->isEnabled(), "actionClearSort must disable again after a clear");
        // NOLINTEND(clang-analyzer-core.CallAndMessage)
    }

    // Item 1: `actionPauseStream` carries an alternate
    // `svgIconPathChecked` glyph so the button invites the
    // opposite transition (pause when running, play when paused),
    // matching the media-player convention every user already
    // knows. Without the On-state pixmap the icon would stay on
    // the pause glyph even while ingestion is paused, leaving the
    // user with no visual cue that another click will resume.
    void TestPauseActionSwapsIconOnCheckedState()
    {
        auto *pauseAction = mWindow->findChild<QAction *>(QStringLiteral("actionPauseStream"));
        QVERIFY2(pauseAction != nullptr, "actionPauseStream must exist");
        // NOLINTBEGIN(clang-analyzer-core.CallAndMessage): prior QVERIFY2 aborts on null.
        QVERIFY2(pauseAction->isCheckable(), "actionPauseStream must be checkable");

        const QIcon icon = pauseAction->icon();
        QVERIFY2(!icon.isNull(), "actionPauseStream must carry a themed icon");

        // `QIcon::pixmap(size, Normal, Off/On)` returns the
        // best-matching pixmap for the requested state. When the
        // On state has its own registered pixmap the two images
        // differ; if the override is missing Qt falls back to the
        // Off pixmap and the images compare equal.
        const QSize size{20, 20};
        const QImage offImage = icon.pixmap(size, QIcon::Normal, QIcon::Off).toImage();
        const QImage onImage = icon.pixmap(size, QIcon::Normal, QIcon::On).toImage();
        QVERIFY2(!offImage.isNull(), "Off-state pixmap must be available");
        QVERIFY2(!onImage.isNull(), "On-state pixmap must be available");
        QVERIFY2(
            offImage != onImage, "actionPauseStream's checked-state icon must differ from its unchecked-state icon"
        );
        // NOLINTEND(clang-analyzer-core.CallAndMessage)
    }

    // Item 1: the split button is added via `QToolBar::addWidget`,
    // which does NOT propagate the toolbar's `iconSize` /
    // `toolButtonStyle`. We mirror them explicitly in
    // `BuildMainToolbar`; this test pins that contract so a future
    // refactor that drops the explicit setters would leave the
    // split button visually off-size from every other action.
    void TestOpenStreamSplitButtonMatchesToolbarSizing()
    {
        auto *toolbar = mWindow->findChild<QToolBar *>(QStringLiteral("mainToolbar"));
        QVERIFY2(toolbar != nullptr, "MainWindow must own the primary toolbar");
        // NOLINTBEGIN(clang-analyzer-core.CallAndMessage): prior QVERIFY2 aborts on null.
        auto *button = mWindow->findChild<QToolButton *>(QStringLiteral("openStreamSplitButton"));
        QVERIFY2(button != nullptr, "primary toolbar must host the open-stream split button");
        QCOMPARE(button->iconSize(), toolbar->iconSize());
        QCOMPARE(button->toolButtonStyle(), Qt::ToolButtonIconOnly);
        // NOLINTEND(clang-analyzer-core.CallAndMessage)
    }

    // Item 1: the stream toolbar shares the top dock-area row with
    // the main toolbar when streaming, so the two must use the
    // same icon edge length and button style -- otherwise the
    // combined strip jumps from compact-icon to icon+text mid-row
    // and looks unfinished. Pinned to catch any future drift.
    void TestStreamToolbarMirrorsMainToolbarStyle()
    {
        auto *mainBar = mWindow->findChild<QToolBar *>(QStringLiteral("mainToolbar"));
        auto *streamBar = mWindow->findChild<QToolBar *>(QStringLiteral("streamToolbar"));
        QVERIFY2(mainBar != nullptr, "MainWindow must own the primary toolbar");
        QVERIFY2(streamBar != nullptr, "MainWindow must own the stream toolbar");
        // NOLINTBEGIN(clang-analyzer-core.CallAndMessage): prior QVERIFY2 aborts on null.
        QCOMPARE(streamBar->iconSize(), mainBar->iconSize());
        QCOMPARE(streamBar->toolButtonStyle(), mainBar->toolButtonStyle());
        // NOLINTEND(clang-analyzer-core.CallAndMessage)
    }

    // Regression: the find-bar arrow icons used `SP_ArrowUp` /
    // `SP_ArrowDown`, whose baked-black pixmaps disappeared on dark
    // themes. The icons must follow `QPalette::WindowText`.
    void TestFindBarArrowIconsFollowPalette()
    {
        auto *findRecord = mWindow->findChild<FindRecordWidget *>();
        QVERIFY2(findRecord != nullptr, "MainWindow must own a FindRecordWidget");
        auto *prevButton = findRecord->findChild<QToolButton *>(QStringLiteral("findPrevious"));
        auto *nextButton = findRecord->findChild<QToolButton *>(QStringLiteral("findNext"));
        QVERIFY2(prevButton != nullptr, "FindRecordWidget must expose its previous button");
        QVERIFY2(nextButton != nullptr, "FindRecordWidget must expose its next button");

        // Count opaque pixels in @p icon matching @p expected (with
        // a small per-channel tolerance for anti-alias drift). A
        // baked-black glyph never matches a non-black @p expected.
        auto opaquePixelCount = [](const QIcon &icon, QRgb expected) -> int {
            const QList<QSize> sizes = icon.availableSizes();
            const QSize size = sizes.isEmpty() ? QSize{16, 16} : sizes.front();
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
                    // Tolerate AA drift -- a pure-red apex blends to
                    // half-transparent maroon at the slope's edge.
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

        // Vivid red palette: the rendered pixmap must contain red.
        // The old baked-black icons would be black regardless of
        // `WindowText`.
        QPalette redPalette = findRecord->palette();
        redPalette.setColor(QPalette::Active, QPalette::WindowText, QColor(255, 0, 0));
        redPalette.setColor(QPalette::Inactive, QPalette::WindowText, QColor(255, 0, 0));
        findRecord->setPalette(redPalette);
        // `setPalette` posts `PaletteChange`; pump the event loop
        // so `changeEvent` rebuilds the icons before we sample.
        QCoreApplication::sendPostedEvents(findRecord, QEvent::PaletteChange);
        const int prevRed = opaquePixelCount(prevButton->icon(), qRgb(255, 0, 0));
        const int nextRed = opaquePixelCount(nextButton->icon(), qRgb(255, 0, 0));
        QVERIFY2(prevRed > 0, "previous-button icon must paint in the palette's WindowText colour");
        QVERIFY2(nextRed > 0, "next-button icon must paint in the palette's WindowText colour");

        // Flip to vivid blue: the rendered pixmap must change too.
        // The real regression is that the first Light -> Dark
        // toggle must actually re-paint the arrows.
        QPalette bluePalette = findRecord->palette();
        bluePalette.setColor(QPalette::Active, QPalette::WindowText, QColor(0, 0, 255));
        bluePalette.setColor(QPalette::Inactive, QPalette::WindowText, QColor(0, 0, 255));
        findRecord->setPalette(bluePalette);
        QCoreApplication::sendPostedEvents(findRecord, QEvent::PaletteChange);
        const int prevBlue = opaquePixelCount(prevButton->icon(), qRgb(0, 0, 255));
        const int nextBlue = opaquePixelCount(nextButton->icon(), qRgb(0, 0, 255));
        QVERIFY2(prevBlue > 0, "previous-button icon must repaint after a palette change");
        QVERIFY2(nextBlue > 0, "next-button icon must repaint after a palette change");
    }

    // Regression: every `dataChanged` invalidation used to call
    // `mMatchCountTimer->start()`, which restarted the trailing
    // timer. Under streaming the trailing timer never fired and
    // only the max-age cap (750 ms) ever delivered. Fix: when both
    // timers are already running, skip the start so the trailing
    // timer can run to its natural deadline.
    void TestBumpMatchCountDebounceCoalescesStreamingBursts()
    {
        auto *findRecord = mWindow->findChild<FindRecordWidget *>();
        QVERIFY2(findRecord != nullptr, "MainWindow must own a FindRecordWidget");
        auto *findEdit = findRecord->findChild<QLineEdit *>(QStringLiteral("findEdit"));
        QVERIFY2(findEdit != nullptr, "FindRecordWidget must expose its QLineEdit");
        // Both timers carry `objectName` so the test can probe them
        // without the fragile "walk all timers and guess" dance.
        auto *trailing = findRecord->findChild<QTimer *>(QStringLiteral("matchCountDebounceTimer"));
        auto *maxAge = findRecord->findChild<QTimer *>(QStringLiteral("matchCountMaxAgeTimer"));
        QVERIFY2(trailing != nullptr, "FindRecordWidget must name its trailing debounce timer");
        QVERIFY2(maxAge != nullptr, "FindRecordWidget must name its max-age debounce timer");

        findEdit->setText(QStringLiteral("needle"));
        // First bump arms both timers (the textChanged emit above
        // already armed them; bump is idempotent on that state).
        findRecord->BumpMatchCountDebounce();
        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): false positive; prior `QVERIFY2` aborts on null.
        QVERIFY2(trailing->isActive(), "first BumpMatchCountDebounce must arm the trailing timer");
        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): false positive; prior `QVERIFY2` aborts on null.
        QVERIFY2(maxAge->isActive(), "first BumpMatchCountDebounce must arm the max-age timer");

        // Let the trailing timer tick down, then re-bump. Remaining
        // time must NOT bounce back up -- that would mean the
        // second bump reset the timer (the bug we're pinning). The wait
        // models time passing on the timer; nothing observable marks it.
        QTest::qWait(20);
        const int remainingBefore = trailing->remainingTime();
        if (remainingBefore < 0)
        {
            QSKIP("The wait outlasted the trailing window; nothing to compare");
        }
        findRecord->BumpMatchCountDebounce();
        const int remainingAfter = trailing->remainingTime();
        // 5 ms slack for scheduler jitter; the invariant is "did
        // not jump back near `interval()`".
        QVERIFY2(
            remainingAfter <= remainingBefore + 5,
            qPrintable(
                QStringLiteral("second bump must not reset the trailing timer (before=%1, after=%2, interval=%3)")
                    .arg(remainingBefore)
                    .arg(remainingAfter)
                    .arg(trailing->interval())
            )
        );
    }

    // Regression: a streaming batch through `LogFilterModel` (no
    // sort, no filter) must emit one bracketed `rowsInserted` pair
    // covering the whole range, not one pair per source row. The
    // reverse-index rebuild lives inside the bracket so no O(n)
    // staleness window on `mSourceRowToProxyRow` is observable.
    void TestStreamingBatchEmitsSingleRowsInsertedBracket()
    {
        auto *model = mWindow->findChild<LogModel *>();
        QVERIFY2(model != nullptr, "MainWindow must own a LogModel");
        auto *filterModel = mWindow->FilterModel();
        QVERIFY2(filterModel != nullptr, "MainWindow must own a LogFilterModel");

        // Drive via direct `AppendBatch` so the batch boundary is
        // well-defined and the only `rowsInserted` emission is ours.
        const TempJsonFile emptyFixture(QStringList{});
        auto fileSource = std::make_unique<loglib::FileLineSource>(emptyFixture.Path().toStdString());
        loglib::FileLineSource *sourcePtr = fileSource.get();
        (void)model->BeginStreamingForSyncTest(std::move(fileSource));

        const QSignalSpy aboutToInsertSpy(filterModel, &QAbstractItemModel::rowsAboutToBeInserted);
        QSignalSpy insertedSpy(filterModel, &QAbstractItemModel::rowsInserted);
        QVERIFY(aboutToInsertSpy.isValid());
        QVERIFY(insertedSpy.isValid());

        constexpr int BATCH_SIZE = 100;
        loglib::KeyIndex &keys = model->Table().Keys();
        {
            loglib::RowBatch batch;
            batch.firstLineNumber = 1;
            batch.newKeys.emplace_back("msg");
            batch.lines.reserve(BATCH_SIZE);
            for (int i = 0; i < BATCH_SIZE; ++i)
            {
                std::vector<std::pair<loglib::KeyId, loglib::LogValue>> values;
                values.emplace_back(keys.GetOrInsert("msg"), loglib::LogValue(std::string("msg-") + std::to_string(i)));
                batch.lines.emplace_back(std::move(values), *sourcePtr, static_cast<size_t>(i));
            }
            model->AppendBatch(std::move(batch));
        }
        QCoreApplication::processEvents();

        QCOMPARE(filterModel->rowCount(), BATCH_SIZE);

        // One bulk bracket, not BATCH_SIZE pairs.
        QCOMPARE(aboutToInsertSpy.count(), 1);
        QCOMPARE(insertedSpy.count(), 1);

        const auto args = insertedSpy.takeFirst();
        QCOMPARE(args.at(1).toInt(), 0);
        QCOMPARE(args.at(2).toInt(), BATCH_SIZE - 1);

        // Reverse index must be consistent: `mapToSource` ->
        // `mapFromSource` round-trips for every row.
        for (int proxyRow = 0; proxyRow < BATCH_SIZE; ++proxyRow)
        {
            const QModelIndex proxyIdx = filterModel->index(proxyRow, 0);
            const QModelIndex sourceIdx = filterModel->mapToSource(proxyIdx);
            QVERIFY2(
                sourceIdx.isValid(),
                qPrintable(QStringLiteral("proxy row %1 must map to a valid source index").arg(proxyRow))
            );
            const QModelIndex roundTrip = filterModel->mapFromSource(sourceIdx);
            QVERIFY2(
                roundTrip.isValid() && roundTrip.row() == proxyRow,
                qPrintable(QStringLiteral("proxy row %1 must round-trip through mapFromSource (got %2)")
                               .arg(proxyRow)
                               .arg(roundTrip.isValid() ? roundTrip.row() : -1))
            );
        }

        model->EndStreaming(false);
    }

    void TestNumericRangeFilterUnboundedSideAcceptsTail()
    {
        constexpr int FIXTURE_LINES = 50;
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

        // Min=30, unbounded max -> 30..49 inclusive (20 rows).
        const QString filterId = QStringLiteral("numeric-range-unbounded");
        QVERIFY2(
            QMetaObject::invokeMethod(
                mWindow,
                "FilterNumericRangeSubmitted",
                Qt::DirectConnection,
                Q_ARG(QString, filterId),
                Q_ARG(int, valueCol),
                Q_ARG(std::optional<double>, std::optional<double>{30.0}),
                Q_ARG(std::optional<double>, std::optional<double>{})
            ),
            "FilterNumericRangeSubmitted slot must be invocable via meta-object"
        );
        QCoreApplication::processEvents();

        const LogFilterModel *filterModel = mWindow->FilterModel();
        QVERIFY(filterModel != nullptr);
        QCOMPARE(filterModel->rowCount(), 20);

        model->EndStreaming(false);
    }
};

QTEST_MAIN(StreamingTest)
#include "apptest_streaming_test.moc"
