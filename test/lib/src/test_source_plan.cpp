#include <loglib/rotation_siblings.hpp>
#include <loglib/source_plan.hpp>

#include <test_common/temp_dir.hpp>

#include <catch2/catch_all.hpp>

#include <filesystem>
#include <span>
#include <string>
#include <vector>

using loglib::CanonicalKeyForPath;
using loglib::ResolvedSource;
using loglib::ResolveSourcePlan;
using loglib::SourcePlanOptions;
using test_common::TempDir;

namespace
{

std::string Basename(const std::filesystem::path &path)
{
    const std::u8string u8 = path.filename().u8string();
    return {reinterpret_cast<const char *>(u8.data()), u8.size()};
}

std::vector<std::string> Basenames(const std::vector<ResolvedSource> &plan)
{
    std::vector<std::string> names;
    names.reserve(plan.size());
    for (const ResolvedSource &entry : plan)
    {
        names.push_back(Basename(entry.path));
    }
    return names;
}

} // namespace

TEST_CASE("ResolveSourcePlan keeps a single file with no siblings", "[source_plan]")
{
    const TempDir dir("source_plan_solo");
    (void)dir.Write("solo.log", "only");
    const std::filesystem::path input[] = {dir.Path() / "solo.log"};

    const std::vector<ResolvedSource> plan = ResolveSourcePlan(input, {});

    REQUIRE(plan.size() == 1);
    CHECK(Basenames(plan) == std::vector<std::string>{"solo.log"});
    CHECK_FALSE(plan.front().autoAdded);
    CHECK(plan.front().familyPrimary.empty());
}

TEST_CASE("ResolveSourcePlan expands a numbered family oldest-first", "[source_plan]")
{
    const TempDir dir("source_plan_numbered");
    (void)dir.Write("app.log", "primary");
    (void)dir.Write("app.log.1", "newer rotated");
    (void)dir.Write("app.log.2", "older rotated");
    const std::filesystem::path input[] = {dir.Path() / "app.log"};

    const std::vector<ResolvedSource> plan = ResolveSourcePlan(input, {});

    REQUIRE(plan.size() == 3);
    CHECK(Basenames(plan) == std::vector<std::string>{"app.log.2", "app.log.1", "app.log"});
    CHECK(plan[0].autoAdded);
    CHECK(plan[1].autoAdded);
    CHECK_FALSE(plan[2].autoAdded);
    CHECK(Basename(plan[0].familyPrimary) == "app.log");
    CHECK(Basename(plan[2].familyPrimary) == "app.log");
}

TEST_CASE("ResolveSourcePlan expands a dated family oldest-first", "[source_plan]")
{
    const TempDir dir("source_plan_dated");
    (void)dir.Write("app.log", "primary");
    (void)dir.Write("app.log-2025-04-28", "later");
    (void)dir.Write("app.log.2025-04-27", "earlier");
    const std::filesystem::path input[] = {dir.Path() / "app.log"};

    const std::vector<ResolvedSource> plan = ResolveSourcePlan(input, {});

    REQUIRE(plan.size() == 3);
    CHECK(Basenames(plan) == std::vector<std::string>{"app.log.2025-04-27", "app.log-2025-04-28", "app.log"});
    CHECK(plan[0].autoAdded);
    CHECK(plan[1].autoAdded);
    CHECK_FALSE(plan[2].autoAdded);
}

TEST_CASE("ResolveSourcePlan concatenates mixed families then residuals", "[source_plan]")
{
    const TempDir dir("source_plan_mixed");
    (void)dir.Write("app.log", "app");
    (void)dir.Write("app.log.1", "app-old");
    (void)dir.Write("svc.log", "svc");
    (void)dir.Write("svc.log.1", "svc-old");
    (void)dir.Write("notes.txt", "residual");
    const std::filesystem::path input[] = {
        dir.Path() / "notes.txt",
        dir.Path() / "svc.log",
        dir.Path() / "app.log",
    };

    const std::vector<ResolvedSource> plan = ResolveSourcePlan(input, {});

    REQUIRE(plan.size() == 5);
    CHECK(Basenames(plan) == std::vector<std::string>{"svc.log.1", "svc.log", "app.log.1", "app.log", "notes.txt"});
    CHECK(plan[0].autoAdded);
    CHECK_FALSE(plan[1].autoAdded);
    CHECK(plan[2].autoAdded);
    CHECK_FALSE(plan[3].autoAdded);
    CHECK_FALSE(plan[4].autoAdded);
    CHECK(plan[4].familyPrimary.empty());
}

