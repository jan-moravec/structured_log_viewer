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
    theme.levels["Error"] = LevelStyle{.foreground = "#FF0000", .background = std::nullopt, .bold = true};

    const LevelStyle errorStyle = StyleForLevel(theme, LogLevel::Error);
    CHECK(errorStyle.foreground == "#FF0000");
    CHECK(errorStyle.bold == true);

    const LevelStyle infoStyle = StyleForLevel(theme, LogLevel::Info);
    CHECK_FALSE(infoStyle.foreground.has_value());
    CHECK_FALSE(infoStyle.background.has_value());
    CHECK(infoStyle.bold == false);
}

TEST_CASE("ChromeStyle JSON round-trips every field", "[Theme]")
{
    Theme original;
    original.name = "Chrome";
    original.kind = ThemeKind::Light;
    original.chrome.window = "#101820";
    original.chrome.windowText = "#F0F0F0";
    original.chrome.text = "#E5E7EB";
    original.chrome.button = "#1F2937";
    original.chrome.buttonText = "#FAFAFA";
    original.chrome.placeholderText = "#9CA3AF";
    original.chrome.toolTipBase = "#374151";
    original.chrome.toolTipText = "#FFFFFF";

    const std::string json = SerializeTheme(original);
    const Theme reloaded = ParseTheme(json);

    CHECK(reloaded.chrome.window == original.chrome.window);
    CHECK(reloaded.chrome.windowText == original.chrome.windowText);
    CHECK(reloaded.chrome.text == original.chrome.text);
    CHECK(reloaded.chrome.button == original.chrome.button);
    CHECK(reloaded.chrome.buttonText == original.chrome.buttonText);
    CHECK(reloaded.chrome.placeholderText == original.chrome.placeholderText);
    CHECK(reloaded.chrome.toolTipBase == original.chrome.toolTipBase);
    CHECK(reloaded.chrome.toolTipText == original.chrome.toolTipText);
    // Defaulted `operator==`: identical themes compare equal.
    CHECK(reloaded == original);
}

TEST_CASE("Theme parses an Unknown level entry", "[Theme]")
{
    constexpr std::string_view JSON = R"({
        "name": "Unknown-aware",
        "kind": "dark",
        "levels": {
            "Unknown": { "foreground": "#888888", "italic": true }
        },
        "table": {},
        "chrome": {},
        "app": {}
    })";

    const Theme parsed = ParseTheme(JSON);
    const LevelStyle unknownStyle = StyleForLevel(parsed, LogLevel::Unknown);
    REQUIRE(unknownStyle.foreground.has_value());
    CHECK(unknownStyle.foreground == "#888888");
    CHECK(unknownStyle.italic == true);
    CHECK(unknownStyle.bold == false);
}

TEST_CASE("Theme defaulted equality distinguishes one differing field", "[Theme]")
{
    Theme baseline;
    baseline.name = "Same";
    baseline.kind = ThemeKind::Light;
    baseline.table.background = "#FFFFFF";

    Theme other = baseline;
    CHECK(baseline == other);

    other.table.background = "#FEFEFE";
    CHECK_FALSE(baseline == other);
}

TEST_CASE("Theme JSON round-trips a fully populated anchorPalette", "[Theme][anchor]")
{
    Theme original;
    original.name = "Anchored";
    original.kind = ThemeKind::Dark;
    original.anchorPalette = {
        "#B91C1C",
        "#C2410C",
        "#A16207",
        "#15803D",
        "#0F766E",
        "#0369A1",
        "#7E22CE",
        "#BE185D",
    };
    REQUIRE(original.anchorPalette.size() == ANCHOR_PALETTE_SIZE);

    const std::string json = SerializeTheme(original);
    // Wire-format sanity check so a future field rename surfaces here.
    CHECK(json.contains("\"anchorPalette\""));
    CHECK(json.contains("\"#B91C1C\""));

    const Theme reloaded = ParseTheme(json);
    CHECK(reloaded.anchorPalette == original.anchorPalette);
    CHECK(reloaded == original);
}

