#include "advanced_filter_editor.hpp"

#include "advanced_filter_highlighter.hpp"
#include "theme_control.hpp"

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
#include <QRegularExpression>
#include <QString>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QTextEdit>
#include <QVBoxLayout>

#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <variant>

namespace
{

/// Initial dialog geometry, sized so the help table and a typical
/// query fit on one line without wrapping.
constexpr int DIALOG_INITIAL_WIDTH_PX = 720;
constexpr int DIALOG_INITIAL_HEIGHT_PX = 380;
/// Floor below which the help table starts wrapping.
constexpr int DIALOG_MINIMUM_WIDTH_PX = 480;
constexpr int DIALOG_MINIMUM_HEIGHT_PX = 300;

/// Pretty-print @p expression. Match-all renders as the empty
/// string, which the caller shows as an empty text field.
[[nodiscard]] QString ExpressionToQueryText(const loglib::FilterExpression &expression)
{
    return QString::fromStdString(loglib::FormatExpression(expression));
}

/// Palette-aware error tint, legible on light and dark themes.
/// Matches `filter_editor.cpp`'s local `WarningColorHex`.
[[nodiscard]] QString ErrorColorHex(const QWidget *widget)
{
    const QPalette palette = (widget != nullptr) ? widget->palette() : QPalette{};
    const bool dark = ThemeControl::IsDarkColor(palette.color(QPalette::Base));
    return dark ? QStringLiteral("#FF8A80") : QStringLiteral("#D32F2F");
}

/// The first invalid regex found in an expression tree: pattern
/// text (for the status message) and Qt's error text.
struct RegexIssue
{
    QString pattern;
    QString errorText;
};

/// Return the first regex `Leaf` whose pattern fails
/// `QRegularExpression::isValid()`, or `nullopt` when all compile.
/// Short-circuits so the status message pins one actionable
/// pattern rather than a summary list. Wildcard leaves are always
/// valid (they go through `wildcardToRegularExpression`).
///
/// clang-tidy misc-no-recursion fires on the visitor lambdas even
/// though the recursion is intentional (walking the boolean AST);
/// silenced with a NOLINT on the entry point rather than on each
/// synthesised visitor specialisation.
// NOLINTNEXTLINE(misc-no-recursion)
[[nodiscard]] std::optional<RegexIssue> FindInvalidRegex(const loglib::FilterExpression &expression)
{
    return std::visit(
        // NOLINTNEXTLINE(misc-no-recursion)
        [](const auto &node) -> std::optional<RegexIssue> {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, loglib::FilterExpression::Leaf>)
            {
                const auto &rule = node.rule;
                if (rule.type != loglib::LeafRule::Type::String ||
                    rule.matchType != loglib::LeafRule::Match::RegularExpression || !rule.filterString.has_value())
                {
                    return std::nullopt;
                }
                const QString pattern = QString::fromStdString(*rule.filterString);
                const QRegularExpression probe(pattern);
                if (probe.isValid())
                {
                    return std::nullopt;
                }
                return RegexIssue{.pattern = pattern, .errorText = probe.errorString()};
            }
            else if constexpr (
                std::is_same_v<T, loglib::FilterExpression::And> || std::is_same_v<T, loglib::FilterExpression::Or>
            )
            {
                for (const auto &child : node.children)
                {
                    if (auto issue = FindInvalidRegex(child); issue.has_value())
                    {
                        return issue;
                    }
                }
                return std::nullopt;
            }
            else
            {
                // Not.
                if (node.child == nullptr)
                {
                    return std::nullopt;
                }
                return FindInvalidRegex(*node.child);
            }
        },
        expression.node
    );
}

} // namespace

