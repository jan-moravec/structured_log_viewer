cmake_minimum_required(VERSION 3.19)

project(StructuredLogViewer VERSION 0.1 LANGUAGES CXX)

find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Widgets)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Widgets)

set(PROJECT_SOURCES
    src/main.cpp
    src/main_window.cpp
    include/main_window.hpp
    src/main_window.ui

    include/log_model.hpp
    src/log_model.cpp
)

if(${QT_VERSION_MAJOR} GREATER_EQUAL 6)
    qt_add_executable(${PROJECT_NAME}
        MANUAL_FINALIZATION
        ${PROJECT_SOURCES}
        include/log_table_view.hpp src/log_table_view.cpp
        include/find_record_widget.hpp src/find_record_widget.cpp
        include/log_filter_model.hpp src/log_filter_model.cpp
        include/filter_records_widget.hpp src/filter_records_widget.cpp
    )

# Define target properties for Android with Qt 6 as:
# set_property(TARGET app APPEND PROPERTY QT_ANDROID_PACKAGE_SOURCE_DIR
# ${CMAKE_CURRENT_SOURCE_DIR}/android)
# For more information, see https://doc.qt.io/qt-6/qt-add-executable.html#target-creation
else()
    if(ANDROID)
        add_library(${PROJECT_NAME} SHARED
            ${PROJECT_SOURCES}
        )

    # Define properties for Android with Qt 5 after find_package() calls as:
    # set(ANDROID_PACKAGE_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/android")
    else()
        add_executable(${PROJECT_NAME}
            ${PROJECT_SOURCES}
        )
    endif()
endif()

target_include_directories(${PROJECT_NAME} PUBLIC ./include)

target_link_libraries(${PROJECT_NAME} PRIVATE Qt${QT_VERSION_MAJOR}::Widgets loglib)

# Qt for iOS sets MACOSX_BUNDLE_GUI_IDENTIFIER automatically since Qt 6.1.
# If you are developing for iOS or macOS you should consider setting an
# explicit, fixed bundle identifier manually though.
if(${QT_VERSION} VERSION_LESS 6.1.0)
    set(BUNDLE_ID_OPTION MACOSX_BUNDLE_GUI_IDENTIFIER com.example.${PROJECT_NAME})
endif()

set_target_properties(${PROJECT_NAME} PROPERTIES
    ${BUNDLE_ID_OPTION}
    MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}
    MACOSX_BUNDLE TRUE
    WIN32_EXECUTABLE TRUE
)

include(GNUInstallDirs)
install(TARGETS ${PROJECT_NAME}
    BUNDLE DESTINATION .
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

if(QT_VERSION_MAJOR EQUAL 6)
    qt_finalize_executable(${PROJECT_NAME})
endif()

# Define a custom command to copy the timezone data directory
add_custom_command(
    OUTPUT ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${TZDATA}
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${TZDATA_DIR} ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${TZDATA}
    DEPENDS ${TZDATA_DIR}
    COMMENT "Copying timezone data directory"
)

# Add a custom target that uses the custom command
add_custom_target(copy_tzdata ALL DEPENDS ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${TZDATA})

# Ensure the custom target runs before the executable is built
add_dependencies(${PROJECT_NAME} copy_tzdata)

# Ensure the clean_tzdata target is run during the clean process
set_property(DIRECTORY APPEND PROPERTY ADDITIONAL_CLEAN_FILES ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${TZDATA})
