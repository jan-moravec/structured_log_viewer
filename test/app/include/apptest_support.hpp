/**
 * @brief Shared fixtures and helpers for the GUI test executables.
 *
 * The GUI tests are split into several QtTest executables by area. This header holds what they share: temporary fixture
 * files, runners that push a file through the streaming pipeline into a LogModel, synthetic row batches, lookups of
 * actions and menu entries, and AppTestBase, the QtTest base class that owns a MainWindow and a ThemeControl for each
 * test function.
 *
 * It is built into the apptest_support static library, which every GUI test executable links.
 */
#pragma once

#include "advanced_filter_editor.hpp"
#include "apptest_wait.hpp"
#include "log_model.hpp"
#include "main_window.hpp"
#include "qstring_path.hpp"
#include "session_history_manager.hpp"
#include "theme_control.hpp"

#include <loglib/core/bytes_producer.hpp>
#include <loglib/core/compact_log_value.hpp>
#include <loglib/core/key_index.hpp>
#include <loglib/core/stream_line_source.hpp>
#include <loglib/output/file_loading.hpp>
#include <loglib/parse/log_parse_sink.hpp>
#include <loglib/parse/parsers/json_parser.hpp>
#include <loglib/schema/filter_expression.hpp>
#include <loglib/schema/log_configuration.hpp>

#include <QAction>
#include <QByteArray>
#include <QClipboard>
#include <QFile>
#include <QFileInfo>
#include <QMainWindow>
#include <QMenu>
#include <QObject>
#include <QPushButton>
#include <QScopedPointer>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QtTest/QtTest>

