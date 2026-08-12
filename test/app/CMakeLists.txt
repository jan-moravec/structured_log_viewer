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

# `main_window_test.cpp` is a monolithic ~20k line TU; MSVC's COFF
# object files hit the 65536-section limit without `/bigobj`. GCC /
# clang cost nothing here.
if(MSVC)
    target_compile_options(apptest PRIVATE /bigobj)
endif()

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

# Focused tests for the shared `leaf_rule_compile` translation unit
# (`ResolveLeafColumnByKeys`, `CompileLeaf`, `CompileExpression`).
# Same concern-per-binary pattern as the tests above; drills into
# each `CompileLeaf` arm's absent-branch and the combinator
# propagation rules so a regression in a single arm surfaces here
# instead of as a distant model-rebuild diff.
qt_add_executable(apptest_leaf_rule_compile src/test_leaf_rule_compile.cpp)

target_link_libraries(apptest_leaf_rule_compile PRIVATE Qt6::Test Qt6::Widgets logapp)

target_include_directories(apptest_leaf_rule_compile PRIVATE ${CMAKE_SOURCE_DIR}/library/include)

add_test(
    NAME apptest_leaf_rule_compile
    COMMAND apptest_leaf_rule_compile
    WORKING_DIRECTORY $<TARGET_FILE_DIR:apptest_leaf_rule_compile>
)

# Focused tests for the filtered-row exporter (ROADMAP item 7).
# Same concern-per-binary pattern as the tests above; covers each
# formatter (JSON Lines, CSV, Snapshot, Markdown) plus the
# `FileSink` atomic-rename contract and the cancel-leaves-no-partial
# invariant.
qt_add_executable(apptest_row_exporter src/test_row_exporter.cpp)

target_link_libraries(apptest_row_exporter PRIVATE Qt6::Test Qt6::Widgets logapp)

target_include_directories(apptest_row_exporter PRIVATE ${CMAKE_SOURCE_DIR}/library/include)

add_test(
    NAME apptest_row_exporter
    COMMAND apptest_row_exporter
    WORKING_DIRECTORY $<TARGET_FILE_DIR:apptest_row_exporter>
)

# ScopedConnections RAII helper (task 1.4). Pure QObject / signal
# plumbing so no widgets, tzdata, or resources are required.
qt_add_executable(apptest_scoped_connections src/test_scoped_connections.cpp)

target_link_libraries(apptest_scoped_connections PRIVATE Qt6::Test logapp)

target_include_directories(apptest_scoped_connections PRIVATE ${CMAKE_SOURCE_DIR}/library/include)

add_test(
    NAME apptest_scoped_connections
    COMMAND apptest_scoped_connections
    WORKING_DIRECTORY $<TARGET_FILE_DIR:apptest_scoped_connections>
)

# Focused characterization + direct `LogSession` / `LogSessionView`
# / `SessionBindContext` / tab / workspace-persistence test targets
# for the MainWindow-architecture / multi-source-tabs feature (see
# `tasks/tasks-main-window-session-tabs.md` §1.9). Phase 1 seeds a
# minimal test per binary; later phases grow each file into the
# authoritative coverage venue for its concern instead of extending
# the monolithic `main_window_test.cpp`.
qt_add_executable(apptest_log_session_lifecycle src/log_session_lifecycle_test.cpp)
target_link_libraries(apptest_log_session_lifecycle PRIVATE Qt6::Test Qt6::Widgets logapp)
target_include_directories(apptest_log_session_lifecycle PRIVATE ${CMAKE_SOURCE_DIR}/library/include)
add_test(
    NAME apptest_log_session_lifecycle
    COMMAND apptest_log_session_lifecycle
    WORKING_DIRECTORY $<TARGET_FILE_DIR:apptest_log_session_lifecycle>
)

qt_add_executable(apptest_log_session src/log_session_test.cpp)
target_link_libraries(apptest_log_session PRIVATE Qt6::Test Qt6::Widgets logapp)
target_include_directories(apptest_log_session PRIVATE ${CMAKE_SOURCE_DIR}/library/include)
add_test(NAME apptest_log_session COMMAND apptest_log_session WORKING_DIRECTORY $<TARGET_FILE_DIR:apptest_log_session>)

qt_add_executable(apptest_log_session_view src/log_session_view_test.cpp)
target_link_libraries(apptest_log_session_view PRIVATE Qt6::Test Qt6::Widgets logapp)
target_include_directories(apptest_log_session_view PRIVATE ${CMAKE_SOURCE_DIR}/library/include)
add_test(
    NAME apptest_log_session_view
    COMMAND apptest_log_session_view
    WORKING_DIRECTORY $<TARGET_FILE_DIR:apptest_log_session_view>
)

qt_add_executable(apptest_dock_binding src/dock_binding_test.cpp)
target_link_libraries(apptest_dock_binding PRIVATE Qt6::Test Qt6::Widgets logapp)
target_include_directories(apptest_dock_binding PRIVATE ${CMAKE_SOURCE_DIR}/library/include)
add_test(
    NAME apptest_dock_binding
    COMMAND apptest_dock_binding
    WORKING_DIRECTORY $<TARGET_FILE_DIR:apptest_dock_binding>
)

qt_add_executable(apptest_session_tabs src/session_tabs_test.cpp)
target_link_libraries(apptest_session_tabs PRIVATE Qt6::Test Qt6::Widgets logapp)
target_include_directories(apptest_session_tabs PRIVATE ${CMAKE_SOURCE_DIR}/library/include)
add_test(
    NAME apptest_session_tabs
    COMMAND apptest_session_tabs
    WORKING_DIRECTORY $<TARGET_FILE_DIR:apptest_session_tabs>
)

qt_add_executable(apptest_workspace_persistence src/workspace_persistence_test.cpp)
target_link_libraries(apptest_workspace_persistence PRIVATE Qt6::Test Qt6::Widgets logapp)
target_include_directories(apptest_workspace_persistence PRIVATE ${CMAKE_SOURCE_DIR}/library/include)
add_test(
    NAME apptest_workspace_persistence
    COMMAND apptest_workspace_persistence
    WORKING_DIRECTORY $<TARGET_FILE_DIR:apptest_workspace_persistence>
)
