#include "log_model.hpp"
#include "qt_streaming_log_sink.hpp"

#include <loglib/exports/export_sink.hpp>
#include <loglib/exports/row_exporter.hpp>
#include <loglib/file_line_source.hpp>
#include <loglib/filter_expression.hpp>
#include <loglib/leaf_rule_compile.hpp>
#include <loglib/log_file.hpp>
#include <loglib/log_filter.hpp>
#include <loglib/log_parse_sink.hpp>
#include <loglib/parsers/json_parser.hpp>
#include <loglib/query_parser.hpp>
#include <loglib/row_ordering.hpp>

#include <QProcess>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#ifndef SLP_EXECUTABLE
#error "SLP_EXECUTABLE must be defined to the built slp binary"
#endif

#ifndef SLP_FIXTURES_DIR
#error "SLP_FIXTURES_DIR must be defined"
#endif

namespace
{

[[nodiscard]] std::filesystem::path FixturePath()
{
    return std::filesystem::path(SLP_FIXTURES_DIR) / "two_lines.jsonl";
}

[[nodiscard]] std::filesystem::path EmptyTemplateDir()
{
    return std::filesystem::path(SLP_FIXTURES_DIR) / "empty_templates";
}

[[nodiscard]] std::string ReadFile(const std::filesystem::path &path)
{
    std::ifstream input(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

[[nodiscard]] std::string DescribeByteDiff(std::string_view expected, std::string_view actual)
{
    std::size_t offset = 0;
    const std::size_t limit = std::min(expected.size(), actual.size());
    while (offset < limit && expected[offset] == actual[offset])
    {
        ++offset;
    }
    std::ostringstream message;
    message << "first difference at offset " << offset << " (gui size " << expected.size() << ", cli size "
            << actual.size() << ")";
    return message.str();
}

[[nodiscard]] bool StreamFixtureInto(LogModel &model, QString *error)
{
    QSignalSpy finishedSpy(&model, &LogModel::streamingFinished);
    if (!finishedSpy.isValid())
    {
        *error = QStringLiteral("streamingFinished spy is invalid");
        return false;
    }

    auto file = std::make_unique<loglib::LogFile>(FixturePath());
    auto fileSource = std::make_unique<loglib::FileLineSource>(std::move(file));
    loglib::FileLineSource *parseSource = fileSource.get();
    const loglib::StopToken stopToken = model.BeginStreamingForSyncTest(std::move(fileSource));

    loglib::ParserOptions options;
    options.stopToken = stopToken;
    options.threads = 1;
    loglib::LogParseSink &sink = *model.Sink();
    loglib::JsonParser{}.ParseStreaming(*parseSource, sink, options);

    const bool finished = finishedSpy.count() > 0 || finishedSpy.wait(5000);
    if (!finished)
    {
        *error = QStringLiteral("streamingFinished must arrive within the timeout");
        return false;
    }
    model.EndStreaming(false);
    return true;
}

[[nodiscard]] std::optional<std::string> ExportGuiBytes(
    LogModel &model,
    loglib::exports::ExportFormat format,
    const std::optional<std::string> &filter,
    const std::optional<std::string> &sort,
    const std::vector<std::size_t> &visibleColumns,
    bool includeHeader,
    QString *error
)
{
    loglib::FilterExpression expression{};
    if (filter.has_value())
    {
        auto parsed = loglib::ParseQuery(*filter);
        if (!parsed.has_value())
        {
            *error = QStringLiteral("filter must parse");
            return std::nullopt;
        }
        expression = std::move(*parsed);
    }
    const auto compiled = loglib::CompileExpression(expression, model.Configuration().columns, &model.Table());
    std::vector<std::size_t> rows = loglib::FilterAcceptedRows(model.Table(), compiled);

    if (sort.has_value())
    {
        std::string_view sortSpec = *sort;
        loglib::SortDirection direction = loglib::SortDirection::Ascending;
        const std::size_t colon = sortSpec.rfind(':');
        if (colon != std::string_view::npos)
        {
            const std::string_view dir = sortSpec.substr(colon + 1);
            if (dir == "desc" || dir == "DESC")
            {
                direction = loglib::SortDirection::Descending;
                sortSpec = sortSpec.substr(0, colon);
            }
            else if (dir == "asc" || dir == "ASC")
            {
                sortSpec = sortSpec.substr(0, colon);
            }
        }
        int columnIndex = -1;
        const auto &columns = model.Configuration().columns;
        for (std::size_t i = 0; i < columns.size(); ++i)
        {
            if (columns[i].header == sortSpec)
            {
                columnIndex = static_cast<int>(i);
                break;
            }
        }
        if (columnIndex < 0)
        {
            *error = QStringLiteral("sort column not found");
            return std::nullopt;
        }
        const loglib::RowOrdering ordering(static_cast<std::size_t>(columnIndex), direction, nullptr);
        rows = ordering.Permute(model.Table(), std::span<const std::size_t>{rows});
    }

    QTemporaryDir temp;
    if (!temp.isValid())
    {
        *error = QStringLiteral("temporary directory is invalid");
        return std::nullopt;
    }
    const std::filesystem::path destination = std::filesystem::path(temp.path().toStdString()) / "export.out";

    loglib::exports::ExportPlan plan;
    plan.format = format;
    plan.sourceRows = std::move(rows);
    plan.visibleColumns = visibleColumns;
    plan.includeAllFieldsForJson = true;
    plan.includeHeaderRow = includeHeader;
    plan.table = &model.Table();
    plan.destination = destination;

    auto exporter = loglib::exports::MakeExporter(plan.format);
    if (exporter == nullptr)
    {
        *error = QStringLiteral("exporter is null");
        return std::nullopt;
    }
    loglib::exports::FileSink sink(destination);
    exporter->Run(plan.View(), sink, {});
    sink.Finish();
    return ReadFile(destination);
}

[[nodiscard]] std::optional<std::string> RunCliOnce(const QStringList &args, QString *error)
{
    QProcess process;
    process.setProgram(QString::fromUtf8(SLP_EXECUTABLE));
    process.setArguments(args);
    process.start();
    if (!process.waitForFinished(30000))
    {
        *error = QStringLiteral("slp timed out");
        return std::nullopt;
    }
    if (process.exitStatus() != QProcess::NormalExit)
    {
        *error = QStringLiteral("slp crashed");
        return std::nullopt;
    }
    const QByteArray out = process.readAllStandardOutput();
    const QByteArray err = process.readAllStandardError();
    if (process.exitCode() != 0)
    {
        *error = QStringLiteral("slp exited %1: %2").arg(process.exitCode()).arg(QString::fromUtf8(err));
        return std::nullopt;
    }
    return std::string(out.constData(), static_cast<std::size_t>(out.size()));
}

[[nodiscard]] int FindColumn(const loglib::LogConfiguration &configuration, std::string_view name)
{
    for (std::size_t i = 0; i < configuration.columns.size(); ++i)
    {
        if (configuration.columns[i].header == name)
        {
            return static_cast<int>(i);
        }
        for (const std::string &key : configuration.columns[i].keys)
        {
            if (key == name)
            {
                return static_cast<int>(i);
            }
        }
    }
    return -1;
}

void CompareParity(std::string_view label, const std::string &gui, const std::string &cli)
{
    if (gui != cli)
    {
        QFAIL(qPrintable(QStringLiteral("%1: %2")
                             .arg(QString::fromUtf8(label.data(), static_cast<qsizetype>(label.size())))
                             .arg(QString::fromStdString(DescribeByteDiff(gui, cli)))));
    }
}

} // namespace

// NOLINTNEXTLINE(misc-use-internal-linkage): QtTest fixture.
class CliExportParityTest : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        QVERIFY(std::filesystem::exists(FixturePath()));
        QVERIFY(std::filesystem::exists(QString::fromUtf8(SLP_EXECUTABLE).toStdString()));
    }

