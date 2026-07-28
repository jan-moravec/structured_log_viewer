#include "advanced_filter_editor.hpp"

#include <loglib/filter_expression.hpp>
#include <loglib/query_parser.hpp>

#include <QColor>
#include <QDialogButtonBox>
#include <QFont>
#include <QFontMetrics>
#include <QLabel>
#include <QList>
#include <QPalette>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QString>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QTextEdit>
#include <QVBoxLayout>

#include <cstddef>
#include <string>
#include <utility>

namespace
{

/// Concise one-line hint below the query field. Shows the operator
/// summary without leaving the dialog.
constexpr auto HELP_TEXT =
    "Operators: col:contains, col=\"exact\", col~/regex/, col%\"wild\", col>N, col<=N,\n"
    "col in [a,b,c] (enum) or col in [min..max] (numeric / ISO time range).\n"
    "Combine with AND / OR / NOT and parentheses. Leave empty to match all rows.";

/// Empty tree = "match all". `FormatExpression` renders this as
/// `*`, but the editor wants a truly empty text field so the user
/// starts on a clean slate rather than seeing a placeholder token.
[[nodiscard]] QString ExpressionToQueryText(const loglib::FilterExpression &expression)
{
    if (loglib::IsMatchAll(expression))
    {
        return {};
    }
    return QString::fromStdString(loglib::FormatExpression(expression));
}

} // namespace

AdvancedFilterEditor::AdvancedFilterEditor(QWidget *parent) : QDialog(parent)
{
    setObjectName(QStringLiteral("advancedFilterEditor"));
    setWindowTitle(tr("Advanced Filter"));
    SetupLayout();
    ReparseAndUpdate();
}

