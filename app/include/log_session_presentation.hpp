#pragma once

#include <loglib/log_configuration.hpp>

#include <QString>

#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

/**
 * @brief Identifies a `LogSession` instance within the process.
 *
 * A generated identity is assigned once and is independent of the session's
 * persistence UUID. The default value is invalid.
 */
class SessionInstanceId
{
public:
    using Value = std::uint64_t;

    /**
     * @brief Constructs an invalid session identity.
     */
    SessionInstanceId() = default;

    /**
     * @brief Constructs an identity from its numeric value.
     *
     * @param value Numeric identity; zero represents an invalid identity.
     */
    constexpr explicit SessionInstanceId(Value value) noexcept
        : mValue(value)
    {
    }

    /**
     * @brief Returns the numeric identity.
     *
     * @return The stored numeric value.
     */
    [[nodiscard]] constexpr Value value() const noexcept
    {
        return mValue;
    }

    /**
     * @brief Tests whether the identity refers to a session.
     *
     * @return `true` when the stored value is nonzero.
     */
    [[nodiscard]] constexpr bool isValid() const noexcept
    {
        return mValue != 0;
    }

    /**
     * @brief Compares two session identities for equality.
     *
     * @param lhs Left identity.
     * @param rhs Right identity.
     * @return `true` when both identities have the same value.
     */
    friend constexpr bool operator==(SessionInstanceId lhs, SessionInstanceId rhs) noexcept = default;

    /**
     * @brief Orders two session identities by numeric value.
     *
     * @param lhs Left identity.
     * @param rhs Right identity.
     * @return The three-way comparison result.
     */
    friend constexpr auto operator<=>(SessionInstanceId lhs, SessionInstanceId rhs) noexcept = default;

    /**
     * @brief Issues the next process-wide session identity.
     *
     * This function is thread-safe and never returns zero.
     *
     * @return A new monotonically increasing identity.
     */
    [[nodiscard]] static SessionInstanceId Next() noexcept;

private:
    Value mValue = 0;
};

/**
 * @brief Classifies the source currently represented by a session.
 *
 * The presentation snapshot selects one value. Stream source kinds take
 * precedence, followed by bundle, live-tail, multi-file, compressed, and
 * static-file classifications.
 */
enum class SessionSourceMode : std::uint8_t
{
    /** @brief No source is bound. */
    Idle,
    /** @brief A static file source is bound. */
    StaticFile,
    /** @brief A live-tailed file is bound. */
    LiveTail,
    /** @brief Standard input is bound. */
    Stdin,
    /** @brief A network listener is bound. */
    Network,
    /** @brief A session bundle is being opened. */
    Bundle,
    /** @brief A compressed source is being opened. */
    Compressed,
    /** @brief Multiple files are bound. */
    MultiFile,
};

/**
 * @brief Defines operation-state bits that may coexist within a session.
 */
enum class SessionOperationState : std::uint32_t
{
    /** @brief No operation is active. */
    Idle = 0,
    /** @brief Static parsing has not delivered its first non-empty batch. */
    Parsing = 1U << 0,
    /** @brief A decompression worker is active. */
    Decompressing = 1U << 1,
    /** @brief An export worker is active. */
    Exporting = 1U << 2,
    /** @brief A live-tail or network producer is active. */
    Ingesting = 1U << 3,
    /** @brief The producer is paused. */
    Paused = 1U << 4,
    /** @brief The source is waiting for input. */
    SourceWaiting = 1U << 5,
    /** @brief A stream source is awaiting reconnection. */
    Disconnected = 1U << 6,
    /** @brief A queued operation failure is waiting to be presented. */
    Failed = 1U << 7,
};

/**
 * @brief Describes persistence-related session state for presentation.
 */
struct SessionDirtyState
{
    /** @brief Whether unsaved filter or highlight-editor-draft changes are present. */
    bool filtersDirty = false;
    /** @brief Whether the source can be restored in place. */
    bool restorableInPlace = false;
    /** @brief Whether the source has no reproducible restore path. */
    bool ephemeralUnreproducible = false;
};

/**
 * @brief Selects how the shell closes or replaces a dirty session.
 */
