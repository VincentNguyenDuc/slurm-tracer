# Plugin build support.
#
#   add_st_probe(<name> SOURCES <file>... [BPF <file.bpf.c>])
#   add_st_sink(<name>  SOURCES <file>...)
#
# Each builds one shared module the daemon dlopen()s at runtime -- nothing is
# linked into the slurm-tracer binary. They land where PluginLoader expects
# them (src/core/plugin_loader.cpp):
#
#   <build>/plugins/probes/libst_probe_<name>.so
#   <build>/plugins/sinks/libst_sink_<name>.so
#
# MODULE rather than SHARED because nothing ever link-depends on a plugin: it
# is loaded by name, from a path derived from the config, and never appears on
# anyone's link line. Each .so exports exactly one entry point
# (st_register_probe / st_register_sink, see core/registry.h), which is why
# every plugin can use the same symbol name without colliding -- they are
# dlopen'd one at a time with RTLD_LOCAL, not linked together.

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
    add_library(${target} MODULE ${ARG_SOURCES})
    target_link_libraries(${target} PRIVATE st_core)
    set_target_properties(${target} PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins/${kind}s"
    )

    if(ARG_BPF)
        # The .bpf.c includes "core/events.h", so it needs the same include root
        # as the C++ side. The skeleton bakes the compiled BPF object into the
        # generated header, so the .so carries its own bytecode and needs no
        # companion file at runtime.
        add_bpf_program(${name} SOURCE "${ARG_BPF}" INCLUDES "${ST_INCLUDE_ROOT}")
        target_link_libraries(${target} PRIVATE bpf::${name} PkgConfig::LIBBPF)
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

# Returns the plugin targets this build declared. They are not linked into
# anything -- the daemon only needs them *built*, so the caller makes them an
# order dependency of the binary and nothing more.
function(st_plugin_targets out_targets)
    get_property(names GLOBAL PROPERTY ST_PLUGIN_NAMES)
    get_property(targets GLOBAL PROPERTY ST_PLUGIN_TARGETS)

    list(LENGTH names count)
    message(STATUS "Plugins (${count}): ${names}")

    set(${out_targets} "${targets}" PARENT_SCOPE)
endfunction()
