#include "log_string_matcher.hpp"

#include "log_model.hpp"

#include <QByteArray>
#include <QRegularExpression>
#include <QString>

#include <string>
#include <string_view>
#include <utility>

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

loglib::CallbackStringRowPredicate::MatchFn MakeStringMatcher(
    const QString &pattern, loglib::LogConfiguration::LogFilter::Match match
)
{
    using Match = loglib::LogConfiguration::LogFilter::Match;
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
        PrimeRegex(regex);
        return [regex](std::string_view bytes) { return regex.match(HaystackQStringFast(bytes)).hasMatch(); };
    }
    case Match::Wildcard:
    {
        QRegularExpression regex(QRegularExpression::wildcardToRegularExpression(pattern));
        PrimeRegex(regex);
        return [regex](std::string_view bytes) { return regex.match(HaystackQStringFast(bytes)).hasMatch(); };
    }
    }
    return [](std::string_view) { return false; };
}

loglib::CallbackStringRowPredicate::MatchFn MakeStringMatcher(
    const QString &pattern, loglib::LogConfiguration::HighlightRule::Match match
)
{
    // The two enums are pinned identical on purpose so this cast
    // stays safe. The static_asserts turn any future reorder of
    // either enum into a build error.
    using HR = loglib::LogConfiguration::HighlightRule::Match;
    using LF = loglib::LogConfiguration::LogFilter::Match;
    static_assert(static_cast<int>(HR::Exactly) == static_cast<int>(LF::Exactly));
    static_assert(static_cast<int>(HR::Contains) == static_cast<int>(LF::Contains));
    static_assert(static_cast<int>(HR::RegularExpression) == static_cast<int>(LF::RegularExpression));
    static_assert(static_cast<int>(HR::Wildcard) == static_cast<int>(LF::Wildcard));
    return MakeStringMatcher(pattern, static_cast<LF>(match));
}
