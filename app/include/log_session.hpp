#pragma once

#include "log_session_commands.hpp"
#include "log_session_presentation.hpp"

#include <loglib/filter_expression.hpp>
#include <loglib/internal/decompressing_byte_source.hpp>
#include <loglib/log_configuration.hpp>
#include <loglib/stop_token.hpp>

#include <QAtomicInteger>
#include <QElapsedTimer>
#include <QFutureWatcher>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QtGlobal>

#include <memory>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

class AnchorManager;
class HighlightRuleSet;
class LogFilterModel;
class LogModel;
class RegexTemplateRegistry;
class RowOrderProxyModel;
class SessionHistoryManager;
class ThemeControl;

/**
 * @brief Owns the non-visual state and resources of one log session.
 *
 * The session owns its models, asynchronous-operation watchers, filters,
 * persistence identity, and presentation state. Service pointers passed to
 * the constructor are non-owning.
 */
class LogSession : public QObject, public LogSessionCommands
{
    Q_OBJECT

public:
    /**
     * @brief Defines the session's current streaming mode.
     */
    enum class Mode : int
    {
        Idle,
        Static,
        LiveTail,
    };

    /**
     * @brief Constructs a session and its model hierarchy.
     *
     * @param theme Non-owning theme service; may be null.
     * @param historyManager Non-owning persistence service; may be null.
     * @param regexTemplateRegistry Non-owning regex-template service; may be null.
     * @param parent Optional Qt owner.
     */
    explicit LogSession(
        ThemeControl *theme = nullptr,
        SessionHistoryManager *historyManager = nullptr,
        RegexTemplateRegistry *regexTemplateRegistry = nullptr,
        QObject *parent = nullptr
    );
    /**
     * @brief Waits for active watchers and destroys owned models safely.
     */
    ~LogSession() override;

    LogSession(const LogSession &) = delete;
    LogSession &operator=(const LogSession &) = delete;
    LogSession(LogSession &&) = delete;
    LogSession &operator=(LogSession &&) = delete;

    /**
     * @brief Handles a new-session request.
     *
     * This method is currently a no-op; the window shell performs the reset.
     */
    void RequestNewSession() override;

    /**
     * @brief Handles a file-open request.
     *
     * This method is currently a no-op; the window shell opens the files.
     *
     * @param files Files to open.
     * @param mode Whether to append to or replace the current session.
     */
    void RequestOpenFiles(const QStringList &files, OpenMode mode) override;

    /**
     * @brief Handles a live-tail request.
     *
     * This method is currently a no-op; the window shell starts the stream.
     *
     * @param filePath File to tail.
     */
    void RequestOpenLogStream(const QString &filePath) override;

    /**
     * @brief Handles an autosave request.
     *
     * This method is currently a no-op; the window shell owns persistence.
     *
     * @param publishOpenWindow Whether to publish the session for restoration.
     */
    void RequestAutoSaveSnapshot(bool publishOpenWindow) override;

    /**
     * @brief Reports conditions requiring shell-owned close handling.
     *
     * @return A bitwise combination of `SessionClosePreconditions`.
     */
    [[nodiscard]] std::uint32_t PreCheckClose() const override;

    /**
     * @brief Returns a successful close result without performing teardown.
     *
     * The window shell owns close orchestration through `PreCheckClose()`.
     *
     * @return `SessionCloseResult::Closed`.
     */
    [[nodiscard]] SessionCloseResult RequestClose() override;

    /**
     * @brief Classifies how the shell should close or replace this session.
     *
     * Clean sessions are `Silent`. Dirty sessions whose source can be
     * autosaved through `ShouldAutoSaveAfterStreaming()` are `Autosave`.
     * Every other dirty session is `Prompt`.
     *
     * @return The close decision for the current dirty state and source.
     */
    [[nodiscard]] SessionCloseDecision CloseDecision() const noexcept;

    /**
     * @brief Builds the current shell presentation state.
     *
     * @return A value snapshot of source, operation, label, count, and close state.
     */
    [[nodiscard]] SessionPresentationSnapshot PresentationSnapshot() const;

    /**
     * @brief Returns the non-owning theme service.
     *
     * @return The service, or null.
     */
    [[nodiscard]] ThemeControl *Theme() const noexcept;
    /**
     * @brief Returns the non-owning history service.
     *
     * @return The service, or null.
     */
    [[nodiscard]] SessionHistoryManager *HistoryManager() const noexcept;
    /**
     * @brief Returns the non-owning regex-template service.
     *
     * @return The service, or null.
     */
    [[nodiscard]] RegexTemplateRegistry *RegexTemplates() const noexcept;

    /**
     * @brief Returns the owned anchor manager.
     *
     * @return A non-null manager.
     */
    [[nodiscard]] AnchorManager *Anchors() const noexcept;
    /**
     * @brief Returns the owned highlight rules.
     *
     * @return A non-null rule set.
     */
    [[nodiscard]] HighlightRuleSet *Highlights() const noexcept;
    /**
     * @brief Returns the owned source model.
     *
     * @return A non-null model.
     */
    [[nodiscard]] LogModel *Model() const noexcept;
    /**
     * @brief Returns the owned row-order proxy.
     *
     * @return A non-null proxy.
     */
    [[nodiscard]] RowOrderProxyModel *RowOrderProxy() const noexcept;
    /**
     * @brief Returns the owned filter and sort proxy.
     *
     * @return A non-null proxy.
     */
    [[nodiscard]] LogFilterModel *FilterProxy() const noexcept;

    /**
     * @brief Tests whether filter state has unsaved changes.
     *
     * @return The dirty marker.
     */
    [[nodiscard]] bool IsFiltersDirty() const noexcept
    {
        return mFiltersDirty;
    }

    /**
     * @brief Tests whether a configuration load is suppressing dirty transitions.
     *
     * @return `true` while configuration loading is active.
     */
    [[nodiscard]] bool IsLoadingConfiguration() const noexcept
    {
        return mLoadingConfiguration;
    }

