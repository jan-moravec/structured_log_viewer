cmake_minimum_required(VERSION 3.28)

set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Widgets)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Widgets)

if(${QT_VERSION} LESS 6.1.0)
    message(FATAL_ERROR "Qt version must be >= 6.1")
endif()

set(PROJECT_SOURCES
    src/main_window.cpp
    include/main_window.hpp
    src/log_model.cpp
    include/log_model.hpp
    src/qt_streaming_log_sink.cpp
    include/qt_streaming_log_sink.hpp
    src/log_table_view.cpp
    include/log_table_view.hpp
    src/find_record_widget.cpp
    include/find_record_widget.hpp
    src/log_filter_model.cpp
    include/log_filter_model.hpp
    src/filter_editor.cpp
    include/filter_editor.hpp
    src/preferences_editor.cpp
    include/preferences_editor.hpp
    src/appearance_control.cpp
    include/appearance_control.hpp
)

qt_add_library(logapp STATIC ${PROJECT_SOURCES})

target_include_directories(logapp PUBLIC ./include)

target_link_libraries(logapp PUBLIC Qt${QT_VERSION_MAJOR}::Widgets loglib project_warnings)

qt_add_resources(RESOURCES ${CMAKE_CURRENT_SOURCE_DIR}/../resources/resources.qrc)
qt_add_executable(StructuredLogViewer MANUAL_FINALIZATION src/main.cpp ${RESOURCES})

target_link_libraries(StructuredLogViewer PUBLIC logapp)

set_target_properties(
    StructuredLogViewer
    PROPERTIES
        ${BUNDLE_ID_OPTION} MACOSX_BUNDLE_BUNDLE_VERSION
        ${PROJECT_VERSION} MACOSX_BUNDLE_SHORT_VERSION_STRING
        ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR} MACOSX_BUNDLE
        TRUE WIN32_EXECUTABLE
        TRUE
)

# On macOS the executable runs from <App>.app/Contents/MacOS/, so the tzdata
# directory that FetchDependencies.cmake copies next to the build output is not
# reachable at runtime. Mirror it into the bundle's Contents/Resources so the
# application can find it via QCoreApplication::applicationDirPath().
if(APPLE)
    add_custom_command(
        TARGET StructuredLogViewer
        POST_BUILD
        COMMAND
            ${CMAKE_COMMAND} -E copy_directory ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/tzdata
            $<TARGET_BUNDLE_CONTENT_DIR:StructuredLogViewer>/Resources/tzdata
        COMMENT "Copying tzdata into macOS app bundle Resources"
        VERBATIM
    )
endif()

include(GNUInstallDirs)
install(
    TARGETS StructuredLogViewer
    BUNDLE DESTINATION .
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

# On Windows the TBB runtime DLL (tbb12.dll) is loaded from the directory next to the
# executable. Copy it into the build tree so the binary can be launched directly from
# build/, and install it next to the binary so the packaged release works too. On
# non-Windows platforms TBB::tbb is resolved via rpath (FetchContent build) or distro
# library paths (USE_SYSTEM_TBB=ON), so no copy step is needed.
if(WIN32)
    add_custom_command(
        TARGET StructuredLogViewer
        POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:TBB::tbb> $<TARGET_FILE_DIR:StructuredLogViewer>
        COMMENT "Copying TBB runtime DLL next to StructuredLogViewer.exe"
        VERBATIM
    )

    install(FILES $<TARGET_FILE:TBB::tbb> DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

qt_finalize_executable(StructuredLogViewer)
