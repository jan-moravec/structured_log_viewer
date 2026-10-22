#include "commands.hpp"

#include "ansi.hpp"
#include "cli_filter.hpp"
#include "diagnostics.hpp"
#include "load_table.hpp"
#include "platform.hpp"
#include "session_config.hpp"

#include <loglib/bytes_producer.hpp>
#include <loglib/exports/export_sink.hpp>
#include <loglib/exports/row_exporter.hpp>
#include <loglib/filter_expression.hpp>
#include <loglib/leaf_rule_compile.hpp>
#include <loglib/line_source.hpp>
#include <loglib/log_configuration.hpp>
#include <loglib/log_filter.hpp>
#include <loglib/log_level.hpp>
#include <loglib/log_parse_sink.hpp>
#include <loglib/log_parser.hpp>
#include <loglib/log_table.hpp>
#include <loglib/query_parser.hpp>
#include <loglib/stdin_bytes_producer.hpp>
#include <loglib/stdin_peek.hpp>
#include <loglib/stop_token.hpp>
#include <loglib/stream_line_source.hpp>
#include <loglib/tailing_bytes_producer.hpp>
#include <loglib/tcp_server_producer.hpp>
#include <loglib/udp_server_producer.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <expected>
#include <filesystem>
#include <format>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace slp
{

namespace
{

/** @brief Rows held back while a filter or column flag waits for its column; beyond this the run fails. */
constexpr std::size_t kMaxDeferredRows = 10'000;

[[nodiscard]] CommandOutput TailFail(ExitCode code, std::string_view sentence)
{
    CommandOutput out;
    out.exitCode = static_cast<int>(code);
    out.err = FormatErrorLine("tail", sentence);
    out.err.push_back('\n');
    return out;
}

/** @brief Writes @p text to stderr at once; a long-running tail cannot wait for the final report. */
void Say(std::string_view text)
{
    std::cerr << text << std::flush;
}

enum class Rendering
{
    Jsonl,
    Text,
};

/** @brief Opens @p path for appending in binary mode, or returns null. */
[[nodiscard]] std::FILE *OpenAppend(const std::filesystem::path &path)
{
#ifdef _WIN32
    std::FILE *file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"ab") != 0)
    {
        return nullptr;
    }
    return file;
#else
    return std::fopen(path.c_str(), "ab");
#endif
}

/** @brief Output stream that closes the file it opened and leaves stdout alone. */
class OutputStream
{
public:
    OutputStream() = default;
    explicit OutputStream(std::FILE *file, bool owned) noexcept : mFile(file), mOwned(owned)
    {
    }

    ~OutputStream()
    {
        if (mOwned && mFile != nullptr)
        {
            (void)std::fclose(mFile);
        }
    }

    OutputStream(const OutputStream &) = delete;
    OutputStream &operator=(const OutputStream &) = delete;
    OutputStream(OutputStream &&) = delete;
    OutputStream &operator=(OutputStream &&) = delete;

    [[nodiscard]] std::FILE *Get() const noexcept
    {
        return mFile;
    }

private:
    std::FILE *mFile = nullptr;
    bool mOwned = false;
};

/**
 * @brief Turns external stop requests into a stop of the parse loop.
 *
 * Polls the signal token and the `--for` deadline, and when either
 * fires requests stop on the local source and stops the attached
 * producer so a parked read returns at once.
 */
class StopWatcher
{
public:
    StopWatcher(
        const loglib::StopToken &external, loglib::StopSource &local, std::optional<std::chrono::milliseconds> limit
    )
        : mExternal(external), mLocal(local), mLimit(limit), mThread([this] { Run(); })
    {
    }

    ~StopWatcher()
    {
        {
            const std::lock_guard<std::mutex> lock(mMutex);
            mDone = true;
        }
        mWake.notify_all();
        mThread.join();
    }

    StopWatcher(const StopWatcher &) = delete;
    StopWatcher &operator=(const StopWatcher &) = delete;
    StopWatcher(StopWatcher &&) = delete;
    StopWatcher &operator=(StopWatcher &&) = delete;

    /** @brief Producer to stop once a stop is requested. Must outlive the watcher. */
    void Attach(loglib::BytesProducer *producer) noexcept
    {
        mProducer.store(producer);
    }

