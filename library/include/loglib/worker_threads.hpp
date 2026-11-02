#pragma once

namespace loglib
{

/**
 * @brief Waits for the parser's worker threads to end.
 *
 * Call this as the last step of `main`, after all parsing has finished. Worker
 * threads can still be starting one another after a parse returns, and process
 * exit kills them wherever they are. Code that runs later in the exit sequence
 * can then fail on state those threads left behind. A later parse starts the
 * workers again.
 *
 * @return `false` if another thread is still using the workers, in which case
 *         they keep running.
 */
bool JoinWorkerThreads() noexcept;

} // namespace loglib
