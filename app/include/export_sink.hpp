#pragma once

#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>

namespace slv::exports
{

/// Byte sink for the export pipeline. The interface is kept
/// minimal so future compressed / container sinks can slot in
/// without touching `RowExporter` implementations.
///
/// Contract:
///   - `Write` may buffer; `Finish` performs the flush + finalise.
///   - `Finish` must be called exactly once on the success path.
///     Destruction without `Finish` is treated as an abort and
///     leaves no partial file behind.
///   - Any method may throw `std::runtime_error` on I/O failure;
///     the sink is unusable after a throw.
class ExportSink
{
public:
    ExportSink() = default;
    virtual ~ExportSink() = default;

    ExportSink(const ExportSink &) = delete;
    ExportSink &operator=(const ExportSink &) = delete;
    ExportSink(ExportSink &&) = delete;
    ExportSink &operator=(ExportSink &&) = delete;

    /// Append @p bytes to the sink.
    virtual void Write(std::string_view bytes) = 0;

    /// Convenience wrapper for single-byte writes.
    void WriteChar(char c)
    {
        Write(std::string_view(&c, 1));
    }

    /// Finalise the sink. For `FileSink` this closes the temp
    /// file and renames it into place. Must be called exactly once
    /// on the success path. Throws `std::runtime_error` on failure.
    virtual void Finish() = 0;
};

/// File-backed sink: writes to `<destination>.tmp` and renames
/// atomically on `Finish`, so a cancelled or failed export never
/// leaves a partial file at the destination path. Same pattern
/// used by `library/src/log_configuration.cpp` (Save).
class FileSink final : public ExportSink
{
public:
    /// Opens `<destination>.tmp` for buffered writing. Throws on
    /// open failure.
    explicit FileSink(std::filesystem::path destination);

    /// Unlinks the temp file if `Finish` was not called successfully.
    ~FileSink() override;

    FileSink(const FileSink &) = delete;
    FileSink &operator=(const FileSink &) = delete;
    FileSink(FileSink &&) = delete;
    FileSink &operator=(FileSink &&) = delete;

    void Write(std::string_view bytes) override;

    /// Flush, close, and atomically rename the temp file onto the
    /// destination. Idempotent; throws on flush/close/rename failure.
    void Finish() override;

    /// True iff `Finish` has completed successfully. Test seam.
    [[nodiscard]] bool Finished() const noexcept
    {
        return mFinished;
    }

    /// Destination path passed to the constructor. Test seam.
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

} // namespace slv::exports