    /**
     * @brief Marks filter state dirty unless a configuration load is active.
     *
     * Emits `filtersDirtyChanged(true)` and `presentationChanged()` only on
     * a false-to-true transition.
     */
    void MarkFiltersDirty();

    /**
     * @brief Clears the filter dirty marker.
     *
     * Emits `filtersDirtyChanged(false)` and `presentationChanged()` only on
     * a true-to-false transition.
     */
    void ClearFiltersDirty();

    /**
     * @brief Enables or disables dirty-state suppression during configuration loading.
     *
     * @param loading Whether configuration loading is active.
     */
    void SetLoadingConfiguration(bool loading) noexcept;

    /**
     * @brief Tests whether a loaded sort is waiting for streaming completion.
     *
     * @return The deferred-sort latch.
     */
    [[nodiscard]] bool HasPendingApplySortFromConfig() const noexcept
    {
        return mPendingApplySortFromConfig;
    }

    /**
     * @brief Sets the deferred-sort latch.
     *
     * @param pending Whether the loaded sort remains pending.
     */
    void SetPendingApplySortFromConfig(bool pending) noexcept;

    /**
     * @brief Returns simple-mode leaves keyed by filter UUID.
     *
     * @return The read-only leaf map.
     */
    [[nodiscard]] const std::unordered_map<std::string, loglib::LeafRule> &SimpleLeaves() const noexcept
    {
        return mSimpleLeaves;
    }

    /**
     * @brief Returns mutable simple-mode leaves.
     *
     * Callers must keep `SimpleLeafOrder()` synchronized.
     *
     * @return The mutable leaf map.
     */
    [[nodiscard]] std::unordered_map<std::string, loglib::LeafRule> &MutableSimpleLeaves() noexcept
    {
        return mSimpleLeaves;
    }

    /**
     * @brief Returns simple-mode leaf identifiers in display order.
     *
     * @return The read-only order vector.
     */
    [[nodiscard]] const std::vector<std::string> &SimpleLeafOrder() const noexcept
    {
        return mSimpleLeafOrder;
    }

    /**
     * @brief Returns mutable simple-mode leaf display order.
     *
     * Callers must keep `SimpleLeaves()` synchronized.
     *
     * @return The mutable order vector.
     */
    [[nodiscard]] std::vector<std::string> &MutableSimpleLeafOrder() noexcept
    {
        return mSimpleLeafOrder;
    }

    /**
     * @brief Clears simple-mode filter leaves and their display order.
     */
    void ResetSimpleFilterState() noexcept;

    /**
     * @brief Rebuilds the model expression from simple filter leaves.
     *
     * Simple leaves retain display order. Existing advanced `Or` and `Not`
     * subtrees are preserved, and missing ordered leaf identifiers are skipped.
     */
    void RebuildFilterExpressionFromSimpleLeaves();

    /**
     * @brief Mirrors the active proxy sort into the model configuration.
     *
     * A transient unsorted proxy does not overwrite a deferred loaded sort.
     */
    void MirrorSortToConfiguration();

    /**
     * @brief Mirrors current anchors into the model configuration.
     */
    void MirrorAnchorsToConfiguration();

    /**
     * @brief Returns the current session mode.
     *
     * @return The current mode.
     */
    [[nodiscard]] Mode SessionMode() const noexcept
    {
        return mMode;
    }

    /**
     * @brief Returns the active mode most recently followed by `Idle`.
     *
     * @return The last terminal mode.
     */
    [[nodiscard]] Mode LastTerminalMode() const noexcept
    {
        return mLastTerminalMode;
    }

    /**
     * @brief Tests whether the session mode is not `Idle`.
     *
     * @return `true` for static or live-tail mode.
     */
    [[nodiscard]] bool IsSessionActive() const noexcept
    {
        return mMode != Mode::Idle;
    }

    /**
     * @brief Tests whether the session is live tailing.
     *
     * @return `true` in `LiveTail` mode.
     */
    [[nodiscard]] bool IsLiveTailSession() const noexcept
    {
        return mMode == Mode::LiveTail;
    }

    /**
     * @brief Sets the current session mode.
     *
     * Entering `Idle` records the previous active mode. A real change emits
     * `presentationChanged()`.
     *
     * @param mode New session mode.
     */
    void SetMode(Mode mode);

    /**
     * @brief Resets both current and last terminal modes to `Idle`.
     *
     * Emits `presentationChanged()` once when either value changes.
     */
    void ResetMode();

    /**
     * @brief Tests whether enum-dependent filters are being rebuilt.
     *
     * @return The rebuild re-entrancy latch.
     */
    [[nodiscard]] bool IsApplyingEnumRebuild() const noexcept
    {
        return mApplyingEnumRebuild;
    }

    /**
     * @brief Sets the enum-filter rebuild re-entrancy latch.
     *
     * @param applying Whether a rebuild is active.
     */
    void SetApplyingEnumRebuild(bool applying) noexcept;

    /**
     * @brief Tests whether a destructive session switch is active.
     *
     * @return The session-switch guard.
     */
    [[nodiscard]] bool IsSessionSwitchInProgress() const noexcept
    {
        return mSessionSwitchInProgress;
    }

    /**
     * @brief Sets the destructive session-switch guard.
     *
     * @param inProgress Whether a session switch is active.
     */
    void SetSessionSwitchInProgress(bool inProgress) noexcept;

    /**
     * @brief Returns the retained line count for streaming status.
     *
     * @return The retained line count.
     */
    [[nodiscard]] qsizetype StreamingLineCount() const noexcept
    {
        return mStreamingLineCount;
    }

    /**
     * @brief Sets the retained line count used by streaming status.
     *
     * @param count New line count.
     */
    void SetStreamingLineCount(qsizetype count) noexcept;

    /**
     * @brief Returns the retained parse-error count.
     *
     * @return The parse-error count.
     */
    [[nodiscard]] qsizetype StreamingErrorCount() const noexcept
    {
        return mStreamingErrorCount;
    }

