#pragma once

#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>

namespace slv::exports
{

/// Byte sink for the export pipeline.
///
/// v1 has only one implementation (`FileSink`) that writes to a
/// temp file and atomically renames on `Finish`. The interface is
/// pinned so future compressed sinks (`GzipSink`, `ZstdSink`) and
/// container-section sinks (for the full-state export bundle) can
/// slot in without touching the `RowExporter` implementations.
///
/// Contract:
///   - `Write` may buffer; callers should not assume bytes have
///     reached disk. `Finish` performs any flush + finalise.
///   - `Finish` must be called exactly once on the success path.
///     Destruction without `Finish` is treated as an abort and
///     leaves no partial file behind (temp file is unlinked).
///   - Any method may throw `std::runtime_error` on I/O failure;
///     the sink is left in an unusable state after a throw.
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

/// File-backed sink writing to `<destination>.tmp` and renaming
/// atomically on `Finish`. Mirrors the pattern used in
/// `library/src/log_configuration.cpp` (Save).
///
/// Destruction without a successful `Finish` unlinks the temp
/// file. This guarantees a cancelled export leaves no partial
/// file in the destination path.
class FileSink final : public ExportSink
{
public:
    /// Opens `<destination>.tmp` for buffered writing. Throws on
    /// open failure. The destination path is remembered for the
    /// atomic-rename in `Finish`.
    explicit FileSink(std::filesystem::path destination);

    /// Aborts the write (unlinks temp file) if `Finish` was not
    /// called successfully.
    ~FileSink() override;

    FileSink(const FileSink &) = delete;
    FileSink &operator=(const FileSink &) = delete;
    FileSink(FileSink &&) = delete;
    FileSink &operator=(FileSink &&) = delete;

    void Write(std::string_view bytes) override;

    /// Flushes and closes the temp file, then renames it onto the
    /// destination path. Idempotent: a second call is a no-op.
    /// Throws on flush / close / rename failure.
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
    // Using FILE* (not std::ofstream) so bufferered writes go through a
    // single well-defined buffer size and errors on write / flush /
    // close surface as std::runtime_error uniformly.
    std::FILE *mFile = nullptr;
    bool mFinished = false;
};

} // namespace slv::exports
