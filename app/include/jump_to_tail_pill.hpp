#pragma once

#include <QToolButton>

class QEvent;
class QGraphicsOpacityEffect;
class QPropertyAnimation;

/// Floating "N new lines" pill (Slack / YouTube pattern) shown on
/// the log table viewport. When the user has scrolled away from
/// the tail and new rows arrive, the pill fades in with a running
/// count; clicking it jumps to the newest row.
///
/// Owned by `LogTableView` and parented to its viewport. The arrow
/// direction tracks the configured `TailEdge` (Down for Bottom, Up
/// for Top); the host repositions the pill near that edge.
class JumpToTailPill : public QToolButton
{
    Q_OBJECT

public:
    enum class ArrowDirection
    {
        Down, ///< matches `LogTableView::TailEdge::Bottom`
        Up,   ///< matches `LogTableView::TailEdge::Top`
    };

    explicit JumpToTailPill(QWidget *parent);

    /// Set the displayed count and fade in / out. Positive counts
    /// fade in; `<= 0` fades out and hides. Text caps at "999+" to
    /// bound the pill width. Emits `contentSizeChanged` so the
    /// host can re-anchor when `sizeHint` shifts (count growth,
    /// shrink, or crossing the cap).
    void SetCount(int count);

    /// Set the arrow direction. Idempotent. The icon is re-tinted
    /// from this widget's palette so theme changes refresh it.
    void SetArrowDirection(ArrowDirection direction);

    [[nodiscard]] ArrowDirection Direction() const noexcept
    {
        return mDirection;
    }

    [[nodiscard]] int Count() const noexcept
    {
        return mCount;
    }

signals:
    /// Rendered text changed and the pill's `sizeHint` may have
    /// moved (in either direction). The host re-runs
    /// `PositionTailPill` so the pill stays centred at the edge.
    void contentSizeChanged();

protected:
    /// Re-tint the icon and re-apply the QSS on palette / style
    /// changes so the pill tracks theme switches without restart.
    void changeEvent(QEvent *event) override;

private:
    /// Rebuild the visible text and accessible name from `mCount`.
    void RebuildText();

    /// Re-rasterise the arrow icon in the current `HighlightedText`
    /// colour at the current device pixel ratio.
    void RefreshIcon();

    /// Apply the rounded-pill QSS resolved against the current
    /// palette. Pulled out so `changeEvent` can re-run it.
    void ApplyStyleSheet();

    /// Run the fade animation, retargeting any in-flight run so a
    /// fade-in interrupted by a fade-out (or vice versa) stays
    /// smooth.
    void FadeTo(qreal endOpacity);

    QGraphicsOpacityEffect *mOpacity = nullptr;
    QPropertyAnimation *mFade = nullptr;
    int mCount = 0;
    ArrowDirection mDirection = ArrowDirection::Down;

    /// Re-entrancy guard. On Qt 6, `setStyleSheet` synchronously
    /// emits `StyleChange` / `PaletteChange`, which would route
    /// back into `ApplyStyleSheet` and recurse forever (this
    /// crashed the test suite during construction). External
    /// theme switches arrive outside our `setStyleSheet`, so the
    /// guard is clear and the refresh still runs.
    bool mApplyingPalette = false;
};
