#include "loglib/column_projection.hpp"

namespace loglib
{

namespace
{

std::vector<std::size_t> CollectVisible(std::span<const LogConfiguration::Column> columns)
{
    std::vector<std::size_t> indices;
    indices.reserve(columns.size());
    for (std::size_t i = 0; i < columns.size(); ++i)
    {
        if (columns[i].visible)
        {
            indices.push_back(i);
        }
    }
    return indices;
}

} // namespace

ColumnProjection::ColumnProjection(std::span<const LogConfiguration::Column> columns)
    : mIndices(CollectVisible(columns))
{
}

ColumnProjection::ColumnProjection(const LogConfiguration &configuration)
    : ColumnProjection(std::span<const LogConfiguration::Column>(configuration.columns))
{
}

} // namespace loglib
