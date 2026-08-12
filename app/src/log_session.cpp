#include "log_session.hpp"

#include "anchor_manager.hpp"
#include "highlight_rule_set.hpp"
#include "log_filter_model.hpp"
#include "log_model.hpp"
#include "row_order_proxy_model.hpp"

#include <Qt>

// Prospective includes for Phase 2 — the ctor and the accessors
// only need forward declarations today, but Phase 2 begins
// dispatching commands through these services so we pull in the
// definitions now to keep the churn on later PRs small.
#include "regex_template_registry.hpp"
#include "session_history_manager.hpp"
#include "theme_control.hpp"

#include <loglib/log_configuration.hpp>
#include <loglib/log_value.hpp>

#include <QAbstractItemModel>
#include <QFileInfo>
#include <QModelIndex>
#include <QPointer>
#include <QSettings>
#include <QString>
#include <QTimer>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <utility>
#include <variant>

SessionInstanceId SessionInstanceId::Next() noexcept
{
    // Process-scoped monotonic counter. `1` is the first valid id so
    // a default-constructed `SessionInstanceId{}` remains the
    // sentinel "no session".
    static std::atomic<Value> counter{0};
    return SessionInstanceId{counter.fetch_add(1, std::memory_order_relaxed) + 1};
}

LogSession::LogSession(
    ThemeControl *theme,
    SessionHistoryManager *historyManager,
    RegexTemplateRegistry *regexTemplateRegistry,
    QObject *parent
)
    : QObject(parent),
      mTheme(theme),
      mHistoryManager(historyManager),
      mRegexTemplateRegistry(regexTemplateRegistry),
      // Construction order matters at *construction* time:
      //   * `AnchorManager` and `HighlightRuleSet` are built first
      //     because `LogModel`'s constructor takes non-owning
      //     pointers to both (anchor overlay hook +
      //     `HighlightRuleSet::RebindColumns` paint cascade). They
      //     must exist before `mModel` is passed them.
      //   * `LogModel` sinks streaming batches and owns the
      //     `LogTable`; it must exist before either proxy so the
      //     `setSourceModel` calls in the body have a target.
      //   * The two proxies chain source-model relationships
      //     (`RowOrderProxyModel` -> `LogFilterModel`).
      //
      // Destruction order matters too, but Qt6's
      // ``QObjectPrivate::deleteChildren`` walks children in
      // *forward* registration order -- so if we relied on the
      // parent-driven sweep alone, `mAnchors` and `mHighlights`
      // would die *before* `mModel`, briefly leaving `mModel`'s
      // non-owning back-pointers dangling until `~LogModel`
      // finishes. This is safe today because none of the
      // destructors dereference those pointers, but a future
      // contributor adding e.g. ``disconnect(mAnchors, ...)`` to
      // ``~LogModel`` would silently hit UB.
      //
      // The explicit reverse-order teardown in ``~LogSession``
      // below makes the intent explicit and closes that window:
      // the proxies die first, then the model, then the two
      // non-owning back-pointer targets. By the time the ``QObject``
      // base-class sweep runs it has no children left to reap.
      mAnchors(new AnchorManager(this)),
      mHighlights(new HighlightRuleSet(this)),
      mModel(new LogModel(this, theme, mAnchors, mHighlights)),
      mRowOrderProxyModel(new RowOrderProxyModel(this)),
      mSortFilterProxyModel(new LogFilterModel(this))
{
    mRowOrderProxyModel->setSourceModel(mModel);
    mSortFilterProxyModel->setSourceModel(mRowOrderProxyModel);
    mSortFilterProxyModel->SetLogModel(mModel);

    // Highlight-cache invariants that are purely session-local live
    // here so a future multi-session window does not resurface the
    // wires against the wrong model. `columnsInserted` still has
    // shell-scoped work (editor UI refresh, filter-menu recompile)
    // and stays in `MainWindow` until task 2.4 splits it.
    connect(mModel, &QAbstractItemModel::rowsInserted, this, [this](const QModelIndex &, int first, int last) {
        if (first < 0 || last < first)
        {
            return;
        }
        mHighlights->OnRowsAppended(mModel->Table(), static_cast<std::size_t>(first), static_cast<std::size_t>(last));
    });
    // FIFO retention fires `rowsRemoved` before appending the new
    // tail; without this the cache would keep stale prefix entries
    // and misalign every subsequent `LastMatchFor`.
    connect(mModel, &QAbstractItemModel::rowsRemoved, this, [this](const QModelIndex &, int first, int last) {
        if (first < 0 || last < first)
        {
            return;
        }
        mHighlights->OnRowsEvicted(static_cast<std::size_t>(first), static_cast<std::size_t>(last));
    });
    connect(mModel, &QAbstractItemModel::modelReset, this, [this]() { mHighlights->ClearMatches(); });
    // Column additions can newly resolve highlight-rule keys, so we
    // must recompile the ruleset against the current columns.
    // Pinned by
    // `TestEnumPromotedOnUnrelatedColumnDoesNotRebuildFilters` on
    // the shell side (filter menus stay untouched when a key was
    // already resolved), which relies on `RebindColumns` running on
    // every column-insert.
    connect(mModel, &QAbstractItemModel::columnsInserted, this, [this](const QModelIndex &, int, int) {
        mHighlights->RebindColumns(mModel->Configuration().columns, &mModel->Table());
    });
}

