/**
 * @brief Options that control a parse.
 *
 * ParserOptions carries the stop token, the time columns to convert while parsing, the time zone and reference time
 * they are read with, and the knobs for the parallel file pipeline: thread count and batch size. It also holds the
 * longest line a live stream buffers. It holds plain values only, so a parse never reads a process-wide setting.
 *
 * This is part of the parse module.
 */
#pragma once

#include "loglib/core/bytes_producer.hpp"
#include "loglib/core/stop_token.hpp"
#include "loglib/schema/column_schema.hpp"
#include "loglib/time/time_zone_context.hpp"

#include <chrono>
#include <cstddef>
#include <functional>
#include <optional>
#include <vector>

namespace loglib
{

/** @brief Thread cap used when ParserOptions::threads is 0, applied on top of the hardware concurrency. */
inline constexpr unsigned int DEFAULT_MAX_PARSE_THREADS = 8;

/** @brief Stage A batch size used when ParserOptions::batchSizeBytes is 0. */
inline constexpr std::size_t DEFAULT_PARSE_BATCH_BYTES = std::size_t{1024} * 1024;

/**
 * @brief Options for LogParser::ParseStreaming.
 *
 * threads and batchSizeBytes tune the parallel static-file pipeline, and zero for either selects the default.
 */
struct ParserOptions
{
    /** @brief Cancels the parse when its source requests a stop. */
    StopToken stopToken;

    /** @brief Time columns to convert while parsing. Values in other columns are left as text. */
    std::vector<Column> timeColumns;

    /** @brief The zone in which timestamps that carry no offset were written. UTC unless set. */
    TimeZoneContext defaultSourceTimeZone;

    /**
     * @brief The "now" for timestamp formats without a year, such as the RFC 3164 syslog header. If unset, the parse
     * reads the clock once when it starts, so every line of one parse gets the same year.
     */
    std::optional<std::chrono::system_clock::time_point> referenceTime;

    /**
     * @brief The clock a live stream reads for the "now" of formats without a year. It is read once for each batch, so
     * a stream that runs across New Year gives the rows after it the new year. Empty means the system clock. It is not
     * used when referenceTime is set, and a static file never reads it, so a file gives the same result however long
     * it takes to read. Tests inject a clock here.
     */
    std::function<std::chrono::system_clock::time_point()> clock;

    /**
     * @brief The most worker threads the static-file pipeline uses. Zero means the smaller of the hardware thread
     * count and DEFAULT_MAX_PARSE_THREADS.
     */
    unsigned int threads = 0;

    /**
     * @brief The size of the byte ranges the static-file pipeline hands to its workers. Zero means
     * DEFAULT_PARSE_BATCH_BYTES. Ranges are extended so that no line is split.
     */
    std::size_t batchSizeBytes = 0;

    /**
     * @brief The longest line, and the longest multi-line record, that a live stream buffers.
     *
     * A longer line is dropped with one error, and parsing continues after its newline. If a record grows beyond the
     * limit, the extra continuation lines are dropped and reported in one error, and the record is kept.
     */
    std::size_t maxLineBytes = DEFAULT_MAX_LINE_BYTES;
};

} // namespace loglib
