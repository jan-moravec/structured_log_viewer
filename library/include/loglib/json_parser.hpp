#pragma once

#include "log_line.hpp"
#include "log_parser.hpp"
#include "parser_options.hpp"
#include "streaming_log_sink.hpp"

#include <filesystem>
#include <string>

namespace loglib
{

class LogFile;

namespace internal
{
struct AdvancedParserOptions;
}

/**
 * @class JsonParser
 * @brief Parser for newline-delimited JSON log files.
 *
 * Implements the streaming pipeline by delegating to the shared TBB harness in
 * `library/src/parser_pipeline.hpp`; the public surface is intentionally
 * minimal.
 */
class JsonParser : public LogParser
{
public:
    bool IsValid(const std::filesystem::path &file) const override;

    /**
     * @brief Streams the parse of @p file into @p sink using the shared
     *        TBB pipeline.
     *
     * Pipeline shape:
     *   - Stage A (`serial_in_order`) chunks the mmap into ~1 MiB batches at
     *     line boundaries.
     *   - Stage B (`parallel`) decodes each batch via simdjson, interns keys
     *     via the canonical `KeyIndex`, and applies inline timestamp promotion
     *     when `options.configuration` is set.
     *   - Stage C (`serial_in_order`) stamps absolute line numbers, computes
     *     the new-keys slice, and hands the resulting `StreamedBatch` to the
     *     sink.
     *
     * Cooperative cancellation is via `options.stopToken`; cancellation
     * latency is bounded by `ntokens * batchSizeBytes` of in-flight work.
     */
    void ParseStreaming(LogFile &file, StreamingLogSink &sink, ParserOptions options = {}) const override;

    /**
     * @brief Streaming parse with both the public options and the advanced
     *        tuning knobs.
     *
     * Reachable only via `<loglib/internal/parser_options.hpp>`. Used by
     * benchmarks and bisects to dial worker count, batch size, scratch
     * caches, and telemetry.
     */
    void ParseStreaming(
        LogFile &file,
        StreamingLogSink &sink,
        ParserOptions options,
        internal::AdvancedParserOptions advanced
    ) const override;

    std::string ToString(const LogLine &line) const override;

    /**
     * @brief Cold-path overload that serialises a `LogMap` directly.
     *
     * Useful for tests, debug dumps and other callers that do not have a
     * `LogLine`/`KeyIndex` pair on hand. The serialisation is byte-identical
     * to `ToString(LogLine)` for the same key/value pairs.
     */
    std::string ToString(const LogMap &values) const;
};

} // namespace loglib