LogSession::~LogSession()
{
    // Defensive drain of the async watchers before either they
    // or the model quintet go away. In the shell path the
    // window's ~MainWindow already ran `CancelInFlightExport()`
    // and `CancelInFlightDecompression()` which drain via
    // `waitForFinished()`, so both watchers observe an already
    // idle future here and this is a no-op. The drain still
    // matters for two edge cases:
    //
    //   1. A bare `LogSession` in a unit test that armed a
    //      watcher via `Ensure*Watcher()` + `setFuture(...)` and
    //      then let the session go out of scope: without this
    //      drain, `~QFutureWatcher` would tear down while its
    //      future is still running (per Qt docs, the future
    //      set with `setFuture()` must be finished when the
    //      watcher is destroyed).
    //
    //   2. A future refactor that skips the shell's
    //      `CancelInFlight*` on some teardown path: this drain
    //      keeps the invariant local so the class does not
    //      silently rot when the shell layer changes.
    //
    // We only *wait* here (no `request_stop()`) because owning
    // the cancel policy would collide with the shell's cancel
    // dialogs on the primary teardown path.
    if (mDecompressionWatcher != nullptr)
    {
        mDecompressionWatcher->waitForFinished();
    }
    if (mExportWatcher != nullptr)
    {
        // `waitForFinished()` on the export watcher can rethrow
        // whatever the worker let escape (`ExportCancelled` on
        // the normal cancel path, plus any I/O error). Swallow
        // both so this destructor stays no-throw. Mirrors the
        // shell's `CancelInFlightExport()` catch.
        try
        {
            mExportWatcher->waitForFinished();
        }
        catch (...) // NOLINT(bugprone-empty-catch)
        {
            // Intentional: we are tearing down the session and
            // there is no UI context left to report errors to.
        }
    }

    // Explicit reverse-order teardown of the model quintet so
    // ``~LogModel`` runs while its non-owning back-pointers to
    // ``mAnchors`` and ``mHighlights`` are still valid. See the
    // long comment in the ctor initializer list for the Qt6
    // forward-child-sweep rationale.
    //
    // Each ``delete`` here also removes the child from
    // ``QObject``'s children list (Qt does this in the child's
    // dtor via ``setParent(nullptr)``-equivalent bookkeeping),
    // so the base-class ``~QObject`` sweep runs against an
    // already-emptied children list. The drained watchers above
    // are still parented on ``this`` and are reaped by that
    // sweep in whatever order Qt chose for them.
    delete mSortFilterProxyModel;
    mSortFilterProxyModel = nullptr;
    delete mRowOrderProxyModel;
    mRowOrderProxyModel = nullptr;
    delete mModel;
    mModel = nullptr;
    delete mHighlights;
    mHighlights = nullptr;
    delete mAnchors;
    mAnchors = nullptr;
}

void LogSession::RequestNewSession()
{
    // Phase 2 will forward to the migrated `NewSession` body.
}

void LogSession::RequestOpenFiles(const QStringList & /*files*/, OpenMode /*mode*/)
{
    // Phase 2 will forward to the migrated static-open dispatch.
}

void LogSession::RequestOpenLogStream(const QString & /*filePath*/)
{
    // Phase 2 will forward to the migrated live-tail open path.
}

void LogSession::RequestAutoSaveSnapshot(bool /*publishOpenWindow*/)
{
    // Phase 2 will forward to `AutoSaveSessionSnapshot` after the
    // history-manager wiring moves into `LogSession`.
}

std::uint32_t LogSession::PreCheckClose() const
{
    // Session-local close probe (task 2.13; review finding #2).
    // Idempotent and side-effect-free: the shell calls this to
    // *ask* which follow-ups (worker drain, user prompt) are
    // required before the tab can close silently.
    //
    // A zero return means the tab is safe to tear down without
    // prompting. Non-zero bits map 1:1 to shell-owned follow-ups
    // so the caller does not have to inspect three separate
    // getters (`IsFiltersDirty`, `IsDecompressionInFlight`,
    // `IsExportInFlight`) after receiving an ambiguous
    // "Cancelled" value.
    //
    // The side-effecting sibling `RequestClose` is stubbed in
    // Phase 2 because the worker-drain / prompt orchestration
    // still lives on the shell; when phase 3 moves the
    // orchestration in, `RequestClose` becomes the sole entry
    // point and this helper stays useful for the pre-walk that
    // decides whether to raise a "closing multiple tabs" dialog.
    std::uint32_t mask = 0;
    if (mFiltersDirty)
    {
        mask |= SessionClosePreconditions::FiltersDirty;
    }
    if (mDecompressionInFlight)
    {
        mask |= SessionClosePreconditions::DecompressionInFlight;
    }
    if (mExportInFlight)
    {
        mask |= SessionClosePreconditions::ExportInFlight;
    }
    return mask;
}

SessionCloseResult LogSession::RequestClose()
{
    // Phase 2 stub (see `LogSessionCommands::RequestClose` docstring).
    // The shell still drives cancel-and-drain / save prompts from
    // `MainWindow::closeEvent`; consult `PreCheckClose()` instead
    // to inspect the reasons a close would not be silent.
    return SessionCloseResult::Closed;
}