    /** @brief Whether the `--for` deadline, not a signal or row cap, ended the run. */
    [[nodiscard]] bool TimedOut() const noexcept
    {
        return mTimedOut.load();
    }

private:
    void Run()
    {
        constexpr auto kPoll = std::chrono::milliseconds(10);
        const auto started = std::chrono::steady_clock::now();
        bool stopping = false;
        std::unique_lock<std::mutex> lock(mMutex);
        while (!mDone)
        {
            if (!stopping)
            {
                if (mLimit && std::chrono::steady_clock::now() - started >= *mLimit)
                {
                    mTimedOut.store(true);
                    stopping = true;
                }
                else if (mExternal.stop_requested() || mLocal.stop_requested())
                {
                    stopping = true;
                }
                if (stopping)
                {
                    mLocal.request_stop();
                }
            }
            if (stopping)
            {
                if (loglib::BytesProducer *producer = mProducer.load())
                {
                    producer->Stop();
                }
            }
            mWake.wait_for(lock, kPoll, [this] { return mDone; });
        }
    }

    loglib::StopToken mExternal;
    loglib::StopSource &mLocal;
    std::optional<std::chrono::milliseconds> mLimit;
    std::atomic<loglib::BytesProducer *> mProducer{nullptr};
    std::atomic<bool> mTimedOut{false};
    std::mutex mMutex;
    std::condition_variable mWake;
    bool mDone = false;
    std::thread mThread;
};

/**
 * @brief Whether @p column holds log levels.
 *
 * Streaming inference promotes a level column one batch late, so a
 * level-named key counts before its type is `Level`.
 */
[[nodiscard]] bool IsLevelColumn(const loglib::Column &column)
{
    return column.type == loglib::ColumnType::Level || std::ranges::any_of(column.keys, loglib::IsLogLevelKey);
}

/** @brief Column roles used by the one-line text rendering. */
struct TextLayout
{
    int time = -1;
    int level = -1;
    int message = -1;
    std::vector<std::size_t> rest;
};

[[nodiscard]] bool IsMessageHeader(std::string_view header) noexcept
{
    std::string lowered(header);
    std::ranges::transform(lowered, lowered.begin(), [](char ch) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    });
    return lowered == "message" || lowered == "msg" || lowered == "text" || lowered == "event";
}

[[nodiscard]] TextLayout MakeTextLayout(const loglib::LogTable &table, const std::vector<std::size_t> &visible)
{
    const auto &columns = table.Configuration().Configuration().columns;
    TextLayout layout;
    for (const std::size_t index : visible)
    {
        const loglib::Column &column = columns[index];
        if (layout.time < 0 && column.type == loglib::ColumnType::Time)
        {
            layout.time = static_cast<int>(index);
        }
        else if (layout.level < 0 && IsLevelColumn(column))
        {
            layout.level = static_cast<int>(index);
        }
        else if (layout.message < 0 && IsMessageHeader(column.header))
        {
            layout.message = static_cast<int>(index);
        }
        else
        {
            layout.rest.push_back(index);
        }
    }
    return layout;
}

/** @brief Replaces line breaks so one row stays on one output line. */
[[nodiscard]] std::string SingleLine(std::string text)
{
    std::string out;
    out.reserve(text.size());
    for (const char ch : text)
    {
        if (ch == '\n')
        {
            out += "\\n";
        }
        else if (ch == '\r')
        {
            out += "\\r";
        }
        else
        {
            out.push_back(ch);
        }
    }
    return out;
}

[[nodiscard]] std::string QuoteIfNeeded(std::string value)
{
    const bool plain = !value.empty() && std::ranges::none_of(value, [](char ch) {
        return ch == ' ' || ch == '=' || ch == '"' || ch == '\t';
    });
    if (plain)
    {
        return value;
    }
    std::string quoted = "\"";
    for (const char ch : value)
    {
        if (ch == '"' || ch == '\\')
        {
            quoted.push_back('\\');
        }
        quoted.push_back(ch);
    }
    quoted.push_back('"');
    return quoted;
}

void AppendField(std::string &line, std::string_view text)
{
    if (text.empty())
    {
        return;
    }
    if (!line.empty())
    {
        line.push_back(' ');
    }
    line.append(text);
}

