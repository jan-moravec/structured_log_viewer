#pragma once

#include <loglib/streaming_log_sink.hpp>

#include <QObject>
#include <QPointer>

#include <atomic>
#include <cstdint>
#include <optional>
#include <stop_token>

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
    /// `ParserOptions::stopToken`. GUI thread.
    std::stop_token BeginParse();

    /// Non-blocking cancel; bumps generation before requesting stop so queued
    /// OnBatch calls drop on arrival. GUI thread.
    void RequestStop();

    /// The canonical KeyIndex (the model's LogTable's). Thread-safe.
    loglib::KeyIndex &Keys() override;

    void OnStarted() override;
    void OnBatch(loglib::StreamedBatch batch) override;
    void OnFinished(bool cancelled) override;

private:
    QPointer<LogModel> mModel;
    std::atomic<uint64_t> mGeneration{0};
    std::optional<std::stop_source> mStopSource;
};
