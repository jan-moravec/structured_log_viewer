#include <loglib/core/error.hpp>
#include <loglib/ingest/internal/stdin_bytes_producer_test_access.hpp>
#include <loglib/ingest/stdin_bytes_producer.hpp>
#include <loglib/ingest/tailing_bytes_producer.hpp>
#include <loglib/ingest/tcp_server_producer.hpp>
#include <loglib/output/exports/export_sink.hpp>
#include <loglib/parse/regex_templates.hpp>
#include <loglib/support/theme.hpp>
#include <loglib/time/time_zone_context.hpp>

#include <test_common/temp_dir.hpp>

#include <catch2/catch_all.hpp>

#include <bit>
#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>

#include "common.hpp"

using namespace loglib;

TEST_CASE("Exporters throw IoError for a destination they cannot open", "[errors][export]")
{
    const test_common::TempDir dir("documented_exceptions_export");
    const auto missingFolder = dir.Path() / "missing" / "out.json";
    CHECK_THROWS_AS(exports::FileSink(missingFolder), IoError);
}

TEST_CASE(
    "Theme and template readers throw FormatError for content they cannot read", "[errors][theme][regex_templates]"
)
{
    CHECK_THROWS_AS(ParseTheme("{"), FormatError);
    CHECK_THROWS_AS(ParseTheme(R"({"name": "x", "unknown": 1})"), FormatError);
    CHECK_THROWS_AS(ParseRegexTemplate("{"), FormatError);
    CHECK_THROWS_AS(ParseRegexTemplate(R"({"name": "x", "unknown": 1})"), FormatError);
}

TEST_CASE("TimeZoneContext::Load throws only the documented exceptions", "[errors][time_zone]")
{
    const test_common::TempDir dir("documented_exceptions_tz");
    CHECK_THROWS_AS(TimeZoneContext::Load(dir.Path() / "missing"), IoError);
    CHECK_THROWS_AS(TimeZoneContext::Load(FindTestTzdata(), "Not/ARealZone"), FormatError);
}

TEST_CASE("TimeZoneContext::Load accepts one tzdata folder for the whole process", "[errors][time_zone]")
{
    InitializeTimezoneData();
    const test_common::TempDir otherFolder("documented_exceptions_other_tzdata");

    // A second, different folder is an error, whichever zone is asked for.
    CHECK_THROWS_AS(TimeZoneContext::Load(otherFolder.Path()), FormatError);
    CHECK_THROWS_AS(TimeZoneContext::Load(otherFolder.Path(), "Europe/Berlin"), FormatError);

    // The same folder again, for another zone, works.
    const TimeZoneContext tokyo = TimeZoneContext::Load(FindTestTzdata(), "Asia/Tokyo");
    CHECK(tokyo.IanaName() == "Asia/Tokyo");
}

TEST_CASE("Network and stdin producers throw IoError for resources they cannot open", "[errors][network]")
{
    SECTION("a TLS certificate that does not exist")
    {
        const test_common::TempDir dir("documented_exceptions_tls");
        TcpServerProducer::Options options;
        options.bindAddress = "127.0.0.1";
        options.tls.emplace();
        options.tls->certificateChain = dir.Path() / "missing-cert.pem";
        options.tls->privateKey = dir.Path() / "missing-key.pem";
        CHECK_THROWS_AS(TcpServerProducer(options), IoError);
    }
    SECTION("an invalid standard input handle")
    {
        void *const invalidHandle = std::bit_cast<void *>(static_cast<std::intptr_t>(-1));
        CHECK_THROWS_AS(
            internal::StdinBytesProducerTestAccess::Create(invalidHandle, StdinBytesProducer::Options{}), IoError
        );
    }
}

TEST_CASE("IoError carries the system error code when it has one", "[errors]")
{
    SECTION("without a code")
    {
        const IoError error("cannot open");
        CHECK_FALSE(error.Code());
        CHECK(std::string(error.what()) == "cannot open");
    }
    SECTION("with a code")
    {
        const IoError error("cannot open", std::make_error_code(std::errc::no_such_file_or_directory));
        CHECK(error.Code() == std::errc::no_such_file_or_directory);
        CHECK(std::string(error.what()).starts_with("cannot open: "));
    }
}

TEST_CASE("A tailed file that does not exist and a bind failure throw IoError with a code", "[errors][network]")
{
    SECTION("a missing tailed file")
    {
        const test_common::TempDir dir("documented_exceptions_tail");
        try
        {
            const TailingBytesProducer producer(dir.Path() / "missing.log", 100);
            FAIL("expected IoError");
        }
        catch (const IoError &error)
        {
            CHECK(error.Code());
        }
    }
    SECTION("an address that cannot be bound")
    {
        TcpServerProducer::Options options;
        options.bindAddress = "not-a-host";
        try
        {
            const TcpServerProducer producer(options);
            FAIL("expected IoError");
        }
        catch (const IoError &error)
        {
            CHECK(error.Code());
        }
    }
}
