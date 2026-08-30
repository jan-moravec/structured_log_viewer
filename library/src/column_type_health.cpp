#include "loglib/column_type_health.hpp"

#include <string>
#include <vector>

namespace loglib
{

bool TagMatchesType(CompactTag tag, ColumnType declaredType) noexcept
{
    using Type = ColumnType;
    using Tag = CompactTag;
    if (tag == Tag::Monostate)
    {
        return false;
    }
    switch (declaredType)
    {
    case Type::Any:
        return true;
    case Type::String:
        return tag == Tag::MmapSlice || tag == Tag::OwnedString || tag == Tag::DictRef;
    case Type::Boolean:
        return tag == Tag::Bool;
    case Type::Integer:
        return tag == Tag::Int64 || tag == Tag::Uint64;
    case Type::Floating:
        return tag == Tag::Double;
    case Type::Number:
        return tag == Tag::Int64 || tag == Tag::Uint64 || tag == Tag::Double;
    case Type::Time:
        // `Timestamp` is the natural tag; epoch integers ride the
        // same comparator so they also count as matching.
        return tag == Tag::Timestamp || tag == Tag::Int64 || tag == Tag::Uint64;
    case Type::Enumeration:
    case Type::Level:
        // Only `DictRef` matches. Unencoded `OwnedString` slots are
        // the over-cap values the diagnostic surfaces.
        return tag == Tag::DictRef;
    }
    return false;
}

ColumnTypeHealth ComputeColumnTypeHealth(
    std::span<const LogLine> lines, const KeyIndex &keys, const Column &column
)
{
    ColumnTypeHealth health;
    health.totalSlots = lines.size();
    if (column.keys.empty())
    {
        return health;
    }

    std::vector<KeyId> aliasKeys;
    aliasKeys.reserve(column.keys.size());
    for (const std::string &key : column.keys)
    {
        const KeyId id = keys.Find(key);
        if (id != INVALID_KEY_ID)
        {
            aliasKeys.push_back(id);
        }
    }
    if (aliasKeys.empty())
    {
        return health;
    }

    for (const LogLine &line : lines)
    {
        const CompactLogValue *slot = nullptr;
        for (const KeyId id : aliasKeys)
        {
            slot = line.FindCompact(id);
            if (slot != nullptr)
            {
                break;
            }
        }
        if (slot == nullptr || slot->tag == CompactTag::Monostate)
        {
            continue;
        }
        ++health.presentSlots;
        if (TagMatchesType(slot->tag, column.type))
        {
            ++health.matchingSlots;
        }
    }
    return health;
}

} // namespace loglib
