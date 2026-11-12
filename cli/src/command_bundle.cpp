#include "commands.hpp"

#include "console.hpp"
#include "diagnostics.hpp"
#include "load_table.hpp"
#include "platform.hpp"
#include "session_config.hpp"
#include "text_util.hpp"

#include <loglib/output/session_bundle.hpp>
#include <loglib/query/column_projection.hpp>
#include <loglib/query/leaf_rule_compile.hpp>
#include <loglib/query/log_filter.hpp>
#include <loglib/schema/filter_expression.hpp>
#include <loglib/table/log_table.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace slp
{

namespace
{

/** @brief Appends `.slvbundle` unless @p path already ends with it, ignoring case. */
[[nodiscard]] std::filesystem::path WithBundleExtension(std::filesystem::path path)
{
    if (!EqualsIgnoreCase(path.extension().string(), loglib::SESSION_BUNDLE_EXTENSION))
    {
        path += loglib::SESSION_BUNDLE_EXTENSION;
    }
    return path;
}

/** @brief One line of @p stream rewritten in place; inert unless @p enabled. */
class ProgressLine
{
public:
    ProgressLine(std::FILE *stream, bool enabled) noexcept
        : mStream(stream), mEnabled(enabled)
    {
    }

    void Update(std::uint64_t written, std::uint64_t total)
    {
        if (!mEnabled)
        {
            return;
        }
        const unsigned percent = total == 0 ? 100U : static_cast<unsigned>(written * 100U / total);
        Write(std::format("\rbundle: {}/{} rows ({}%)", written, total, percent));
        mDirty = true;
    }

    /** @brief Ends the rewritten line so later stderr output starts on a fresh one. */
    void Finish()
    {
        if (mDirty)
        {
            Write("\n");
            mDirty = false;
        }
    }

private:
    /** @brief Progress output is best effort: a failed write is not an error of the command. */
    void Write(std::string_view text) const
    {
        (void)std::fwrite(text.data(), 1, text.size(), mStream);
        (void)std::fflush(mStream);
    }

    std::FILE *mStream;
    bool mEnabled;
    bool mDirty = false;
};

} // namespace

ExitCode RunBundle(
    const BundleOptions &options, const GlobalOptions &global, Console &console, const RuntimeContext &runtime
)
{
    const auto parseStarted = std::chrono::steady_clock::now();
    auto loaded = LoadTable(global, options.input, console, runtime);
    if (!loaded)
    {
        return console.Report(loaded.error());
    }
    console.Stage("parse", loaded->table.RowCount(), parseStarted);

    auto session = ApplySessionConfiguration(options.filter, options.view, loaded->table, runtime, BundleOptions::NAME);
    if (!session)
    {
        return console.Report(session.error());
    }
    if (auto failure = CheckLoadedFilter(options.filter, *session, loaded->table, BundleOptions::NAME))
    {
        return console.Report(*failure);
    }

    loglib::LogConfiguration configuration = session->configuration;
    configuration.source.reset();

    std::optional<std::vector<std::size_t>> filteredRows;
    if (options.filteredRows)
    {
        const auto filterStarted = std::chrono::steady_clock::now();
        const auto compiled = loglib::CompileExpression(
            session->expression, loaded->table.Configuration().Configuration().columns, &loaded->table
        );
        filteredRows = loglib::FilterAcceptedRows(loaded->table, compiled);
        console.Stage("filter", filteredRows->size(), filterStarted);
        configuration.expression = loglib::FilterExpression{};
    }

    console.ParseErrors(loaded->parseErrors);

    const std::filesystem::path destination = WithBundleExtension(options.output);

    ProgressLine progress(console.Err(), !console.Quiet() && IsTerminal(console.Err()));
    loglib::SessionBundleWriteOptions writeOptions;
    writeOptions.compressionLevel = options.compressionLevel;
    writeOptions.totalWorkers = static_cast<int>(options.bundleThreads);
    writeOptions.stopToken = runtime.stop;
    writeOptions.canonicalizeSourceLocator = CanonicalSourceLocator;
    writeOptions.progress = [&progress](std::uint64_t written, std::uint64_t total) {
        progress.Update(written, total);
    };

    const auto writeStarted = std::chrono::steady_clock::now();
    try
    {
        if (filteredRows)
        {
            loglib::WriteSessionBundle(loaded->table, configuration, *filteredRows, destination, writeOptions);
        }
        else
        {
            loglib::WriteSessionBundle(loaded->table, configuration, destination, writeOptions);
        }
    }
    catch (const loglib::OperationCancelled &)
    {
        progress.Finish();
        return ExitCode::Cancelled;
    }
    catch (const std::exception &ex)
    {
        progress.Finish();
        return console.Report(console.Fail(
            ExitCode::Output, std::format("could not write bundle '{}': {}", destination.string(), ex.what())
        ));
    }
    progress.Finish();
    console.Stage("write", filteredRows ? filteredRows->size() : loaded->table.RowCount(), writeStarted);
    return ExitCode::Success;
}

} // namespace slp
