#include "commands.hpp"

#include "diagnostics.hpp"
#include "load_table.hpp"
#include "platform.hpp"
#include "session_config.hpp"

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

[[nodiscard]] CommandOutput BundleFail(ExitCode code, std::string_view sentence)
{
    CommandOutput out;
    out.exitCode = static_cast<int>(code);
    out.err = FormatErrorLine("bundle", sentence);
    out.err.push_back('\n');
    return out;
}

/** @brief Appends `.slvbundle` unless @p path already ends with it, ignoring case. */
[[nodiscard]] std::filesystem::path WithBundleExtension(std::filesystem::path path)
{
    std::string extension = path.extension().string();
    std::ranges::transform(extension, extension.begin(), [](char ch) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    });
    if (extension != loglib::SESSION_BUNDLE_EXTENSION)
    {
        path += loglib::SESSION_BUNDLE_EXTENSION;
    }
    return path;
}

/** @brief One stderr line rewritten in place; inert unless @p enabled. */
class ProgressLine
{
public:
    explicit ProgressLine(bool enabled) noexcept : mEnabled(enabled)
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
            stderr,
            "\rbundle: %llu/%llu rows (%u%%)",
            static_cast<unsigned long long>(written),
            static_cast<unsigned long long>(total),
            percent
        );
        std::fflush(stderr);
        mDirty = true;
    }

    /** @brief Ends the rewritten line so later stderr output starts on a fresh one. */
    void Finish()
    {
        if (mDirty)
        {
            std::fputc('\n', stderr);
            std::fflush(stderr);
            mDirty = false;
        }
    }

private:
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

CommandOutput RunBundle(const Options &options, const RuntimeContext &runtime)
{
    CommandOutput out;
    if (options.output == "-")
    {
        return BundleFail(ExitCode::Usage, "'--output' must be a file path; a bundle cannot be written to stdout.");
    }

    auto [loaded, loadSide] = LoadTable(options, runtime);
    out.err += loadSide.err;
    if (loadSide.exitCode != 0)
    {
        out.exitCode = loadSide.exitCode;
        return out;
    }

    auto session = ApplySessionConfiguration(options, loaded.table, runtime, "bundle");
    if (!session)
    {
        out.err += session.error().err;
        out.exitCode = session.error().exitCode;
        return out;
    }

    loglib::LogConfiguration configuration = session->configuration;
    configuration.source.reset();

    if (options.filteredRows)
    {
        const auto compiled = loglib::CompileExpression(
            session->expression, loaded.table.Configuration().Configuration().columns, &loaded.table
        );
        RetainRows(loaded.table, loglib::FilterAcceptedRows(loaded.table, compiled));
        configuration.expression = loglib::FilterExpression{};
    }

    if (!loaded.parseErrors.empty() && !options.quiet)
    {
        out.err += FormatErrorLine(
            {}, std::format("{} lines could not be parsed (use --verbose to list)", loaded.parseErrors.size())
        );
        out.err.push_back('\n');
        if (options.verbose > 0)
        {
            for (const std::string &error : loaded.parseErrors)
            {
                out.err += FormatWarningLine(error);
                out.err.push_back('\n');
            }
        }
    }

    const std::filesystem::path destination = WithBundleExtension(std::filesystem::path(options.output));

    ProgressLine progress(!options.quiet && IsTerminal(stderr));
    loglib::SessionBundleWriteOptions writeOptions;
    writeOptions.compressionLevel = options.compressionLevel;
    writeOptions.totalWorkers = static_cast<int>(options.bundleThreads);
    writeOptions.stopToken = runtime.stop;
    writeOptions.canonicalizeSourceLocator = CanonicalSourceLocator;
    writeOptions.progress = [&progress](std::uint64_t written, std::uint64_t total) { progress.Update(written, total); };

    try
    {
        loglib::WriteSessionBundle(loaded.table, configuration, destination, writeOptions);
    }
    catch (const loglib::SessionBundleCancelled &)
    {
        progress.Finish();
        out.exitCode = static_cast<int>(ExitCode::Cancelled);
        return out;
    }
    catch (const std::exception &ex)
    {
        progress.Finish();
        CommandOutput failure = BundleFail(
            ExitCode::Output, std::format("could not write bundle '{}': {}", destination.string(), ex.what())
        );
        out.err += failure.err;
        out.exitCode = failure.exitCode;
        return out;
    }
    progress.Finish();
    return out;
}

} // namespace slp
