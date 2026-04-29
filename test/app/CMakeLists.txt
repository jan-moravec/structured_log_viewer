cmake_minimum_required(VERSION 3.28)

find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Widgets)

find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Test Widgets)

set(CMAKE_AUTOMOC ON)

qt_add_resources(RESOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/../../resources/resources.qrc
    ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/fixtures.qrc
)
qt_add_executable(apptest src/main_window_test.cpp ${RESOURCES})

target_link_libraries(apptest PRIVATE Qt${QT_VERSION_MAJOR}::Test logapp)

target_include_directories(apptest PRIVATE ${CMAKE_SOURCE_DIR}/library/include)

# WORKING_DIRECTORY pinned to the binary directory so MainWindow's deferred
# tzdata loader resolves `current_path() / "tzdata"` against the staged
# folder. Otherwise ctest launches apptest from build/test/app/ where no
# tzdata exists and the loader fires a QMessageBox that segfaults under
# the Qt offscreen platform.
add_test(NAME apptest COMMAND apptest WORKING_DIRECTORY $<TARGET_FILE_DIR:apptest>)
