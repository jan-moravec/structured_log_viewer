#pragma once

#include <QStyledItemDelegate>

class QAbstractItemModel;
class ThemeControl;

/// Paints the level column as an icon-only pill when the active
/// theme opts into icon mode. Cell text (`DisplayRole`) is left
/// undrawn but kept on the model so copy and Find still see the
/// raw level string.
///
/// Self-gating: every `paint()` consults
/// `LogModel::IsLevelIconModeActive()` and falls through to the
/// base class when icon mode is off or the model/theme are
/// missing. `ThemeControl*` may be null (no-theme test fixtures).
class LevelCellDelegate : public QStyledItemDelegate
{
    Q_OBJECT

public:
    explicit LevelCellDelegate(ThemeControl *theme, QObject *parent = nullptr);

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override;

    /// Icon-only width: small-icon size + pill padding + room for
    /// the header sort chevron (`PM_HeaderMarkSize`). The chevron
    /// budget keeps the indicator visible after the column shrinks
    /// to the icon footprint.
    [[nodiscard]] QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override;

private:
    /// Walks a proxy-model chain to the underlying `LogModel`.
    /// Returns `nullptr` when the chain root is something else
    /// (e.g. a test stub).
    [[nodiscard]] const class LogModel *ResolveLogModel(const QAbstractItemModel *model) const noexcept;

    /// Non-owning. May be null.
    ThemeControl *mTheme = nullptr;
};
