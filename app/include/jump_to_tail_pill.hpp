#pragma once

#include <QToolButton>

class QEvent;
class QGraphicsOpacityEffect;
class QPropertyAnimation;

/// Floating "* N new lines" pill rendered on top of the log table
/// viewport. Surfaces the reading-position preservation that
/// `LogTableView::SaveAnchorIfShouldPreserve` /
/// `RestoreAnchorIfSaved` already perform: when the user has
/// scrolled away from the tail edge and new rows arrive, the pill
/// fades in with a running count; a click jumps to the newest row
/// and (in live-tail sessions) re-engages Follow newest.
///
/// Owned by `LogTableView` and parented to its viewport so it
/// floats over the rows without being clipped by the grid. The
/// arrow direction follows the configured `TailEdge` -- "down"
/// for `Bottom` (default append-at-bottom), "up" for `Top`
/// (newest-first) -- and the host repositions the pill near the
/// matching viewport edge.
///
/// Pattern: YouTube comments / Slack "* N new messages" pill.
class JumpToTailPill : public QToolButton
{
    Q_OBJECT

public:
    /// Which way the arrow glyph points. `Down` matches
    /// `LogTableView::TailEdge::Bottom`; `Up` matches `Top`.
    enum class ArrowDirection
    {
        Down,
        Up,
    };

    explicit JumpToTailPill(QWidget *parent);

    /// Update the displayed count and drive the fade animation.
    /// `count <= 0` fades the pill out (and hides it once the
    /// animation finishes); a positive count fades it in. The
    /// host (`LogTableView`) feeds the running tally; this widget
    /// is purely a renderer and emits `clicked` on press.
    ///
    /// Displayed text caps at "999+" so a runaway live tail does
    /// not stretch the pill across the viewport. The cap is purely
    /// cosmetic; callers may pass any non-negative count.
    void SetCount(int count);

    /// Update which way the arrow points. Idempotent. The icon is
    /// re-tinted from the *button's* palette (not the parent's),
    /// so a theme switch reaching the pill via `changeEvent`
    /// refreshes the glyph correctly.
    void SetArrowDirection(ArrowDirection direction);

    /// Current rendered direction (read by tests and by
    /// `LogTableView::PositionTailPill` for edge selection).
    [[nodiscard]] ArrowDirection Direction() const noexcept
    {
        return mDirection;
    }

    /// Current count last fed to `SetCount`. Read by tests.
    [[nodiscard]] int Count() const noexcept
    {
        return mCount;
    }

protected:
    /// Re-tint the arrow icon and re-apply the rounded-pill QSS
    /// whenever the palette / style flips so the pill tracks
    /// Light <-> Dark theme switches without restart.
    void changeEvent(QEvent *event) override;

private:
    /// Rebuild the displayed text from `mCount` (e.g. "↓ 3 new
    /// lines" / "↑ 1 new line" / "↓ 999+ new lines"). Pure
    /// formatter; no side effects beyond `setText`.
    void RebuildText();

    /// Re-rasterise the arrow icon at the current palette's
    /// `HighlightedText` colour so the glyph reads as foreground
    /// on the pill's `Highlight` background. Called from
    /// `SetArrowDirection`, `changeEvent`, and after the first
    /// `show()` so the device-pixel-ratio matches.
    void RefreshIcon();

    /// Apply the rounded-pill QSS (background = `Highlight`,
    /// foreground = `HighlightedText`, `border-radius = h/2`).
    /// Pulled out so `changeEvent(PaletteChange)` can re-run it.
    void ApplyStyleSheet();

    /// Run the fade-in / fade-out animation. Sets `setVisible`
    /// at the appropriate ends so the hidden pill doesn't intercept
    /// hover events. Idempotent; re-targeting an in-flight
    /// animation just retargets the QPropertyAnimation's end
    /// value rather than chaining a new one.
    void FadeTo(qreal endOpacity);

    QGraphicsOpacityEffect *mOpacity = nullptr;
    QPropertyAnimation *mFade = nullptr;
    int mCount = 0;
    ArrowDirection mDirection = ArrowDirection::Down;
};
