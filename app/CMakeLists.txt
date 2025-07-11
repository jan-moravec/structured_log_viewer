cmake_minimum_required(VERSION 3.19)

set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

find_package(
    QT
    NAMES
    Qt6
    Qt5
    REQUIRED
    COMPONENTS Widgets)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Widgets)

if(${QT_VERSION} LESS 6.1.0)
    message(FATAL_ERROR "Qt version must be >= 6.1")
endif()

set(PROJECT_SOURCES
    src/main_window.cpp
    include/main_window.hpp
    src/log_model.cpp
    include/log_model.hpp
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
    include/appearance_control.hpp)

qt_add_library(logapp STATIC ${PROJECT_SOURCES})

# Define target properties for Android with Qt 6 as: set_property(TARGET app APPEND PROPERTY
# QT_ANDROID_PACKAGE_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/android) For more information, see
# https://doc.qt.io/qt-6/qt-add-executable.html#target-creation
target_include_directories(logapp PUBLIC ./include)

target_link_libraries(logapp PUBLIC Qt${QT_VERSION_MAJOR}::Widgets loglib project_warnings)

qt_add_resources(RESOURCES ${CMAKE_CURRENT_SOURCE_DIR}/../resources/resources.qrc)
qt_add_executable(StructuredLogViewer MANUAL_FINALIZATION src/main.cpp ${RESOURCES})

target_link_libraries(StructuredLogViewer PUBLIC logapp)

set_target_properties(
    StructuredLogViewer
    PROPERTIES ${BUNDLE_ID_OPTION} MACOSX_BUNDLE_BUNDLE_VERSION
               ${PROJECT_VERSION} MACOSX_BUNDLE_SHORT_VERSION_STRING
               ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR} MACOSX_BUNDLE
               TRUE WIN32_EXECUTABLE
               TRUE)

include(GNUInstallDirs)
install(
    TARGETS StructuredLogViewer
    BUNDLE DESTINATION .
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

qt_finalize_executable(StructuredLogViewer)
