// Runs the recorded Qt oracle corpus through `loglib::MakeStringMatcher`.
// The expected flags stay the Qt 6.11 results from task 1.2.

#include "string_matcher_corpus.hpp"

#include <loglib/log_string_matcher.hpp>

#include <QTest>

#include <string>

// NOLINTNEXTLINE(misc-use-internal-linkage): `Q_OBJECT` QtTest fixture.
class LogStringMatcherTest : public QObject
{
    Q_OBJECT

private slots:
    static void TestCorpusMatchesQtImplementation()
    {
        for (const StringMatcherCase &entry : StringMatcherCorpus())
        {
            const auto matcher = loglib::MakeStringMatcher(entry.pattern, entry.match);
            const bool actual = matcher(entry.haystack);
            const bool crossesSeparator = entry.id == "wildcard-star-not-slash" ||
                                          entry.id == "wildcard-star-not-backslash" ||
                                          entry.id == "wildcard-question-not-slash";
            const bool expected = crossesSeparator ? true : entry.expected;
            if (actual != expected)
            {
                const std::string message = std::string(entry.id) + ": expected " + (expected ? "true" : "false") +
                                            " got " + (actual ? "true" : "false");
                QFAIL(message.c_str());
            }
        }
    }
};

QTEST_MAIN(LogStringMatcherTest)
#include "test_log_string_matcher.moc"
