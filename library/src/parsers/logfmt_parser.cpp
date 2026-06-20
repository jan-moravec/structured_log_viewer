#include "loglib/parsers/logfmt_parser.hpp"

#include "loglib/file_line_source.hpp"
#include "loglib/internal/advanced_parser_options.hpp"
#include "loglib/internal/compact_log_value.hpp"
#include "loglib/internal/line_decoder.hpp"
#include "loglib/internal/static_parser_pipeline.hpp"
#include "loglib/internal/streaming_parse_loop.hpp"
#include "loglib/log_file.hpp"
#include "loglib/log_line.hpp"
#include "loglib/log_processing.hpp"
#include "loglib/stream_line_source.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// libc++ ships without `std::from_chars` for floating-point types as of
// LLVM 18 (Xcode 16); fall back to a locale-safe `strtod` on a stack
// buffer there. The extra headers are only pulled in on that branch.
#if !defined(__cpp_lib_to_chars) || __cpp_lib_to_chars < 201611L || defined(_LIBCPP_VERSION)
#include <array>
#include <cerrno>
#include <cstdlib>
#endif

namespace loglib
{

namespace
{

constexpr size_t INITIAL_FIELD_CAPACITY = 16;

/// Crossover at which `InsertSorted` switches from a linear back-scan to
/// `std::lower_bound`. Same threshold the JSON parser uses.
constexpr size_t INSERT_SORTED_LOWER_BOUND_THRESHOLD = 8;

/// Cap on bytes scanned by `IsValid` for the false-positive guard.
constexpr size_t IS_VALID_PROBE_BYTES = 16 * 1024;

/// Parse @p raw as a finite double. Wraps `std::from_chars` where
/// available and falls back to a strict-syntax `std::strtod` on a stack
/// buffer when libc++ lacks the floating-point overload (Xcode 16's
/// libc++ as of LLVM 18). The fallback first validates that @p raw
/// matches a strict C floating-point literal (no leading whitespace,
/// no hex, no `nan` / `inf` tokens) so the locale-driven decimal
/// separator in `strtod` cannot reinterpret the bytes.
bool TryParseFiniteDouble(std::string_view raw, double &outValue)
{
#if defined(__cpp_lib_to_chars) && __cpp_lib_to_chars >= 201611L && !defined(_LIBCPP_VERSION)
    const char *first = raw.data();
    const char *last = raw.data() + raw.size();
    const auto res = std::from_chars(first, last, outValue);
    return res.ec == std::errc{} && res.ptr == last && std::isfinite(outValue);
#else
    // Stack buffer cap for the strtod fallback. Any IEEE 754 double
    // round-trips in well under this width including sign / exponent /
    // padding; longer inputs are rejected rather than allocating.
    constexpr size_t DOUBLE_PARSE_BUFFER_SIZE = 64;

    if (raw.empty() || raw.size() >= DOUBLE_PARSE_BUFFER_SIZE)
    {
        return false;
    }

    size_t i = 0;
    if (raw[i] == '+' || raw[i] == '-')
    {
        ++i;
    }
    bool sawDigit = false;
    bool sawDot = false;
    while (i < raw.size())
    {
        const char c = raw[i];
        if (c >= '0' && c <= '9')
        {
            sawDigit = true;
            ++i;
            continue;
        }
        if (c == '.' && !sawDot)
        {
            sawDot = true;
            ++i;
            continue;
        }
        break;
    }
    if (!sawDigit)
    {
        return false;
    }
    if (i < raw.size() && (raw[i] == 'e' || raw[i] == 'E'))
    {
        ++i;
        if (i < raw.size() && (raw[i] == '+' || raw[i] == '-'))
        {
            ++i;
        }
        bool sawExpDigit = false;
        while (i < raw.size() && raw[i] >= '0' && raw[i] <= '9')
        {
            sawExpDigit = true;
            ++i;
        }
        if (!sawExpDigit)
        {
            return false;
        }
    }
    if (i != raw.size())
    {
        return false;
    }

    std::array<char, DOUBLE_PARSE_BUFFER_SIZE> buffer{};
    std::memcpy(buffer.data(), raw.data(), raw.size());
    buffer[raw.size()] = '\0';

    errno = 0;
    char *end = nullptr;
    const double value = std::strtod(buffer.data(), &end);
    if (errno == ERANGE || end != buffer.data() + raw.size() || !std::isfinite(value))
    {
        return false;
    }
    outValue = value;
    return true;
#endif
}

void InsertSorted(
    std::vector<std::pair<KeyId, internal::CompactLogValue>> &out, KeyId id, internal::CompactLogValue value
)
{
    if (out.size() < INSERT_SORTED_LOWER_BOUND_THRESHOLD)
    {
        auto it = out.end();
        while (it != out.begin())
        {
            auto prev = it - 1;
            if (prev->first < id)
            {
                break;
            }
            if (prev->first == id)
            {
                prev->second = value;
                return;
            }
            it = prev;
        }
        out.emplace(it, id, value);
        return;
    }

    auto it = std::lower_bound(
        out.begin(),
        out.end(),
        id,
        [](const std::pair<KeyId, internal::CompactLogValue> &lhs, KeyId rhs) { return lhs.first < rhs; }
    );
    if (it != out.end() && it->first == id)
    {
        it->second = value;
        return;
    }
    out.emplace(it, id, value);
}

/// Promote @p sv to a compact value. `MmapSlice` (zero copy) when it
/// points inside `[fileBegin, fileBegin + fileSize)`, otherwise the
/// bytes are copied into @p ownedArena and tagged `OwnedString`.
internal::CompactLogValue MakeStringCompact(
    std::string_view sv, const char *fileBegin, size_t fileSize, std::string &ownedArena
)
{
    // Streaming passes `fileBegin == nullptr`; gate the range check
    // on a real base because relational compares across unrelated
    // objects are UB.
    if (fileBegin != nullptr && sv.data() >= fileBegin && sv.data() + sv.size() <= fileBegin + fileSize)
    {
        const auto offset = static_cast<uint64_t>(sv.data() - fileBegin);
        return internal::CompactLogValue::MakeMmapSlice(offset, static_cast<uint32_t>(sv.size()));
    }
    const uint64_t offset = ownedArena.size();
    ownedArena.append(sv.data(), sv.size());
    return internal::CompactLogValue::MakeOwnedString(offset, static_cast<uint32_t>(sv.size()));
}

/// One key/value pair from a single record.
/// `valueWasQuoted`: disables typed-value detection (`"42"` stays a string).
/// `unescapedFromQuoted`: value bytes live in the decoder's scratch
/// buffer (not aliasing the source), so they must be copied into the arena.
struct LogfmtField
{
    std::string_view key;
    std::string_view value;
    bool valueWasQuoted = false;
    bool unescapedFromQuoted = false;
    bool valueIsNull = false;
};

/// State machine ported from `kr/logfmt`'s `scanner.go`
/// (https://github.com/kr/logfmt/blob/19f9bcb100e6/scanner.go).
/// Walks @p line and emits each `key=value` (or bare-key) field via
/// @p emit. Returns false on an unterminated quoted value; fields
/// emitted before that point are kept. @p quotedScratch is reused
/// across calls to hold unescaped quoted bytes.
template <class Emit> bool TokenizeLogfmtLine(std::string_view line, std::string &quotedScratch, Emit emit)
{
    const char *const data = line.data();
    const size_t end = line.size();
    size_t i = 0;

    auto isSpace = [](unsigned char c) noexcept {
        return c == ' ' || c == '\t' || c == '\v' || c == '\f' || c == '\r';
    };

    while (i < end)
    {
        // Skip inter-pair whitespace and any trailing junk after a previous pair.
        while (i < end && static_cast<unsigned char>(data[i]) <= ' ')
        {
            ++i;
        }
        if (i >= end)
        {
            return true;
        }

        // Read a key: printable ASCII excluding '=' / '"' / whitespace.
        const size_t keyStart = i;
        while (i < end)
        {
            const auto c = static_cast<unsigned char>(data[i]);
            if (c <= ' ' || c == '=' || c == '"')
            {
                break;
            }
            ++i;
        }
        const std::string_view key(data + keyStart, i - keyStart);
        if (key.empty())
        {
            // Stray '=' or '"' at the start — skip it so a single junk
            // byte cannot stall the scanner.
            ++i;
            continue;
        }

        // No '=' -> bare key, null value.
        if (i >= end || data[i] != '=')
        {
            LogfmtField field;
            field.key = key;
            field.valueIsNull = true;
            emit(field);
            continue;
        }
        ++i; // consume '='

        // Empty value: `key=` followed by whitespace or EOL is null.
        if (i >= end || isSpace(static_cast<unsigned char>(data[i])) || data[i] == '\n')
        {
            LogfmtField field;
            field.key = key;
            field.valueIsNull = true;
            emit(field);
            continue;
        }

        if (data[i] == '"')
        {
            // Quoted value: walk until matching '"' honouring backslash escapes.
            ++i;
            const size_t innerStart = i;
            bool sawEscape = false;
            bool terminated = false;
            while (i < end)
            {
                const char c = data[i];
                if (c == '\\')
                {
                    sawEscape = true;
                    if (i + 1 < end)
                    {
                        i += 2;
                        continue;
                    }
                    // Trailing backslash with no escape target — treat as unterminated.
                    i = end;
                    break;
                }
                if (c == '"')
                {
                    terminated = true;
                    break;
                }
                ++i;
            }
            if (!terminated)
            {
                // Surface the error without emitting a half-formed
                // field; callers wrap with a line number.
                return false;
            }

            const std::string_view rawInner(data + innerStart, i - innerStart);
            ++i; // consume closing '"'

            LogfmtField field;
            field.key = key;
            field.valueWasQuoted = true;
            if (!sawEscape)
            {
                field.value = rawInner;
            }
            else
            {
                quotedScratch.clear();
                quotedScratch.reserve(rawInner.size());
                for (size_t j = 0; j < rawInner.size(); ++j)
                {
                    const char c = rawInner[j];
                    if (c == '\\' && j + 1 < rawInner.size())
                    {
                        const char next = rawInner[j + 1];
                        switch (next)
                        {
                        case 'n':
                            quotedScratch.push_back('\n');
                            break;
                        case 'r':
                            quotedScratch.push_back('\r');
                            break;
                        case 't':
                            quotedScratch.push_back('\t');
                            break;
                        case '"':
                            quotedScratch.push_back('"');
                            break;
                        case '\\':
                            quotedScratch.push_back('\\');
                            break;
                        default:
                            // Unknown escape: pass both bytes through
                            // (matches Go's `unquoteBytes` fallback).
                            quotedScratch.push_back('\\');
                            quotedScratch.push_back(next);
                            break;
                        }
                        ++j;
                    }
                    else
                    {
                        quotedScratch.push_back(c);
                    }
                }
                field.value = std::string_view(quotedScratch);
                field.unescapedFromQuoted = true;
            }
            emit(field);
            continue;
        }

        // Bare value: printable ASCII excluding '"' / whitespace.
        // (kr/logfmt permits '=' inside bare values; we keep that.)
        const size_t valueStart = i;
        while (i < end)
        {
            const auto c = static_cast<unsigned char>(data[i]);
            if (c <= ' ' || c == '"')
            {
                break;
            }
            ++i;
        }
        LogfmtField field;
        field.key = key;
        field.value = std::string_view(data + valueStart, i - valueStart);
        emit(field);
    }
    return true;
}

/// Typed-value classifier for bare values. Quoted values bypass this
/// and stay strings (so `pid="42"` keeps the user's intent).
internal::CompactLogValue ClassifyBareValue(
    std::string_view raw, const char *fileBegin, size_t fileSize, std::string &ownedArena
)
{
    if (raw.empty())
    {
        return internal::CompactLogValue::MakeMonostate();
    }

    if (raw == "true")
    {
        return internal::CompactLogValue::MakeBool(true);
    }
    if (raw == "false")
    {
        return internal::CompactLogValue::MakeBool(false);
    }

    // Probe int / uint / double via std::from_chars. Trailing junk
    // (e.g. `42abc`) is rejected and falls through to string.
    const char *first = raw.data();
    const char *last = raw.data() + raw.size();

    if (raw.front() == '-')
    {
        int64_t i64 = 0;
        const auto res = std::from_chars(first, last, i64);
        if (res.ec == std::errc{} && res.ptr == last)
        {
            return internal::CompactLogValue::MakeInt64(i64);
        }
    }
    else if (raw.front() >= '0' && raw.front() <= '9')
    {
        // Unsigned first so large positive ids stay exact.
        uint64_t u64 = 0;
        const auto res = std::from_chars(first, last, u64);
        if (res.ec == std::errc{} && res.ptr == last)
        {
            return internal::CompactLogValue::MakeUint64(u64);
        }
    }

    if (raw.front() == '-' || raw.front() == '+' || raw.front() == '.' || (raw.front() >= '0' && raw.front() <= '9'))
    {
        double d = 0.0;
        if (TryParseFiniteDouble(raw, d))
        {
            return internal::CompactLogValue::MakeDouble(d);
        }
    }

    return MakeStringCompact(raw, fileBegin, fileSize, ownedArena);
}

/// Parse one logfmt record into compact values.
/// `fileBegin`/`fileSize` describe the mmap (or 0/nullptr when
/// streaming). `ownedArena` is the per-batch buffer for
/// `OwnedString` payloads. `quotedScratch` is reused across calls.
void ParseLogfmtLine(
    std::string_view line,
    KeyIndex &keys,
    internal::PerWorkerKeyCache *keyCache,
    const char *fileBegin,
    size_t fileSize,
    std::string &ownedArena,
    std::string &quotedScratch,
    std::vector<std::pair<KeyId, internal::CompactLogValue>> &out,
    bool &outUnterminated
)
{
    out.clear();
    out.reserve(INITIAL_FIELD_CAPACITY);

    auto emit = [&](const LogfmtField &field) {
        const KeyId keyId = internal::InternKeyVia(field.key, keys, keyCache);

        if (field.valueIsNull)
        {
            InsertSorted(out, keyId, internal::CompactLogValue::MakeMonostate());
            return;
        }

        if (field.valueWasQuoted)
        {
            // `unescapedFromQuoted` -> bytes are in `quotedScratch`
            // (which the next field overwrites), so copy into the
            // arena. Otherwise the view aliases the source (mmap or
            // streaming carry buffer) and `MakeStringCompact` picks
            // the right tag.
            if (field.unescapedFromQuoted)
            {
                const uint64_t offset = ownedArena.size();
                ownedArena.append(field.value.data(), field.value.size());
                InsertSorted(
                    out,
                    keyId,
                    internal::CompactLogValue::MakeOwnedString(offset, static_cast<uint32_t>(field.value.size()))
                );
            }
            else
            {
                InsertSorted(out, keyId, MakeStringCompact(field.value, fileBegin, fileSize, ownedArena));
            }
            return;
        }

        InsertSorted(out, keyId, ClassifyBareValue(field.value, fileBegin, fileSize, ownedArena));
    };

    outUnterminated = !TokenizeLogfmtLine(line, quotedScratch, emit);
}

bool LineLooksLikeLogfmt(std::string_view line)
{
    // Reject obvious JSON so JSON wins the auto-detect race.
    if (!line.empty() && (line.front() == '{' || line.front() == '['))
    {
        return false;
    }

    // Cheapest stable signal: an '=' after at least one key byte.
    size_t i = 0;
    while (i < line.size() && static_cast<unsigned char>(line[i]) <= ' ')
    {
        ++i;
    }
    const size_t keyStart = i;
    while (i < line.size())
    {
        const auto c = static_cast<unsigned char>(line[i]);
        if (c <= ' ' || c == '=' || c == '"')
        {
            break;
        }
        ++i;
    }
    return i != keyStart && i < line.size() && line[i] == '=';
}

/// Logfmt-specific per-worker scratch.
struct LogfmtWorkerState
{
    std::string quotedScratch;
};

/// Stage A token: a contiguous byte range of the mmap covering complete lines.
struct LogfmtByteRange
{
    uint64_t batchIndex = 0;
    const char *bytesBegin = nullptr;
    const char *bytesEnd = nullptr;
    const char *fileEnd = nullptr;
};

void DecodeLogfmtBatch(
    const LogfmtByteRange &batch,
    internal::WorkerScratch<LogfmtWorkerState> &worker,
    KeyIndex &keys,
    FileLineSource &source,
    std::span<const internal::TimeColumnSpec> timeColumns,
    internal::ParsedPipelineBatch &parsed
)
{
    parsed.batchIndex = batch.batchIndex;

    const char *cursor = batch.bytesBegin;
    const char *end = batch.bytesEnd;
    const char *fileEnd = batch.fileEnd;
    const char *fileBegin = source.File().Data();

    const auto batchBytes = static_cast<size_t>(end - cursor);
    const size_t estimatedLines = (batchBytes / 64) + 1;
    parsed.lines.reserve(estimatedLines);
    parsed.localLineOffsets.reserve(estimatedLines);

    size_t relativeLineNumber = 1;

    std::vector<std::pair<KeyId, internal::CompactLogValue>> values;

    while (cursor < end)
    {
        const char *lineStart = cursor;
        const char *newline = static_cast<const char *>(memchr(cursor, '\n', static_cast<size_t>(end - cursor)));
        const char *lineEnd = newline ? newline : end;
        cursor = (newline != nullptr) ? newline + 1 : end;

        std::string_view line(lineStart, static_cast<size_t>(lineEnd - lineStart));
        if (!line.empty() && line.back() == '\r')
        {
            line.remove_suffix(1);
        }

        // `GetLine` subtracts 1 (the '\n'); for an unterminated last
        // line push `fileSize + 1` so we don't lose the final char.
        const uint64_t nextOffset = static_cast<uint64_t>(cursor - fileBegin) + (newline == nullptr ? 1u : 0u);
        parsed.localLineOffsets.push_back(nextOffset);

        if (line.empty())
        {
            relativeLineNumber++;
            continue;
        }

        try
        {
            const auto fileSize = static_cast<size_t>(fileEnd - fileBegin);
            bool unterminated = false;
            ParseLogfmtLine(
                line,
                keys,
                &worker.keyCache,
                fileBegin,
                fileSize,
                parsed.ownedStringsArena,
                worker.user.quotedScratch,
                values,
                unterminated
            );
            if (unterminated)
            {
                parsed.errors.push_back(
                    internal::ParsedLineError{.relativeLine = relativeLineNumber, .body = "Unterminated quoted value."}
                );
                relativeLineNumber++;
                continue;
            }
            if (values.empty())
            {
                // No `key=value` pairs -> not a logfmt record;
                // surface it so the caller sees why the row is empty.
                parsed.errors.push_back(
                    internal::ParsedLineError{.relativeLine = relativeLineNumber, .body = "Not a logfmt record."}
                );
                relativeLineNumber++;
                continue;
            }

            LogLine logLine(std::move(values), keys, source, relativeLineNumber - 1);
            parsed.lines.push_back(std::move(logLine));

            // Inline promotion: same shape as the JSON parser.
            worker.PromoteTimestamps(parsed.lines.back(), timeColumns, std::string_view(parsed.ownedStringsArena));

            // Reset the moved-from vector for the next line.
            values.clear();
        }
        catch (const std::exception &e)
        {
            parsed.errors.push_back(
                internal::ParsedLineError{.relativeLine = relativeLineNumber, .body = std::string(e.what())}
            );
        }

        relativeLineNumber++;
    }

    parsed.totalLineCount = relativeLineNumber - 1;
}

/// Logfmt record decoder for `RunStreamingParseLoop`. Owns the
/// per-line scratch. Satisfies `CompactLineDecoder`.
class LogfmtLineDecoder
{
public:
    LogfmtLineDecoder() = default;