[[nodiscard]] std::string RenderTextLine(
    const loglib::LogTable &table, std::size_t row, const TextLayout &layout, bool color
)
{
    const auto &columns = table.Configuration().Configuration().columns;
    std::string line;
    if (layout.time >= 0)
    {
        AppendField(line, SingleLine(table.GetFormattedValue(row, static_cast<std::size_t>(layout.time))));
    }
    if (layout.level >= 0)
    {
        const auto column = static_cast<std::size_t>(layout.level);
        const std::string value = SingleLine(table.GetFormattedValue(row, column));
        if (color && !value.empty())
        {
            const auto level = loglib::ParseLevelName(value).value_or(loglib::LogLevel::Unknown);
            const std::string_view prefix = LevelAnsiPrefix(level);
            std::string styled;
            if (!prefix.empty())
            {
                styled.append(prefix);
                styled.append(value);
                styled.append(kAnsiReset);
            }
            else
            {
                styled = value;
            }
            AppendField(line, styled);
        }
        else
        {
            AppendField(line, value);
        }
    }
    if (layout.message >= 0)
    {
        AppendField(line, SingleLine(table.GetFormattedValue(row, static_cast<std::size_t>(layout.message))));
    }
    for (const std::size_t index : layout.rest)
    {
        const std::string value = SingleLine(table.GetFormattedValue(row, index));
        if (value.empty())
        {
            continue;
        }
        AppendField(line, columns[index].header + "=" + QuoteIfNeeded(value));
    }
    line.push_back('\n');
    return line;
}

/**
 * @brief Filters, renders, and retains each batch as it arrives.
 *
 * Rows are emitted before they are evicted, so retention bounds memory
 * without dropping output. A filter or column flag whose column has not
 * appeared yet holds its rows back; the run fails with that flag's
 * message once the stream ends or too many rows are waiting.
 */
class TailSink final : public loglib::LogParseSink
{
public:
    TailSink(
        const Options &options,
        const RuntimeContext &runtime,
        loglib::LogTable &table,
        std::FILE *output,
        Rendering rendering,
        bool color,
        std::size_t retentionCap,
        loglib::StopSource &stop
    )
        : mOptions(options),
          mRuntime(runtime),
          mTable(table),
          mOutput(output),
          mRendering(rendering),
          mColor(color),
          mRetentionCap(retentionCap),
          mStop(stop),
          mSessionOptions(options)
    {
        mSessionOptions.minLevel.reset();
        if (options.minLevel)
        {
            mMinLevel = loglib::ParseLevelName(*options.minLevel);
        }
    }

    loglib::KeyIndex &Keys() override
    {
        return mTable.Keys();
    }

    void OnStarted() override
    {
    }

    void OnBatch(loglib::StreamedBatch batch) override
    {
        if (mFailure || mDone)
        {
            return;
        }
        if (!batch.errors.empty())
        {
            mErrors.insert(
                mErrors.end(), std::make_move_iterator(batch.errors.begin()), std::make_move_iterator(batch.errors.end())
            );
            batch.errors.clear();
        }
        AppendStreamedBatch(mTable, std::move(batch));
        Process(false);
        Evict();
    }

    void OnFinished(bool /*cancelled*/) override
    {
    }

    /** @brief Emits rows still held back, after the stream has ended. */
    void Finish()
    {
        if (mFailure || mDone)
        {
            return;
        }
        if (mNextRow < mTable.RowCount())
        {
            (void)mTable.FinalizeAutoDetection();
            mTable.ApplyPendingLevelBubbles();
            Process(true);
        }
    }

    [[nodiscard]] const std::optional<CommandOutput> &Failure() const noexcept
    {
        return mFailure;
    }

    [[nodiscard]] const std::vector<std::string> &ParseErrors() const noexcept
    {
        return mErrors;
    }

