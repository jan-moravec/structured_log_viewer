#include "log_session.hpp"

#include "anchor_manager.hpp"
#include "highlight_rule_set.hpp"
#include "log_filter_model.hpp"
#include "log_model.hpp"
#include "qt_streaming_log_sink.hpp"
#include "row_order_proxy_model.hpp"

#include <Qt>

#include "regex_template_registry.hpp"
#include "session_history_manager.hpp"
#include "theme_control.hpp"

#include <loglib/log_configuration.hpp>
#include <loglib/log_value.hpp>

#include <QAbstractItemModel>
#include <QFileInfo>
#include <QFuture>
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
      // Dependencies must exist before the model and proxy chain.
      mAnchors(new AnchorManager(this)),
      mHighlights(new HighlightRuleSet(this)),
      mModel(new LogModel(this, theme, mAnchors, mHighlights)),
      mRowOrderProxyModel(new RowOrderProxyModel(this)),
      mSortFilterProxyModel(new LogFilterModel(this))
{
    mRowOrderProxyModel->setSourceModel(mModel);
    mSortFilterProxyModel->setSourceModel(mRowOrderProxyModel);
    mSortFilterProxyModel->SetLogModel(mModel);

    // Keep the highlight cache synchronized with this session's model.
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
    // New columns can resolve previously unmatched highlight keys.
    connect(mModel, &QAbstractItemModel::columnsInserted, this, [this](const QModelIndex &, int, int) {
        mHighlights->RebindColumns(mModel->Configuration().columns, &mModel->Table());
    });
}

