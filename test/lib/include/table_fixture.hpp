#pragma once

#include <loglib/table/log_table.hpp>

#include <utility>

// Test fixtures often fill a table only to exercise something else, and never look at the column moves and enumeration
// demotions that AppendBatch and FinalizeAutoDetection report. The production API is [[nodiscard]] so real callers
// cannot drop that result by accident; fixtures that do not need it go through these helpers, which say so once.

inline void AppendFixtureBatch(loglib::LogTable &table, loglib::RowBatch batch)
{
    (void)table.AppendBatch(std::move(batch));
}

inline void FinalizeFixture(loglib::LogTable &table)
{
    (void)table.FinalizeAutoDetection();
}