    /**
     * @brief Sets the retained parse-error count.
     *
     * Emits `presentationChanged()` on a real change.
     *
     * @param count New error count.
     */
    void SetStreamingErrorCount(qsizetype count);

    /**
     * @brief Returns the consumed-error high-water mark.
     *
     * @return The index after the last consumed streaming error.
     */
    [[nodiscard]] std::size_t StreamingErrorsCut() const noexcept
    {
        return mStreamingErrorsCut;
    }

    /**
     * @brief Sets the consumed-error high-water mark.
     *
     * Emits `presentationChanged()` on a real change.
     *
     * @param cut New high-water mark.
     */
    void SetStreamingErrorsCut(std::size_t cut);

    /**
     * @brief Tests whether the first non-empty streaming batch arrived.
     *
     * @return The first-batch latch.
     */
    [[nodiscard]] bool FirstStreamingBatchSeen() const noexcept
    {
        return mFirstStreamingBatchSeen;
    }

    /**
     * @brief Sets the first-batch latch.
     *
     * Emits `presentationChanged()` on a real change.
     *
     * @param seen Whether a non-empty batch has arrived.
     */
    void SetFirstStreamingBatchSeen(bool seen);

    /**
     * @brief Tests whether the current source is waiting for input.
     *
     * @return The source-waiting latch.
     */
    [[nodiscard]] bool IsSourceWaiting() const noexcept
    {
        return mSourceWaiting;
    }

    /**
     * @brief Sets the source-waiting latch.
     *
     * Emits `presentationChanged()` on a real change.
     *
     * @param waiting Whether the source is waiting for input.
     */
    void SetSourceWaiting(bool waiting);

    /**
     * @brief Tests whether the rotation status flash is active.
     *
     * @return `true` until the session-owned timeout expires.
     */
    [[nodiscard]] bool IsRotationFlashActive() const noexcept
    {
        return mRotationFlashActive;
    }

    /**
     * @brief Starts or refreshes the rotation status flash.
     *
     * The rising edge emits `rotationFlashChanged(true)`. Repeated calls
     * extend the deadline without repeating that rising-edge signal.
     */
    void TriggerRotationFlash();

    /** @brief Duration of the rotation status flash in milliseconds. */
    static constexpr int ROTATION_FLASH_DURATION_MS = 3000;

    /**
     * @brief Returns the current streaming display name.
     *
     * @return The display name.
     */
    [[nodiscard]] const QString &StreamingFileName() const noexcept
    {
        return mStreamingFileName;
    }

    /**
     * @brief Sets the current streaming display name.
     *
     * Emits `presentationChanged()` on a real change.
     *
     * @param fileName New display name.
     */
    void SetStreamingFileName(QString fileName);

    /**
     * @brief Clears the streaming display name.
     *
     * Emits `presentationChanged()` when the name was non-empty.
     */
    void ClearStreamingFileName();

    /**
     * @brief Returns the current source descriptor.
     *
     * @return The optional source descriptor.
     */
    [[nodiscard]] const std::optional<loglib::LogConfiguration::Source> &CurrentSource() const noexcept
    {
        return mCurrentSource;
    }

    /**
     * @brief Returns the mutable source descriptor.
     *
     * Direct mutation does not emit `presentationChanged()`.
     *
     * @return The mutable optional source descriptor.
     */
    [[nodiscard]] std::optional<loglib::LogConfiguration::Source> &MutableCurrentSource() noexcept
    {
        return mCurrentSource;
    }

    /**
     * @brief Replaces the current source descriptor.
     *
     * Value-bearing writes emit `presentationChanged()` because source
     * descriptors do not provide equality comparison.
     *
     * @param source New source descriptor.
     */
    void SetCurrentSource(std::optional<loglib::LogConfiguration::Source> source);

    /**
     * @brief Clears the current source descriptor.
     *
     * Emits `presentationChanged()` when a source was present.
     */
    void ResetCurrentSource();

    /**
     * @brief Mutates the source descriptor and emits a presentation update.
     *
     * The signal is emitted unconditionally after the callback returns.
     *
     * @param fn Callable receiving the mutable optional source descriptor.
     */
    template <typename Fn> void MutateCurrentSource(Fn &&fn)
    {
        std::forward<Fn>(fn)(mCurrentSource);
        emit presentationChanged();
    }

    /**
     * @brief Emits `presentationChanged()` unconditionally.
     */
    void NotifyPresentationChanged();

    /**
     * @brief Returns files waiting to be opened.
     *
     * @return The read-only FIFO file queue.
     */
    [[nodiscard]] const QStringList &PendingOpenFiles() const noexcept
    {
        return mPendingOpenFiles;
    }

    /**
     * @brief Returns the mutable pending-file queue.
     *
     * @return The FIFO file queue.
     */
    [[nodiscard]] QStringList &MutablePendingOpenFiles() noexcept
    {
        return mPendingOpenFiles;
    }

    /**
     * @brief Replaces the pending-file queue.
     *
     * @param files New FIFO file queue.
     */
    void SetPendingOpenFiles(QStringList files);

    /**
     * @brief Clears the pending-file queue.
     */
    void ClearPendingOpenFiles();

    /**
     * @brief Returns open and parse errors accumulated for the queue.
     *
     * @return The read-only error list.
     */
    [[nodiscard]] const std::vector<std::string> &PendingOpenErrors() const noexcept
    {
        return mPendingOpenErrors;
    }

    /**
     * @brief Returns mutable open and parse errors.
     *
     * @return The mutable error list.
     */
    [[nodiscard]] std::vector<std::string> &MutablePendingOpenErrors() noexcept
    {
        return mPendingOpenErrors;
    }

    /**
     * @brief Clears accumulated open and parse errors.
     */
    void ClearPendingOpenErrors() noexcept;