SessionPresentationSnapshot LogSession::PresentationSnapshot() const
{
    SessionPresentationSnapshot snapshot;

    // ------------------------------------------------------------------
    // Source mode / operations bitmask -- projected from the migrated
    // scalar state (task 2.5/2.7/2.8/2.9). The mode enum is
    // single-valued; see `SessionSourceMode`'s docstring for the
    // priority order between overlapping classifications (review
    // finding #4). Later phases can layer additional latches (e.g.
    // "was originally compressed" survives decompression completion)
    // without moving the responsibility off `LogSession`.
    // ------------------------------------------------------------------
    snapshot.mode = [&] {
        // Stream source kinds override everything: neither Bundle nor
        // MultiFile applies to a stdin / network producer.
        if (mCurrentSource.has_value())
        {
            switch (mCurrentSource->kind)
            {
            case loglib::LogConfiguration::Source::Kind::Stdin:
                return SessionSourceMode::Stdin;
            case loglib::LogConfiguration::Source::Kind::NetworkStream:
                return SessionSourceMode::Network;
            case loglib::LogConfiguration::Source::Kind::File:
                break;
            }
        }

        switch (mMode)
        {
        case Mode::Idle:
            return SessionSourceMode::Idle;
        case Mode::LiveTail:
            // Bundle beats LiveTail because the bundle badge is more
            // distinctive; a live-tail bundle is exotic and the
            // rendering still needs the bundle chrome.
            if (ShouldApplyEmbeddedBundleConfig())
            {
                return SessionSourceMode::Bundle;
            }
            return SessionSourceMode::LiveTail;
        case Mode::Static:
            if (ShouldApplyEmbeddedBundleConfig())
            {
                return SessionSourceMode::Bundle;
            }
            if (mCurrentSource.has_value() && mCurrentSource->locators.size() > 1U)
            {
                return SessionSourceMode::MultiFile;
            }
            if (mDecompressionInFlight)
            {
                return SessionSourceMode::Compressed;
            }
            return SessionSourceMode::StaticFile;
        }
        return SessionSourceMode::Idle;
    }();

    std::uint32_t ops = 0;
    if (mDecompressionInFlight)
    {
        ops |= static_cast<std::uint32_t>(SessionOperationState::Decompressing);
    }
    if (mExportInFlight)
    {
        ops |= static_cast<std::uint32_t>(SessionOperationState::Exporting);
    }
    if (mSourceWaiting)
    {
        ops |= static_cast<std::uint32_t>(SessionOperationState::SourceWaiting);
    }
    if (mMode == Mode::Static && !mFirstStreamingBatchSeen)
    {
        // `Parsing` == "Static parse in progress (streaming to model)"
        // per `SessionOperationState`'s enum comment. Review finding
        // #3: gate on `Mode::Static` so a fresh LiveTail open with
        // no data yet does not also project `Parsing` alongside
        // `Ingesting` and confuse the tab-strip badge selector.
        // Ingesting/Paused/Disconnected land when the producer
        // lifecycle moves into `LogSession` (task 2.10).
        ops |= static_cast<std::uint32_t>(SessionOperationState::Parsing);
    }
    if (mMode == Mode::LiveTail && !mSourceWaiting)
    {
        ops |= static_cast<std::uint32_t>(SessionOperationState::Ingesting);
    }
    snapshot.operations = ops;

    // ------------------------------------------------------------------
    // Dirty state. The `filtersDirty` and `restorableInPlace` /
    // `ephemeralUnreproducible` flags follow the same taxonomy the
    // shell already uses in ``ShouldAutoSaveAfterStreaming`` and
    // ``RestorableSessionUuid``.
    // ------------------------------------------------------------------
    snapshot.dirty.filtersDirty = mFiltersDirty;
    if (mCurrentSource.has_value())
    {
        // The three "restorable" predicates (`restorableInPlace`
        // here, `RestorableSessionUuid`, `ShouldAutoSaveAfterStreaming`)
        // must agree on the "File descriptor + empty locators"
        // corner case (a partially-cancelled open leaves the
        // descriptor pinned but the locator vector empty). Review
        // finding #6 already fixed `RestorableSessionUuid` to
        // treat this as "not restorable"; without the parallel
        // guard here, the tab strip would advertise
        // `restorableInPlace=true` while the actual autosave gate
        // silently refuses to persist. Keep the three predicates
        // in lockstep.
        const auto &source = *mCurrentSource;
        const bool isFile = source.kind == loglib::LogConfiguration::Source::Kind::File;
        const bool hasLocators = !source.locators.empty();
        snapshot.dirty.restorableInPlace = isFile && hasLocators && mMode != Mode::LiveTail;
        // Stdin / network can't be reopened from a saved locator, and a
        // live-tail file source is not restore-safe either (would
        // silently downgrade to a static open). A File descriptor
        // with no locators is not "ephemeral" per se -- the caller
        // can retry the open -- so it maps to neither flag.
        snapshot.dirty.ephemeralUnreproducible = !isFile || (isFile && mMode == Mode::LiveTail);
    }

    // ------------------------------------------------------------------
    // Labels / status text. Populated from the migrated
    // `mStreamingFileName` + source descriptor.
    //
    // Review finding #5: `mStreamingFileName` can be a full path
    // (e.g. `C:/logs/app.log`) or a bare basename depending on how
    // the shell seeded it. The `SessionPresentationSnapshot`
    // docstring is explicit: `shortLabel` is "For the tab title
    // (elided-safe, no path)" while `tooltip` is "For the tab
    // tooltip (full source or set)". Split them here so the tab
    // strip never leaks a path.
    //
    // The full elision policy (multi-locator "app.log +2", bundle
    // qualifier, compressed suffix badge) lands in `LogSessionView`
    // (task 3.x) once the descriptor-driven labels move in.
    // ------------------------------------------------------------------
    if (!mStreamingFileName.isEmpty())
    {
        snapshot.tooltip = mStreamingFileName;
        snapshot.sourceLabel = mStreamingFileName;
        const QString basename = QFileInfo(mStreamingFileName).fileName();
        // `QFileInfo::fileName` returns "" for a bare directory path
        // ("C:/logs/") or for a string that ends in a separator; fall
        // back to the seed so the tab title still shows *something*
        // rather than a mysterious empty label.
        snapshot.shortLabel = basename.isEmpty() ? mStreamingFileName : basename;
    }

    // ------------------------------------------------------------------
    // Row counts / errors. Model may be null in unit-test fixtures
    // that build a bare `LogSession` without wiring services;
    // guard every deref.
    // ------------------------------------------------------------------
    if (mModel != nullptr)
    {
        snapshot.rowCount = static_cast<qsizetype>(mModel->rowCount());
    }
    if (mSortFilterProxyModel != nullptr)
    {
        snapshot.visibleRows = static_cast<qsizetype>(mSortFilterProxyModel->rowCount());
    }
    snapshot.errorCount = static_cast<qsizetype>(mStreamingErrorCount);
    snapshot.droppedErrors = static_cast<qsizetype>(mStreamingErrorsCut);

    // ------------------------------------------------------------------
    // Mutation / close-confirm gates. Decompression + export gate
    // mutations today (window-wide before migration; now session-
    // local). Any in-flight worker triggers the close-confirm prompt.
    // ------------------------------------------------------------------
    snapshot.mutationsAllowed = !mDecompressionInFlight && !mExportInFlight;
    snapshot.confirmBeforeClose = mFiltersDirty || mDecompressionInFlight || mExportInFlight;

    return snapshot;
}

ThemeControl *LogSession::Theme() const noexcept
{
    return mTheme;
}

SessionHistoryManager *LogSession::HistoryManager() const noexcept
{
    return mHistoryManager;
}

RegexTemplateRegistry *LogSession::RegexTemplates() const noexcept
{
    return mRegexTemplateRegistry;
}