TEST_CASE("Theme without anchorPalette decodes as an empty vector", "[Theme][anchor]")
{
    // Pre-feature theme JSONs must continue to parse cleanly; the app
    // layer falls back to its built-in palette when this is empty.
    constexpr std::string_view JSON = R"({
        "name": "Pre-anchor",
        "kind": "dark",
        "levels": {},
        "table": {},
        "chrome": {},
        "app": {}
    })";

    const Theme parsed = ParseTheme(JSON);
    CHECK(parsed.name == "Pre-anchor");
    CHECK(parsed.anchorPalette.empty());
}

TEST_CASE("Theme round-trips a full levelColumnOverride block", "[Theme][levelOverride]")
{
    Theme original;
    original.name = "Iconic";
    original.kind = ThemeKind::Dark;
    LevelColumnOverride override;
    override.header = "Sev";
    override.headerIcon = ":/icons/level-info.svg";
    override.levels["Info"] = LevelDisplayOverride{
        .icon = ":/icons/level-info.svg",
        .pillBackground = "#1E3A5F",
    };
    override.levels["Warn"] = LevelDisplayOverride{
        .icon = ":/icons/level-warn.svg",
        .pillBackground = "#7C5A12",
        .pillForeground = "#FFFFFF",
    };
    override.levels["Error"] = LevelDisplayOverride{.icon = ":/icons/level-error.svg"};
    original.levelColumnOverride = std::move(override);

    const std::string json = SerializeTheme(original);
    // Wire-format sanity check so a future field rename surfaces here.
    CHECK(json.contains("\"levelColumnOverride\""));
    CHECK(json.contains("\"pillBackground\""));

    const Theme reloaded = ParseTheme(json);
    REQUIRE(reloaded.levelColumnOverride.has_value());
    CHECK(reloaded.levelColumnOverride->header == "Sev");
    CHECK(reloaded.levelColumnOverride->headerIcon == ":/icons/level-info.svg");
    REQUIRE(reloaded.levelColumnOverride->levels.contains("Info"));
    CHECK(reloaded.levelColumnOverride->levels.at("Info").pillBackground == "#1E3A5F");
    CHECK(reloaded.levelColumnOverride->levels.at("Warn").pillForeground == "#FFFFFF");
    CHECK_FALSE(reloaded.levelColumnOverride->levels.at("Error").pillBackground.has_value());
    // Defaulted `operator==` propagates through the new optional.
    CHECK(reloaded == original);
}

TEST_CASE("Theme without levelColumnOverride parses as nullopt", "[Theme][levelOverride]")
{
    constexpr std::string_view JSON = R"({
        "name": "Pre-icon",
        "kind": "dark",
        "levels": {},
        "table": {},
        "chrome": {},
        "app": {}
    })";

    const Theme parsed = ParseTheme(JSON);
    CHECK(parsed.name == "Pre-icon");
    CHECK_FALSE(parsed.levelColumnOverride.has_value());
}

TEST_CASE("Theme preserves blank-string header override through round-trip", "[Theme][levelOverride]")
{
    Theme original;
    original.name = "Blank";
    original.kind = ThemeKind::Light;
    LevelColumnOverride override;
    // Empty string is a legitimate value: render no header text.
    // Distinct from `std::nullopt`, which means "fall back to
    // `Column::header`".
    override.header = "";
    original.levelColumnOverride = std::move(override);

    const std::string json = SerializeTheme(original);
    const Theme reloaded = ParseTheme(json);
    REQUIRE(reloaded.levelColumnOverride.has_value());
    REQUIRE(reloaded.levelColumnOverride->header.has_value());
    CHECK(reloaded.levelColumnOverride->header->empty());
}