    /**
     * @brief Returns decompression errors accumulated for the queue.
     *
     * @return The read-only error list.
     */
    [[nodiscard]] const std::vector<std::string> &PendingDecompressionErrors() const noexcept
    {
        return mPendingDecompressionErrors;
    }

    /**
     * @brief Returns mutable decompression errors.
     *
     * @return The mutable error list.
     */
    [[nodiscard]] std::vector<std::string> &MutablePendingDecompressionErrors() noexcept
    {
        return mPendingDecompressionErrors;
    }

    /**
     * @brief Clears accumulated decompression errors.
     */
    void ClearPendingDecompressionErrors() noexcept;

    /**
     * @brief Clears pending files and both pending error lists.
     */
    void ClearPendingOpenQueues() noexcept;

    /**
     * @brief Returns stored parse-error presentation state.
     *
     * @return The read-only parse-error log.
     */
    [[nodiscard]] const SessionParseErrorLog &ParseErrorLog() const noexcept
    {
        return mParseErrorLog;
    }

    /**
     * @brief Returns mutable parse-error presentation state.
     *
     * @return The mutable parse-error log.
     */
    [[nodiscard]] SessionParseErrorLog &MutableParseErrorLog() noexcept
    {
        return mParseErrorLog;
    }

    /**
     * @brief Clears parse-error batches, dropped count, and first-batch state.
     */
    void ResetParseErrorLog() noexcept;

    /**
     * @brief Returns UI work queued for the next time this tab is selected.
     * @return The pending presentation payload.
     */
    [[nodiscard]] const SessionPendingPresentation &PendingPresentation() const noexcept
    {
        return mPendingPresentation;
    }

    /**
     * @brief Queues a status-bar message for the next time this tab is selected.
     * @param message Status text to show.
     * @param timeoutMs Display timeout in milliseconds.
     */
    void QueueStatusMessage(QString message, int timeoutMs) noexcept
    {
        mPendingPresentation.statusMessage = std::move(message);
        mPendingPresentation.statusTimeoutMs = timeoutMs;
    }

    /**
     * @brief Queues a failure dialog for the next time this tab is selected.
     * @param title Dialog title.
     * @param message Dialog body.
     */
    void QueueFailureNotice(QString title, QString message) noexcept
    {
        mPendingPresentation.failureTitle = std::move(title);
        mPendingPresentation.failureMessage = std::move(message);
    }

    /**
     * @brief Requests that the parse-errors dock raise when this tab is selected.
     */
    void QueueParseErrorsRaise() noexcept
    {
        mPendingPresentation.raiseParseErrors = true;
    }

    /**
     * @brief Takes and clears queued UI work for this session.
     * @return The pending payload, which may be empty.
     */
    [[nodiscard]] SessionPendingPresentation TakePendingPresentation() noexcept
    {
        SessionPendingPresentation taken = std::move(mPendingPresentation);
        mPendingPresentation = {};
        return taken;
    }

    /**
     * @brief Returns the stored find query.
     *
     * @return The read-only query state.
     */
    [[nodiscard]] const SessionFindQueryState &FindQuery() const noexcept
    {
        return mFindQuery;
    }

    /**
     * @brief Returns the mutable find query.
     *
     * @return The mutable query state.
     */
    [[nodiscard]] SessionFindQueryState &MutableFindQuery() noexcept
    {
        return mFindQuery;
    }

    /**
     * @brief Resets the stored query text and matcher toggles.
     */
    void ResetFindQuery() noexcept;

    /**
     * @brief Returns stored histogram presentation state.
     *
     * @return The read-only histogram state.
     */
    [[nodiscard]] const SessionHistogramState &HistogramState() const noexcept
    {
        return mHistogramState;
    }

    /**
     * @brief Returns mutable histogram presentation state.
     *
     * @return The mutable histogram state.
     */
    [[nodiscard]] SessionHistogramState &MutableHistogramState() noexcept
    {
        return mHistogramState;
    }

    /**
     * @brief Clears the pinned histogram bucket size.
     */
    void ResetHistogramState() noexcept;

    /**
     * @brief Returns the stored record-detail pin.
     *
     * @return The read-only pin state.
     */
    [[nodiscard]] const SessionRecordDetailPin &RecordDetailPin() const noexcept
    {
        return mRecordDetailPin;
    }

    /**
     * @brief Returns the mutable record-detail pin.
     *
     * @return The mutable pin state.
     */
    [[nodiscard]] SessionRecordDetailPin &MutableRecordDetailPin() noexcept
    {
        return mRecordDetailPin;
    }

    /**
     * @brief Clears the stored row, stable key, and pin-history latch.
     */
    void ResetRecordDetailPin() noexcept;

    /**
     * @brief Returns the stored anchor selection.
     *
     * @return The read-only selection state.
     */
    [[nodiscard]] const SessionAnchorsSelection &AnchorsSelection() const noexcept
    {
        return mAnchorsSelection;
    }

    /**
     * @brief Returns the mutable anchor selection.
     *
     * @return The mutable selection state.
     */
    [[nodiscard]] SessionAnchorsSelection &MutableAnchorsSelection() noexcept
    {
        return mAnchorsSelection;
    }

    /**
     * @brief Returns the file to tail after a historical prefix loads.
     *
     * @return The pending primary path.
     */
    [[nodiscard]] const QString &PendingLiveTailPrimary() const noexcept
    {
        return mPendingLiveTailPrimary;
    }

    /**
     * @brief Returns the retention cap for pending live-tail promotion.
     *
     * @return The pending retention cap.
     */
    [[nodiscard]] std::size_t PendingLiveTailRetention() const noexcept
    {
        return mPendingLiveTailRetention;
    }

    /**
     * @brief Tests whether live-tail promotion state is present.
     *
     * @return `true` when a primary path or retention cap is stored.
     */
    [[nodiscard]] bool HasPendingLiveTailPromotion() const noexcept
    {
        return !mPendingLiveTailPrimary.isEmpty() || mPendingLiveTailRetention != 0;
    }