AnchorManager *LogSession::Anchors() const noexcept
{
    return mAnchors;
}

HighlightRuleSet *LogSession::Highlights() const noexcept
{
    return mHighlights;
}

LogModel *LogSession::Model() const noexcept
{
    return mModel;
}

RowOrderProxyModel *LogSession::RowOrderProxy() const noexcept
{
    return mRowOrderProxyModel;
}

LogFilterModel *LogSession::FilterProxy() const noexcept
{
    return mSortFilterProxyModel;
}

void LogSession::MarkFiltersDirty()
{
    if (mLoadingConfiguration || mFiltersDirty)
    {
        return;
    }
    mFiltersDirty = true;
    emit filtersDirtyChanged(true);
    // Dirty state and `confirmBeforeClose` are both part of the
    // presentation snapshot; the shell's tab-strip / window-title
    // subscribers rely on the combined `presentationChanged` fan
    // so they don't have to listen to every state signal.
    emit presentationChanged();
}

void LogSession::ClearFiltersDirty()
{
    if (!mFiltersDirty)
    {
        return;
    }
    mFiltersDirty = false;
    emit filtersDirtyChanged(false);
    emit presentationChanged();
}

void LogSession::SetLoadingConfiguration(bool loading) noexcept
{
    mLoadingConfiguration = loading;
}

void LogSession::SetPendingApplySortFromConfig(bool pending) noexcept
{
    mPendingApplySortFromConfig = pending;
}

void LogSession::ResetSimpleFilterState() noexcept
{
    mSimpleLeaves.clear();
    mSimpleLeafOrder.clear();
}

void LogSession::SetApplyingEnumRebuild(bool applying) noexcept
{
    mApplyingEnumRebuild = applying;
}

void LogSession::RebuildFilterExpressionFromSimpleLeaves()
{
    // Preserved invariant from the old MainWindow implementation:
    //   1. Simple-mode leaves become the leading `And` children,
    //      preserving `SimpleLeafOrder()`.
    //   2. Advanced-mode `Or`/`Not` subtrees carried on the
    //      existing expression must survive a simple-mode edit —
    //      an existing root `And` contributes its non-`Leaf`
    //      children; an existing root `Or`/`Not` is preserved
    //      wholesale as a single child.
    //   3. A bare-`Leaf` root (legacy config / direct expression
    //      set) is preserved unless the same rule is already
    //      reflected in `SimpleLeaves()`.
    loglib::FilterExpression::And newAnd;
    newAnd.children.reserve(mSimpleLeafOrder.size() + 1);
    for (const auto &id : mSimpleLeafOrder)
    {
        const auto it = mSimpleLeaves.find(id);
        if (it == mSimpleLeaves.end())
        {
            continue;
        }
        loglib::FilterExpression leaf;
        leaf.node = loglib::FilterExpression::Leaf{it->second};
        newAnd.children.push_back(std::move(leaf));
    }
    // Copy by value: `SetExpression` below invalidates references
    // into `mModel->Configuration().expression`.
    const loglib::FilterExpression existing = mModel->Configuration().expression;
    if (const auto *existingAnd = std::get_if<loglib::FilterExpression::And>(&existing.node); existingAnd != nullptr)
    {
        for (const auto &child : existingAnd->children)
        {
            if (!std::holds_alternative<loglib::FilterExpression::Leaf>(child.node))
            {
                newAnd.children.push_back(child);
            }
        }
    }
    else if (
        std::holds_alternative<loglib::FilterExpression::Or>(existing.node) ||
        std::holds_alternative<loglib::FilterExpression::Not>(existing.node)
    )
    {
        newAnd.children.push_back(existing);
    }
    else if (
        const auto *existingLeaf = std::get_if<loglib::FilterExpression::Leaf>(&existing.node); existingLeaf != nullptr
    )
    {
        const bool alreadyInSimple = std::ranges::any_of(mSimpleLeaves, [&existingLeaf](const auto &entry) {
            return entry.second == existingLeaf->rule;
        });
        if (!alreadyInSimple)
        {
            newAnd.children.push_back(existing);
        }
    }
    loglib::FilterExpression rootExpression;
    rootExpression.node = std::move(newAnd);
    mModel->ConfigurationManager().SetExpression(std::move(rootExpression));
}

void LogSession::MirrorSortToConfiguration()
{
    // Read live from the proxy so the persisted value matches what
    // the user sees. *Exception:* while a deferred sort is pending
    // and the proxy is still unsorted (`-1`), preserve the
    // configuration's existing sort — the live `-1` is transient
    // until `OnStreamingFinished` reapplies the loaded sort.
    const int proxySortColumn = mSortFilterProxyModel->SortColumn();
    if (proxySortColumn >= 0 || !mPendingApplySortFromConfig)
    {
        loglib::LogConfiguration::Sort sort;
        sort.columnIndex = proxySortColumn;
        sort.descending = (mSortFilterProxyModel->SortOrder() == Qt::DescendingOrder);
        mModel->ConfigurationManager().SetSort(sort);
    }
}

void LogSession::MirrorAnchorsToConfiguration()
{
    // `mAnchors` and `mModel` are constructed as children of `this`
    // in the ctor and are guaranteed non-null for the session's
    // lifetime, so no null-check is needed here. The old MainWindow
    // site guarded these pointers because they used to be nullable
    // during teardown; that is no longer possible now that both are
    // reaped by `~LogSession()`'s explicit sweep. Sibling
    // `MirrorSortToConfiguration` follows the same contract.
    mModel->ConfigurationManager().SetAnchors(mAnchors->Entries());
}

void LogSession::SetMode(Mode mode)
{
    if (mMode == mode)
    {
        return;
    }
    // The old `MainWindow` behaviour: only the transition *into*
    // `Idle` latches the previous mode into `LastTerminalMode()`.
    // Every other transition leaves the mirror untouched so it
    // continues to reflect the previous live run.
    if (mode == Mode::Idle && mMode != Mode::Idle)
    {
        mLastTerminalMode = mMode;
    }
    mMode = mode;
    emit presentationChanged();
}

