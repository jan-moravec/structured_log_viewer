// Unit tests for `logapp::BoundedBatchQueue` -- the bounded SPSC queue
// that propagates GUI back-pressure to the parser worker. These tests
// cover the queue in isolation; the end-to-end wiring through
// `QtStreamingLogSink` is covered in `main_window_test.cpp`.

#include "bounded_batch_queue.hpp"

#include <loglib/log_parse_sink.hpp>

#include <QtTest/QtTest>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <thread>
#include <vector>

namespace
{

// Helper: build a `StreamedBatch` whose `firstLineNumber` doubles as a
// payload id so test assertions can verify FIFO ordering without
// touching the move-only `lines` vector.
loglib::StreamedBatch MakeBatch(std::size_t id)
{
    loglib::StreamedBatch batch;
    batch.firstLineNumber = id;
    return batch;
}

constexpr auto SHORT_WAIT = std::chrono::milliseconds(50);
constexpr auto STOP_DEADLINE = std::chrono::milliseconds(200);
constexpr auto STRESS_DEADLINE = std::chrono::seconds(5);

} // namespace

class BoundedBatchQueueTest : public QObject
{
    Q_OBJECT

private slots:
    /// Capacity 2: enqueue two items, spawn a producer that wants to
    /// enqueue a third; verify it is parked on `WaitEnqueue` until a
    /// `DrainAll` opens a slot.
    void testEnqueueBlocksAtCapacity()
    {
        logapp::BoundedBatchQueue queue(2);
        QVERIFY(queue.WaitEnqueue(MakeBatch(0)));
        QVERIFY(queue.WaitEnqueue(MakeBatch(1)));
        QCOMPARE(queue.SizeApprox(), std::size_t{2});

        std::atomic<bool> entered{false};
        std::atomic<bool> finished{false};
        std::thread producer([&] {
            entered.store(true, std::memory_order_release);
            const bool ok = queue.WaitEnqueue(MakeBatch(2));
            finished.store(ok, std::memory_order_release);
        });

        // Wait until the producer has at least entered the call.
        const auto enterDeadline = std::chrono::steady_clock::now() + SHORT_WAIT;
        while (!entered.load(std::memory_order_acquire))
        {
            QVERIFY(std::chrono::steady_clock::now() < enterDeadline);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        // The producer must remain blocked: queue stays at capacity.
        std::this_thread::sleep_for(SHORT_WAIT);
        QVERIFY(!finished.load(std::memory_order_acquire));
        QCOMPARE(queue.SizeApprox(), std::size_t{2});

        // Drain unblocks the producer; the third batch lands.
        auto drained = queue.DrainAll();
        QCOMPARE(drained.size(), std::size_t{2});
        QCOMPARE(drained[0].firstLineNumber, std::size_t{0});
        QCOMPARE(drained[1].firstLineNumber, std::size_t{1});

        producer.join();
        QVERIFY(finished.load(std::memory_order_acquire));
        auto tail = queue.DrainAll();
        QCOMPARE(tail.size(), std::size_t{1});
        QCOMPARE(tail[0].firstLineNumber, std::size_t{2});
    }

    /// `NotifyStop` must wake the blocked producer immediately (no
    /// polling loop). Capacity 1 + one prefilled item parks the
    /// producer; the wake must arrive well inside the deadline.
    void testNotifyStopWakesBlockedProducer()
    {
        logapp::BoundedBatchQueue queue(1);
        QVERIFY(queue.WaitEnqueue(MakeBatch(0)));

        std::atomic<bool> entered{false};
        std::atomic<bool> result{true};
        std::thread producer([&] {
            entered.store(true, std::memory_order_release);
            const bool ok = queue.WaitEnqueue(MakeBatch(1));
            result.store(ok, std::memory_order_release);
        });

        const auto enterDeadline = std::chrono::steady_clock::now() + SHORT_WAIT;
        while (!entered.load(std::memory_order_acquire))
        {
            QVERIFY(std::chrono::steady_clock::now() < enterDeadline);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        // Give the producer a beat to reach the cv wait inside the queue.
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

        const auto stopStart = std::chrono::steady_clock::now();
        queue.NotifyStop();
        producer.join();
        const auto wakeLatency = std::chrono::steady_clock::now() - stopStart;

        QVERIFY2(
            wakeLatency < STOP_DEADLINE,
            qPrintable(QStringLiteral("Producer wake latency was %1 ms, expected < %2 ms")
                           .arg(std::chrono::duration_cast<std::chrono::milliseconds>(wakeLatency).count())
                           .arg(STOP_DEADLINE.count()))
        );
        QVERIFY(!result.load(std::memory_order_acquire));
    }

    /// `WaitEnqueue` only fails on stop when the queue is full --
    /// callers with available capacity must still enqueue (preserves
    /// the drain-phase `OnBatch` contract: between `RequestStop` and
    /// the worker join the worker may emit more batches and they must
    /// reach the GUI).
    void testStopWithSpaceStillEnqueues()
    {
        logapp::BoundedBatchQueue queue(4);
        queue.NotifyStop();

        const bool ok = queue.WaitEnqueue(MakeBatch(7));
        QVERIFY(ok);
        auto drained = queue.DrainAll();
        QCOMPARE(drained.size(), std::size_t{1});
        QCOMPARE(drained[0].firstLineNumber, std::size_t{7});
    }

    /// `WaitEnqueue` against a full + stopped queue must not park the
    /// caller; it returns `false` immediately with the batch dropped.
    void testStopWithFullQueueReturnsFalseImmediately()
    {
        logapp::BoundedBatchQueue queue(1);
        QVERIFY(queue.WaitEnqueue(MakeBatch(0)));
        queue.NotifyStop();

        const auto start = std::chrono::steady_clock::now();
        const bool ok = queue.WaitEnqueue(MakeBatch(1));
        const auto elapsed = std::chrono::steady_clock::now() - start;

        QVERIFY(!ok);
        QVERIFY2(
            elapsed < SHORT_WAIT,
            qPrintable(QStringLiteral("WaitEnqueue blocked for %1 ms against full+stopped queue")
                           .arg(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()))
        );
    }

    /// Producer enqueues 100 items in order; consumer issues several
    /// `DrainAll` calls. The concatenation of drained items must be
    /// the original sequence.
    void testFifoOrderAcrossDrains()
    {
        constexpr std::size_t COUNT = 100;
        logapp::BoundedBatchQueue queue(8);

        std::thread producer([&] {
            for (std::size_t i = 0; i < COUNT; ++i)
            {
                QVERIFY(queue.WaitEnqueue(MakeBatch(i)));
            }
        });

        std::vector<std::size_t> received;
        received.reserve(COUNT);
        const auto deadline = std::chrono::steady_clock::now() + STRESS_DEADLINE;
        while (received.size() < COUNT)
        {
            QVERIFY(std::chrono::steady_clock::now() < deadline);
            auto chunk = queue.DrainAll();
            for (auto &batch : chunk)
            {
                received.push_back(batch.firstLineNumber);
            }
            if (chunk.empty())
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        producer.join();

        QCOMPARE(received.size(), COUNT);
        for (std::size_t i = 0; i < COUNT; ++i)
        {
            QCOMPARE(received[i], i);
        }
    }

    /// Stop, then `Reset` must wipe leftover items and clear the
    /// stopped flag so the queue is usable again. After Reset the
    /// queue should also block again on full-capacity (i.e. the
    /// stopped flag is genuinely cleared, not just hidden).
    void testResetClearsAndReArms()
    {
        logapp::BoundedBatchQueue queue(2);
        QVERIFY(queue.WaitEnqueue(MakeBatch(7)));
        queue.NotifyStop();

        queue.Reset();
        QCOMPARE(queue.SizeApprox(), std::size_t{0});

        // Post-Reset the queue is operational again.
        QVERIFY(queue.WaitEnqueue(MakeBatch(9)));
        auto drained = queue.DrainAll();
        QCOMPARE(drained.size(), std::size_t{1});
        QCOMPARE(drained[0].firstLineNumber, std::size_t{9});

        // Capacity bound is back in effect (no stop poison from the
        // previous session leaking through).
        QVERIFY(queue.WaitEnqueue(MakeBatch(10)));
        QVERIFY(queue.WaitEnqueue(MakeBatch(11)));
        QCOMPARE(queue.SizeApprox(), std::size_t{2});
    }

    /// One producer + one consumer move 100 000 items through a
    /// capacity-8 queue; verify FIFO and a generous time bound.
    void testStressOneProducerOneConsumer()
    {
        constexpr std::size_t COUNT = 100'000;
        logapp::BoundedBatchQueue queue(8);

        std::atomic<bool> producerDone{false};
        std::thread producer([&] {
            for (std::size_t i = 0; i < COUNT; ++i)
            {
                if (!queue.WaitEnqueue(MakeBatch(i)))
                {
                    return;
                }
            }
            producerDone.store(true, std::memory_order_release);
        });

        std::vector<std::size_t> received;
        received.reserve(COUNT);
        const auto start = std::chrono::steady_clock::now();
        const auto deadline = start + STRESS_DEADLINE;
        while (received.size() < COUNT)
        {
            QVERIFY(std::chrono::steady_clock::now() < deadline);
            auto chunk = queue.DrainAll();
            for (auto &batch : chunk)
            {
                received.push_back(batch.firstLineNumber);
            }
        }
        producer.join();
        QVERIFY(producerDone.load(std::memory_order_acquire));

        QCOMPARE(received.size(), COUNT);
        for (std::size_t i = 0; i < COUNT; ++i)
        {
            QCOMPARE(received[i], i);
        }
    }
};

QTEST_GUILESS_MAIN(BoundedBatchQueueTest)
#include "test_bounded_batch_queue.moc"
