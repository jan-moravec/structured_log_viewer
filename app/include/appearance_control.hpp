#pragma once

#include <QString>

class AppearanceControl
{
public:
    static bool IsDarkTheme();
    static void SaveConfiguration();
    static void LoadConfiguration();

private:
    struct Configuration
    {
        QString style;
        QString font;
    };

    static Configuration mConfiguration;
};
