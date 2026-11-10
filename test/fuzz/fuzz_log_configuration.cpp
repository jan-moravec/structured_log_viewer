// Fuzz target for loading a saved configuration from text.

#include "fuzz_common.hpp"

#include "loglib/schema/log_configuration.hpp"

#include <cstddef>
#include <cstdint>
#include <stdexcept>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size)
{
    try
    {
        loglib::LogConfigurationManager manager;
        manager.LoadFromString(loglib::fuzz::AsText(data, size));
    }
    catch (const std::runtime_error &) // NOLINT(bugprone-empty-catch)
    {
        // The documented failure for text that is not a configuration.
    }
    return 0;
}
