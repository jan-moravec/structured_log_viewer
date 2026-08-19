#pragma once

#include <QPointer>

class LogSession;
class LogSessionView;
class LogModel;
class LogFilterModel;
class RowOrderProxyModel;
class AnchorManager;
class HighlightRuleSet;
class ThemeControl;
class QItemSelectionModel;

/**
 * @brief Bundles guarded, non-owning objects used to bind session UI.
 *
 * A default-constructed context is unbound. Session and view objects are
 * guarded by `QPointer`, so destroying an object clears the corresponding
 * handle.
 */
struct SessionBindContext
{
    /**
     * @brief Builds a context from a session and its view.
     *
     * If either required argument is null, the function returns an unbound
     * context. The selection model may remain null while the view is being
     * initialized or torn down.
     *
     * @param session Session that owns the models and state.
     * @param view View paired with the session.
     * @param theme Non-owning theme service; may be null.
     * @return A bound context, or an unbound context for a partial pair.
     */
    [[nodiscard]] static SessionBindContext FromSessionAndView(
        LogSession *session, LogSessionView *view, ThemeControl *theme = nullptr
    );

    /**
     * @brief Creates an explicitly unbound context.
     *
     * @return A context with all handles null.
     */
    [[nodiscard]] static SessionBindContext MakeUnbound() noexcept
    {
        return SessionBindContext{};
    }

    /** @brief Bound session. */
    QPointer<LogSession> session;
    /** @brief View paired with the session. */
    QPointer<LogSessionView> view;

    /** @brief Session source model. */
    QPointer<LogModel> model;
    /** @brief Session row-order proxy. */
    QPointer<RowOrderProxyModel> rowOrderProxy;
    /** @brief Session filter and sort proxy. */
    QPointer<LogFilterModel> filterProxy;

    /** @brief Session anchor manager. */
    QPointer<AnchorManager> anchors;
    /** @brief Session highlight rules. */
    QPointer<HighlightRuleSet> highlights;

    /** @brief Optional view selection model. */
    QPointer<QItemSelectionModel> selection;

    /** @brief Non-owning theme service; may be null. */
    ThemeControl *theme = nullptr;

    /**
     * @brief Tests whether all required session-owned objects are alive.
     *
     * The selection model and theme service are optional and do not affect
     * the result.
     *
     * @return `true` when the session, view, models, anchors, and highlights
     * are all available.
     */
    [[nodiscard]] bool IsBound() const noexcept
    {
        return !session.isNull() && !view.isNull() && !model.isNull() && !rowOrderProxy.isNull() &&
               !filterProxy.isNull() && !anchors.isNull() && !highlights.isNull();
    }
};