void AdvancedFilterEditor::SetupLayout()
{
    auto *layout = new QVBoxLayout(this);

    auto *editorLabel = new QLabel(tr("Filter query"), this);
    editorLabel->setObjectName(QStringLiteral("advancedFilterEditorLabel"));
    layout->addWidget(editorLabel);

    mQueryEdit = new QPlainTextEdit(this);
    mQueryEdit->setObjectName(QStringLiteral("advancedFilterQueryEdit"));
    // Monospace so operators / brackets line up predictably; matches
    // how users see the same syntax in the record-detail dock.
    QFont mono(QStringLiteral("Consolas"));
    mono.setStyleHint(QFont::Monospace);
    mQueryEdit->setFont(mono);
    // Leave the editor a bit taller than one line so long queries
    // wrap into a readable block instead of scrolling horizontally.
    const int approxRowHeight = QFontMetrics(mQueryEdit->font()).lineSpacing();
    mQueryEdit->setMinimumHeight(approxRowHeight * 4);
    layout->addWidget(mQueryEdit);

    mHelpLabel = new QLabel(tr(HELP_TEXT), this);
    mHelpLabel->setObjectName(QStringLiteral("advancedFilterHelpLabel"));
    mHelpLabel->setWordWrap(true);
    mHelpLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    // Dim slightly so the help block reads as tertiary content.
    QPalette dimPalette = mHelpLabel->palette();
    dimPalette.setColor(QPalette::WindowText, dimPalette.color(QPalette::Disabled, QPalette::WindowText));
    mHelpLabel->setPalette(dimPalette);
    layout->addWidget(mHelpLabel);

    mStatusLabel = new QLabel(this);
    mStatusLabel->setObjectName(QStringLiteral("advancedFilterStatusLabel"));
    mStatusLabel->setWordWrap(true);
    layout->addWidget(mStatusLabel);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->setObjectName(QStringLiteral("advancedFilterButtonBox"));
    mOkButton = buttons->button(QDialogButtonBox::Ok);
    mCancelButton = buttons->button(QDialogButtonBox::Cancel);
    layout->addWidget(buttons);

    connect(mQueryEdit, &QPlainTextEdit::textChanged, this, &AdvancedFilterEditor::ReparseAndUpdate);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

void AdvancedFilterEditor::LoadFromExpression(const loglib::FilterExpression &expression)
{
    SetQueryText(ExpressionToQueryText(expression));
}

void AdvancedFilterEditor::SetQueryText(const QString &text)
{
    mQueryEdit->setPlainText(text);
}

QString AdvancedFilterEditor::QueryText() const
{
    return mQueryEdit->toPlainText();
}

std::optional<loglib::FilterExpression> AdvancedFilterEditor::Result() const
{
    return mCachedResult;
}

void AdvancedFilterEditor::ReparseAndUpdate()
{
    const QString queryText = mQueryEdit->toPlainText();
    const std::string source = queryText.toStdString();
    auto parsed = loglib::ParseQuery(source);
    if (parsed.has_value())
    {
        mCachedResult = std::move(*parsed);
        if (loglib::IsMatchAll(*mCachedResult))
        {
            mStatusLabel->setText(tr("Empty query \u2014 matches every row."));
        }
        else
        {
            const std::string formatted = loglib::FormatExpression(*mCachedResult);
            mStatusLabel->setText(tr("Parsed OK: %1").arg(QString::fromStdString(formatted)));
        }
        // Reset any warning styling from prior parse errors.
        mStatusLabel->setStyleSheet(QString());
        // Drop any prior underline so the query field is clean when
        // the user recovers.
        ClearErrorHighlight();
        mOkButton->setEnabled(true);
    }
    else
    {
        mCachedResult.reset();
        const auto &err = parsed.error();
        // Report the caret offset in 1-based column form so it
        // aligns with the user's mental model of the text they
        // just typed. The absolute byte offset stays visible for
        // longer expressions where line-column would drift.
        const auto offset = static_cast<qsizetype>(err.offset);
        mStatusLabel->setText(
            tr("Parse error at position %1: %2").arg(offset).arg(QString::fromStdString(err.message))
        );
        mStatusLabel->setStyleSheet(QStringLiteral("color: palette(highlight);"));
        // Convert the byte offset into a UTF-16 code-unit offset so
        // Qt's `QTextCursor` (which counts code units) lands on the
        // right character even when earlier bytes were multi-byte
        // UTF-8. Clamp to the current text length so trailing-EOF
        // errors highlight the last visible character.
        HighlightErrorAt(queryText, static_cast<std::size_t>(err.offset));
        mOkButton->setEnabled(false);
    }
}

void AdvancedFilterEditor::HighlightErrorAt(const QString &queryText, std::size_t byteOffset)
{
    if (mQueryEdit == nullptr)
    {
        return;
    }
    // Translate byte-offset -> UTF-16 code-unit index by re-encoding
    // the prefix. Fine to walk the string here -- queries are short
    // and this only runs on `textChanged`.
    const QByteArray utf8 = queryText.toUtf8();
    const auto clampedByteOffset = std::min(byteOffset, static_cast<std::size_t>(utf8.size()));
    const int codeUnitOffset =
        QString::fromUtf8(utf8.constData(), static_cast<qsizetype>(clampedByteOffset)).size();
    const int textSize = queryText.size();
    const int selectStart = std::min(codeUnitOffset, textSize);
    // Highlight at least one character so a caret-at-EOF error is
    // still visible. If the error is past end-of-text, back up one
    // character so we underline the last real glyph.
    QTextEdit::ExtraSelection selection;
    QTextCursor cursor(mQueryEdit->document());
    if (selectStart >= textSize && textSize > 0)
    {
        cursor.setPosition(textSize - 1);
        cursor.setPosition(textSize, QTextCursor::KeepAnchor);
    }
    else if (textSize > 0)
    {
        cursor.setPosition(selectStart);
        cursor.setPosition(std::min(selectStart + 1, textSize), QTextCursor::KeepAnchor);
    }
    else
    {
        // Empty text — no visible glyph to underline; leave the
        // status-label offset as the sole cue.
        ClearErrorHighlight();
        return;
    }
    QTextCharFormat fmt;
    fmt.setUnderlineStyle(QTextCharFormat::WaveUnderline);
    // Prefer the palette's highlight colour so the wavy line remains
    // legible on both light and dark themes.
    fmt.setUnderlineColor(mQueryEdit->palette().color(QPalette::Highlight));
    selection.cursor = cursor;
    selection.format = fmt;
    mQueryEdit->setExtraSelections({selection});
}

void AdvancedFilterEditor::ClearErrorHighlight()
{
    if (mQueryEdit == nullptr)
    {
        return;
    }
    mQueryEdit->setExtraSelections({});
}