    [[nodiscard]] std::uint64_t Emitted() const noexcept
    {
        return mEmitted;
    }

private:
    /** @brief Resolves the session when the column set changed. Returns whether it is usable. */
    [[nodiscard]] bool EnsureSession()
    {
        const auto &columns = mTable.Configuration().Configuration().columns;
        std::vector<std::string> headers;
        headers.reserve(columns.size());
        for (const loglib::Column &column : columns)
        {
            headers.push_back(column.header);
        }
        if (mSession && headers == mHeaders)
        {
            return true;
        }
        auto resolved = ApplySessionConfiguration(mSessionOptions, mTable, mRuntime, "tail");
        if (!resolved)
        {
            mSessionError = std::move(resolved.error());
            mSession.reset();
            return false;
        }
        if (mMinLevel)
        {
            const auto level = std::ranges::find_if(columns, IsLevelColumn);
            if (level == columns.end())
            {
                mSessionError = TailFail(
                    ExitCode::Usage,
                    std::format(
                        "'--min-level' needs a level column; available columns are {}.", AvailableColumnList(columns)
                    )
                );
                mSession.reset();
                return false;
            }
            mLevelColumn = static_cast<std::size_t>(level - columns.begin());
        }
        mSession = std::move(*resolved);
        mHeaders = std::move(headers);
        mLayout = MakeTextLayout(mTable, mSession->visibleColumns);
        return true;
    }

    void Fail(CommandOutput failure)
    {
        mFailure = std::move(failure);
        mStop.request_stop();
    }

    void Process(bool final)
    {
        const std::size_t total = mTable.RowCount();
        if (mNextRow >= total)
        {
            return;
        }
        if (!EnsureSession())
        {
            if (final || total - mNextRow > kMaxDeferredRows)
            {
                Fail(std::move(mSessionError));
            }
            return;
        }

        const auto compiled = loglib::CompileExpression(
            mSession->expression, mTable.Configuration().Configuration().columns, &mTable
        );
        std::vector<std::size_t> accepted;
        std::string buffer;
        for (std::size_t row = mNextRow; row < total; ++row)
        {
            // `--min-level` is applied here, by level text, because streaming inference
            // may not have typed the column as `Level` yet. `--invert` negates the
            // whole combination, so it also negates the level test.
            const bool expressionMatch = loglib::EvaluateExpression(compiled, mTable, row);
            bool levelOk = true;
            if (mMinLevel)
            {
                const auto level = loglib::ParseLevelName(mTable.GetValueOrFormatted(row, mLevelColumn, buffer));
                levelOk = level && *level != loglib::LogLevel::Unknown && *level >= *mMinLevel;
            }
            const bool match = mOptions.invert ? (expressionMatch || !levelOk) : (expressionMatch && levelOk);
            if (match)
            {
                accepted.push_back(row);
            }
        }
        mNextRow = total;

        if (mOptions.maxRows)
        {
            const std::uint64_t remaining = *mOptions.maxRows - mEmitted;
            if (accepted.size() > remaining)
            {
                accepted.resize(static_cast<std::size_t>(remaining));
            }
        }
        if (!accepted.empty())
        {
            Write(accepted);
            mEmitted += accepted.size();
        }
        if (mOptions.maxRows && mEmitted >= *mOptions.maxRows)
        {
            mDone = true;
            mStop.request_stop();
        }
    }

    void Write(const std::vector<std::size_t> &rows)
    {
        loglib::exports::StreamSink sink(mOutput);
        if (mRendering == Rendering::Jsonl)
        {
            loglib::exports::ExportPlan plan;
            plan.format = loglib::exports::ExportFormat::JsonLines;
            plan.sourceRows = rows;
            plan.visibleColumns = mSession->visibleColumns;
            plan.includeAllFieldsForJson = !mOptions.columns.has_value();
            plan.table = &mTable;
            const auto exporter = loglib::exports::MakeExporter(plan.format);
            exporter->Run(plan.View(), sink, loglib::StopToken{});
        }
        else
        {
            std::string text;
            for (const std::size_t row : rows)
            {
                text += RenderTextLine(mTable, row, mLayout, mColor);
            }
            sink.Write(text);
        }
        // One flush per batch so a piped consumer sees rows promptly.
        sink.Finish();
    }

    void Evict()
    {
        if (mRetentionCap == 0)
        {
            return;
        }
        const std::size_t total = mTable.RowCount();
        if (total <= mRetentionCap)
        {
            return;
        }
        // Only rows already considered may go; held-back rows stay.
        const std::size_t drop = std::min(total - mRetentionCap, mNextRow);
        if (drop == 0)
        {
            return;
        }
        mTable.EvictPrefixRows(drop);
        mNextRow -= drop;
    }

    const Options &mOptions;
    const RuntimeContext &mRuntime;
    loglib::LogTable &mTable;
    std::FILE *mOutput;
    Rendering mRendering;
    bool mColor;
    std::size_t mRetentionCap;
    loglib::StopSource &mStop;
    /** @brief `options` without `--min-level`, which the sink applies itself. */
    Options mSessionOptions;
    std::optional<loglib::LogLevel> mMinLevel;
    std::size_t mLevelColumn = 0;

