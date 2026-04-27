#pragma once

#include <loglib/streaming_log_sink.hpp>

#include <QObject>
#include <QPointer>

#include <atomic>
#include <cstdint>
#include <optional>
#include <stop_token>

class LogModel;

/**
 * @brief Qt adapter that forwards `loglib::StreamingLogSink` callbacks from
 *        a TBB worker thread to a `LogModel` on the GUI thread.
 *
 * Owns a per-parse `std::stop_source` and a generation id. `BeginParse` (GUI
 * thread) replaces both; `RequestStop` (GUI thread) bumps the generation
 * *before* requesting stop so any `OnBatch` already queued in the event
 * loop is dropped on receipt. The TBB-thread callbacks capture the live
 * generation, post to the GUI via `QMetaObject::invokeMethod(...,
 * Qt::QueuedConnection)`, and the receiver re-checks `mGeneration` and
 * silently drops on mismatch.
 *
 * The adapter does NOT own the parse thread. The owner (e.g. `MainWindow`)
 * launches `LogParser::ParseStreaming` via `QtConcurrent::run` and tracks
 * the resulting `QFuture`.
 */
class QtStreamingLogSink : public QObject, public loglib::StreamingLogSink
{
    Q_OBJECT

public:
    explicit QtStreamingLogSink(LogModel *model, QObject *parent = nullptr);
    ~QtStreamingLogSink() override = default;

    /**
     * @brief GUI-thread entry point that opens a fresh parse generation.
     *
     * Bumps `mGeneration` (so any in-flight queued events from a previous
     * parse are dropped on receipt), replaces `mStopSource` with a freshly
     * constructed `std::stop_source`, and returns its `stop_token` so the
     * caller can install it on `ParserOptions::stopToken`.
     */
    std::stop_token BeginParse();

    /**
     * @brief GUI-thread entry point that cancels the in-flight parse
     *        without blocking.
     *
     * Bumps `mGeneration` *first* (so any queued `OnBatch` calls from
     * before the cancellation are dropped) then calls `request_stop()` on
     * the owned `mStopSource`. The parser may still emit a few more
     * batches before fully shutting down; the generation check absorbs
     * them all.
     */
    void RequestStop();

    /**
     * @brief Returns the canonical KeyIndex the parser interns into.
     *
     * Routed to the model's `LogTable` so streaming workers, the table,
     * and the GUI all observe one dictionary. Safe to call from any
     * thread: the underlying `KeyIndex` is thread-safe and the `LogModel`
     * does not destroy/replace its `LogTable` while a parse is in flight.
     */
    loglib::KeyIndex &Keys() override;

    void OnStarted() override;
    void OnBatch(loglib::StreamedBatch batch) override;
    void OnFinished(bool cancelled) override;

private:
    QPointer<LogModel> mModel;
    std::atomic<uint64_t> mGeneration{0};
    std::optional<std::stop_source> mStopSource;
};