    /**
     * @brief Stores live-tail promotion state.
     *
     * @param primary File to tail after the prefix completes.
     * @param retention Retention cap to apply.
     */
    void SetPendingLiveTailPromotion(QString primary, std::size_t retention);

    /**
     * @brief Clears live-tail promotion state.
     */
    void ClearPendingLiveTailPromotion() noexcept;

    /**
     * @brief Tests whether rotation-history detection is overridden off.
     *
     * @return The override state.
     */
    [[nodiscard]] bool DisableRotationHistoryOverride() const noexcept
    {
        return mDisableRotationHistoryOverride;
    }

    /**
     * @brief Sets the rotation-history override.
     *
     * @param disable Whether automatic rotation history is disabled.
     */
    void SetDisableRotationHistoryOverride(bool disable) noexcept;

    /**
     * @brief Returns inputs from the last rotation expansion.
     *
     * @return The original input list.
     */
    [[nodiscard]] const QStringList &LastRotationExpansionOriginalInputs() const noexcept
    {
        return mLastRotationExpansionOriginalInputs;
    }

    /**
     * @brief Tests whether the last rotation expansion began as live tail.
     *
     * @return The stored live-tail flag.
     */
    [[nodiscard]] bool LastRotationExpansionWasLiveTail() const noexcept
    {
        return mLastRotationExpansionWasLiveTail;
    }

    /**
     * @brief Stores undo state for a rotation expansion.
     *
     * @param originalInputs Inputs supplied before expansion.
     * @param wasLiveTail Whether the expansion began as live tail.
     */
    void SetLastRotationExpansion(QStringList originalInputs, bool wasLiveTail);

    /**
     * @brief Consumes and clears pending live-tail promotion state.
     *
     * @return The stored primary path and retention cap.
     */
    [[nodiscard]] std::pair<QString, std::size_t> TakePendingLiveTailPromotion() noexcept;

    /**
     * @brief Clears rotation-expansion undo state.
     */
    void ClearRotationExpansionUndoState() noexcept;

    /**
     * @brief Returns the persistence identity used for autosave.
     *
     * @return The UUID, or an empty string before identity assignment.
     */
    [[nodiscard]] const QString &AutoSaveUuid() const noexcept
    {
        return mAutoSaveUuid;
    }

    /**
     * @brief Tests whether the UUID is published for process restoration.
     *
     * @return The publication latch.
     */
    [[nodiscard]] bool IsAutoSaveUuidPublished() const noexcept
    {
        return mAutoSaveUuidPublished;
    }

    /**
     * @brief Sets the autosave persistence identity.
     *
     * @param uuid New persistence UUID.
     */
    void SetAutoSaveUuid(QString uuid);

    /**
     * @brief Sets whether the autosave UUID is published.
     *
     * @param published New publication state.
     */
    void SetAutoSaveUuidPublished(bool published) noexcept;

    /**
     * @brief Clears the autosave UUID and publication latch.
     */
    void ClearAutoSaveUuid() noexcept;

    /**
     * @brief Detaches the session from its published persistence identity.
     *
     * Removes a published UUID from the process-shared set, then clears the
     * local UUID and publication latch. An empty UUID is a no-op.
     */
    void DetachAutoSaveUuid();

    /**
     * @brief Returns the UUID suitable for process restoration.
     *
     * A UUID is returned for a source-less configuration or a file source with
     * locators. Stream sources and empty file descriptors are not restorable.
     *
     * @return The restorable UUID, or an empty string.
     */
    [[nodiscard]] QString RestorableSessionUuid() const noexcept;

    /**
     * @brief Tests whether a completed stream should be autosaved.
     *
     * Autosave requires a history service and a reproducible file source.
     * Live-tail completions are excluded.
     *
     * @param justFinishedMode Mode of the completed stream.
     * @return `true` when a snapshot should be persisted.
     */
    [[nodiscard]] bool ShouldAutoSaveAfterStreaming(Mode justFinishedMode) const noexcept;

    /**
     * @brief Tests the effective rotation-history setting before source policy.
     *
     * @return `true` when settings enable detection and no override disables it.
     */
    [[nodiscard]] bool ShouldAutoDetectRotationHistory() const;

    /**
     * @brief Tests whether rotation history should be followed for this source.
     *
     * @return `true` when global and source-specific policy allow it.
     */
    [[nodiscard]] bool EffectiveAutoDetectRotationHistory() const;

    /**
     * @brief Returns the monotonic live-tail elapsed timer.
     *
     * @return The session-owned timer.
     */
    [[nodiscard]] const QElapsedTimer &LiveTailElapsedTimer() const noexcept
    {
        return mLiveTailElapsedTimer;
    }

    /**
     * @brief Restarts the live-tail elapsed timer.
     */
    void StartLiveTailElapsedTimer() noexcept
    {
        mLiveTailElapsedTimer.start();
    }

    using DecompressionByteSourcePtr = std::shared_ptr<loglib::internal::DecompressingByteSource>;
    using DecompressionWatcher = QFutureWatcher<DecompressionByteSourcePtr>;
    using ExportWatcher = QFutureWatcher<void>;

    /**
     * @brief Returns the decompression watcher if allocated.
     *
     * @return The session-owned watcher, or null.
     */
    [[nodiscard]] DecompressionWatcher *DecompressionWatcherPtr() const noexcept
    {
        return mDecompressionWatcher;
    }

    /**
     * @brief Returns the session-owned decompression watcher, allocating it if needed.
     *
     * @return A non-null watcher parented to this session.
     */
    [[nodiscard]] DecompressionWatcher *EnsureDecompressionWatcher();

    /**
     * @brief Waits for the decompression worker and consumes its stored result.
     *
     * Cancellation stores `DecompressionCancelled` in the future. Retrieving
     * that result prevents `std::terminate` when the watcher is reset or
     * destroyed.
     */
    void DrainDecompressionWatcher() noexcept;

