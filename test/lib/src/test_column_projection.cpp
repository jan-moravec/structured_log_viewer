#include <loglib/bytes_producer.hpp>
#include <loglib/column_projection.hpp>
#include <loglib/exports/export_sink.hpp>
#include <loglib/exports/row_exporter.hpp>
#include <loglib/key_index.hpp>
#include <loglib/log_configuration.hpp>
#include <loglib/log_data.hpp>
#include <loglib/log_line.hpp>
#include <loglib/log_table.hpp>
#include <loglib/log_value.hpp>
#include <loglib/stop_token.hpp>
#include <loglib/stream_line_source.hpp>

#include <catch2/catch_all.hpp>

#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using loglib::Column;
using loglib::ColumnProjection;
using loglib::ColumnType;
using loglib::LogConfiguration;
using loglib::LogConfigurationManager;
using loglib::exports::ExportFormat;
using loglib::exports::RowSource;

namespace
{

Column MakeColumn(std::string header, std::string key, bool visible = true)
{
    return {
        .header = std::move(header),
        .keys = {std::move(key)},
        .printFormat = "{}",
        .type = ColumnType::String,
        .parseFormats = {},
        .visible = visible
    };
}

class MemorySink : public loglib::exports::ExportSink
{
public:
    void Write(std::string_view bytes) override
    {
        mBuffer.append(bytes.data(), bytes.size());
    }
    void Finish() override
    {
    }
    [[nodiscard]] const std::string &Bytes() const noexcept
    {
        return mBuffer;
    }

private:
    std::string mBuffer;
};

loglib::LogTable BuildSparseTable()
{
    auto stream = std::make_unique<loglib::StreamLineSource>(std::filesystem::path("sparse.jsonl"), nullptr);
    stream->AppendLine(R"({"a":"x","c":"z"})", {});
    auto *sourcePtr = stream.get();

    loglib::KeyIndex keys;
    loglib::LogMap map;
    map["a"] = std::string("x");
    map["c"] = std::string("z");
    std::vector<loglib::LogLine> lines;
    lines.emplace_back(map, keys, *sourcePtr, 1);

    loglib::LogData data(std::move(stream), std::move(lines), std::move(keys));

    LogConfiguration config;
    config.columns.push_back(MakeColumn("A", "a"));
    config.columns.push_back(MakeColumn("B", "b"));
    config.columns.push_back(MakeColumn("C", "c"));

    LogConfigurationManager manager;
    manager.SetConfiguration(std::move(config));
    return {std::move(data), std::move(manager)};
}

std::string ExportFormatBytes(
    const loglib::LogTable &table, ExportFormat format, std::span<const std::size_t> columns, bool includeAllJson
)
{
    std::vector<std::size_t> rows = {0};
    std::vector<std::size_t> cols(columns.begin(), columns.end());
    const RowSource source{
        .table = &table,
        .sourceRows = rows,
        .visibleColumns = cols,
        .includeAllFieldsForJson = includeAllJson,
        .includeHeaderRow = true,
    };
    MemorySink sink;
    auto exporter = loglib::exports::MakeExporter(format);
    REQUIRE(exporter != nullptr);
    exporter->Run(source, sink, loglib::StopToken{});
    return sink.Bytes();
}

} // namespace

TEST_CASE("column_projection: all visible columns stay in display order", "[column_projection]")
{
    LogConfiguration config;
    config.columns.push_back(MakeColumn("A", "a"));
    config.columns.push_back(MakeColumn("B", "b"));
    config.columns.push_back(MakeColumn("C", "c"));

    const ColumnProjection projection(config);
    REQUIRE(projection.Size() == 3);
    REQUIRE_FALSE(projection.Empty());
    REQUIRE(projection.Indices() == std::vector<std::size_t>{0, 1, 2});
}

