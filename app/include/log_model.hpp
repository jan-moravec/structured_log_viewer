#pragma once

#include "anchor_manager.hpp"

#include <loglib/bytes_producer.hpp>
#include <loglib/log_data.hpp>
#include <loglib/log_level.hpp>
#include <loglib/log_parse_sink.hpp>
#include <loglib/log_parser.hpp>
#include <loglib/log_table.hpp>
#include <loglib/parser_options.hpp>
#include <loglib/stop_token.hpp>

#include <QAbstractTableModel>
#include <QFuture>
#include <QIcon>
#include <QStringList>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

template <typename T> class QFutureWatcher;
class HighlightRuleSet;
class QtStreamingLogSink;
class ThemeControl;

enum LogModelItemDataRole
{
    UserRole = Qt::UserRole,
    SortRole,
    CopyLine,
    /// Source-model row index, used as a stable sort tie-break.
    InsertionOrderRole,
    /// `loglib::EnumValueId` as `qint32` for `DictRef` slots; invalid
    /// otherwise. Kept for external readers; the filter proxy bypasses
    /// it and queries `LogTable` directly.
    EnumValueRole,
};

/// Outcome reported by `LogModel::streamingFinished`.
enum class StreamingResult : int
{
    Success = 0,
    Cancelled = 1,
    Failed = 2,
};

/// Shape change reported by `LogModel::enumColumnsChanged`, so receivers
/// can scope their reaction:
///   - `Promoted` -- a column flipped into `Type::Enumeration`. The
///     proxy's `EnumDictRank` cache has no entry yet, so no
///     invalidation is needed; filter rules built before the promotion
///     want a rebuild onto the bitset fast path.
///   - `Grew` -- an existing enum column's dictionary gained at least
///     one id. `EnumRankFor` self-heals via its `DictSize()` check,
///     so the rank cache stays valid; filter rules only need a rebuild
///     if a previously-unresolved selected value just got interned.
///   - `Demoted` -- an enum column lost its dictionary (registry
///     erase). The cached `EnumDictionary*` is now dangling, so the
///     rank cache entry must be dropped and rules rebuilt onto the
///     string-set fallback.
enum class EnumColumnsChangeReason : int
{
    Promoted = 0,
    Grew = 1,
    Demoted = 2,
};

class LogModel : public QAbstractTableModel
{
    Q_OBJECT

public:
    /// @p theme drives per-`LogLevel` styling in `data()`; `nullptr`
    /// disables it (colour / font roles return `{}`).
    ///
    /// @p anchors, when non-null, paints anchored rows with the
    /// anchor brush regardless of level style. The model is a
    /// non-owning observer and listens for `anchorChanged` /
    /// `anchorsReset` to scope its `dataChanged` emits.
    ///
    /// @p highlights, when non-null, paints rule-matched rows with
    /// the rule's foreground / background / font. Anchor overlay
    /// still takes precedence; highlights sit between anchors and
    /// the level brush in the render cascade. The model is a
    /// non-owning observer.
    explicit LogModel(
        QObject *parent = nullptr,
        ThemeControl *theme = nullptr,
        AnchorManager *anchors = nullptr,
        HighlightRuleSet *highlights = nullptr
    );
    /// Test-only overload with a custom bounded-queue capacity for the
    /// embedded `QtStreamingLogSink`.
    LogModel(
        QObject *parent,
        std::size_t pendingCapacity,
        ThemeControl *theme = nullptr,
        AnchorManager *anchors = nullptr,
        HighlightRuleSet *highlights = nullptr
    );
    ~LogModel() override;

    /// `(canonical locator, lineId)` for @p row, or nullopt if @p row
    /// is out of range or its `LogLine` has no source.
    ///
    /// `noexcept` because the canonical-locator cache is pre-warmed
    /// on every source change; a stray cache miss returns an empty
    /// locator rather than allocating in the paint stack.
    [[nodiscard]] std::optional<AnchorManager::Key> AnchorKeyForRow(int row) const noexcept;

    /// Linear scan for @p key over visible rows. Returns the first
    /// matching source-row index, or -1 if no live row matches.
    [[nodiscard]] int SourceRowForAnchorKey(const AnchorManager::Key &key) const noexcept;

    /// Anchor palette slot for @p row, or nullopt when the row has
    /// no anchor / isn't valid / the manager isn't wired. Folds the
    /// `AnchorKeyForRow` + `AnchorManager::ColorFor` chain into one
    /// call for overlay callers. Fast-paths through `Empty()` so
    /// anchor-free sessions pay ~nothing.
    [[nodiscard]] std::optional<uint8_t> AnchorSlotForRow(int row) const noexcept;

