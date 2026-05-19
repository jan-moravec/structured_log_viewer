#pragma once

#include <QList>
#include <QPair>
#include <QString>
#include <QWidget>

class LogModel;
class QGroupBox;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QResizeEvent;
class QTableWidget;

/// Plain data carried by a single record-detail view: the row summary,
/// the (header, formatted value) pairs for every configured column,
/// the original on-disk bytes (`rawJson`) and a pretty-printed
/// rendering of those bytes (`formattedJson`). `valid == false` means
/// the detail view is a placeholder (no row selected, evicted by
/// retention, etc.) and `placeholderText` is the explanation.
///
/// `rawJson` is what the "Copy raw JSON" button writes to the
/// clipboard -- exactly the byte sequence the parser ingested. The
/// QPlainTextEdit in the widget displays `formattedJson` so users
/// can read deeply-nested objects; users wanting to copy that
/// formatted view can select+Ctrl+C inside the edit directly.
///
/// The struct deliberately owns all its strings so a snapshot copy
/// captured at one point in time keeps rendering after the underlying
/// `LogModel` mutates (FIFO eviction, modelReset, ...).
struct RecordDetailContent
{
    QString summary;
    QList<QPair<QString, QString>> fields;
    QString rawJson;
    QString formattedJson;
    bool valid = false;
    QString placeholderText;
};

/// Snapshot the record at @p sourceRow of @p model into a
/// `RecordDetailContent`. Out-of-range or otherwise unresolvable rows
/// produce an invalid content with `placeholderText` set.
[[nodiscard]] RecordDetailContent BuildRecordDetailContent(const LogModel &model, int sourceRow);

/// Default placeholder shown when no row is pinned. Single source of
/// truth so the dock, the snapshot window, and `BuildRecordDetailContent`
/// agree -- otherwise the three call sites drift and translators see
/// the same sentence three times.
[[nodiscard]] QString DefaultRecordDetailPlaceholder();

/// Reusable display widget for one log record. Renders a
/// `RecordDetailContent` as a header summary, a two-column key/value
/// table, and a collapsible pretty-printed raw-JSON section.
///
/// The widget itself does not know about the `LogModel`: the dockable
/// pane refreshes content on selection change, the pop-out window
/// holds a frozen snapshot, and tests can drive the widget with a
/// hand-built `RecordDetailContent` without ever opening a file.
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

#ifdef LOGAPP_BUILD_TESTING
    /// Test-only direct accessors. `findChild<>` lookups by object
    /// name are unreliable on the GitHub-hosted Linux runner with
    /// Qt 6.8 + offscreen QPA (same workaround as `ColumnEditor`).
    [[nodiscard]] QTableWidget *FieldsTableForTest() const noexcept
    {
        return mFieldsTable;
    }
    [[nodiscard]] QPlainTextEdit *RawEditForTest() const noexcept
    {
        return mRawEdit;
    }
    [[nodiscard]] QPushButton *CopyJsonButtonForTest() const noexcept
    {
        return mCopyJsonButton;
    }
    [[nodiscard]] QPushButton *CopyKeyValueButtonForTest() const noexcept
    {
        return mCopyKvButton;
    }
    [[nodiscard]] QPushButton *OpenInNewWindowButtonForTest() const noexcept
    {
        return mOpenInNewWindowButton;
    }
#endif

signals:
    /// User clicked "Open in new window". The owning widget (dock)
    /// builds a frozen snapshot and shows a top-level
    /// `RecordDetailWindow`.
    void openInNewWindowRequested();

protected:
    /// Re-flow row heights when our value column resizes. Word-wrap
    /// is on so the wrapped height of a long single-line value
    /// depends on the available column width; recomputing on resize
    /// keeps long messages from being clipped to a single row.
    void resizeEvent(QResizeEvent *event) override;

private slots:
    void CopyAsJsonClicked() const;
    void CopyAsKeyValueClicked() const;
    /// Ctrl+C inside the fields table. Writes the selected cells
    /// to the clipboard as TSV (one row per line, tab between
    /// key/value columns), reading the underlying field text so
    /// the `<empty>` decoration isn't copied verbatim.
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
};
