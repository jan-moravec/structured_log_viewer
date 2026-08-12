#pragma once

#include <QMetaObject>
#include <QObject>

#include <utility>
#include <vector>

/**
 * @brief Owns a scoped set of Qt connection handles.
 *
 * Destruction and `Clear()` disconnect every stored connection. The class is
 * move-only; moving transfers the handles and empties the source.
 */
class ScopedConnections
{
public:
    /**
     * @brief Constructs an empty connection set.
     */
    ScopedConnections() = default;

    /**
     * @brief Disconnects all stored connections.
     */
    ~ScopedConnections()
    {
        Clear();
    }

    ScopedConnections(const ScopedConnections &) = delete;
    ScopedConnections &operator=(const ScopedConnections &) = delete;

    /**
     * @brief Transfers all connections from another set.
     *
     * @param other Set whose connections are transferred.
     */
    ScopedConnections(ScopedConnections &&other) noexcept
        : mConnections(std::move(other.mConnections))
    {
        other.mConnections.clear();
    }

    /**
     * @brief Replaces the stored connections with another set's connections.
     *
     * Existing connections are disconnected before the transfer.
     *
     * @param other Set whose connections are transferred.
     * @return A reference to this set.
     */
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

    /**
     * @brief Adds a connection to the set.
     *
     * Invalid connections are ignored.
     *
     * @param connection Connection to adopt.
     */
    void Add(QMetaObject::Connection connection)
    {
        if (connection)
        {
            mConnections.push_back(std::move(connection));
        }
    }

    /**
     * @brief Adds a connection and returns this set.
     *
     * @param connection Connection to adopt.
     * @return A reference to this set.
     */
    ScopedConnections &operator+=(QMetaObject::Connection connection)
    {
        Add(std::move(connection));
        return *this;
    }

    /**
     * @brief Disconnects and removes every stored connection.
     *
     * This operation is idempotent.
     */
    void Clear() noexcept
    {
        // Move handles aside so re-entrant clearing cannot revisit them.
        std::vector<QMetaObject::Connection> local;
        local.swap(mConnections);
        for (auto &connection : local)
        {
            QObject::disconnect(connection);
        }
    }

    /**
     * @brief Tests whether the set contains no connections.
     *
     * @return `true` when the set is empty.
     */
    [[nodiscard]] bool Empty() const noexcept
    {
        return mConnections.empty();
    }

    /**
     * @brief Returns the number of stored connections.
     *
     * @return The connection count.
     */
    [[nodiscard]] std::size_t Size() const noexcept
    {
        return mConnections.size();
    }

private:
    std::vector<QMetaObject::Connection> mConnections;
};
