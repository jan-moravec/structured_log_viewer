#include "common.hpp"

#include <loglib/core/error.hpp>
#include <loglib/core/file_line_source.hpp>

#include <catch2/catch_all.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

using loglib::FileLineSource;

TEST_CASE("FileLineSource: Path() reports the file path", "[FileLineSource]")
{
    const TestLogFile testLogFile;
    testLogFile.Write("hello\n");

    const FileLineSource source(testLogFile.GetFilePath());
    CHECK(source.Path() == testLogFile.GetFilePath());
}

TEST_CASE("FileLineSource: the two-path constructor maps one file and reports another", "[FileLineSource]")
{
    const TestLogFile testLogFile;
    testLogFile.Write("hello\n");

    const FileLineSource source(testLogFile.GetFilePath(), "logical.log");
    CHECK(source.Path() == std::filesystem::path("logical.log"));
    CHECK(source.StableBytes().size() == 6);
}

TEST_CASE("FileLineSource: throws IoError when the file does not exist", "[FileLineSource]")
{
    CHECK_THROWS_AS(FileLineSource("non_existent_file.txt"), loglib::IoError);
}

TEST_CASE("FileLineSource: RawLine returns CR-stripped per-line text", "[FileLineSource]")
{
    const MemoryLogFile testLogFile;
    testLogFile.Write("Line 1\nLine 2\nLine 3\n");

    const auto source = testLogFile.CreateFileLineSource();

    CHECK(source->LineCount() == 3);
    CHECK(source->RawLine(0) == "Line 1");
    CHECK(source->RawLine(1) == "Line 2");
    CHECK(source->RawLine(2) == "Line 3");
}

TEST_CASE("FileLineSource: RawLine strips CRLF line endings", "[FileLineSource]")
{
    // Byte offsets and CRLF stripping must work regardless of the host's line-ending conventions.
    const MemoryLogFile testLogFile;
    testLogFile.Write("Line 1\r\nLine 2\r\nLine 3\r\n");

    const auto source = testLogFile.CreateFileLineSource();

    CHECK(source->RawLine(0) == "Line 1");
    CHECK(source->RawLine(1) == "Line 2");
    CHECK(source->RawLine(2) == "Line 3");
}

TEST_CASE("FileLineSource: RawLine reads a last line without a trailing newline", "[FileLineSource]")
{
    // The parsers push a virtual terminator one byte past the end for such a line.
    const MemoryLogFile testLogFile;
    testLogFile.Write("Line 1\nLine 2");

    const auto source = testLogFile.CreateFileLineSource();

    CHECK(source->LineCount() == 2);
    CHECK(source->RawLine(0) == "Line 1");
    CHECK(source->RawLine(1) == "Line 2");
}

TEST_CASE("FileLineSource: an empty file maps no bytes", "[FileLineSource]")
{
    const TestLogFile testLogFile;
    testLogFile.Write("");

    const FileLineSource source(testLogFile.GetFilePath());
    CHECK(source.Path() == testLogFile.GetFilePath());
    CHECK(source.StableBytes().empty());
    CHECK_FALSE(source.BytesAreStable());
    CHECK(source.LineCount() == 0);
}

TEST_CASE("FileLineSource: RawLine returns joined bytes for multi-line records", "[FileLineSource][multiline]")
{
    const MemoryLogFile testLogFile;
    testLogFile.Write(
        "level=info msg=first\n"
        "\tframe A1\n"
        "\tframe A2\n"
        "level=warn msg=second\n"
        "\tframe B1\n"
    );

    const auto source = testLogFile.CreateFileLineSource();
    REQUIRE(source->LineCount() == 5);
    CHECK_FALSE(source->HasMultiLineRecords());

    source->RegisterMultiLineRecord(0, 2);
    source->RegisterMultiLineRecord(3, 4);
    CHECK(source->HasMultiLineRecords());

    CHECK(source->RawLine(0) == "level=info msg=first\n\tframe A1\n\tframe A2");
    CHECK(source->RawLine(3) == "level=warn msg=second\n\tframe B1");

    // The continuation lines keep their own text.
    CHECK(source->RawLine(1) == "\tframe A1");
    CHECK(source->RawLine(2) == "\tframe A2");
    CHECK(source->RawLine(4) == "\tframe B1");
}

TEST_CASE("FileLineSource: a single-line span is not a multi-line record", "[FileLineSource][multiline]")
{
    const MemoryLogFile testLogFile;
    testLogFile.Write("Line 1\nLine 2\nLine 3\n");

    const auto source = testLogFile.CreateFileLineSource();
    source->RegisterMultiLineRecord(1, 1);

    CHECK_FALSE(source->HasMultiLineRecords());
    CHECK(source->RawLine(0) == "Line 1");
    CHECK(source->RawLine(1) == "Line 2");
    CHECK(source->RawLine(2) == "Line 3");
}