TEST_CASE("column_projection: mixed hidden columns are omitted", "[column_projection]")
{
    LogConfiguration config;
    config.columns.push_back(MakeColumn("A", "a", true));
    config.columns.push_back(MakeColumn("B", "b", false));
    config.columns.push_back(MakeColumn("C", "c", true));

    const ColumnProjection fromConfig(config);
    REQUIRE(fromConfig.Indices() == std::vector<std::size_t>{0, 2});

    const ColumnProjection fromSpan(std::span<const Column>(config.columns));
    REQUIRE(fromSpan.Indices() == fromConfig.Indices());
}

TEST_CASE("column_projection: all hidden yields an empty projection", "[column_projection]")
{
    LogConfiguration config;
    config.columns.push_back(MakeColumn("A", "a", false));
    config.columns.push_back(MakeColumn("B", "b", false));

    const ColumnProjection projection(config);
    REQUIRE(projection.Empty());
    REQUIRE(projection.Size() == 0);
    REQUIRE(projection.Indices().empty());
}

TEST_CASE("column_projection: reorder then project follows display order", "[column_projection]")
{
    LogConfiguration config;
    config.columns.push_back(MakeColumn("A", "a", true));
    config.columns.push_back(MakeColumn("B", "b", false));
    config.columns.push_back(MakeColumn("C", "c", true));

    LogConfigurationManager manager;
    manager.SetConfiguration(std::move(config));
    REQUIRE(ColumnProjection(manager.Configuration()).Indices() == std::vector<std::size_t>{0, 2});

    manager.MoveColumn(2, 0);
    REQUIRE(manager.Configuration().columns[0].header == "C");
    REQUIRE(manager.Configuration().columns[1].header == "A");
    REQUIRE(manager.Configuration().columns[2].header == "B");
    REQUIRE(ColumnProjection(manager.Configuration()).Indices() == std::vector<std::size_t>{0, 1});
}

TEST_CASE("column_projection: SetColumnVisible is the hide/show source", "[column_projection]")
{
    LogConfiguration config;
    config.columns.push_back(MakeColumn("A", "a"));
    config.columns.push_back(MakeColumn("B", "b"));

    LogConfigurationManager manager;
    manager.SetConfiguration(std::move(config));
    manager.SetColumnVisible(1, false);
    REQUIRE(ColumnProjection(manager.Configuration()).Indices() == std::vector<std::size_t>{0});

    manager.SetColumnVisible(1, true);
    REQUIRE(ColumnProjection(manager.Configuration()).Indices() == std::vector<std::size_t>{0, 1});
}

TEST_CASE("column_projection: missing keys stay as empty CSV/Markdown cells", "[column_projection][exports]")
{
    const auto table = BuildSparseTable();
    const ColumnProjection projection(table.Configuration().Configuration());
    REQUIRE(projection.Indices() == std::vector<std::size_t>{0, 1, 2});

    const std::string csv = ExportFormatBytes(table, ExportFormat::Csv, projection.Indices(), false);
    REQUIRE(csv.starts_with("A,B,C\n"));
    REQUIRE(csv.contains("\nx,,z\n"));

    const std::string markdown = ExportFormatBytes(table, ExportFormat::Markdown, projection.Indices(), false);
    REQUIRE(markdown.contains("| A | B | C |"));
    REQUIRE(markdown.contains("| x |  | z |"));
}

TEST_CASE("column_projection: missing keys are omitted from JSON Lines", "[column_projection][exports]")
{
    const auto table = BuildSparseTable();
    const ColumnProjection projection(table.Configuration().Configuration());

    const std::string jsonAll = ExportFormatBytes(table, ExportFormat::JsonLines, projection.Indices(), true);
    REQUIRE(jsonAll.contains("\"a\":\"x\""));
    REQUIRE(jsonAll.contains("\"c\":\"z\""));
    REQUIRE_FALSE(jsonAll.contains("\"b\""));

    const std::string jsonProjected = ExportFormatBytes(table, ExportFormat::JsonLines, projection.Indices(), false);
    REQUIRE(jsonProjected.contains("\"a\":\"x\""));
    REQUIRE(jsonProjected.contains("\"c\":\"z\""));
    REQUIRE_FALSE(jsonProjected.contains("\"b\""));
}