enum class SessionCloseDecision : std::uint8_t
{
    /** @brief The session has no unsaved changes and closes without a prompt. */
    Silent,
    /** @brief A restorable file-backed session is autosaved without a prompt. */
    Autosave,
    /** @brief Unsaved changes that cannot be autosaved require Save, Discard, or Cancel. */
    Prompt,
};

/**
 * @brief Stores uncommitted Highlight Rules editor state for one session.
 *
 * `localRules` is the in-progress buffer. `baseline` is the last committed
 * snapshot used for dirty detection. `currentRow` is the selected list index,
 * or `-1` when the list is empty. Tab switches capture and restore this
 * value without committing rules.
 */
struct HighlightRulesEditorDraft
{
    /** @brief In-progress rule list, including unsaved form edits. */
    std::vector<loglib::LogConfiguration::HighlightRule> localRules;
    /** @brief Last committed rule list. */
    std::vector<loglib::LogConfiguration::HighlightRule> baseline;
    /** @brief Selected rule row, or `-1` when none. */
    int currentRow = -1;

    /**
     * @brief Tests whether the in-progress list differs from the baseline.
     *
     * @return `true` when `localRules` and `baseline` are not equal.
     */
    [[nodiscard]] bool isDirty() const
    {
        return localRules != baseline;
    }
};

/**
 * @brief Defines reasons a session cannot be closed silently.
 *
 * `LogSession::PreCheckClose()` returns a bitwise combination of these
 * values. The shell handles the corresponding prompt or worker drain.
 */
enum class SessionClosePreconditions : std::uint32_t
{
    /** @brief No shell-owned close handling is required. */
    None = 0,
    /** @brief Unsaved filter or highlight-editor-draft changes require close handling. */
    FiltersDirty = 1U << 0,
    /** @brief Active decompression must be stopped and drained. */
    DecompressionInFlight = 1U << 1,
    /** @brief Active export must be stopped and drained. */
    ExportInFlight = 1U << 2,
};

/**
 * @brief Combines two close preconditions.
 *
 * @param a Left precondition.
 * @param b Right precondition.
 * @return The combined bit mask.
 */
[[nodiscard]] constexpr std::uint32_t operator|(SessionClosePreconditions a, SessionClosePreconditions b) noexcept
{
    return static_cast<std::uint32_t>(a) | static_cast<std::uint32_t>(b);
}

/**
 * @brief Adds a close precondition to a bit mask.
 *
 * @param a Existing bit mask.
 * @param b Precondition to add.
 * @return The combined bit mask.
 */
[[nodiscard]] constexpr std::uint32_t operator|(std::uint32_t a, SessionClosePreconditions b) noexcept
{
    return a | static_cast<std::uint32_t>(b);
}

/**
 * @brief Tests a close-precondition bit in a mask.
 *
 * @param a Existing bit mask.
 * @param b Precondition to test.
 * @return The matching bit value, or zero.
 */
[[nodiscard]] constexpr std::uint32_t operator&(std::uint32_t a, SessionClosePreconditions b) noexcept
{
    return a & static_cast<std::uint32_t>(b);
}

/**
 * @brief Adds a close precondition to a bit mask in place.
 *
 * @param a Bit mask to update.
 * @param b Precondition to add.
 * @return A reference to the updated mask.
 */
constexpr std::uint32_t &operator|=(std::uint32_t &a, SessionClosePreconditions b) noexcept
{
    a |= static_cast<std::uint32_t>(b);
    return a;
}

/**
 * @brief Stores the find query and matcher options for a session.
 *
 * Match counts are derived from the query and model and are not stored here.
 */
struct SessionFindQueryState
{
    /** @brief Last search text; may be empty. */
    QString query;
    /** @brief Whether wildcard matching is enabled. */
    bool wildcards = false;
    /** @brief Whether regular-expression matching is enabled. */
    bool regex = false;
};

/**
 * @brief Groups parse errors under a presentation title.
 */
struct SessionParseErrorBatch
{
    /** @brief Group header. */
    QString title;
    /** @brief Error rows in insertion order. */
    std::vector<std::string> errors;
};

