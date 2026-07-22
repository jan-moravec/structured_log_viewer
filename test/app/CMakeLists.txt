cmake_minimum_required(VERSION 3.28)

# `Network` / `Svg` are required at this scope because logapp now
# PUBLIC-links both: `QHostAddress` validates the network-stream
# dialog's bind-address, and `QSvgRenderer` powers the themed-icon
# loader behind the primary toolbar. The Qt CMake auto-finalizer
# walks transitive dependencies and warns on missing targets
# otherwise; pulling the find_package up here keeps the warning
# silent without bloating the tests' own dependency surface.
find_package(Qt6 6.1 REQUIRED COMPONENTS Test Widgets Network Svg)

set(CMAKE_AUTOMOC ON)

qt_add_resources(RESOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/../../resources/resources.qrc
    ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/fixtures.qrc
)
qt_add_executable(apptest src/main_window_test.cpp ${RESOURCES})

# `test_common` provides Tcp/UdpLogClient used by the network-stream
# open-path tests. Qt::Test stays first so generated MOC integrates
# normally; logapp pulls in the GUI under test.
#
# ZLIB::ZLIB is linked so the transparent-decompression regression
# tests can compress a fixture in-process (deterministic byte-for-byte
# output, no dependency on a system `gzip` binary). Only gzip is
# needed at the test surface: the other three codecs are already
# exercised by `test_lib_decompressing_byte_source`, and the app-side
# tests focus on the MainWindow orchestration, which is codec-agnostic.
target_link_libraries(apptest PRIVATE Qt6::Test logapp test_common ZLIB::ZLIB)

target_include_directories(apptest PRIVATE ${CMAKE_SOURCE_DIR}/library/include)

# Expose the test-only accessors on `logapp` classes (cache-size
# probes, column-move helpers, session-mode shims) while we're
# building tests. The macro is set as a PUBLIC compile definition on
# `logapp` so both the library TU and every test TU see the same
# decls. Gated by `BUILD_TESTING` at the top level via the
# `add_subdirectory(test/...)` block, so `-DBUILD_TESTING=OFF` strips
# the helpers from shipped binaries. Mirrors the existing
# `LOGLIB_KEY_INDEX_INSTRUMENTATION` pattern in `test/lib`.
target_compile_definitions(logapp PUBLIC LOGAPP_BUILD_TESTING)

# WORKING_DIRECTORY pinned to the binary directory so
# `MainWindow::InitializeTimezoneDatabase` (called from the QtTest
# `initTestCase` slot) resolves the staged `tzdata/` against the binary's
# directory. Both the `appDir / "tzdata"` probe and the CWD-ancestor
# fallback succeed under this layout; without the pin, ctest launches
# apptest from build/test/app/ where no tzdata exists and the init slot
# fails the suite with a `QVERIFY2` diagnostic.
add_test(NAME apptest COMMAND apptest WORKING_DIRECTORY $<TARGET_FILE_DIR:apptest>)

# Separate executable for the GUI-path throughput benchmark. Kept out of
# the default ctest run via the `benchmark` label (mirrors the lib test
# tree's convention): `ctest -L benchmark` to include it,
# `ctest -LE benchmark` to skip it.
qt_add_executable(apptest_bench src/benchmark_main_window.cpp)

target_link_libraries(apptest_bench PRIVATE Qt6::Test logapp test_common)

target_include_directories(apptest_bench PRIVATE ${CMAKE_SOURCE_DIR}/library/include)

add_test(NAME apptest_bench COMMAND apptest_bench WORKING_DIRECTORY $<TARGET_FILE_DIR:apptest_bench>)
set_tests_properties(apptest_bench PROPERTIES LABELS "benchmark")

# Unit tests for the BoundedBatchQueue primitive (pure C++; no Qt event
# loop needed). Kept in its own executable so the back-pressure tests do
# not pull in the GUI fixture loader.
qt_add_executable(apptest_queue src/test_bounded_batch_queue.cpp)

target_link_libraries(apptest_queue PRIVATE Qt6::Test logapp)

target_include_directories(apptest_queue PRIVATE ${CMAKE_SOURCE_DIR}/library/include)

add_test(NAME apptest_queue COMMAND apptest_queue WORKING_DIRECTORY $<TARGET_FILE_DIR:apptest_queue>)

# Unit tests for ThemeControl. Needs a QApplication (palette + setStyle)
# and the built-in theme JSONs from resources.qrc, so it links the
# resources separately.
qt_add_resources(THEME_RESOURCES ${CMAKE_CURRENT_SOURCE_DIR}/../../resources/resources.qrc)
qt_add_executable(apptest_theme src/test_theme_control.cpp ${THEME_RESOURCES})

target_link_libraries(apptest_theme PRIVATE Qt6::Test Qt6::Widgets logapp)

target_include_directories(apptest_theme PRIVATE ${CMAKE_SOURCE_DIR}/library/include)

add_test(NAME apptest_theme COMMAND apptest_theme WORKING_DIRECTORY $<TARGET_FILE_DIR:apptest_theme>)

# Unit tests for the histogram / activity-rate strip (ROADMAP item 2).
# Focused binary so the histogram concern doesn't inflate the
# monolithic `apptest`. Runs the same offscreen QPA pattern.
qt_add_executable(apptest_histogram src/test_histogram_dock.cpp)

target_link_libraries(apptest_histogram PRIVATE Qt6::Test Qt6::Widgets logapp)

target_include_directories(apptest_histogram PRIVATE ${CMAKE_SOURCE_DIR}/library/include)

add_test(NAME apptest_histogram COMMAND apptest_histogram WORKING_DIRECTORY $<TARGET_FILE_DIR:apptest_histogram>)

# Unit tests for the match overview rail (ROADMAP item 13). Same
# concern-per-binary pattern as `apptest_histogram`: keeps the
# monolithic `apptest` lean and lets `ctest -R apptest_overview_rail`
# rerun just this concern.
#
# Links the built-in theme JSONs from `resources.qrc` so the paint
# tests can load the real Dark / Light themes via
# `ThemeControl::SetActiveSelection` and assert contrast against a
# realistic palette (not the offscreen QPA's blank fallback).
qt_add_resources(RAIL_TEST_RESOURCES ${CMAKE_CURRENT_SOURCE_DIR}/../../resources/resources.qrc)
qt_add_executable(apptest_overview_rail src/test_overview_rail.cpp ${RAIL_TEST_RESOURCES})

target_link_libraries(apptest_overview_rail PRIVATE Qt6::Test Qt6::Widgets logapp)

target_include_directories(apptest_overview_rail PRIVATE ${CMAKE_SOURCE_DIR}/library/include)

add_test(
    NAME apptest_overview_rail
    COMMAND apptest_overview_rail
    WORKING_DIRECTORY $<TARGET_FILE_DIR:apptest_overview_rail>
)

# Focused tests for `HighlightRuleSet` (ROADMAP item 3). Kept out
# of the monolithic `apptest` so `ctest -R apptest_highlight_rules`
# reruns just this concern; mirrors the histogram / overview-rail
# split.
qt_add_executable(apptest_highlight_rules src/test_highlight_rule_set.cpp)

target_link_libraries(apptest_highlight_rules PRIVATE Qt6::Test Qt6::Widgets logapp)

target_include_directories(apptest_highlight_rules PRIVATE ${CMAKE_SOURCE_DIR}/library/include)

add_test(
    NAME apptest_highlight_rules
    COMMAND apptest_highlight_rules
    WORKING_DIRECTORY $<TARGET_FILE_DIR:apptest_highlight_rules>
)