    void TestJsonlMatchesGuiExport()
    {
        LogModel model;
        QString error;
        QVERIFY2(StreamFixtureInto(model, &error), qPrintable(error));

        const auto gui = ExportGuiBytes(
            model, loglib::exports::ExportFormat::JsonLines, std::nullopt, std::nullopt, {}, true, &error
        );
        QVERIFY2(gui.has_value(), qPrintable(error));
        const auto cli = RunCliOnce(
            {QStringLiteral("query"),
             QStringLiteral("--tz"),
             QStringLiteral("UTC"),
             QStringLiteral("--template-dir"),
             QString::fromStdString(EmptyTemplateDir().string()),
             QStringLiteral("-t"),
             QStringLiteral("jsonl"),
             QString::fromStdString(FixturePath().string())},
            &error
        );
        QVERIFY2(cli.has_value(), qPrintable(error));
        CompareParity("jsonl", *gui, *cli);
    }

    void TestCsvProjectedColumnsMatchGuiExport()
    {
        LogModel model;
        QString error;
        QVERIFY2(StreamFixtureInto(model, &error), qPrintable(error));
        const int level = FindColumn(model.Configuration(), "level");
        const int msg = FindColumn(model.Configuration(), "msg");
        QVERIFY(level >= 0);
        QVERIFY(msg >= 0);

        const auto gui = ExportGuiBytes(
            model,
            loglib::exports::ExportFormat::Csv,
            std::nullopt,
            std::nullopt,
            {static_cast<std::size_t>(level), static_cast<std::size_t>(msg)},
            true,
            &error
        );
        QVERIFY2(gui.has_value(), qPrintable(error));
        const auto cli = RunCliOnce(
            {QStringLiteral("query"),
             QStringLiteral("--tz"),
             QStringLiteral("UTC"),
             QStringLiteral("--template-dir"),
             QString::fromStdString(EmptyTemplateDir().string()),
             QStringLiteral("-t"),
             QStringLiteral("csv"),
             QStringLiteral("--columns"),
             QStringLiteral("level,msg"),
             QString::fromStdString(FixturePath().string())},
            &error
        );
        QVERIFY2(cli.has_value(), qPrintable(error));
        CompareParity("csv", *gui, *cli);
    }