LogSession::~LogSession()
{
    // Qt requires each watched future to finish before its watcher is destroyed.
    DrainDecompressionWatcher();
    if (mExportWatcher != nullptr)
    {
        // Teardown cannot report worker exceptions to the UI.
        try
        {
            mExportWatcher->waitForFinished();
        }
        catch (...) // NOLINT(bugprone-empty-catch)
        {
            // Preserve the destructor's no-throw contract.
        }
    }

    // Destroy dependents first so model back-pointers remain valid.
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

std::uint32_t LogSession::PreCheckClose() const
{
    std::uint32_t mask = 0;
    if (HasUnsavedChanges())
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

SessionCloseDecision LogSession::CloseDecision() const noexcept
{
    if (!HasUnsavedChanges())
    {
        return SessionCloseDecision::Silent;
    }
    if (HasDirtyHighlightEditorDraft())
    {
        return SessionCloseDecision::Prompt;
    }
    if (ShouldAutoSaveAfterStreaming(EffectiveTerminalMode()))
    {
        return SessionCloseDecision::Autosave;
    }
    return SessionCloseDecision::Prompt;
}

SessionPresentationSnapshot LogSession::PresentationSnapshot() const
{
    SessionPresentationSnapshot snapshot;

    // Project overlapping source properties into one presentation mode.
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
    const bool isNetworkOrStdin =
        mCurrentSource.has_value() && (mCurrentSource->kind == loglib::LogConfiguration::Source::Kind::NetworkStream ||
                                       mCurrentSource->kind == loglib::LogConfiguration::Source::Kind::Stdin);
    if (mSourceWaiting)
    {
        if (isNetworkOrStdin)
        {
            ops |= static_cast<std::uint32_t>(SessionOperationState::Disconnected);
        }
        else
        {
            ops |= static_cast<std::uint32_t>(SessionOperationState::SourceWaiting);
        }
    }
    if (mMode == Mode::Static && !mFirstStreamingBatchSeen)
    {
        // Live-tail startup is represented by Ingesting, not Parsing.
        ops |= static_cast<std::uint32_t>(SessionOperationState::Parsing);
    }
    if (mMode == Mode::LiveTail && !mSourceWaiting)
    {
        ops |= static_cast<std::uint32_t>(SessionOperationState::Ingesting);
    }
    if (mModel != nullptr)
    {
        QtStreamingLogSink *sink = mModel->Sink();
        if (sink != nullptr && sink->IsPaused())
        {
            ops |= static_cast<std::uint32_t>(SessionOperationState::Paused);
        }
    }
    if (!mPendingPresentation.failureTitle.isEmpty() || !mPendingDecompressionErrors.empty())
    {
        ops |= static_cast<std::uint32_t>(SessionOperationState::Failed);
    }
    snapshot.operations = ops;

    const auto hasOp = [ops](SessionOperationState bit) { return (ops & static_cast<std::uint32_t>(bit)) != 0; };
    if (hasOp(SessionOperationState::Failed))
    {
        snapshot.statusSummary = tr("Failed");
    }
    else if (hasOp(SessionOperationState::Disconnected))
    {
        snapshot.statusSummary = tr("Disconnected");
    }
    else if (hasOp(SessionOperationState::Paused))
    {
        snapshot.statusSummary = tr("Paused");
    }
    else if (hasOp(SessionOperationState::Ingesting))
    {
        snapshot.statusSummary = tr("Ingesting");
    }
    else if (hasOp(SessionOperationState::Decompressing))
    {
        snapshot.statusSummary = tr("Decompressing");
    }
    else if (hasOp(SessionOperationState::Exporting))
    {
        snapshot.statusSummary = tr("Exporting");
    }
    else if (hasOp(SessionOperationState::Parsing))
    {
        snapshot.statusSummary = tr("Parsing");
    }
    else if (hasOp(SessionOperationState::SourceWaiting))
    {
        snapshot.statusSummary = tr("Waiting");
    }
    else
    {
        snapshot.statusSummary = tr("Idle");
    }

    // ------------------------------------------------------------------
    // Dirty state. The `filtersDirty` and `restorableInPlace` /
    // `ephemeralUnreproducible` flags follow the same taxonomy the
    // shell already uses in ``ShouldAutoSaveAfterStreaming`` and
    // ``RestorableSessionUuid``.
    // ------------------------------------------------------------------
    snapshot.dirty.filtersDirty = HasUnsavedChanges();
    if (mCurrentSource.has_value())
    {
        // Restorability requires a concrete file locator and non-tail mode.
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

    // Keep full source text out of the compact tab label. File sources
    // use the same automatic name as Recent Sessions (`basename` or
    // `basename + N more`). Streaming display names cover network /
    // stdin producers. A workspace-restored placeholder can keep a
    // captured fallback name.
    if (mCurrentSource.has_value() && !mCurrentSource->locators.empty() &&
        mCurrentSource->kind == loglib::LogConfiguration::Source::Kind::File)
    {
        loglib::LogConfiguration named;
        named.source = mCurrentSource;
        snapshot.shortLabel = SessionHistoryManager::BuildLabel(named);
        snapshot.tooltip = QString::fromStdString(mCurrentSource->locators.front());
        snapshot.sourceLabel = snapshot.tooltip;
        if (snapshot.tooltip.isEmpty() && !mStreamingFileName.isEmpty())
        {
            snapshot.tooltip = mStreamingFileName;
            snapshot.sourceLabel = mStreamingFileName;
        }
    }
    else if (!mStreamingFileName.isEmpty())
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
    else if (mCurrentSource.has_value() && !mCurrentSource->locators.empty())
    {
        loglib::LogConfiguration named;
        named.source = mCurrentSource;
        snapshot.shortLabel = SessionHistoryManager::BuildLabel(named);
        snapshot.tooltip = snapshot.shortLabel;
        snapshot.sourceLabel = snapshot.shortLabel;
    }
    else if (!mFallbackTabLabel.isEmpty())
    {
        snapshot.shortLabel = mFallbackTabLabel;
        snapshot.tooltip = mFallbackTabLabel;
        snapshot.sourceLabel = mFallbackTabLabel;
    }

    if (!mCustomTabLabel.isEmpty())
    {
        snapshot.shortLabel = mCustomTabLabel;
        if (snapshot.tooltip.isEmpty())
        {
            snapshot.tooltip = mCustomTabLabel;
        }
        if (snapshot.sourceLabel.isEmpty())
        {
            snapshot.sourceLabel = mCustomTabLabel;
        }
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

    // In-flight workers prevent mutation and require close handling.
    snapshot.mutationsAllowed = !mDecompressionInFlight && !mExportInFlight;
    snapshot.confirmBeforeClose = HasUnsavedChanges() || mDecompressionInFlight || mExportInFlight;

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

void LogSession::SetHighlightEditorDraft(std::optional<HighlightRulesEditorDraft> draft)
{
    const bool wasUnsaved = HasUnsavedChanges();
    mHighlightEditorDraft = std::move(draft);
    if (wasUnsaved == HasUnsavedChanges())
    {
        return;
    }
    emit filtersDirtyChanged(HasUnsavedChanges());
    emit presentationChanged();
}

void LogSession::ClearHighlightEditorDraft()
{
    SetHighlightEditorDraft(std::nullopt);
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
    const bool wasUnsaved = HasUnsavedChanges();
    mFiltersDirty = false;
    if (wasUnsaved != HasUnsavedChanges())
    {
        emit filtersDirtyChanged(false);
        emit presentationChanged();
    }
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
    // Preserve simple leaves before advanced expression subtrees.
    //   1. Simple-mode leaves become the leading `And` children,
    //      preserving `SimpleLeafOrder()`.
    //   2. Advanced-mode `Or`/`Not` subtrees carried on the
    //      existing expression must survive a simple-mode edit —
    //      an existing root `And` contributes its non-`Leaf`
    //      children; an existing root `Or`/`Not` is preserved
    //      wholesale as a single child.
    //   3. A bare `Leaf` root
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
    // Both objects are owned by the session and remain valid here.
    mModel->ConfigurationManager().SetAnchors(mAnchors->Entries());
}

void LogSession::SetMode(Mode mode)
{
    if (mMode == mode)
    {
        return;
    }
    // Preserve the last active mode only when entering Idle.
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
    // Invalidate an earlier timer when the flash window is refreshed.
    const std::uint64_t generation = ++mRotationFlashGeneration;
    // Guard the callback against session teardown.
    const QPointer<LogSession> origin(this);
    QTimer::singleShot(ROTATION_FLASH_DURATION_MS, this, [origin, generation]() {
        if (origin.isNull())
        {
            return;
        }
        auto *self = origin.data();
        if (!self->mRotationFlashActive)
        {
            return;
        }
        // A later `TriggerRotationFlash()` extended the window;
        // let ITS lambda be the one that eventually clears the
        // flash so the deadline actually reflects the last call.
        if (self->mRotationFlashGeneration != generation)
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

void LogSession::SetFallbackTabLabel(QString label)
{
    if (mFallbackTabLabel == label)
    {
        return;
    }
    mFallbackTabLabel = std::move(label);
    emit presentationChanged();
}

void LogSession::SetCustomTabLabel(QString label)
{
    if (label.size() > MAX_TAB_LABEL_LENGTH)
    {
        label.truncate(MAX_TAB_LABEL_LENGTH);
    }
    if (mCustomTabLabel == label)
    {
        return;
    }
    mCustomTabLabel = std::move(label);
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
    // Coalesce per-file progress resets into one presentation update.
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
    // Source has no equality operator, so value-bearing writes always fan out.
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
    mFallbackTabLabel.clear();
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

void LogSession::ResetParseErrorLog() noexcept
{
    // Reset the first-batch latch together with the stored errors.
    mParseErrorLog.batches.clear();
    mParseErrorLog.droppedCount = 0;
    mParseErrorLog.hasSeenFirstBatch = false;
}

void LogSession::ResetFindQuery() noexcept
{
    mFindQuery.query.clear();
    mFindQuery.wildcards = false;
    mFindQuery.regex = false;
}

void LogSession::ResetHistogramState() noexcept
{
    mHistogramState.bucketSizePinned = false;
    mHistogramState.bucketSize.reset();
}

void LogSession::ResetRecordDetailPin() noexcept
{
    // Clear the stable key so a later bind cannot restore a stale record.
    mRecordDetailPin.pinnedSourceRow = -1;
    mRecordDetailPin.everPinned = false;
    mRecordDetailPin.keyLocator.clear();
    mRecordDetailPin.keyLineId = 0;
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
        // Live-tail completions stay out of the close autosave path so
        // a dirty tail still prompts. Quit persistence uses
        // `CanPersistRestorableSnapshot()` and restores the file statically.
        return false;
    }
    return true;
}

bool LogSession::CanPersistRestorableSnapshot() const noexcept
{
    if (mHistoryManager == nullptr)
    {
        return false;
    }
    if (!mCurrentSource.has_value())
    {
        return !mAutoSaveUuid.isEmpty();
    }
    const auto &source = *mCurrentSource;
    if (source.kind != loglib::LogConfiguration::Source::Kind::File)
    {
        return false;
    }
    return !source.locators.empty();
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
    // No source permits a columns-only restore; an empty file descriptor does not.
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
    // Let pollers reject updates from a completed operation generation.
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
    // Session parentage ties watcher lifetime to the operation owner.
    if (mDecompressionWatcher == nullptr)
    {
        mDecompressionWatcher = new DecompressionWatcher(this);
    }
    return mDecompressionWatcher;
}

void LogSession::DrainDecompressionWatcher() noexcept
{
    if (mDecompressionWatcher == nullptr)
    {
        return;
    }
    try
    {
        mDecompressionWatcher->waitForFinished();
        if (mDecompressionWatcher->future().isValid())
        {
            (void)mDecompressionWatcher->result();
        }
    }
    catch (...) // NOLINT(bugprone-empty-catch)
    {
        // Cancellation and decode errors are consumed by the closer.
    }
    mDecompressionWatcher->setFuture(QFuture<DecompressionByteSourcePtr>{});
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
        // Avoid an index array that would cost about 40 MiB for 10 million rows.
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
        // Scan visible rows so heavy filtering does not scale with all source rows.
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
