/**
 * @brief Lets tests replace the clock a LogTable reads.
 *
 * A table reads the clock for the "now" of timestamp formats without a year, once per batch of a live stream. Tests
 * that cross a year boundary need to move that clock between batches, which SetReferenceTime cannot do. The clock hook
 * is private to LogTable, and this struct is its only caller. It is not part of the public API.
 *
 * This is part of the table module.
 */
#pragma once

#include "loglib/table/log_table.hpp"

#include <chrono>
#include <functional>
#include <utility>

namespace loglib::internal
{

/** @brief Reaches the test hooks of LogTable. */
struct LogTableTestAccess
{
    /** @brief Replaces the clock the table reads for the "now" of timestamp formats without a year. */
    static void SetClock(LogTable &table, std::function<std::chrono::system_clock::time_point()> clock)
    {
        table.SetClock(std::move(clock));
    }
};

} // namespace loglib::internal
