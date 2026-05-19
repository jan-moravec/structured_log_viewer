#pragma once

#include "record_detail_widget.hpp"

#include <QWidget>

/// Top-level snapshot window for a single log record. Holds a
/// `RecordDetailWidget` with a frozen `RecordDetailContent`; the
/// widget never re-reads from the model after construction, so the
/// content survives FIFO eviction and `modelReset` (and even the
/// `LogModel` itself being destroyed).
///
/// Multiple windows can coexist for record-by-record comparison.
/// Each window uses `Qt::WA_DeleteOnClose`; owners store
/// `QPointer<RecordDetailWindow>` so the list self-cleans.
class RecordDetailWindow : public QWidget
{
    Q_OBJECT

public:
    /// Construct a frozen snapshot view. The "Open in new window"
    /// button is hidden -- you are already in a new window.
    explicit RecordDetailWindow(const RecordDetailContent &content, QWidget *parent = nullptr);

#ifdef LOGAPP_BUILD_TESTING
    [[nodiscard]] RecordDetailWidget *WidgetForTest() const noexcept
    {
        return mWidget;
    }
#endif

private:
    RecordDetailWidget *mWidget = nullptr;
};
