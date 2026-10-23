#include "commands.hpp"

#include "console.hpp"
#include "diagnostics.hpp"
#include "json_report.hpp"
#include "text_table.hpp"
#include "text_util.hpp"

#include <loglib/regex_templates.hpp>
#include <loglib/user_paths.hpp>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <utility>
#include <vector>

namespace slp
{

// Report structs have external linkage: glaze reflects member names
// through templates that MSVC cannot instantiate for internal types.
namespace reports
{

/** @brief One parser in `slp formats --output-format json`. */
struct ParserEntry
{
    std::string id;
    std::string description;
};

/** @brief One regex template in `slp formats --output-format json`. */
struct TemplateEntry
{
    std::string slug;
    std::string name;
    std::string continuationMode;
    std::string description;
    int priority = 0;
    std::string source;
    std::optional<std::string> shadows;
};

/** @brief The `slp formats --output-format json` report. */
struct FormatsReport
{
    std::vector<ParserEntry> parsers;
    std::vector<TemplateEntry> templates;
};

} // namespace reports

namespace
{

using reports::FormatsReport;
using reports::ParserEntry;
using reports::TemplateEntry;

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
        if (!entry.is_regular_file() || !EqualsIgnoreCase(entry.path().extension().string(), ".json"))
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
    TextTable parsers({}, "  ");
    parsers.AddRow(
        {"auto",
         "Detect the format from the file, or from a short sniff for stdin and network sources. JSON Lines is the "
         "fallback. This is the default."}
    );
    parsers.AddRow({"json", "JSON Lines, one object per line."});
    parsers.AddRow({"logfmt", "logfmt key=value lines."});
    parsers.AddRow({"csv", "Comma-separated values."});
    parsers.AddRow({"regex", "A PCRE2 pattern with named captures, or a regex template selected by slug."});

    auto templates = [](const std::vector<ListedTemplate> &rows, bool showUser) {
        TextTable table({}, "  ");
        for (const ListedTemplate &row : rows)
        {
            std::string description = FirstLine(row.description);
            if (showUser)
            {
                description += "  priority " + std::to_string(row.priority);
                if (!row.shadows.empty())
                {
                    description += ", shadows " + row.shadows;
                }
            }
            table.AddRow({row.slug, row.name, row.continuation, std::move(description)});
        }
        return table.Render();
    };

    std::string out = "Parsers\n" + parsers.Render() + "\nBuilt-in regex templates\n" + templates(builtins, false);
    if (!users.empty())
    {
        out += "\nUser regex templates\n" + templates(users, true);
    }
    return out;
}

[[nodiscard]] std::string RenderJson(
    const std::vector<ListedTemplate> &builtins, const std::vector<ListedTemplate> &users
)
{
    FormatsReport report;
    report.parsers = {
        {"auto", "Detect the format. JSON Lines is the fallback. This is the default."},
        {"json", "JSON Lines, one object per line."},
        {"logfmt", "logfmt key=value lines."},
        {"csv", "Comma-separated values."},
        {"regex", "A PCRE2 pattern with named captures, or a regex template."},
    };
    for (const auto *rows : {&builtins, &users})
    {
        for (const ListedTemplate &row : *rows)
        {
            report.templates.push_back(
                TemplateEntry{
                    .slug = row.slug,
                    .name = row.name,
                    .continuationMode = row.continuation,
                    .description = row.description,
                    .priority = row.priority,
                    .source = row.user ? "user" : "builtin",
                    .shadows = row.shadows.empty() ? std::nullopt : std::optional<std::string>(row.shadows),
                }
            );
        }
    }
    return WriteJsonReport(report);
}

} // namespace

ExitCode RunFormats(const FormatsOptions &options, const GlobalOptions &global, Console &console)
{
    const std::vector<ListedTemplate> builtins = BuiltinList();
    const bool loadUser = options.listUserTemplates || options.showSlug.has_value();
    const std::filesystem::path userDirectory = global.templateDir.value_or(loglib::UserRegexTemplateDirectory());
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
    for (const std::string &warning : users.warnings)
    {
        console.Warn(warning);
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
            return console.Report(console.Fail(ExitCode::Usage, showError));
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
            return console.Report(console.Fail(ExitCode::Usage, message));
        }
        if (json->empty() || json->back() != '\n')
        {
            json->push_back('\n');
        }
        console.Print(*json);
        return ExitCode::Success;
    }

    const std::vector<ListedTemplate> listedUsers =
        options.listUserTemplates ? users.templates : std::vector<ListedTemplate>{};
    console.Print(
        options.outputFormat == ReportFormat::Json ? RenderJson(builtins, listedUsers)
                                                   : RenderText(builtins, listedUsers)
    );
    return ExitCode::Success;
}

} // namespace slp
