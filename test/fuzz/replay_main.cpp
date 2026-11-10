// Replay driver for the fuzz targets. It calls LLVMFuzzerTestOneInput once for every file in the directories (or files)
// named on the command line, so the saved corpora and regression inputs run as ordinary tests on every platform and
// compiler. It prints the name of each file before it runs it, so a failure names its input.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size);

namespace
{

namespace fs = std::filesystem;

void CollectInputs(const fs::path &argument, std::vector<fs::path> &inputs)
{
    std::error_code error;
    if (fs::is_directory(argument, error))
    {
        std::vector<fs::path> found;
        for (const fs::directory_entry &entry : fs::directory_iterator(argument, error))
        {
            // Skip placeholders such as .gitkeep and anything that is not a plain file.
            if (entry.is_regular_file() && !entry.path().filename().string().starts_with('.'))
            {
                found.push_back(entry.path());
            }
        }
        std::ranges::sort(found);
        inputs.insert(inputs.end(), found.begin(), found.end());
    }
    else if (fs::is_regular_file(argument, error))
    {
        inputs.push_back(argument);
    }
    // A directory that does not exist is skipped: a target may have no regression inputs yet.
}

} // namespace

// NOLINTNEXTLINE(bugprone-exception-escape)
int main(int argc, const char *const *argv)
{
    try
    {
        std::vector<fs::path> inputs;
        for (int i = 1; i < argc; ++i)
        {
            CollectInputs(fs::path{argv[i]}, inputs);
        }
        if (inputs.empty())
        {
            // A corpus is never empty, so no input means a wrong path, which must not pass as a clean replay.
            std::cerr << "no inputs found\n";
            return 2;
        }

        for (const fs::path &input : inputs)
        {
            std::cout << "replay " << input.generic_string() << '\n' << std::flush;

            std::ifstream stream(input, std::ios::binary);
            if (!stream)
            {
                std::cerr << "cannot open " << input.generic_string() << '\n';
                return 2;
            }
            const std::vector<char> bytes{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
            LLVMFuzzerTestOneInput(reinterpret_cast<const std::uint8_t *>(bytes.data()), bytes.size());
        }

        std::cout << "replayed " << inputs.size() << " inputs\n";
        return 0;
    }
    catch (const std::exception &error)
    {
        // An exception that a target lets escape is a finding, just as it would be under libFuzzer.
        std::cerr << "uncaught exception: " << error.what() << '\n';
        return 1;
    }
    catch (...)
    {
        std::cerr << "uncaught exception of an unknown type\n";
        return 1;
    }
}
