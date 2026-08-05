#include "loglib/session_bundle.hpp"

#include "loglib/internal/log_configuration_glaze_meta.hpp"
#include "loglib/internal/normalized_json_row.hpp"
#include "loglib/internal/path_encoding.hpp"
#include "loglib/line_source.hpp"
#include "loglib/log_data.hpp"
#include "loglib/log_line.hpp"
#include "loglib/log_table.hpp"

#include <glaze/glaze.hpp>
#include <zstd.h>

#include <cerrno>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace loglib
{
namespace
{

constexpr std::size_t MAX_RECORD_BYTES = 256U * 1024U * 1024U;
constexpr std::size_t MAX_METADATA_BYTES = 64U * 1024U * 1024U;
constexpr std::size_t MAX_BUNDLE_ROWS = 1'000'000'000U;
constexpr std::size_t PROGRESS_INTERVAL_ROWS = 4096;

void PollStop(const StopToken &token)
{
    if (token.stop_requested())
    {
        throw SessionBundleCancelled{};
    }
}

class FileHandle
{
public:
    explicit FileHandle(const std::filesystem::path &path)
        : mPath(path), mFile(internal::OpenFileForBinaryWrite(path))
    {
        if (mFile == nullptr)
        {
            throw std::runtime_error(
                "Session bundle: failed to open '" + internal::PathToUtf8(path) +
                "' for writing (errno " + std::to_string(errno) + ")"
            );
        }
    }

    ~FileHandle()
    {
        if (mFile != nullptr)
        {
            (void)std::fclose(mFile);
        }
    }

    FileHandle(const FileHandle &) = delete;
    FileHandle &operator=(const FileHandle &) = delete;

    void Write(std::string_view bytes)
    {
        if (!bytes.empty() && std::fwrite(bytes.data(), 1, bytes.size(), mFile) != bytes.size())
        {
            throw std::runtime_error(
                "Session bundle: short write to '" + internal::PathToUtf8(mPath) + "'"
            );
        }
    }

    void Close()
    {
        if (mFile == nullptr)
        {
            return;
        }
        if (std::fflush(mFile) != 0 || std::fclose(mFile) != 0)
        {
            mFile = nullptr;
            throw std::runtime_error(
                "Session bundle: failed to flush '" + internal::PathToUtf8(mPath) + "'"
            );
        }
        mFile = nullptr;
    }

private:
    std::filesystem::path mPath;
    std::FILE *mFile = nullptr;
};

class ZstdWriter
{
public:
    ZstdWriter(FileHandle &file, const SessionBundleWriteOptions &options)
        : mFile(file), mContext(ZSTD_createCCtx()), mOutput(ZSTD_CStreamOutSize())
    {
        if (mContext == nullptr)
        {
            throw std::runtime_error("Session bundle: ZSTD_createCCtx failed");
        }
        SetParameter(ZSTD_c_compressionLevel, options.compressionLevel);
        SetParameter(ZSTD_c_checksumFlag, 1);
        if (options.totalWorkers > 0)
        {
            const std::size_t result = ZSTD_CCtx_setParameter(mContext, ZSTD_c_nbWorkers, options.totalWorkers);
            if (ZSTD_isError(result))
            {
                throw std::runtime_error(
                    std::string("Session bundle: zstd worker configuration failed: ") +
                    ZSTD_getErrorName(result)
                );
            }
        }
    }

    ~ZstdWriter()
    {
        if (mContext != nullptr)
        {
            (void)ZSTD_freeCCtx(mContext);
        }
    }

    ZstdWriter(const ZstdWriter &) = delete;
    ZstdWriter &operator=(const ZstdWriter &) = delete;

    void Write(std::string_view bytes)
    {
        ZSTD_inBuffer input{bytes.data(), bytes.size(), 0};
        while (input.pos < input.size)
        {
            Drain(input, ZSTD_e_continue);
        }
    }

    void Finish()
    {
        ZSTD_inBuffer input{nullptr, 0, 0};
        std::size_t remaining = 1;
        while (remaining != 0)
        {
            remaining = Drain(input, ZSTD_e_end);
        }
    }

private:
    void SetParameter(ZSTD_cParameter parameter, int value)
    {
        const std::size_t result = ZSTD_CCtx_setParameter(mContext, parameter, value);
        if (ZSTD_isError(result))
        {
            throw std::runtime_error(
                std::string("Session bundle: zstd configuration failed: ") + ZSTD_getErrorName(result)
            );
        }
    }

    std::size_t Drain(ZSTD_inBuffer &input, ZSTD_EndDirective directive)
    {
        ZSTD_outBuffer output{mOutput.data(), mOutput.size(), 0};
        const std::size_t result = ZSTD_compressStream2(mContext, &output, &input, directive);
        if (ZSTD_isError(result))
        {
            throw std::runtime_error(
                std::string("Session bundle: zstd compression failed: ") + ZSTD_getErrorName(result)
            );
        }
        mFile.Write({mOutput.data(), output.pos});
        return result;
    }

    FileHandle &mFile;
    ZSTD_CCtx *mContext = nullptr;
    std::vector<char> mOutput;
};

void RemapAnchors(
    LogConfiguration &configuration,
    const std::vector<LogLine> &lines,
    std::string_view flattenedLocator
)
{
    std::vector<LogConfiguration::AnchorEntry> remapped;
    remapped.reserve(configuration.anchors.size());
    for (const auto &anchor : configuration.anchors)
    {
        std::optional<std::uint64_t> denseId;
        for (std::size_t row = 0; row < lines.size(); ++row)
        {
            const LogLine &line = lines[row];
            if (line.LineId() != anchor.lineId || line.Source() == nullptr)
            {
                continue;
            }
            const std::string sourceLocator = internal::PathToUtf8(line.Source()->Path());
            if (anchor.locator != sourceLocator && !(anchor.locator.empty() && sourceLocator.empty()))
            {
                continue;
            }
            if (denseId.has_value())
            {
                denseId.reset();
                break;
            }
            denseId = static_cast<std::uint64_t>(row);
        }
        if (!denseId.has_value())
        {
            continue;
        }
        auto copy = anchor;
        copy.locator.assign(flattenedLocator);
        copy.lineId = *denseId;
        remapped.push_back(std::move(copy));
    }
    configuration.anchors = std::move(remapped);
}

std::string SerializeCompactConfiguration(const LogConfiguration &configuration)
{
    std::string json;
    const auto error = glz::write_json(configuration, json);
    if (error)
    {
        throw std::runtime_error(
            "Session bundle: failed to serialize configuration: " + glz::format_error(error, json)
        );
    }
    return json;
}

void ReplaceAtomically(const std::filesystem::path &temporary, const std::filesystem::path &destination)
{
#ifdef _WIN32
    if (::MoveFileExW(
            temporary.c_str(),
            destination.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH
        ) == 0)
    {
        throw std::runtime_error(
            "Session bundle: atomic replacement failed with Windows error " +
            std::to_string(static_cast<unsigned long>(::GetLastError()))
        );
    }
#else
    std::error_code error;
    std::filesystem::rename(temporary, destination, error);
    if (error)
    {
        throw std::runtime_error("Session bundle: atomic replacement failed: " + error.message());
    }
#endif
}

} // namespace

void WriteSessionBundle(
    const LogTable &table,
    const LogConfiguration &configuration,
    const std::filesystem::path &destination,
    const SessionBundleWriteOptions &options
)
{
    PollStop(options.stopToken);
    if (destination.empty() || destination.filename().empty())
    {
        throw std::invalid_argument("WriteSessionBundle requires a destination file");
    }

    const auto &lines = table.Data().Lines();
    const auto &keys = table.Data().Keys();
    if (lines.size() > MAX_BUNDLE_ROWS)
    {
        throw std::length_error("Session bundle row count exceeds the one-billion-row limit");
    }

    LogConfiguration embedded = configuration;
    const std::string physicalLocator = internal::PathToUtf8(destination);
    RemapAnchors(embedded, lines, physicalLocator);
    embedded.source = LogConfiguration::Source{
        .kind = LogConfiguration::Source::Kind::File,
        .format = LogConfiguration::Source::Format::Json,
        .locators = {physicalLocator},
        .locatorDedupKeys = {physicalLocator},
        .regexPattern = {},
    };

    const std::string configJson = SerializeCompactConfiguration(embedded);
    std::string metadata =
        "{\"__slv_bundle__\":{\"formatVersion\":1,\"rowCount\":" +
        std::to_string(static_cast<std::uint64_t>(lines.size())) +
        ",\"configuration\":" + configJson + "}}\n";
    if (metadata.size() > MAX_METADATA_BYTES)
    {
        throw std::length_error("Session bundle metadata exceeds the 64 MiB limit");
    }

    std::filesystem::path temporary = destination;
    temporary += ".tmp";
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);

    try
    {
        FileHandle file(temporary);
        ZstdWriter writer(file, options);
        writer.Write(metadata);

        for (std::size_t row = 0; row < lines.size(); ++row)
        {
            if ((row % PROGRESS_INTERVAL_ROWS) == 0)
            {
                PollStop(options.stopToken);
            }
            std::string json = internal::SerializeNormalizedJsonRow(lines[row], keys);
            if (json.size() > MAX_RECORD_BYTES)
            {
                throw std::length_error("Session bundle row exceeds the 256 MiB limit");
            }
            json.push_back('\n');
            writer.Write(json);
            if (options.progress && ((row + 1) % PROGRESS_INTERVAL_ROWS) == 0)
            {
                options.progress(row + 1, lines.size());
            }
        }

        PollStop(options.stopToken);
        writer.Finish();
        file.Close();
        if (options.progress)
        {
            options.progress(lines.size(), lines.size());
        }
        ReplaceAtomically(temporary, destination);
    }
    catch (...)
    {
        std::error_code cleanupError;
        std::filesystem::remove(temporary, cleanupError);
        throw;
    }
}

} // namespace loglib
