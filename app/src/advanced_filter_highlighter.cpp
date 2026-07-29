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
    // Accent that reads on light/dark themes without clashing with
    // the error underline (which uses the error-tint colour).
    keywordFormat.setForeground(pal.color(QPalette::Link));

    QTextCharFormat operatorFormat;
    operatorFormat.setFontWeight(QFont::Bold);

    QTextCharFormat literalFormat;
    literalFormat.setFontItalic(true);
    // Dimmed: signals "opaque payload, not syntax".
    literalFormat.setForeground(pal.color(QPalette::PlaceholderText));

    mBaseRules.clear();
    mOverlayRules.clear();

    // Keywords: `\b` bounds reject `AND` inside identifiers like
    // `handle_or_fail`. Case-insensitive to match the grammar.
    mBaseRules.push_back(Rule{
        QRegularExpression(QStringLiteral("\\b(?:and|or|not|in)\\b"), QRegularExpression::CaseInsensitiveOption),
        keywordFormat,
        0
    });

    // Two-char operators come first so `>=` isn't half-consumed as `>`.
    mBaseRules.push_back(Rule{QRegularExpression(QStringLiteral(">=|<=|[:~%><=]")), operatorFormat, 0});

    // Double-quoted strings with `\\.` escapes.
    mOverlayRules.push_back(Rule{
        QRegularExpression(QStringLiteral("\"(?:\\\\.|[^\"\\\\])*\"")),
        literalFormat,
        0
    });

    // Regex literals only after `~`, so `path:/var/log` stays plain.
    // Capture group 1 = `/.../` body, so leading `~` isn't styled.
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
                // qsizetype -> int cast: query text is short.
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
    // Overlay second: literal spans win over keyword/operator formatting.
    apply(mOverlayRules);
}
