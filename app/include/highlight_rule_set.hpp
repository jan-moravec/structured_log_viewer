#pragma once

#include <loglib/log_configuration.hpp>

#include <QObject>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace loglib
{
class LogTable;
} // namespace loglib

/// Runtime companion to `LogConfiguration::highlightRules`.
///
/// Owns the compiled `RowPredicate` for each rule plus a per-row
/// "last matching rule" cache that `LogModel` reads from the paint
/// hot path. Rules bind by `Column::keys` (stable across
/// `MoveColumn` and cross-source apply); unresolved rules stay
/// inert and are counted in `InactiveCount`.
///
/// Rebuild triggers:
///   - `SetRules` — editor Save / config load; full rebuild.
///   - `RebindColumns` — after `AppendKeys` or type flips;
///     recompiles and rebuilds matches, rule list unchanged.
///   - `OnRowsAppended` — streaming tail; evaluates only new rows.
///   - `OnRowsEvicted` — FIFO retention; shifts the cache.
///   - `ClearMatches` — model reset; drops the cache, keeps rules.
///
/// Rules apply in vector order, **last match wins per row**.
/// Rendering fields (fg / bg / bold / italic) are applied by
/// `LogModel::data` on top of the level brush; anchor overlays
/// still win.
class HighlightRuleSet : public QObject
{
    Q_OBJECT

public:
    explicit HighlightRuleSet(QObject *parent = nullptr);
    /// Out-of-line so `unique_ptr<CompiledRule>` can hold a
    /// pimpl-forward-declared type.
    ~HighlightRuleSet() override;

    HighlightRuleSet(const HighlightRuleSet &) = delete;
    HighlightRuleSet &operator=(const HighlightRuleSet &) = delete;
    HighlightRuleSet(HighlightRuleSet &&) = delete;
    HighlightRuleSet &operator=(HighlightRuleSet &&) = delete;

    /// Replace the rule set, recompile against @p columns, and (if
    /// @p table is non-null) rebuild the row-match cache. Emits
    /// `rulesChanged` then `matchesChanged`.
    void SetRules(
        std::vector<loglib::LogConfiguration::HighlightRule> rules,
        const std::vector<loglib::LogConfiguration::Column> &columns,
        const loglib::LogTable *table
    );

    /// Recompile the existing rules against a new column layout.
    /// Called after `AppendKeys`, `SetColumnType`, or dictionary
    /// growth so a rule can activate / deactivate without reopening
    /// the editor.
    void RebindColumns(
        const std::vector<loglib::LogConfiguration::Column> &columns, const loglib::LogTable *table
    );

    /// Evaluate rules against newly-appended rows
    /// `[firstNewRow, lastNewRow]`. @p table must already contain
    /// them. No-op when empty.
    void OnRowsAppended(const loglib::LogTable &table, std::size_t firstNewRow, std::size_t lastNewRow);

    /// Erase cache entries for rows `[first, last]` and shift the
    /// tail down. Called on `QAbstractItemModel::rowsRemoved` so
    /// FIFO retention doesn't leave stale entries at the front of
    /// the cache. No `matchesChanged` emit: the view already
    /// repaints from `rowsRemoved`.
    void OnRowsEvicted(std::size_t first, std::size_t last);

    /// Drop the row-match cache; keep compiled rules so the next
    /// stream restarts hot. After this, `LastMatchFor` returns
    /// `nullopt` for every row but `HasActiveRules()` is unchanged.
    void ClearMatches();

    /// Source rules (used by the editor + config mirror). Order is
    /// significant (last-match-wins).
    [[nodiscard]] const std::vector<loglib::LogConfiguration::HighlightRule> &Rules() const noexcept;

    /// Winning rule index for @p sourceRow, or `nullopt` when no
    /// rule matches (or the row is out of range).
    [[nodiscard]] std::optional<std::size_t> LastMatchFor(std::size_t sourceRow) const noexcept;

    /// Rules whose keys don't resolve or whose match spec is
    /// incomplete. Displayed as a status-bar toast after config load.
    [[nodiscard]] std::size_t InactiveCount() const noexcept;

    /// Fast short-circuit for `LogModel::data`: no rules at all.
    [[nodiscard]] bool Empty() const noexcept;

    /// True iff any compiled rule is currently active. Independent
    /// of the cache -- lets `LogModel::data` distinguish "no rules"
    /// from "cache not built yet".
    [[nodiscard]] bool HasActiveRules() const noexcept;

    /// Test-only: resolved column index per rule, -1 for inactive.
    [[nodiscard]] const std::vector<int> &ResolvedColumnsForTest() const noexcept;

signals:
    /// Fired after `SetRules` / `RebindColumns` finish recompiling.
    /// @p inactiveCount drives the status-bar toast.
    void rulesChanged(std::size_t inactiveCount);

    /// Fired after any change to the row-match cache. `LogModel`
    /// re-emits `dataChanged` for the whole table in response
    /// (coarse but rare -- only editor Save / config load / column
    /// bind / streaming tail).
    void matchesChanged();

private:
    struct CompiledRule;

    /// Compile @p rule. Returns `nullopt` when the rule can't be
    /// compiled (missing needle, malformed spec, unresolved enum
    /// dict, ...); the rule stays listed but doesn't participate
    /// in matching.
    static std::optional<CompiledRule> CompileRule(
        const loglib::LogConfiguration::HighlightRule &rule,
        int resolvedColumn,
        const std::vector<loglib::LogConfiguration::Column> &columns,
        const loglib::LogTable *table
    );

    /// Resolve @p keys against @p columns using subset-match
    /// semantics (rule keys must all appear in the column's keys).
    /// Returns -1 when no column matches or @p keys is empty.
    static int ResolveColumnByKeys(
        const std::vector<std::string> &keys, const std::vector<loglib::LogConfiguration::Column> &columns
    ) noexcept;

    /// Recompile every rule; refreshes `mCompiled`,
    /// `mResolvedColumn`, `mInactiveCount`, `mActiveCount`.
    void RecompileAll(
        const std::vector<loglib::LogConfiguration::Column> &columns, const loglib::LogTable *table
    );

    /// Evaluate rows `[first, last]` into `mRowMatch`. Requires
    /// `mRowMatch.size() > last`.
    void EvaluateRows(const loglib::LogTable &table, std::size_t first, std::size_t last);

    /// Rebuild `mRowMatch` from scratch against @p table.
    void RebuildAllMatches(const loglib::LogTable &table);

    std::vector<loglib::LogConfiguration::HighlightRule> mRules;

    /// Same size as `mRules`; `nullptr` marks an inert entry.
    /// `unique_ptr` keeps `RowPredicate` (a non-trivial `variant`)
    /// stable across vector moves / erases.
    std::vector<std::unique_ptr<CompiledRule>> mCompiled;

    /// Same size as `mRules`. Resolved column index, or -1 when
    /// unresolved. Exposed via `ResolvedColumnsForTest`.
    std::vector<int> mResolvedColumn;

    /// Per-row winning rule index, or -1 for "no match". Sized to
    /// `LogTable::RowCount()`. `int16_t` caps the rule count at
    /// 32k, well above realistic use; the paint path branches on
    /// `>= 0`.
    std::vector<std::int16_t> mRowMatch;

    std::size_t mInactiveCount = 0;
    std::size_t mActiveCount = 0;
};
