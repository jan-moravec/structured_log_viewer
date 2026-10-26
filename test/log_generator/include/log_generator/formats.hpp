#pragma once

#include <test_common/log_format.hpp>
#include <test_common/log_record.hpp>

#include <cstdint>
#include <ostream>
#include <span>
#include <string>
#include <string_view>

namespace log_generator
{

/** @brief Group a `--format` value belongs to. */
enum class FormatFamily
{
    /** Serializes every record field: JSON Lines, logfmt, or CSV. */
    Structured,
    /** Synthesizes lines that a shipped `loglib::RegexTemplate` parses. */
    RegexTemplate,
};

/** @brief One `--format` value and the serializer it selects. */
struct FormatSpec
{
    /** Command-line value, such as `json` or `apache-combined`. */
    std::string_view name;
    FormatFamily family;
    /** Name of the matching `loglib::RegexTemplate`; empty for structured formats. */
    std::string_view templateName;
    /** One-line description shown by `--list-formats`. */
    std::string_view description;
    /** Whether the serializer needs a record schema for its header. */
    bool needsSchema;
    /** Creates the serializer; @p schema is empty unless `needsSchema` is set. */
    test_common::LogFormat (*make)(const test_common::RecordSchema &schema);
};

/** @brief Serializer selected by a `FormatSpec`, ready to write. */
struct ResolvedFormat
{
    test_common::LogFormat format;
    test_common::RecordSchema schema;

    /** @brief Returns the header line without its newline, or an empty string for headerless formats. */
    std::string Header() const
    {
        return format.writeHeader(schema);
    }
};

/** @brief Returns every supported format, structured formats first. */
std::span<const FormatSpec> Formats() noexcept;

/**
 * @brief Finds a format by its command-line name, ignoring case.
 *
 * @return The matching format, or `nullptr`.
 */
const FormatSpec *FindFormat(std::string_view name) noexcept;

/** @brief Writes the `--list-formats` table to @p os. */
void PrintFormatList(std::ostream &os);

/**
 * @brief Returns the output file name used when `--output` is omitted.
 *
 * Structured formats use `generated` plus the format's extension, for example
 * `generated.jsonl`. Regex-template formats add the name, for example
 * `generated_syslog.log`.
 */
std::string DefaultFileName(const FormatSpec &spec);

/**
 * @brief Creates the serializer for @p spec.
 *
 * Schema-bearing formats derive their column list from one probe record
 * generated with @p seed, so the header matches the generated records.
 *
 * @param spec Format to create.
 * @param seed Seed of the generation run.
 * @return The serializer and its schema.
 */
ResolvedFormat ResolveFormat(const FormatSpec &spec, std::uint32_t seed);

} // namespace log_generator
