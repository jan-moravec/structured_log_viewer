/**
 * @brief Serializes rows as normalized JSON objects.
 *
 * The session bundle and the JSON Lines export store each row as one compact JSON object with typed values. The fields
 * follow the display order of the table's columns, so the same table always gives the same text, whatever order the
 * parser interned the keys in. A writer works that order out once for an export and then writes each row in it.
 *
 * This is part of the output module.
 */
#pragma once

#include "loglib/core/key_index.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace loglib
{
struct CompactLogValue;
class LogLine;
class LogTable;
} // namespace loglib

namespace loglib::internal
{

/**
 * @brief Writes the rows of one table as compact JSON objects with typed values.
 *
 * Fields are ordered by the position of their key in the columns of the table, in display order and with each column's
 * keys in the order the column lists them, hidden columns included. A field whose key no column lists comes last,
 * ordered by key text. Make one writer for each export and do not change the table's columns while it is in use. A
 * writer is not thread-safe.
 */
class NormalizedJsonRowWriter
{
public:
    explicit NormalizedJsonRowWriter(const LogTable &table);

    /** @brief Appends the row to out. The text has no newline at the end. */
    void Append(const LogLine &line, std::string &out);

private:
    struct RowField
    {
        KeyId id;
        const CompactLogValue *slot;
    };

    /** @brief The text that starts a field: the escaped key in quotes and a colon. */
    [[nodiscard]] const std::string &KeyPrefix(KeyId id);

    const KeyIndex &mKeys;
    /** @brief The rank of each key id, or UNRANKED when no column lists the key. */
    std::vector<std::uint32_t> mRankByKey;
    std::vector<std::string> mKeyPrefix;
    /** @brief The fields of the current row, in key id order. */
    std::vector<RowField> mRow;
    /** @brief The key ids of the previous row. A row with the same ids reuses mOrder. */
    std::vector<KeyId> mPreviousIds;
    /** @brief Positions in mRow, in the order the fields are written. */
    std::vector<std::uint32_t> mOrder;
};

} // namespace loglib::internal
