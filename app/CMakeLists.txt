cmake_minimum_required(version 3.19)

project(
    StructuredLogViewer
    version 0.1
    languages CXX)

find_package(
    QT
    NAMES
    Qt6
    Qt5
    required
    components Widgets)
find_package(Qt${QT_VERSION_MAJOR} required components Widgets)

set(PROJECT_SOURCES
    src/main.cpp
    src/main_window.cpp
    include/main_window.hpp
    src/main_window.ui
    include/log_model.hpp
    src/log_model.cpp)

qt_add_resources(RESOURCES ../resources/resources.qrc)

if(${QT_VERSION_MAJOR} GREATER_EQUAL 6)
    qt_add_executable(
        ${PROJECT_NAME}
        MANUAL_FINALIZATION
        ${PROJECT_SOURCES}
        ${RESOURCES}
        include/log_table_view.hpp
        src/log_table_view.cpp
        include/find_record_widget.hpp
        src/find_record_widget.cpp
        include/log_filter_model.hpp
        src/log_filter_model.cpp
        include/filter_rule.hpp
        include/filter_editor.hpp
        src/filter_editor.cpp)

    # Define target properties for Android with Qt 6 as: set_property(TARGET app APPEND PROPERTY
    # QT_ANDROID_PACKAGE_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/android) For more information, see
    # https://doc.qt.io/qt-6/qt-add-executable.html#target-creation
else()
    if(ANDROID)
        add_library(${PROJECT_NAME} shared ${PROJECT_SOURCES})

        # Define properties for Android with Qt 5 after find_package() calls as: set(ANDROID_PACKAGE_SOURCE_DIR
        # "${CMAKE_CURRENT_SOURCE_DIR}/android")
    else()
        add_executable(${PROJECT_NAME} ${PROJECT_SOURCES})
    endif()
endif()

target_include_directories(${PROJECT_NAME} public ./include)

target_link_libraries(${PROJECT_NAME} private Qt${QT_VERSION_MAJOR}::Widgets loglib)

# Qt for iOS sets MACOSX_BUNDLE_GUI_IDENTIFIER automatically since Qt 6.1. If you are developing for iOS or macOS you
# should consider setting an explicit, fixed bundle identifier manually though.
if(${QT_VERSION} version_less 6.1.0)
    set(BUNDLE_ID_OPTION MACOSX_BUNDLE_GUI_IDENTIFIER com.example.${PROJECT_NAME})
endif()

set_target_properties(
    ${PROJECT_NAME}
    properties ${BUNDLE_ID_OPTION} MACOSX_BUNDLE_BUNDLE_VERSION
               ${PROJECT_VERSION} MACOSX_BUNDLE_SHORT_VERSION_STRING
               ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR} MACOSX_BUNDLE
               TRUE WIN32_EXECUTABLE
               TRUE)

include(GNUInstallDirs)
install(
    targets ${PROJECT_NAME}
    bundle destination .
    library destination ${CMAKE_INSTALL_LIBDIR}
    runtime destination ${CMAKE_INSTALL_BINDIR})

if(QT_VERSION_MAJOR equal 6)
    qt_finalize_executable(${PROJECT_NAME})
endif()

# Copy the timezone data next to the binary
execute_process(command ${CMAKE_COMMAND} -E copy_directory ${TZDATA_DIR} ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${TZDATA}
                        COMMAND_ERROR_IS_FATAL ANY)
