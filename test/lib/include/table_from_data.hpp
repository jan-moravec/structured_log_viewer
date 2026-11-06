/**
 * @brief Builds a LogTable from rows that a test made by hand.
 *
 * Some table tests need exact cells that no parser would give, such as a value of a chosen type on a chosen line. They
 * build a LogData with LogLine objects and hand it to these functions, which feed it through the same path a parser
 * uses: the sources are attached, the keys are interned and the rows arrive as one batch. Tests that can use a real
 * file use LoadFile instead.
 *
 * This is a test helper. It is not part of the library.
 */
#pragma once

#include "loglib/core/log_data.hpp"
#include "loglib/schema/log_configuration.hpp"
#include "loglib/table/log_table.hpp"

namespace test_common_lib
{

/**
 * @brief Adds the sources and rows of data to table as one batch and maps the keys of data onto the table's keys.
 *
 * The first call on a table begins a stream, later calls append. Keys the table has no column for become new columns at
 * the end. Type detection is not finished, so call loglib::FinishLoading when the last data is in.
 */
void AppendData(loglib::LogTable &table, loglib::LogData data);

/**
 * @brief A table that starts with the given configuration and holds data, with type detection finished.
 */
[[nodiscard]] loglib::LogTable TableFromData(loglib::LogData data, loglib::LogConfigurationManager configuration);

} // namespace test_common_lib
