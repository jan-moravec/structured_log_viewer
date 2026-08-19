// Tests for `ScopedConnections` ownership and disconnect semantics.

#include "scoped_connections.hpp"

#include <QObject>
#include <QTest>

#include <memory>
#include <utility>

namespace
{
/// Minimal test-only signal source.
class TickSource : public QObject
{
    Q_OBJECT

public:
    using QObject::QObject;
    void FireTick()
    {
        emit tick();
    }

signals:
    void tick();
};

/// Minimal test-only receiver that just counts invocations.
class TickSink : public QObject
{
    Q_OBJECT

public:
    using QObject::QObject;
    [[nodiscard]] int Count() const noexcept
    {
        return mCount;
    }
    void Bump()
    {
        ++mCount;
    }

private:
    int mCount = 0;
};
} // namespace

// NOLINTNEXTLINE(misc-use-internal-linkage): `Q_OBJECT` QtTest fixture.
class ScopedConnectionsTest : public QObject
{
    Q_OBJECT

private slots:
    static void TestDefaultBagIsEmpty()
    {
        const ScopedConnections scope;
        QVERIFY(scope.Empty());
        QCOMPARE(scope.Size(), static_cast<std::size_t>(0));
    }

    static void TestAddIgnoresInvalidConnection()
    {
        ScopedConnections scope;
        scope.Add(QMetaObject::Connection{});
        scope += QMetaObject::Connection{};
        QVERIFY(scope.Empty());
    }

    static void TestClearDisconnectsHeldConnections()
    {
        TickSource source;
        // NOLINTNEXTLINE(misc-const-correctness): mutated via `QObject::connect` slot.
        TickSink sink;
        {
            ScopedConnections scope;
            scope += QObject::connect(&source, &TickSource::tick, &sink, &TickSink::Bump);
            source.FireTick();
            QCOMPARE(sink.Count(), 1);
            scope.Clear();
            source.FireTick();
            QCOMPARE(sink.Count(), 1); // No further deliveries.
        }
    }

    static void TestDestructorDisconnectsHeldConnections()
    {
        TickSource source;
        // NOLINTNEXTLINE(misc-const-correctness): mutated via `QObject::connect` slot.
        TickSink sink;
        {
            ScopedConnections scope;
            scope += QObject::connect(&source, &TickSource::tick, &sink, &TickSink::Bump);
            source.FireTick();
            QCOMPARE(sink.Count(), 1);
        }
        source.FireTick();
        QCOMPARE(sink.Count(), 1);
    }

    static void TestSurvivesReceiverDestroyedFirst()
    {
        // Docks are commonly destroyed while their bound session is
        // still alive; the bag must not access the receiver after
        // it has been reaped. `std::unique_ptr` lets clang-tidy see
        // the deterministic release; `.get()` gives us the raw
        // pointer needed for `QObject::connect`.
        TickSource source;
        auto sink = std::make_unique<TickSink>();
        ScopedConnections scope;
        scope += QObject::connect(&source, &TickSource::tick, sink.get(), &TickSink::Bump);
        sink.reset();
        source.FireTick(); // Must not access the reaped object.
        scope.Clear();     // No-op on invalidated connections.
        QVERIFY(scope.Empty());
    }

    static void TestClearIsIdempotent()
    {
        TickSource source;
        // Connected but the deliveries are cancelled by `Clear`, so
        // `sink.Count()` is asserted to stay at 0. clang-tidy still
        // reports mutability across the slot boundary — suppress.
        // NOLINTNEXTLINE(misc-const-correctness): mutated via `QObject::connect` slot.
        TickSink sink;
        ScopedConnections scope;
        scope += QObject::connect(&source, &TickSource::tick, &sink, &TickSink::Bump);
        scope.Clear();
        scope.Clear();
        QVERIFY(scope.Empty());
        source.FireTick();
        QCOMPARE(sink.Count(), 0);
    }

    static void TestMoveTransfersOwnership()
    {
        TickSource source;
        // NOLINTNEXTLINE(misc-const-correctness): mutated via `QObject::connect` slot.
        TickSink sink;
        ScopedConnections original;
        original += QObject::connect(&source, &TickSource::tick, &sink, &TickSink::Bump);
        QCOMPARE(original.Size(), static_cast<std::size_t>(1));

        ScopedConnections receiver(std::move(original));
        QVERIFY(original.Empty()); // NOLINT(bugprone-use-after-move)
        QCOMPARE(receiver.Size(), static_cast<std::size_t>(1));

        source.FireTick();
        QCOMPARE(sink.Count(), 1);
        receiver.Clear();
        source.FireTick();
        QCOMPARE(sink.Count(), 1);

        ScopedConnections assignedFrom;
        assignedFrom += QObject::connect(&source, &TickSource::tick, &sink, &TickSink::Bump);
        ScopedConnections assignedTo;
        assignedTo = std::move(assignedFrom);
        QVERIFY(assignedFrom.Empty()); // NOLINT(bugprone-use-after-move)
        source.FireTick();
        QCOMPARE(sink.Count(), 2);
        assignedTo.Clear();
        source.FireTick();
        QCOMPARE(sink.Count(), 2);
    }

    static void TestMoveAssignReleasesPriorHeldConnections()
    {
        TickSource source;
        // NOLINTNEXTLINE(misc-const-correctness): mutated via `QObject::connect` slot.
        TickSink sink;

        ScopedConnections firstBag;
        firstBag += QObject::connect(&source, &TickSource::tick, &sink, &TickSink::Bump);

        ScopedConnections secondBag;
        secondBag += QObject::connect(&source, &TickSource::tick, &sink, &TickSink::Bump);

        source.FireTick();
        QCOMPARE(sink.Count(), 2); // Both bags deliver.

        // Move-assign into `firstBag`; its previously-held
        // connection must be disconnected so we do not leak a
        // subscription across an "unbind + rebind".
        firstBag = std::move(secondBag);
        QVERIFY(secondBag.Empty()); // NOLINT(bugprone-use-after-move)
        source.FireTick();
        QCOMPARE(sink.Count(), 3); // Only one delivery this time.
    }
};

QTEST_MAIN(ScopedConnectionsTest)
#include "test_scoped_connections.moc"
