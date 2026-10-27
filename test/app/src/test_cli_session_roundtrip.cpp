#include "log_model.hpp"
#include "log_table_view.hpp"
#include "main_window.hpp"
#include "qt_streaming_log_sink.hpp"
#include "theme_control.hpp"
#include "uuid_utils.hpp"

#include <loglib/file_line_source.hpp>
#include <loglib/filter_expression.hpp>
#include <loglib/highlight_rule.hpp>
#include <loglib/log_configuration.hpp>
#include <loglib/log_file.hpp>
#include <loglib/log_filter.hpp>
#include <loglib/log_parse_sink.hpp>
#include <loglib/parsers/json_parser.hpp>
#include <loglib/query_parser.hpp>

#include <QAbstractItemModel>
#include <QCoreApplication>
#include <QHeaderView>
#include <QProcess>
#include <QScopedPointer>
#include <QSettings>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#ifndef SLP_EXECUTABLE
#error "SLP_EXECUTABLE must be defined to the built slp binary"
#endif

#ifndef SLP_FIXTURES_DIR
#error "SLP_FIXTURES_DIR must be defined"
#endif

namespace
{

constexpr const char *FOUR_ROWS =
    "{\"ts\":\"2024-01-01T00:00:00Z\",\"level\":\"info\",\"msg\":\"alpha\",\"service\":\"auth\"}\n"
    "{\"ts\":\"2024-01-01T00:00:01Z\",\"level\":\"error\",\"msg\":\"bravo\",\"service\":\"auth\"}\n"
    "{\"ts\":\"2024-01-01T00:00:02Z\",\"level\":\"warn\",\"msg\":\"charlie\",\"service\":\"db\"}\n"
    "{\"ts\":\"2024-01-01T00:00:03Z\",\"level\":\"error\",\"msg\":\"delta\",\"service\":\"db\"}\n";

struct CliResult
{
    int exitCode = -1;
    QByteArray out;
    QByteArray err;
};

[[nodiscard]] CliResult RunSlp(const QStringList &args)
{
    CliResult result;
    QProcess process;
    process.setProgram(QString::fromUtf8(SLP_EXECUTABLE));
    process.setArguments(args);
    process.start();
    if (!process.waitForFinished(30000))
    {
        process.kill();
        process.waitForFinished();
        result.err = "slp timed out";
        return result;
    }
    result.exitCode = process.exitStatus() == QProcess::NormalExit ? process.exitCode() : -1;
    result.out = process.readAllStandardOutput();
    result.err = process.readAllStandardError();
    return result;
}

[[nodiscard]] QStringList QueryArgs(const QString &logPath, const QStringList &extra)
{
    QStringList args{
        QStringLiteral("query"),
        QStringLiteral("--tz"),
        QStringLiteral("UTC"),
        QStringLiteral("--template-dir"),
        QString::fromStdString((std::filesystem::path(SLP_FIXTURES_DIR) / "empty_templates").string()),
    };
    args += extra;
    args.append(logPath);
    return args;
}

[[nodiscard]] int ColumnByHeader(const loglib::LogConfiguration &configuration, std::string_view header)
{
    for (std::size_t i = 0; i < configuration.columns.size(); ++i)
    {
        if (configuration.columns[i].header == header)
        {
            return static_cast<int>(i);
        }
    }
    return -1;
}

/** @brief Message cells of @p model top to bottom. */
[[nodiscard]] QStringList MessageColumn(const QAbstractItemModel &model, int column)
{
    QStringList values;
    for (int row = 0; row < model.rowCount(); ++row)
    {
        values.append(model.index(row, column).data(Qt::DisplayRole).toString());
    }
    return values;
}

/** @brief Message values of the JSON-lines @p out in output order. */
[[nodiscard]] QStringList MessagesFromJsonl(const QByteArray &out)
{
    QStringList values;
    for (const QByteArray &line : out.split('\n'))
    {
        if (line.isEmpty())
        {
            continue;
        }
        const qsizetype key = line.indexOf("\"msg\":\"");
        if (key < 0)
        {
            continue;
        }
        const qsizetype begin = key + 7;
        values.append(QString::fromUtf8(line.mid(begin, line.indexOf('"', begin) - begin)));
    }
    return values;
}

} // namespace

