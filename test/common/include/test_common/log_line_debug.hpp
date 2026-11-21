/**
 * @brief Slow, name-based helpers for building and reading LogLine values in tests.
 *
 * A LogLine stores key ids and compact values, so reading it by key name needs the KeyIndex that issued the ids. These
 * helpers do that work with simple maps and strings, which keeps test code short. They are not meant for production
 * code, which works with key ids.
 */
#pragma once

#include <loglib/core/key_index.hpp>
#include <loglib/core/line_source.hpp>
#include <loglib/core/log_line.hpp>
#include <loglib/core/log_value.hpp>

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace test_common
{

/** A row as a map from key name to value. */
using LogMap = std::unordered_map<std::string, loglib::LogValue>;

/** Builds a row from a map, adding unknown keys to keys. The row does not keep keys. */
[[nodiscard]] inline loglib::LogLine MakeLine(
    const LogMap &values, loglib::KeyIndex &keys, loglib::LineSource &source, size_t lineId
)
{
    std::vector<std::pair<loglib::KeyId, loglib::LogValue>> sorted;
    sorted.reserve(values.size());
    for (const auto &[key, value] : values)
    {
        sorted.emplace_back(keys.GetOrInsert(key), value);
    }
    std::ranges::sort(sorted, [](const auto &a, const auto &b) { return a.first < b.first; });
    return loglib::LogLine(std::move(sorted), source, lineId);
}

/** The value of a field by key name, or std::monostate if the key is unknown or the row has no such field. */
[[nodiscard]] inline loglib::LogValue ValueByName(
    const loglib::LogLine &line, const loglib::KeyIndex &keys, std::string_view key
)
{
    if (key.empty())
    {
        return loglib::LogValue{std::monostate{}};
    }
    const loglib::KeyId id = keys.Find(key);
    if (id == loglib::INVALID_KEY_ID)
    {
        return loglib::LogValue{std::monostate{}};
    }
    return line.GetValue(id);
}

/** The names of the fields of the row, sorted by key id. */
[[nodiscard]] inline std::vector<std::string> KeyNames(const loglib::LogLine &line, const loglib::KeyIndex &keys)
{
    std::vector<std::string> names;
    for (const auto &[id, value] : line.IndexedValues())
    {
        names.emplace_back(keys.KeyOf(id));
    }
    return names;
}

/** All fields of the row by key name. */
[[nodiscard]] inline LogMap ToMap(const loglib::LogLine &line, const loglib::KeyIndex &keys)
{
    LogMap map;
    for (auto &[id, value] : line.IndexedValues())
    {
        map.emplace(std::string(keys.KeyOf(id)), std::move(value));
    }
    return map;
}

} // namespace test_common
