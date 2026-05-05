# BundleLicenses.cmake
#
# Generates a single `THIRD_PARTY_LICENSES.txt` aggregating every
# license file from the third-party dependencies actually shipped in
# the binary, plus the project's own `LICENSE`. The file is staged
# next to the executable in the build tree (so it ships with the
# Linux AppImage / Windows zip / macOS DMG) and copied into the
# install tree.
#
# Background: Qt (LGPLv3), TBB / simdjson / OpenSSL (Apache-2.0),
# fmt / glaze / mio / robin_map / date / argparse / efsw (MIT or
# BSD), Asio / Catch2 (BSL-1.0) all require source attribution
# in redistributed binary form. Prior to this module the project
# packages shipped no third-party license text at all -- a quiet
# compliance gap this module closes.
#
# Usage: include this file from the top-level CMakeLists.txt after
# all the FetchContent_MakeAvailable / find_package calls so the
# dependency source dirs are populated. It exposes one function:
#
#   bundle_third_party_licenses(<output-target>)
#
# `<output-target>` is the executable target the licenses ship with
# (e.g. `StructuredLogViewer`). The generated `THIRD_PARTY_LICENSES.txt`
# is added as a POST_BUILD copy step next to the binary, and the
# install rule places it alongside the install RUNTIME destination.

include(FetchContent)

# Capture the directory where THIS file lives so the function can find
# our checked-in license snippets later. `${CMAKE_CURRENT_LIST_DIR}` at
# include-time resolves to `cmake/`; resolving it inside the function
# would point at the *caller's* file directory instead.
set(_BUNDLE_LICENSES_DIR "${CMAKE_CURRENT_LIST_DIR}" CACHE INTERNAL "")

# Append "<DisplayName>|<SourceFile>" to @p outvar (PARENT_SCOPE) when
# the source file exists. Skips silently when missing so optional
# `USE_SYSTEM_*` paths don't litter the configure output.
function(_bundle_collect_entry display_name source_file outvar)
    if(NOT EXISTS "${source_file}")
        message(STATUS "BundleLicenses: skipping ${display_name} (no license at ${source_file})")
        return()
    endif()
    list(APPEND ${outvar} "${display_name}|${source_file}")
    set(${outvar} "${${outvar}}" PARENT_SCOPE)
endfunction()

# Get the FetchContent-populated source dir of @p dep_name into @p outvar
# (PARENT_SCOPE). Reads from CMake's GLOBAL property cache so the lookup
# survives the `block()` scoping used by some FetchContent_MakeAvailable
# call sites. Sets `<outvar> = ""` if the dep was not populated (e.g.
# `USE_SYSTEM_<dep>=ON`).
function(_bundle_get_source_dir dep_name outvar)
    FetchContent_GetProperties(${dep_name})
    if(${dep_name}_POPULATED)
        set(${outvar} "${${dep_name}_SOURCE_DIR}" PARENT_SCOPE)
    else()
        set(${outvar} "" PARENT_SCOPE)
    endif()
endfunction()

