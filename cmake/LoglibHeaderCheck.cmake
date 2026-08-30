# Compiles each supported public header as the first include of an otherwise
# empty translation unit. Fails the build when a header is not self-contained.
# Completeness of types wins over dropping includes: if isolation compile
# requires a full include instead of a forward declaration, put the include
# back.

function(loglib_add_public_header_check)
    set(_dir "${CMAKE_CURRENT_BINARY_DIR}/header_check")
    file(MAKE_DIRECTORY "${_dir}")
    set(_sources)
    foreach(_header IN LISTS LOGLIB_PUBLIC_HEADERS)
        string(REGEX REPLACE "^include/" "" _include_path "${_header}")
        string(REPLACE "/" "_" _stem "${_include_path}")
        string(REPLACE "." "_" _stem "${_stem}")
        set(_in "${_dir}/${_stem}.cpp.in")
        set(_tu "${_dir}/${_stem}.cpp")
        file(WRITE "${_in}" "#include <${_include_path}>\n")
        configure_file("${_in}" "${_tu}" COPYONLY)
        list(APPEND _sources "${_tu}")
    endforeach()

    add_library(loglib_header_check OBJECT ${_sources})
    # In-tree policy target only; not installed. `loglib` supplies include
    # dirs, `NOMINMAX`, and `robin_map`. `project_warnings` stays PRIVATE.
    target_link_libraries(loglib_header_check PRIVATE loglib project_warnings)
    # Isolation TUs are never linked into a product binary; skip LTO.
    set_target_properties(
        loglib_header_check
        PROPERTIES
            INTERPROCEDURAL_OPTIMIZATION OFF
            INTERPROCEDURAL_OPTIMIZATION_RELEASE OFF
            INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO OFF
    )
    # Ninja does not build unused OBJECT libraries as part of `all`.
    add_custom_target(loglib_header_self_containment ALL)
    add_dependencies(loglib_header_self_containment loglib_header_check)
endfunction()
