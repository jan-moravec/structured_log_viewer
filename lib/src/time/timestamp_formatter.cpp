#include "loglib/time/internal/timestamp_formatter.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace loglib::internal
{

namespace
{

constexpr std::int64_t SECONDS_PER_MINUTE = 60;
constexpr std::int64_t MINUTES_PER_HOUR = 60;
constexpr std::int64_t HOURS_PER_DAY = 24;
constexpr std::int64_t MICROS_PER_MILLI = 1'000;
constexpr std::int64_t MICROS_PER_SECOND = 1'000'000;
constexpr std::int64_t MICROS_PER_MINUTE = SECONDS_PER_MINUTE * MICROS_PER_SECOND;
constexpr std::int64_t MICROS_PER_HOUR = MINUTES_PER_HOUR * MICROS_PER_MINUTE;
constexpr std::int64_t MICROS_PER_DAY = HOURS_PER_DAY * MICROS_PER_HOUR;

// The times from the start of year 0000 to the end of year 9999, kept two days clear of both ends so that no zone
// offset moves a time out of the years the steps write.
constexpr std::int64_t FIRST_YEAR_SECONDS = -62'167'219'200;
constexpr std::int64_t AFTER_LAST_YEAR_SECONDS = 253'402'300'800;
constexpr std::int64_t YEAR_MARGIN_MICROS = 2 * MICROS_PER_DAY;
constexpr std::int64_t FIRST_YEAR_MICROS = (FIRST_YEAR_SECONDS * MICROS_PER_SECOND) + YEAR_MARGIN_MICROS;
constexpr std::int64_t LAST_YEAR_MICROS = (AFTER_LAST_YEAR_SECONDS * MICROS_PER_SECOND) - YEAR_MARGIN_MICROS;
constexpr std::int64_t LAST_YEAR = 9999;

// A zone offset has at most two digits of hours.
constexpr std::int64_t MAX_OFFSET_HOURS = 99;
constexpr std::int64_t MAX_OFFSET_SECONDS = MAX_OFFSET_HOURS * MINUTES_PER_HOUR * SECONDS_PER_MINUTE;

constexpr std::size_t DATE_LENGTH = 10;
constexpr std::size_t TIME_OF_DAY_LENGTH = 8;
constexpr std::size_t OFFSET_LENGTH = 5;
constexpr std::size_t OFFSET_WITH_COLON_LENGTH = 6;

constexpr unsigned DECIMAL_BASE = 10;
constexpr unsigned TWO_DIGIT_LIMIT = 100;
constexpr unsigned FOUR_DIGIT_LIMIT = 10'000;

/** @brief The two-digit text of every number from 0 to 99. */
struct DigitPairs
{
    std::array<char, 2 * TWO_DIGIT_LIMIT> text{};

    constexpr DigitPairs()
    {
        for (unsigned i = 0; i < TWO_DIGIT_LIMIT; ++i)
        {
            text[2 * i] = static_cast<char>('0' + (i / DECIMAL_BASE));
            text[(2 * i) + 1] = static_cast<char>('0' + (i % DECIMAL_BASE));
        }
    }
};
constexpr DigitPairs DIGIT_PAIRS;

void PutTwo(char *&at, unsigned value) noexcept
{
    at[0] = DIGIT_PAIRS.text[2 * value];
    at[1] = DIGIT_PAIRS.text[(2 * value) + 1];
    at += 2;
}

void PutFour(char *&at, unsigned value) noexcept
{
    PutTwo(at, value / TWO_DIGIT_LIMIT);
    PutTwo(at, value % TWO_DIGIT_LIMIT);
}

/** @brief Writes the fraction of a second: a dot and then three or six digits. */
void PutFraction(char *&at, std::int64_t microsecondsInSecond, FractionPrecision precision) noexcept
{
    *at++ = '.';
    if (precision == FractionPrecision::Milliseconds)
    {
        const auto milliseconds = static_cast<unsigned>(microsecondsInSecond / MICROS_PER_MILLI);
        *at++ = static_cast<char>('0' + (milliseconds / TWO_DIGIT_LIMIT));
        PutTwo(at, milliseconds % TWO_DIGIT_LIMIT);
        return;
    }
    const auto value = static_cast<unsigned>(microsecondsInSecond);
    PutTwo(at, value / FOUR_DIGIT_LIMIT);
    PutTwo(at, (value / TWO_DIGIT_LIMIT) % TWO_DIGIT_LIMIT);
    PutTwo(at, value % TWO_DIGIT_LIMIT);
}

/** @brief The largest whole number that is not above the quotient. */
[[nodiscard]] std::int64_t FloorDivide(std::int64_t value, std::int64_t divisor) noexcept
{
    const std::int64_t quotient = value / divisor;
    return (value % divisor != 0 && (value < 0) != (divisor < 0)) ? quotient - 1 : quotient;
}

/** @brief A year, month and day of the proleptic Gregorian calendar. */
struct CivilDate
{
    std::int64_t year = 0;
    std::uint32_t month = 0;
    std::uint32_t day = 0;
};

/** @brief The date of a day counted from 1970-01-01. */
[[nodiscard]] CivilDate CivilFromDays(std::int64_t days) noexcept
{
    // The shifted-era algorithm, with March as the first month of the year. Its constants are lengths of the calendar:
    // 146097 days in 400 years, 36524 in 100 years, 1460 in 4 years, and 153 days in five months from March.
    // NOLINTBEGIN(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)
    const std::int64_t shifted = days + 719'468;
    const std::int64_t era = FloorDivide(shifted, 146'097);
    const std::int64_t dayOfEra = shifted - (era * 146'097);
    const std::int64_t yearOfEra = (dayOfEra - (dayOfEra / 1460) + (dayOfEra / 36'524) - (dayOfEra / 146'096)) / 365;
    const std::int64_t dayOfYear = dayOfEra - ((365 * yearOfEra) + (yearOfEra / 4) - (yearOfEra / 100));
    const std::int64_t monthIndex = ((5 * dayOfYear) + 2) / 153;
    const auto dayOfMonth = static_cast<std::uint32_t>(dayOfYear - (((153 * monthIndex) + 2) / 5) + 1);
    const auto month = static_cast<std::uint32_t>(monthIndex < 10 ? monthIndex + 3 : monthIndex - 9);
    return CivilDate{
        .year = yearOfEra + (era * 400) + (month <= 2 ? 1 : 0),
        .month = month,
        .day = dayOfMonth,
    };
    // NOLINTEND(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)
}

} // namespace

TimestampFormatter::TimestampFormatter(TimeZoneContext zone, std::string_view format, FractionPrecision precision)
    : mZone(std::move(zone)), mFormat(format), mPrecision(precision)
{
    Compile();
}

void TimestampFormatter::Compile()
{
    const std::size_t fractionLength = mPrecision == FractionPrecision::Milliseconds ? 4 : 7;
    std::string literal;
    const auto flushLiteral = [&] {
        if (!literal.empty())
        {
            mFixedLength += literal.size();
            mSteps.push_back(Step{.kind = StepKind::Literal, .literal = std::move(literal)});
            literal.clear();
        }
    };
    const auto addStep = [&](StepKind kind, std::size_t length) {
        flushLiteral();
        mFixedLength += length;
        mSteps.push_back(Step{.kind = kind, .literal = {}});
    };

    for (std::size_t i = 0; i < mFormat.size(); ++i)
    {
        if (mFormat[i] != '%')
        {
            literal.push_back(mFormat[i]);
            continue;
        }
        if (++i == mFormat.size())
        {
            mSteps.clear();
            return;
        }
        switch (mFormat[i])
        {
        case 'F':
            addStep(StepKind::Date, DATE_LENGTH);
            break;
        case 'T':
            addStep(StepKind::TimeOfDay, TIME_OF_DAY_LENGTH + fractionLength);
            break;
        case 'Y':
            addStep(StepKind::Year, 4);
            break;
        case 'm':
            addStep(StepKind::Month, 2);
            break;
        case 'd':
            addStep(StepKind::Day, 2);
            break;
        case 'H':
            addStep(StepKind::Hour, 2);
            break;
        case 'M':
            addStep(StepKind::Minute, 2);
            break;
        case 'S':
            addStep(StepKind::Second, 2 + fractionLength);
            break;
        case 'z':
            addStep(StepKind::Offset, OFFSET_LENGTH);
            break;
        case 'Z':
            addStep(StepKind::Abbreviation, 0);
            ++mAbbreviationCount;
            break;
        case 'E':
            if (i + 1 == mFormat.size() || mFormat[i + 1] != 'z')
            {
                mSteps.clear();
                return;
            }
            ++i;
            addStep(StepKind::OffsetWithColon, OFFSET_WITH_COLON_LENGTH);
            break;
        default:
            mSteps.clear();
            return;
        }
    }
    flushLiteral();
    mSupported = true;
}

bool TimestampFormatter::AppendWithSteps(std::string &out, TimeStamp timeStamp)
{
    std::int64_t utc = timeStamp.time_since_epoch().count();
    if (utc < FIRST_YEAR_MICROS || utc > LAST_YEAR_MICROS)
    {
        return false;
    }
    if (mPrecision == FractionPrecision::Milliseconds)
    {
        utc = std::chrono::round<std::chrono::milliseconds>(timeStamp).time_since_epoch().count() * MICROS_PER_MILLI;
    }

    std::int64_t offsetSeconds = 0;
    std::string_view abbreviation = "UTC";
    if (!mZone.IsUtc())
    {
        // Most times fall in the span of the previous one, which costs two comparisons instead of a zone lookup.
        if (!mHasSpan || utc < mSpan.beginUtc || utc >= mSpan.endUtc)
        {
            std::optional<ZoneSpan> span = mZone.ZoneSpanAt(utc);
            if (!span)
            {
                return false;
            }
            mSpan = std::move(*span);
            mHasSpan = true;
        }
        if (mSpan.offsetSeconds <= -MAX_OFFSET_SECONDS || mSpan.offsetSeconds >= MAX_OFFSET_SECONDS)
        {
            return false;
        }
        offsetSeconds = mSpan.offsetSeconds;
        abbreviation = mSpan.abbreviation;
    }

    const std::int64_t local = utc + (offsetSeconds * MICROS_PER_SECOND);
    const std::int64_t day = FloorDivide(local, MICROS_PER_DAY);
    const std::int64_t timeOfDay = local - (day * MICROS_PER_DAY);

    if (!mHasDay || day != mDay)
    {
        const CivilDate date = CivilFromDays(day);
        if (date.year < 0 || date.year > LAST_YEAR)
        {
            return false;
        }
        mYear = static_cast<std::int32_t>(date.year);
        mMonth = date.month;
        mDayOfMonth = date.day;
        char *at = mDayText.data();
        PutFour(at, static_cast<unsigned>(mYear));
        *at++ = '-';
        PutTwo(at, mMonth);
        *at++ = '-';
        PutTwo(at, mDayOfMonth);
        mDay = day;
        mHasDay = true;
    }

    const auto hour = static_cast<unsigned>(timeOfDay / MICROS_PER_HOUR);
    const auto minute = static_cast<unsigned>((timeOfDay / MICROS_PER_MINUTE) % MINUTES_PER_HOUR);
    const auto second = static_cast<unsigned>((timeOfDay / MICROS_PER_SECOND) % SECONDS_PER_MINUTE);
    const std::int64_t microsecondsInSecond = timeOfDay % MICROS_PER_SECOND;

    const std::size_t start = out.size();
    out.resize_and_overwrite(
        start + mFixedLength + (mAbbreviationCount * abbreviation.size()), [&](char *buffer, std::size_t) {
            char *at = buffer + start;
            for (const Step &step : mSteps)
            {
                switch (step.kind)
                {
                case StepKind::Literal:
                    at = std::copy(step.literal.begin(), step.literal.end(), at);
                    break;
                case StepKind::Date:
                    at = std::copy(mDayText.begin(), mDayText.end(), at);
                    break;
                case StepKind::TimeOfDay:
                    PutTwo(at, hour);
                    *at++ = ':';
                    PutTwo(at, minute);
                    *at++ = ':';
                    PutTwo(at, second);
                    PutFraction(at, microsecondsInSecond, mPrecision);
                    break;
                case StepKind::Year:
                    PutFour(at, static_cast<unsigned>(mYear));
                    break;
                case StepKind::Month:
                    PutTwo(at, mMonth);
                    break;
                case StepKind::Day:
                    PutTwo(at, mDayOfMonth);
                    break;
                case StepKind::Hour:
                    PutTwo(at, hour);
                    break;
                case StepKind::Minute:
                    PutTwo(at, minute);
                    break;
                case StepKind::Second:
                    PutTwo(at, second);
                    PutFraction(at, microsecondsInSecond, mPrecision);
                    break;
                case StepKind::Offset:
                case StepKind::OffsetWithColon:
                {
                    // Whole minutes, truncated toward zero, as the general formatter writes them.
                    std::int64_t offsetMinutes = offsetSeconds / SECONDS_PER_MINUTE;
                    *at++ = offsetMinutes < 0 ? '-' : '+';
                    if (offsetMinutes < 0)
                    {
                        offsetMinutes = -offsetMinutes;
                    }
                    PutTwo(at, static_cast<unsigned>(offsetMinutes / MINUTES_PER_HOUR));
                    if (step.kind == StepKind::OffsetWithColon)
                    {
                        *at++ = ':';
                    }
                    PutTwo(at, static_cast<unsigned>(offsetMinutes % MINUTES_PER_HOUR));
                    break;
                }
                case StepKind::Abbreviation:
                    at = std::copy(abbreviation.begin(), abbreviation.end(), at);
                    break;
                }
            }
            return static_cast<std::size_t>(at - buffer);
        }
    );
    return true;
}

void TimestampFormatter::Append(std::string &out, TimeStamp timeStamp)
{
    if (mSupported && AppendWithSteps(out, timeStamp))
    {
        return;
    }
    out.append(mZone.Format(timeStamp, mFormat, mPrecision));
}

std::string TimestampFormatter::Format(TimeStamp timeStamp)
{
    std::string text;
    Append(text, timeStamp);
    return text;
}

} // namespace loglib::internal
