# Library tests CMakeLists.txt
cmake_minimum_required(VERSION 3.19)

find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Widgets)

find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Test Widgets)

# Enable automoc for Qt's meta-object compiler
set(CMAKE_AUTOMOC ON)

# Create the test executable for MainWindow
qt_add_resources(RESOURCES ${CMAKE_CURRENT_SOURCE_DIR}/../../resources/resources.qrc)
qt_add_executable(apptest src/main_window_test.cpp ${RESOURCES})

# Link against Qt libraries and your library code
target_link_libraries(apptest PRIVATE Qt${QT_VERSION_MAJOR}::Test logapp)

# Include directories
target_include_directories(apptest PRIVATE ${CMAKE_SOURCE_DIR}/library/include)

# Add the test to CTest. The working directory is pinned to the binary directory
# so the deferred MainWindow::QTimer::singleShot tzdata loader (which resolves
# `current_path() / "tzdata"`) finds the same `tzdata/` folder the production
# Windows install layout uses. Without this, ctest launches apptest from the
# build/test/app/ directory where no tzdata exists and the loader fires a
# QMessageBox::critical that segfaults under the Qt offscreen platform.
add_test(NAME apptest COMMAND apptest WORKING_DIRECTORY $<TARGET_FILE_DIR:apptest>)
