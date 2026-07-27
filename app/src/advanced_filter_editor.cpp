#include "advanced_filter_editor.hpp"

#include <loglib/filter_expression.hpp>
#include <loglib/query_parser.hpp>

#include <QDialogButtonBox>
#include <QFont>
#include <QFontMetrics>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QString>
#include <QVBoxLayout>

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
    const std::string source = mQueryEdit->toPlainText().toStdString();
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
        mOkButton->setEnabled(false);
    }
}