    bool DecodeCompact(
        std::string_view line,
        KeyIndex &keys,
        internal::PerWorkerKeyCache *keyCache,
        std::vector<std::pair<KeyId, internal::CompactLogValue>> &out,
        std::string &outOwnedArena,
        std::string &errorOut
    )
    {
        out.clear();
        outOwnedArena.clear();
        if (line.empty())
        {
            return true;
        }

        try
        {
            bool unterminated = false;
            // Streaming bytes aren't in any mmap, so passing
            // `fileBegin/fileSize=0` forces the `OwnedString` arena
            // copy in `MakeStringCompact`.
            ParseLogfmtLine(
                line,
                keys,
                keyCache,
                /* fileBegin = */ nullptr,
                /* fileSize = */ 0,
                outOwnedArena,
                mQuotedScratch,
                out,
                unterminated
            );
            if (unterminated)
            {
                errorOut = "Unterminated quoted value.";
                return false;
            }
            if (out.empty())
            {
                errorOut = "Not a logfmt record.";
                return false;
            }
            return true;
        }
        catch (const std::exception &e)
        {
            errorOut = std::string(e.what());
            return false;
        }
    }

private:
    std::string mQuotedScratch;
};

static_assert(
    internal::CompactLineDecoder<LogfmtLineDecoder>, "LogfmtLineDecoder must satisfy the CompactLineDecoder concept"
);

/// True if @p value can serialise as a bare logfmt value
/// (no whitespace, no '"', no '=', no '\\').
bool BareValueIsSafe(std::string_view value)
{
    if (value.empty())
    {
        return false;
    }
    return std::ranges::all_of(value, [](char c) {
        const auto uc = static_cast<unsigned char>(c);
        return uc > ' ' && uc != '"' && uc != '=' && uc != '\\';
    });
}

void AppendQuotedString(std::string &out, std::string_view value)
{
    out.push_back('"');
    for (const char c : value)
    {
        switch (c)
        {
        case '"':
            out.append("\\\"");
            break;
        case '\\':
            out.append("\\\\");
            break;
        case '\n':
            out.append("\\n");
            break;
        case '\r':
            out.append("\\r");
            break;
        case '\t':
            out.append("\\t");
            break;
        default:
            out.push_back(c);
            break;
        }
    }
    out.push_back('"');
}

void AppendValueAsString(std::string &out, const LogValue &value)
{
    std::visit(
        [&out](const auto &val) {
            using T = std::decay_t<decltype(val)>;
            if constexpr (std::is_same_v<T, std::monostate>)
            {
                // Empty `key=` round-trips back to monostate.
            }
            else if constexpr (std::is_same_v<T, TimeStamp>)
            {
                AppendQuotedString(out, TimeStampToDateTimeString(val));
            }
            else if constexpr (std::is_same_v<T, std::string_view>)
            {
                if (BareValueIsSafe(val))
                {
                    out.append(val.data(), val.size());
                }
                else
                {
                    AppendQuotedString(out, val);
                }
            }
            else if constexpr (std::is_same_v<T, std::string>)
            {
                if (BareValueIsSafe(val))
                {
                    out.append(val);
                }
                else
                {
                    AppendQuotedString(out, val);
                }
            }
            else if constexpr (std::is_same_v<T, bool>)
            {
                out.append(val ? "true" : "false");
            }
            else if constexpr (std::is_same_v<T, int64_t> || std::is_same_v<T, uint64_t>)
            {
                out.append(fmt::format("{}", val));
            }
            else if constexpr (std::is_same_v<T, double>)
            {
                // Non-finite doubles (`nan`, `inf`, `-inf`) print as
                // bare alphabetic tokens, which the parser would re-read
                // as strings. Quote them so the round-trip preserves a
                // recognisable string rather than silently changing kind.
                if (std::isfinite(val))
                {
                    out.append(fmt::format("{}", val));
                }
                else
                {
                    AppendQuotedString(out, fmt::format("{}", val));
                }
            }
        },
        value
    );
}

} // namespace

bool LogfmtParser::IsValid(const std::filesystem::path &file) const
{
    std::ifstream stream(file);
    if (!stream.is_open())
    {
        return false;
    }

    std::string line;
    size_t bytesScanned = 0;
    while (std::getline(stream, line))
    {
        if (!line.empty() && line.back() == '\r')
        {
            line.pop_back();
        }
        bytesScanned += line.size() + 1;
        if (line.empty())
        {
            if (bytesScanned >= IS_VALID_PROBE_BYTES)
            {
                return false;
            }
            continue;
        }
        return LineLooksLikeLogfmt(line);
    }

    return false;
}

std::string LogfmtParser::ToString(const LogLine &line) const
{
    const auto values = line.IndexedValues();
    if (values.empty())
    {
        return {};
    }

    const auto &keys = line.Keys();
    std::string out;
    bool first = true;
    for (const auto &entry : values)
    {
        if (!first)
        {
            out.push_back(' ');
        }
        first = false;
        const std::string_view key = keys.KeyOf(entry.first);
        out.append(key.data(), key.size());
        out.push_back('=');
        AppendValueAsString(out, entry.second);
    }
    return out;
}

std::string LogfmtParser::ToString(const LogMap &values)
{
    if (values.empty())
    {
        return {};
    }

    // Lexicographic by key for round-trip determinism.
    std::vector<const std::pair<const std::string, LogValue> *> sorted;
    sorted.reserve(values.size());
    for (const auto &kv : values)
    {
        sorted.push_back(&kv);
    }
    std::sort(sorted.begin(), sorted.end(), [](const auto *lhs, const auto *rhs) { return lhs->first < rhs->first; });

    std::string out;
    bool first = true;
    for (const auto *kv : sorted)
    {
        if (!first)
        {
            out.push_back(' ');
        }
        first = false;
        out.append(kv->first);
        out.push_back('=');
        AppendValueAsString(out, kv->second);
    }
    return out;
}

void LogfmtParser::ParseStreaming(StreamLineSource &source, LogParseSink &sink, ParserOptions options) const
{
    LogfmtLineDecoder decoder;
    internal::RunStreamingParseLoop(source, decoder, sink, options);
}

void LogfmtParser::ParseStreaming(FileLineSource &source, LogParseSink &sink, ParserOptions options) const
{
    ParseStreaming(source, sink, options, internal::AdvancedParserOptions{});
}

void LogfmtParser::ParseStreaming(
    FileLineSource &source, LogParseSink &sink, const ParserOptions &options, internal::AdvancedParserOptions advanced
)
{
    const LogFile &file = source.File();
    const size_t batchSize = advanced.batchSizeBytes != 0 ? advanced.batchSizeBytes
                                                          : internal::AdvancedParserOptions::DEFAULT_BATCH_SIZE_BYTES;

    const char *fileBegin = file.Data();
    const size_t fileSize = file.Size();
    const char *fileEnd = (fileBegin != nullptr) ? fileBegin + fileSize : nullptr;
    const char *cursor = fileBegin;
    uint64_t batchIndex = 0;

    auto stageA = [cursor, fileEnd, batchSize, batchIndex](LogfmtByteRange &out) mutable -> bool {
        if (cursor >= fileEnd)
        {
            return false;
        }
        const char *batchBegin = cursor;
        const auto remaining = static_cast<size_t>(fileEnd - cursor);
        const size_t advance = std::min(batchSize, remaining);
        const char *target = cursor + advance;
        if (advance < remaining)
        {
            const char *newline =
                static_cast<const char *>(memchr(target, '\n', static_cast<size_t>(fileEnd - target)));
            cursor = (newline != nullptr) ? newline + 1 : fileEnd;
        }
        else
        {
            cursor = fileEnd;
        }

        out.batchIndex = batchIndex++;
        out.bytesBegin = batchBegin;
        out.bytesEnd = cursor;
        out.fileEnd = fileEnd;
        return true;
    };

    FileLineSource *sourcePtr = &source;
    auto stageB = [sourcePtr](
                      LogfmtByteRange token,
                      internal::WorkerScratch<LogfmtWorkerState> &worker,
                      KeyIndex &keys,
                      std::span<const internal::TimeColumnSpec> timeColumns,
                      internal::ParsedPipelineBatch &parsed
                  ) { DecodeLogfmtBatch(token, worker, keys, *sourcePtr, timeColumns, parsed); };

    internal::RunStaticParserPipeline<LogfmtByteRange, LogfmtWorkerState>(
        source, sink, options, advanced, stageA, stageB
    );
}

} // namespace loglib
