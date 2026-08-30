# Installs the loglib static library, supported headers, bundled robin-map
# headers, and a CMake package that exports `loglib::loglib`. Private
# compiled archives are staged next to the package and attached with
# `$<LINK_ONLY:>` so vendor types stay out of the public compile interface.
# Header-only private deps (mio, glaze, asio, date) are compiled into
# `loglib` and are not installed.

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include(FetchContent)

function(loglib_resolve_target out name)
    set(_t "${name}")
    foreach(_unused RANGE 16)
        if(NOT TARGET "${_t}")
            message(FATAL_ERROR "loglib install: missing target '${_t}' (from '${name}')")
        endif()
        get_target_property(_aliased "${_t}" ALIASED_TARGET)
        if(_aliased)
            set(_t "${_aliased}")
        else()
            break()
        endif()
    endforeach()
    set(${out} "${_t}" PARENT_SCOPE)
endfunction()

# Records a PRIVATE compiled dependency so its archive/DLL is installed and
# linked into consumers without usage requirements (includes, defs).
function(loglib_add_private_runtime_dep name)
    loglib_resolve_target(_real "${name}")
    get_target_property(_type "${_real}" TYPE)
    if(_type STREQUAL "INTERFACE_LIBRARY")
        return()
    endif()
    if(NOT _type STREQUAL "STATIC_LIBRARY" AND NOT _type STREQUAL "SHARED_LIBRARY")
        message(FATAL_ERROR "loglib install: '${_real}' has unsupported type '${_type}'")
    endif()
    set_property(GLOBAL APPEND PROPERTY LOGLIB_PRIVATE_RUNTIME_DEPS "${_real}|${_type}")
    target_link_libraries(loglib INTERFACE "$<INSTALL_INTERFACE:$<LINK_ONLY:loglib::_priv_${_real}>>")
endfunction()