    /// Full teardown followed by a model reset. Emits `lineCountChanged(0)`,
    /// `errorCountChanged(0)`, and a compensating `streamingFinished` if
    /// a session was still active.
    void Reset();

    /// Same teardown as `Reset()` but keeps visible rows for post-stop
    /// sort/filter/copy.
    void StopAndKeepRows();

    /// Static-file streaming entry point. Runs @p parseCallable on a
    /// `QtConcurrent::run` worker; the future is parked on the model so
    /// teardown can join before the borrowed `LogFile*` is unmapped.
    /// Exceptions escaping the callable surface as a synthetic terminal
    /// `OnBatch` + `OnFinished(false)`.
    loglib::StopToken BeginStreaming(
        std::unique_ptr<loglib::FileLineSource> source, std::function<void(loglib::StopToken)> parseCallable
    );

    /// Append a follow-up file to an already-active static-file session.
    /// Reuses the existing `KeyIndex` so columns line up across files.
    loglib::StopToken AppendStreaming(
        std::unique_ptr<loglib::FileLineSource> source, std::function<void(loglib::StopToken)> parseCallable
    );

    /// Builds the parser the live-tail worker drives. Called on the
    /// worker thread before any bytes are read. An empty factory
    /// defaults to `JsonParser` for back-compat with unported callers.
    using LogParserFactory = std::function<std::unique_ptr<loglib::LogParser>()>;

    /// Live-tail entry point. Takes ownership of @p source, arms the
    /// sink, and spawns a parser worker from @p parserFactory (empty
    /// = `JsonParser`). Teardown order: producer Stop → parser stop
    /// token → worker join → sink drain. Both stop signals are
    /// required — the parser token alone cannot unblock a worker
    /// parked on I/O. `options.stopToken` is overwritten with the
    /// sink's token before the worker captures it.
    loglib::StopToken BeginStreaming(
        std::unique_ptr<loglib::StreamLineSource> source,
        loglib::ParserOptions options,
        LogParserFactory parserFactory = {}
    );

    /// Test-only: install @p source and arm the sink without spawning a
    /// worker. Pair with `EndStreaming(...)` or `Reset()`.
    loglib::StopToken BeginStreamingForSyncTest(std::unique_ptr<loglib::LineSource> source);

    /// Append one streamed batch and emit Qt model signals. When a
    /// `RetentionCap()` is set, FIFO-evict the visible prefix before
    /// insertion; over-cap batches are head-trimmed so per-batch
    /// eviction stays O(cap).
    void AppendBatch(loglib::StreamedBatch batch);

    /// Finalise the streaming parse and emit `streamingFinished`.
    void EndStreaming(bool cancelled);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    int columnCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    template <typename T> std::optional<std::pair<T, T>> GetMinMaxValues(int column) const;

    const loglib::LogTable &Table() const;
    loglib::LogTable &Table();
    const loglib::LogData &Data() const;
    const loglib::LogConfiguration &Configuration() const;
    loglib::LogConfigurationManager &ConfigurationManager();

    /// GUI-side bridging sink owned by the model.
    QtStreamingLogSink *Sink();

    /// Per-line errors collected since the last `Reset`/`BeginStreaming`.
    /// Also appends a synthetic message for any newly-dropped batches
    /// reported by the sink (back-pressure shutdown).
    const std::vector<std::string> &StreamingErrors() const;

    /// Whether a streaming session is currently armed.
    [[nodiscard]] bool IsStreamingActive() const noexcept;

    /// Update the in-memory line cap.
    ///   - Running: FIFO-trim visible rows to the new cap; raising the
    ///     cap has no immediate effect.
    ///   - Paused: trim the paused buffer so visible+buffered <= cap.
    ///   - Idle: record for the next session.
    /// `cap == 0` means unbounded; live-tail substitutes
    /// `StreamingControl::DEFAULT_RETENTION_LINES` if still 0.
    void SetRetentionCap(size_t cap);

    /// Current retention cap (`0` means unbounded). GUI thread only.
    [[nodiscard]] size_t RetentionCap() const noexcept;

    /// UTF-8 bytes -> single-line, simplified `QString` (the
    /// `Qt::DisplayRole` representation). Public so `MatchRow` and
    /// `MainWindow::MakeStringMatcher` apply the same normalisation
    /// the user sees on screen.
    static QString ConvertToSingleLineCompactQString(std::string_view bytes);

