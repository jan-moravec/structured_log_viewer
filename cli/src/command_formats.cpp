#include "commands.hpp"

#include "diagnostics.hpp"

#include <loglib/regex_templates.hpp>
#include <loglib/user_paths.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <utility>
#include <vector>

namespace slp
{

namespace
{

constexpr int MAX_CLOSEST = 5;
constexpr int CLOSE_DISTANCE = 3;

struct ListedTemplate
{
    std::string slug;
    std::string name;
    std::string continuation;
    std::string description;
    int priority = 0;
    bool user = false;
    std::string shadows;
};

[[nodiscard]] std::string FirstLine(std::string_view text)
{
    const auto end = text.find_first_of("\r\n");
    if (end == std::string_view::npos)
    {
        return std::string(text);
    }
    return std::string(text.substr(0, end));
}

[[nodiscard]] int EditDistance(std::string_view left, std::string_view right)
{
    std::vector<int> previous(right.size() + 1);
    std::vector<int> current(right.size() + 1);
    for (std::size_t column = 0; column < previous.size(); ++column)
    {
        previous[column] = static_cast<int>(column);
    }
    for (std::size_t row = 1; row <= left.size(); ++row)
    {
        current[0] = static_cast<int>(row);
        for (std::size_t column = 1; column <= right.size(); ++column)
        {
            const int substitution = left[row - 1] == right[column - 1] ? 0 : 1;
            current[column] =
                std::min({current[column - 1] + 1, previous[column] + 1, previous[column - 1] + substitution});
        }
        previous.swap(current);
    }
    return previous.back();
}

void AppendJsonString(std::string &out, std::string_view text)
{
    out.push_back('"');
    for (const unsigned char ch : text)
    {
        switch (ch)
        {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (ch < 0x20)
            {
                constexpr char HEX[] = "0123456789abcdef";
                out += "\\u00";
                out.push_back(HEX[ch >> 4]);
                out.push_back(HEX[ch & 0x0f]);
            }
            else
            {
                out.push_back(static_cast<char>(ch));
            }
            break;
        }
    }
    out.push_back('"');
}

[[nodiscard]] std::string ContinuationOf(const loglib::RegexTemplate &tmpl)
{
    return std::string(loglib::ContinuationModeName(tmpl.continuationMode));
}

[[nodiscard]] std::vector<ListedTemplate> BuiltinList()
{
    std::vector<ListedTemplate> listed;
    for (const loglib::BuiltinRegexTemplateInfo &info : loglib::BuiltinRegexTemplateCatalog())
    {
        ListedTemplate row;
        row.slug = info.slug;
        row.name = info.tmpl.name;
        row.continuation = ContinuationOf(info.tmpl);
        row.description = info.tmpl.description;
        row.priority = info.tmpl.priority;
        listed.push_back(std::move(row));
    }
    return listed;
}

[[nodiscard]] std::string ShadowedSlug(const std::vector<ListedTemplate> &builtins, std::string_view name)
{
    for (const ListedTemplate &builtin : builtins)
    {
        if (builtin.name == name)
        {
            return builtin.slug;
        }
    }
    return {};
}

struct UserLoad
{
    std::vector<ListedTemplate> templates;
    std::vector<std::string> warnings;
};

UserLoad LoadUser(const std::filesystem::path &directory, const std::vector<ListedTemplate> &builtins)
{
    UserLoad loaded;
    std::error_code error;
    if (!std::filesystem::is_directory(directory, error) || error)
    {
        return loaded;
    }

    std::vector<std::filesystem::path> files;
    for (const std::filesystem::directory_entry &entry : std::filesystem::directory_iterator(directory, error))
    {
        if (error)
        {
            loaded.warnings.push_back("could not read template directory '" + directory.string() + "'.");
            return loaded;
        }
        std::string extension = entry.path().extension().string();
        std::ranges::transform(extension, extension.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
        if (!entry.is_regular_file() || extension != ".json")
        {
            continue;
        }
        files.push_back(entry.path());
    }
    std::ranges::sort(files);

    std::vector<loglib::RegexTemplate> parsed;
    for (const std::filesystem::path &path : files)
    {
        const std::ifstream input(path, std::ios::binary);
        if (!input)
        {
            loaded.warnings.push_back("skipped unreadable template '" + path.filename().string() + "'.");
            continue;
        }
        std::ostringstream buffer;
        buffer << input.rdbuf();
        try
        {
            loglib::RegexTemplate tmpl = loglib::ParseRegexTemplate(buffer.str());
            ListedTemplate row;
            row.slug = path.stem().string();
            row.name = tmpl.name;
            row.continuation = ContinuationOf(tmpl);
            row.description = tmpl.description;
            row.priority = tmpl.priority;
            row.user = true;
            row.shadows = ShadowedSlug(builtins, tmpl.name);
            loaded.templates.push_back(std::move(row));
            parsed.push_back(std::move(tmpl));
        }
        catch (const std::exception &ex)
        {
            loaded.warnings.push_back("skipped template '" + path.filename().string() + "': " + ex.what());
        }
    }
    loglib::SetExtraRegexTemplates(parsed);
    return loaded;
}

[[nodiscard]] std::string ClosestSlugs(const std::vector<ListedTemplate> &catalog, std::string_view needle)
{
    struct Candidate
    {
        int distance = 0;
        std::string slug;
    };
    std::vector<Candidate> candidates;
    for (const ListedTemplate &row : catalog)
    {
        const int distance = EditDistance(needle, row.slug);
        const bool contains = row.slug.find(needle) != std::string::npos || needle.find(row.slug) != std::string::npos;
        if (distance <= CLOSE_DISTANCE || contains)
        {
            candidates.push_back(Candidate{distance, row.slug});
        }
    }
    std::ranges::sort(candidates, [](const Candidate &lhs, const Candidate &rhs) {
        if (lhs.distance != rhs.distance)
        {
            return lhs.distance < rhs.distance;
        }
        return lhs.slug < rhs.slug;
    });
    if (static_cast<int>(candidates.size()) > MAX_CLOSEST)
    {
        candidates.resize(static_cast<std::size_t>(MAX_CLOSEST));
    }
    std::string joined;
    for (const Candidate &candidate : candidates)
    {
        if (!joined.empty())
        {
            joined += ", ";
        }
        joined += candidate.slug;
    }
    return joined;
}

[[nodiscard]] const ListedTemplate *FindSlug(const std::vector<ListedTemplate> &rows, std::string_view slug)
{
    for (const ListedTemplate &row : rows)
    {
        if (row.slug == slug)
        {
            return &row;
        }
    }
    return nullptr;
}

/** @brief Copy of the built-in template named @p slug; the catalog is returned by value, so no pointer into it may
 * escape. */
[[nodiscard]] std::optional<loglib::RegexTemplate> FindBuiltin(std::string_view slug)
{
    for (const loglib::BuiltinRegexTemplateInfo &info : loglib::BuiltinRegexTemplateCatalog())
    {
        if (info.slug == slug)
        {
            return info.tmpl;
        }
    }
    return std::nullopt;
}

[[nodiscard]] std::string RenderText(
    const std::vector<ListedTemplate> &builtins, const std::vector<ListedTemplate> &users
)
{
    std::ostringstream out;
    out << "Parsers\n";
    out << "  auto    Detect the format from the file, or from a short sniff for stdin and network sources. "
           "JSON Lines is the fallback. This is the default.\n";
    out << "  json    JSON Lines, one object per line.\n";
    out << "  logfmt  logfmt key=value lines.\n";
    out << "  csv     Comma-separated values.\n";
    out << "  regex   A PCRE2 pattern with named captures, or a regex template selected by slug.\n";
    out << "\nBuilt-in regex templates\n";

    auto writeRows = [&out](const std::vector<ListedTemplate> &rows, bool showUser) {
        std::size_t slugWidth = 4;
        std::size_t nameWidth = 4;
        std::size_t modeWidth = 4;
        for (const ListedTemplate &row : rows)
        {
            slugWidth = std::max(slugWidth, row.slug.size());
            nameWidth = std::max(nameWidth, row.name.size());
            modeWidth = std::max(modeWidth, row.continuation.size());
        }
        for (const ListedTemplate &row : rows)
        {
            out << "  " << row.slug << std::string(slugWidth - row.slug.size() + 2, ' ') << row.name
                << std::string(nameWidth - row.name.size() + 2, ' ') << row.continuation
                << std::string(modeWidth - row.continuation.size() + 2, ' ') << FirstLine(row.description);
            if (showUser)
            {
                out << "  priority " << row.priority;
                if (!row.shadows.empty())
                {
                    out << ", shadows " << row.shadows;
                }
            }
            out << '\n';
        }
    };
    writeRows(builtins, false);
    if (!users.empty())
    {
        out << "\nUser regex templates\n";
        writeRows(users, true);
    }
    return out.str();
}

[[nodiscard]] std::string RenderJson(
    const std::vector<ListedTemplate> &builtins, const std::vector<ListedTemplate> &users
)
{
    std::string out = "{\n  \"parsers\": [\n";
    const std::array<std::pair<std::string_view, std::string_view>, 5> parsers = {{
        {"auto", "Detect the format. JSON Lines is the fallback. This is the default."},
        {"json", "JSON Lines, one object per line."},
        {"logfmt", "logfmt key=value lines."},
        {"csv", "Comma-separated values."},
        {"regex", "A PCRE2 pattern with named captures, or a regex template."},
    }};
    for (std::size_t index = 0; index < parsers.size(); ++index)
    {
        out += "    {\"id\": ";
        AppendJsonString(out, parsers[index].first);
        out += ", \"description\": ";
        AppendJsonString(out, parsers[index].second);
        out += "}";
        if (index + 1 != parsers.size())
        {
            out += ',';
        }
        out += '\n';
    }
    out += "  ],\n  \"templates\": [\n";
    std::vector<const ListedTemplate *> rows;
    rows.reserve(builtins.size() + users.size());
    for (const ListedTemplate &row : builtins)
    {
        rows.push_back(&row);
    }
    for (const ListedTemplate &row : users)
    {
        rows.push_back(&row);
    }
    for (std::size_t index = 0; index < rows.size(); ++index)
    {
        const ListedTemplate &row = *rows[index];
        out += "    {\"slug\": ";
        AppendJsonString(out, row.slug);
        out += ", \"name\": ";
        AppendJsonString(out, row.name);
        out += ", \"continuationMode\": ";
        AppendJsonString(out, row.continuation);
        out += ", \"description\": ";
        AppendJsonString(out, row.description);
        out += ", \"priority\": " + std::to_string(row.priority);
        out += ", \"source\": ";
        AppendJsonString(out, row.user ? "user" : "builtin");
        if (!row.shadows.empty())
        {
            out += ", \"shadows\": ";
            AppendJsonString(out, row.shadows);
        }
        out += "}";
        if (index + 1 != rows.size())
        {
            out += ',';
        }
        out += '\n';
    }
    out += "  ]\n}\n";
    return out;
}

} // namespace

CommandOutput RenderFormats(const Options &options)
{
    CommandOutput result;
    const std::vector<ListedTemplate> builtins = BuiltinList();
    const bool loadUser = options.listUserTemplates || options.showSlug.has_value();
    const std::filesystem::path userDirectory = options.templateDir.value_or(loglib::UserRegexTemplateDirectory());
    UserLoad users;
    if (loadUser)
    {
        try
        {
            users = LoadUser(userDirectory, builtins);
        }
        catch (const std::exception &ex)
        {
            users.warnings.push_back(std::string("could not read user templates: ") + ex.what());
        }
    }
    if (!options.quiet)
    {
        for (const std::string &warning : users.warnings)
        {
            result.err += FormatWarningLine(warning);
            result.err.push_back('\n');
        }
    }

    if (options.showSlug.has_value())
    {
        std::string showError;
        std::optional<std::string> json;
        try
        {
            if (!options.listUserTemplates)
            {
                if (const auto builtin = FindBuiltin(*options.showSlug))
                {
                    json = loglib::SerializeRegexTemplate(*builtin);
                }
            }
            if (!json.has_value())
            {
                const ListedTemplate *user = FindSlug(users.templates, *options.showSlug);
                if (user != nullptr)
                {
                    const std::filesystem::path candidate = userDirectory / (user->slug + ".json");
                    const std::ifstream input(candidate, std::ios::binary);
                    if (!input)
                    {
                        showError = "could not read template '" + candidate.string() + "'.";
                    }
                    else
                    {
                        std::ostringstream buffer;
                        buffer << input.rdbuf();
                        json = loglib::SerializeRegexTemplate(loglib::ParseRegexTemplate(buffer.str()));
                    }
                }
            }
            if (!json.has_value() && options.listUserTemplates)
            {
                if (const auto builtin = FindBuiltin(*options.showSlug))
                {
                    json = loglib::SerializeRegexTemplate(*builtin);
                }
            }
        }
        catch (const std::exception &ex)
        {
            showError = ex.what();
        }
        if (!showError.empty())
        {
            result.exitCode = static_cast<int>(ExitCode::Usage);
            result.err += FormatErrorLine("formats", showError);
            result.err.push_back('\n');
            return result;
        }
        if (!json.has_value())
        {
            std::vector<ListedTemplate> all = builtins;
            all.insert(all.end(), users.templates.begin(), users.templates.end());
            const std::string closest = ClosestSlugs(all, *options.showSlug);
            std::string message = "unknown template slug '" + *options.showSlug + "'";
            if (closest.empty())
            {
                message += "; run `slp formats` to list the known slugs.";
            }
            else
            {
                message += "; closest matches are " + closest + ".";
            }
            result.exitCode = static_cast<int>(ExitCode::Usage);
            result.err += FormatErrorLine("formats", message);
            result.err.push_back('\n');
            return result;
        }
        result.out = *json;
        if (result.out.empty() || result.out.back() != '\n')
        {
            result.out.push_back('\n');
        }
        return result;
    }

    if (options.outputFormat == "json")
    {
        result.out = RenderJson(builtins, options.listUserTemplates ? users.templates : std::vector<ListedTemplate>{});
    }
    else
    {
        result.out = RenderText(builtins, options.listUserTemplates ? users.templates : std::vector<ListedTemplate>{});
    }
    return result;
}

} // namespace slp
