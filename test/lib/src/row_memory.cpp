#include "row_memory.hpp"

#include "fixture_params.hpp"

#include <loglib/core/enum_dictionary.hpp>
#include <loglib/core/file_line_source.hpp>
#include <loglib/core/key_index.hpp>
#include <loglib/core/log_line.hpp>
#include <loglib/core/stream_line_source.hpp>

#include <test_common/log_format.hpp>

#include <stdexcept>
#include <string>

namespace bench
{

RowMemory MeasureRowMemory(const loglib::LogTable &table)
{
    RowMemory memory;
    const loglib::LogData &data = table.Data();
    memory.rows = data.Lines().size();
    memory.lineObjects = memory.rows * sizeof(loglib::LogLine);
    for (const loglib::LogLine &line : data.Lines())
    {
        memory.ownedByRows += line.OwnedMemoryBytes();
    }
    if (const loglib::FileLineSource *file = data.FrontFileSource(); file != nullptr)
    {
        memory.fieldArenas = file->FieldArenasMemoryBytes();
        memory.lineOffsets = file->LineOffsetsMemoryBytes();
        memory.arena = file->OwnedStringsMemoryBytes();
    }
    else if (const loglib::StreamLineSource *stream = data.BackStreamSource(); stream != nullptr)
    {
        // A stream keeps the text of each line and its decoded strings.
        memory.arena = stream->OwnedMemoryBytes();
    }
    memory.keyIndex = data.Keys().EstimatedMemoryBytes();
    memory.enumDictionaries = table.EnumDictionaries().EstimatedMemoryBytes();
    return memory;
}

GeneratedJsonTable LoadGeneratedJsonTable(std::size_t rows, const std::string &fileName)
{
    GeneratedJsonTable result;
    result.file = std::make_unique<TestStructuredLogFile>(
        StreamedRecords{
            .count = rows,
            .seed = LARGE_FIXTURE_SEED,
            .timestamps = DeterministicBenchmarkTimestamps(),
        },
        test_common::JsonLines(),
        test_common::RecordSchema{},
        fileName
    );
    InitializeTimezoneData();
    result.loaded = std::make_unique<loglib::LoadResult>(loglib::LoadFile(result.file->GetFilePath()));
    if (!result.loaded->errors.empty() || result.loaded->table.RowCount() != rows)
    {
        throw std::runtime_error("the generated JSON file did not load");
    }
    return result;
}

} // namespace bench