    /**
     * @brief Returns the export watcher if allocated.
     *
     * @return The session-owned watcher, or null.
     */
    [[nodiscard]] ExportWatcher *ExportWatcherPtr() const noexcept
    {
        return mExportWatcher;
    }

    /**
     * @brief Returns the session-owned export watcher, allocating it if needed.
     *
     * @return A non-null watcher parented to this session.
     */
    [[nodiscard]] ExportWatcher *EnsureExportWatcher();

    /**
     * @brief Finds a visible source row at or after a timestamp.
     *
     * Monotonic unsorted sources use binary search; other cases scan visible
     * rows in the ordering needed by the active proxies.
     *
     * @param timeCol Source-model timestamp column.
     * @param targetMicros Target epoch timestamp in microseconds.
     * @return The matching source-row index, or `-1` when none is available.
     */
    [[nodiscard]] int FindFirstRowAtOrAfterTimestamp(int timeCol, std::int64_t targetMicros) const;

    /**
     * @brief Finds a configured column whose keys exactly match.
     *
     * @param keys Column-key sequence to match.
     * @return The column index, or `-1` for empty keys or no match.
     */
    [[nodiscard]] int FindColumnIndexByKeys(const std::vector<std::string> &keys) const;

    /**
     * @brief Returns the bundle path authorized to apply embedded configuration.
     *
     * @return The authorized path, or an empty string when disabled.
     */
    [[nodiscard]] const QString &ApplyEmbeddedBundleConfigForPath() const noexcept
    {
        return mApplyEmbeddedBundleConfigForPath;
    }

    /**
     * @brief Tests whether embedded bundle configuration is authorized.
     *
     * @return `true` when an authorized bundle path is stored.
     */
    [[nodiscard]] bool ShouldApplyEmbeddedBundleConfig() const noexcept
    {
        return !mApplyEmbeddedBundleConfigForPath.isEmpty();
    }

    /**
     * @brief Sets the bundle path authorized to apply embedded configuration.
     *
     * Emits `presentationChanged()` only when authorization changes between
     * enabled and disabled.
     *
     * @param bundlePath Bundle path to authorize; empty disables the gate.
     */
    void SetApplyEmbeddedBundleConfigForPath(QString bundlePath);

    /**
     * @brief Disables embedded bundle configuration application.
     *
     * Emits `presentationChanged()` when authorization was enabled.
     */
    void ClearApplyEmbeddedBundleConfig();

    /**
     * @brief Tests whether decompression is active.
     *
     * @return The decompression in-flight latch.
     */
    [[nodiscard]] bool IsDecompressionInFlight() const noexcept
    {
        return mDecompressionInFlight;
    }

    /**
     * @brief Sets decompression activity.
     *
     * A false-to-true transition advances the generation. Real changes emit
     * `presentationChanged()`.
     *
     * @param inFlight Whether decompression is active.
     */
    void SetDecompressionInFlight(bool inFlight);

    /**
     * @brief Returns the current decompression operation generation.
     *
     * Pollers use this value to reject updates from an older operation.
     *
     * @return The monotonic generation counter.
     */
    [[nodiscard]] std::uint64_t DecompressionGeneration() const noexcept
    {
        return mDecompressionGeneration;
    }

    /**
     * @brief Returns the path being decompressed.
     *
     * @return The source path.
     */
    [[nodiscard]] const QString &DecompressionOriginalPath() const noexcept
    {
        return mDecompressionOriginalPath;
    }

    /**
     * @brief Sets the path being decompressed.
     *
     * @param path Source path.
     */
    void SetDecompressionOriginalPath(QString path);

    /**
     * @brief Returns the active decompression codec label.
     *
     * @return The codec label.
     */
    [[nodiscard]] const QString &DecompressionCodecName() const noexcept
    {
        return mDecompressionCodecName;
    }

    /**
     * @brief Sets the active decompression codec label.
     *
     * @param codec Codec label.
     */
    void SetDecompressionCodecName(QString codec);

    /**
     * @brief Returns the decompression start time.
     *
     * @return The monotonic start time.
     */
    [[nodiscard]] std::chrono::steady_clock::time_point DecompressionStartedAt() const noexcept
    {
        return mDecompressionStartedAt;
    }

    /**
     * @brief Sets the decompression start time.
     *
     * @param startedAt Monotonic start time.
     */
    void SetDecompressionStartedAt(std::chrono::steady_clock::time_point startedAt) noexcept;

    /**
     * @brief Clears the decompression source path and codec label.
     */
    void ClearDecompressionScratchPaths() noexcept;

    /**
     * @brief Returns the decompression cooperative-stop source.
     *
     * @return The read-only stop source.
     */
    [[nodiscard]] const loglib::StopSource &DecompressionStopSource() const noexcept
    {
        return mDecompressionStopSource;
    }

    /**
     * @brief Returns the mutable decompression cooperative-stop source.
     *
     * @return The mutable stop source.
     */
    [[nodiscard]] loglib::StopSource &MutableDecompressionStopSource() noexcept
    {
        return mDecompressionStopSource;
    }

    /**
     * @brief Returns the export cooperative-stop source.
     *
     * @return The read-only stop source.
     */
    [[nodiscard]] const loglib::StopSource &ExportStopSource() const noexcept
    {
        return mExportStopSource;
    }

    /**
     * @brief Returns the mutable export cooperative-stop source.
     *
     * @return The mutable stop source.
     */
    [[nodiscard]] loglib::StopSource &MutableExportStopSource() noexcept
    {
        return mExportStopSource;
    }

    /**
     * @brief Returns the decompressed-input byte counter.
     *
     * @return The read-only atomic counter.
     */
    [[nodiscard]] const QAtomicInteger<qint64> &DecompressionBytesIn() const noexcept
    {
        return mDecompressionBytesIn;
    }

    /**
     * @brief Returns the mutable decompressed-input byte counter.
     *
     * @return The mutable atomic counter.
     */
    [[nodiscard]] QAtomicInteger<qint64> &MutableDecompressionBytesIn() noexcept
    {
        return mDecompressionBytesIn;
    }

