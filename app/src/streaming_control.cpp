#include "streaming_control.hpp"

#include <QSettings>
#include <QVariant>

#include <algorithm>

namespace
{
const QString CONFIGURATION_RETENTION_LINES = "streaming/retentionLines";
const QString CONFIGURATION_NEWEST_FIRST = "streaming/newestFirst";
} // namespace

StreamingControl::Configuration StreamingControl::mConfiguration;

void StreamingControl::SaveConfiguration()
{
    QSettings settings;
    settings.setValue(CONFIGURATION_RETENTION_LINES, static_cast<qulonglong>(mConfiguration.retentionLines));
    settings.setValue(CONFIGURATION_NEWEST_FIRST, mConfiguration.newestFirst);
}

void StreamingControl::LoadConfiguration()
{
    QSettings settings;
    if (const QVariant value = settings.value(CONFIGURATION_RETENTION_LINES); value.isValid())
    {
        bool ok = false;
        const qulonglong raw = value.toULongLong(&ok);
        if (ok && raw >= MIN_RETENTION_LINES && raw <= MAX_RETENTION_LINES)
        {
            mConfiguration.retentionLines = static_cast<size_t>(raw);
        }
        else
        {
            // Drop a corrupt / out-of-range value so it can't wedge
            // the spinbox.
            settings.remove(CONFIGURATION_RETENTION_LINES);
            mConfiguration.retentionLines = DEFAULT_RETENTION_LINES;
        }
    }
    else
    {
        mConfiguration.retentionLines = DEFAULT_RETENTION_LINES;
    }

    if (const QVariant value = settings.value(CONFIGURATION_NEWEST_FIRST); value.isValid())
    {
        mConfiguration.newestFirst = value.toBool();
    }
    else
    {
        mConfiguration.newestFirst = DEFAULT_NEWEST_FIRST;
    }
}

size_t StreamingControl::RetentionLines()
{
    return mConfiguration.retentionLines;
}

void StreamingControl::SetRetentionLines(size_t value)
{
    mConfiguration.retentionLines = std::clamp(value, MIN_RETENTION_LINES, MAX_RETENTION_LINES);
}

bool StreamingControl::IsNewestFirst()
{
    return mConfiguration.newestFirst;
}

void StreamingControl::SetNewestFirst(bool value)
{
    mConfiguration.newestFirst = value;
}