TEST_CASE("FileLineSource: ResolveMmapBytes indexes into the mapped bytes", "[FileLineSource]")
{
    const TestLogFile testLogFile;
    const std::string content = "abcdef\nghijkl\n";
    testLogFile.Write(content);

    const FileLineSource source(testLogFile.GetFilePath());
    REQUIRE(source.StableBytes().size() == content.size());

    CHECK(source.BytesAreStable());
    CHECK_FALSE(source.SupportsEviction());

    CHECK(source.ResolveMmapBytes(0, 6, /*lineId=*/0) == "abcdef");
    CHECK(source.ResolveMmapBytes(7, 6, /*lineId=*/1) == "ghijkl");
    CHECK(source.ResolveMmapBytes(0, static_cast<uint32_t>(content.size()), /*lineId=*/0) == content);

    // Out-of-range queries return an empty view (not UB).
    CHECK(source.ResolveMmapBytes(0, 1024, /*lineId=*/0).empty());
    CHECK(source.ResolveMmapBytes(content.size() + 1, 1, /*lineId=*/0).empty());
}

TEST_CASE("FileLineSource: ResolveOwnedBytes indexes into the owned-string arena", "[FileLineSource]")
{
    const TestLogFile testLogFile;
    testLogFile.Write("x\n");

    FileLineSource source(testLogFile.GetFilePath());

    const auto firstOffset = source.AppendOwnedBytes(0, "first");
    const auto secondOffset = source.AppendOwnedBytes(0, "second");
    REQUIRE(firstOffset == 0);
    REQUIRE(secondOffset == 5);

    CHECK(source.ResolveOwnedBytes(firstOffset, 5, /*lineId=*/0) == "first");
    CHECK(source.ResolveOwnedBytes(secondOffset, 6, /*lineId=*/0) == "second");

    // Out-of-range queries return an empty view.
    CHECK(source.ResolveOwnedBytes(0, 1024, /*lineId=*/0).empty());
    CHECK(source.ResolveOwnedBytes(1024, 1, /*lineId=*/0).empty());
}

TEST_CASE("FileLineSource: TryExtendOwnedString extends only the newest string", "[FileLineSource]")
{
    const TestLogFile testLogFile;
    testLogFile.Write("x\n");

    FileLineSource source(testLogFile.GetFilePath());
    const auto first = source.AppendOwnedBytes(0, "first");
    const auto second = source.AppendOwnedBytes(0, "second");

    CHECK_FALSE(source.TryExtendOwnedString(first, 5, "!"));
    CHECK(source.TryExtendOwnedString(second, 6, "!"));
    CHECK(source.ResolveOwnedBytes(second, 7, /*lineId=*/0) == "second!");
}

TEST_CASE("FileLineSource: eviction is a no-op for a file source", "[FileLineSource]")
{
    const MemoryLogFile testLogFile;
    testLogFile.Write("a\nb\nc\n");

    const auto source = testLogFile.CreateFileLineSource();

    CHECK(source->FirstAvailableLineId() == 0);
    source->EvictBefore(2);
    CHECK(source->FirstAvailableLineId() == 0);
    CHECK(source->RawLine(0) == "a");
    CHECK(source->RawLine(2) == "c");
}

// Regression: AttachLifetimeAnchor used to overwrite the previous anchor, dropping it before the mapping was released
// (a silent temporary file leak on Windows). Anchors are composed and both survive until the source is destroyed.
TEST_CASE("FileLineSource: AttachLifetimeAnchor composes multiple anchors", "[FileLineSource]")
{
    const TestLogFile testLogFile;
    testLogFile.Write("Line 1\nLine 2\n");

    struct Sentinel
    {
        int *counter;
        explicit Sentinel(int *c)
            : counter(c)
        {
        }
        ~Sentinel()
        {
            ++(*counter);
        }
        Sentinel(const Sentinel &) = delete;
        Sentinel &operator=(const Sentinel &) = delete;
        Sentinel(Sentinel &&) = delete;
        Sentinel &operator=(Sentinel &&) = delete;
    };

    int firstDestructed = 0;
    int secondDestructed = 0;
    {
        FileLineSource source(testLogFile.GetFilePath());

        auto first = std::make_shared<Sentinel>(&firstDestructed);
        auto second = std::make_shared<Sentinel>(&secondDestructed);

        source.AttachLifetimeAnchor(first);
        source.AttachLifetimeAnchor(second);

        // Drop the local shared_ptrs; the source's composed anchor is now the sole owner of both sentinels.
        first.reset();
        second.reset();

        CHECK(firstDestructed == 0);
        CHECK(secondDestructed == 0);
    }

    // Both sentinels were destroyed exactly once, so the first anchor was not overwritten.
    CHECK(firstDestructed == 1);
    CHECK(secondDestructed == 1);
}

