/**
 * @brief Waits for the library's worker threads before the program exits.
 *
 * Parsing, filtering and sorting run on a shared pool of worker threads. Call ShutdownWorkerThreads as the last step of
 * main, after all of that work is done. The threads can still be starting one another after a call returns, and process
 * exit would stop them in the middle of that.
 *
 * This is part of the core module.
 */
#pragma once

namespace loglib
{

/**
 * @brief Waits for the library's worker threads to end. Returns false, and leaves them running, if another thread is
 * still using them.
 *
 * Call it as the last step of main, after all parsing, filtering and sorting is done. Code that runs later in the exit
 * sequence can otherwise fail on state the threads left behind. Later work starts the workers again.
 */
bool ShutdownWorkerThreads() noexcept;

} // namespace loglib
