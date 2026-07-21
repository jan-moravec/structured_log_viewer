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
/// Owns the compiled per-rule `loglib::RowPredicate` list plus a
/// per-source-row "last matching rule index" cache that `LogModel`
/// consults from the paint hot path. Rules are Configuration-scope
/// (persisted alongside columns) but the compiled state is
/// runtime-only and lives here.
///
/// Column identity: rules bind by `Column::keys` (the same stable
/// identifier used elsewhere in `LogConfiguration`), not by column
/// index -- so `MoveColumn`, cross-source apply, and unrelated
/// column additions do not disturb rules. When a rule's keys don't
/// resolve against the current columns it becomes inert (no cost at
/// match time) and the editor renders it greyed out. `InactiveCount`
/// surfaces the count to `MainWindow` for a status-bar toast.
///
/// Rebuild model:
///   - `SetRules` (editor Save / config load): full recompile +
///     full row-match rebuild.
///   - `RebindColumns` (`AppendKeys`, `SetColumnType`, dictionary
///     growth): keeps the rule list, refreshes the resolved-index
///     and predicate caches, and rebuilds row matches.
///   - `OnRowsAppended` (`LogModel::rowsInserted`): tail-evaluates
///     only the new rows against the existing compiled rules.
///   - `ClearMatches` (`LogModel::modelReset`): drops the row-match
///     cache; compiled rules are kept so the next stream restarts
///     hot.
///
/// Semantics: rules are applied in vector order, **last match wins
/// per row**. Rendering (foreground / background / bold / italic)
/// is read from `HighlightRule` fields and applied by `LogModel::data`
/// on top of the level brush; anchor overlays still take precedence.
class HighlightRuleSet : public QObject
{
    Q_OBJECT

public:
    explicit HighlightRuleSet(QObject *parent = nullptr);
    /// Out-of-line so `unique_ptr<CompiledRule>` can hold a
    /// forward-declared pimpl without leaking the definition into
    /// this header.
    ~HighlightRuleSet() override;

    HighlightRuleSet(const HighlightRuleSet &) = delete;
    HighlightRuleSet &operator=(const HighlightRuleSet &) = delete;
    HighlightRuleSet(HighlightRuleSet &&) = delete;
    HighlightRuleSet &operator=(HighlightRuleSet &&) = delete;

    /// Replace the rule set. Compiles predicates against @p columns
    /// and, when @p table is non-null, rebuilds the row-match cache
    /// synchronously. Emits `rulesChanged` and (if the cache was
    /// rebuilt) `matchesChanged`.
    void SetRules(
        std::vector<loglib::LogConfiguration::HighlightRule> rules,
        const std::vector<loglib::LogConfiguration::Column> &columns,
        const loglib::LogTable *table
    );

    /// Rebind against a new column layout (rules unchanged).
    /// Refreshes cached column indices, re-compiles predicates
    /// (dictionary pointers may have moved) and, when @p table is
    /// non-null, rebuilds the row-match cache.
    ///
    /// Called after `LogConfigurationManager::AppendKeys` (streaming
    /// discovers new columns) and after column type / enum-dictionary
    /// changes, so a rule can activate (or deactivate) without the
    /// user reopening the editor.
    void RebindColumns(
        const std::vector<loglib::LogConfiguration::Column> &columns, const loglib::LogTable *table
    );

    /// Tail-evaluate rules against rows `[firstNewRow, lastNewRow]`
    /// (inclusive). @p table must already reflect the appended rows.
    /// No-op if `Empty()`.
    void OnRowsAppended(const loglib::LogTable &table, std::size_t firstNewRow, std::size_t lastNewRow);

    /// Drop the row-match cache. The compiled rules and resolved
    /// column indices are kept so a follow-up stream doesn't need
    /// to re-parse the JSON.
    void ClearMatches();

    /// Snapshot of the source rules (for the editor + the config
    /// mirror). Vector order is meaningful (last-match-wins).
    [[nodiscard]] const std::vector<loglib::LogConfiguration::HighlightRule> &Rules() const noexcept;

    /// Rule index for @p sourceRow, or `nullopt` when no rule matches
    /// (or when @p sourceRow is out of range for the cache). The
    /// caller pairs this with `Rules()[index]` to read the rendering
    /// fields.
    [[nodiscard]] std::optional<std::size_t> LastMatchFor(std::size_t sourceRow) const noexcept;