void LogSession::ResetMode()
{
    const bool changed = (mMode != Mode::Idle) || (mLastTerminalMode != Mode::Idle);
    mMode = Mode::Idle;
    mLastTerminalMode = Mode::Idle;
    if (changed)
    {
        emit presentationChanged();
    }
}

void LogSession::SetSessionSwitchInProgress(bool inProgress) noexcept
{
    mSessionSwitchInProgress = inProgress;
}

void LogSession::SetStreamingLineCount(qsizetype count) noexcept
{
    // `rowCount` on the snapshot is projected from the owned model
    // directly (`mModel->rowCount()`), so this scalar is a status-
    // bar mirror rather than a snapshot input. No presentation
    // signal here; the model's own `rowsInserted` is the source of
    // truth for row-count consumers.
    mStreamingLineCount = count;
}

void LogSession::SetStreamingErrorCount(qsizetype count)
{
    if (mStreamingErrorCount == count)
    {
        return;
    }
    mStreamingErrorCount = count;
    emit presentationChanged();
}

void LogSession::SetStreamingErrorsCut(std::size_t cut)
{
    if (mStreamingErrorsCut == cut)
    {
        return;
    }
    mStreamingErrorsCut = cut;
    emit presentationChanged();
}

void LogSession::SetFirstStreamingBatchSeen(bool seen)
{
    if (mFirstStreamingBatchSeen == seen)
    {
        return;
    }
    mFirstStreamingBatchSeen = seen;
    // Flips the `Parsing` operation bit (Static-loading-spinner
    // indicator) so the tab strip can clear its startup badge.
    emit presentationChanged();
}

void LogSession::SetSourceWaiting(bool waiting)
{
    if (mSourceWaiting == waiting)
    {
        return;
    }
    mSourceWaiting = waiting;
    // Flips the `SourceWaiting` op bit and (indirectly) the
    // `Ingesting` op bit for a live-tail session.
    emit presentationChanged();
}

void LogSession::TriggerRotationFlash()
{
    // Rising-edge fan: emit the boolean signal exactly once per
    // false -> true transition. Successive calls inside the same
    // active window refresh the deadline without re-emitting (the
    // shell's UpdateStreamingStatus is idempotent, but the extra
    // work / cache pressure are avoidable).
    const bool wasActive = mRotationFlashActive;
    mRotationFlashActive = true;
    if (!wasActive)
    {
        emit rotationFlashChanged(true);
    }
    // `QPointer<LogSession>` origin: if the session is torn down
    // before the singleShot fires, `origin.isNull()` short-circuits
    // safely. Binding to `this` is also sufficient (the receiver
    // check auto-drops the callback on destruction) but the
    // explicit QPointer is defensive against future refactors that
    // might route the callback through a different receiver.
    const QPointer<LogSession> origin(this);
    QTimer::singleShot(ROTATION_FLASH_DURATION_MS, this, [origin]() {
        if (origin.isNull())
        {
            return;
        }
        auto *self = origin.data();
        if (!self->mRotationFlashActive)
        {
            return;
        }
        self->mRotationFlashActive = false;
        emit self->rotationFlashChanged(false);
    });
}

void LogSession::SetStreamingFileName(QString fileName)
{
    if (mStreamingFileName == fileName)
    {
        return;
    }
    mStreamingFileName = std::move(fileName);
    // `shortLabel` / `tooltip` / `sourceLabel` on the snapshot
    // are all derived from this string; a change flips all three.
    emit presentationChanged();
}

void LogSession::ClearStreamingFileName()
{
    if (mStreamingFileName.isEmpty())
    {
        return;
    }
    mStreamingFileName.clear();
    emit presentationChanged();
}

void LogSession::ResetStreamingCountersAndFileName()
{
    // Coalesce every field-level change into a single
    // `presentationChanged` at the end so a session-switch does
    // not fan out six emits.
    const bool changed = !mStreamingFileName.isEmpty() || mStreamingLineCount != 0 || mStreamingErrorCount != 0 ||
                         mStreamingErrorsCut != 0 || mFirstStreamingBatchSeen || mSourceWaiting;
    mStreamingFileName.clear();
    mStreamingLineCount = 0;
    mStreamingErrorCount = 0;
    mStreamingErrorsCut = 0;
    mFirstStreamingBatchSeen = false;
    mSourceWaiting = false;
    if (changed)
    {
        emit presentationChanged();
    }
}

void LogSession::ResetStreamingProgress()
{
    // Per-file start pattern: caller invokes this before every
    // `BeginStreaming`. Historically this was signal-free on the
    // theory that the interim "Parsing" bit is transient and the
    // next batch would re-fan the signal via
    // `SetFirstStreamingBatchSeen(true)`. That assumption breaks
    // for slow-source scenarios (compressed source that has not
    // yet handed the first batch, network stream in a waiting
    // state, ...) where the tab strip would keep its "loaded"
    // affordance instead of the "loading" spinner between reset
    // and first batch.
    //
    // Coalesce every field update into a single fan when any
    // tracked field actually transitioned; on an already-cleared
    // session (typical after the first `BeginStreaming`) skip the
    // emit entirely so a rapid open/close/open sequence does not
    // fan spurious signals into subscribers.
    const bool changed = mStreamingLineCount != 0 || mStreamingErrorCount != 0 || mFirstStreamingBatchSeen;
    mStreamingLineCount = 0;
    mStreamingErrorCount = 0;
    mFirstStreamingBatchSeen = false;
    if (changed)
    {
        emit presentationChanged();
    }
}

void LogSession::SetCurrentSource(std::optional<loglib::LogConfiguration::Source> source)
{
    // `loglib::LogConfiguration::Source` does not (currently)
    // define `operator==`, so we cannot cheaply compare the
    // optional payload. Field-by-field compare is doable but
    // brittle across future additions to `Source`; `SetCurrentSource`
    // is called from open / restore flows (not hot paths), so
    // emitting unconditionally is the pragmatic call. Consumers
    // that need a "real change only" filter should install their
    // own snapshot-diff on `presentationChanged`.
    const bool hadValue = mCurrentSource.has_value();
    mCurrentSource = std::move(source);
    const bool hasValue = mCurrentSource.has_value();
    if (hadValue || hasValue)
    {
        // `mode`, `dirty.restorableInPlace`, and
        // `dirty.ephemeralUnreproducible` all read the descriptor;
        // a change here can flip every one of them.
        emit presentationChanged();
    }
}

