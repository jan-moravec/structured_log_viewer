/**
 * @brief Destinations for exported bytes.
 *
 * An ExportSink receives the bytes of one export run. Write may buffer, and Finish must be called once on success. If a
 * sink is destroyed without Finish, the export counts as aborted and no partial file is left behind. The header
 * provides sinks for files and for streams such as standard output.
 *
 * This is part of the output module.
 */
#pragma once

#include <cstddef>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <string>
#include <string_view>

namespace loglib::exports
{

/**
 * @brief Where the bytes of one export go.
 *
 * Write may buffer, and Finish flushes and completes the output. On success, call Finish once. A sink destroyed without
 * Finish counts as an aborted export and must not leave a partial destination file. Any member may throw
 * std::runtime_error on an I/O error, and StreamSink throws BrokenPipe when the reader has closed its end. A sink must
 * not be used after it threw.
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

    /** @brief Adds bytes to the output. */
    virtual void Write(std::string_view bytes) = 0;

    /** @brief Adds a single byte to the output. */
    void WriteChar(char c)
    {
        Write(std::string_view(&c, 1));
    }

    /**
     * @brief Completes the output. Call it once when everything has been written.
     *
     * A FileSink closes its temporary file and renames it onto the destination. A StreamSink flushes the stream it
     * borrowed and renames nothing. It throws std::runtime_error on failure, and StreamSink throws BrokenPipe when the
     * reader has gone away.
     */
    virtual void Finish() = 0;
};

/**
 * @brief A sink that writes to a temporary file next to the destination and renames it when Finish succeeds.
 *
 * A cancelled or failed export never leaves a partial file at the destination. LogConfigurationManager::Save works the
 * same way.
 */
class FileSink final : public ExportSink
{
public:
    /**
     * @brief Opens the temporary file for buffered writing. The destination is the path the file gets after a
     * successful Finish.
     */
    explicit FileSink(std::filesystem::path destination);

    /** @brief Deletes the temporary file if Finish did not succeed. */
    ~FileSink() override;

    FileSink(const FileSink &) = delete;
    FileSink &operator=(const FileSink &) = delete;
    FileSink(FileSink &&) = delete;
    FileSink &operator=(FileSink &&) = delete;

    void Write(std::string_view bytes) override;

    /**
     * @brief Flushes and closes the temporary file and renames it onto the destination in one step. Calling it again is
     * harmless. It throws if flushing, closing or renaming fails.
     */
    void Finish() override;

    /** @brief Returns whether Finish has completed successfully. */
    [[nodiscard]] bool Finished() const noexcept
    {
        return mFinished;
    }

    /** @brief The destination path given to the constructor. */
    [[nodiscard]] const std::filesystem::path &Destination() const noexcept
    {
        return mDestination;
    }

private:
    std::filesystem::path mDestination;
    std::filesystem::path mTempPath;
    // A FILE is used and not a std::ofstream, because it has one clear buffer and reports write, flush and close
    // failures in the same way.
    std::FILE *mFile = nullptr;
    bool mFinished = false;
};

/**
 * @brief Thrown when a write reaches a pipe or socket that the reader has closed.
 *
 * It is not a loglib::Error or a std::runtime_error, so a general I/O handler does not report it. Callers that treat a
 * closed reader as success, as in slp query piped into head, catch it and exit quietly.
 */
class BrokenPipe : public std::exception
{
public:
    [[nodiscard]] const char *what() const noexcept override
    {
        return "Broken pipe";
    }
};

/**
 * @brief A sink that writes to a FILE it borrows.
 *
 * Write uses the buffer of the stream. Finish flushes it and does not rename anything or replace a path. The
 * destructor neither closes the stream nor removes anything.
 */
class StreamSink final : public ExportSink
{
public:
    /** @brief Borrows an open stream for the lifetime of the sink. The caller keeps ownership. */
    explicit StreamSink(std::FILE *file);

    /** @brief Leaves the borrowed stream open. */
    ~StreamSink() override = default;

    StreamSink(const StreamSink &) = delete;
    StreamSink &operator=(const StreamSink &) = delete;
    StreamSink(StreamSink &&) = delete;
    StreamSink &operator=(StreamSink &&) = delete;

    void Write(std::string_view bytes) override;

    /**
     * @brief Flushes the borrowed stream without closing it. Calling it again is harmless. It throws BrokenPipe when
     * the reader has closed its end, and std::runtime_error for any other flush failure.
     */
    void Finish() override;

    /** @brief Returns whether Finish has completed successfully. */
    [[nodiscard]] bool Finished() const noexcept
    {
        return mFinished;
    }

private:
    // Borrowed from the caller, who closes it.
    std::FILE *mFile = nullptr;
    bool mFinished = false;
};

} // namespace loglib::exports
