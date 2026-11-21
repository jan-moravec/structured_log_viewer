#include "loglib/parse/regex_templates.hpp"

#include "loglib/core/ascii_case.hpp"
#include "loglib/core/error.hpp"
#include "loglib/core/internal/glaze_options.hpp"
#include "loglib/core/internal/path_encoding.hpp"
#include "loglib/parse/internal/regex_template_glaze_meta.hpp"

#include <glaze/glaze.hpp>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace loglib
{

namespace
{

[[nodiscard]] bool IsJsonFile(const std::filesystem::path &path)
{
    return EqualsIgnoreCaseAscii(internal::PathToUtf8(path.extension()), ".json");
}

constexpr size_t FILE_READ_CHUNK_BYTES = 4096;

[[nodiscard]] std::string ReadFile(std::FILE *file, bool &failed)
{
    std::string data;
    char buffer[FILE_READ_CHUNK_BYTES];
    while (true)
    {
        const size_t read = std::fread(buffer, 1, sizeof(buffer), file);
        if (read > 0)
        {
            data.append(buffer, read);
        }
        if (read < sizeof(buffer))
        {
            break;
        }
    }
    failed = std::ferror(file) != 0;
    return data;
}

} // namespace

RegexTemplate ParseRegexTemplate(std::string_view content)
{
    constexpr auto OPTS = loglib::internal::JSON_FILE_OPTIONS;

    RegexTemplate parsed;
    const auto error = glz::read<OPTS>(parsed, content);
    if (error)
    {
        throw FormatError("Invalid regex template JSON: " + glz::format_error(error, std::string(content)));
    }
    return parsed;
}

std::string SerializeRegexTemplate(const RegexTemplate &tmpl)
{
    constexpr auto OPTS = loglib::internal::JSON_FILE_OPTIONS;

    std::string json;
    const auto error = glz::write<OPTS>(tmpl, json);
    if (error)
    {
        throw FormatError("Failed to serialise regex template JSON: " + glz::format_error(error));
    }
    return json;
}

RegexTemplateLoadResult LoadRegexTemplatesFromDirectory(const std::filesystem::path &directory)
{
    RegexTemplateLoadResult result;
    std::error_code existsError;
    const bool exists = std::filesystem::exists(directory, existsError);
    if (existsError || !exists)
    {
        if (existsError)
        {
            result.errors.push_back(
                "Cannot inspect regex template directory '" + internal::PathToUtf8(directory) +
                "': " + existsError.message()
            );
        }
        return result;
    }
    if (!std::filesystem::is_directory(directory, existsError) || existsError)
    {
        result.errors.push_back("Regex template path is not a directory: '" + internal::PathToUtf8(directory) + "'");
        return result;
    }

    std::vector<std::filesystem::path> files;
    try
    {
        for (const std::filesystem::directory_entry &entry : std::filesystem::directory_iterator{directory})
        {
            if (!entry.is_regular_file() || !IsJsonFile(entry.path()))
            {
                continue;
            }
            files.push_back(entry.path());
        }
    }
    catch (const std::filesystem::filesystem_error &error)
    {
        result.errors.push_back(
            "Cannot list regex template directory '" + internal::PathToUtf8(directory) + "': " + error.what()
        );
        return result;
    }

    std::ranges::sort(files, [](const std::filesystem::path &left, const std::filesystem::path &right) {
        return left.filename() < right.filename();
    });

    result.templates.reserve(files.size());
    for (const std::filesystem::path &path : files)
    {
        const std::string pathText = internal::PathToUtf8(path);
        std::FILE *file = internal::OpenFileForBinaryRead(path);
        if (file == nullptr)
        {
            const int error = errno;
            result.errors.push_back(
                "Cannot read regex template '" + pathText +
                "': " + std::error_code{error, std::generic_category()}.message()
            );
            continue;
        }
        bool failed = false;
        const std::string bytes = ReadFile(file, failed);
        // NOLINTNEXTLINE(cppcoreguidelines-owning-memory,cert-err33-c)
        (void)std::fclose(file);
        if (failed)
        {
            result.errors.push_back("Cannot read regex template '" + pathText + "'");
            continue;
        }
        try
        {
            result.templates.push_back(ParseRegexTemplate(bytes));
        }
        catch (const std::exception &error)
        {
            result.errors.push_back("Cannot parse regex template '" + pathText + "': " + error.what());
        }
    }
    return result;
}

} // namespace loglib
