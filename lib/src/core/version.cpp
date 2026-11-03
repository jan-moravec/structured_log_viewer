#include "loglib/core/version.hpp"

namespace loglib
{

std::string_view Version() noexcept
{
#ifndef LOGLIB_VERSION_STRING
#error "LOGLIB_VERSION_STRING is set by lib/CMakeLists.txt"
#endif
    return LOGLIB_VERSION_STRING;
}

} // namespace loglib
