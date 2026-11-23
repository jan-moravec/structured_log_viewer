/**
 * @brief A canonical text dump of everything a parser reports for one input, for golden-file tests.
 *
 * DumpFileParse and DumpStreamParse run a parser over an input through the file pipeline or the live-stream loop and
 * write what it reported as text: one line per row with the line id and every field as key, storage kind and value,
 * then the errors, the line offsets, the multi-line spans and the order in which keys were announced. The batches the
 * sink receives are joined, so the dump does not depend on when a parser flushed. The tests compare it with files kept
 * in the repository, so a change to a parser or to a driver shows up as a difference in the text.
 *
 * This is test support. It is not part of the library.
 */
#pragma once

#include "common.hpp"

#include <loglib/config/column.hpp>
#include <loglib/core/bytes_producer.hpp>
#include <loglib/core/compact_log_value.hpp>
#include <loglib/core/file_line_source.hpp>
#include <loglib/core/key_index.hpp>
#include <loglib/core/log_line.hpp>
#include <loglib/core/row_batch.hpp>
#include <loglib/core/stream_line_source.hpp>
#include <loglib/parse/log_parse_sink.hpp>
#include <loglib/parse/log_parser.hpp>
#include <loglib/parse/parser_options.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

namespace test_support
{

/** @brief What the dump of one run holds. */
struct ParseDump
{
    std::string text;
};

/** @brief Writes text with control characters and backslashes escaped, so one value stays on one line. */
[[nodiscard]] inline std::string EscapeForDump(std::string_view text)
{
    std::string out;
    for (const char c : text)
    {
        const auto byte = static_cast<unsigned char>(c);
        if (c == '\\')
        {
            out += "\\\\";
        }
        else if (c == '\n')
        {
            out += "\\n";
        }
        else if (c == '\r')
        {
            out += "\\r";
        }
        else if (c == '\t')
        {
            out += "\\t";
        }
        else if (byte < 0x20 || byte == 0x7F)
        {
            out += std::format("\\x{:02x}", byte);
        }
        else
        {
            out += c;
        }
    }
    return out;
}

[[nodiscard]] inline const char *TagName(loglib::CompactTag tag)
{
    switch (tag)
    {
    case loglib::CompactTag::Monostate:
        return "null";
    case loglib::CompactTag::MmapSlice:
        return "slice";
    case loglib::CompactTag::OwnedString:
        return "owned";
    case loglib::CompactTag::DictRef:
        return "dict";
    case loglib::CompactTag::Int64:
        return "int";
    case loglib::CompactTag::Uint64:
        return "uint";
    case loglib::CompactTag::Double:
        return "double";
    case loglib::CompactTag::Bool:
        return "bool";
    case loglib::CompactTag::Timestamp:
        return "time";
    }
    return "?";
}

[[nodiscard]] inline std::string ValueText(const loglib::LogValue &value)
{
    return std::visit(
        [](const auto &held) -> std::string {
            using T = std::decay_t<decltype(held)>;
            if constexpr (std::is_same_v<T, std::string_view> || std::is_same_v<T, std::string>)
            {
                return EscapeForDump(held);
            }
            else if constexpr (std::is_same_v<T, double>)
            {
                return std::format("{:.17g}", held);
            }
            else if constexpr (std::is_same_v<T, bool>)
            {
                return held ? "true" : "false";
            }
            else if constexpr (std::is_same_v<T, loglib::TimeStamp>)
            {
                return std::to_string(held.time_since_epoch().count());
            }
            else if constexpr (std::is_same_v<T, std::monostate>)
            {
                return "";
            }
            else
            {
                return std::to_string(held);
            }
        },
        value
    );
}

/** @brief A sink that keeps what every batch holds, in order, as text. */
class DumpSink final : public loglib::LogParseSink
{
public:
    loglib::KeyIndex &Keys() override
    {
        return mKeys;
    }

    void OnStarted() override
    {
        ++mStarted;
    }

