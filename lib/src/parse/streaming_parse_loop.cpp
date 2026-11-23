#include "loglib/parse/internal/streaming_parse_loop.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace loglib::internal
{

void SliceValuesOfRawLine(std::vector<CompactLogValue> &values, std::string &ownedArena, std::string_view rawLine)
{
    if (ownedArena.empty())
    {
        return;
    }
    const std::string_view arena(ownedArena);
    std::string kept;
    for (CompactLogValue &field : values)
    {
        if (field.tag != CompactTag::OwnedString)
        {
            continue;
        }
        const std::string_view text = arena.substr(field.payload, field.aux);
        const size_t position = text.empty() ? std::string_view::npos : rawLine.find(text);
        if (position != std::string_view::npos)
        {
            field = CompactLogValue::MakeMmapSlice(position, field.aux).WithKey(field.Key());
            continue;
        }
        const uint64_t offset = kept.size();
        kept.append(text);
        field.payload = offset;
    }
    ownedArena = std::move(kept);
}

} // namespace loglib::internal
