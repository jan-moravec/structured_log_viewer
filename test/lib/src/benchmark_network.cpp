// Network ingest benchmarks: a local client sends logfmt lines to TcpServerProducer or UdpServerProducer, and a
// streaming parse feeds them into a LogTable. The figure is bytes per second from the start of sending to the arrival
// of the last row.
//
// The client runs in this process, on its own thread, during the timed window, so the figure includes the cost of
// sending over the loopback interface. The producer has no queue limit here, so a slow parse delays the sender and
// does not drop lines. The UDP sender waits until the producer has received all but a few datagrams, so the socket
// buffer never overflows.

#include "benchmark_common.hpp"
#include "benchmark_registry.hpp"
#include "common.hpp"

#include <loglib/core/stop_token.hpp>
#include <loglib/core/stream_line_source.hpp>
#include <loglib/ingest/tcp_server_producer.hpp>
#include <loglib/ingest/udp_server_producer.hpp>
#include <loglib/parse/log_parse_sink.hpp>
#include <loglib/parse/parser_options.hpp>
#include <loglib/parse/parsers/logfmt_parser.hpp>
#include <loglib/schema/log_configuration.hpp>
#include <loglib/table/log_table.hpp>
#include <loglib/time/time_zone_context.hpp>

#include <test_common/log_format.hpp>
#include <test_common/log_generator.hpp>
#include <test_common/network_log_client.hpp>

#include <benchmark/benchmark.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace loglib;
using namespace std::chrono_literals;

namespace bench
{

namespace
{

// Lines are grouped into chunks of about this size. One chunk is one datagram, and complete lines only.
constexpr std::size_t CHUNK_BYTES = 8000;

// How many datagrams the UDP sender may have in flight. Together they stay below the smallest socket buffer.
constexpr std::size_t UDP_WINDOW = 4;

// A run takes about 3 microseconds a line over loopback, so the full tier uses a quarter of the rows of the other
// tables to keep a benchmark with its warm-up and repetitions near 10 seconds.
constexpr std::size_t NETWORK_ROWS = 250'000;

// The longest the run waits for the last row.
constexpr auto RUN_TIMEOUT = 60s;

enum class Transport
{
    Tcp,
    Udp,
};

// The lines to send, in chunks of whole lines.
struct Payload
{
    std::vector<std::string> chunks;
    std::size_t bytes = 0;
    std::size_t rows = 0;
};

std::unique_ptr<Payload> MakePayload(std::size_t rows)
{
    auto payload = std::make_unique<Payload>();
    payload->rows = rows;
    const test_common::LogFormat format = test_common::Logfmt();
    const test_common::TimestampPolicy timestamps = DeterministicBenchmarkTimestamps();
    // NOLINTNEXTLINE(cert-msc32-c,cert-msc51-cpp,bugprone-random-generator-seed)
    std::mt19937 rng(LARGE_FIXTURE_SEED);
    std::string chunk;
    for (std::size_t i = 0; i < rows; ++i)
    {
        chunk += format.writeLine(test_common::GenerateRandomLogRecord(rng, i, timestamps));
        chunk += '\n';
        if (chunk.size() >= CHUNK_BYTES)
        {
            payload->bytes += chunk.size();
            payload->chunks.push_back(std::move(chunk));
            chunk.clear();
        }
    }
    if (!chunk.empty())
    {
        payload->bytes += chunk.size();
        payload->chunks.push_back(std::move(chunk));
    }
    return payload;
}

// Appends every batch to the table like the application does, and counts the rows as they arrive.
struct CountingSink final : StreamSink
{
    std::atomic<std::size_t> rows{0};