    /// True iff @p bytes is already byte-equal to
    /// `ConvertToSingleLineCompactQString(bytes).toUtf8()`: pure 7-bit
    /// ASCII, no leading/trailing space, no double-space, no
    /// `\n`/`\r`/`\t`/`\v`/`\f`/control byte. When both pattern and
    /// haystack pass, `MakeStringMatcher`'s `Exactly` / `Contains`
    /// paths byte-compare directly and skip the
    /// `QString::fromUtf8` + `simplified()` walk. Early-exits on the
    /// first violating byte so the common ASCII log line costs a
    /// single linear scan.
    [[nodiscard]] static bool IsSingleLineAsciiTrim(std::string_view bytes) noexcept;

    /// Move column @p srcIndex to @p destIndex (absolute final
    /// position, not Qt's "insert before"). Emits `columnsMoved`
    /// and remaps `LogFilter::row` so saved filters follow the
    /// column. Returns `true` on success.
    bool MoveColumn(int srcIndex, int destIndex);

    /// Tell the view to refresh its column structure after an
    /// out-of-band rewrite of the configuration (e.g. `Load`).
    /// Wraps a `beginResetModel` / `endResetModel`; row data is
    /// untouched.
    void NotifyConfigurationReplaced();

    /// Most recent `ColumnTypeHealth` snapshot for @p section, or
    /// `nullopt` if out of range or no snapshot exists yet. Drives
    /// the header tooltip / warning icon and the status-bar summary.
    [[nodiscard]] std::optional<loglib::LogTable::ColumnTypeHealth> ColumnHealth(int section) const;

    /// Replace the per-column active-filter titles cache used by
    /// the funnel decoration and tooltip in `headerData`. Each
    /// entry holds the titles of one column's active filters
    /// (caller pre-sorts them); empty entry = no filter.
    /// `perColumnTitles` is resized to `columnCount()`. Idempotent:
    /// emits `headerDataChanged` only for the changed range.
    void SetColumnFilterDetails(std::vector<QStringList> perColumnTitles);

    /// True if @p section has any cached filter title. Out-of-range
    /// returns `false`.
    [[nodiscard]] bool HasFilterForColumn(int section) const noexcept;

    /// Drop the cached funnel icon and re-emit `headerDataChanged`
    /// for filtered columns so the header re-renders against the
    /// new palette. Called from `MainWindow::OnThemeChanged`.
    void RefreshHeaderIcons();

    /// Recompute every column's `ColumnTypeHealth` snapshot. Emits
    /// `headerDataChanged` and `columnHealthChanged` when something
    /// moved. Walks the whole table; meant for end-of-stream and
    /// post-`Load`, not per-batch.
    void RefreshColumnHealth();

    /// Emit `headerDataChanged` + a column-wide `dataChanged` after
    /// an out-of-band column edit (header / type / visibility).
    /// No-op for an out-of-range index.
    void NotifyColumnEdited(int columnIndex);

    /// Apply a `(type, autoDetect)` edit at @p columnIndex as one
    /// transaction: write the pair through the manager, reconcile
    /// loaded rows (back-fill, drop dictionaries / time formats as
    /// needed), emit `enumColumnsChanged` when the edit crosses the
    /// enum/level boundary, and refresh `ColumnHealth`. Out-of-range
    /// index is a silent no-op.
    void ApplyColumnTypeEdit(int columnIndex, loglib::LogConfiguration::Type newType, bool newAutoDetect);

    /// Canonical-level -> raw-dictionary-bytes mapping captured just
    /// before a `Type::Level` column lost its dictionary in the most
    /// recent `AppendBatch`. Lets the `enumColumnsChanged(Demoted)`
    /// receiver translate canonical-name filters (`"Info"`) into the
    /// raw entries (`"info"`, `"INF"`, ...) the column actually
    /// contained. Returns `nullptr` when no level demote happened for
    /// @p columnIndex in the last batch. Cleared at the start of every
    /// subsequent `AppendBatch`.
    [[nodiscard]] const std::unordered_map<loglib::LogLevel, std::vector<std::string>> *LastBatchLevelDemoteMappingFor(
        int columnIndex
    ) const noexcept;

    /// Emit `dataChanged` for the theme-derived style roles
    /// (Background, Foreground, Font) across the whole table.
    /// `MainWindow::OnThemeChanged` calls this on Light <-> Dark
    /// flips; `viewport()->update()` alone doesn't reliably
    /// invalidate the view's per-item style cache.
    void RefreshAllRowStyles();

