#include "log_string_matcher.hpp"

#include "log_model.hpp"

#include <QByteArray>
#include <QLoggingCategory>
#include <QRegularExpression>
#include <QString>

#include <string>
#include <string_view>
#include <utility>

Q_LOGGING_CATEGORY(logMatcher, "logapp.matcher")

namespace
{

/// JIT-prime the regex so captured copies don't race on a lazy
/// first `match()`. `QRegularExpression` is CoW / implicitly
/// shared, and `match()` is thread-safe once the private is
/// compiled; the parallel filter workers rely on that guarantee.
void PrimeRegex(QRegularExpression &regex)
{
    (void)regex.match(QStringLiteral(""));
}

/// Convert @p bytes to `QString`, skipping the `simplified()` walk
/// when the bytes are already canonical.
QString HaystackQStringFast(std::string_view bytes)
{
    if (LogModel::IsSingleLineAsciiTrim(bytes))
    {
        return QString::fromUtf8(bytes.data(), static_cast<qsizetype>(bytes.size()));
    }
    return LogModel::ConvertToSingleLineCompactQString(bytes);
}

} // namespace

loglib::CallbackStringRowPredicate::MatchFn MakeStringMatcher(const QString &pattern, loglib::LeafRule::Match match)
{
    using Match = loglib::LeafRule::Match;
    switch (match)
    {
    case Match::Exactly:
    {
        // Capture by value; `QString`'s implicit sharing keeps
        // this a refcount bump.
        const QByteArray patternUtf8 = pattern.toUtf8();
        std::string patternBytes{patternUtf8.constData(), static_cast<size_t>(patternUtf8.size())};
        if (LogModel::IsSingleLineAsciiTrim(patternBytes))
        {
            // NOLINTNEXTLINE(bugprone-exception-escape)
            return [patternBytes = std::move(patternBytes), pattern](std::string_view bytes) {
                if (LogModel::IsSingleLineAsciiTrim(bytes))
                {
                    return bytes == std::string_view{patternBytes};
                }
                return LogModel::ConvertToSingleLineCompactQString(bytes) == pattern;
            };
        }
        // clang-tidy flags the QString allocation as
        // exception-escape; benign here.
        // NOLINTNEXTLINE(bugprone-exception-escape)
        return
            [pattern](std::string_view bytes) { return LogModel::ConvertToSingleLineCompactQString(bytes) == pattern; };
    }
    case Match::Contains:
    {
        const QByteArray patternUtf8 = pattern.toUtf8();
        std::string patternBytes{patternUtf8.constData(), static_cast<size_t>(patternUtf8.size())};
        if (LogModel::IsSingleLineAsciiTrim(patternBytes))
        {
            // NOLINTNEXTLINE(bugprone-exception-escape)
            return [patternBytes = std::move(patternBytes), pattern](std::string_view bytes) {
                if (LogModel::IsSingleLineAsciiTrim(bytes))
                {
                    return bytes.contains(patternBytes);
                }
                return LogModel::ConvertToSingleLineCompactQString(bytes).contains(pattern);
            };
        }
        // NOLINTNEXTLINE(bugprone-exception-escape)
        return [pattern](std::string_view bytes) {
            return LogModel::ConvertToSingleLineCompactQString(bytes).contains(pattern);
        };
    }
    case Match::RegularExpression:
    {
        QRegularExpression regex(pattern);
        if (!regex.isValid())
        {
            // Callers validate up front (`FilterSubmitted`,
            // `AdvancedFilterEditor`); reaching here means a
            // hand-edited config or a bypass. Warn and return
            // match-none: visibly wrong beats silently permissive.
            qCWarning(logMatcher).noquote()
                << "MakeStringMatcher: invalid regular expression"
                << pattern
                << "-"
                << regex.errorString();
            return [](std::string_view) { return false; };
        }
        PrimeRegex(regex);
        return [regex](std::string_view bytes) { return regex.match(HaystackQStringFast(bytes)).hasMatch(); };
    }
    case Match::Wildcard:
    {
        // `wildcardToRegularExpression` always emits valid regex.
        QRegularExpression regex(QRegularExpression::wildcardToRegularExpression(pattern));
        PrimeRegex(regex);
        return [regex](std::string_view bytes) { return regex.match(HaystackQStringFast(bytes)).hasMatch(); };
    }
    }
    return [](std::string_view) { return false; };
}