    void OnBatch(RowBatch batch) override
    {
        const std::size_t count = batch.lines.size();
        StreamSink::OnBatch(std::move(batch));
        rows.fetch_add(count, std::memory_order_release);
    }
};

// One run over a fresh producer, table and parser. Returns the seconds from the first byte sent to the last row, or a
// negative number when rows were lost.
double RunOnce(const Payload &payload, Transport transport)
{
    const TcpServerProducer *tcp = nullptr;
    UdpServerProducer *udp = nullptr;
    std::unique_ptr<BytesProducer> producer;
    std::uint16_t port = 0;
    if (transport == Transport::Tcp)
    {
        TcpServerProducer::Options options;
        options.bindAddress = "127.0.0.1";
        options.maxQueueBytes = 0;
        auto created = std::make_unique<TcpServerProducer>(options);
        tcp = created.get();
        port = created->BoundPort();
        producer = std::move(created);
    }
    else
    {
        UdpServerProducer::Options options;
        options.bindAddress = "127.0.0.1";
        options.maxQueueBytes = 0;
        auto created = std::make_unique<UdpServerProducer>(options);
        udp = created.get();
        port = created->BoundPort();
        producer = std::move(created);
    }
    BytesProducer *producerPtr = producer.get();

    const std::shared_ptr<const LogConfiguration> configuration = MakeTimestampConfiguration();
    LogTable table(*configuration);
    table.SetDisplayTimeZone(ProcessDefaultTimeZone());
    table.SetDefaultSourceTimeZone(ProcessDefaultTimeZone());
    auto owned = std::make_unique<StreamLineSource>(std::filesystem::path("network"), std::move(producer));
    StreamLineSource &source = *owned;
    table.BeginStreaming(std::move(owned));

    CountingSink sink;
    sink.table = &table;
    StopSource stop;
    ParserOptions options;
    options.stopToken = stop.GetToken();
    options.timeColumns = TimeColumnsOf(*configuration);
    options.defaultSourceTimeZone = table.DefaultSourceTimeZone();
    std::thread parser([&] { LogfmtParser{}.ParseStreaming(source, sink, options); });

    // The client connects before the timer starts.
    std::unique_ptr<test_common::TcpLogClient> tcpClient;
    std::unique_ptr<test_common::UdpLogClient> udpClient;
    if (tcp != nullptr)
    {
        tcpClient = std::make_unique<test_common::TcpLogClient>("127.0.0.1", port);
        const auto connectDeadline = std::chrono::steady_clock::now() + 5s;
        while (tcp->ActiveClientCount() == 0 && std::chrono::steady_clock::now() < connectDeadline)
        {
            std::this_thread::sleep_for(100us);
        }
    }
    else
    {
        udpClient = std::make_unique<test_common::UdpLogClient>("127.0.0.1", port);
    }

    const auto start = std::chrono::steady_clock::now();
    const auto deadline = start + RUN_TIMEOUT;
    std::thread sender([&] {
        std::size_t datagrams = 0;
        for (const std::string &chunk : payload.chunks)
        {
            if (tcpClient)
            {
                tcpClient->SendRaw(chunk);
                continue;
            }
            while (datagrams - std::min(datagrams, udp->DatagramCount()) >= UDP_WINDOW)
            {
                if (std::chrono::steady_clock::now() > deadline)
                {
                    return;
                }
                std::this_thread::yield();
            }
            udpClient->Send(chunk);
            ++datagrams;
        }
    });

    while (sink.rows.load(std::memory_order_acquire) < payload.rows && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(50us);
    }
    const auto end = std::chrono::steady_clock::now();

    sender.join();
    producerPtr->Stop();
    stop.RequestStop();
    parser.join();

    const bool complete = table.RowCount() == payload.rows && producerPtr->DroppedByteCount() == 0;
    return complete ? std::chrono::duration<double>(end - start).count() : -1.0;
}

struct NetworkInput
{
    std::shared_ptr<LazyInput<Payload>> payload;
};

void RegisterNetwork(const char *label, Transport transport, const std::shared_ptr<LazyInput<Payload>> &payload)
{
    RegisterLarge<NetworkInput>(
        std::string(label) + " throughput, " + CountText(NETWORK_ROWS) + " logfmt lines into a LogTable",
        [payload] {
            InitializeTimezoneData();
            return std::make_unique<NetworkInput>(NetworkInput{.payload = payload});
        },
        [transport](NetworkInput &input, benchmark::State &state, bool first) {
            const Payload &payload = input.payload->Get();
            if (first)
            {
                RunOnce(payload, transport);
            }
            for (const auto _ : state)
            {
                const double seconds = RunOnce(payload, transport);
                if (seconds < 0.0)
                {
                    Fail(state, "the table did not receive every row");
                    return;
                }
                state.SetIterationTime(seconds);
            }
            const auto iterations = static_cast<std::int64_t>(state.iterations());
            state.SetBytesProcessed(iterations * static_cast<std::int64_t>(payload.bytes));
            state.SetItemsProcessed(iterations * static_cast<std::int64_t>(payload.rows));
        },
        LargeOptions{.manualTime = true}
    );
}

} // namespace

void RegisterNetworkBenchmarks()
{
    const auto payload = MakeLazy<Payload>([] { return MakePayload(NETWORK_ROWS); });
    RegisterNetwork("TCP", Transport::Tcp, payload);
    RegisterNetwork("UDP", Transport::Udp, payload);
}

} // namespace bench