    /// True iff @p roles is non-empty AND every entry is purely
    /// decorative (Background / Foreground / Font / Decoration /
    /// ToolTip). Value-only listeners (find cache, record-detail)
    /// use this to skip theme-refresh and icon-mode emits.
    ///
    /// Empty `roles` is Qt's "I don't know what changed" sentinel;
    /// this helper reports `false` so callers conservatively refresh.
    ///
    /// Emitter contract: any `dataChanged` listing only these roles
    /// MUST NOT mutate value-bearing roles for the emitted range.
    [[nodiscard]] static bool IsStyleOnlyRoleChange(const QList<int> &roles) noexcept;

    /// Cached first-`Type::Level` column index (`-1` when none).
    /// Public so `MainWindow::ApplyLevelCellDelegate` and the
    /// `data()`/`headerData()` icon-mode branches share one scan.
    [[nodiscard]] int FirstLevelColumnIndex() const noexcept;

    /// Toggle the "Show level icons" user preference. When on (and
    /// the theme supplies a `levelColumnOverride`), the level
    /// column renders as an icon pill. Emits scoped `headerDataChanged`
    /// + per-cell `dataChanged` on the level column so any header
    /// override pops in/out without a full reset.
    void SetShowLevelIcons(bool show);

    /// `mShowLevelIcons && mTheme && mTheme->HasLevelColumnOverride()`.
    /// Single source of truth for "is the level column in icon-pill
    /// mode?" -- consulted by `data()` and the delegate's self-gate.
    [[nodiscard]] bool IsLevelIconModeActive() const noexcept;

signals:
    /// Cumulative error count, emitted when a batch carries errors.
    void errorCountChanged(qsizetype count);

    /// Emitted after every `AppendBatch`.
    void lineCountChanged(qsizetype count);

    /// Emitted from `EndStreaming` (and as a compensating signal from
    /// `Reset()` when the queued `OnFinished` was generation-stamped).
    void streamingFinished(StreamingResult result);

    /// Rotation reported by the active producer; re-emitted on the GUI.
    void rotationDetected();

    /// Producer status transition; re-emitted on the GUI.
    void sourceStatusChanged(loglib::SourceStatus status);

    /// Emitted when a `Type::Enumeration` column or its dictionary
    /// changes shape (promotion, dict growth, end-of-stream finalise).
    /// One signal per affected column so receivers can scope by
    /// @p columnIndex (source-table coords). `columnIndex == -1` is
    /// "unscoped" (registry-wide sweep) -- treat as "any enum filter
    /// may need attention". See `EnumColumnsChangeReason`.
    void enumColumnsChanged(EnumColumnsChangeReason reason, int columnIndex);

    /// Emitted from `RefreshColumnHealth` when at least one column's
    /// snapshot changed. Drives the status-bar mismatch summary and
    /// the Configuration Diagnostics dialog.
    void columnHealthChanged();

private:
    /// Shared `BeginStreaming` setup: install @p source, reset the
    /// model, and arm the sink. Reserves per-line offsets for
    /// `FileLineSource` inputs.
    void BeginStreamingShared(std::unique_ptr<loglib::LineSource> source);

    /// Shared implementation of `Reset()` / `StopAndKeepRows()`.
    void TeardownStreamingSessionInternal(bool resetTable);

    /// Canonical level for @p row via the first `Type::Level`
    /// column. Returns nullopt when there's no level column or the
    /// row has no resolvable level. Drives the Background /
    /// Foreground / Font role branches in `data()`.
    [[nodiscard]] std::optional<loglib::LogLevel> LevelForRow(int row) const noexcept;

    /// Like `LevelForRow` but returns `LogLevel::Unknown` for
    /// unmapped values so the icon-mode paint paths can render the
    /// generic "unknown" glyph. `nullopt` still means "no value at
    /// all", which keeps blank cells blank.
    [[nodiscard]] std::optional<loglib::LogLevel> DisplayLevelForRow(int row) const noexcept;

    /// Linear scan for the first `Type::Level` column. Returns
    /// `LEVEL_COLUMN_NONE` when none match.
    [[nodiscard]] int ComputeFirstLevelColumnIndex() const noexcept;

    static constexpr int LEVEL_COLUMN_UNCACHED = -2;
    static constexpr int LEVEL_COLUMN_NONE = -1;

    loglib::LogTable mLogTable;
    QtStreamingLogSink *mSink = nullptr;

    /// Future for the active parse worker.
    QFutureWatcher<void> *mStreamingWatcher = nullptr;

    /// Producer of the active session, or nullptr.
    [[nodiscard]] loglib::BytesProducer *ActiveProducer() noexcept;

    qsizetype mErrorCount = 0;

    /// Race-free "still streaming" flag; the watcher's `isRunning()`
    /// flips off before the queued `OnFinished` reaches the GUI.
    bool mStreamingActive = false;

    mutable std::vector<std::string> mStreamingErrors;