void LogSession::ResetCurrentSource()
{
    if (!mCurrentSource.has_value())
    {
        return;
    }
    mCurrentSource.reset();
    emit presentationChanged();
}

void LogSession::NotifyPresentationChanged()
{
    // Public trigger for callers that edited state through a raw
    // mutable accessor (`MutableCurrentSource()`, mutable atomics,
    // mutable stop sources, ...) and need the presentation
    // subscribers to refresh. Fires the signal unconditionally --
    // this is an escape hatch, not a diff-guarded setter, so it
    // never suppresses the emit. See the `MutableCurrentSource()`
    // docstring for the "prefer a setter or `MutateCurrentSource`"
    // guidance.
    emit presentationChanged();
}

void LogSession::SetPendingOpenFiles(QStringList files)
{
    mPendingOpenFiles = std::move(files);
}

void LogSession::ClearPendingOpenFiles()
{
    mPendingOpenFiles.clear();
}

void LogSession::ClearPendingOpenErrors() noexcept
{
    mPendingOpenErrors.clear();
}

void LogSession::ClearPendingDecompressionErrors() noexcept
{
    mPendingDecompressionErrors.clear();
}

void LogSession::ClearPendingOpenQueues() noexcept
{
    mPendingOpenFiles.clear();
    mPendingOpenErrors.clear();
    mPendingDecompressionErrors.clear();
}

void LogSession::SetPendingLiveTailPromotion(QString primary, std::size_t retention)
{
    mPendingLiveTailPrimary = std::move(primary);
    mPendingLiveTailRetention = retention;
}

void LogSession::ClearPendingLiveTailPromotion() noexcept
{
    mPendingLiveTailPrimary.clear();
    mPendingLiveTailRetention = 0;
}

std::pair<QString, std::size_t> LogSession::TakePendingLiveTailPromotion() noexcept
{
    std::pair<QString, std::size_t> promoted{std::move(mPendingLiveTailPrimary), mPendingLiveTailRetention};
    mPendingLiveTailPrimary.clear();
    mPendingLiveTailRetention = 0;
    return promoted;
}

void LogSession::SetDisableRotationHistoryOverride(bool disable) noexcept
{
    mDisableRotationHistoryOverride = disable;
}

void LogSession::SetLastRotationExpansion(QStringList originalInputs, bool wasLiveTail)
{
    mLastRotationExpansionOriginalInputs = std::move(originalInputs);
    mLastRotationExpansionWasLiveTail = wasLiveTail;
}

void LogSession::ClearRotationExpansionUndoState() noexcept
{
    mLastRotationExpansionOriginalInputs.clear();
    mLastRotationExpansionWasLiveTail = false;
}

void LogSession::SetAutoSaveUuid(QString uuid)
{
    mAutoSaveUuid = std::move(uuid);
}

void LogSession::SetAutoSaveUuidPublished(bool published) noexcept
{
    mAutoSaveUuidPublished = published;
}

void LogSession::ClearAutoSaveUuid() noexcept
{
    mAutoSaveUuid.clear();
    mAutoSaveUuidPublished = false;
}

void LogSession::DetachAutoSaveUuid()
{
    if (mAutoSaveUuid.isEmpty())
    {
        return;
    }
    // Skip the cross-process Remove when we never published; saves
    // a lock acquisition on the common closeEvent path.
    if (mAutoSaveUuidPublished)
    {
        SessionHistoryManager::RemoveOpenWindowUuid(mAutoSaveUuid);
    }
    mAutoSaveUuid.clear();
    mAutoSaveUuidPublished = false;
}

bool LogSession::ShouldAutoSaveAfterStreaming(Mode justFinishedMode) const noexcept
{
    if (mHistoryManager == nullptr)
    {
        return false;
    }
    if (!loglib::HasLocators(mCurrentSource))
    {
        // No source -> can't be reopened from Recent Sessions.
        return false;
    }
    // `HasLocators` already gated `has_value`; clang-tidy's optional
    // analyser cannot trace through the helper.
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    const auto &source = *mCurrentSource;
    if (source.kind != loglib::LogConfiguration::Source::Kind::File)
    {
        // Network streams: locator is a producer URI, not a path.
        return false;
    }
    if (justFinishedMode == Mode::LiveTail)
    {
        // Live-tail looks like a static File source on disk but
        // binds a tailing producer. Reopening would silently
        // downgrade the user to a one-shot static load.
        return false;
    }
    return true;
}

bool LogSession::ShouldAutoDetectRotationHistory() const
{
    if (mDisableRotationHistoryOverride)
    {
        return false;
    }
    const QSettings settings;
    return settings.value(QStringLiteral("ui/autoDetectRotatedHistory"), true).toBool();
}

bool LogSession::EffectiveAutoDetectRotationHistory() const
{
    if (!ShouldAutoDetectRotationHistory())
    {
        return false;
    }
    return !mCurrentSource.has_value() || mCurrentSource->followRotationSiblings;
}

QString LogSession::RestorableSessionUuid() const noexcept
{
    // Worth fan-restoring iff (a) a uuid is pinned, and (b) the
    // session is round-trippable. Mirrors `ShouldAutoSaveSession`
    // gates, with one difference: a pinned-uuid window with no
    // source (configuration-only restore) is still restorable
    // because the user explicitly clicked that recents entry.
    if (mAutoSaveUuid.isEmpty())
    {
        return {};
    }
    // Review finding #6: distinguish "no source at all" (valid
    // columns-only restore) from "descriptor with empty locators"
    // (invalid intermediate state, e.g. a partially-cancelled open).
    // The prior `!HasLocators` short-circuit collapsed both into the
    // columns-only branch and would have tried to fan-restore an
    // invalid File descriptor, causing the loader to silently no-op
    // or error out.
    if (!mCurrentSource.has_value())
    {
        // Pinned uuid + no source = columns-only restore.
        return mAutoSaveUuid;
    }
    const auto &source = *mCurrentSource;
    if (source.kind != loglib::LogConfiguration::Source::Kind::File)
    {
        // Stream sources cannot be re-bound from a saved locator.
        return {};
    }
    if (source.locators.empty())
    {
        // File descriptor with no locators is a partially-torn-down
        // state, not a "columns-only" open; drop the uuid so the
        // shell falls back to the empty-window path rather than
        // asking the loader to re-open nothing.
        return {};
    }
    return mAutoSaveUuid;
}

