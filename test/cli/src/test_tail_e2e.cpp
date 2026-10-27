#include "slp_process.hpp"

#include <loglib/tcp_server_producer.hpp>
#include <loglib/udp_server_producer.hpp>

#include <test_common/network_log_client.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using slp::test::FixturesDir;
using slp::test::RunSlp;
using slp::test::RunSlpWithStdin;
using slp::test::SlpChild;

namespace
{

using std::chrono::milliseconds;

constexpr auto LONG_WAIT = milliseconds(30000);

[[nodiscard]] std::filesystem::path ScratchRoot()
{
    const auto root = std::filesystem::temp_directory_path() / "slp_cli_tail";
    std::error_code ec;
    std::filesystem::create_directories(root, ec);
    return root;
}

[[nodiscard]] std::string EmptyTemplates()
{
    return (FixturesDir() / "empty_templates").string();
}

[[nodiscard]] std::string Row(std::string_view level, std::string_view msg, int second = 0)
{
    return R"({"ts":"2024-01-01T00:00:)" + std::string(second < 10 ? "0" : "") + std::to_string(second) +
           R"(Z","level":")" + std::string(level) + R"(","msg":")" + std::string(msg) + "\"}\n";
}

[[nodiscard]] std::string FourRows()
{
    return Row("info", "alpha", 0) + Row("error", "bravo", 1) + Row("warn", "charlie", 2) + Row("error", "delta", 3);
}

[[nodiscard]] std::vector<std::string> TailArgs(std::vector<std::string> extra)
{
    std::vector<std::string> args = {"tail", "--tz", "UTC", "--template-dir", EmptyTemplates()};
    args.insert(args.end(), extra.begin(), extra.end());
    return args;
}

[[nodiscard]] std::size_t CountLines(const std::string &text)
{
    return static_cast<std::size_t>(std::ranges::count(text, '\n'));
}

[[nodiscard]] bool Contains(const std::string &text, std::string_view needle)
{
    return text.contains(needle);
}

/** @brief Removes `NO_COLOR` from the environment so `--color always` is honored, then restores it. */
class NoColorUnset
{
public:
    NoColorUnset()
    {
#ifdef _WIN32
        char *value = nullptr;
        std::size_t length = 0;
        if (_dupenv_s(&value, &length, "NO_COLOR") == 0 && value != nullptr)
        {
            mSaved = value;
            // NOLINTNEXTLINE(cppcoreguidelines-no-malloc): `_dupenv_s` allocates with `malloc`.
            std::free(value);
        }
#else
        // NOLINTNEXTLINE(concurrency-mt-unsafe): single-threaded test setup.
        if (const char *value = std::getenv("NO_COLOR"); value != nullptr)
        {
            mSaved = value;
        }
#endif
        Set(nullptr);
    }

    ~NoColorUnset()
    {
        Set(mSaved ? mSaved->c_str() : nullptr);
    }

    NoColorUnset(const NoColorUnset &) = delete;
    NoColorUnset &operator=(const NoColorUnset &) = delete;
    NoColorUnset(NoColorUnset &&) = delete;
    NoColorUnset &operator=(NoColorUnset &&) = delete;

private:
    static void Set(const char *value)
    {
#ifdef _WIN32
        // An empty value removes the variable from the process environment.
        (void)_putenv_s("NO_COLOR", value != nullptr ? value : "");
#else
        if (value != nullptr)
        {
            // NOLINTNEXTLINE(concurrency-mt-unsafe): single-threaded test setup.
            (void)setenv("NO_COLOR", value, 1);
        }
        else
        {
            // NOLINTNEXTLINE(concurrency-mt-unsafe): single-threaded test setup.
            (void)unsetenv("NO_COLOR");
        }
#endif
    }

    std::optional<std::string> mSaved;
};

/** @brief A loopback port that was free a moment ago. */
template <class Producer> [[nodiscard]] std::uint16_t FreePort()
{
    typename Producer::Options options;
    options.bindAddress = "127.0.0.1";
    options.port = 0;
    Producer producer(std::move(options));
    return producer.BoundPort();
}

