#include "main_window.hpp"

#include "appearance_control.hpp"

#include <QApplication>

int main(int argc, char *argv[])
{
    const QApplication a(argc, argv);

    QCoreApplication::setOrganizationName("jan-moravec");
    QCoreApplication::setApplicationName("StructuredLogViewer");

    AppearanceControl::LoadConfiguration();

    MainWindow w;
    w.show();

    return QApplication::exec();
}
