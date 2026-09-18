# Core. Everything that does not touch libbpf lives here, so tests can exercise
# the resolver, the record model and the pipeline without loading BPF programs
# or needing CAP_BPF.
add_library(st_core STATIC
    attribution/attribution.cpp
    utilities/clock.cpp
    config/config.cpp
    config/config_file.cpp
    pipeline/pipeline.cpp
    plugin_loader.cpp
    record/record.cpp
)

# src/ is the one include root, and nothing below it. Every include is
# layer-qualified ("record/record.h"), so the layering stays visible at
# the point of use; adding  here would let a bare "record.h" resolve from
# anywhere and the structure would stop meaning anything.
target_include_directories(st_core PUBLIC ${ST_INCLUDE_ROOT})
# CMAKE_DL_LIBS: plugin_loader.cpp dlopen()s the probe/sink modules.
target_link_libraries(st_core
    PUBLIC Threads::Threads spdlog::spdlog nlohmann_json::nlohmann_json ${CMAKE_DL_LIBS}
)

add_subdirectory(attribution)
add_subdirectory(../probes/oom probes/oom)
add_subdirectory(../probes/proc_lifecycle probes/proc_lifecycle)
add_subdirectory(../sinks/http sinks/http)
add_subdirectory(../sinks/null sinks/null)
add_subdirectory(../sinks/stdout_json sinks/stdout_json)
st_plugin_targets(ST_PLUGIN_TARGETS)

add_executable(slurm-tracer
    main.cpp
    daemon.cpp
    event_loop.cpp
)
target_link_libraries(slurm-tracer PRIVATE st_attribution)

# Not linked, only built: the daemon dlopen()s these at runtime from
# <build>/plugins/. The dependency just means `cmake --build . --target
# slurm-tracer` still leaves a runnable tree behind.
if(ST_PLUGIN_TARGETS)
    add_dependencies(slurm-tracer ${ST_PLUGIN_TARGETS})
endif()

# Keep the binary at the top of the build tree now that it is declared in a
# subdirectory: `make run` and the docs both expect build/<preset>/slurm-tracer.
set_target_properties(slurm-tracer PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)
