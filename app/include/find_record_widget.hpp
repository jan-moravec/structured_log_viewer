#pragma once

#include <QString>
#include <QWidget>

class QAction;
class QEvent;
class QIcon;
class QKeyEvent;
class QLabel;
class QLineEdit;
class QObject;
class QTimer;
class QToolButton;

/**
 * @brief Incremental find bar with regex and wildcard modes.
 *
 * Return and Shift+Return navigate matches, while Escape dismisses
 * the host dock. Match-count requests are debounced and answered
 * through `SetMatchInfo()`.
 */
class FindRecordWidget : public QWidget
{
    Q_OBJECT
public:
    explicit FindRecordWidget(QWidget *parent = nullptr);

public slots:
    /** @brief Focuses the search field and selects its contents. */
    void SetEditFocus();

    /**
     * @brief Updates the match-count indicator.
     * @param current One-based current match, or a non-positive value when unknown.
     * @param total Total matches, or a non-positive value to clear the label.
     * @param overflowed Whether `total` is a lower bound.
     */
    void SetMatchInfo(int current, int total, bool overflowed = false);

    /** @brief Closes the nearest host dock, if present. */
    void DismissBar();

    /** @brief Arms match recounting for a non-empty query. */
    void BumpMatchCountDebounce();

    /**
     * @brief Returns the current query text.
     * @return Search text, or an empty string when unavailable.
     */
    [[nodiscard]] QString queryText() const;
    /**
     * @brief Reports whether wildcard matching is enabled.
     * @return True when wildcard mode is active.
     */
    [[nodiscard]] bool queryWildcards() const;
    /**
     * @brief Reports whether regular-expression matching is enabled.
     * @return True when regular-expression mode is active.
     */
    [[nodiscard]] bool queryRegex() const;

    /**
     * @brief Restores query text and matching modes.
     * @param text Query text to restore.
     * @param wildcards Whether wildcard matching was enabled.
     * @param regex Whether regular-expression matching was enabled.
     *
     * Regular-expression mode wins if both mode arguments are true.
     * Navigation is not emitted, and non-empty text arms a recount.
     */
    void RestoreQueryState(const QString &text, bool wildcards, bool regex);

    /** @brief Stops both pending match-count timers. */
    void CancelPendingMatchCountRequest();

signals:
    /**
     * @brief Requests navigation to a matching record.
     * @param text Search text.
     * @param next Whether to search forward.
     * @param wildcards Whether wildcard matching is enabled.
     * @param regularExpressions Whether regular-expression matching is enabled.
     */
    void FindRecords(const QString &text, bool next, bool wildcards, bool regularExpressions);

    /**
     * @brief Requests a match recount after query state changes.
     * @param text Search text; empty means no active query.
     * @param wildcards Whether wildcard matching is enabled.
     * @param regularExpressions Whether regular-expression matching is enabled.
     */
    void MatchCountRequested(const QString &text, bool wildcards, bool regularExpressions);

public:
    /**
     * @brief Handles Return navigation in the search field.
     * @param watched Object receiving the event.
     * @param event Event being filtered.
     * @return True when a navigation key is consumed.
     */
    bool eventFilter(QObject *watched, QEvent *event) override;

protected:
    void keyPressEvent(QKeyEvent *event) override;

    /**
     * @brief Repaints arrow icons after palette or style changes.
     * @param event Change event.
     */
    void changeEvent(QEvent *event) override;

    /**
     * @brief Refreshes arrow icons after device-pixel-ratio changes.
     * @param event Event to dispatch.
     * @return The result of `QWidget::event()`.
     */
    bool event(QEvent *event) override;

private slots:
    void FindNext();
    void FindPrevious();

    /** @brief Schedules a match-count request after query changes. */
    void RequestMatchCountSoon();

    /** @brief Emits one match-count request and stops both timers. */
    void EmitMatchCountRequest();

private:
    QLineEdit *mEdit = nullptr;
    QAction *mWildcardsAction = nullptr;
    QAction *mRegexAction = nullptr;
    QToolButton *mButtonNext = nullptr;
    QToolButton *mButtonPrevious = nullptr;
    QLabel *mMatchCountLabel = nullptr;

    // Trailing-edge debounce coalesces rapid changes into one scan.
    QTimer *mMatchCountTimer = nullptr;

    // Max-age timer guarantees an update during continuous activity.
    QTimer *mMatchCountMaxAgeTimer = nullptr;

    // Prevents both timers from emitting the same request.
    bool mEmittingMatchCountRequest = false;

    /** @brief Rebuilds navigation icons for the current palette and DPI. */
    void RefreshArrowIcons();
};
