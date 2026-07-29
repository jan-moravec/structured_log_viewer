#include "advanced_filter_highlighter.hpp"

#include <QApplication>
#include <QPalette>
#include <QRegularExpressionMatchIterator>
#include <QTextDocument>

AdvancedFilterHighlighter::AdvancedFilterHighlighter(QTextDocument *parent) : QSyntaxHighlighter(parent)
{
    RebuildRules();
}

void AdvancedFilterHighlighter::RebuildRules()
{
    const QPalette pal = QApplication::palette();

    QTextCharFormat keywordFormat;
    keywordFormat.setFontWeight(QFont::Bold);
    // `QPalette::Link` reads as an accent on both light and dark
    // themes without competing with the wavy error underline (which
    // uses the app's error-tint colour).
    keywordFormat.setForeground(pal.color(QPalette::Link));

    QTextCharFormat operatorFormat;
    operatorFormat.setFontWeight(QFont::Bold);

    QTextCharFormat literalFormat;
    literalFormat.setFontItalic(true);
    // Dimmed foreground signals "opaque payload -- don't try to
    // parse this as syntax". Matches the palette role the help
    // label uses for its own gloss text.
    literalFormat.setForeground(pal.color(QPalette::PlaceholderText));

    mBaseRules.clear();
    mOverlayRules.clear();

    // Keywords: `\b` on either side rejects `AND` inside identifiers
    // like `service:command` or `handle_or_fail`. Case-insensitive
    // because the grammar accepts any casing (see `LexIdent`).
    mBaseRules.push_back(Rule{
        QRegularExpression(QStringLiteral("\\b(?:and|or|not|in)\\b"), QRegularExpression::CaseInsensitiveOption),
        keywordFormat,
        0
    });

    // Operator punctuation. Two-char forms (`>=`, `<=`) come first
    // in the alternation so `>=` doesn't get half-consumed as `>`.
    mBaseRules.push_back(Rule{QRegularExpression(QStringLiteral(">=|<=|[:~%><=]")), operatorFormat, 0});

    // Double-quoted string literals with `\\.` escapes. The
    // alternation is `\\.` OR `[^"\\]` so we stop at the first
    // unescaped closing quote.
    mOverlayRules.push_back(Rule{
        QRegularExpression(QStringLiteral("\"(?:\\\\.|[^\"\\\\])*\"")),
        literalFormat,
        0
    });

    // Regex literals only follow `~`. Anchoring on the tilde avoids
    // painting the `/` characters in a bareword like `path:/var/log`
    // as a regex delimiter. Capture group 1 is the `/.../` body so
    // `setFormat` styles only the regex, not the leading tilde /
    // whitespace.
    mOverlayRules.push_back(Rule{
        QRegularExpression(QStringLiteral("~\\s*(/(?:\\\\.|[^/\\\\])*/)")),
        literalFormat,
        1
    });
}

void AdvancedFilterHighlighter::highlightBlock(const QString &text)
{
    const auto apply = [this, &text](const std::vector<Rule> &rules) {
        for (const auto &rule : rules)
        {
            QRegularExpressionMatchIterator it = rule.pattern.globalMatch(text);
            while (it.hasNext())
            {
                const auto match = it.next();
                // `capturedStart` / `capturedLength` return
                // `qsizetype` (64-bit) but `setFormat` takes `int`.
                // Query text is short (a user-typed filter, not a
                // document body), so the cast is safe; explicit
                // form silences clang-tidy narrowing warnings.
                const qsizetype start = match.capturedStart(rule.captureGroup);
                const qsizetype length = match.capturedLength(rule.captureGroup);
                if (start < 0 || length <= 0)
                {
                    continue;
                }
                setFormat(static_cast<int>(start), static_cast<int>(length), rule.format);
            }
        }
    };
    apply(mBaseRules);
    // Overlay rules run second so quoted / regex bodies overwrite
    // any keyword or operator formatting picked up on the first
    // pass (`msg ~ /OR/` must not render the inner `OR` as bold /
    // accent-coloured).
    apply(mOverlayRules);
}
