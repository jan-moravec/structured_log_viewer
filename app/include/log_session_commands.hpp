#pragma once

#include "log_session_presentation.hpp"

#include <QString>
#include <QStringList>

#include <cstdint>

/**
 * @brief Defines the shell-facing command surface for a log session.
 *
 * The source, open, and autosave request methods are currently no-op
 * delegation points. The window shell performs those operations.
 */
class LogSessionCommands
{
public:
    /**
     * @brief Destroys the command interface.
     */
    virtual ~LogSessionCommands() = default;

    LogSessionCommands(const LogSessionCommands &) = delete;
    LogSessionCommands &operator=(const LogSessionCommands &) = delete;
    LogSessionCommands(LogSessionCommands &&) = delete;
    LogSessionCommands &operator=(LogSessionCommands &&) = delete;

    /**
     * @brief Requests an empty session.
     *
     * The current implementation is a no-op; the shell performs the reset.
     */
    virtual void RequestNewSession() = 0;

    /**
     * @brief Selects whether a file request appends to or replaces the session.
     */
    enum class OpenMode
    {
        Append,
        Replace,
    };

    /**
     * @brief Requests opening a set of files.
     *
     * The current implementation is a no-op; the shell opens the files.
     *
     * @param files Files to open.
     * @param mode Whether to append to or replace the current session.
     */
    virtual void RequestOpenFiles(const QStringList &files, OpenMode mode) = 0;

    /**
     * @brief Requests live tailing a file.
     *
     * The current implementation is a no-op; the shell starts the stream.
     *
     * @param filePath File to tail.
     */
    virtual void RequestOpenLogStream(const QString &filePath) = 0;

    /**
     * @brief Requests an autosave snapshot.
     *
     * The current implementation is a no-op; the shell owns persistence.
     *
     * @param publishOpenWindow Whether to publish the session for process restoration.
     */
    virtual void RequestAutoSaveSnapshot(bool publishOpenWindow) = 0;

    /**
     * @brief Reports conditions that require shell-owned close handling.
     *
     * This probe is side-effect free.
     *
     * @return A bitwise combination of `SessionClosePreconditions`; zero means
     * the session needs no prompt or worker drain.
     */
    [[nodiscard]] virtual std::uint32_t PreCheckClose() const = 0;

    /**
     * @brief Reports a successful close request without performing teardown.
     *
     * `MainWindow` owns close orchestration and uses `PreCheckClose()` to
     * determine required prompts and worker drains.
     *
     * @return `SessionCloseResult::Closed`.
     */
    virtual SessionCloseResult RequestClose() = 0;

protected:
    /**
     * @brief Constructs the command interface.
     */
    LogSessionCommands() = default;
};
