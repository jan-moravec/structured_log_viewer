#include "appearance_control.hpp"

#include <QApplication>
#include <QFont>
#include <QPalette>
#include <QSettings>
#include <QStyle>
#include <QStyleFactory>

namespace
{
const QString CONFIGURATION_STYLE = "appearence/style";
const QString CONFIGURATION_FONT = "appearence/font";
} // namespace

AppearanceControl::Configuration AppearanceControl::mConfiguration;

bool AppearanceControl::IsDarkTheme()
{
    QColor bgColor = qApp->palette().color(QPalette::Window);
    int brightness = (bgColor.red() * 299 + bgColor.green() * 587 + bgColor.blue() * 114) / 1000;
    return brightness < 128;
}

void AppearanceControl::SaveConfiguration()
{
    mConfiguration.style = qApp->style()->name();
    mConfiguration.font = qApp->font().toString();

    QSettings settings;
    settings.setValue(CONFIGURATION_STYLE, mConfiguration.style);
    settings.setValue(CONFIGURATION_FONT, mConfiguration.font);
}

void AppearanceControl::LoadConfiguration()
{
    if (mConfiguration.style.isEmpty())
    {
        static const QString DEFAULT_STYLE = "fusion";
        mConfiguration.style = DEFAULT_STYLE;
    }
    if (mConfiguration.font.isEmpty())
    {
        mConfiguration.font = qApp->font().toString();
    }

    QSettings settings;
    if (const QVariant value = settings.value(CONFIGURATION_STYLE); value.isValid())
    {
        mConfiguration.style = value.toString();
    }
    if (const QVariant value = settings.value(CONFIGURATION_FONT); value.isValid())
    {
        mConfiguration.font = value.toString();
    }

    if (qApp->style()->name() != mConfiguration.style)
    {
        QStyle *style = QStyleFactory::create(mConfiguration.style);
        if (style)
        {
            qApp->setStyle(style);
        }
        else
        {
            // Invalid style configuration
            settings.remove(CONFIGURATION_STYLE);
            mConfiguration.style.clear();
        }
    }
    if (qApp->font().toString() != mConfiguration.font)
    {
        QFont font;
        if (font.fromString(mConfiguration.font))
        {
            qApp->setFont(font);
        }
        else
        {
            // Invalid font configuration
            settings.remove(CONFIGURATION_FONT);
            mConfiguration.font.clear();
        }
    }
}
