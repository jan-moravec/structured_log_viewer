#include "commands.hpp"

#include "console.hpp"
#include "diagnostics.hpp"
#include "load_table.hpp"
#include "platform.hpp"
#include "session_config.hpp"
#include "text_util.hpp"

#include <loglib/column_projection.hpp>
#include <loglib/filter_expression.hpp>
#include <loglib/leaf_rule_compile.hpp>
#include <loglib/log_filter.hpp>
#include <loglib/log_table.hpp>
#include <loglib/session_bundle.hpp>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <format>
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
        std::fprintf(
            mStream,
            "\rbundle: %llu/%llu rows (%u%%)",
            static_cast<unsigned long long>(written),
            static_cast<unsigned long long>(total),
            percent
        );
        std::fflush(mStream);
        mDirty = true;
    }

    /** @brief Ends the rewritten line so later stderr output starts on a fresh one. */
    void Finish()
    {
        if (mDirty)
        {
            std::fputc('\n', mStream);
            std::fflush(mStream);
            mDirty = false;
        }
    }

private:
    std::FILE *mStream;
    bool mEnabled;
    bool mDirty = false;
};

/**
 * @brief Keeps only the lines at @p rows, in ascending row order.
 *
 * The table's derived state (row ids, enum caches) is stale afterward;
 * the caller only hands the table to `WriteSessionBundle`, which reads
 * `Data().Lines()` and `Data().Keys()`.
 */
void RetainRows(loglib::LogTable &table, std::vector<std::size_t> rows)
{
    std::ranges::sort(rows);
    auto &lines = table.Data().Lines();
    std::size_t kept = 0;
    for (const std::size_t row : rows)
    {
        if (row != kept)
        {
            lines[kept] = std::move(lines[row]);
        }
        ++kept;
    }
    lines.erase(lines.begin() + static_cast<std::ptrdiff_t>(kept), lines.end());
}

} // namespace

ExitCode RunBundle(
    const BundleOptions &options, const GlobalOptions &global, Console &console, const RuntimeContext &runtime
)
{
    auto loaded = LoadTable(global, options.input, console, runtime);
    if (!loaded)
    {
        return console.Report(loaded.error());
    }

    auto session = ApplySessionConfiguration(options.filter, options.view, loaded->table, runtime, BundleOptions::NAME);
    if (!session)
    {
        return console.Report(session.error());
    }

    loglib::LogConfiguration configuration = session->configuration;
    configuration.source.reset();

    if (options.filteredRows)
    {
        const auto compiled = loglib::CompileExpression(
            session->expression, loaded->table.Configuration().Configuration().columns, &loaded->table
        );
        RetainRows(loaded->table, loglib::FilterAcceptedRows(loaded->table, compiled));
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

    try
    {
        loglib::WriteSessionBundle(loaded->table, configuration, destination, writeOptions);
    }
    catch (const loglib::SessionBundleCancelled &)
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
    return ExitCode::Success;
}

} // namespace slp
