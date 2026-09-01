#pragma once

#include <string>
#include <utility>
#include <vector>

namespace loglib
{

/**
 * @brief Per-column rendering / detection type. Auto-detection is gated
 * by `Column::autoDetect`: a column is a detector candidate iff
 * `type == Any && autoDetect`. JSON wire format uses lowerCamelCase
 * keys; see `internal/log_configuration_glaze_meta.hpp`.
 *   - `Any`         - default for fresh keys (with `autoDetect`)
 *                     and the bail bucket for unclassifiable
 *                     values. With `autoDetect=false`, the
 *                     explicit "treat as text" opt-out. Sorts /
 *                     filters as string.
 *   - `String`      - inferred string column.
 *   - `Boolean`     - JSON `true`/`false`; false < true.
 *   - `Integer`     - only Int64/UInt64 observed.
 *   - `Floating`    - only Double observed.
 *   - `Number`      - mix of integer and floating.
 *   - `Time`        - timestamp column.
 *   - `Enumeration` - small fixed vocabulary stored as `DictRef`.
 *   - `Level`       - Enumeration subtype for log-level columns;
 *                     sorts / filters / styles by severity rank.
 */
enum class ColumnType
{
    Any,
    String,
    Boolean,
    Integer,
    Floating,
    Number,
    Time,
    Enumeration,
    Level
};

/**
 * @brief One column in the persisted schema: header, keys, type, and
 * display/parse options.
 */
struct Column
{
    std::string header;
    std::vector<std::string> keys;
    std::string printFormat;
    /**
     * @brief Defaults to the detector-candidate state (paired with
     * `autoDetect=true` below). Time promotion is destructive
     * (only `Reset()` reverts); enum promotion can demote on
     * overflow, but only while `autoDetect` is on.
     */
    ColumnType type = ColumnType::Any;
    std::vector<std::string> parseFormats;
    /**
     * @brief Hidden columns stay in the table (data, sort, and filters
     * keep working); only the view toggles `setSectionHidden`.
     * Defaults to `true` so saved JSON without this field loads as visible.
     */
    bool visible = true;
    /**
     * @brief Per-column alias overrides for `ColumnType::Level` columns. Each
     * entry is `(alias, canonicalName)`: aliases match the raw
     * user string case-insensitively, canonical names must spell
     * a `LogLevel` (`"Info"`, `"Warn"`, ...). Augments the
     * built-in alias table. Ignored for non-Level columns.
     */
    std::vector<std::pair<std::string, std::string>> levelMapping;
    /**
     * @brief `true`: the auto-detector owns the column (scans `Any`
     * candidates, demotes overflowing enums). `false`: the user
     * has pinned the column; no automatic promotion or demotion.
     * Defaults to `true` so unedited columns stay auto-detected.
     */
    bool autoDetect = true;
};

/**
 * @brief Default `parseFormats` seed for freshly-detected `ColumnType::Time`
 * columns and for editor-driven pins that leave the list empty.
 * Kept in a single place so the three seed sites
 * (`LogConfigurationManager::Update`, `::AppendKeys`, and
 * `LogTable::OnUserChangedColumnType`) never drift.
 *
 * Order matters: the promotion loop tries formats in list order and
 * stops on the first match. ISO 8601 variants come first because
 * they cover the JSON / logfmt / RFC 5424 lion's share (fast path
 * via `Iso8601_T` / `Iso8601_Space` -- see
 * `ClassifyTimestampFormat`). Non-ISO tail formats target the
 * shipped regex-template shapes so their timestamps promote to
 * `Type::Timestamp` without the user editing the column:
 *   * `%d/%b/%Y:%H:%M:%S %z` -- Apache/nginx CLF (Combined + Common
 *     + AWS CloudFront and every downstream that inherits CLF).
 *   * `%b %e %H:%M:%S` / `%b %d %H:%M:%S` -- RFC 3164 syslog
 *     header (Mmm  D HH:MM:SS, space- or zero-padded day; no year).
 *     Routes to the `SyslogRfc3164NoYear` fast path which injects
 *     the current year via the standard month-rollover heuristic.
 *
 * Each addition here widens the auto-parse net; keep new entries
 * specific enough that `date::from_stream` fails deterministically
 * on non-matching inputs (character-class-level anchors: literal
 * `/`, `:`, `-`, ...). A too-permissive format at the tail can
 * misclassify a numeric column as `ColumnType::Time`.
 */
[[nodiscard]] inline std::vector<std::string> DefaultTimeParseFormats()
{
    return {
        "%FT%T%Ez",
        "%F %T%Ez",
        "%FT%T",
        "%F %T",
        "%d/%b/%Y:%H:%M:%S %z",
        "%b %e %H:%M:%S",
        "%b %d %H:%M:%S",
    };
}

} // namespace loglib
