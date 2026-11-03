#include "loglib/parse/worker_threads.hpp"

#include <oneapi/tbb/global_control.h>

#include <new>

namespace loglib
{

bool JoinWorkerThreads() noexcept
{
    oneapi::tbb::task_scheduler_handle handle{oneapi::tbb::attach{}};
    return oneapi::tbb::finalize(handle, std::nothrow);
}

} // namespace loglib
