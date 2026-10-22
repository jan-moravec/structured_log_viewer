cmake_minimum_required(VERSION 3.28)

# `slp` links loglib only. Qt stays inside `app/`, which the root
# skips entirely when SLV_BUILD_GUI is OFF.
configure_file(include/version.hpp.in ${CMAKE_CURRENT_BINARY_DIR}/generated/version.hpp @ONLY)

add_library(
    slp_core
    STATIC
    src/options.cpp
    src/diagnostics.cpp
    src/platform.cpp
    src/commands.cpp
    src/command_formats.cpp
    src/command_query.cpp
)

target_include_directories(slp_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_BINARY_DIR}/generated)

target_link_libraries(slp_core PUBLIC loglib PRIVATE argparse project_warnings)

add_executable(slp src/main.cpp)
target_link_libraries(slp PRIVATE slp_core project_warnings)

include(GNUInstallDirs)
install(TARGETS slp RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# On Windows the TBB runtime DLL is loaded from the directory next to
# the executable. Mirror the GUI's POST_BUILD copy so a GUI-off build
# can still launch slp.exe. tests.exe copies the same DLL; copy_if_different
# keeps those steps from fighting over identical bytes.
if(WIN32)
    add_custom_command(
        TARGET slp
        POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:TBB::tbb> $<TARGET_FILE_DIR:slp>
        COMMENT "Copying TBB runtime DLL next to slp.exe"
        VERBATIM
    )
    install(FILES $<TARGET_FILE:TBB::tbb> DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

if(WIN32 AND MSVC)
    set(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS_SKIP TRUE)
    set(CMAKE_INSTALL_DEBUG_LIBRARIES FALSE)
    include(InstallRequiredSystemLibraries)
    if(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS)
        message(STATUS "MSVC runtime DLLs (staged next to slp): ${CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS}")
        add_custom_command(
            TARGET slp
            POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS} $<TARGET_FILE_DIR:slp>
            COMMENT "Copying MSVC runtime DLLs next to slp.exe"
            VERBATIM
        )
        install(PROGRAMS ${CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS} DESTINATION ${CMAKE_INSTALL_BINDIR})
    else()
        message(
            WARNING
            "InstallRequiredSystemLibraries resolved no MSVC runtime DLLs; slp.exe will require the system VC++ redist."
        )
    endif()
endif()

if(WIN32 AND LOGLIB_NETWORK_TLS)
    get_filename_component(OPENSSL_BIN_HINT "${OPENSSL_INCLUDE_DIR}/../bin" ABSOLUTE)
    find_file(
        OPENSSL_DLL_LIBSSL
        NAMES libssl-3-x64.dll libssl-3.dll libssl.dll
        PATHS "${OPENSSL_BIN_HINT}"
        NO_DEFAULT_PATH
    )
    find_file(
        OPENSSL_DLL_LIBCRYPTO
        NAMES libcrypto-3-x64.dll libcrypto-3.dll libcrypto.dll
        PATHS "${OPENSSL_BIN_HINT}"
        NO_DEFAULT_PATH
    )
    if(OPENSSL_DLL_LIBSSL AND OPENSSL_DLL_LIBCRYPTO)
        message(STATUS "OpenSSL runtime DLLs for slp: ${OPENSSL_DLL_LIBSSL}, ${OPENSSL_DLL_LIBCRYPTO}")
        add_custom_command(
            TARGET slp
            POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${OPENSSL_DLL_LIBSSL}" $<TARGET_FILE_DIR:slp>
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${OPENSSL_DLL_LIBCRYPTO}" $<TARGET_FILE_DIR:slp>
            COMMENT "Copying OpenSSL runtime DLLs next to slp.exe"
            VERBATIM
        )
        install(FILES "${OPENSSL_DLL_LIBSSL}" "${OPENSSL_DLL_LIBCRYPTO}" DESTINATION ${CMAKE_INSTALL_BINDIR})
    else()
        message(
            WARNING
            "LOGLIB_NETWORK_TLS=ON but OpenSSL runtime DLLs not found under ${OPENSSL_BIN_HINT}; "
            "slp.exe will fail at startup with STATUS_DLL_NOT_FOUND if it loads OpenSSL."
        )
    endif()
endif()

bundle_third_party_licenses(slp)