/** @brief Port printed on the `listening on` line of @p child. */
[[nodiscard]] std::string ListeningLine(const SlpChild &child, std::string_view scheme)
{
    REQUIRE(child.WaitForErr(std::string("listening on ") + std::string(scheme) + "://", LONG_WAIT));
    return child.Err();
}

} // namespace

TEST_CASE("slp tail reads standard input to EOF", "[cli][tail][e2e]")
{
    const auto result = RunSlpWithStdin(TailArgs({"-"}), FourRows());
    INFO(result.err);
    CHECK(result.exitCode == 0);
    CHECK(CountLines(result.out) == 4);
    CHECK(Contains(result.out, "\"msg\":\"alpha\""));
    CHECK(Contains(result.out, "\"msg\":\"delta\""));
    CHECK(result.out.find("alpha") < result.out.find("delta"));
}

TEST_CASE("slp tail applies filter, level, and invert flags per batch", "[cli][tail][e2e]")
{
    const auto filtered = RunSlpWithStdin(TailArgs({"--filter", "level:error", "-"}), FourRows());
    INFO(filtered.err);
    CHECK(filtered.exitCode == 0);
    CHECK(CountLines(filtered.out) == 2);
    CHECK(Contains(filtered.out, "bravo"));
    CHECK(Contains(filtered.out, "delta"));

    const auto leveled = RunSlpWithStdin(TailArgs({"--min-level", "warn", "-"}), FourRows());
    INFO(leveled.err);
    CHECK(leveled.exitCode == 0);
    CHECK(CountLines(leveled.out) == 3);
    CHECK_FALSE(Contains(leveled.out, "alpha"));

    const auto inverted = RunSlpWithStdin(TailArgs({"--filter", "level:error", "--invert", "-"}), FourRows());
    INFO(inverted.err);
    CHECK(inverted.exitCode == 0);
    CHECK(CountLines(inverted.out) == 2);
    CHECK(Contains(inverted.out, "alpha"));
    CHECK(Contains(inverted.out, "charlie"));

    const auto belowLevel = RunSlpWithStdin(TailArgs({"--min-level", "warn", "--invert", "-"}), FourRows());
    INFO(belowLevel.err);
    CHECK(belowLevel.exitCode == 0);
    CHECK(CountLines(belowLevel.out) == 1);
    CHECK(Contains(belowLevel.out, "alpha"));

    const auto noLevel = RunSlpWithStdin(TailArgs({"--min-level", "warn", "-"}), "{\"msg\":\"a\"}\n{\"msg\":\"b\"}\n");
    INFO(noLevel.err);
    CHECK(noLevel.exitCode == 2);
    CHECK(Contains(noLevel.err, "needs a level column"));

    const auto malformed = RunSlpWithStdin(TailArgs({"--filter", "level:(", "-"}), FourRows());
    CHECK(malformed.exitCode == 2);
}

TEST_CASE("slp tail renders one text line per row", "[cli][tail][e2e]")
{
    const auto result = RunSlpWithStdin(TailArgs({"--output-format", "text", "--color", "never", "-"}), FourRows());
    INFO(result.err);
    REQUIRE(result.exitCode == 0);
    CHECK(CountLines(result.out) == 4);
    CHECK(result.out.starts_with("2024-01-01 00:00:00.000 info alpha\n"));
    CHECK_FALSE(Contains(result.out, "\033["));

    const NoColorUnset allowColor;
    const auto colored = RunSlpWithStdin(TailArgs({"--output-format", "text", "--color", "always", "-"}), FourRows());
    INFO(colored.err);
    REQUIRE(colored.exitCode == 0);
    CHECK(Contains(colored.out, "\033[31merror\033[0m"));
}

TEST_CASE("slp tail follows a file and stops at --max-rows", "[cli][tail][e2e]")
{
    const auto path = ScratchRoot() / "follow.jsonl";
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output << Row("info", "alpha", 0) << Row("error", "bravo", 1);
    }

    SlpChild child(TailArgs({"--max-rows", "4", "--for", "25s", path.string()}));
    REQUIRE(child.WaitForOut("bravo", LONG_WAIT));
    {
        std::ofstream output(path, std::ios::binary | std::ios::app);
        output << Row("warn", "charlie", 2) << Row("error", "delta", 3);
    }
    const auto exitCode = child.Wait(LONG_WAIT);
    REQUIRE(exitCode.has_value());
    INFO(child.Err());
    CHECK(*exitCode == 0);
    const std::string out = child.Out();
    CHECK(CountLines(out) == 4);
    CHECK(out.find("alpha") < out.find("delta"));
}

