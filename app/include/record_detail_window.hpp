#pragma once

#include "record_detail_widget.hpp"

#include <QWidget>

/// Top-level snapshot window for one log record. Holds a frozen
/// `RecordDetailContent`; the widget never re-reads from the model,
/// so the snapshot survives FIFO eviction, `modelReset`, and even
/// the model itself being destroyed. Multiple windows can coexist
/// for side-by-side comparison. `Qt::WA_DeleteOnClose`; owners hold
/// `QPointer<RecordDetailWindow>` for self-cleaning lists.
class RecordDetailWindow : public QWidget
{
    Q_OBJECT

public:
    /// Construct a frozen snapshot view. The "Open in new window"
    /// button is hidden -- this *is* a new window.
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
