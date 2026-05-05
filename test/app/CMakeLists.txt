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

# Separate executable for the GUI-path throughput benchmark. Kept out of
# the default ctest run via the `benchmark` label (mirrors the lib test
# tree's convention): `ctest -L benchmark` to include it,
# `ctest -LE benchmark` to skip it.
qt_add_executable(apptest_bench src/benchmark_main_window.cpp)

target_link_libraries(apptest_bench PRIVATE Qt${QT_VERSION_MAJOR}::Test logapp test_common)

target_include_directories(apptest_bench PRIVATE ${CMAKE_SOURCE_DIR}/library/include)

add_test(NAME apptest_bench COMMAND apptest_bench WORKING_DIRECTORY $<TARGET_FILE_DIR:apptest_bench>)
set_tests_properties(apptest_bench PROPERTIES LABELS "benchmark")

# Unit tests for the BoundedBatchQueue primitive (pure C++; no Qt event
# loop needed). Kept in its own executable so the back-pressure tests do
# not pull in the GUI fixture loader.
qt_add_executable(apptest_queue src/test_bounded_batch_queue.cpp)

target_link_libraries(apptest_queue PRIVATE Qt${QT_VERSION_MAJOR}::Test logapp)

target_include_directories(apptest_queue PRIVATE ${CMAKE_SOURCE_DIR}/library/include)

add_test(NAME apptest_queue COMMAND apptest_queue WORKING_DIRECTORY $<TARGET_FILE_DIR:apptest_queue>)
