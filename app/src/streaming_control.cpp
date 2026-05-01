#include "streaming_control.hpp"

#include <QSettings>
#include <QVariant>

#include <algorithm>

namespace
{
const QString CONFIGURATION_RETENTION_LINES = "streaming/retentionLines";
} // namespace

StreamingControl::Configuration StreamingControl::mConfiguration;

void StreamingControl::SaveConfiguration()
{
    QSettings settings;
    settings.setValue(CONFIGURATION_RETENTION_LINES, static_cast<qulonglong>(mConfiguration.retentionLines));
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
            return;
        }
        // Out-of-range or unparsable value: drop and fall through to the
        // default. Avoids a corrupted setting wedging the spinbox.
        settings.remove(CONFIGURATION_RETENTION_LINES);
    }
    mConfiguration.retentionLines = kDefaultRetentionLines;
}

size_t StreamingControl::RetentionLines()
{
    return mConfiguration.retentionLines;
}

void StreamingControl::SetRetentionLines(size_t value)
{
    mConfiguration.retentionLines = std::clamp(value, kMinRetentionLines, kMaxRetentionLines);
}
