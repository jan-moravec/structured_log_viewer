#include "main_window.hpp"

#include <QApplication>
#include <QStyleFactory>

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);
    a.setStyle(QStyleFactory::create("windows11")); // "windows11", "windowsvista", "Windows", "Fusion"
    MainWindow w;
    w.show();
    return a.exec();
}
