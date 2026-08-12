#pragma once

#include <QMetaObject>
#include <QObject>

#include <utility>
#include <vector>

/// Move-only RAII bag of `QMetaObject::Connection` handles for one
/// scoped subscription set (typically the connections a shared dock
/// installs when it binds to the active `LogSession`).
///
/// Usage:
/// ```cpp
/// ScopedConnections scope;
/// scope += connect(session, &LogSession::rowsChanged, dock, ...);
/// scope += connect(view, &LogSessionView::selectionChanged, dock, ...);
/// // ~ScopedConnections or `Clear()` disconnects everything in the bag.
/// ```
///
/// Contract:
///
/// - `Clear` calls `QObject::disconnect` on every held connection and
///   then empties the bag; safe to call repeatedly.
/// - The destructor calls `Clear`.
/// - `disconnect` on an already-invalid connection is documented as a
///   no-op, so the bag is safe to destroy after either endpoint has
///   already been torn down.
/// - Move construction / move assignment transfer ownership; the
///   moved-from bag is empty and its destructor is a no-op.
/// - Copying is forbidden — a copy would double-disconnect.
class ScopedConnections
{
public:
    ScopedConnections() = default;
    ~ScopedConnections()
    {
        Clear();
    }

    ScopedConnections(const ScopedConnections &) = delete;
    ScopedConnections &operator=(const ScopedConnections &) = delete;

    ScopedConnections(ScopedConnections &&other) noexcept
        : mConnections(std::move(other.mConnections))
    {
        other.mConnections.clear();
    }

    ScopedConnections &operator=(ScopedConnections &&other) noexcept
    {
        if (this != &other)
        {
            Clear();
            mConnections = std::move(other.mConnections);
            other.mConnections.clear();
        }
        return *this;
    }

    /// Adopt @p connection into the bag. Invalid connections
    /// (default-constructed sentinel) are dropped silently so
    /// callers can pass the result of `connect` unchecked.
    void Add(QMetaObject::Connection connection)
    {
        if (connection)
        {
            mConnections.push_back(std::move(connection));
        }
    }

    /// Same as `Add` but chainable for `scope += connect(...)`.
    ScopedConnections &operator+=(QMetaObject::Connection connection)
    {
        Add(std::move(connection));
        return *this;
    }

    /// Disconnect every held connection and empty the bag.
    /// Idempotent; safe to call from a QObject destructor.
    void Clear() noexcept
    {
        // Iterate a moved-out local so a re-entrant `Clear` from a
        // slot invoked by a `disconnect` cannot revisit the same
        // connections. `disconnect` on an invalid Connection is a
        // no-op per Qt's documented contract.
        std::vector<QMetaObject::Connection> local;
        local.swap(mConnections);
        for (auto &connection : local)
        {
            QObject::disconnect(connection);
        }
    }

    [[nodiscard]] bool Empty() const noexcept
    {
        return mConnections.empty();
    }

    [[nodiscard]] std::size_t Size() const noexcept
    {
        return mConnections.size();
    }

private:
    std::vector<QMetaObject::Connection> mConnections;
};
