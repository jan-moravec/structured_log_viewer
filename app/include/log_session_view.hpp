#pragma once

#include <QPointer>
#include <QString>
#include <QWidget>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

class LogSession;
class LogTableView;
class OverviewRailModel;
class OverviewRailWidget;
class ThemeControl;
class QAbstractItemDelegate;
class QLabel;
class QProgressBar;
class QPushButton;
class QVBoxLayout;
class QWidget;

/**
 * @brief Per-tab workspace bound to one `LogSession`.
 *
 * The view owns its table, overview rail model, overview rail widget,
 * and progress widgets through Qt parentage. The bound session is
 * borrowed and is not replaced during the view's lifetime.
 */
class LogSessionView : public QWidget
{
    Q_OBJECT

public:
    /**
     * @brief Constructs a session view with optional theme support.
     * @param session Session to borrow for the view's lifetime; must not be null.
     * @param theme Theme service to borrow, or `nullptr` to use palette defaults.
     * @param parent Parent widget.
     */
    LogSessionView(LogSession *session, ThemeControl *theme, QWidget *parent = nullptr);

    /**
     * @brief Constructs a session view using palette defaults.
     * @param session Session to borrow for the view's lifetime; must not be null.
     * @param parent Parent widget.
     */
    explicit LogSessionView(LogSession *session, QWidget *parent = nullptr);
    ~LogSessionView() override;

    LogSessionView(const LogSessionView &) = delete;
    LogSessionView &operator=(const LogSessionView &) = delete;
    LogSessionView(LogSessionView &&) = delete;
    LogSessionView &operator=(LogSessionView &&) = delete;

    /**
     * @brief Returns the session bound to this view.
     * @return The borrowed session, or `nullptr` after it is destroyed.
     */
    [[nodiscard]] LogSession *Session() const noexcept;

    /**
     * @brief Returns the owned log table view.
     * @return A non-null child of this view.
     */
    [[nodiscard]] LogTableView *TableView() const noexcept
    {
        return mTableView;
    }

    /**
     * @brief Returns the owned overview rail widget.
     * @return A non-null widget that is hidden until attached and shown.
     */
    [[nodiscard]] OverviewRailWidget *OverviewRail() const noexcept
    {
        return mOverviewRailWidget;
    }

    /**
     * @brief Returns the owned overview rail model.
     * @return A non-null model used by `OverviewRail()`.
     */
    [[nodiscard]] OverviewRailModel *OverviewRailModelPtr() const noexcept
    {
        return mOverviewRailModel;
    }

    /**
     * @brief Attaches or detaches this view's overview rail on its table.
     *
     * A visible rail is reparented into the table's reserved viewport
     * margin. A hidden rail is reparented onto this view, hidden, and
     * its bucket vector is cleared so proxy signals skip rebuilds.
     *
     * @param visible Whether the rail should be attached and shown.
     */
    void SetOverviewRailVisible(bool visible);

    /**
     * @brief Selects and centers a source-model row in the table.
     * @param sourceRow Source-model row index.
     *
     * Emits `rowNotVisible()` if the row is invalid or filtered out.
     */
    void SelectSourceRow(int sourceRow);

    /**
     * @brief Centers a filter-proxy row in the table.
     * @param proxyRow Filter-proxy row index.
     * @param replaceSelection Whether to replace the current selection with the row.
     *
     * Valid navigation emits `followTailDisengageRequested()`.
     */
    void ScrollToProxyRow(int proxyRow, bool replaceSelection);

    /**
     * @brief Applies configured column visibility to the table header.
     *
     * The operation is idempotent and is a no-op when the model or
     * header is unavailable.
     */
    void ApplyColumnVisibility();

    /**
     * @brief Applies a borrowed delegate to the active level column.
     * @param delegate Delegate owned by the caller; `nullptr` leaves the table unchanged.
     *
     * In text mode, or when the level column moves, the previously
     * installed column delegate is detached.
     */
    void ApplyLevelCellDelegate(QAbstractItemDelegate *delegate);

    /**
     * @brief Parsed timestamp input.
     *
     * `micros` is epoch microseconds. `isNaive` is true when the
     * accepted format has no time-zone specifier.
     */
    struct GotoTimestampParse
    {
        int64_t micros = 0;
        bool isNaive = false;
    };

    /**
     * @brief Opens the one-based Go to Line dialog.
     *
     * Line 1 is the earliest retained row. Invalid input is reported
     * through `statusMessageRequested()`.
     */
    void PromptGotoLine();

    /**
     * @brief Opens the Go to Timestamp dialog.
     *
     * The dialog accepts configured timestamp formats, ISO fallbacks,
     * and relative hour or minute shortcuts.
     */
    void PromptGotoTimestamp();

    /**
     * @brief Processes one-based Go to Line input.
     * @param input Text to parse as a source row number.
     */
    void ExecuteGotoLine(const QString &input);

