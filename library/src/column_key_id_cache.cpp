#include "loglib/internal/column_key_id_cache.hpp"

#include <algorithm>
#include <iterator>
#include <string_view>
#include <unordered_set>

namespace loglib::internal
{

void RefreshColumnKeyIds(
    std::vector<std::vector<KeyId>> &cache, const LogConfiguration &configuration, const KeyIndex &keys
)
{
    const auto &columns = configuration.columns;
    cache.clear();
    cache.reserve(columns.size());
    for (const auto &column : columns)
    {
        std::vector<KeyId> ids;
        ids.reserve(column.keys.size());
        for (const auto &key : column.keys)
        {
            ids.push_back(keys.Find(key));
        }
        cache.push_back(std::move(ids));
    }
}

void RefreshColumnKeyIdsForKeys(
    std::vector<std::vector<KeyId>> &cache,
    const LogConfiguration &configuration,
    const KeyIndex &keys,
    const std::vector<std::string> &newKeys
)
{
    if (newKeys.empty())
    {
        return;
    }

    std::unordered_set<std::string_view> newKeySet;
    newKeySet.reserve(newKeys.size());
    for (const std::string &key : newKeys)
    {
        newKeySet.emplace(key);
    }

    const auto &columns = configuration.columns;
    cache.resize(columns.size());

    for (size_t columnIndex = 0; columnIndex < columns.size(); ++columnIndex)
    {
        const auto &column = columns[columnIndex];

        bool affected = cache[columnIndex].size() != column.keys.size();
        if (!affected)
        {
            for (const std::string &key : column.keys)
            {
                if (newKeySet.contains(std::string_view(key)))
                {
                    affected = true;
                    break;
                }
            }
        }
        if (!affected)
        {
            continue;
        }

        std::vector<KeyId> ids;
        ids.reserve(column.keys.size());
        for (const std::string &key : column.keys)
        {
            ids.push_back(keys.Find(key));
        }
        cache[columnIndex] = std::move(ids);
    }
}

void MoveColumnKeyIds(std::vector<std::vector<KeyId>> &cache, size_t srcIndex, size_t destIndex)
{
    if (srcIndex == destIndex || srcIndex >= cache.size() || destIndex >= cache.size())
    {
        return;
    }
    using Diff = std::vector<std::vector<KeyId>>::difference_type;
    auto begin = cache.begin();
    if (srcIndex > destIndex)
    {
        std::rotate(
            std::next(begin, static_cast<Diff>(destIndex)),
            std::next(begin, static_cast<Diff>(srcIndex)),
            std::next(begin, static_cast<Diff>(srcIndex + 1))
        );
    }
    else
    {
        std::rotate(
            std::next(begin, static_cast<Diff>(srcIndex)),
            std::next(begin, static_cast<Diff>(srcIndex + 1)),
            std::next(begin, static_cast<Diff>(destIndex + 1))
        );
    }
}

} // namespace loglib::internal