    std::optional<EffectiveSession> mSession;
    std::vector<std::string> mHeaders;
    TextLayout mLayout;
    CommandOutput mSessionError;
    std::optional<CommandOutput> mFailure;
    std::vector<std::string> mErrors;
    std::size_t mNextRow = 0;
    std::uint64_t mEmitted = 0;
    bool mDone = false;
};

/** @brief Live byte source with the label used in messages. */
struct LiveSource
{
    std::unique_ptr<loglib::BytesProducer> producer;
    std::filesystem::path displayName;
    std::string label;
    bool network = false;
};

[[nodiscard]] std::expected<LiveSource, CommandOutput> OpenLiveSource(const Options &options, const SourceSpec &spec)
{
    LiveSource live;
    switch (spec.kind)
    {
    case SourceKind::Path:
    {
        std::error_code ec;
        if (!std::filesystem::exists(spec.path, ec) || ec)
        {
            return std::unexpected(
                TailFail(ExitCode::Input, std::format("source '{}': does not exist", spec.path.string()))
            );
        }
        if (std::filesystem::is_directory(spec.path, ec))
        {
            return std::unexpected(
                TailFail(ExitCode::Input, std::format("source '{}': is a directory", spec.path.string()))
            );
        }
        // The producer pre-fills the last retained rows before it follows the file.
        const std::size_t prefill = !options.retention  ? loglib::DEFAULT_RETENTION_LINES
                                    : *options.retention == 0 ? loglib::MAX_RETENTION_LINES
                                                              : static_cast<std::size_t>(*options.retention);
        try
        {
            live.producer = std::make_unique<loglib::TailingBytesProducer>(spec.path, prefill);
        }
        catch (const std::exception &ex)
        {
            return std::unexpected(
                TailFail(ExitCode::Input, std::format("source '{}': {}", spec.path.string(), ex.what()))
            );
        }
        live.displayName = spec.path;
        live.label = spec.path.string();
        break;
    }
    case SourceKind::Stdin:
    {
        if (loglib::IsStdinInteractive())
        {
            return std::unexpected(TailFail(
                ExitCode::Input, "standard input is a terminal; pipe log data into slp or redirect from a file"
            ));
        }
        try
        {
            live.producer = std::make_unique<loglib::StdinBytesProducer>();
        }
        catch (const std::exception &ex)
        {
            return std::unexpected(
                TailFail(ExitCode::Input, std::format("failed to attach to stdin: {}", ex.what()))
            );
        }
        live.displayName = "-";
        live.label = "-";
        break;
    }
    case SourceKind::Tcp:
    case SourceKind::Udp:
    {
        const std::string host = spec.host.empty() ? std::string("0.0.0.0") : spec.host;
        try
        {
            if (spec.kind == SourceKind::Tcp)
            {
                loglib::TcpServerProducer::Options tcp;
                tcp.bindAddress = host;
                tcp.port = spec.port;
                live.producer = std::make_unique<loglib::TcpServerProducer>(std::move(tcp));
            }
            else
            {
                loglib::UdpServerProducer::Options udp;
                udp.bindAddress = host;
                udp.port = spec.port;
                live.producer = std::make_unique<loglib::UdpServerProducer>(std::move(udp));
            }
        }
        catch (const std::exception &ex)
        {
            return std::unexpected(TailFail(
                ExitCode::Input, std::format("cannot listen on {}://{}:{}: {}", spec.kind == SourceKind::Tcp ? "tcp" : "udp", host, spec.port, ex.what())
            ));
        }
        live.displayName = live.producer->DisplayName();
        live.label = live.producer->DisplayName();
        live.network = true;
        break;
    }
    case SourceKind::Bundle:
        return std::unexpected(TailFail(ExitCode::Usage, "a .slvbundle cannot be followed; use `slp query` instead."));
    }
    return live;
}

} // namespace