TEST_CASE("FileLineSource::FromBytes serves the given bytes", "[FileLineSource][FromBytes]")
{
    const std::string content = "abcdef\nghijkl\n";
    const auto source = FileLineSource::FromBytes(content, "memory.log");

    CHECK(source->Path() == std::filesystem::path("memory.log"));
    CHECK(source->BytesAreStable());
    CHECK_FALSE(source->SupportsEviction());
    REQUIRE(source->StableBytes().size() == content.size());
    CHECK(std::string_view(source->StableBytes().data(), source->StableBytes().size()) == content);

    CHECK(source->ResolveMmapBytes(0, 6, /*lineId=*/0) == "abcdef");
    CHECK(source->ResolveMmapBytes(7, 6, /*lineId=*/1) == "ghijkl");
    CHECK(source->ResolveMmapBytes(0, 1024, /*lineId=*/0).empty());

    // A fresh source has no line offsets; a parse or the caller adds them.
    CHECK(source->LineCount() == 0);
    source->AppendLineOffsets(LineEndOffsets(content));
    CHECK(source->LineCount() == 2);
    CHECK(source->RawLine(0) == "abcdef");
    CHECK(source->RawLine(1) == "ghijkl");
}

TEST_CASE("FileLineSource::FromBytes with no bytes behaves like an empty file", "[FileLineSource][FromBytes]")
{
    const auto source = FileLineSource::FromBytes(std::string{}, "empty.log");

    CHECK(source->Path() == std::filesystem::path("empty.log"));
    CHECK(source->StableBytes().empty());
    CHECK_FALSE(source->BytesAreStable());
    CHECK(source->LineCount() == 0);
    CHECK(source->ResolveMmapBytes(0, 1, /*lineId=*/0).empty());
}

TEST_CASE("FileLineSource::FromBytes reads a last line without a trailing newline", "[FileLineSource][FromBytes]")
{
    const std::string content = "Line 1\r\nLine 2";
    const auto source = FileLineSource::FromBytes(content, "no-newline.log");
    source->AppendLineOffsets(LineEndOffsets(content));

    CHECK(source->LineCount() == 2);
    CHECK(source->RawLine(0) == "Line 1");
    CHECK(source->RawLine(1) == "Line 2");
}

TEST_CASE("FileLineSource::FromBytes joins the lines of a multi-line record", "[FileLineSource][FromBytes][multiline]")
{
    const std::string content = "ERROR boom\n\tat A\n\tat B\nINFO next\n";
    const auto source = FileLineSource::FromBytes(content, "multi.log");
    source->AppendLineOffsets(LineEndOffsets(content));
    source->RegisterMultiLineRecord(0, 2);

    CHECK(source->HasMultiLineRecords());
    CHECK(source->RawLine(0) == "ERROR boom\n\tat A\n\tat B");
    CHECK(source->RawLine(1) == "\tat A");
    CHECK(source->RawLine(3) == "INFO next");
}

TEST_CASE("FileLineSource::FromBytes keeps views valid while the arena grows", "[FileLineSource][FromBytes]")
{
    // A short string keeps its characters inside the string object, so this also checks that the bytes do not move
    // when the source is built.
    const auto source = FileLineSource::FromBytes("short\n", "short.log");

    const char *const dataBefore = source->StableBytes().data();
    const std::string_view mappedView = source->ResolveMmapBytes(0, 5, /*lineId=*/0);
    const uint64_t firstOffset = source->AppendOwnedBytes(0, "first");
    const std::string_view ownedView = source->ResolveOwnedBytes(firstOffset, 5, /*lineId=*/0);
    const char *const ownedData = ownedView.data();

    // Enough appends to force the arena to allocate many blocks.
    const std::string filler(10'000, 'x');
    for (int i = 0; i < 2'000; ++i)
    {
        source->AppendOwnedBytes(0, filler);
    }

    CHECK(source->StableBytes().data() == dataBefore);
    CHECK(mappedView == "short");
    CHECK(ownedView.data() == ownedData);
    CHECK(ownedView == "first");
    CHECK(source->ResolveOwnedBytes(firstOffset, 5, /*lineId=*/0) == "first");
}