TEST_CASE("Theme round-trips a levelsHighContrast override map", "[Theme][highContrast]")
{
    Theme original;
    original.name = "Punchy";
    original.kind = ThemeKind::Dark;
    // Subtle defaults.
    original.levels["Warn"] = LevelStyle{.foreground = "#FCD34D", .background = "#272620"};
    original.levels["Error"] = LevelStyle{.foreground = "#FCA5A5", .background = "#352121"};
    original.levels["Fatal"] = LevelStyle{.foreground = "#FECACA", .background = "#4A1E1E", .bold = true};
    // Loud overrides for the same keys.
    original.levelsHighContrast["Warn"] = LevelStyle{.foreground = "#FCD34D", .background = "#2A2418"};
    original.levelsHighContrast["Error"] = LevelStyle{.foreground = "#FCA5A5", .background = "#4C1D1D"};
    original.levelsHighContrast["Fatal"] = LevelStyle{.foreground = "#FECACA", .background = "#7F1D1D", .bold = true};

    const std::string json = SerializeTheme(original);
    CHECK(json.contains("\"levelsHighContrast\""));

    const Theme reloaded = ParseTheme(json);
    REQUIRE(reloaded.levelsHighContrast.size() == 3);
    CHECK(reloaded.levelsHighContrast.at("Warn").background == "#2A2418");
    CHECK(reloaded.levelsHighContrast.at("Error").background == "#4C1D1D");
    CHECK(reloaded.levelsHighContrast.at("Fatal").bold == true);
    // Defaulted `operator==` propagates through the new map.
    CHECK(reloaded == original);
}

TEST_CASE("Theme without levelsHighContrast decodes as an empty map", "[Theme][highContrast]")
{
    constexpr std::string_view JSON = R"({
        "name": "Pre-toggle",
        "kind": "dark",
        "levels": { "Error": { "foreground": "#FF0000" } },
        "table": {},
        "chrome": {},
        "app": {}
    })";

    const Theme parsed = ParseTheme(JSON);
    CHECK(parsed.levelsHighContrast.empty());
}

TEST_CASE("StyleForLevel honours useHighContrast with sparse fall-back", "[Theme][highContrast]")
{
    Theme theme;
    theme.levels["Warn"] = LevelStyle{.foreground = "#AA8800", .background = "#272620"};
    theme.levels["Error"] = LevelStyle{.foreground = "#AA0000", .background = "#352121"};
    theme.levels["Fatal"] = LevelStyle{.foreground = "#FF6688", .background = "#4A1E1E", .bold = true};
    // Sparse override: only `Error` boosts; `Warn` and `Fatal` keep
    // the subtle entry when high contrast is on.
    theme.levelsHighContrast["Error"] = LevelStyle{.foreground = "#FF0000", .background = "#7F1D1D"};

    // useHighContrast=false matches the no-bool overload.
    const LevelStyle warnSubtle = StyleForLevel(theme, LogLevel::Warn, false);
    CHECK(warnSubtle.background == "#272620");
    const LevelStyle warnLoud = StyleForLevel(theme, LogLevel::Warn, true);
    CHECK(warnLoud.background == "#272620"); // sparse: falls back to `levels`

    // Explicit override wins when present.
    const LevelStyle errorSubtle = StyleForLevel(theme, LogLevel::Error, false);
    CHECK(errorSubtle.background == "#352121");
    const LevelStyle errorLoud = StyleForLevel(theme, LogLevel::Error, true);
    CHECK(errorLoud.background == "#7F1D1D");
    CHECK(errorLoud.foreground == "#FF0000");

    // Fatal sits in `levels` only -- both branches resolve to the
    // same `LevelStyle`, including bold.
    const LevelStyle fatalLoud = StyleForLevel(theme, LogLevel::Fatal, true);
    CHECK(fatalLoud.background == "#4A1E1E");
    CHECK(fatalLoud.bold == true);
}

TEST_CASE("Theme tolerates a sparse anchorPalette with empty slots", "[Theme][anchor]")
{
    // Empty strings let a theme override only a couple of slots and
    // delegate the rest to the app's built-in palette. The parse path
    // round-trips them verbatim; the app layer interprets empty as
    // "use fallback for this index".
    constexpr std::string_view JSON = R"({
        "name": "Sparse",
        "kind": "light",
        "levels": {},
        "table": {},
        "chrome": {},
        "app": {},
        "anchorPalette": ["", "#EA580C", "", "", "#0D9488", "", "", ""]
    })";

    const Theme parsed = ParseTheme(JSON);
    REQUIRE(parsed.anchorPalette.size() == 8);
    CHECK(parsed.anchorPalette[0].empty());
    CHECK(parsed.anchorPalette[1] == "#EA580C");
    CHECK(parsed.anchorPalette[4] == "#0D9488");
    CHECK(parsed.anchorPalette[7].empty());
}
