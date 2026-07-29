#pragma once

#include <QRegularExpression>
#include <QSyntaxHighlighter>
#include <QTextCharFormat>

#include <vector>

class QTextDocument;

/// Live syntax highlighter for the Advanced Filter query editor.
///
/// Applies four palette-aware categories to `QTextDocument` blocks so
/// the user gets a running "what did I type?" signal while composing:
///   - **Keywords** (`AND` / `OR` / `NOT` / `IN`, whole-word,
///     case-insensitive) render bold + `QPalette::Link` colour so
///     they read as connectives rather than column names.
///   - **Operator punctuation** (`:` `~` `%` `>=` `<=` `>` `<` `=`)
///     renders bold so the column-vs-value boundary is visible even
///     without extra whitespace.
///   - **Quoted string literals** (`"..."`) and **regex literals**
///     (`/.../` following `~`) render italic + `QPalette::
///     PlaceholderText` so the "this text isn't parsed as syntax"
///     regions stand out.
///
/// The wavy parse-error underline installed via
/// `QPlainTextEdit::setExtraSelections` layers on top independently
/// (extra selections and character formats live in disjoint
/// rendering channels), so both cues coexist without either erasing
/// the other.
class AdvancedFilterHighlighter : public QSyntaxHighlighter
{
    Q_OBJECT

public:
    explicit AdvancedFilterHighlighter(QTextDocument *parent);
    ~AdvancedFilterHighlighter() override = default;

    AdvancedFilterHighlighter(const AdvancedFilterHighlighter &) = delete;
    AdvancedFilterHighlighter &operator=(const AdvancedFilterHighlighter &) = delete;
    AdvancedFilterHighlighter(AdvancedFilterHighlighter &&) = delete;
    AdvancedFilterHighlighter &operator=(AdvancedFilterHighlighter &&) = delete;

protected:
    void highlightBlock(const QString &text) override;

private:
    /// One rule per category. `captureGroup` picks which submatch to
    /// style: `0` = whole match (keywords / operators / strings),
    /// `1` = a specific capture group (used for regex literals so
    /// only the `/.../` body is styled, not the leading `~`).
    struct Rule
    {
        QRegularExpression pattern;
        QTextCharFormat format;
        int captureGroup = 0;
    };

    /// Rebuild `mBaseRules` and `mOverlayRules` from the current
    /// application palette. Called from the constructor; palette
    /// changes on modal dialogs are rare enough that we don't hook
    /// `QEvent::PaletteChange` for now.
    void RebuildRules();

    /// Rules applied first (keywords + operator punctuation). Later
    /// rules overwrite earlier ones for overlapping ranges.
    std::vector<Rule> mBaseRules;

    /// Rules applied last (quoted strings + regex literals). These
    /// win over keywords / operators inside quoted or regex spans so
    /// e.g. `msg ~ /OR/` doesn't paint the inner `OR` as a keyword.
    std::vector<Rule> mOverlayRules;
};
