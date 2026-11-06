
#include "table_from_data.hpp"

#include "loglib/core/key_index.hpp"
#include "loglib/core/log_line.hpp"
#include "loglib/core/row_batch.hpp"
#include "loglib/table/table_loader.hpp"

#include <algorithm>
#include <cassert>
#include <string>
#include <utility>
#include <vector>

namespace test_common_lib
{

void AppendData(loglib::LogTable &table, loglib::LogData data)
{
    using namespace loglib;

    // The rows keep their sources, which are heap objects, so moving the pointers keeps every address.
    std::vector<std::unique_ptr<LineSource>> sources = std::move(data.Sources());
    data.Sources().clear();
    bool first = table.Data().Sources().empty();
    for (auto &source : sources)
    {
        if (first)
        {
            table.BeginStreaming(std::move(source));
            first = false;
        }
        else
        {
            table.AppendStreaming(std::move(source));
        }
    }
    if (first)
    {
        // No source at all: begin an empty stream so the table has a key index of its own.
        table.BeginStreaming(nullptr);
    }

    // Map each key id of the data onto the table's key index.
    std::vector<KeyId> remap(data.Keys().Size());
    std::vector<std::string> newKeys;
    for (size_t id = 0; id < remap.size(); ++id)
    {
        const std::string_view key = data.Keys().KeyOf(static_cast<KeyId>(id));
        if (table.Keys().Find(key) == INVALID_KEY_ID)
        {
            newKeys.emplace_back(key);
        }
        remap[id] = table.Keys().GetOrInsert(key);
    }

    RowBatch batch;
    batch.lines.reserve(data.Lines().size());
    for (LogLine &line : data.Lines())
    {
        std::vector<std::pair<KeyId, CompactLogValue>> remapped;
        for (const auto &entry : line.CompactValues())
        {
            remapped.emplace_back(remap[entry.first], entry.second);
        }
        std::ranges::sort(remapped, [](const auto &a, const auto &b) { return a.first < b.first; });
        assert(line.Source() != nullptr);
        batch.lines.emplace_back(std::move(remapped), table.Keys(), *line.Source(), line.LineId());
    }
    batch.newKeys = std::move(newKeys);
    (void)table.AppendBatch(std::move(batch));
}

loglib::LogTable TableFromData(loglib::LogData data, loglib::LogConfigurationManager configuration)
{
    loglib::LogTable table;
    table.Configuration() = std::move(configuration);
    AppendData(table, std::move(data));
    loglib::FinishLoading(table);
    return table;
}

} // namespace test_common_lib
