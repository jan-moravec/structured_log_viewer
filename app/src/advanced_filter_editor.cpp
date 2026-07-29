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

/// Initial dialog geometry. Wider than Qt's default `sizeHint` so
/// the help table plus a realistic query -- e.g. a full ISO
/// timestamp range or an `OR` chain of several leaves -- fit on
/// one line without wrapping. Matches the width used by the
/// sibling `ConfigurationDiagnosticsDialog`.
constexpr int DIALOG_INITIAL_WIDTH_PX = 720;
constexpr int DIALOG_INITIAL_HEIGHT_PX = 380;
/// Prevent the user from shrinking the dialog below the point
/// where the help table starts wrapping. The value keeps the
/// widest table row (`latency > 100` + its gloss) on a single
/// line while still allowing a reasonably compact layout on
/// small displays.
constexpr int DIALOG_MINIMUM_WIDTH_PX = 480;
constexpr int DIALOG_MINIMUM_HEIGHT_PX = 300;

/// `FormatExpression` renders match-all as the empty string
/// already, so this is currently equivalent to a direct call --
/// keep the wrapper for readability and to pin the dialog's
/// "empty field means match all" invariant if the pretty printer
/// ever changes.
[[nodiscard]] QString ExpressionToQueryText(const loglib::FilterExpression &expression)
{
    return QString::fromStdString(loglib::FormatExpression(expression));
}

/// Palette-aware error colour, legible on both light and dark
/// backgrounds. Mirrors `filter_editor.cpp`'s local
/// `WarningColorHex` so the two dialogs share their validation
/// vocabulary. `palette(highlight)` (the previous choice) reads as
/// a hyperlink on many themes because it's the selection colour,
/// not a semantic error tint.
[[nodiscard]] QString ErrorColorHex(const QWidget *widget)
{
    const QPalette palette = (widget != nullptr) ? widget->palette() : QPalette{};
    const bool dark = ThemeControl::IsDarkColor(palette.color(QPalette::Base));
    return dark ? QStringLiteral("#FF8A80") : QStringLiteral("#D32F2F");
}

/// Diagnostic returned by `FindInvalidRegex`: the offending pattern
/// (for the status message) and the QRegularExpression error text.
struct RegexIssue
{
    QString pattern;
    QString errorText;
};

