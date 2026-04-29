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

    /// Opens a fresh parse generation; returns the stop_token to install on
    /// `ParserOptions::stopToken`. GUI thread. Renamed from `BeginParse` to
    /// emphasise that this only *arms* the sink (bumps generation, fits a
    /// fresh `StopSource`); the parser worker is started elsewhere.
    loglib::StopToken Arm();

    /// Non-blocking cancel; signals the parse's `stop_token`. Does **not**
    /// touch the generation: the worker's drain-phase `OnBatch` /
    /// `OnFinished` calls run *after* this returns, and they capture
    /// whatever generation is current at the time they run. Use
    /// `DropPendingBatches()` *after* `waitForFinished()` to invalidate the
    /// queued lambdas they emitted. GUI thread.
    void RequestStop();

    /// Bumps the sink generation so any GUI-thread lambda already queued by
    /// `OnBatch` / `OnFinished` short-circuits on its mismatch check when it
    /// later runs. Must be called *after* the parse has joined (i.e. after
    /// `QFutureWatcher::waitForFinished()` returns) so the worker has no
    /// chance to capture this new generation in a fresh queued lambda — that
    /// is the whole point of deferring the bump. GUI thread.
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
