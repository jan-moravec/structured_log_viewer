#include "log_filter_model.hpp"
#include "log_model.hpp"
#include "log_table_view.hpp"
#include "main_window.hpp"
#include <QtTest/QtTest>

class MainWindowTest : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        // Called before the first test function
        qDebug() << "Starting MainWindow tests";

        // Set the platform to offscreen
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }

    void cleanupTestCase()
    {
        // Called after the last test function
        qDebug() << "MainWindow tests complete";
    }

    void init()
    {
        // Called before each test function
        window = new MainWindow();
    }

    void cleanup()
    {
        // Called after each test function
        delete window;
        window = nullptr;
    }

    // Test methods
    void testWindowTitle()
    {
        QCOMPARE(window->windowTitle(), QString("Structured Log Viewer"));
    }

    void testWindowIcon()
    {
        QVERIFY(!window->windowIcon().isNull());
    }

private:
    MainWindow *window;
};

QTEST_MAIN(MainWindowTest)
#include "main_window_test.moc"