AdvancedFilterEditor::AdvancedFilterEditor(QWidget *parent)
    : QDialog(parent)
{
    setObjectName(QStringLiteral("advancedFilterEditor"));
    setWindowTitle(tr("Advanced Filter"));
    // Explicit size + floor: `sizeHint` alone wraps every help-table row.
    resize(DIALOG_INITIAL_WIDTH_PX, DIALOG_INITIAL_HEIGHT_PX);
    setMinimumSize(DIALOG_MINIMUM_WIDTH_PX, DIALOG_MINIMUM_HEIGHT_PX);
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
    // Monospace: operators / brackets line up predictably.
    QFont mono(QStringLiteral("Consolas"));
    mono.setStyleHint(QFont::Monospace);
    mQueryEdit->setFont(mono);
    // Four visible lines so long queries wrap into a readable block.
    const int approxRowHeight = QFontMetrics(mQueryEdit->font()).lineSpacing();
    mQueryEdit->setMinimumHeight(approxRowHeight * 4);
    // Live syntax highlighting; parented to the document, Qt owns it.
    new AdvancedFilterHighlighter(mQueryEdit->document());
    layout->addWidget(mQueryEdit);

    // Concise operator cheat-sheet. The literal is inlined (not
    // hoisted into a constexpr) so `lupdate` can extract it for
    // translation. Two-column table keeps the italic gloss
    // vertically aligned regardless of example width.
    mHelpLabel = new QLabel(
        tr("Combine <b>leaves</b> (column, operator, value &mdash; see below) with "
           "<b>AND</b> / <b>OR</b> / <b>NOT</b> / <b>IN</b> (case-insensitive) and parentheses. "
           "Leave empty to match every row."
           "<table cellspacing='0' cellpadding='2'>"
           "<tr><td><code><b>service</b>:auth</code></td>"
           "<td><i>substring match (contains)</i></td></tr>"
           "<tr><td><code><b>service</b>=\"auth-svc\"</code></td>"
           "<td><i>exact match (quoted for spaces)</i></td></tr>"
           "<tr><td><code><b>msg</b> ~ /err(or)?/</code></td>"
           "<td><i>regular expression</i></td></tr>"
           "<tr><td><code><b>path</b> % \"*.log\"</code></td>"
           "<td><i>wildcard glob (<code>*</code> <code>?</code>)</i></td></tr>"
           "<tr><td><code><b>latency</b> &gt; 100</code></td>"
           "<td><i>numeric or ISO-time compare (=, &gt;, &gt;=, &lt;, &lt;=)</i></td></tr>"
           "<tr><td><code><b>level</b> IN [Warn, Error]</code></td>"
           "<td><i>enum multi-select</i></td></tr>"
           "<tr><td><code><b>latency</b> IN [10..100]</code></td>"
           "<td><i>numeric or ISO-time range</i></td></tr>"
           "</table>"),
        this
    );
    mHelpLabel->setObjectName(QStringLiteral("advancedFilterHelpLabel"));
    mHelpLabel->setTextFormat(Qt::RichText);
    mHelpLabel->setWordWrap(true);
    mHelpLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    // Dim so the help reads as tertiary content.
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
        // Second-stage regex validity check: `ParseQuery` extracts
        // the `/.../` body verbatim, so a valid query can still
        // carry a broken pattern that would silently reject every
        // row. Gate OK on it and name the offending pattern.
        if (auto regexIssue = FindInvalidRegex(*parsed); regexIssue.has_value())
        {
            mCachedResult.reset();
            mStatusLabel->setText(
                tr("Invalid regular expression /%1/: %2").arg(regexIssue->pattern, regexIssue->errorText)
            );
            mStatusLabel->setStyleSheet(QStringLiteral("color: %1;").arg(ErrorColorHex(this)));
            // Query parsed, so any stale syntax-error underline
            // would point at the wrong spot -- clear it.
            ClearErrorHighlight();
            mOkButton->setEnabled(false);
            return;
        }
        mCachedResult = std::move(*parsed);
        if (loglib::IsMatchAll(*mCachedResult))
        {
            mStatusLabel->setText(tr("Empty query \u2014 matches every row."));
        }
        else
        {
            // Show the canonical form only when it differs from the
            // typed text (whitespace-insensitive), so the user sees
            // what will be saved when casing/spacing normalises.
            const std::string formatted = loglib::FormatExpression(*mCachedResult);
            const QString canonical = QString::fromStdString(formatted);
            const QString typedTrimmed = queryText.simplified();
            const QString canonicalTrimmed = canonical.simplified();
            if (typedTrimmed == canonicalTrimmed)
            {
                mStatusLabel->setText(tr("Parsed OK."));
            }
            else
            {
                mStatusLabel->setText(tr("Parsed OK \u2014 will save as: %1").arg(canonical));
            }
        }
        mStatusLabel->setStyleSheet(QString());
        ClearErrorHighlight();
        mOkButton->setEnabled(true);
    }
    else
    {
        mCachedResult.reset();
        const auto &err = parsed.error();
        // Display column is 1-based; the raw offset is 0-based and
        // stays 0-based for the underline call below.
        const auto displayOffset = static_cast<qsizetype>(err.offset) + 1;
        mStatusLabel->setText(
            tr("Parse error at position %1: %2").arg(displayOffset).arg(QString::fromStdString(err.message))
        );
        mStatusLabel->setStyleSheet(QStringLiteral("color: %1;").arg(ErrorColorHex(this)));
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
    // Byte-offset -> UTF-16 code-unit index for QTextCursor.
    // Walking the prefix is fine: queries are short.
    const QByteArray utf8 = queryText.toUtf8();
    const auto clampedByteOffset = std::min(byteOffset, static_cast<std::size_t>(utf8.size()));
    // Query text is UI-bounded (a single dialog input); narrowing
    // the code-unit count to `int` is safe and matches Qt's
    // `QTextCursor::setPosition` argument type.
    const int codeUnitOffset =
        static_cast<int>(QString::fromUtf8(utf8.constData(), static_cast<qsizetype>(clampedByteOffset)).size());
    const int textSize = static_cast<int>(queryText.size());
    const int selectStart = std::min(codeUnitOffset, textSize);
    // For a caret at EOF, back up one glyph so at least one
    // character is underlined and visible.
    QTextEdit::ExtraSelection selection;
    QTextCursor cursor(mQueryEdit->document());
    if (selectStart >= textSize && textSize > 0)
    {
        cursor.setPosition(textSize);
        // `PreviousCharacter` walks by grapheme, so surrogate pairs
        // and combining marks stay intact.
        cursor.movePosition(QTextCursor::PreviousCharacter, QTextCursor::KeepAnchor);
    }
    else if (textSize > 0)
    {
        cursor.setPosition(selectStart);
        // Grapheme-aware advance: avoids splitting surrogate pairs
        // on supplementary-plane characters.
        cursor.movePosition(QTextCursor::NextCharacter, QTextCursor::KeepAnchor);
    }
    else
    {
        // Empty text: nothing to underline. Status label carries the cue.
        ClearErrorHighlight();
        return;
    }
    QTextCharFormat fmt;
    fmt.setUnderlineStyle(QTextCharFormat::WaveUnderline);
    fmt.setUnderlineColor(QColor(ErrorColorHex(this)));
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