/// Depth-first walk over @p expression looking for the first
/// `Type::String` leaf whose match kind is `RegularExpression` and
/// whose pattern fails `QRegularExpression::isValid()`. Wildcard
/// patterns are always accepted (`wildcardToRegularExpression`
/// escapes special characters). Returns `nullopt` when every regex
/// leaf compiles; short-circuits on the first failure so the status
/// message pins one actionable pattern instead of a summary list.
[[nodiscard]] std::optional<RegexIssue> FindInvalidRegex(const loglib::FilterExpression &expression)
{
    return std::visit(
        [](const auto &node) -> std::optional<RegexIssue> {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, loglib::FilterExpression::Leaf>)
            {
                const auto &rule = node.rule;
                if (rule.type != loglib::LeafRule::Type::String ||
                    rule.matchType != loglib::LeafRule::Match::RegularExpression ||
                    !rule.filterString.has_value())
                {
                    return std::nullopt;
                }
                const QString pattern = QString::fromStdString(*rule.filterString);
                const QRegularExpression probe(pattern);
                if (probe.isValid())
                {
                    return std::nullopt;
                }
                return RegexIssue{pattern, probe.errorString()};
            }
            else if constexpr (std::is_same_v<T, loglib::FilterExpression::And> ||
                               std::is_same_v<T, loglib::FilterExpression::Or>)
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

AdvancedFilterEditor::AdvancedFilterEditor(QWidget *parent) : QDialog(parent)
{
    setObjectName(QStringLiteral("advancedFilterEditor"));
    setWindowTitle(tr("Advanced Filter"));
    // Explicit initial size + floor so the help table and typical
    // queries fit without immediate wrapping. Without this the
    // dialog defaulted to `sizeHint`, which packed the tallest
    // widget (the four-row query editor) tightly and produced a
    // ~370 px wide window that wrapped every help-table row.
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
    // Monospace so operators / brackets line up predictably; matches
    // how users see the same syntax in the record-detail dock.
    QFont mono(QStringLiteral("Consolas"));
    mono.setStyleHint(QFont::Monospace);
    mQueryEdit->setFont(mono);
    // Leave the editor a bit taller than one line so long queries
    // wrap into a readable block instead of scrolling horizontally.
    const int approxRowHeight = QFontMetrics(mQueryEdit->font()).lineSpacing();
    mQueryEdit->setMinimumHeight(approxRowHeight * 4);
    // Live syntax highlighting -- keywords, operator punctuation,
    // and quoted / regex literals get their own formats so the
    // user can tell a keyword from a column name at a glance.
    // Parented to the document; Qt manages the lifetime.
    new AdvancedFilterHighlighter(mQueryEdit->document());
    layout->addWidget(mQueryEdit);

    // Concise operator summary so the user doesn't have to leave the
    // dialog. Spelled out inline rather than hoisted into a
    // `constexpr` string: `lupdate` only extracts `tr()` arguments
    // that are literals, so `tr(HELP_TEXT)` compiled fine but left
    // the block permanently untranslatable. Uses Qt rich text --
    // `QLabel` auto-detects the HTML tags and renders bold /
    // monospace / italic accordingly.
    //
    // Two-column HTML table so the italic gloss aligns vertically
    // regardless of how wide each concrete example is (the previous
    // inline `<br>` layout let each row's gloss start at a
    // different horizontal position, which read as "all over the
    // place"). The example column is monospace so operators /
    // brackets align; the gloss column is italic so the reader can
    // tell "what the operator does" from "what you actually type".
    // The intro / grammar sentence sits *above* the table -- a
    // single line that answers "what am I looking at?" (leaves
    // combined by boolean connectives) and "what happens if I
    // leave it blank?", so the reader gets one paragraph of
    // context, then a scannable reference table underneath.
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
        // Regex validity is a second-stage check: `ParseQuery` only
        // extracts the pattern between `/.../` delimiters, so a
        // syntactically valid query can still carry an invalid
        // `QRegularExpression` payload. Without this gate an
        // accepted expression like `msg~/*[bad/` compiles into a
        // matcher that silently rejects every row -- the log view
        // goes blank with no cue. Surface the failure here so the
        // OK button stays disabled and the status label explains
        // which pattern to fix.
        if (auto regexIssue = FindInvalidRegex(*parsed); regexIssue.has_value())
        {
            mCachedResult.reset();
            mStatusLabel->setText(
                tr("Invalid regular expression /%1/: %2").arg(regexIssue->pattern, regexIssue->errorText)
            );
            mStatusLabel->setStyleSheet(QStringLiteral("color: %1;").arg(ErrorColorHex(this)));
            // No caret offset is available for individual regex leaves
            // (the parser doesn't record per-leaf source positions),
            // but the query *did* parse, so any underline still on
            // screen belongs to a previous, now-fixed syntax error and
            // would point the user at the wrong character. Clear it and
            // let the status label name the offending pattern.
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
            // Only surface the canonical form when it differs from
            // what the user typed (whitespace-insensitive) -- the
            // previous "Parsed OK: <same text>" echo was a redundant
            // repetition of the editor's contents. When casing /
            // spacing normalises (e.g. `level in [warn]` -> `level
            // IN [Warn]`), show the delta so the user can see what
            // will actually be saved.
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
        // just typed (position 1 for the first character). The raw
        // byte offset is 0-based; `+ 1` shifts it to the display
        // form. `HighlightErrorAt` below still consumes the raw
        // 0-based offset, so the underline stays on the exact
        // offending byte.
        const auto displayOffset = static_cast<qsizetype>(err.offset) + 1;
        mStatusLabel->setText(
            tr("Parse error at position %1: %2").arg(displayOffset).arg(QString::fromStdString(err.message))
        );
        mStatusLabel->setStyleSheet(QStringLiteral("color: %1;").arg(ErrorColorHex(this)));
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
    // Use the same palette-aware error tint as the status label
    // (matches `filter_editor`'s validation vocabulary). The
    // previous `QPalette::Highlight` picked up the selection colour
    // -- typically a bright blue that reads as a hyperlink rather
    // than a validation error on most themes.
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