function(loglib_install_library)
    if(WIN32)
        target_link_libraries(loglib PRIVATE ws2_32 mswsock)
    endif()
    if(UNIX)
        find_package(Threads REQUIRED)
        target_link_libraries(loglib INTERFACE "$<INSTALL_INTERFACE:Threads::Threads>")
    endif()

    foreach(
        _dep
        IN
        ITEMS
            simdjson::simdjson
            date::date-tz
            fmt::fmt
            TBB::tbb
            efsw
            PCRE2::pcre2-8
            ZLIB::ZLIB
            BZip2::BZip2
            LibLZMA::LibLZMA
            zstd::libzstd
    )
        loglib_add_private_runtime_dep("${_dep}")
    endforeach()

    if(LOGLIB_NETWORK_TLS)
        target_link_libraries(
            loglib
            INTERFACE
                "$<INSTALL_INTERFACE:$<LINK_ONLY:OpenSSL::SSL>>"
                "$<INSTALL_INTERFACE:$<LINK_ONLY:OpenSSL::Crypto>>"
        )
    endif()

    get_property(_priv_specs GLOBAL PROPERTY LOGLIB_PRIVATE_RUNTIME_DEPS)

    set(_imported_cmake "")
    string(APPEND _imported_cmake "# Generated. Imported private archives for loglib::loglib.\n")
    string(APPEND _imported_cmake "# Usage requirements are omitted; consumers link archives only.\n\n")

    foreach(_spec IN LISTS _priv_specs)
        string(REPLACE "|" ";" _parts "${_spec}")
        list(GET _parts 0 _real)
        list(GET _parts 1 _type)

        if(_type STREQUAL "STATIC_LIBRARY")
            install(FILES $<TARGET_FILE:${_real}> DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT loglib EXCLUDE_FROM_ALL)
            string(
                APPEND _imported_cmake
                "if(NOT TARGET loglib::_priv_${_real})\n"
                "    add_library(loglib::_priv_${_real} STATIC IMPORTED)\n"
                "    set_target_properties(loglib::_priv_${_real} PROPERTIES\n"
                "        IMPORTED_LOCATION \"\${PACKAGE_PREFIX_DIR}/${CMAKE_INSTALL_LIBDIR}/$<TARGET_FILE_NAME:${_real}>\"\n"
                "    )\n"
                "endif()\n\n"
            )
        elseif(_type STREQUAL "SHARED_LIBRARY")
            if(WIN32)
                install(
                    FILES $<TARGET_FILE:${_real}>
                    DESTINATION ${CMAKE_INSTALL_BINDIR}
                    COMPONENT loglib
                    EXCLUDE_FROM_ALL
                )
                install(
                    FILES $<TARGET_LINKER_FILE:${_real}>
                    DESTINATION ${CMAKE_INSTALL_LIBDIR}
                    COMPONENT loglib
                    EXCLUDE_FROM_ALL
                )
                string(
                    APPEND _imported_cmake
                    "if(NOT TARGET loglib::_priv_${_real})\n"
                    "    add_library(loglib::_priv_${_real} SHARED IMPORTED)\n"
                    "    set_target_properties(loglib::_priv_${_real} PROPERTIES\n"
                    "        IMPORTED_LOCATION \"\${PACKAGE_PREFIX_DIR}/${CMAKE_INSTALL_BINDIR}/$<TARGET_FILE_NAME:${_real}>\"\n"
                    "        IMPORTED_IMPLIB \"\${PACKAGE_PREFIX_DIR}/${CMAKE_INSTALL_LIBDIR}/$<TARGET_LINKER_FILE_NAME:${_real}>\"\n"
                    "    )\n"
                    "endif()\n\n"
                )
            else()
                install(
                    FILES $<TARGET_FILE:${_real}>
                    DESTINATION ${CMAKE_INSTALL_LIBDIR}
                    COMPONENT loglib
                    EXCLUDE_FROM_ALL
                )
                install(
                    FILES $<TARGET_SONAME_FILE:${_real}>
                    DESTINATION ${CMAKE_INSTALL_LIBDIR}
                    COMPONENT loglib
                    EXCLUDE_FROM_ALL
                )
                string(
                    APPEND _imported_cmake
                    "if(NOT TARGET loglib::_priv_${_real})\n"
                    "    add_library(loglib::_priv_${_real} SHARED IMPORTED)\n"
                    "    set_target_properties(loglib::_priv_${_real} PROPERTIES\n"
                    "        IMPORTED_LOCATION \"\${PACKAGE_PREFIX_DIR}/${CMAKE_INSTALL_LIBDIR}/$<TARGET_FILE_NAME:${_real}>\"\n"
                    "    )\n"
                    "endif()\n\n"
                )
            endif()
        endif()
    endforeach()

    file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/loglibPrivateImported.cmake" CONTENT "${_imported_cmake}")

    install(
        TARGETS loglib
        EXPORT loglibTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT loglib EXCLUDE_FROM_ALL
        FILE_SET HEADERS DESTINATION ${CMAKE_INSTALL_INCLUDEDIR} COMPONENT loglib EXCLUDE_FROM_ALL
    )

    if(NOT USE_SYSTEM_ROBIN_MAP)
        FetchContent_GetProperties(robin_map)
        if(NOT robin_map_SOURCE_DIR)
            message(FATAL_ERROR "loglib install: robin_map_SOURCE_DIR is empty; cannot stage tsl/ headers")
        endif()
        install(
            DIRECTORY "${robin_map_SOURCE_DIR}/include/tsl"
            DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
            COMPONENT loglib
            EXCLUDE_FROM_ALL
        )
        if(MSVC AND EXISTS "${robin_map_SOURCE_DIR}/tsl-robin-map.natvis")
            # `robin_map` lists this as `$<INSTALL_INTERFACE:...>` INTERFACE_SOURCES.
            install(
                FILES "${robin_map_SOURCE_DIR}/tsl-robin-map.natvis"
                DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}
                COMPONENT loglib
                EXCLUDE_FROM_ALL
            )
        endif()
        install(
            TARGETS robin_map
            EXPORT loglibTargets
            COMPONENT loglib
            EXCLUDE_FROM_ALL
            INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
        )
    endif()

    install(
        EXPORT loglibTargets
        FILE loglibTargets.cmake
        NAMESPACE loglib::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/loglib
        COMPONENT loglib
        EXCLUDE_FROM_ALL
    )

    configure_package_config_file(
        "${CMAKE_SOURCE_DIR}/cmake/loglibConfig.cmake.in"
        "${CMAKE_CURRENT_BINARY_DIR}/loglibConfig.cmake"
        INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/loglib
    )
    write_basic_package_version_file(
        "${CMAKE_CURRENT_BINARY_DIR}/loglibConfigVersion.cmake"
        VERSION ${PROJECT_VERSION}
        COMPATIBILITY SameMajorVersion
    )

    install(
        FILES
            "${CMAKE_CURRENT_BINARY_DIR}/loglibConfig.cmake"
            "${CMAKE_CURRENT_BINARY_DIR}/loglibConfigVersion.cmake"
            "${CMAKE_CURRENT_BINARY_DIR}/loglibPrivateImported.cmake"
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/loglib
        COMPONENT loglib
        EXCLUDE_FROM_ALL
    )
endfunction()