function(bundle_third_party_licenses target_name)
    if(NOT TARGET ${target_name})
        message(WARNING "bundle_third_party_licenses: target '${target_name}' does not exist; skipping")
        return()
    endif()

    set(LICENSE_ENTRIES "")

    # FetchContent-staged dependencies. Each dep name and license-file
    # path matches what FetchDependencies.cmake fetches; we use
    # FetchContent_GetProperties so the lookup works even for deps
    # populated inside `block()` scopes.
    set(FETCHED_DEPS
        "fmt:LICENSE:fmt (MIT)"
        "glaze:LICENSE:glaze (MIT)"
        "date:LICENSE.txt:Howard Hinnant date (MIT)"
        "simdjson:LICENSE:simdjson (Apache-2.0)"
        "tbb:LICENSE.txt:oneTBB (Apache-2.0)"
        "mio:LICENSE:mio (MIT)"
        "robin_map:LICENSE:robin_map (MIT)"
        "efsw:LICENSE:efsw (MIT)"
        "argparse:LICENSE:argparse (MIT)"
        "asio:asio/LICENSE_1_0.txt:Asio (BSL-1.0)"
    )
    foreach(spec IN LISTS FETCHED_DEPS)
        string(REPLACE ":" ";" parts "${spec}")
        list(GET parts 0 dep)
        list(GET parts 1 rel)
        list(GET parts 2 disp)
        _bundle_get_source_dir(${dep} src_dir)
        if(NOT src_dir)
            message(STATUS "BundleLicenses: ${dep} not fetched (USE_SYSTEM_*?), license aggregation skipped for it")
            continue()
        endif()
        _bundle_collect_entry("${disp}" "${src_dir}/${rel}" LICENSE_ENTRIES)
    endforeach()

    # OpenSSL is a system find_package dep so we ship a checked-in
    # snippet rather than reading from a fetched source dir. Only
    # attributed when LOGLIB_NETWORK_TLS is enabled (and therefore
    # actually linked into the binary).
    if(LOGLIB_NETWORK_TLS)
        _bundle_collect_entry(
            "OpenSSL (Apache-2.0)" "${_BUNDLE_LICENSES_DIR}/license_snippets/openssl.txt" LICENSE_ENTRIES
        )
    endif()

    # Qt is bundled at deploy time (linuxdeploy / windeployqt /
    # macdeployqt). LGPLv3 requires we ship the LGPL-3.0 text and
    # an offer-of-source pointer. Use a short snippet pointing at the
    # canonical Qt legal page.
    _bundle_collect_entry("Qt 6 (LGPL-3.0)" "${_BUNDLE_LICENSES_DIR}/license_snippets/qt.txt" LICENSE_ENTRIES)

    # Compose the aggregated file. Generation runs at configure time
    # so the file is ready before the binary's POST_BUILD copy step.
    set(GENERATED_OUT "${CMAKE_BINARY_DIR}/THIRD_PARTY_LICENSES.txt")
    file(
        WRITE "${GENERATED_OUT}"
        "Structured Log Viewer\n"
        "Third-party licenses\n"
        "====================\n\n"
        "This file aggregates the license texts of every third-party\n"
        "library distributed with this binary. Each section starts\n"
        "with a `=== <name> ===` rule of equals signs. The project\n"
        "is itself MIT-licensed (see LICENSE next to this file).\n\n"
    )

    foreach(entry IN LISTS LICENSE_ENTRIES)
        string(FIND "${entry}" "|" sep)
        string(SUBSTRING "${entry}" 0 ${sep} display_name)
        math(EXPR after "${sep} + 1")
        string(SUBSTRING "${entry}" ${after} -1 source_file)

        file(READ "${source_file}" body)
        file(APPEND "${GENERATED_OUT}" "=== ${display_name} ===\n\n")
        file(APPEND "${GENERATED_OUT}" "${body}")
        file(APPEND "${GENERATED_OUT}" "\n\n")
    endforeach()

    list(LENGTH LICENSE_ENTRIES n_entries)
    message(STATUS "BundleLicenses: aggregated ${GENERATED_OUT} from ${n_entries} dependencies")

    # Stage next to the binary so packaging (linuxdeploy / CPack /
    # zip) picks it up automatically.
    add_custom_command(
        TARGET ${target_name}
        POST_BUILD
        COMMAND
            ${CMAKE_COMMAND} -E copy_if_different "${GENERATED_OUT}"
            "$<TARGET_FILE_DIR:${target_name}>/THIRD_PARTY_LICENSES.txt"
        COMMAND
            ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_SOURCE_DIR}/LICENSE"
            "$<TARGET_FILE_DIR:${target_name}>/LICENSE"
        COMMENT "Staging THIRD_PARTY_LICENSES.txt + LICENSE next to ${target_name}"
        VERBATIM
    )

    # Install rules: place both files next to the binary in every
    # packaged artifact. macOS bundle install puts them alongside the
    # .app for now (the AppleStore-style "inside Resources" dance can
    # come later if needed).
    install(
        FILES "${GENERATED_OUT}" "${CMAKE_SOURCE_DIR}/LICENSE"
        DESTINATION "${CMAKE_INSTALL_BINDIR}"
        COMPONENT licenses
    )
    if(APPLE)
        # Mirror the Resources/tzdata pattern: a POST_BUILD step that
        # drops the licenses inside the .app bundle so a user dragging
        # the bundle out of the DMG also gets the attribution texts.
        add_custom_command(
            TARGET ${target_name}
            POST_BUILD
            COMMAND
                ${CMAKE_COMMAND} -E copy_if_different "${GENERATED_OUT}"
                "$<TARGET_BUNDLE_CONTENT_DIR:${target_name}>/Resources/THIRD_PARTY_LICENSES.txt"
            COMMAND
                ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_SOURCE_DIR}/LICENSE"
                "$<TARGET_BUNDLE_CONTENT_DIR:${target_name}>/Resources/LICENSE"
            COMMENT "Copying license bundle into macOS .app Resources"
            VERBATIM
        )
    endif()
endfunction()