/**
 * @brief Stores parse-error presentation state for a session.
 */
struct SessionParseErrorLog
{
    /** @brief Batches accumulated since the last reset, in insertion order. */
    std::vector<SessionParseErrorBatch> batches;

    /** @brief Number of errors omitted because of the display cap. */
    int droppedCount = 0;

    /** @brief Whether the dock has already handled its first-batch behavior. */
    bool hasSeenFirstBatch = false;
};

/**
 * @brief Stores the user-selected histogram bucket size.
 *
 * The bucket size uses the underlying representation of
 * `loglib::HistogramBucketSize` to avoid a loglib dependency.
 */
struct SessionHistogramState
{
    /** @brief Whether automatic bucket-size selection is disabled. */
    bool bucketSizePinned = false;

    /** @brief Last selected bucket-size value, when available. */
    std::optional<std::uint8_t> bucketSize;
};

/**
 * @brief Stores the selected anchor's stable key.
 *
 * An empty locator and zero line identifier represent no selection.
 */
struct SessionAnchorsSelection
{
    /** @brief Stable source locator. */
    std::string keyLocator;
    /** @brief Stable source line identifier. */
    std::uint64_t keyLineId = 0;
};

/**
 * @brief Stores the record-detail pin for a session.
 *
 * A row of `-1` with `everPinned == false` means no record has been pinned.
 * A row of `-1` with `everPinned == true` means the pinned record was evicted.
 */
struct SessionRecordDetailPin
{
    /** @brief Fallback source row; `-1` means no current row. */
    int pinnedSourceRow = -1;
    /** @brief Whether a record has ever been pinned. */
    bool everPinned = false;

    /** @brief Stable source locator; empty when no key is available. */
    std::string keyLocator;
    /** @brief Stable source line identifier; zero when no key is available. */
    std::uint64_t keyLineId = 0;
};

/**
 * @brief Captures session state consumed by shell presentation.
 */
struct SessionPresentationSnapshot
{
    /** @brief Current source classification. */
    SessionSourceMode mode = SessionSourceMode::Idle;
    /** @brief Bitwise combination of `SessionOperationState` values. */
    std::uint32_t operations = 0;
    /** @brief Persistence-related state. */
    SessionDirtyState dirty;

    /** @brief Elision-safe tab label without a path. */
    QString shortLabel;
    /** @brief Full source description for the tab tooltip. */
    QString tooltip;
    /** @brief Source label for the status bar and window title. */
    QString sourceLabel;
    /** @brief Localized source-status summary. */
    QString statusSummary;

    /** @brief Number of retained rows in the session model. */
    qsizetype rowCount = 0;
    /** @brief Number of rows after filtering and sorting. */
    qsizetype visibleRows = 0;
    /** @brief Parse-error count. */
    qsizetype errorCount = 0;
    /** @brief Number of errors omitted by retention. */
    qsizetype droppedErrors = 0;

    /** @brief Whether session mutation commands are currently allowed. */
    bool mutationsAllowed = true;

    /** @brief Whether closing requires confirmation or worker handling. */
    bool confirmBeforeClose = false;
};

/**
 * @brief Stores UI work produced by a background-tab completion.
 *
 * The shell applies and clears this payload when the originating tab
 * becomes selected. Session state and tab chrome update immediately;
 * shared window widgets do not.
 */
struct SessionPendingPresentation
{
    /** @brief Status-bar text to show on activation; empty when none. */
    QString statusMessage;
    /** @brief Display timeout for `statusMessage`, in milliseconds. */
    int statusTimeoutMs = 0;
    /** @brief Modal failure title; empty when no dialog is queued. */
    QString failureTitle;
    /** @brief Modal failure body; empty when no dialog is queued. */
    QString failureMessage;
    /** @brief Whether to raise the parse-errors dock on activation. */
    bool raiseParseErrors = false;

    /**
     * @brief Tests whether any UI work is queued.
     * @return `true` when every field is at its default.
     */
    [[nodiscard]] bool isEmpty() const noexcept
    {
        return statusMessage.isEmpty() && failureTitle.isEmpty() && failureMessage.isEmpty() && !raiseParseErrors;
    }
};