CommandOutput RunTail(const Options &options, const RuntimeContext &runtime)
{
    CommandOutput templates;
    LoadUserTemplates(options, templates);
    Say(templates.err);

    if (options.sources.size() != 1)
    {
        return TailFail(ExitCode::Usage, "tail follows exactly one source.");
    }
    const SourceSpec &spec = options.sources.front();

    // Reject malformed flags before any socket is bound or file is opened.
    for (const std::string &query : options.filters)
    {
        if (auto parsed = loglib::ParseQuery(query); !parsed)
        {
            CommandOutput out;
            out.exitCode = static_cast<int>(ExitCode::Usage);
            out.err = FormatFilterParseDiagnostic(query, parsed.error().offset, parsed.error().message);
            out.err.push_back('\n');
            return out;
        }
    }
    if (options.minLevel)
    {
        const auto level = loglib::ParseLevelName(*options.minLevel);
        if (!level || *level == loglib::LogLevel::Unknown)
        {
            return TailFail(
                ExitCode::Usage,
                std::format(
                    "'--min-level' expects trace, debug, info, warn, error, or fatal, not '{}'.", *options.minLevel
                )
            );
        }
    }
    if (options.maxRows && *options.maxRows == 0)
    {
        return {};
    }

    const bool toStdout = options.output == "-" || options.output.empty();
    Rendering rendering = Rendering::Jsonl;
    if (options.outputFormat == "text" || (options.outputFormat == "auto" && toStdout && IsTerminal(stdout)))
    {
        rendering = Rendering::Text;
    }
    const bool color = rendering == Rendering::Text &&
                       (toStdout ? ColorActive(options, stdout) : options.color == ColorMode::Always);

    std::unique_ptr<OutputStream> fileOutput;
    std::FILE *stream = stdout;
    if (!toStdout)
    {
        std::FILE *file = OpenAppend(std::filesystem::path(options.output));
        if (file == nullptr)
        {
            return TailFail(ExitCode::Output, std::format("could not open output '{}' for appending.", options.output));
        }
        fileOutput = std::make_unique<OutputStream>(file, true);
        stream = file;
    }

    auto parser = MakeStreamParser(options);
    if (!parser)
    {
        return std::move(parser.error());
    }

    auto live = OpenLiveSource(options, spec);
    if (!live)
    {
        return std::move(live.error());
    }
    if (live->network && !options.quiet)
    {
        Say(std::format("slp tail: listening on {}\n", live->label));
    }

    loglib::StopSource stop;
    loglib::LogTable table{loglib::LogData{}, loglib::LogConfigurationManager{}};
    loglib::BytesProducer *producer = live->producer.get();
    auto source = std::make_unique<loglib::StreamLineSource>(live->displayName, std::move(live->producer));
    loglib::StreamLineSource *rawSource = source.get();
    table.BeginStreaming(std::move(source));

    std::size_t retentionCap = loglib::DEFAULT_RETENTION_LINES;
    if (options.retention)
    {
        retentionCap = static_cast<std::size_t>(*options.retention);
    }

    TailSink sink(options, runtime, table, stream, rendering, color, retentionCap, stop);
    StopWatcher watcher(runtime.stop, stop, options.duration);
    watcher.Attach(producer);

    parser.value()->ParseStreaming(*rawSource, sink, StreamParserOptions(options, stop.get_token()));
    sink.Finish();

    CommandOutput out;
    if (sink.Failure())
    {
        out.err = sink.Failure()->err;
        out.exitCode = sink.Failure()->exitCode;
        return out;
    }

    const auto &parseErrors = sink.ParseErrors();
    if (!parseErrors.empty())
    {
        if (!options.quiet)
        {
            out.err += FormatErrorLine({}, std::format("{} lines could not be parsed (use --verbose to list)", parseErrors.size()));
            out.err.push_back('\n');
            if (options.verbose > 0)
            {
                for (const std::string &error : parseErrors)
                {
                    out.err += FormatWarningLine(error);
                    out.err.push_back('\n');
                }
            }
        }
        if (spec.kind == SourceKind::Tcp && sink.Emitted() == 0 && table.RowCount() == 0)
        {
            out.err += FormatErrorLine(
                "tail",
                std::format(
                    "no rows could be parsed from '{}'; if the peer connects over TLS, note that slp tail does not "
                    "support TLS, so a TLS-only peer cannot be read",
                    live->label
                )
            );
            out.err.push_back('\n');
        }
        if (options.strict)
        {
            out.exitCode = static_cast<int>(ExitCode::Data);
        }
    }
    return out;
}

} // namespace slp
