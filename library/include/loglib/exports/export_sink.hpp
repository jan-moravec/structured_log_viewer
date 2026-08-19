#pragma once

#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>

namespace loglib::exports
{

/**
 * @brief Byte sink for one export run.
 *
 * `Write` may buffer; `Finish` flushes and finalises. Call `Finish`
 * exactly once on the success path. Destruction without `Finish` is
 * an abort and must not leave a partial destination file. Any method
 * may throw `std::runtime_error` on I/O failure; the sink is unusable
 * after a throw.
 */
class ExportSink
{
public:
    ExportSink() = default;
    virtual ~ExportSink() = default;

    ExportSink(const ExportSink &) = delete;
    ExportSink &operator=(const ExportSink &) = delete;
    ExportSink(ExportSink &&) = delete;
    ExportSink &operator=(ExportSink &&) = delete;

    /** @brief Appends @p bytes to the sink. */
    virtual void Write(std::string_view bytes) = 0;

    /** @brief Writes a single byte. */
    void WriteChar(char c)
    {
        Write(std::string_view(&c, 1));
    }

    /**
     * @brief Finalises the sink.
     *
     * For `FileSink` this closes the temp file and renames it onto
     * the destination. Must be called exactly once on the success
     * path. Throws `std::runtime_error` on failure.
     */
    virtual void Finish() = 0;
};

/**
 * @brief File-backed sink that writes `<destination>.tmp` and renames
 * atomically on `Finish`.
 *
 * A cancelled or failed export never leaves a partial file at the
 * destination path. Matches the pattern used by
 * `LogConfigurationManager::Save`.
 */
class FileSink final : public ExportSink
{
public:
    /**
     * @brief Opens `<destination>.tmp` for buffered writing.
     *
     * @param destination Final path after a successful `Finish`.
     */
    explicit FileSink(std::filesystem::path destination);

    /** @brief Unlinks the temp file if `Finish` did not succeed. */
    ~FileSink() override;

    FileSink(const FileSink &) = delete;
    FileSink &operator=(const FileSink &) = delete;
    FileSink(FileSink &&) = delete;
    FileSink &operator=(FileSink &&) = delete;

    void Write(std::string_view bytes) override;

    /**
     * @brief Flushes, closes, and atomically renames the temp file
     * onto the destination.
     *
     * Idempotent. Throws on flush, close, or rename failure.
     */
    void Finish() override;

    /** @brief Returns whether `Finish` has completed successfully. */
    [[nodiscard]] bool Finished() const noexcept
    {
        return mFinished;
    }

    /** @brief Returns the destination path passed to the constructor. */
    [[nodiscard]] const std::filesystem::path &Destination() const noexcept
    {
        return mDestination;
    }

private:
    std::filesystem::path mDestination;
    std::filesystem::path mTempPath;
    // FILE* (not std::ofstream): single well-defined buffer and
    // uniform error reporting for write / flush / close failures.
    std::FILE *mFile = nullptr;
    bool mFinished = false;
};

} // namespace loglib::exports