void LogSession::SetApplyEmbeddedBundleConfigForPath(QString bundlePath)
{
    // The gate is boolean-valued from the snapshot's perspective
    // (`Bundle` mode fires when the path is non-empty); an
    // empty-to-empty or non-empty-to-different-non-empty write
    // never crosses that boundary, so only a "was set / is set"
    // transition should fan a `presentationChanged` out.
    const bool wasSet = !mApplyEmbeddedBundleConfigForPath.isEmpty();
    mApplyEmbeddedBundleConfigForPath = std::move(bundlePath);
    const bool isSet = !mApplyEmbeddedBundleConfigForPath.isEmpty();
    if (wasSet != isSet)
    {
        emit presentationChanged();
    }
}

void LogSession::ClearApplyEmbeddedBundleConfig()
{
    if (mApplyEmbeddedBundleConfigForPath.isEmpty())
    {
        return;
    }
    mApplyEmbeddedBundleConfigForPath.clear();
    emit presentationChanged();
}

void LogSession::SetDecompressionInFlight(bool inFlight)
{
    if (mDecompressionInFlight == inFlight)
    {
        return;
    }
    // Bump the generation on the rising edge so a poll timer that
    // captured the previous generation can detect completion +
    // silent rearm on the next-queued file (review finding #4).
    // The wraparound branch is defensive: 2^64 begins would need
    // ~584 years at 1 GHz -- but wrapping past zero would collide
    // with a not-yet-armed timer, so skip zero on wrap.
    if (inFlight)
    {
        ++mDecompressionGeneration;
        if (mDecompressionGeneration == 0)
        {
            mDecompressionGeneration = 1;
        }
    }
    mDecompressionInFlight = inFlight;
    // Flips the `Compressed` source-mode projection (under Static),
    // the `Decompressing` op bit, `mutationsAllowed`, and
    // `confirmBeforeClose` on the snapshot.
    emit presentationChanged();
}

LogSession::DecompressionWatcher *LogSession::EnsureDecompressionWatcher()
{
    // Review finding #13: keep the lazy allocation policy inside
    // `LogSession` so the public API cannot silently leak a live
    // watcher. Parent on `this` so tab / session teardown reaps
    // the watcher automatically; the shell uses
    // `Qt::UniqueConnection` at the callsite so repeated calls do
    // not accumulate duplicate `finished` slot invocations.
    if (mDecompressionWatcher == nullptr)
    {
        mDecompressionWatcher = new DecompressionWatcher(this);
    }
    return mDecompressionWatcher;
}

LogSession::ExportWatcher *LogSession::EnsureExportWatcher()
{
    if (mExportWatcher == nullptr)
    {
        mExportWatcher = new ExportWatcher(this);
    }
    return mExportWatcher;
}

void LogSession::SetDecompressionOriginalPath(QString path)
{
    mDecompressionOriginalPath = std::move(path);
}

void LogSession::SetDecompressionCodecName(QString codec)
{
    mDecompressionCodecName = std::move(codec);
}

void LogSession::SetDecompressionStartedAt(std::chrono::steady_clock::time_point startedAt) noexcept
{
    mDecompressionStartedAt = startedAt;
}

void LogSession::ClearDecompressionScratchPaths() noexcept
{
    mDecompressionOriginalPath.clear();
    mDecompressionCodecName.clear();
}

void LogSession::SetExportInFlight(bool inFlight)
{
    if (mExportInFlight == inFlight)
    {
        return;
    }
    if (inFlight)
    {
        ++mExportGeneration;
        if (mExportGeneration == 0)
        {
            mExportGeneration = 1;
        }
    }
    mExportInFlight = inFlight;
    // Flips the `Exporting` op bit, `mutationsAllowed`, and
    // `confirmBeforeClose` on the snapshot.
    emit presentationChanged();
}

void LogSession::SetExportIsBundle(bool isBundle) noexcept
{
    // Label selector only -- does not enter the snapshot's
    // source-mode / operations projection today. Kept `noexcept`
    // and signal-free so it can be flipped from hot paths.
    mExportIsBundle = isBundle;
}

void LogSession::SetExportDestinationPath(QString path)
{
    mExportDestinationPath = std::move(path);
}

void LogSession::SetExportFormatLabel(QString label)
{
    mExportFormatLabel = std::move(label);
}

void LogSession::SetExportStartedAt(std::chrono::steady_clock::time_point startedAt) noexcept
{
    mExportStartedAt = startedAt;
}

void LogSession::ClearExportScratchState() noexcept
{
    mExportDestinationPath.clear();
    mExportFormatLabel.clear();
}

