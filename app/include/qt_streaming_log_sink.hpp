#pragma once

#include <loglib/stop_token.hpp>
#include <loglib/streaming_log_sink.hpp>

#include <QObject>
#include <QPointer>

#include <atomic>
#include <cstdint>
#include <optional>

class LogModel;

/// Qt adapter that forwards `loglib::StreamingLogSink` callbacks from a TBB
/// worker thread to a `LogModel` on the GUI thread. Generation/stop-source
/// owned by the GUI thread; worker callbacks post via QueuedConnection and
/// drop on generation mismatch. The parse thread itself is owned by the
/// caller (typically `MainWindow` via `QtConcurrent::run`).
class QtStreamingLogSink : public QObject, public loglib::StreamingLogSink
{
    Q_OBJECT

public:
    explicit QtStreamingLogSink(LogModel *model, QObject *parent = nullptr);
    ~QtStreamingLogSink() override = default;

    /// Opens a fresh parse generation and returns the stop_token to install
    /// on `ParserOptions::stopToken`. Only arms the sink; the worker is
    /// spawned elsewhere. GUI thread.
    loglib::StopToken Arm();

    /// Non-blocking cooperative cancel. Does **not** bump the generation —
    /// the worker's drain-phase `OnBatch`/`OnFinished` still need their
    /// captured generation to match. Pair with `DropPendingBatches()` after
    /// joining the worker. GUI thread.
    void RequestStop();

    /// Bumps the sink generation so already-queued GUI-thread lambdas
    /// short-circuit. Must be called **after** `waitForFinished()` returns
    /// so the worker cannot capture the new generation. GUI thread.
    void DropPendingBatches();

    /// The canonical KeyIndex (the model's LogTable's). Thread-safe.
    loglib::KeyIndex &Keys() override;

    void OnStarted() override;
    void OnBatch(loglib::StreamedBatch batch) override;
    void OnFinished(bool cancelled) override;

private:
    QPointer<LogModel> mModel;
    std::atomic<uint64_t> mGeneration{0};
    std::optional<loglib::StopSource> mStopSource;
};
