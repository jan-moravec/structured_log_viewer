#pragma once

#include <QList>
#include <QPair>
#include <QString>
#include <QWidget>
#include <Qt>

class LogModel;
class QGroupBox;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QResizeEvent;
class QTableWidget;
class QTimer;

/// Item-data role flagging the muted em-dash "empty value"
/// placeholder so consumers can tell a synthetic placeholder from a
/// real value that happens to be an em-dash. Exported so tests don't
/// hand-spell `Qt::UserRole + 1`.
constexpr int RECORD_DETAIL_EMPTY_PLACEHOLDER_ROLE = Qt::UserRole + 1;

/// Data for one record-detail view. `valid == false` means the view
/// shows `placeholderText` instead.
///
/// `rawJson` is the on-disk line bytes (UTF-8 decoded) -- "Copy raw
/// JSON" pushes this verbatim. `formattedJson` is the pretty-printed
/// rendering shown in the widget.
///
/// All strings are owned, so a snapshot keeps rendering after the
/// underlying `LogModel` mutates or goes away.
struct RecordDetailContent
{
    QString summary;
    QList<QPair<QString, QString>> fields;
    QString rawJson;
    QString formattedJson;
    bool valid = false;
    QString placeholderText;
};

/// Snapshot @p sourceRow of @p model. Out-of-range rows produce an
/// invalid content with `placeholderText` set.
[[nodiscard]] RecordDetailContent BuildRecordDetailContent(const LogModel &model, int sourceRow);

/// Default "select a row" placeholder. Single source of truth so the
/// three call sites (dock, snapshot window, builder) stay in sync.
[[nodiscard]] QString DefaultRecordDetailPlaceholder();

/// Placeholder for a pinned record that has been evicted from the
/// model (streaming FIFO). Distinct from the default placeholder so
/// the user can tell "never picked anything" from "what I picked is
/// gone".
[[nodiscard]] QString EvictedRecordPlaceholder();

/// Renders one `RecordDetailContent` as a summary label, a key/value
/// table, and a collapsible raw-JSON section. The widget itself knows
/// nothing about `LogModel`: the dock refreshes it on selection
/// change, snapshot windows feed it a frozen content, and tests can
/// drive it with a hand-built one.
class RecordDetailWidget : public QWidget
{
    Q_OBJECT

public:
    explicit RecordDetailWidget(QWidget *parent = nullptr);

    /// Replace the displayed content. Idempotent.
    void SetContent(const RecordDetailContent &content);

    [[nodiscard]] const RecordDetailContent &Content() const noexcept
    {
        return mContent;
    }

    /// Show / hide the "Open in new window" button. Defaults to
    /// visible; snapshot windows hide it.
    void SetOpenInNewWindowVisible(bool visible);

    /// Re-render the displayed content against the current
    /// palette. Cell brushes are snapshotted at `SetContent` time
    /// (placeholder rows stash `QPalette::PlaceholderText` on the
    /// item), so theme changes need this nudge. Idempotent.
    void RefreshPalette();

signals:
    /// User clicked "Open in new window". The owner builds a snapshot
    /// `RecordDetailWindow`.
    void openInNewWindowRequested();

protected:
    /// Reflow wrapped row heights when the value column's width
    /// changes; otherwise long messages clip to one row.
    void resizeEvent(QResizeEvent *event) override;

private slots:
    void CopyAsJsonClicked() const;
    void CopyAsKeyValueClicked() const;
    /// Ctrl+C inside the fields table. Writes selected cells as TSV,
    /// reading the underlying field text so the em-dash placeholder
    /// for empty values isn't copied verbatim.
    void CopyFieldsSelectionToClipboard() const;

private:
    void PopulateUi();

    RecordDetailContent mContent;
    QLabel *mSummaryLabel = nullptr;
    QLabel *mPlaceholderLabel = nullptr;
    QTableWidget *mFieldsTable = nullptr;
    QGroupBox *mRawGroup = nullptr;
    QPlainTextEdit *mRawEdit = nullptr;
    QPushButton *mCopyJsonButton = nullptr;
    QPushButton *mCopyKvButton = nullptr;
    QPushButton *mOpenInNewWindowButton = nullptr;

    /// User's preferred expand/collapse state, kept separate from
    /// `mRawGroup->isChecked()` so a record with no raw bytes (forced
    /// collapsed) doesn't overwrite the user's intent. Restored on
    /// the next record that has raw bytes. `mSuppressRawToggleHandler`
    /// gates programmatic toggles so they don't update the preference.
    bool mUserPrefersRawExpanded = false;
    bool mSuppressRawToggleHandler = false;

    /// Debounces `resizeRowsToContents` (O(rows * cells)) during a
    /// horizontal drag, which can emit dozens of resize events per
    /// frame. Single-shot per resize burst.
    QTimer *mResizeReflowTimer = nullptr;
};