    void TestMarkdownFilteredMatchGuiExport()
    {
        LogModel model;
        QString error;
        QVERIFY2(StreamFixtureInto(model, &error), qPrintable(error));
        const int level = FindColumn(model.Configuration(), "level");
        const int msg = FindColumn(model.Configuration(), "msg");
        QVERIFY(level >= 0);
        QVERIFY(msg >= 0);

        const auto gui = ExportGuiBytes(
            model,
            loglib::exports::ExportFormat::Markdown,
            std::string{"level:error"},
            std::nullopt,
            {static_cast<std::size_t>(level), static_cast<std::size_t>(msg)},
            true,
            &error
        );
        QVERIFY2(gui.has_value(), qPrintable(error));
        const auto cli = RunCliOnce(
            {QStringLiteral("query"),
             QStringLiteral("--tz"),
             QStringLiteral("UTC"),
             QStringLiteral("--template-dir"),
             QString::fromStdString(EmptyTemplateDir().string()),
             QStringLiteral("--filter"),
             QStringLiteral("level:error"),
             QStringLiteral("-t"),
             QStringLiteral("markdown"),
             QStringLiteral("--columns"),
             QStringLiteral("level,msg"),
             QString::fromStdString(FixturePath().string())},
            &error
        );
        QVERIFY2(cli.has_value(), qPrintable(error));
        CompareParity("markdown", *gui, *cli);
    }
};

QTEST_MAIN(CliExportParityTest)
#include "test_cli_export_parity.moc"