    /**
     * @brief Returns the total compressed-input byte counter.
     *
     * @return The read-only atomic counter.
     */
    [[nodiscard]] const QAtomicInteger<qint64> &DecompressionTotalBytesIn() const noexcept
    {
        return mDecompressionTotalBytesIn;
    }

    /**
     * @brief Returns the mutable total compressed-input byte counter.
     *
     * @return The mutable atomic counter.
     */
    [[nodiscard]] QAtomicInteger<qint64> &MutableDecompressionTotalBytesIn() noexcept
    {
        return mDecompressionTotalBytesIn;
    }

    /**
     * @brief Returns the exported-row progress counter.
     *
     * @return The read-only atomic counter.
     */
    [[nodiscard]] const QAtomicInteger<qint64> &ExportRowsWritten() const noexcept
    {
        return mExportRowsWritten;
    }

    /**
     * @brief Returns the mutable exported-row progress counter.
     *
     * @return The mutable atomic counter.
     */
    [[nodiscard]] QAtomicInteger<qint64> &MutableExportRowsWritten() noexcept
    {
        return mExportRowsWritten;
    }

    /**
     * @brief Returns the total export-row counter.
     *
     * @return The read-only atomic counter.
     */
    [[nodiscard]] const QAtomicInteger<qint64> &ExportRowsTotal() const noexcept
    {
        return mExportRowsTotal;
    }

    /**
     * @brief Returns the mutable total export-row counter.
     *
     * @return The mutable atomic counter.
     */
    [[nodiscard]] QAtomicInteger<qint64> &MutableExportRowsTotal() noexcept
    {
        return mExportRowsTotal;
    }

    /** @brief Maximum number of exact row matches retained in the find cache. */
    static constexpr int MAX_FIND_MATCH_COUNT = 10000;

    /**
     * @brief Caches find matches and overview-rail density for one query.
     */
    struct FindMatchCache
    {
        /** @brief Query text. */
        QString needle;
        /** @brief Whether wildcard matching was used. */
        bool wildcards = false;
        /** @brief Whether regular-expression matching was used. */
        bool regularExpressions = false;
        /** @brief Whether exact row storage reached its limit. */
        bool overflowed = false;
        /** @brief Sorted, deduplicated source rows. */
        std::vector<int> sortedRows;
        /** @brief Exact count, or a lower bound when `overflowed` is true. */
        uint32_t totalMatches = 0;
        /** @brief Match density by overview bucket. */
        std::vector<uint32_t> bucketCounts;
    };

    /**
     * @brief Returns the current find-match cache.
     *
     * @return The optional read-only cache.
     */
    [[nodiscard]] const std::optional<FindMatchCache> &FindMatchCacheState() const noexcept
    {
        return mFindMatchCache;
    }

    /**
     * @brief Returns the mutable find-match cache.
     *
     * @return The optional mutable cache.
     */
    [[nodiscard]] std::optional<FindMatchCache> &MutableFindMatchCacheState() noexcept
    {
        return mFindMatchCache;
    }

    /**
     * @brief Clears the find-match cache.
     */
    void ResetFindMatchCache() noexcept
    {
        mFindMatchCache.reset();
    }

    /**
     * @brief Tests whether export is active.
     *
     * @return The export in-flight latch.
     */
    [[nodiscard]] bool IsExportInFlight() const noexcept
    {
        return mExportInFlight;
    }

    /**
     * @brief Returns the current export operation generation.
     *
     * @return The monotonic generation counter.
     */
    [[nodiscard]] std::uint64_t ExportGeneration() const noexcept
    {
        return mExportGeneration;
    }

    /**
     * @brief Sets export activity.
     *
     * A false-to-true transition advances the generation. Real changes emit
     * `presentationChanged()`.
     *
     * @param inFlight Whether export is active.
     */
    void SetExportInFlight(bool inFlight);

    /**
     * @brief Tests whether the active export is a session bundle.
     *
     * @return The bundle-export selector.
     */
    [[nodiscard]] bool IsExportBundle() const noexcept
    {
        return mExportIsBundle;
    }

    /**
     * @brief Sets whether the active export is a session bundle.
     *
     * This label selector does not emit presentation signals.
     *
     * @param isBundle Whether the export is a bundle.
     */
    void SetExportIsBundle(bool isBundle) noexcept;

    /**
     * @brief Returns the active export destination.
     *
     * @return The destination path.
     */
    [[nodiscard]] const QString &ExportDestinationPath() const noexcept
    {
        return mExportDestinationPath;
    }

    /**
     * @brief Sets the active export destination.
     *
     * @param path Destination path.
     */
    void SetExportDestinationPath(QString path);

    /**
     * @brief Returns the human-readable export format.
     *
     * @return The format label.
     */
    [[nodiscard]] const QString &ExportFormatLabel() const noexcept
    {
        return mExportFormatLabel;
    }

    /**
     * @brief Sets the human-readable export format.
     *
     * @param label Format label.
     */
    void SetExportFormatLabel(QString label);

    /**
     * @brief Returns the export start time.
     *
     * @return The monotonic start time.
     */
    [[nodiscard]] std::chrono::steady_clock::time_point ExportStartedAt() const noexcept
    {
        return mExportStartedAt;
    }

    /**
     * @brief Sets the export start time.
     *
     * @param startedAt Monotonic start time.
     */
    void SetExportStartedAt(std::chrono::steady_clock::time_point startedAt) noexcept;

    /**
     * @brief Clears the export destination and format label.
     */
    void ClearExportScratchState() noexcept;

    /**
     * @brief Resets all streaming counters, waiting state, and display name.
     *
     * Emits one `presentationChanged()` signal when tracked state changes.
     */
    void ResetStreamingCountersAndFileName();

    /**
     * @brief Resets per-file line, error, and first-batch progress.
     *
     * Emits one `presentationChanged()` signal when tracked state changes.
     */
    void ResetStreamingProgress();