    void OnBatch(loglib::RowBatch batch) override
    {
        for (const std::string &key : batch.newKeys)
        {
            mKeyOrder += key + "\n";
        }
        for (const loglib::LogLine &line : batch.lines)
        {
            std::string row = std::format("row {}:", line.LineId());
            for (const loglib::CompactLogValue &field : line.CompactValues())
            {
                row += std::format(
                    " {}={}:{}",
                    EscapeForDump(mKeys.KeyOf(field.Key())),
                    TagName(field.tag),
                    ValueText(line.GetValue(field.Key()))
                );
            }
            mRows += row + "\n";
        }
        for (const loglib::ParseError &error : batch.errors)
        {
            mErrors += std::format("error {}-{}: {}\n", error.firstLine, error.lastLine, EscapeForDump(error.message));
        }
        for (const std::uint64_t offset : batch.localLineOffsets)
        {
            mOffsets += std::format(" {}", offset);
        }
        for (const loglib::MultiLineRecordSpan &span : batch.multiLineSpans)
        {
            mSpans += std::format("span {}-{}\n", span.headerLineId, span.lastLineId);
        }
        mLastLine = batch.lastLineNumber;
        ++mBatches;
    }

    void OnFinished(bool cancelled) override
    {
        mCancelled = cancelled;
        ++mFinished;
    }

    [[nodiscard]] std::string Text() const
    {
        std::string text = "# rows\n" + mRows + "# errors\n" + mErrors + "# offsets\n" + mOffsets + "\n# spans\n" +
                           mSpans + "# keys\n" + mKeyOrder;
        text += std::format("# last line {}\n", mLastLine);
        text += std::format("# started {} finished {} cancelled {}\n", mStarted, mFinished, mCancelled);
        return text;
    }

private:
    loglib::KeyIndex mKeys;
    std::string mRows;
    std::string mErrors;
    std::string mOffsets;
    std::string mSpans;
    std::string mKeyOrder;
    size_t mLastLine = 0;
    size_t mBatches = 0;
    int mStarted = 0;
    int mFinished = 0;
    bool mCancelled = false;
};

/** @brief The options a golden run uses: fixed time, the given Time columns and no configuration. */
[[nodiscard]] inline loglib::ParserOptions DumpOptions(
    const std::vector<std::string> &timeKeys, unsigned threads, std::size_t batchBytes
)
{
    loglib::ParserOptions options;
    options.threads = threads;
    options.batchSizeBytes = batchBytes;
    options.referenceTime = std::chrono::sys_days{std::chrono::year{2026} / 6 / 15};
    for (const std::string &key : timeKeys)
    {
        options.columns.push_back(
            loglib::Column{
                .header = key,
                .keys = {key},
                .printFormat = "%F %H:%M:%S",
                .type = loglib::ColumnType::Time,
                .parseFormats = loglib::DefaultTimeParseFormats(),
                .autoDetect = false,
            }
        );
    }
    return options;
}

/** @brief Parses bytes through the file pipeline and returns the dump. */
[[nodiscard]] inline std::string DumpFileParse(
    const loglib::LogParser &parser,
    const std::string &bytes,
    const std::vector<std::string> &timeKeys,
    unsigned threads,
    std::size_t batchBytes
)
{
    const TestLogFile file("golden_dump_input.log");
    file.Write(bytes);
    const std::unique_ptr<loglib::FileLineSource> source = std::make_unique<loglib::FileLineSource>(file.GetFilePath());
    DumpSink sink;
    parser.Parse(*source, sink, DumpOptions(timeKeys, threads, batchBytes));
    return sink.Text();
}

/** @brief Parses bytes through the live-stream loop, read in chunks of chunkBytes, and returns the dump. */
[[nodiscard]] inline std::string DumpStreamParse(
    const loglib::LogParser &parser,
    const std::string &bytes,
    const std::vector<std::string> &timeKeys,
    size_t chunkBytes
)
{
    loglib::StreamLineSource source("memory.log", std::make_unique<InMemoryBytesProducer>(bytes, chunkBytes));
    DumpSink sink;
    parser.Parse(source, sink, DumpOptions(timeKeys, 0, 0));
    return sink.Text();
}

} // namespace test_support
