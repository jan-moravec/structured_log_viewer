/**
 * @brief Writes timestamps as text from a format that is read once.
 *
 * Exports and tables format the same column over and over, and the general date formatter spends most of its time on
 * streams and locales. A TimestampFormatter reads a strftime-style format once into a few steps and then writes each
 * time as digits into the caller's buffer. It handles the specifiers F, T, Y, m, d, H, M, S, z, Ez and Z with literal
 * text between them. Any other format, a year outside 0000 to 9999 and a time the zone cannot answer for go to the
 * general formatter, so the text is always the same as TimeZoneContext::Format with the same precision gives.
 *
 * This is part of the time module. A formatter keeps the last day and the last zone span it used, so use one for each
 * thread or export and do not share it.
 */
#pragma once

#include "loglib/core/log_value.hpp"
#include "loglib/time/time_zone_context.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace loglib::internal
{

/**
 * @brief Formats timestamps in one zone with one format and one number of fraction digits.
 *
 * A time is first rounded to the precision, in UTC. The fraction digits follow every second that the format prints,
 * whether through S or T.
 */
class TimestampFormatter
{
public:
    TimestampFormatter(TimeZoneContext zone, std::string_view format, FractionPrecision precision);

    /** @brief Appends the text of the time to out. Throws std::runtime_error if the format cannot be applied. */
    void Append(std::string &out, TimeStamp timeStamp);

    /** @brief Returns the text of the time. Throws std::runtime_error if the format cannot be applied. */
    [[nodiscard]] std::string Format(TimeStamp timeStamp);

private:
    enum class StepKind : std::uint8_t
    {
        Literal,
        Date,
        TimeOfDay,
        Year,
        Month,
        Day,
        Hour,
        Minute,
        Second,
        Offset,
        OffsetWithColon,
        Abbreviation,
    };

    struct Step
    {
        StepKind kind = StepKind::Literal;
        std::string literal;
    };

    /** @brief Writes the time with the steps. Returns false, writing nothing, if the general formatter must be used. */
    [[nodiscard]] bool AppendWithSteps(std::string &out, TimeStamp timeStamp);

    /** @brief Reads the format into mSteps. Leaves mSupported false if it holds anything the steps do not cover. */
    void Compile();

    TimeZoneContext mZone;
    std::string mFormat;
    FractionPrecision mPrecision;
    bool mSupported = false;
    std::vector<Step> mSteps;
    /** @brief The most bytes the steps write, without the abbreviation. */
    std::size_t mFixedLength = 0;
    /** @brief How many times the steps write the zone abbreviation. */
    std::size_t mAbbreviationCount = 0;

    /** @brief The zone span of the last time that was written, valid when mHasSpan is set. */
    ZoneSpan mSpan;
    bool mHasSpan = false;

    /** @brief The day that mDayText holds, in days since the epoch, or no day yet. */
    std::int64_t mDay = 0;
    bool mHasDay = false;
    std::int32_t mYear = 0;
    std::uint32_t mMonth = 0;
    std::uint32_t mDayOfMonth = 0;
    /** @brief The length of a date written as year-month-day. */
    static constexpr std::size_t DAY_TEXT_LENGTH = 10;
    /** @brief The text of mDay as year-month-day. */
    std::array<char, DAY_TEXT_LENGTH> mDayText{};
};

} // namespace loglib::internal
