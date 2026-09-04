# Plugin build support.
#
#   add_st_probe(<name> SOURCES <file>... [BPF <file.bpf.c>])
#   add_st_sink(<name>  SOURCES <file>...)
#
# Each builds one static library and records the plugin's name. After every
# plugin has been declared, st_generate_plugin_manifest() writes
# registry_manifest.cpp, which calls register_<name>(Registries&) once per
# plugin.
#
# Why a generated manifest rather than a REGISTER_PROBE static initialiser: the
# manifest names register_<name> as a real undefined symbol, so the linker is
# obliged to pull the plugin's translation unit in. A static initialiser inside a
# static library gets dropped silently -- no compile error, no link error, the
# plugin simply never appears at runtime. (--whole-archive would also fix that,
# but it needs CMake 3.24 and this project targets 3.16.)

# Build a subset of the plugins, e.g. -DST_PLUGINS="proc_lifecycle;stdout_json".
# Empty means everything the tree declares.
set(ST_PLUGINS "" CACHE STRING "Plugins to build; empty = all")

# True when <name> should be built under the current ST_PLUGINS setting.
function(_st_plugin_enabled name out_var)
    if(NOT ST_PLUGINS)
        set(${out_var} TRUE PARENT_SCOPE)
        return()
    endif()
    if("${name}" IN_LIST ST_PLUGINS)
        set(${out_var} TRUE PARENT_SCOPE)
    else()
        set(${out_var} FALSE PARENT_SCOPE)
    endif()
endfunction()

function(_st_add_plugin kind name)
    cmake_parse_arguments(ARG "" "BPF" "SOURCES" ${ARGN})
    if(NOT ARG_SOURCES)
        message(FATAL_ERROR "add_st_${kind}(${name}): SOURCES is required")
    endif()

    _st_plugin_enabled("${name}" enabled)
    if(NOT enabled)
        message(STATUS "Plugin ${name}: disabled by ST_PLUGINS")
        return()
    endif()

    set(target "st_${kind}_${name}")
    add_library(${target} STATIC ${ARG_SOURCES})
    target_link_libraries(${target} PUBLIC st_core)

    if(ARG_BPF)
        # The .bpf.c includes "core/events.h", so it needs the same include root
        # as the C++ side.
        add_bpf_program(${name} SOURCE "${ARG_BPF}" INCLUDES "${ST_INCLUDE_ROOT}")
        target_link_libraries(${target} PUBLIC bpf::${name} PkgConfig::LIBBPF)
    endif()

    set_property(GLOBAL APPEND PROPERTY ST_PLUGIN_NAMES "${name}")
    set_property(GLOBAL APPEND PROPERTY ST_PLUGIN_TARGETS "${target}")
endfunction()

function(add_st_probe name)
    _st_add_plugin(probe "${name}" ${ARGN})
endfunction()

function(add_st_sink name)
    _st_add_plugin(sink "${name}" ${ARGN})
endfunction()

# Writes registry_manifest.cpp and returns its path plus the plugin targets to
# link. Call once, after every plugin directory has been added.
function(st_generate_plugin_manifest out_source out_targets)
    get_property(names GLOBAL PROPERTY ST_PLUGIN_NAMES)
    get_property(targets GLOBAL PROPERTY ST_PLUGIN_TARGETS)

    set(ST_MANIFEST_DECLS "")
    set(ST_MANIFEST_CALLS "")
    foreach(name IN LISTS names)
        string(APPEND ST_MANIFEST_DECLS "void register_${name}(Registries&);\n")
        string(APPEND ST_MANIFEST_CALLS "    register_${name}(r);\n")
    endforeach()

    set(generated "${CMAKE_BINARY_DIR}/registry_manifest.cpp")
    # configure_file only rewrites when the content changes, so an unchanged
    # plugin set does not trigger a rebuild.
    configure_file("${CMAKE_SOURCE_DIR}/cmake/registry_manifest.cpp.in" "${generated}" @ONLY)

    list(LENGTH names count)
    message(STATUS "Plugins (${count}): ${names}")

    set(${out_source} "${generated}" PARENT_SCOPE)
    set(${out_targets} "${targets}" PARENT_SCOPE)
endfunction()
