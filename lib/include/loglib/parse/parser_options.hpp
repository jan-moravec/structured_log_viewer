/**
 * @brief Options that control a parse.
 *
 * ParserOptions carries the stop token, the optional configuration, and the knobs for the parallel file pipeline:
 * thread count and batch size. It also holds the logfmt continuation switch, the longest line a live stream buffers,
 * and bytes already consumed that the parser must process first.
 *
 * This is part of the parse module.
 */
#pragma once

#include "loglib/core/bytes_producer.hpp"
#include "loglib/core/stop_token.hpp"

#include <cstddef>
#include <memory>
#include <string>

namespace loglib
{

struct LogConfiguration;

/** @brief Thread cap used when ParserOptions::threads is 0, applied on top of the hardware concurrency. */
inline constexpr unsigned int DEFAULT_MAX_PARSE_THREADS = 8;

/** @brief Stage A batch size used when ParserOptions::batchSizeBytes is 0. */
inline constexpr std::size_t DEFAULT_PARSE_BATCH_BYTES = std::size_t{1024} * 1024;

/**
 * @brief Options for LogParser::ParseStreaming.
 *
 * threads and batchSizeBytes tune the parallel static-file pipeline, and zero for either selects the default. The
 * configuration is only declared here, so including this header does not pull in the whole configuration.
 */
struct ParserOptions
{
    /** @brief Cancels the parse when its source requests a stop. */
    StopToken stopToken{};

    /** @brief The configuration to parse with. May be null. */
    std::shared_ptr<const LogConfiguration> configuration;

    /**
     * @brief Whether the logfmt parser treats lines that start with a space or tab as the continuation of the previous
     * record's last field. Other parsers ignore it.
     */
    bool multilineLogfmt = true;

    /**
     * @brief Bytes that were already read from the stream and must be parsed first.
     *
     * AutoDetectParser and the standard input path use it to give the bytes they read for format detection to the
     * chosen parser, without changing the producer. Only a live stream may set it; a static-file parse asserts that it
     * is empty.
     */
    std::string initialCarry;

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