TEST_CASE("ResolveSourcePlan with rotation disabled returns the inputs unchanged", "[source_plan]")
{
    const TempDir dir("source_plan_disabled");
    (void)dir.Write("app.log", "primary");
    (void)dir.Write("app.log.1", "sibling");
    const std::filesystem::path app = dir.Path() / "app.log";
    const std::filesystem::path input[] = {app, app};

    SourcePlanOptions options;
    options.rotationHistory = false;
    const std::vector<ResolvedSource> plan = ResolveSourcePlan(input, options);

    REQUIRE(plan.size() == 2);
    CHECK(plan[0].path == app);
    CHECK(plan[1].path == app);
    CHECK_FALSE(plan[0].autoAdded);
    CHECK_FALSE(plan[1].autoAdded);
    CHECK(plan[0].familyPrimary.empty());
}

TEST_CASE("ResolveSourcePlan deduplicates a repeated path when rotation is on", "[source_plan]")
{
    const TempDir dir("source_plan_duplicate");
    (void)dir.Write("app.log", "primary");
    (void)dir.Write("app.log.1", "sibling");
    const std::filesystem::path app = dir.Path() / "app.log";
    const std::filesystem::path input[] = {app, app};

    const std::vector<ResolvedSource> plan = ResolveSourcePlan(input, {});

    REQUIRE(plan.size() == 2);
    CHECK(Basenames(plan) == std::vector<std::string>{"app.log.1", "app.log"});
    CHECK(plan[0].autoAdded);
    CHECK_FALSE(plan[1].autoAdded);
}

TEST_CASE("ResolveSourcePlan emits one family when two inputs share it", "[source_plan]")
{
    const TempDir dir("source_plan_overlap");
    (void)dir.Write("app.log", "primary");
    (void)dir.Write("app.log.1", "middle");
    (void)dir.Write("app.log.2", "oldest");
    (void)dir.Write("other.log", "after");
    const std::filesystem::path input[] = {
        dir.Path() / "app.log.2",
        dir.Path() / "other.log",
        dir.Path() / "app.log",
    };

    const std::vector<ResolvedSource> plan = ResolveSourcePlan(input, {});

    REQUIRE(plan.size() == 4);
    CHECK(Basenames(plan) == std::vector<std::string>{"app.log.2", "app.log.1", "app.log", "other.log"});
    CHECK_FALSE(plan[0].autoAdded);
    CHECK(plan[1].autoAdded);
    CHECK_FALSE(plan[2].autoAdded);
    CHECK_FALSE(plan[3].autoAdded);
}

TEST_CASE("ResolveSourcePlan keeps a nonexistent path as one residual", "[source_plan]")
{
    const TempDir dir("source_plan_missing");
    const std::filesystem::path missing = dir.Path() / "missing.log";
    const std::filesystem::path input[] = {missing};

    const std::vector<ResolvedSource> plan = ResolveSourcePlan(input, {});

    REQUIRE(plan.size() == 1);
    CHECK(plan.front().path == missing);
    CHECK_FALSE(plan.front().autoAdded);
    CHECK(plan.front().familyPrimary.empty());
    CHECK(plan.front().canonicalKey == CanonicalKeyForPath(missing));
}

TEST_CASE("ResolveSourcePlan suppressKeys drops already loaded paths", "[source_plan]")
{
    const TempDir dir("source_plan_suppress");
    (void)dir.Write("app.log", "primary");
    (void)dir.Write("app.log.1", "sibling");
    const std::filesystem::path primary = dir.Path() / "app.log";
    const std::string loaded[] = {CanonicalKeyForPath(primary)};
    const std::filesystem::path input[] = {primary};

    SourcePlanOptions options;
    options.suppressKeys = loaded;
    const std::vector<ResolvedSource> plan = ResolveSourcePlan(input, options);

    REQUIRE(plan.size() == 1);
    CHECK(Basename(plan.front().path) == "app.log.1");
    CHECK(plan.front().autoAdded);
    CHECK(Basename(plan.front().familyPrimary) == "app.log");
}
