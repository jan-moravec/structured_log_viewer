#pragma once

#include "key_index.hpp"
#include "log_configuration.hpp"
#include "log_data.hpp"

#include <date/date.h>
#include <date/tz.h>
#include <fmt/format.h>

#include <string>
#include <vector>

namespace loglib
{

class LogTable
{
public:
    /**
     * @brief Default constructor for LogTable.
     */
    LogTable() = default;

    /**
     * @brief Constructs a LogTable with the specified data and configuration.
     *
     * @param data The log data to be stored in the table.
     * @param configuration The configuration manager that controls how log data is displayed.
     */
    LogTable(LogData data, LogConfigurationManager configuration);

    // Deleted copy constructor and copy assignment operator for efficiency.
    LogTable(LogTable &) = delete;
    LogTable &operator=(const LogTable &) = delete;

    // Defaulted move constructor and move assignment operator.
    LogTable(LogTable &&) = default;
    LogTable &operator=(LogTable &&) = default;

    /**
     * @brief Updates the table with new log data.
     *
     * @param data The new log data to be merged with the existing data.
     */
    void Update(LogData &&data);

    /**
     * @brief Gets the header text for the specified column.
     *
     * @param column The index of the column.
     * @return The header text as a string.
     */
    std::string GetHeader(size_t column) const;

    /**
     * @brief Gets the total number of columns in the table.
     *
     * @return The number of columns.
     */
    size_t ColumnCount() const;

    /**
     * @brief Gets the raw log value at the specified row and column.
     *
     * @param row The index of the row.
     * @param column The index of the column.
     * @return The log value at the specified position.
     */
    LogValue GetValue(size_t row, size_t column) const;

    /**
     * @brief Gets the formatted string representation of the value at the specified row and column.
     *
     * @param row The index of the row.
     * @param column The index of the column.
     * @return The formatted string representation of the value.
     */
    std::string GetFormattedValue(size_t row, size_t column) const;

    /**
     * @brief Gets the total number of rows in the table.
     *
     * @return The number of rows.
     */
    size_t RowCount() const;

    /**
     * @brief Gets a const reference to the underlying log data.
     *
     * @return A const reference to the log data.
     */
    const LogData &Data() const;

    /**
     * @brief Gets a mutable reference to the underlying log data.
     *
     * @return A mutable reference to the log data.
     */
    LogData &Data();

    /**
     * @brief Gets a const reference to the configuration manager.
     *
     * @return A const reference to the configuration manager.
     */
    const LogConfigurationManager &Configuration() const;

    /**
     * @brief Gets a mutable reference to the configuration manager.
     *
     * @return A mutable reference to the configuration manager.
     */
    LogConfigurationManager &Configuration();

private:
    /**
     * @brief Formats a log value according to the specified format string.
     *
     * @param format The format string to use.
     * @param value The log value to format.
     * @return The formatted string representation of the value.
     */
    static std::string FormatLogValue(const std::string &format, const LogValue &value);

    /**
     * @brief Rebuilds the column → KeyId cache from the current configuration.
     *
     * The cache is one inner vector per configured column. Each inner vector
     * lists the canonical KeyIds of the configuration's `column.keys`,
     * resolved through the owning `LogData::Keys()`. Unknown keys are stored
     * as `kInvalidKeyId` and skipped at lookup time. Called whenever the data
     * or configuration changes (PRD req. 4.1.12).
     */
    void RefreshColumnKeyIds();

    LogData mData;
    LogConfigurationManager mConfiguration;
    std::vector<std::vector<KeyId>> mColumnKeyIds;
};

} // namespace loglib
