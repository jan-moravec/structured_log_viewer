#pragma once

#include <log_generator/options.hpp>

#include <cstddef>
#include <memory>
#include <ostream>
#include <string>
#include <string_view>

namespace log_generator
{

/** @brief Destination for generated lines. */
class Sink
{
public:
    virtual ~Sink() = default;

    /**
     * @brief Writes @p line followed by a newline.
     *
     * @param line Serialized record without a trailing newline.
     * @return `false` when the reader has gone away and generation should stop quietly.
     * @throws std::runtime_error when the write fails.
     */
    virtual bool WriteLine(std::string_view line) = 0;

    /** @brief Pushes buffered lines to the reader. */
    virtual void Flush()
    {
    }

    /** @brief Flushes and releases the destination. */
    virtual void Close()
    {
    }

    /** @brief Returns how many times the output file was rotated. */
    virtual std::size_t Rotations() const noexcept
    {
        return 0;
    }
};

/**
 * @brief Opens the destination selected by `GeneratorOptions::target`.
 *
 * A non-empty @p header is written first, except when appending to a file.
 * File sinks rotate according to `GeneratorOptions::rotation` and report each
 * rotation on @p diag.
 *
 * @param options Validated generator settings.
 * @param header Format header line without its newline; empty for headerless formats.
 * @param diag Stream for progress messages; it must outlive the sink.
 * @return The open sink.
 * @throws std::runtime_error when the destination cannot be opened or reached.
 */
std::unique_ptr<Sink> OpenSink(const GeneratorOptions &options, const std::string &header, std::ostream &diag);

} // namespace log_generator
