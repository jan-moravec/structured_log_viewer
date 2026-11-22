#include "loglib/parse/parsers/regex_parser.hpp"

#include "loglib/config/log_configuration.hpp"
#include "loglib/core/compact_log_value.hpp"
#include "loglib/core/file_line_source.hpp"
#include "loglib/core/internal/pcre2_support.hpp"
#include "loglib/core/log_line.hpp"
#include "loglib/core/stream_line_source.hpp"
#include "loglib/parse/internal/classify_bare_scalar.hpp"
#include "loglib/parse/internal/line_decoder.hpp"
#include "loglib/parse/internal/probe_line_view.hpp"
#include "loglib/parse/internal/regex_compiled_pattern.hpp"
#include "loglib/parse/internal/static_parser_pipeline.hpp"
#include "loglib/parse/internal/streaming_parse_loop.hpp"
#include "loglib/parse/log_parser.hpp"
#include "loglib/parse/regex_templates.hpp"

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

// ---------------------------------------------------------------------
// Schema interning: named groups -> KeyIds, in pcre2 name-table order.
// ---------------------------------------------------------------------

/**
 * Pre-intern every named group as a KeyId so the decoder does not
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
    std::vector<loglib::CompactLogValue> &out,
    std::string &errorOut,
    KeyId textOnlyKey
)
{
    // Clear the outputs here, so a caller that forgets to clear them between rows cannot leak the previous row's
    // fields.
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
            // Optional groups (e.g. (?<pid>\d+)?) that did not
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
        out.push_back(compact.WithKey(columnKeys[i]));
    }
    return true;
}

// ---------------------------------------------------------------------
// Static-pipeline glue.
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
 * The regex decoder. It classifies continuation lines according to its mode. UntilNextHeader uses the header anchor
 * when one is supplied, otherwise it probes the main compiled pattern. It owns the match data, which is mutable PCRE2
 * state, so each worker needs its own decoder over the shared compiled patterns.
 */
class RegexLineDecoder
{
public:
    /** A UTF-8 byte order mark can start the first line, and the pattern should still match from its start. */
    static constexpr bool kStripBom = true;

    RegexLineDecoder(
        const CompiledPattern &compiled,
        const std::vector<KeyId> &columnKeys,
        ContinuationMode mode = ContinuationMode::None,
        const CompiledPattern *headerAnchor = nullptr
    )
        : mCompiled(&compiled),
          mColumnKeys(&columnKeys),
          mMode(mode),
          mContinuationTarget(
              (mode != ContinuationMode::None && !columnKeys.empty()) ? columnKeys.back() : INVALID_KEY_ID
          )
    {
        mMatchData = compiled.NewMatchData();
        if (mMode == ContinuationMode::UntilNextHeader)
        {
            mHeaderProbe = (headerAnchor != nullptr) ? headerAnchor : &compiled;
            mHeaderMatchData = mHeaderProbe->NewMatchData();
        }
    }

    internal::LineDecodeResult Decode(
        std::string_view line,
        internal::LineContext &ctx,
        std::vector<loglib::CompactLogValue> &out,
        std::string &errorOut
    )
    {
        // The schema keys are interned before the parse starts, so the key cache is not needed.
        out.clear();

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
                ctx.stableBytes.data(),
                ctx.stableBytes.size(),
                ctx.ownedBytes,
                out,
                errorOut,
                mContinuationTarget
            ))
        {
            return internal::LineDecodeResult::Error;
        }
        // A row stores its fields by ascending key id, and the captures come in pattern order.
        std::sort(out.begin(), out.end(), [](const auto &a, const auto &b) { return a.Key() < b.Key(); });
        return internal::LineDecodeResult::Emit;
    }

    /** The key of the pattern's final source-order named group, or none when continuation is off. */
    [[nodiscard]] KeyId ContinuationTarget() const noexcept
    {
        return mContinuationTarget;
    }

private:
    const CompiledPattern *mCompiled;
    const std::vector<KeyId> *mColumnKeys;
    ContinuationMode mMode;
    KeyId mContinuationTarget;
    /** Non-owning header probe; null outside UntilNextHeader mode. */
    const CompiledPattern *mHeaderProbe = nullptr;
    Pcre2MatchDataPtr mMatchData;
    /**
     * Match data is tied to mHeaderProbe and cannot be shared with
     * the main pattern's match state.
     */
    Pcre2MatchDataPtr mHeaderMatchData;
};

static_assert(internal::LineDecoder<RegexLineDecoder>, "RegexLineDecoder must satisfy the LineDecoder concept");

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

RegexParser::RegexParser(RegexSettings settings)
    : mPattern(std::move(settings.pattern)),
      mContinuationMode(settings.continuation),
      mHeaderAnchor(std::move(settings.headerAnchor))
{
}

void RegexParser::Parse(StreamLineSource &source, LogParseSink &sink, ParserOptions options) const
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

void RegexParser::Parse(FileLineSource &source, LogParseSink &sink, ParserOptions options) const
{
    // An empty pattern fails with the same error as the streaming overload.
    const std::string &pattern = mPattern;

    if (pattern.empty())
    {
        EmitErrorAndFinish(sink, "Regex parser requires a non-empty pattern.");
        return;
    }

    // Compile once on the orchestrator thread; the pipeline
    // shares the compiled code read-only across the decoding workers.
    // RunStaticParserPipeline runs synchronously (joins every
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
    // outlives every decoding worker.
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

    // The named groups are interned up front, so the pipeline announces them as declared keys.
    internal::RunStaticParserPipeline<RegexLineDecoder>(
        source,
        sink,
        options,
        [&] { return RegexLineDecoder(compiled, columnKeys, staticContinuationMode, anchorPtr); },
        columnKeys
    );
}

} // namespace loglib