// NOLINTNEXTLINE(misc-use-internal-linkage): QtTest fixture.
class CliSessionRoundTripTest : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName(QStringLiteral("structured-log-viewer-tests"));
        QCoreApplication::setApplicationName(QStringLiteral("apptest_cli_session_roundtrip"));
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QVERIFY(MainWindow::InitializeTimezoneDatabase());
        QVERIFY(std::filesystem::exists(std::string(SLP_EXECUTABLE)));

        QVERIFY(mDir.isValid());
        mLogPath = mDir.filePath(QStringLiteral("four_rows.jsonl"));
        std::ofstream output(mLogPath.toStdString(), std::ios::binary);
        output << FOUR_ROWS;
        output.close();
        QVERIFY(output.good());
    }

    void init()
    {
        QSettings().clear();
        mTheme.reset(new ThemeControl());
        mWindow = new MainWindow(mTheme.data());
        mWindow->SetSuppressDialogsForTest(true);
    }

    void cleanup()
    {
        delete mWindow;
        mWindow = nullptr;
        mTheme.reset();
    }

    // slp --save-session -> GUI Load restores the filter and the sort, and
    // the rows streamed afterwards reproduce the CLI's selection.
    void TestSaveSessionRestoresFilterAndSort()
    {
        // Pin `msg` to a string column: the GUI keeps simple filters only on
        // columns whose type matches the filter, and four rows would otherwise
        // auto-promote `msg` to an enumeration.
        const QString seedPath = WriteSeed(/*pinMsgToString=*/true, /*withRule=*/false);
        const QString session = mDir.filePath(QStringLiteral("cli_session.json"));
        const auto cli = RunSlp(QueryArgs(
            mLogPath,
            {QStringLiteral("--config"),
             seedPath,
             QStringLiteral("--filter"),
             QStringLiteral("msg:l"),
             QStringLiteral("--sort"),
             QStringLiteral("msg:desc"),
             QStringLiteral("-t"),
             QStringLiteral("jsonl"),
             QStringLiteral("--save-session"),
             session}
        ));
        QVERIFY2(cli.exitCode == 0, cli.err.constData());
        const QStringList cliMessages = MessagesFromJsonl(cli.out);
        QCOMPARE(
            cliMessages, (QStringList{QStringLiteral("delta"), QStringLiteral("charlie"), QStringLiteral("alpha")})
        );

        mWindow->LoadConfigurationFromPathForTest(session);
        QCoreApplication::processEvents();

        QCOMPARE(mWindow->LastDroppedFilterCountForTest(), 0);
        QCOMPARE(mWindow->Filters().size(), static_cast<std::size_t>(1));
        QCOMPARE(mWindow->Filters().begin()->second.columnKeys, (std::vector<std::string>{"msg"}));

        auto *model = mWindow->Model();
        const int msgColumn = ColumnByHeader(model->Configuration(), "msg");
        QVERIFY(msgColumn >= 0);
        auto *tableView = mWindow->findChild<LogTableView *>();
        QVERIFY(tableView != nullptr);
        const QHeaderView *header = tableView->horizontalHeader();
        QVERIFY(header->isSortIndicatorShown());
        QCOMPARE(header->sortIndicatorSection(), msgColumn);
        QCOMPARE(header->sortIndicatorOrder(), Qt::DescendingOrder);

        QVERIFY(StreamLogIntoModel());
        QCOMPARE(MessageColumn(*tableView->model(), msgColumn), cliMessages);
    }

    // slp --save-columns -> GUI Load restores the column layout and the
    // highlight rules and leaves filter and sort at their defaults.
    void TestSaveColumnsRestoresLayoutAndRulesOnly()
    {
        const QString seedPath = WriteSeed(/*pinMsgToString=*/false, /*withRule=*/true);
        loglib::LogConfigurationManager seedManager;
        seedManager.Load(seedPath.toStdString());
        const loglib::LogConfiguration &seed = seedManager.Configuration();

        const QString columns = mDir.filePath(QStringLiteral("cli_columns.json"));
        const auto cli = RunSlp(QueryArgs(
            mLogPath,
            {QStringLiteral("--config"),
             seedPath,
             QStringLiteral("--filter"),
             QStringLiteral("level:error"),
             QStringLiteral("--sort"),
             QStringLiteral("msg"),
             QStringLiteral("--columns"),
             QStringLiteral("msg,level"),
             QStringLiteral("--count"),
             QStringLiteral("--save-columns"),
             columns}
        ));
        QVERIFY2(cli.exitCode == 0, cli.err.constData());
        QCOMPARE(cli.out.trimmed(), QByteArray("2"));

        mWindow->LoadConfigurationFromPathForTest(columns);
        QCoreApplication::processEvents();

        const loglib::LogConfiguration &loaded = mWindow->Model()->Configuration();
        QCOMPARE(loaded.columns.size(), seed.columns.size());
        QCOMPARE(loaded.columns[0].header, std::string("msg"));
        QCOMPARE(loaded.columns[1].header, std::string("level"));
        QVERIFY(loaded.columns[0].visible);
        QVERIFY(loaded.columns[1].visible);
        QVERIFY(!loaded.columns.back().visible);
        QCOMPARE(loaded.highlightRules.size(), static_cast<std::size_t>(1));
        QCOMPARE(loaded.highlightRules[0].name, std::string("errors"));

        QVERIFY(mWindow->Filters().empty());
        QCOMPARE(loaded.sort.columnIndex, -1);
        auto *tableView = mWindow->findChild<LogTableView *>();
        QVERIFY(tableView != nullptr);

        // Streaming afterwards shows every row in source order.
        QVERIFY(StreamLogIntoModel());
        const int msgColumn = ColumnByHeader(loaded, "msg");
        QVERIFY(msgColumn >= 0);
        QCOMPARE(
            MessageColumn(*tableView->model(), msgColumn),
            (QStringList{
                QStringLiteral("alpha"), QStringLiteral("bravo"), QStringLiteral("charlie"), QStringLiteral("delta")
            })
        );
    }

    // GUI-saved session -> slp query --config selects and orders the same rows.
    void TestGuiSessionReproducedByQueryConfig()
    {
        QVERIFY(StreamLogIntoModel());
        auto *model = mWindow->Model();
        const int msgColumn = ColumnByHeader(model->Configuration(), "msg");
        QVERIFY(msgColumn >= 0);

        QMetaObject::invokeMethod(
            mWindow,
            "FilterSubmitted",
            Qt::DirectConnection,
            Q_ARG(QString, QStringLiteral("roundtrip-filter")),
            Q_ARG(int, msgColumn),
            Q_ARG(QString, QStringLiteral("l")),
            Q_ARG(int, static_cast<int>(loglib::LeafRule::Match::Contains))
        );
        auto *tableView = mWindow->findChild<LogTableView *>();
        QVERIFY(tableView != nullptr);
        tableView->sortByColumn(msgColumn, Qt::DescendingOrder);
        QCoreApplication::processEvents();

        const QStringList guiMessages = MessageColumn(*tableView->model(), msgColumn);
        QCOMPARE(
            guiMessages, (QStringList{QStringLiteral("delta"), QStringLiteral("charlie"), QStringLiteral("alpha")})
        );

        const QString session = mDir.filePath(QStringLiteral("gui_session.json"));
        mWindow->SaveConfigurationToPathForTest(session, loglib::SaveScope::Full);

        const auto cli = RunSlp(
            QueryArgs(mLogPath, {QStringLiteral("--config"), session, QStringLiteral("-t"), QStringLiteral("jsonl")})
        );
        QVERIFY2(cli.exitCode == 0, cli.err.constData());
        QCOMPARE(MessagesFromJsonl(cli.out), guiMessages);
    }

    // slp bundle -> GUI opens the bundle with every row retained and the
    // anchors, highlight rules, filter, sort, and column set intact.
    void TestBundleOpensWithEmbeddedSession()
    {
        const QString seedPath = WriteSeed(/*pinMsgToString=*/true, /*withRule=*/true);
        QVERIFY(!seedPath.isEmpty());
        loglib::LogConfigurationManager seedManager;
        seedManager.Load(seedPath.toStdString());
        loglib::LogConfiguration seed = seedManager.Configuration();
        // Anchor the second source row by its canonical source locator.
        seed.anchors = {
            {.locator = logapp::CanonicalLocator(mLogPath).toStdString(), .lineId = 1, .colorIndex = 0, .note = "kept"}
        };
        const QString anchoredSeed = mDir.filePath(QStringLiteral("anchored_seed.json"));
        loglib::LogConfigurationManager::Save(seed, anchoredSeed.toStdString(), loglib::SaveScope::Full);

        const QString bundle = mDir.filePath(QStringLiteral("cli.slvbundle"));
        QStringList args{
            QStringLiteral("bundle"),
            QStringLiteral("--tz"),
            QStringLiteral("UTC"),
            QStringLiteral("--template-dir"),
            QString::fromStdString((std::filesystem::path(SLP_FIXTURES_DIR) / "empty_templates").string()),
            QStringLiteral("--config"),
            anchoredSeed,
            QStringLiteral("--filter"),
            QStringLiteral("msg:l"),
            QStringLiteral("--sort"),
            QStringLiteral("msg:desc"),
            QStringLiteral("--columns"),
            QStringLiteral("msg,level"),
            QStringLiteral("-o"),
            bundle,
            mLogPath,
        };
        const auto cli = RunSlp(args);
        QVERIFY2(cli.exitCode == 0, cli.err.constData());
        QVERIFY(std::filesystem::exists(bundle.toStdString()));

        QSignalSpy finishedSpy(mWindow->Model(), &LogModel::streamingFinished);
        QVERIFY(finishedSpy.isValid());
        mWindow->OpenMixedFilesForTest({bundle}, MainWindow::OpenMode::Replace);
        QVERIFY(finishedSpy.wait(5000) || finishedSpy.count() > 0);
        QCoreApplication::processEvents();

        auto *model = mWindow->Model();
        // Non-destructive: the filtered-out row is still loaded.
        QCOMPARE(model->rowCount(), 4);

        const loglib::LogConfiguration &loaded = model->Configuration();
        QCOMPARE(loaded.columns[0].header, std::string("msg"));
        QCOMPARE(loaded.columns[1].header, std::string("level"));
        QVERIFY(loaded.columns[0].visible);
        QVERIFY(loaded.columns[1].visible);
        QVERIFY(!loaded.columns.back().visible);

        QCOMPARE(loaded.highlightRules.size(), static_cast<std::size_t>(1));
        QCOMPARE(loaded.highlightRules[0].name, std::string("errors"));

        QCOMPARE(mWindow->Filters().size(), static_cast<std::size_t>(1));
        QCOMPARE(mWindow->Filters().begin()->second.columnKeys, (std::vector<std::string>{"msg"}));

        auto *tableView = mWindow->findChild<LogTableView *>();
        QVERIFY(tableView != nullptr);
        const int msgColumn = ColumnByHeader(loaded, "msg");
        QVERIFY(msgColumn >= 0);
        const QHeaderView *header = tableView->horizontalHeader();
        QVERIFY(header->isSortIndicatorShown());
        QCOMPARE(header->sortIndicatorSection(), msgColumn);
        QCOMPARE(header->sortIndicatorOrder(), Qt::DescendingOrder);
        QCOMPARE(
            MessageColumn(*tableView->model(), msgColumn),
            (QStringList{QStringLiteral("delta"), QStringLiteral("charlie"), QStringLiteral("alpha")})
        );

        QCOMPARE(loaded.anchors.size(), static_cast<std::size_t>(1));
        QVERIFY(mWindow->Anchors() != nullptr);
        QCOMPARE(mWindow->Anchors()->Count(), 1);
        QVERIFY(model->AnchorSlotForRow(1).has_value());
    }

