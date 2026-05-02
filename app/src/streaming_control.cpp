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
        if (ok && raw >= kMinRetentionLines && raw <= kMaxRetentionLines)
        {
            mConfiguration.retentionLines = static_cast<size_t>(raw);
        }
        else
        {
            // Out-of-range or unparsable value: drop and fall through to the
            // default. Avoids a corrupted setting wedging the spinbox.
            settings.remove(CONFIGURATION_RETENTION_LINES);
            mConfiguration.retentionLines = kDefaultRetentionLines;
        }
    }
    else
    {
        mConfiguration.retentionLines = kDefaultRetentionLines;
    }

    if (const QVariant value = settings.value(CONFIGURATION_NEWEST_FIRST); value.isValid())
    {
        // `QVariant::toBool` accepts both the canonical bool encoding
        // and the legacy string forms QSettings may produce on some
        // backends; either way an unparsable value falls back to the
        // default below.
        mConfiguration.newestFirst = value.toBool();
    }
    else
    {
        mConfiguration.newestFirst = kDefaultNewestFirst;
    }
}

size_t StreamingControl::RetentionLines()
{
    return mConfiguration.retentionLines;
}

void StreamingControl::SetRetentionLines(size_t value)
{
    mConfiguration.retentionLines = std::clamp(value, kMinRetentionLines, kMaxRetentionLines);
}

bool StreamingControl::IsNewestFirst()
{
    return mConfiguration.newestFirst;
}

void StreamingControl::SetNewestFirst(bool value)
{
    mConfiguration.newestFirst = value;
}