#include <chrono>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace apptest_support
{

/// Saves the columns of @p model as a configuration file.
void SaveLayoutForTest(const LogModel &model, const std::string &path);

/// Replaces the columns of @p model with those of a configuration file.
void LoadLayoutForTest(LogModel &model, const std::string &path);

/// Extract top-level `LeafRule` children from a `FilterExpression`.
/// Test-only shim: the historical simple-mode UX only ever wrote
/// flat AND trees, so this covers `LegacyLeavesOf` for the app
/// tests without duplicating them.
[[nodiscard]] inline std::vector<loglib::LeafRule> WireLeavesOf(const loglib::FilterExpression &expression)
{
    std::vector<loglib::LeafRule> out;
    if (const auto *leaf = std::get_if<loglib::FilterExpression::Leaf>(&expression.node); leaf != nullptr)
    {
        out.push_back(leaf->rule);
        return out;
    }
    if (const auto *asAnd = std::get_if<loglib::FilterExpression::And>(&expression.node); asAnd != nullptr)
    {
        for (const auto &child : asAnd->children)
        {
            if (const auto *childLeaf = std::get_if<loglib::FilterExpression::Leaf>(&child.node); childLeaf != nullptr)
            {
                out.push_back(childLeaf->rule);
            }
        }
    }
    return out;
}

/// Collect every `LeafRule` in the tree, at any depth. Needed where
/// `WireLeavesOf`'s top-level-only view is too shallow -- e.g. a leaf
/// the simple editor can't represent, which
/// `ApplyAdvancedFilterResult` nests one level down to keep it out of
/// `mSimpleLeaves`.
[[nodiscard]] inline std::vector<loglib::LeafRule> AllLeavesOf(const loglib::FilterExpression &expression)
{
    std::vector<loglib::LeafRule> out;
    // The lambda calls itself via the `self`-parameter trick to
    // walk the boolean AST recursively; misc-no-recursion has no
    // way to reason about `auto` self-parameters, hence the NOLINT.
    // NOLINTNEXTLINE(misc-no-recursion)
    const auto visit = [&out](const loglib::FilterExpression &node, const auto &self) -> void {
        if (const auto *leaf = std::get_if<loglib::FilterExpression::Leaf>(&node.node); leaf != nullptr)
        {
            out.push_back(leaf->rule);
        }
        else if (const auto *asAnd = std::get_if<loglib::FilterExpression::And>(&node.node); asAnd != nullptr)
        {
            for (const auto &child : asAnd->children)
            {
                self(child, self);
            }
        }
        else if (const auto *asOr = std::get_if<loglib::FilterExpression::Or>(&node.node); asOr != nullptr)
        {
            for (const auto &child : asOr->children)
            {
                self(child, self);
            }
        }
        else if (
            const auto *asNot = std::get_if<loglib::FilterExpression::Not>(&node.node);
            asNot != nullptr && asNot->child != nullptr
        )
        {
            self(*asNot->child, self);
        }
    };
    visit(expression, visit);
    return out;
}

/// Install @p leaves as a top-level `And` expression.
[[nodiscard]] inline loglib::FilterExpression WireLeavesAsExpression(std::vector<loglib::LeafRule> leaves)
{
    loglib::FilterExpression::And node;
    node.children.reserve(leaves.size());
    for (auto &rule : leaves)
    {
        loglib::FilterExpression child;
        child.node = loglib::FilterExpression::Leaf{std::move(rule)};
        node.children.push_back(std::move(child));
    }
    loglib::FilterExpression expression;
    expression.node = std::move(node);
    return expression;
}

// Tiny RAII helper that writes a JSONL fixture into a QTemporaryDir.
// Kept in-TU to avoid pulling library-test fixtures (and their Catch2
// dependency) into `apptest`.
// In-memory storage backend for `SessionHistoryManager` tests so each
// case stays isolated from `QSettings` and the user's profile.
class InMemoryRecentsIndexStorage final : public IRecentsIndexStorage
{
public:
    QList<RecentSessionEntry> Read() const override
    {
        return mEntries;
    }

    void Write(const QList<RecentSessionEntry> &entries) override
    {
        mEntries = entries;
    }

    std::optional<QString> ReadLastUuid() const override
    {
        return mLastUuid;
    }

    void WriteLastUuid(const std::optional<QString> &uuid) override
    {
        mLastUuid = uuid;
    }

private:
    QList<RecentSessionEntry> mEntries;
    std::optional<QString> mLastUuid;
};

class TempJsonFile
{
public:
    explicit TempJsonFile(const QStringList &lines)
    {
        QVERIFY2(mDir.isValid(), "QTemporaryDir creation must succeed");
        mPath = mDir.filePath("parity.json");
        std::ofstream stream(mPath.toStdString(), std::ios::binary);
        QVERIFY2(stream.is_open(), "fixture file must be openable");
        for (const QString &line : lines)
        {
            stream << line.toStdString() << '\n';
        }
    }

    QString Path() const
    {
        return mPath;
    }

private:
    QTemporaryDir mDir;
    QString mPath;
};

QString WriteBundleFixture(
    const QTemporaryDir &directory,
    const QStringList &lines,
    const std::function<void(loglib::LogConfiguration &)> &configure
);

// Pumps events until the model's parse is armed, then stops before any later event runs. Files open on a worker, so
// the parse starts from an event; a parse that finishes can only be reported by a later event.
[[nodiscard]] bool PumpUntilStreamingActive(const LogModel *model);

// Gzip-compress @p bytes into a self-contained gzip stream
// (windowBits = 15 + 16). In-TU helper for the transparent-
// decompression regression suite; the codec-level round-trip lives
// in `test_decompressing_byte_source.cpp`.
//
// Uses `qFatal` (not `QVERIFY2`, which expands to `return;` and is
// ill-formed in a value-returning function). A zlib init failure at
// fixture time means the whole suite is broken.
QByteArray GzipCompressForTest(const QByteArray &bytes);

// Write @p bytes to @p path (binary, truncating). Used with
// `GzipCompressForTest` to stage compressed fixtures without
// pulling in the library-tests' `TempBinaryFile`. Same qFatal
// rationale as above.
void WriteBinaryForTest(const QString &path, const QByteArray &bytes);

// Returns a deterministic JSONL fixture used by the
// `*AboutToBeInserted` regression test below. Keys are intentionally
// alphabetic so the parser's per-batch insertion order matches a
// sorted-order column layout, which keeps the assertion shape simple.
QStringList MakeParityFixture();

// Extracts a Qt resource (e.g. ":/fixtures/empty.jsonl") into a unique
// QTemporaryDir so the parser can mmap it. The directory and the extracted
// copy live for the FixtureFile lifetime.
class FixtureFile
{
public:
    explicit FixtureFile(const QString &resourcePath)
    {
        QVERIFY2(mDir.isValid(), "QTemporaryDir creation must succeed");
        QFile in(resourcePath);
        QVERIFY2(
            in.open(QFile::ReadOnly),
            qPrintable(QStringLiteral("embedded fixture %1 must be readable").arg(resourcePath))
        );
        mPath = mDir.filePath(QFileInfo(resourcePath).fileName());
        QFile out(mPath);
        QVERIFY2(
            out.open(QFile::WriteOnly),
            qPrintable(QStringLiteral("fixture extraction file %1 must be writable").arg(mPath))
        );
        out.write(in.readAll());
    }

    QString Path() const
    {
        return mPath;
    }

private:
    QTemporaryDir mDir;
    QString mPath;
};

// Result of running one fixture through the streaming pipeline. `cancelled`
// is true iff the streaming pipeline reported `StreamingResult::Cancelled`
// (the externally-stopped path); `Success` and `Failed` both produce
// `cancelled == false`. Tests that need to distinguish the latter two
// should read the captured `StreamingResult` directly.
struct StreamingRun
{
    std::unique_ptr<LogModel> model;
    qsizetype finishedCount = 0;
    bool cancelled = false;
};

// Drives one fixture through `LogModel::BeginStreaming` +
// `JsonParser::Parse` + `QtStreamingLogSink`, mirroring
// `MainWindow::OpenJsonStreaming` but on the calling thread. Pinned to
// `threads=1` so per-batch newKeys / cell ordering is deterministic.
StreamingRun RunStreaming(const QString &fixturePath, ThemeControl *theme = nullptr);

// Logfmt sibling of `RunStreaming` (same shape so assertions can
// match the JSON counterparts line-for-line).
StreamingRun RunStreamingLogfmt(const QString &fixturePath, ThemeControl *theme = nullptr);

// Loads a file into a LogModel the way the GUI does for a static file, on the calling thread with the default
// number of parser threads. Returns the model, which owns the table.
std::unique_ptr<LogModel> RunStaticLoadInModel(const QString &fixturePath, const loglib::LogParser &parser);

// A fixture larger than one parse batch, so the comparison also covers keys and columns that arrive in later batches.
QString WriteEquivalenceFixture(const QTemporaryDir &directory, const QString &name, const std::string &content);

struct EquivalenceFixtures
{
    std::string json;
    std::string logfmt;
    std::string csv;
};

// Rows with a timestamp, a level, a few repeated strings and numbers. One row in five carries one more key.
EquivalenceFixtures MakeEquivalenceFixtures();

// A producer whose Read throws, to drive a parse that fails part-way.
class ThrowingBytesProducer final : public loglib::BytesProducer
{
public:
    std::size_t Read(std::span<char> /*buffer*/) override
    {
        throw std::runtime_error("producer failed");
    }

    void WaitForBytes(std::chrono::milliseconds /*timeout*/) override
    {
    }

    void Stop() noexcept override
    {
    }

    [[nodiscard]] bool IsClosed() const noexcept override
    {
        return false;
    }

    [[nodiscard]] std::string DisplayName() const override
    {
        return "throwing";
    }
};

// A parser that cannot be built: the factory throws before any parse starts.
std::unique_ptr<loglib::LogParser> ThrowingParserFactory();

// RAII helper for the live-tail smoke test: a temp directory that lives
// for the lifetime of the test, plus an `Append` helper that opens, writes,
// and flushes a string to a file in a single call. We don't share the
// `TempJsonFile` helper because it pre-writes a fixed line list at
// construction; the live-tail flow needs to *grow* the file across the
// test's runtime so the parser actually exercises its tailing path
// (rather than draining a static fixture).
class TempLiveTailFile
{
public:
    TempLiveTailFile()
    {
        QVERIFY2(mDir.isValid(), "QTemporaryDir creation must succeed");
        mPath = mDir.filePath("live.jsonl");
        // Touch the file so `TailingBytesProducer`'s open() succeeds. Pre-fill
        // walks the existing content (zero bytes here) before tailing.
        const std::ofstream stream(mPath.toStdString(), std::ios::binary);
        QVERIFY2(stream.is_open(), "live-tail fixture file must be openable");
    }

    QString Path() const
    {
        return mPath;
    }

    /// Append @p bytes and flush. The producer side of the live-tail flow.
    void Append(const std::string &bytes) const
    {
        std::ofstream stream(mPath.toStdString(), std::ios::binary | std::ios::app);
        QVERIFY2(stream.is_open(), "live-tail append must succeed");
        stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        stream.flush();
    }

private:
    QTemporaryDir mDir;
    QString mPath;
};

// Wait until the model's `lineCountChanged` signal reports at least
// @p target rows, draining the Qt event queue while we wait so the
// QueuedConnection lambdas the parser worker posts actually run. Returns
// `true` on success, `false` if the deadline expires (the caller
// usually `QVERIFY2`s the result).
bool WaitForLineCount(LogModel &model, qsizetype target, std::chrono::milliseconds deadline);

// Column index whose configured `Column::header` equals @p header,
// or -1. Reads the configuration directly (not `DisplayRole`) so
// it survives theme overrides that suppress the header text.
int ColumnByHeader(const LogModel &model, const QString &header);

// Arms @p model for a live-tail-style synthetic-batch session by
// installing a no-producer `StreamLineSource` so subsequent
// `LogLine`s constructed for synthetic batches have a valid
// `LineSource *` to point at. Returns the installed source by
// reference for callers that publish raw bytes via `AppendLine`.
loglib::StreamLineSource &BeginSyntheticStreamSession(LogModel &model);

// Static-mode counterpart of `BeginSyntheticStreamSession`. Installs a
// synthetic `StreamLineSource` (the `LineSource` flavour is irrelevant
// for proxy-orientation tests) but without any rows; callers are
// expected to push synthetic batches via `OnBatch` if they need rows.
// Returns the installed source by reference for callers that publish
// raw bytes via `AppendLine`.
loglib::StreamLineSource &BeginSyntheticStaticSession(LogModel &model);

// Builds one synthetic streaming batch carrying @p count `LogLine`
// rows with line ids `[firstLineId, firstLineId + count)`, each
// holding a single int64 value field equal to its line id under
// @p valueKey. Each row's raw bytes are also published into
// @p streamSource so `RawLine(lineId)` round-trips. Set
// @p declareNewKey on the first batch in a sequence so
// `LogTable::PreviewAppend`'s column predictor matches reality.
loglib::RowBatch MakeSyntheticBatch(
    loglib::StreamLineSource &streamSource, loglib::KeyId valueKey, size_t firstLineId, size_t count, bool declareNewKey
);

// Locate a UI-file-declared `QAction` by `objectName`. Thin wrapper
// over `findChild<QAction*>` so call sites read intent rather than the
// generic Qt API.
QAction *FindActionByObjectName(QMainWindow *window, const QString &name);

// Tests drive `RecordDetailDock` visibility via `emit
// visibilityChanged(...)` instead of `dock.show()` / `dock.hide()`
// because the dock's refresh gate keys off `mPerceivedVisible`, which
// is fed by the `visibilityChanged` signal -- this lets tests simulate
// "buried tab" transitions where the perceived-visible flag flips
// without the explicit-hide flag flipping (the dock isn't actually
// hidden, just covered). Tests that attach the dock to an unrealised
// `QMainWindow` also avoid `QDockWidget::setVisible(true)` walking
// `QMainWindowLayout`'s dock-area state, which is only wired up by
// the host's first paint cycle.

/// Returns the first action in @p menu whose visible text (mnemonic
/// stripped) equals @p text, or null. Lookup by label avoids
/// hard-coded indices in the row-context-menu tests.
[[nodiscard]] inline QAction *FindMenuActionByText(const QMenu *menu, const QString &text)
{
    if (menu == nullptr)
    {
        return nullptr;
    }
    for (QAction *action : menu->actions())
    {
        if (action == nullptr)
        {
            continue;
        }
        QString visible = action->text();
        visible.remove(QChar('&'));
        if (visible == text)
        {
            return action;
        }
    }
    return nullptr;
}

/**
 * @brief QtTest base class that gives each test function a fresh MainWindow and ThemeControl.
 *
 * The first test function of a run sets up a test-only settings identity, redirects the standard paths and loads the
 * time zone database. Each test function then starts with cleared settings and a new window, and ends by deleting the
 * window before the theme controller.
 */
class AppTestBase : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();
    void init();
    void cleanup();

protected:
    // Stream a small two-column fixture and return the `level`
    // column index. QTest filters non-void slots out of the test
    // scan, so this stays callable from inside `private slots:`.
    int StreamFixtureForColumnTests();

    // Stream a three-row fixture with a `timestamp` column (promoted to
    // `Type::Time`) and a `msg` column. Returns the time-column index
    // post-bubble, or -1 on streaming failure.
    int StreamFixtureWithTimeColumnForRowMenuTests();

    // Click a clipboard-producing button and read the clipboard,
    // retrying until the text contains `marker`. The Windows OLE
    // clipboard is async and contended; `OleSetClipboard` can fail
    // outright when other tests hammer the marshaller. The retry
    // re-clicks each iteration so lost OLE handshakes also resolve.
    static QString ClickAndReadClipboardWithRetry(
        QClipboard *clipboard, QPushButton *button, const QString &marker, int maxAttempts = 20, int sleepMs = 25
    );

    // Same retry as the click overload, but drives the producer via
    // `QMetaObject::invokeMethod` (for private slots).
    static QString InvokeAndReadClipboardWithRetry(
        QClipboard *clipboard,
        QObject *target,
        const char *member,
        const QString &marker,
        int maxAttempts = 20,
        int sleepMs = 25
    );

    // A log value with no zone is read in the display zone. Outside
    // `private slots:` so moc doesn't expose it as a test method.
    static qint64 NaiveToUtcUs(qint64 wallClockUs);

    // Shared helper for the ISO/timestamp fixtures. Outside `private slots:`
    // so moc doesn't expose it as a test method.
    static void AssertTimestampFixture(StreamingRun &run, qint64 expectedFirstUtcUs, int rowCount);

    MainWindow *mWindow{};
    QScopedPointer<ThemeControl> mTheme;
};

} // namespace apptest_support
