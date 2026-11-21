// Fuzz target for loading a saved configuration from text.

#include "fuzz_common.hpp"

#include "loglib/config/log_configuration.hpp"
#include "loglib/core/error.hpp"

#include <cstddef>
#include <cstdint>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size)
{
    try
    {
        static_cast<void>(loglib::ParseConfiguration(loglib::fuzz::AsText(data, size)));
    }
    catch (const loglib::FormatError &) // NOLINT(bugprone-empty-catch)
    {
        // The documented failure for text that is not a configuration.
    }
    return 0;
}
