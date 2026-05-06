#include "appearance_control.hpp"

#include <QApplication>
#include <QFont>
#include <QLatin1String>
#include <QPalette>
#include <QSettings>
#include <QStyle>
#include <QStyleFactory>

namespace
{
constexpr char CONFIGURATION_STYLE[] = "appearance/style";
constexpr char CONFIGURATION_FONT[] = "appearance/font";

constexpr int K_MID_GRAY_BRIGHTNESS = 128;
} // namespace

AppearanceControl::Configuration AppearanceControl::mConfiguration;

bool AppearanceControl::IsDarkTheme()
{
    const QColor bgColor = qApp->palette().color(QPalette::Window);
    const int brightness = ((bgColor.red() * 299) + (bgColor.green() * 587) + (bgColor.blue() * 114)) / 1000;
    return brightness < K_MID_GRAY_BRIGHTNESS;
}

void AppearanceControl::SaveConfiguration()
{
    mConfiguration.style = qApp->style()->name();
    mConfiguration.font = qApp->font().toString();

    QSettings settings;
    settings.setValue(QLatin1String(CONFIGURATION_STYLE), mConfiguration.style);
    settings.setValue(QLatin1String(CONFIGURATION_FONT), mConfiguration.font);
}

void AppearanceControl::LoadConfiguration()
{
    if (mConfiguration.style.isEmpty())
    {
        mConfiguration.style = QStringLiteral("fusion");
    }
    if (mConfiguration.font.isEmpty())
    {
        mConfiguration.font = qApp->font().toString();
    }

    QSettings settings;
    if (const QVariant value = settings.value(QLatin1String(CONFIGURATION_STYLE)); value.isValid())
    {
        mConfiguration.style = value.toString();
    }
    if (const QVariant value = settings.value(QLatin1String(CONFIGURATION_FONT)); value.isValid())
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
            settings.remove(QLatin1String(CONFIGURATION_STYLE));
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
            settings.remove(QLatin1String(CONFIGURATION_FONT));
            mConfiguration.font.clear();
        }
    }
}