    /// Number of rules whose `columnKeys` don't resolve against the
    /// current columns (or whose match spec is incomplete). The
    /// status bar shows this after `ApplyLoadedConfiguration`.
    [[nodiscard]] std::size_t InactiveCount() const noexcept;

    /// Fast short-circuit for `LogModel::data`: no rules, no cache.
    [[nodiscard]] bool Empty() const noexcept;

    /// True iff any compiled rule is currently active (keys resolved
    /// + valid predicate). Independent of whether the cache has been
    /// populated. Used by `LogModel::data` after `Empty()`: an empty
    /// row-match cache with active rules means "cache not built yet".
    [[nodiscard]] bool HasActiveRules() const noexcept;

    /// Ranges over the currently resolved column index for each rule,
    /// or -1 for inactive ones. Test-only accessor.
    [[nodiscard]] const std::vector<int> &ResolvedColumnsForTest() const noexcept;

signals:
    /// Emitted after `SetRules` / `RebindColumns` finishes recompiling
    /// (before the row-match rebuild). @p inactiveCount lets the
    /// status bar surface `"N highlight rule(s) inactive"` without
    /// probing the set.
    void rulesChanged(std::size_t inactiveCount);

    /// Emitted after any change to the row-match cache: full rebuild
    /// (`SetRules` / `RebindColumns` with a table), tail append
    /// (`OnRowsAppended`), or `ClearMatches`. `LogModel` reacts by
    /// emitting `dataChanged` on the affected rows.
    ///
    /// Simple broadcast for now: the initial implementation always
    /// invalidates the whole visible region. A follow-up can refine
    /// this to a row range when perf demands it.
    void matchesChanged();

private:
    struct CompiledRule;

    /// Compile @p rule against a resolved column index. Returns a
    /// null-optional when the rule can't be compiled (missing
    /// filterString, malformed match spec, unresolved enum dict,
    /// ...); the rule then remains listed in `Rules()` but doesn't
    /// participate in matching.
    static std::optional<CompiledRule> CompileRule(
        const loglib::LogConfiguration::HighlightRule &rule,
        int resolvedColumn,
        const std::vector<loglib::LogConfiguration::Column> &columns,
        const loglib::LogTable *table
    );

    /// Resolve @p keys against @p columns. Matches the first column
    /// whose `keys` vector is a superset of @p keys (usually a
    /// single-element vector). Returns -1 when no column matches or
    /// @p keys is empty.
    static int ResolveColumnByKeys(
        const std::vector<std::string> &keys, const std::vector<loglib::LogConfiguration::Column> &columns
    ) noexcept;

    /// Recompile every rule against the current @p columns. Refreshes
    /// `mCompiled` / `mResolvedColumn` / `mInactiveCount`.
    void RecompileAll(
        const std::vector<loglib::LogConfiguration::Column> &columns, const loglib::LogTable *table
    );

    /// Evaluate `mCompiled` against @p table for rows
    /// `[first, last]` and write into `mRowMatch`. Assumes
    /// `mRowMatch.size() > last`.
    void EvaluateRows(const loglib::LogTable &table, std::size_t first, std::size_t last);

    /// Full rebuild of `mRowMatch` from @p table.
    void RebuildAllMatches(const loglib::LogTable &table);

    std::vector<loglib::LogConfiguration::HighlightRule> mRules;

    /// Same size as `mRules`. `nullopt` = rule is inert (unresolved
    /// keys, incomplete match spec, ...). Held via `unique_ptr` so
    /// the vector's move / erase paths don't churn `RowPredicate`s.
    std::vector<std::unique_ptr<CompiledRule>> mCompiled;

    /// Same size as `mRules`. Column index resolved from `columnKeys`
    /// at compile time, or -1 when unresolved. Test-only accessor
    /// (`ResolvedColumnsForTest`) reads this directly.
    std::vector<int> mResolvedColumn;

    /// Per-source-row last-matching rule index (`0..mRules.size()`),
    /// or -1 when no rule matches. Sized to `LogTable::RowCount()`
    /// after each rebuild.
    ///
    /// `int16_t` caps the practical rule count at 32k, roughly 6
    /// orders of magnitude above any realistic user rule set. The
    /// cache doubles as a bitmap of "does this row need custom
    /// styling?" -- the paint path branches on `>= 0`.
    std::vector<std::int16_t> mRowMatch;

    std::size_t mInactiveCount = 0;
    std::size_t mActiveCount = 0;
};