    /**
     * @brief Processes Go to Timestamp input.
     * @param input Timestamp or relative shortcut to parse.
     * @param now Reference time used by relative shortcuts.
     */
    void ExecuteGotoTimestamp(const QString &input, std::chrono::system_clock::time_point now);

    /**
     * @brief Parses absolute or relative timestamp input.
     * @param input Text to parse.
     * @param columnParseFormats Configured formats to try before ISO fallbacks.
     * @param now Reference time used by relative shortcuts.
     * @return Parsed timestamp metadata, or `std::nullopt` on failure.
     */
    [[nodiscard]] static std::optional<GotoTimestampParse> ParseGotoTimestampInput(
        const QString &input,
        const std::vector<std::string> &columnParseFormats,
        std::chrono::system_clock::time_point now
    );

    /**
     * @brief Returns the last submitted Go to Timestamp text.
     * @return The sticky timestamp input.
     */
    [[nodiscard]] QString LastGotoTimestampInput() const noexcept
    {
        return mLastGotoTimestampInput;
    }

    /** @brief Clears the sticky Go to Line and Go to Timestamp inputs. */
    void ClearGotoStickyInputs() noexcept
    {
        mLastGotoLineInput.clear();
        mLastGotoTimestampInput.clear();
    }

    /**
     * @brief Sets the sticky timestamp input for tests.
     * @param value Value to store.
     */
    void SetLastGotoTimestampInputForTest(QString value) noexcept
    {
        mLastGotoTimestampInput = std::move(value);
    }

    /** @brief Scrolls the viewport to the newest visible row. */
    void JumpToNewestRow();

    /**
     * @brief Selects the next or previous visible anchor.
     * @param forward Select the next anchor when true, otherwise the previous anchor.
     */
    void JumpToAnchor(bool forward);

    /**
     * @brief Shows the per-tab operation progress strip.
     * @param label Operation label.
     * @param percent Progress from 0 through 100, or a negative value for indeterminate mode.
     */
    void ShowOperationProgress(const QString &label, int percent);

    /**
     * @brief Updates the visible operation progress strip.
     * @param label Operation label.
     * @param percent Progress from 0 through 100, or a negative value for indeterminate mode.
     *
     * The call is a no-op while the strip is hidden.
     */
    void UpdateOperationProgress(const QString &label, int percent);

    /** @brief Hides and resets the per-tab progress strip. */
    void HideOperationProgress();

    /**
     * @brief Enables or disables the table and overview rail without affecting the progress strip.
     * @param enabled Whether table and rail interaction is allowed.
     *
     * The progress strip and its Cancel control stay enabled so a
     * background tab can cancel its own operation.
     */
    void SetContentEnabled(bool enabled);

    /**
     * @brief Reports whether the table content is enabled.
     * @return `true` when the table accepts interaction.
     */
    [[nodiscard]] bool IsContentEnabled() const noexcept;

    /**
     * @brief Reports whether the progress strip has been shown.
     * @return True when the strip is not explicitly hidden.
     */
    [[nodiscard]] bool IsOperationProgressVisible() const noexcept;

    /**
     * @brief Returns the progress strip's Cancel button.
     * @return The button after the strip is created, or `nullptr`.
     */
    [[nodiscard]] QPushButton *ProgressCancelButton() const noexcept
    {
        return mProgressCancelButton;
    }

signals:
    /** @brief Emitted when a requested source row is not visible. */
    void rowNotVisible();

    /** @brief Emitted before explicit proxy-row navigation scrolls the view. */
    void followTailDisengageRequested();

    /**
     * @brief Requests presentation of a transient status message.
     * @param message Message to present.
     */
    void statusMessageRequested(const QString &message);

    /** @brief Emitted when the progress strip's Cancel button is clicked. */
    void progressCancelRequested();

private:
    /**
     * @brief Creates and wires the table, rail, and layout.
     * @param theme Theme service to borrow, or `nullptr`.
     */
    void Initialise(ThemeControl *theme);

    QPointer<LogSession> mSession;
    LogTableView *mTableView = nullptr;
    OverviewRailModel *mOverviewRailModel = nullptr;
    OverviewRailWidget *mOverviewRailWidget = nullptr;
    QVBoxLayout *mLayout = nullptr;

    // Column carrying the level delegate, or -1 when detached.
    int mInstalledLevelDelegateColumn = -1;

    // Last submitted Go to Timestamp input.
    QString mLastGotoTimestampInput;

    // Last submitted Go to Line input.
    QString mLastGotoLineInput;

    // Lazily-created per-tab progress widgets.
    QWidget *mProgressStrip = nullptr;
    QLabel *mProgressLabel = nullptr;
    QProgressBar *mProgressBar = nullptr;
    QPushButton *mProgressCancelButton = nullptr;

    /** @brief Creates the progress strip on first use. */
    void EnsureProgressStrip();
};
