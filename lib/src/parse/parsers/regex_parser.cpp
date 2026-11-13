#include "loglib/parse/parsers/regex_parser.hpp"

#include "loglib/core/compact_log_value.hpp"
#include "loglib/core/file_line_source.hpp"
#include "loglib/core/internal/pcre2_support.hpp"
#include "loglib/core/log_line.hpp"
#include "loglib/core/stream_line_source.hpp"
#include "loglib/parse/internal/classify_bare_scalar.hpp"
#include "loglib/parse/internal/line_decoder.hpp"
#include "loglib/parse/internal/probe_line_view.hpp"
#include "loglib/parse/internal/regex_compiled_pattern.hpp"
#include "loglib/parse/internal/regex_template_probe_list.hpp"
#include "loglib/parse/internal/static_parser_pipeline.hpp"
#include "loglib/parse/internal/streaming_parse_loop.hpp"
#include "loglib/parse/log_parser.hpp"
#include "loglib/parse/regex_templates.hpp"
#include "loglib/schema/log_configuration.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <memory>
#include <numeric>
#include <optional>
#include <shared_mutex>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace loglib
{

namespace
{

using internal::CompiledPattern;
using internal::FormatPcre2Error;
using internal::PatternSchema;
using internal::Pcre2CodePtr;
using internal::Pcre2MatchContextPtr;
using internal::Pcre2MatchDataPtr;
using internal::UTF8_BOM;

// ---------------------------------------------------------------------
// Schema interning: named groups -> KeyIds, in pcre2 name-table order.
// ---------------------------------------------------------------------

/**
 * Pre-intern every named group as a KeyId so Stage B does not
 * touch the global KeyIndex per cell. Mirrors CSV's eager header
 * intern step.
 */
std::vector<KeyId> InternSchemaKeys(const PatternSchema &schema, KeyIndex &keys)
{
    std::vector<KeyId> ids;
    ids.reserve(schema.groupNames.size());
    for (const std::string &name : schema.groupNames)
    {
        ids.push_back(keys.GetOrInsert(name));
    }
    return ids;
}

// ---------------------------------------------------------------------
// Match-and-emit: shared between the static and streaming pipelines.
// ---------------------------------------------------------------------

/**
 * Run one pcre2_match against @p line and emit the captured
 * named groups as compact values. out is appended in source
 * order (caller sorts before constructing the LogLine). The
 * decoder reuses ClassifyBareScalar so numeric captures get
 * typed (status codes, byte counts, ...), matching CSV's
 * bare-cell typing.
 *
 * fileBegin/fileSize enable the zero-copy MmapSlice fast
 * path for static parsing; streaming callers pass nullptr/0.
 * errorOut is populated on no-match / match-limit / other
 * errors and the function returns false; the caller decides
 * whether to surface it as a ParsedLineError or a
 * LineDecodeResult::Error. The capture of textOnlyKey is always
 * stored as text.
 */
bool MatchLineAndEmit(
    const CompiledPattern &compiled,
    pcre2_match_data *matchData,
    const std::vector<KeyId> &columnKeys,
    std::string_view line,
    const char *fileBegin,
    size_t fileSize,
    std::string &ownedArena,
    std::vector<std::pair<KeyId, loglib::CompactLogValue>> &out,
    std::string &errorOut,
    KeyId textOnlyKey
)
{
    // Self-contained contract: callers previously had to clear
    // `out` and `errorOut` themselves — brittle if any future
    // caller ever forgets on an error iteration. Clearing here
    // is a no-op for the current hot paths (both static and
    // streaming clear locally) but doesn't trust the call site.
    out.clear();
    errorOut.clear();

    // Outside UTF mode any bytes are a valid subject, so the check is skipped, and a JIT-compiled pattern is matched
    // directly, which also skips the argument checks of pcre2_match. A UTF pattern keeps the checked call, because
    // invalid UTF-8 must be reported and not read.
    const auto *subject = reinterpret_cast<PCRE2_SPTR>(line.data());
    int rc = 0;
    if (compiled.Utf())
    {
        rc = pcre2_match(compiled.Code(), subject, line.size(), 0, 0, matchData, compiled.Context());
    }
    else if (compiled.JitCompiled())
    {
        rc = pcre2_jit_match(compiled.Code(), subject, line.size(), 0, 0, matchData, compiled.Context());
    }
    else
    {
        rc = pcre2_match(compiled.Code(), subject, line.size(), 0, PCRE2_NO_UTF_CHECK, matchData, compiled.Context());
    }
    if (rc == PCRE2_ERROR_NOMATCH)
    {
        errorOut = "Line did not match the regex pattern.";
        return false;
    }
    if (rc == PCRE2_ERROR_MATCHLIMIT || rc == PCRE2_ERROR_DEPTHLIMIT)
    {
        errorOut = "Pattern match exceeded the backtracking limit on this line.";
        return false;
    }
    if (rc < 0)
    {
        errorOut = fmt::format("PCRE2 match error: {}", FormatPcre2Error(rc));
        return false;
    }

    const PCRE2_SIZE *ovector = pcre2_get_ovector_pointer(matchData);
    const auto captureCount = static_cast<uint32_t>(rc);

    const auto &schema = compiled.Schema();
    out.reserve(schema.groupIndices.size());
    for (size_t i = 0; i < schema.groupIndices.size(); ++i)
    {
        const uint32_t groupIndex = schema.groupIndices[i];
        if (groupIndex >= captureCount)
        {
            continue;
        }
        const PCRE2_SIZE startOff = ovector[2 * groupIndex];
        const PCRE2_SIZE endOff = ovector[(2 * groupIndex) + 1];
        if (startOff == PCRE2_UNSET || endOff == PCRE2_UNSET || endOff < startOff)
        {
            // Optional groups (e.g. `(?<pid>\d+)?`) that did not
            // participate are simply absent from the row; lookups
            // return monostate.
            continue;
        }
        const std::string_view captured(line.data() + startOff, endOff - startOff);
        if (captured.empty())
        {
            // Same convention as CSV: an empty capture is
            // "field present but blank" — drop it so it doesn't
            // bloat the per-line array.
            continue;
        }
        // The capture that continuation lines are added to must stay text, or the continuations could not join it.
        const loglib::CompactLogValue compact =
            columnKeys[i] == textOnlyKey ? internal::MakeStringCompact(captured, fileBegin, fileSize, ownedArena)
                                         : internal::ClassifyBareScalar(captured, fileBegin, fileSize, ownedArena);
        out.emplace_back(columnKeys[i], compact);
    }
    return true;
}

// ---------------------------------------------------------------------
// Static-pipeline glue.
// ---------------------------------------------------------------------

struct RegexWorkerState
{
    Pcre2MatchDataPtr matchData;
};

/**
 * Lazily attach @p worker to @p compiled on first use.
 * enumerable_thread_specific default-constructs RegexWorkerState,
 * so we can't allocate at construction: the worker doesn't yet
 * know which compiled pattern to size against. Kept as a free
 * function so RegexWorkerState stays a pure scratch struct
 * (no member functions alongside public data — placates
 * misc-non-private-member-variables-in-classes).
 */
void EnsureWorkerMatchData(RegexWorkerState &worker, const CompiledPattern &compiled)
{
    if (!worker.matchData)
    {
        worker.matchData = compiled.NewMatchData();
    }
}

bool RegexLooksLikeHeader(const CompiledPattern &compiled, pcre2_match_data *matchData, std::string_view line);

std::string_view StripCr(std::string_view s) noexcept
{
    if (!s.empty() && s.back() == '\r')
    {
        s.remove_suffix(1);
    }
    return s;
}

/**
 * Strip a leading UTF-8 BOM from @p sv if present. Mirrors CSV's
 * helper; only valid on the very first line of a file.
 */
std::string_view StripBom(std::string_view sv) noexcept
{
    if (sv.starts_with(UTF8_BOM))
    {
        sv.remove_prefix(UTF8_BOM.size());
    }
    return sv;
}

void DecodeRegexBatch(
    const internal::ByteRange &batch,
    internal::WorkerScratch<RegexWorkerState> &worker,
    const KeyIndex &keys,
    FileLineSource &source,
    std::span<const internal::TimeColumnSpec> timeColumns,
    internal::ParsedPipelineBatch &parsed,
    const CompiledPattern &compiled,
    const std::vector<KeyId> &columnKeys,
    ContinuationMode continuationMode,
    const CompiledPattern *headerAnchor
)
{
    (void)keys; // Schema is pre-interned in `columnKeys`.
    parsed.batchIndex = batch.batchIndex;

    EnsureWorkerMatchData(worker.user, compiled);

    // `columnKeys` follows the schema's group-index sort, so its last
    // entry is the final named group in pattern source order.
    const KeyId continuationTargetKey =
        (continuationMode != ContinuationMode::None && !columnKeys.empty()) ? columnKeys.back() : INVALID_KEY_ID;

    // Match data is per batch because PCRE2 match state is mutable.
    const CompiledPattern *headerProbe = (headerAnchor != nullptr) ? headerAnchor : &compiled;
    Pcre2MatchDataPtr headerScratch;
    if (continuationMode == ContinuationMode::UntilNextHeader)
    {
        headerScratch = headerProbe->NewMatchData();
    }

    const char *cursor = batch.bytesBegin;
    const char *end = batch.bytesEnd;
    const char *fileEnd = batch.fileEnd;
    const char *fileBegin = source.StableBytes().data();
    const auto fileSize = static_cast<size_t>(fileEnd - fileBegin);

    const auto batchBytes = static_cast<size_t>(end - cursor);
    const size_t estimatedLines = (batchBytes / 64) + 1;
    parsed.lines.reserve(estimatedLines);
    parsed.localLineOffsets.reserve(estimatedLines);

    size_t relativeLineNumber = 1;
    std::vector<std::pair<KeyId, loglib::CompactLogValue>> values;
    std::string lineError;
    // Batch-relative physical span of the current final record.
    size_t tailHeaderPhysical = 0;
    size_t tailLastPhysical = 0;
    // Leading blanks belong to the held record only when followed by
    // another continuation.
    size_t pendingLeadingBlanks = 0;

    while (cursor < end)
    {
        const char *lineStart = cursor;
        const char *newline = static_cast<const char *>(memchr(cursor, '\n', static_cast<size_t>(end - cursor)));
        const char *lineEnd = (newline != nullptr) ? newline : end;
        cursor = (newline != nullptr) ? newline + 1 : end;

        std::string_view line(lineStart, static_cast<size_t>(lineEnd - lineStart));
        line = StripCr(line);
        // Strip a leading UTF-8 BOM only when this line starts at
        // file byte 0. Keeps `^...` anchors in user / built-in
        // patterns binding after a BOM-prefixed editor save.
        // Stage A emits batches on line boundaries, so only the
        // first line of batch 0 can ever match here.
        if (lineStart == fileBegin)
        {
            line = StripBom(line);
        }

        // Pad by 1 on an unterminated last line so the final
        // byte isn't lost on `FileLineSource::RawLine` round-trips
        // (matches the CSV pipeline).
        const uint64_t nextOffset = static_cast<uint64_t>(cursor - fileBegin) + (newline == nullptr ? 1U : 0U);
        parsed.localLineOffsets.push_back(nextOffset);

        if (line.empty())
        {
            if (continuationMode != ContinuationMode::None && parsed.lines.empty())
            {
                ++pendingLeadingBlanks;
            }
            ++relativeLineNumber;
            continue;
        }

        bool isContinuation = false;
        if (continuationMode == ContinuationMode::Indented)
        {
            isContinuation = (line.front() == ' ' || line.front() == '\t');
        }
        else if (continuationMode == ContinuationMode::UntilNextHeader)
        {
            isContinuation = !RegexLooksLikeHeader(*headerProbe, headerScratch.get(), line);
        }

        if (isContinuation)
        {
            if (parsed.lines.empty())
            {
                // Include preceding blanks in the held record's physical span.
                parsed.leadingContinuationLineCount += pendingLeadingBlanks;
                pendingLeadingBlanks = 0;
                if (!parsed.leadingContinuationBytes.empty())
                {
                    parsed.leadingContinuationBytes.push_back('\n');
                }
                parsed.leadingContinuationBytes.append(line.data(), line.size());
                parsed.leadingContinuationLineCount++;
            }
            else
            {
                std::string continuation;
                continuation.reserve(1 + line.size());
                continuation.push_back('\n');
                continuation.append(line.data(), line.size());

                const auto compactSpan = parsed.lines.back().CompactValues();
                std::vector<std::pair<KeyId, loglib::CompactLogValue>> mutableValues(
                    compactSpan.begin(), compactSpan.end()
                );
                const std::string_view mmapView(fileBegin, fileBegin ? static_cast<size_t>(fileEnd - fileBegin) : 0);
                const internal::ContinuationSpliceOutcome outcome = internal::ExtendContinuationTarget(
                    mutableValues, parsed.ownedStringsArena, continuationTargetKey, continuation, mmapView
                );
                if (outcome == internal::ContinuationSpliceOutcome::Ok)
                {
                    const size_t headerLineId = parsed.lines.back().LineId();
                    parsed.lines.pop_back();
                    LogLine rebuilt(std::move(mutableValues), source, headerLineId);
                    parsed.lines.push_back(std::move(rebuilt));
                    tailLastPhysical = relativeLineNumber - 1;
                }
                else
                {
                    parsed.errors.push_back(
                        internal::ParsedLineError{
                            .relativeLine = relativeLineNumber,
                            .body = outcome == internal::ContinuationSpliceOutcome::MissingTarget
                                        ? "Continuation lines dropped: target field is not present in the record."
                                        : "Continuation lines dropped: target field is not a string.",
                        }
                    );
                }
            }
            ++relativeLineNumber;
            continue;
        }

        if (!MatchLineAndEmit(
                compiled,
                worker.user.matchData.get(),
                columnKeys,
                line,
                fileBegin,
                fileSize,
                parsed.ownedStringsArena,
                values,
                lineError,
                continuationTargetKey
            ))
        {
            parsed.errors.push_back(internal::ParsedLineError{.relativeLine = relativeLineNumber, .body = lineError});
            lineError.clear();
            ++relativeLineNumber;
            continue;
        }

        pendingLeadingBlanks = 0;

        // Save the span being sealed; Stage C handles the batch tail.
        if (continuationMode != ContinuationMode::None && !parsed.lines.empty() &&
            tailLastPhysical > tailHeaderPhysical)
        {
            parsed.completedMultiLineSpans.push_back(
                internal::ParsedPipelineBatch::MultiLineSpan{
                    .headerPhysicalLine = tailHeaderPhysical,
                    .lastPhysicalLine = tailLastPhysical,
                }
            );
        }

        // `LogLine` requires KeyId order, unlike source-order captures.
        std::sort(values.begin(), values.end(), [](const auto &a, const auto &b) { return a.first < b.first; });

        LogLine logLine(std::move(values), source, relativeLineNumber - 1);
        parsed.lines.push_back(std::move(logLine));
        worker.PromoteTimestamps(parsed.lines.back(), timeColumns, std::string_view(parsed.ownedStringsArena));
        tailHeaderPhysical = relativeLineNumber - 1;
        tailLastPhysical = tailHeaderPhysical;

        values.clear();
        ++relativeLineNumber;
    }

    parsed.totalLineCount = relativeLineNumber - 1;

    if (continuationMode != ContinuationMode::None && !parsed.lines.empty() && continuationTargetKey != INVALID_KEY_ID)
    {
        parsed.lastRecordOpenForContinuation = true;
        parsed.continuationTargetKeyId = continuationTargetKey;
        parsed.tailRecordHeaderPhysicalLine = tailHeaderPhysical;
        parsed.tailRecordLastPhysicalLine = tailLastPhysical;
    }
}

// ---------------------------------------------------------------------
// Streaming-loop glue.
// ---------------------------------------------------------------------

/**
 * Test @p line with an anchored, partial-hard header probe. A partial
 * prefix counts as a header so an incomplete live-tail line is not
 * folded into the preceding record.
 */
bool RegexLooksLikeHeader(const CompiledPattern &compiled, pcre2_match_data *matchData, std::string_view line)
{
    if (line.empty() || compiled.Code() == nullptr)
    {
        return false;
    }
    const int rc = pcre2_match(
        compiled.Code(),
        reinterpret_cast<PCRE2_SPTR>(line.data()),
        line.size(),
        /*startoffset*/ 0,
        PCRE2_ANCHORED | PCRE2_PARTIAL_HARD,
        matchData,
        compiled.Context()
    );
    return rc >= 0 || rc == PCRE2_ERROR_PARTIAL;
}

/**
 * Live-tail decoder that classifies continuations according to
 * @p mode. UntilNextHeader uses @p headerAnchor when supplied,
 * otherwise it probes the main compiled pattern.
 */
class RegexLineDecoder
{
public:
    RegexLineDecoder(
        const CompiledPattern &compiled,
        const std::vector<KeyId> &columnKeys,
        ContinuationMode mode = ContinuationMode::None,
        const CompiledPattern *headerAnchor = nullptr
    )
        : mCompiled(&compiled),
          mColumnKeys(&columnKeys),
          mMode(mode),
          mLastContinuationTarget(columnKeys.empty() ? INVALID_KEY_ID : columnKeys.back())
    {
        mMatchData = compiled.NewMatchData();
        if (mMode == ContinuationMode::UntilNextHeader)
        {
            mHeaderProbe = (headerAnchor != nullptr) ? headerAnchor : &compiled;
            mHeaderMatchData = mHeaderProbe->NewMatchData();
        }
    }

    internal::LineDecodeResult DecodeCompact(
        std::string_view line,
        KeyIndex &keys,
        internal::PerWorkerKeyCache &keyCache,
        std::vector<std::pair<KeyId, loglib::CompactLogValue>> &out,
        std::string &outOwnedArena,
        std::string &errorOut
    )
    {
        (void)keys;     // Schema pre-interned.
        (void)keyCache; // ditto.
        out.clear();
        outOwnedArena.clear();
        // Strip a leading UTF-8 BOM only on the first line of the
        // stream. The flag latches so a literal `\xEF\xBB\xBF`
        // inside a later line stays intact.
        if (!mSawFirstLine)
        {
            line = StripBom(line);
            mSawFirstLine = true;
        }
        if (line.empty())
        {
            // The streaming loop already filters blank lines;
            // this branch is defensive. `Skip` (no row, no error)
            // is the right result if it's ever reached.
            return internal::LineDecodeResult::Skip;
        }

        // Classify continuations before record matching to avoid false errors.
        if (mMode == ContinuationMode::Indented)
        {
            if (line.front() == ' ' || line.front() == '\t')
            {
                return internal::LineDecodeResult::Continue;
            }
        }
        else if (
            mMode == ContinuationMode::UntilNextHeader &&
            !RegexLooksLikeHeader(*mHeaderProbe, mHeaderMatchData.get(), line)
        )
        {
            return internal::LineDecodeResult::Continue;
        }

        if (!MatchLineAndEmit(
                *mCompiled,
                mMatchData.get(),
                *mColumnKeys,
                line,
                /*fileBegin=*/nullptr,
                /*fileSize=*/0,
                outOwnedArena,
                out,
                errorOut,
                mMode != ContinuationMode::None ? mLastContinuationTarget : INVALID_KEY_ID
            ))
        {
            return internal::LineDecodeResult::Error;
        }
        return internal::LineDecodeResult::Emit;
    }

    /** KeyId of the pattern's final source-order named group. */
    [[nodiscard]] KeyId LastContinuationTarget() const noexcept
    {
        return mLastContinuationTarget;
    }

private:
    const CompiledPattern *mCompiled;
    const std::vector<KeyId> *mColumnKeys;
    ContinuationMode mMode;
    KeyId mLastContinuationTarget;
    /** Non-owning header probe; null outside UntilNextHeader mode. */
    const CompiledPattern *mHeaderProbe = nullptr;
    Pcre2MatchDataPtr mMatchData;
    /**
     * Match data is tied to mHeaderProbe and cannot be shared with
     * the main pattern's match state.
     */
    Pcre2MatchDataPtr mHeaderMatchData;
    bool mSawFirstLine = false;
};

static_assert(
    internal::CompactLineDecoder<RegexLineDecoder>, "RegexLineDecoder must satisfy the CompactLineDecoder concept"
);

/**
 * Single-error terminal pass: honour the sink contract (one
 * OnBatch before OnFinished) when we can't start a pipeline
 * at all — empty pattern, bad pattern, missing named groups.
 * @p streaming picks flush thresholds that match the surrounding
 * pipeline. Both presets behave identically for a single-batch
 * error pass, but the symmetry keeps call sites consistent.
 */
void EmitErrorAndFinish(LogParseSink &sink, std::string_view message, bool streaming = false)
{
    sink.OnStarted();
    KeyIndex &keys = sink.Keys();
    const size_t flushLines = streaming ? internal::STREAMING_BATCH_FLUSH_LINES : internal::STATIC_BATCH_FLUSH_LINES;
    const auto flushInterval =
        streaming ? internal::STREAMING_BATCH_FLUSH_INTERVAL : internal::STATIC_BATCH_FLUSH_INTERVAL;
    internal::BatchCoalescer coalescer(sink, keys, flushLines, flushInterval);
    coalescer.Pending().errors.push_back(ParseError{.message = std::string(message)});
    coalescer.Finish(0, /*wasCancelled=*/false);
}

} // namespace

// ---------------------------------------------------------------------
// RegexParser public surface.
// ---------------------------------------------------------------------

RegexParser::RegexParser(std::string pattern, ContinuationMode continuationMode, std::string headerAnchor)
    : mPattern(std::move(pattern)), mContinuationMode(continuationMode), mHeaderAnchor(std::move(headerAnchor))
{
}

void RegexParser::ParseStreaming(StreamLineSource &source, LogParseSink &sink, ParserOptions options) const
{
    const std::string &pattern = mPattern;
    if (pattern.empty())
    {
        EmitErrorAndFinish(sink, "Regex parser requires a non-empty pattern.", /*streaming=*/true);
        return;
    }

    CompiledPattern compiled;
    std::string compileError;
    if (!compiled.Compile(pattern, compileError))
    {
        EmitErrorAndFinish(sink, compileError, /*streaming=*/true);
        return;
    }
    if (!compiled.HasNamedGroups())
    {
        EmitErrorAndFinish(
            sink,
            "Regex pattern has no named capture groups; nothing to put in columns. Use `(?<Name>...)`.",
            /*streaming=*/true
        );
        return;
    }

    const std::vector<KeyId> columnKeys = InternSchemaKeys(compiled.Schema(), sink.Keys());

    const ContinuationMode mode = mContinuationMode;
    const std::string &headerAnchorSource = mHeaderAnchor;

    // A configured anchor must compile; null means reuse the main pattern.
    CompiledPattern headerAnchor;
    const CompiledPattern *anchorPtr = nullptr;
    if (mode == ContinuationMode::UntilNextHeader && !headerAnchorSource.empty())
    {
        std::string anchorErr;
        if (!headerAnchor.Compile(headerAnchorSource, anchorErr))
        {
            EmitErrorAndFinish(sink, "Header anchor compile failed: " + anchorErr, /*streaming=*/true);
            return;
        }
        anchorPtr = &headerAnchor;
    }

    RegexLineDecoder decoder(compiled, columnKeys, mode, anchorPtr);
    // The named groups are interned up front, so the loop announces them as declared keys.
    internal::RunStreamingParseLoop(source, decoder, sink, options, columnKeys);
}

void RegexParser::ParseStreaming(FileLineSource &source, LogParseSink &sink, ParserOptions options) const
{
    // An empty pattern fails with the same error as the streaming overload.
    const std::string &pattern = mPattern;

    const std::span<const char> bytes = source.StableBytes();
    const char *fileBegin = bytes.data();
    const size_t fileSize = bytes.size();
    const char *fileEnd = (fileBegin != nullptr) ? fileBegin + fileSize : nullptr;

    if (pattern.empty())
    {
        EmitErrorAndFinish(sink, "Regex parser requires a non-empty pattern.");
        return;
    }

    // Compile once on the orchestrator thread; the pipeline
    // shares the compiled code read-only across Stage B workers.
    // `RunStaticParserPipeline` runs synchronously (joins every
    // TBB worker before returning) so a stack value captured by
    // reference outlives all concurrent uses — no shared_ptr or
    // heap allocation needed.
    CompiledPattern compiled;
    std::string compileError;
    if (!compiled.Compile(pattern, compileError))
    {
        EmitErrorAndFinish(sink, compileError);
        return;
    }
    if (!compiled.HasNamedGroups())
    {
        EmitErrorAndFinish(
            sink, "Regex pattern has no named capture groups; nothing to put in columns. Use `(?<Name>...)`."
        );
        return;
    }

    const std::vector<KeyId> columnKeys = InternSchemaKeys(compiled.Schema(), sink.Keys());

    const ContinuationMode staticContinuationMode = mContinuationMode;
    const std::string &headerAnchorSource = mHeaderAnchor;

    // The synchronous pipeline guarantees this stack-owned pattern
    // outlives every Stage B worker.
    CompiledPattern headerAnchor;
    const CompiledPattern *anchorPtr = nullptr;
    if (staticContinuationMode == ContinuationMode::UntilNextHeader && !headerAnchorSource.empty())
    {
        std::string anchorErr;
        if (!headerAnchor.Compile(headerAnchorSource, anchorErr))
        {
            EmitErrorAndFinish(sink, "Header anchor compile failed: " + anchorErr);
            return;
        }
        anchorPtr = &headerAnchor;
    }

    const size_t batchSize = options.batchSizeBytes != 0 ? options.batchSizeBytes : DEFAULT_PARSE_BATCH_BYTES;

    auto stageA = internal::MakeLineAlignedBatcher(fileBegin, fileEnd, batchSize);

    FileLineSource *sourcePtr = &source;
    auto stageB = [sourcePtr, &compiled, &columnKeys, staticContinuationMode, anchorPtr](
                      internal::ByteRange token,
                      internal::WorkerScratch<RegexWorkerState> &worker,
                      KeyIndex &keys,
                      std::span<const internal::TimeColumnSpec> timeColumns,
                      internal::ParsedPipelineBatch &parsed
                  ) {
        DecodeRegexBatch(
            token,
            worker,
            keys,
            *sourcePtr,
            timeColumns,
            parsed,
            compiled,
            columnKeys,
            staticContinuationMode,
            anchorPtr
        );
    };

    // The named groups are interned up front, so the pipeline announces them as declared keys.
    internal::RunStaticParserPipeline<internal::ByteRange, RegexWorkerState>(
        source, sink, options, stageA, stageB, columnKeys
    );
}

} // namespace loglib