TEST_CASE("slp tail reports unusable sources", "[cli][tail][e2e]")
{
    const auto missing = RunSlp(TailArgs({(ScratchRoot() / "no_such_file.log").string()}));
    CHECK(missing.exitCode == 3);
    CHECK(Contains(missing.err, "does not exist"));

    const auto two = RunSlp(TailArgs({(ScratchRoot() / "a.log").string(), (ScratchRoot() / "b.log").string()}));
    CHECK(two.exitCode == 2);

    const auto bundle = RunSlp(TailArgs({(ScratchRoot() / "any.slvbundle").string()}));
    CHECK(bundle.exitCode == 2);

    // A non-local address cannot be bound on any host.
    const auto bind = RunSlp(TailArgs({"tcp://203.0.113.1:9"}));
    INFO(bind.err);
    CHECK(bind.exitCode == 3);
    CHECK(Contains(bind.err, "cannot listen on tcp://203.0.113.1:9"));
}

TEST_CASE("slp tail receives rows over TCP", "[cli][tail][e2e][network]")
{
    const std::uint16_t port = FreePort<loglib::TcpServerProducer>();
    SlpChild child(TailArgs({"--max-rows", "3", "--for", "25s", "tcp://127.0.0.1:" + std::to_string(port)}));
    const std::string err = ListeningLine(child, "tcp");
    CHECK(Contains(err, "127.0.0.1:" + std::to_string(port)));

    {
        test_common::TcpLogClient client("127.0.0.1", port);
        client.Send(Row("info", "one", 0));
        client.Send(Row("warn", "two", 1));
        client.Send(Row("error", "three", 2));
    }
    const auto exitCode = child.Wait(LONG_WAIT);
    REQUIRE(exitCode.has_value());
    INFO(child.Err());
    CHECK(*exitCode == 0);
    const std::string out = child.Out();
    CHECK(CountLines(out) == 3);
    CHECK(Contains(out, "\"msg\":\"one\""));
    CHECK(Contains(out, "\"msg\":\"three\""));
}

TEST_CASE("slp tail receives rows over UDP", "[cli][tail][e2e][network]")
{
    const std::uint16_t port = FreePort<loglib::UdpServerProducer>();
    SlpChild child(TailArgs({"--max-rows", "3", "--for", "25s", "udp://127.0.0.1:" + std::to_string(port)}));
    (void)ListeningLine(child, "udp");

    test_common::UdpLogClient client("127.0.0.1", port);
    // Datagrams can be lost before the listener drains its socket; resend until the run ends.
    const std::vector<std::string> lines = {Row("info", "one", 0), Row("warn", "two", 1), Row("error", "three", 2)};
    std::optional<int> exitCode;
    for (int attempt = 0; attempt < 50 && !exitCode.has_value(); ++attempt)
    {
        if (attempt == 0)
        {
            for (const std::string &line : lines)
            {
                client.Send(line);
            }
        }
        exitCode = child.Wait(milliseconds(100));
    }
    REQUIRE(exitCode.has_value());
    INFO(child.Err());
    CHECK(*exitCode == 0);
    const std::string out = child.Out();
    CHECK(CountLines(out) == 3);
    CHECK(Contains(out, "\"msg\":\"two\""));
}

TEST_CASE("slp tail --for ends an idle listener with exit 0", "[cli][tail][e2e][network]")
{
    const std::uint16_t port = FreePort<loglib::TcpServerProducer>();
    const auto started = std::chrono::steady_clock::now();
    const auto result = RunSlp(TailArgs({"--for", "1s", "tcp://127.0.0.1:" + std::to_string(port)}));
    const auto elapsed = std::chrono::steady_clock::now() - started;
    INFO(result.err);
    CHECK(result.exitCode == 0);
    CHECK(result.out.empty());
    CHECK(elapsed >= milliseconds(900));
    CHECK(elapsed < std::chrono::seconds(15));
}

