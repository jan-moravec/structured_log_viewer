/**
 * @brief Waits for the parser's worker threads before the program exits.
 *
 * Call JoinWorkerThreads as the last step of main. Worker threads can still be starting other threads after a parse
 * returns, and process exit would stop them in the middle of that.
 *
 * This is part of the parse module.
 */
#pragma once

namespace loglib
{

/**
 * @brief Waits for the parser's worker threads to end. Returns false, and leaves them running, if another thread is
 * still using them.
 *
 * Call it as the last step of main, after all parsing is done. The threads can still be starting one another after a
 * parse returns, and the end of the process stops them wherever they are. Code that runs later in the exit sequence can
 * then fail on state the threads left behind. A later parse starts the workers again.
 */
bool JoinWorkerThreads() noexcept;

} // namespace loglib
