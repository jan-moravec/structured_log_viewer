#pragma once

#include "log_source.hpp"

#include <atomic>
#include <cstddef>
#include <memory>
#include <string>

namespace loglib
{

class LogFile;

/// `LogSource` over a memory-mapped, finite `LogFile`. The static
/// `File → Open…` path runs through this source: the existing TBB
/// pipeline detects the mmap-backing via `IsMappedFile()` /
/// `GetMappedLogFile()` and operates directly on the mmap, preserving
/// the `[large]` / `[wide]` / `[allocations]` benchmark headroom (PRD
/// 4.9.1.i, task 1.2).
///
/// The class supports both an **owning** form (used by `LogModel` /
/// `BufferingSink` — the model owns the file across the parse and
/// teardown order matters) and a **borrowing** form (used by
/// `JsonParser::ParseStreaming(LogFile&, ...)` to wrap a caller-owned
/// `LogFile` without transferring ownership).
class MappedFileSource final : public LogSource
{
public:
    /// Owning ctor: takes ownership of @p file for the source's lifetime.
    explicit MappedFileSource(std::unique_ptr<LogFile> file);

    /// Borrowing ctor: caller must keep @p file alive for the source's
    /// lifetime. Used by the `LogFile&`-overload wrappers.
    explicit MappedFileSource(LogFile &file);

    ~MappedFileSource() override;

    MappedFileSource(const MappedFileSource &) = delete;
    MappedFileSource &operator=(const MappedFileSource &) = delete;
    MappedFileSource(MappedFileSource &&) = delete;
    MappedFileSource &operator=(MappedFileSource &&) = delete;

    /// Yields the next slice of the mmap into @p buffer (memcpy from
    /// `LogFile::Data() + cursor`). Advances the internal cursor. Returns
    /// 0 once exhausted, with `IsClosed() == true`. Returns 0 immediately
    /// if `Stop()` has been observed.
    size_t Read(std::span<char> buffer) override;

    /// No-op for finite mmap sources: every byte is available the moment
    /// the source is constructed.
    void WaitForBytes(std::chrono::milliseconds timeout) override;

    void Stop() noexcept override;

    [[nodiscard]] bool IsClosed() const noexcept override;

    [[nodiscard]] std::string DisplayName() const override;

    [[nodiscard]] bool IsMappedFile() const noexcept override;

    [[nodiscard]] LogFile *GetMappedLogFile() noexcept override;

    /// Releases ownership of the underlying `LogFile`. After this call the
    /// source is unusable (`IsClosed()` returns true). Returns the released
    /// owner; callers that constructed via the borrowing ctor get an empty
    /// `unique_ptr` back. Used by the static-path `BufferingSink::TakeData`
    /// transition (task 1.5).
    std::unique_ptr<LogFile> ReleaseFile() noexcept;

private:
    std::unique_ptr<LogFile> mOwnedFile;
    LogFile *mFile = nullptr;
    std::atomic<size_t> mCursor{0};
    std::atomic<bool> mStopped{false};
};

} // namespace loglib