    /// High-water mark of sink-dropped batches we've already surfaced.
    mutable std::size_t mLastReportedShutdownDropCount = 0;

    /// Retention cap; `0` means unbounded.
    size_t mRetentionCap = 0;

    /// Per-batch capture: column -> (canonical level -> raw dictionary
    /// bytes), recorded when a `Type::Level` column demotes during the
    /// current `AppendBatch`. Consumed by
    /// `MainWindow::enumColumnsChanged(Demoted)`; cleared at the top
    /// of every `AppendBatch`.
    std::unordered_map<int, std::unordered_map<loglib::LogLevel, std::vector<std::string>>>
        mLastBatchLevelDemoteMapping;

    /// Per-column health cache, parallel to `Configuration().columns`.
    /// Written only by `RefreshColumnHealth`; empty until first refresh.
    std::vector<loglib::LogTable::ColumnTypeHealth> mColumnHealth;

    /// Per-column active-filter titles, parallel to
    /// `Configuration().columns`. Written only by
    /// `SetColumnFilterDetails`; empty entry = no filter.
    std::vector<QStringList> mColumnFilterDetails;

    /// Themed funnel glyph for the "column has a filter" header
    /// decoration, rendered lazily on first decoration query and
    /// cleared by `RefreshHeaderIcons` on palette changes.
    /// `mutable` so `headerData` (const) can populate the cache.
    /// `mFunnelIconAttempted` guards against re-rendering on every
    /// repaint if the resource ever fails to load.
    mutable QIcon mFunnelIconCache;

    /// Latched after the first `mFunnelIconCache` load attempt;
    /// reset by `RefreshHeaderIcons` to force a re-render.
    mutable bool mFunnelIconAttempted = false;

    /// Cached first-`Type::Level` column index. Sentinel
    /// `LEVEL_COLUMN_UNCACHED` before first scan, `LEVEL_COLUMN_NONE`
    /// when no level column exists. Invalidated by every structural
    /// mutator.
    mutable int mFirstLevelColumnCache = LEVEL_COLUMN_UNCACHED;

    /// "Show level icons" pref (persisted by `MainWindow` as
    /// `ui/showLevelIcons`). Defaults true so fresh installs see
    /// the icon-mode visual on themes that ship the override.
    bool mShowLevelIcons = true;

    /// Non-owning theme controller pointer. May be null when the
    /// model is constructed via the legacy default ctor (test
    /// fixtures that don't exercise theming); `data()` gates its
    /// theme-derived branches on a null check.
    ThemeControl *mTheme = nullptr;

    /// Non-owning anchor manager; null for legacy test fixtures.
    /// The `data()` anchor branch and signal wiring null-check first.
    AnchorManager *mAnchors = nullptr;

    /// Non-owning highlight rule set; null for legacy test fixtures.
    /// The `data()` render cascade slots highlights between anchors
    /// and level brushes. Signals (`rulesChanged` / `matchesChanged`)
    /// are wired at construction so the model repaints when the rule
    /// list or match cache mutates.
    HighlightRuleSet *mHighlights = nullptr;

    /// Emit `dataChanged` (Background + Foreground + Font) across
    /// the whole visible table. Used on the highlight rule set's
    /// `rulesChanged` / `matchesChanged` signals.
    void RefreshAllHighlightRows();

    /// Emit `dataChanged` (Background + Foreground) on every row
    /// matching @p key. Usually one row; multi-file sessions may
    /// have id collisions across sources.
    void RefreshRowsForAnchor(const AnchorManager::Key &key);

    /// Drop anchors on rows `[0, dropCount)` before the table's
    /// `EvictPrefixRows` runs, so anchors can't dangle past FIFO
    /// retention.
    void DropAnchorsForEvictionPrefix(int dropCount);

    /// Emit `dataChanged` (Background + Foreground) across the whole
    /// visible table. Used on `anchorsReset`.
    void RefreshAllAnchorRows();

    /// Cache the canonical locator for every live `LineSource`.
    /// Called after each source mutation so `AnchorKeyForRow` stays
    /// noexcept on the paint path. Idempotent; O(unique sources).
    void PrewarmCanonicalLocatorCache();

    /// Per-`LineSource` cache of canonical locators. Keyed by raw
    /// pointer (stable for the source store's lifetime). Cleared on
    /// session swap to avoid dangling keys.
    mutable std::unordered_map<const loglib::LineSource *, std::string> mCanonicalLocatorCache;
};

Q_DECLARE_METATYPE(StreamingResult)
Q_DECLARE_METATYPE(EnumColumnsChangeReason)
Q_DECLARE_METATYPE(loglib::SourceStatus)
