/**
 * @brief Helpers shared by the fuzz targets.
 *
 * Every fuzz target is a function that takes arbitrary bytes and must neither crash nor hang. The parser targets all
 * need the same setup: a line source over the input bytes, a sink that reads every value the parser produced, and
 * options that keep a run deterministic. This header holds that setup so each target stays a few lines long.
 *
 * This is a test helper. It is not part of the library.
 */
#pragma once

#include "loglib/core/file_line_source.hpp"
#include "loglib/core/key_index.hpp"
#include "loglib/core/row_batch.hpp"
#include "loglib/parse/log_parse_sink.hpp"
#include "loglib/parse/log_parser.hpp"
#include "loglib/parse/parser_options.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace loglib::fuzz
{

/** @brief Views the fuzzer input as text. A null pointer with a zero size gives an empty view. */
[[nodiscard]] inline std::string_view AsText(const std::uint8_t *data, std::size_t size) noexcept
{
    return size == 0 ? std::string_view{} : std::string_view{reinterpret_cast<const char *>(data), size};
}

/**
 * @brief A sink that reads every byte of every value and of every raw line, so a bad offset or length becomes a read
 * that sanitizers can see.
 *
 * A row stores its strings as offsets into the source, and building a string view from them reads nothing. The sink
 * therefore folds each byte into a checksum, and ParseBytes stores the checksum where the optimizer cannot drop it.
 */
class ReadingSink final : public LogParseSink
{
public:
    KeyIndex &Keys() override
    {
        return mKeys;
    }

    void OnStarted() override
    {
    }

    void OnBatch(RowBatch batch) override
    {
        for (const LogLine &line : batch.lines)
        {
            for (const auto &[key, value] : line.IndexedValues())
            {
                mChecksum += key;
                if (const auto *view = std::get_if<std::string_view>(&value))
                {
                    Fold(*view);
                }
                else if (const auto *owned = std::get_if<std::string>(&value))
                {
                    Fold(*owned);
                }
            }
            for (const std::string &name : line.GetKeys(mKeys))
            {
                Fold(name);
            }
            Fold(line.Source()->RawLine(line.LineId()));
        }
        mRowCount += batch.lines.size();
        mErrorCount += batch.errors.size();
    }

    void OnFinished(bool /*cancelled*/) override
    {
    }

    [[nodiscard]] std::size_t RowCount() const noexcept
    {
        return mRowCount;
    }

    /** @brief A sum over every byte read, so the reads have a visible result. */
    [[nodiscard]] std::size_t Checksum() const noexcept
    {
        return mChecksum;
    }

private:
    void Fold(std::string_view bytes) noexcept
    {
        for (const char byte : bytes)
        {
            mChecksum = (mChecksum * 31U) + static_cast<unsigned char>(byte);
        }
    }

    KeyIndex mKeys;
    std::size_t mRowCount = 0;
    std::size_t mErrorCount = 0;
    std::size_t mChecksum = 0;
};

/**
 * @brief Runs a parser over the bytes as an in-memory file and reads everything it produced.
 *
 * One worker thread and a tiny batch size keep a run reproducible while still crossing batch boundaries.
 */
inline void ParseBytes(const LogParser &parser, const std::uint8_t *data, std::size_t size)
{
    const std::string_view text = AsText(data, size);
    (void)parser.IsValidBytes(text);

    const std::unique_ptr<FileLineSource> source = FileLineSource::FromBytes(std::string{text}, "fuzz-input");
    ReadingSink sink;
    ParserOptions options;
    options.threads = 1;
    options.batchSizeBytes = 64;
    parser.ParseStreaming(*source, sink, std::move(options));

    // A volatile store keeps the optimizer from removing the reads behind the checksum.
    static volatile std::size_t observed = 0;
    observed = sink.Checksum();
}

} // namespace loglib::fuzz
