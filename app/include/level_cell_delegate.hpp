#pragma once

#include <QStyledItemDelegate>

class QAbstractItemModel;
class ThemeControl;

/// Custom item delegate for the level column when the active theme
/// opts into icon mode (`Theme::levelColumnOverride.has_value()`).
/// Renders the cell as a rounded-rect "pill" filled with the
/// theme's `pillBackground` and a centred `DecorationRole` icon;
/// the cell text (`DisplayRole`) is intentionally not drawn so the
/// glyph reads as the only signal.
///
/// `DisplayRole` is preserved on the model so copy-to-clipboard
/// and the find-bar substring scan still hit the raw level
/// string; the delegate is the only place that suppresses the
/// on-screen text without dropping the role itself.
///
/// Self-gating: `paint()` consults `LogModel::IsLevelIconModeActive()`
/// (chasing through proxy models) on every call. When the model
/// reports icon mode off -- because the user toggled
/// `ui/showLevelIcons`, or because the active theme has no
/// override -- the delegate forwards to
/// `QStyledItemDelegate::paint(...)` so the standard text
/// rendering keeps working. This makes the toggle-order between
/// `setItemDelegateForColumn(...)` and `SetShowLevelIcons(bool)`
/// in `MainWindow` an optimisation, not a correctness
/// requirement: even if the model flag flips before the delegate
/// detach, the next paint falls through to the base class.
///
/// `ThemeControl*` is taken by raw pointer because the delegate
/// is owned by `MainWindow` and its lifetime is bounded by the
/// `ThemeControl` it observes. Passing `nullptr` is supported:
/// `IsLevelIconModeActive()` returns false on the model side when
/// the theme is absent, so the delegate falls straight through to
/// the base class on every paint.
class LevelCellDelegate : public QStyledItemDelegate
{
    Q_OBJECT

public:
    explicit LevelCellDelegate(ThemeControl *theme, QObject *parent = nullptr);

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override;

    /// Narrow square cell hint: enough room for a `PM_SmallIconSize`
    /// glyph plus padding plus the header sort-indicator width
    /// (`PM_HeaderMarkSize`). Without the mark budget, sorting by
    /// the level column would crop Qt's sort chevron once the
    /// delegate's icon-only width shrinks the column below the
    /// chevron's needs.
    [[nodiscard]] QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override;

private:
    /// Walks `QSortFilterProxyModel`/`QAbstractProxyModel` chains
    /// to find the underlying `LogModel`. Returns nullptr when
    /// the chain root isn't a `LogModel` (e.g. test fixtures with
    /// a stub model); the delegate then falls through to the base
    /// class so the cell still paints text.
    [[nodiscard]] const class LogModel *ResolveLogModel(const QAbstractItemModel *model) const noexcept;

    /// Non-owning. May be null (see class doc).
    ThemeControl *mTheme = nullptr;
};
