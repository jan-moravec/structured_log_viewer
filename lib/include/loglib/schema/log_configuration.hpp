/**
 * @brief The content of a configuration file, and functions to load and save it.
 *
 * A LogConfiguration is what a configuration file holds: the columns, the session view and the source of the log. It is
 * a plain value. LoadConfiguration and ParseConfiguration read it, and SaveConfiguration and SerializeConfiguration
 * write it. A session bundle stores the same JSON for its configuration.
 *
 * The file is JSON with a required version, which is 1. Unknown keys are errors. ColumnLayout holds the live columns of
 * a table and SessionView holds the live session state, and a caller builds a LogConfiguration from them when it saves.
 * A column is saved with its type only when the user chose it.
 *
 * This is part of the schema module.
 */
#pragma once

#include "loglib/schema/column_schema.hpp"
#include "loglib/schema/session_view.hpp"
#include "loglib/schema/source_descriptor.hpp"

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace loglib
{

/** @brief What a configuration file holds. */
struct LogConfiguration
{
    /** @brief The columns, in display order. A file must have them, and they decide the layout everywhere. */
    std::vector<Column> columns;

    /** @brief The filter, sort, highlight rules and anchors. A file without them has the defaults. */
    SessionView view;

    /** @brief Where the log came from. Empty when the configuration is not tied to a source. */
    std::optional<Source> source;
};

/** @brief Chooses what a save writes. */
enum class SaveScope
{
    /** @brief The columns only, so the file can be used with other logs. */
    Columns,
    /** @brief Everything: columns, view and source. */
    Full,
};

/** @brief Chooses the shape of serialized text. */
enum class ConfigurationLayout
{
    /** @brief Indented over several lines, for files people read. */
    Indented,
    /** @brief One line, for the metadata line of a session bundle. */
    Compact,
};

/** @brief How long SaveConfiguration retries a busy destination when the caller gives no timeout. */
inline constexpr std::chrono::milliseconds DEFAULT_SAVE_BUSY_TIMEOUT{1000};

/**
 * @brief Reads a configuration file.
 *
 * Throws IoError if the file cannot be read and FormatError if its content is not a valid configuration. The message
 * names the file.
 */
[[nodiscard]] LogConfiguration LoadConfiguration(const std::filesystem::path &path);

/**
 * @brief Reads a configuration from text, so a caller can read a short prefix of a file without going through a large
 * log. Throws FormatError if the text is not a valid configuration. The origin is named in the message, for example the
 * path of the file.
 */
[[nodiscard]] LogConfiguration ParseConfiguration(std::string_view content, std::string_view origin = {});

/** @brief Writes a configuration as JSON text. A Columns scope leaves out the view and the source. */
[[nodiscard]] std::string SerializeConfiguration(
    const LogConfiguration &configuration,
    SaveScope scope = SaveScope::Full,
    ConfigurationLayout layout = ConfigurationLayout::Indented
);

/**
 * @brief Saves a configuration to a file. Throws IoError if it cannot be written.
 *
 * The scope has no default, so a caller must choose between columns only and everything, and cannot be caught out when
 * the configuration grows. The file is replaced by a rename. If the destination stays busy, for example held open by
 * another program, the rename is retried for up to busyRetryTimeout and then the save throws.
 */
void SaveConfiguration(
    const LogConfiguration &configuration,
    const std::filesystem::path &path,
    SaveScope scope,
    std::chrono::milliseconds busyRetryTimeout = DEFAULT_SAVE_BUSY_TIMEOUT
);

} // namespace loglib