TEST_CASE("slp tail exits 0 on SIGINT after emitting what arrived", "[cli][tail][e2e][network]")
{
    const std::uint16_t port = FreePort<loglib::TcpServerProducer>();
    SlpChild child(TailArgs({"tcp://127.0.0.1:" + std::to_string(port)}));
    (void)ListeningLine(child, "tcp");
    {
        test_common::TcpLogClient client("127.0.0.1", port);
        client.Send(Row("info", "before-interrupt", 0));
        client.Send(Row("warn", "also-before", 1));
    }
    REQUIRE(child.WaitForOut("also-before", LONG_WAIT));
    if (!child.Interrupt())
    {
        SKIP("this platform cannot deliver SIGINT to one child");
    }
    const auto exitCode = child.Wait(LONG_WAIT);
    REQUIRE(exitCode.has_value());
    INFO(child.Err());
    CHECK(*exitCode == 0);
    CHECK(CountLines(child.Out()) == 2);
}

TEST_CASE("slp tail says so when a TCP peer speaks TLS", "[cli][tail][e2e][network]")
{
    const std::uint16_t port = FreePort<loglib::TcpServerProducer>();
    SlpChild child(TailArgs({"--for", "3s", "tcp://127.0.0.1:" + std::to_string(port)}));
    (void)ListeningLine(child, "tcp");
    {
        test_common::TcpLogClient client("127.0.0.1", port);
        // A TLS ClientHello record header followed by binary noise and a line break.
        client.SendRaw(std::string("\x16\x03\x01\x02\x00\x01\x00\x01\xfc\x03\x03\x8f\x2a\x00\x11\x22", 16) + "\n");
    }
    const auto exitCode = child.Wait(LONG_WAIT);
    REQUIRE(exitCode.has_value());
    const std::string err = child.Err();
    INFO(err);
    CHECK(*exitCode == 0);
    CHECK(Contains(err, "TLS"));
}

TEST_CASE("slp tail emits every row when retention evicts", "[cli][tail][e2e][retention]")
{
    constexpr int ROWS = 3500;
    std::string input;
    for (int i = 0; i < ROWS; ++i)
    {
        input += R"({"ts":"2024-01-01T00:00:00Z","level":"info","msg":"row-)" + std::to_string(i) + "\"}\n";
    }
    const auto result = RunSlpWithStdin(TailArgs({"--retention", "1000", "-"}), input);
    INFO(result.err);
    REQUIRE(result.exitCode == 0);
    CHECK(CountLines(result.out) == static_cast<std::size_t>(ROWS));
    CHECK(Contains(result.out, "\"msg\":\"row-0\""));
    CHECK(Contains(result.out, "\"msg\":\"row-1000\""));
    CHECK(Contains(result.out, "\"msg\":\"row-3499\""));
}

TEST_CASE("slp tail -o appends and leaves stdout empty", "[cli][tail][e2e]")
{
    const auto path = ScratchRoot() / "appended.jsonl";
    std::filesystem::remove(path);

    for (int run = 0; run < 2; ++run)
    {
        const auto result = RunSlpWithStdin(TailArgs({"-o", path.string(), "-"}), FourRows());
        INFO(result.err);
        REQUIRE(result.exitCode == 0);
        CHECK(result.out.empty());
    }
    std::ifstream input(path, std::ios::binary);
    const std::string contents((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    CHECK(CountLines(contents) == 8);
}

TEST_CASE("slp tail counts parse errors and honors --strict", "[cli][tail][e2e]")
{
    const std::string input = Row("info", "alpha", 0) + "{ not json\n" + Row("warn", "charlie", 2);
    const auto lenient = RunSlpWithStdin(TailArgs({"-"}), input);
    INFO(lenient.err);
    CHECK(lenient.exitCode == 0);
    CHECK(CountLines(lenient.out) == 2);
    CHECK(Contains(lenient.err, "could not be parsed"));

    const auto strict = RunSlpWithStdin(TailArgs({"--strict", "-"}), input);
    CHECK(strict.exitCode == 4);
    CHECK(CountLines(strict.out) == 2);
}
