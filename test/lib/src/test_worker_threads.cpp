#include "parse_only.hpp"

#include <loglib/core/file_line_source.hpp>
#include <loglib/core/worker_threads.hpp>
#include <loglib/parse/log_parser.hpp>
#include <loglib/parse/parsers/logfmt_parser.hpp>

#include <test_common/temp_dir.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/reporters/catch_reporter_event_listener.hpp>
#include <catch2/reporters/catch_reporter_registrars.hpp>

#include <cstddef>
#include <filesystem>
#include <memory>
#include <utility>

using namespace loglib;

namespace
{

/// Ends the worker threads before the test process exits.
class ShutdownWorkerThreadsAtRunEnd : public Catch::EventListenerBase
{
public:
    using Catch::EventListenerBase::EventListenerBase;

    void testRunEnded(const Catch::TestRunStats & /*stats*/) override
    {
        (void)ShutdownWorkerThreads();
    }
};

std::size_t ParseLineCount(const std::filesystem::path &path)
{
    auto source = std::make_unique<FileLineSource>(path);
    FileLineSource &sourceRef = *source;
    internal::BufferingSink sink(std::move(source));
    LogfmtParser{}.Parse(sourceRef, sink, ParserOptions{});
    return sink.TakeData().Lines().size();
}

} // namespace

CATCH_REGISTER_LISTENER(ShutdownWorkerThreadsAtRunEnd)

TEST_CASE("ShutdownWorkerThreads ends the workers and a later parse starts them again", "[worker_threads]")
{
    const test_common::TempDir dir("worker_threads");
    const std::filesystem::path path = dir.Write("input.log", "ts=1 msg=a\nts=2 msg=b\n");

    CHECK(ShutdownWorkerThreads());

    CHECK(ParseLineCount(path) == 2);
    CHECK(ShutdownWorkerThreads());

    CHECK(ParseLineCount(path) == 2);
    CHECK(ShutdownWorkerThreads());
}