int LogSession::FindFirstRowAtOrAfterTimestamp(int timeCol, std::int64_t targetMicros) const
{
    if (mModel == nullptr || mSortFilterProxyModel == nullptr || mRowOrderProxyModel == nullptr)
    {
        return -1;
    }
    const int sourceRowCount = mModel->rowCount();
    if (timeCol < 0 || sourceRowCount == 0)
    {
        return -1;
    }

    // Cell -> epoch micros. `nullopt` for rows whose timestamp
    // slot is missing / unpromoted; both searches below skip such
    // rows explicitly (treating them as `-inf` would break the
    // fast-path binary search's monotonicity invariant).
    const auto tsFor = [this, timeCol](int sourceRow) -> std::optional<std::int64_t> {
        return loglib::AsEpochMicroseconds(
            mModel->Table().GetValue(static_cast<std::size_t>(sourceRow), static_cast<std::size_t>(timeCol))
        );
    };

    // Source row -> visible through the outer proxy? Respects the
    // mid-proxy's newest-first reversal and the active row filter.
    const auto isVisible = [this](int sourceRow) -> bool {
        const QModelIndex sourceIdx = mModel->index(sourceRow, 0);
        const QModelIndex midIdx = mRowOrderProxyModel->mapFromSource(sourceIdx);
        if (!midIdx.isValid())
        {
            return false;
        }
        return mSortFilterProxyModel->mapFromSource(midIdx).isValid();
    };

    // Outer proxy row -> source row, or `-1` when the mapping is
    // transiently broken (proxy layout change in flight).
    const auto proxyRowToSource = [this](int proxyRow) -> int {
        const QModelIndex proxyIdx = mSortFilterProxyModel->index(proxyRow, 0);
        const QModelIndex midIdx = mSortFilterProxyModel->mapToSource(proxyIdx);
        if (!midIdx.isValid())
        {
            return -1;
        }
        const QModelIndex sourceIdx = mRowOrderProxyModel->mapToSource(midIdx);
        return sourceIdx.isValid() ? sourceIdx.row() : -1;
    };

    const int userSortColumn = mSortFilterProxyModel->SortColumn();
    const bool timestampsMonotonic = mModel->TimestampsAreMonotonic();

    if (userSortColumn < 0 && timestampsMonotonic)
    {
        // Fast path: binary-search source rows in place. Building
        // an `iota` index array would cost ~40 MiB on a 10 M-row
        // file, blowing the ROADMAP `< 100 ms` budget.
        //
        // Missing timestamps require an explicit skip -- there is
        // no `-inf` / `+inf` rule that preserves the monotonicity
        // `lower_bound` needs when gaps interleave valid rows. Each
        // probe walks forward from `mid` to the first valid ts.
        // Worst case (all rows missing) is O(N); typical case
        // (rare gaps) stays O(log N).
        //
        // Invariants:
        //   * `[0, lo)`  -- every row has "missing OR ts < target".
        //   * `[hi, sourceRowCount)` -- either empty or all-missing
        //     as observed by the `hi = mid` branch.
        // At `lo == hi`, the answer (if any) is the first valid
        // ts >= target in `[lo, sourceRowCount)`.
        int lo = 0;
        int hi = sourceRowCount;
        while (lo < hi)
        {
            const int mid = lo + ((hi - lo) / 2);
            int probe = mid;
            std::int64_t probeTsValue = 0;
            bool probeHasTs = false;
            for (; probe < hi; ++probe)
            {
                if (const auto probeTs = tsFor(probe); probeTs.has_value())
                {
                    probeTsValue = *probeTs;
                    probeHasTs = true;
                    break;
                }
            }
            if (!probeHasTs)
            {
                // `[mid, hi)` is all missing -- shrink to the left.
                hi = mid;
            }
            else if (probeTsValue < targetMicros)
            {
                lo = probe + 1;
            }
            else
            {
                hi = probe;
            }
        }

        if (lo >= sourceRowCount)
        {
            return -1;
        }

        // Happy path: `lo` itself is a valid, visible answer.
        // Skips the proxy walk below in the common no-filter case,
        // keeping the fast path O(log N) end-to-end.
        {
            const auto ts = tsFor(lo);
            if (ts.has_value() && *ts >= targetMicros && isVisible(lo))
            {
                return lo;
            }
        }

        // Otherwise walk the outer proxy (not the source) for the
        // smallest visible source row `>= lo` with a valid ts.
        // O(N_visible), not O(N_source) -- a heavy filter that
        // hides most of `[lo, sourceRowCount)` used to blow the
        // ROADMAP budget on huge files.
        //
        // Correctness: the binary-search invariant guarantees
        // every source row `>= lo` is either missing or has
        // ts >= target, so any visible one with a valid ts
        // qualifies; the smallest such row is earliest
        // chronologically (by monotonicity).
        int best = -1;
        const int proxyRowCount = mSortFilterProxyModel->rowCount();
        for (int proxyRow = 0; proxyRow < proxyRowCount; ++proxyRow)
        {
            const int sourceRow = proxyRowToSource(proxyRow);
            if (sourceRow < lo)
            {
                continue;
            }
            const auto ts = tsFor(sourceRow);
            if (!ts.has_value() || *ts < targetMicros)
            {
                continue;
            }
            if (best < 0 || sourceRow < best)
            {
                best = sourceRow;
            }
        }
        return best;
    }

    if (userSortColumn < 0)
    {
        // Non-monotonic source: the binary search above would
        // silently return a wrong row. Walk the outer proxy and
        // pick the visible row with the smallest ts satisfying
        // `>= target` -- the chronologically earliest match.
        int best = -1;
        std::int64_t bestTs = std::numeric_limits<std::int64_t>::max();
        const int proxyRowCount = mSortFilterProxyModel->rowCount();
        for (int proxyRow = 0; proxyRow < proxyRowCount; ++proxyRow)
        {
            const int sourceRow = proxyRowToSource(proxyRow);
            if (sourceRow < 0)
            {
                continue;
            }
            const auto ts = tsFor(sourceRow);
            if (!ts.has_value() || *ts < targetMicros)
            {
                continue;
            }
            if (*ts < bestTs)
            {
                bestTs = *ts;
                best = sourceRow;
            }
        }
        return best;
    }

    // User sort active: linear scan in display order so "first"
    // matches the user's chosen sort (may not be earliest in
    // wall-clock time).
    const int proxyRowCount = mSortFilterProxyModel->rowCount();
    for (int proxyRow = 0; proxyRow < proxyRowCount; ++proxyRow)
    {
        const int sourceRow = proxyRowToSource(proxyRow);
        if (sourceRow < 0)
        {
            continue;
        }
        const auto ts = tsFor(sourceRow);
        if (ts.has_value() && *ts >= targetMicros)
        {
            return sourceRow;
        }
    }
    return -1;
}

int LogSession::FindColumnIndexByKeys(const std::vector<std::string> &keys) const
{
    if (mModel == nullptr || keys.empty())
    {
        return -1;
    }
    const auto &columns = mModel->Configuration().columns;
    for (std::size_t i = 0; i < columns.size(); ++i)
    {
        if (columns[i].keys == keys)
        {
            return static_cast<int>(i);
        }
    }
    return -1;
}
