#include "loglib/schema/log_configuration.hpp"
#include "loglib/core/error.hpp"
#include "loglib/core/internal/file_busy_retry.hpp"
#include "loglib/core/internal/glaze_options.hpp"
#include "loglib/core/internal/path_encoding.hpp"
#include "loglib/schema/internal/log_configuration_glaze_meta.hpp"

#include <fmt/format.h>
#include <glaze/glaze.hpp>

#include <algorithm>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace
{

/** The text of the key that starts at the given offset, up to its closing quote. */
std::string KeyAt(std::string_view content, size_t offset)
{
    if (offset >= content.size())
    {
        return {};
    }
    const size_t end = content.find('"', offset);
    return std::string(content.substr(offset, end == std::string_view::npos ? std::string_view::npos : end - offset));
}

/** The first line of the message names the key, because callers often show only that line. */
std::string Describe(const glz::error_ctx &error, std::string_view content)
{
    if (error.ec == glz::error_code::unknown_key)
    {
        return fmt::format("unknown key '{}'.\n{}", KeyAt(content, error.count), glz::format_error(error, content));
    }
    if (error.ec == glz::error_code::missing_key)
    {
        return fmt::format(
            "the required key '{}' is missing.\n{}", error.custom_error_message, glz::format_error(error, content)
        );
    }
    return glz::format_error(error, content);
}

} // namespace

namespace loglib
{

void Source::ReplaceDisplayPaths(std::vector<std::string> paths)
{
    std::vector<std::string> stagedKeys = std::move(pendingDedupKeys);
    pendingDedupKeys.clear();
    std::vector<SourceLocator> next(paths.size());
    for (size_t i = 0; i < paths.size(); ++i)
    {
        next[i].displayPath = std::move(paths[i]);
        if (i < stagedKeys.size())
        {
            next[i].dedupKey = std::move(stagedKeys[i]);
        }
        else if (i < locators.size())
        {
            next[i].dedupKey = std::move(locators[i].dedupKey);
        }
    }
    locators = std::move(next);
}

void Source::ReplaceDedupKeys(std::vector<std::string> keys)
{
    if (locators.empty())
    {
        pendingDedupKeys = std::move(keys);
        return;
    }
    pendingDedupKeys.clear();
    const size_t n = std::min(locators.size(), keys.size());
    for (size_t i = 0; i < n; ++i)
    {
        locators[i].dedupKey = std::move(keys[i]);
    }
}

std::vector<std::string> Source::DisplayPaths() const
{
    std::vector<std::string> paths;
    paths.reserve(locators.size());
    for (const SourceLocator &locator : locators)
    {
        paths.push_back(locator.displayPath);
    }
    return paths;
}

std::vector<std::string> Source::DedupKeys() const
{
    std::vector<std::string> keys;
    keys.reserve(locators.size());
    for (const SourceLocator &locator : locators)
    {
        keys.push_back(locator.dedupKey);
    }
    return keys;
}

LogConfiguration LoadConfiguration(const std::filesystem::path &path)
{
    const std::ifstream file(path);
    if (!file.is_open())
    {
        throw IoError("Failed to open file '" + internal::PathToUtf8(path) + "'.");
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return ParseConfiguration(buffer.str(), internal::PathToUtf8(path));
}

LogConfiguration ParseConfiguration(std::string_view content, std::string_view origin)
{
    const std::string name = origin.empty() ? std::string("configuration") : fmt::format("configuration '{}'", origin);

    // The document is parsed on its own first, so a mid-file error leaves nothing half built.
    internal::ConfigurationDocument document;
    const auto error = glz::read<internal::CONFIGURATION_OPTS>(document, content);
    if (error)
    {
        throw FormatError(fmt::format("Invalid {}: {}", name, Describe(error, content)));
    }
    if (document.version != internal::CONFIGURATION_FORMAT_VERSION)
    {
        throw FormatError(
            fmt::format(
                "Invalid {}: the version is {}, but only version {} is supported.",
                name,
                document.version,
                internal::CONFIGURATION_FORMAT_VERSION
            )
        );
    }
    return internal::MakeConfiguration(std::move(document));
}

std::string SerializeConfiguration(const LogConfiguration &configuration, SaveScope scope, ConfigurationLayout layout)
{
    const internal::ConfigurationDocument document = internal::MakeDocument(configuration, scope);
    std::string json;
    const auto error = layout == ConfigurationLayout::Compact
                           ? glz::write<internal::CONFIGURATION_COMPACT_OPTS>(document, json)
                           : glz::write<internal::CONFIGURATION_OPTS>(document, json);
    if (error)
    {
        throw IoError("Failed to serialize configuration: " + glz::format_error(error));
    }
    return json;
}

void SaveConfiguration(
    const LogConfiguration &configuration,
    const std::filesystem::path &path,
    SaveScope scope,
    std::chrono::milliseconds busyRetryTimeout
)
{
    const std::string json = SerializeConfiguration(configuration, scope);

    // Atomic write: stream to <path>.tmp, flush + check stream
    // state, then rename. Prevents a crash / ENOSPC mid-write from
    // leaving a truncated <uuid>.json the recents index points at.
    // Stale .tmp files are swept by CleanupOrphanFiles.
    std::filesystem::path tempPath = path;
    tempPath += ".tmp";
    {
        std::ofstream file(tempPath);
        if (!file.is_open())
        {
            throw IoError("Failed to open file '" + internal::PathToUtf8(tempPath) + "'.");
        }
        file << json;
        file.flush();
        if (!file.good())
        {
            std::error_code cleanupEc;
            std::filesystem::remove(tempPath, cleanupEc);
            throw IoError("Failed to write file '" + internal::PathToUtf8(tempPath) + "'.");
        }
        // Explicit close + good() check so close-time failures
        // (network shares, deferred-write filesystems) are observable
        // -- the destructor would swallow them.
        file.close();
        if (!file.good())
        {
            std::error_code cleanupEc;
            std::filesystem::remove(tempPath, cleanupEc);
            throw IoError("Failed to close file '" + internal::PathToUtf8(tempPath) + "'.");
        }
    }
    const std::error_code ec = internal::RenameRetryingWhileFileBusy(tempPath, path, busyRetryTimeout);
    if (ec)
    {
        std::error_code cleanupEc;
        std::filesystem::remove(tempPath, cleanupEc);
        throw IoError(
            "Failed to rename '" + internal::PathToUtf8(tempPath) + "' to '" + internal::PathToUtf8(path) +
            "': " + ec.message()
        );
    }
}

} // namespace loglib
