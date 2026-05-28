#include <loglib/log_level.hpp>
#include <loglib/theme.hpp>

#include <catch2/catch_all.hpp>

#include <string>

using namespace loglib;

TEST_CASE("Theme JSON round-trip preserves all fields", "[Theme]")
{
    Theme original;
    original.name = "Dark";
    original.kind = ThemeKind::Dark;
    original.levels["Trace"] = LevelStyle{.foreground = "#64748B", .background = "#1F2228"};
    original.levels["Info"] = LevelStyle{};
    original.levels["Warn"] = LevelStyle{.foreground = "#FCD34D", .background = "#2A2418"};
    original.levels["Error"] = LevelStyle{.foreground = "#FCA5A5", .background = "#4C1D1D"};
    original.levels["Fatal"] = LevelStyle{
        .foreground = "#FECACA",
        .background = "#7F1D1D",
        .bold = true,
    };
    original.table.background = "#222222";
    original.table.alternateRowBackground = "#2A2A2A";
    original.table.selectionBackground = "#00518F";
    original.table.selectionForeground = "#FFFFFF";
    original.app.qtStyle = "fusion";
    original.app.fontFamily = "Monospace";
    original.app.fontSize = 10;

    const std::string json = SerializeTheme(original);
    const Theme reloaded = ParseTheme(json);

    CHECK(reloaded.name == original.name);
    CHECK(reloaded.kind == original.kind);
    REQUIRE(reloaded.levels.size() == original.levels.size());
    CHECK(reloaded.levels.at("Trace").foreground == original.levels.at("Trace").foreground);
    CHECK(reloaded.levels.at("Trace").background == original.levels.at("Trace").background);
    CHECK(reloaded.levels.at("Warn").background == original.levels.at("Warn").background);
    CHECK(reloaded.levels.at("Error").background == original.levels.at("Error").background);
    CHECK(reloaded.levels.at("Fatal").bold == true);
    CHECK(reloaded.table.background == original.table.background);
    CHECK(reloaded.table.alternateRowBackground == original.table.alternateRowBackground);
    CHECK(reloaded.app.qtStyle == original.app.qtStyle);
    CHECK(reloaded.app.fontFamily == original.app.fontFamily);
    CHECK(reloaded.app.fontSize == original.app.fontSize);
}

TEST_CASE("Theme parses unknown keys without throwing", "[Theme]")
{
    constexpr std::string_view JSON = R"({
        "name": "Forward Compat",
        "kind": "light",
        "futureField": 42,
        "levels": { "Info": { "foreground": "#222222" } },
        "table": {},
        "app": {}
    })";

    const Theme parsed = ParseTheme(JSON);
    CHECK(parsed.name == "Forward Compat");
    CHECK(parsed.kind == ThemeKind::Light);
    REQUIRE(parsed.levels.contains("Info"));
    CHECK(parsed.levels.at("Info").foreground == "#222222");
}

TEST_CASE("Theme parse rejects bad kind value", "[Theme]")
{
    constexpr std::string_view JSON = R"({
        "name": "Bad",
        "kind": "neon",
        "levels": {},
        "table": {},
        "app": {}
    })";

    CHECK_THROWS_AS(ParseTheme(JSON), std::runtime_error);
}

TEST_CASE("StyleForLevel returns default when level missing", "[Theme]")
{
    Theme theme;
    theme.levels["Error"] = LevelStyle{.foreground = "#FF0000", .bold = true};

    const LevelStyle errorStyle = StyleForLevel(theme, LogLevel::Error);
    CHECK(errorStyle.foreground == "#FF0000");
    CHECK(errorStyle.bold == true);

    const LevelStyle infoStyle = StyleForLevel(theme, LogLevel::Info);
    CHECK_FALSE(infoStyle.foreground.has_value());
    CHECK_FALSE(infoStyle.background.has_value());
    CHECK(infoStyle.bold == false);
}