    /**
     * @brief Returns this session's process identity.
     *
     * @return The immutable instance identity.
     */
    [[nodiscard]] SessionInstanceId InstanceId() const noexcept
    {
        return mInstanceId;
    }

signals:
    /**
     * @brief Signals that presentation state may have changed.
     *
     * Most mutators emit only on a real change. Explicit notification,
     * callback-based source mutation, and value-bearing source replacement
     * emit unconditionally by contract. Model row signals remain authoritative
     * for row-set changes.
     */
    void presentationChanged();

    /**
     * @brief Signals a filter dirty-state transition.
     *
     * @param dirty New dirty state.
     */
    void filtersDirtyChanged(bool dirty);

    /**
     * @brief Signals a rotation-flash state transition.
     *
     * @param active New flash state.
     */
    void rotationFlashChanged(bool active);

private:
    // Raw pointers rather than `QPointer` because these services are
    // owned by the application coordinator and outlive every
    // `LogSession`. Using raw pointers here also keeps the header
    // free of the service definitions (see accessor note above).
    ThemeControl *mTheme = nullptr;
    SessionHistoryManager *mHistoryManager = nullptr;
    RegexTemplateRegistry *mRegexTemplateRegistry = nullptr;

    // Owned via Qt parentage — each of the five is constructed as a
    // child of `this` in the ctor (see accessor block above for the
    // required lifetime order). Raw pointers not `QPointer` because
    // the parent-child destruction chain guarantees the members are
    // torn down before `LogSession` itself.
    AnchorManager *mAnchors = nullptr;
    HighlightRuleSet *mHighlights = nullptr;
    LogModel *mModel = nullptr;
    RowOrderProxyModel *mRowOrderProxyModel = nullptr;
    LogFilterModel *mSortFilterProxyModel = nullptr;

    // Unsaved filter-state marker.
    bool mFiltersDirty = false;

    // Suppresses dirty transitions during configuration loading.
    bool mLoadingConfiguration = false;

    // Keeps a loaded sort pending until streaming completes.
    bool mPendingApplySortFromConfig = false;

    // Simple-mode leaves and their stable display order.
    std::unordered_map<std::string, loglib::LeafRule> mSimpleLeaves;
    std::vector<std::string> mSimpleLeafOrder;

    // Prevents nested enum-dependent filter rebuilds.
    bool mApplyingEnumRebuild = false;

    // Current and most recently completed session modes.
    Mode mMode = Mode::Idle;
    Mode mLastTerminalMode = Mode::Idle;

    // Prevents completion handling from re-entering a destructive switch.
    bool mSessionSwitchInProgress = false;

    // Streaming status counters and latches.
    qsizetype mStreamingLineCount = 0;
    qsizetype mStreamingErrorCount = 0;
    std::size_t mStreamingErrorsCut = 0;
    bool mFirstStreamingBatchSeen = false;
    bool mSourceWaiting = false;

    // Rotation flash is isolated to this session.
    bool mRotationFlashActive = false;

    // Invalidates earlier flash timers when the deadline is refreshed.
    std::uint64_t mRotationFlashGeneration = 0;

    // User-facing source label.
    QString mStreamingFileName;

    // Source represented by the current model contents.
    std::optional<loglib::LogConfiguration::Source> mCurrentSource;

    // Pending open queue and categorized errors.
    QStringList mPendingOpenFiles;
    std::vector<std::string> mPendingOpenErrors;
    std::vector<std::string> mPendingDecompressionErrors;

    // Session-specific dock state.
    SessionParseErrorLog mParseErrorLog;
    SessionPendingPresentation mPendingPresentation;
    SessionFindQueryState mFindQuery;
    SessionHistogramState mHistogramState;
    SessionRecordDetailPin mRecordDetailPin;
    SessionAnchorsSelection mAnchorsSelection;

    // Live-tail promotion state.
    QString mPendingLiveTailPrimary;
    std::size_t mPendingLiveTailRetention = 0;

    // Rotation-history policy and undo state.
    bool mDisableRotationHistoryOverride = false;
    QStringList mLastRotationExpansionOriginalInputs;
    bool mLastRotationExpansionWasLiveTail = false;

    // Persistence identity and process-publication state.
    QString mAutoSaveUuid;
    bool mAutoSaveUuidPublished = false;

    // Authorizes embedded configuration for one bundle path.
    QString mApplyEmbeddedBundleConfigForPath;

    // Decompression operation identity and labels.
    bool mDecompressionInFlight = false;
    std::uint64_t mDecompressionGeneration = 0;
    QString mDecompressionOriginalPath;
    QString mDecompressionCodecName;
    std::chrono::steady_clock::time_point mDecompressionStartedAt;

    // Export operation identity and labels.
    bool mExportInFlight = false;
    std::uint64_t mExportGeneration = 0;
    bool mExportIsBundle = false;
    QString mExportDestinationPath;
    QString mExportFormatLabel;
    std::chrono::steady_clock::time_point mExportStartedAt;

    // Cooperative cancellation sources for active workers.
    loglib::StopSource mDecompressionStopSource;
    loglib::StopSource mExportStopSource;

    // Worker-written progress counters polled by the UI thread.
    QAtomicInteger<qint64> mDecompressionBytesIn = 0;
    QAtomicInteger<qint64> mDecompressionTotalBytesIn = 0;
    QAtomicInteger<qint64> mExportRowsWritten = 0;
    QAtomicInteger<qint64> mExportRowsTotal = 0;

    // Derived match cache, invalidated by row-set changes.
    std::optional<FindMatchCache> mFindMatchCache;

    // Monotonic clock for live-tail status.
    QElapsedTimer mLiveTailElapsedTimer;

    // Lazily allocated watchers parented to this session.
    DecompressionWatcher *mDecompressionWatcher = nullptr;
    ExportWatcher *mExportWatcher = nullptr;

    SessionInstanceId mInstanceId = SessionInstanceId::Next();
};