private:
    /**
     * @brief Writes a full session seeded from the columns slp detects for the log.
     *
     * @param pinMsgToString Pins `msg` to a non-auto-detected string column.
     * @param withRule Adds one highlight rule on `level`.
     */
    [[nodiscard]] QString WriteSeed(bool pinMsgToString, bool withRule) const
    {
        const QString probePath = mDir.filePath(QStringLiteral("probe.json"));
        const auto probe =
            RunSlp(QueryArgs(mLogPath, {QStringLiteral("--save-columns"), probePath, QStringLiteral("--count")}));
        if (probe.exitCode != 0)
        {
            qWarning() << probe.err;
            return {};
        }
        loglib::LogConfigurationManager manager;
        manager.Load(probePath.toStdString());
        loglib::LogConfiguration seed = manager.Configuration();
        if (pinMsgToString)
        {
            const int msg = ColumnByHeader(seed, "msg");
            seed.columns[static_cast<std::size_t>(msg)].type = loglib::ColumnType::String;
            seed.columns[static_cast<std::size_t>(msg)].autoDetect = false;
        }
        if (withRule)
        {
            loglib::HighlightRule rule;
            rule.name = "errors";
            rule.columnKeys = {"level"};
            rule.type = loglib::HighlightRule::Type::Enumeration;
            rule.filterValues = {"Error"};
            rule.backgroundIndex = 1;
            seed.highlightRules.push_back(rule);
        }
        const QString seedPath = mDir.filePath(QStringLiteral("seed.json"));
        loglib::LogConfigurationManager::Save(seed, seedPath.toStdString(), loglib::SaveScope::Full);
        return seedPath;
    }

    /** @brief Streams the four-row log into the window's model, as `OpenFiles` would. */
    [[nodiscard]] bool StreamLogIntoModel()
    {
        auto *model = mWindow->Model();
        QSignalSpy finishedSpy(model, &LogModel::streamingFinished);
        if (!finishedSpy.isValid())
        {
            return false;
        }
        auto file = std::make_unique<loglib::LogFile>(mLogPath.toStdString());
        auto fileSource = std::make_unique<loglib::FileLineSource>(std::move(file));
        loglib::FileLineSource *parseSource = fileSource.get();
        const loglib::StopToken stopToken = model->BeginStreamingForSyncTest(std::move(fileSource));

        loglib::ParserOptions options;
        options.stopToken = stopToken;
        options.threads = 1;
        loglib::LogParseSink &sink = *model->Sink();
        const loglib::JsonParser parser;
        parser.ParseStreaming(*parseSource, sink, options);
        if (finishedSpy.count() == 0 && !finishedSpy.wait(5000))
        {
            return false;
        }
        QCoreApplication::processEvents();
        return true;
    }

    QTemporaryDir mDir;
    QString mLogPath;
    QScopedPointer<ThemeControl> mTheme;
    MainWindow *mWindow = nullptr;
};

QTEST_MAIN(CliSessionRoundTripTest)
#include "test_cli_session_roundtrip.moc"
