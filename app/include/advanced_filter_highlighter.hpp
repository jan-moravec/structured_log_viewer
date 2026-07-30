#pragma once

#include <QRegularExpression>
#include <QSyntaxHighlighter>
#include <QTextCharFormat>

#include <vector>

class QTextDocument;

/// Live syntax highlighter for the Advanced Filter query editor.
///
/// Applies four palette-aware categories:
///   - **Keywords** (`AND` / `OR` / `NOT` / `IN`, case-insensitive):
///     bold + link colour.
///   - **Operator punctuation** (`:` `~` `%` `>=` `<=` `>` `<` `=`):
///     bold, so the column/value boundary is visible.
///   - **Quoted strings** (`"..."`) and **regex literals** (`/.../`
///     following `~`): italic + placeholder-text colour.
///
/// Layers cleanly under the wavy parse-error underline set via
/// `QPlainTextEdit::setExtraSelections` (disjoint rendering channels).
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
    /// One highlight rule. `captureGroup` picks which submatch to
    /// style (0 = whole match; 1 = e.g. the regex-literal body only).
    struct Rule
    {
        QRegularExpression pattern;
        QTextCharFormat format;
        int captureGroup = 0;
    };

    /// Rebuild rule sets from the current application palette.
    void RebuildRules();

    /// Applied first (keywords + operator punctuation).
    std::vector<Rule> mBaseRules;

    /// Applied last, overriding base rules inside literals so e.g.
    /// `msg ~ /OR/` doesn't paint the inner `OR` as a keyword.
    std::vector<Rule> mOverlayRules;
};
