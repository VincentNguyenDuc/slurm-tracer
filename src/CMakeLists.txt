# Core. Everything that does not touch libbpf lives here, so tests can exercise
# the resolver, the record model and the pipeline without loading BPF programs
# or needing CAP_BPF.
add_library(st_core STATIC
    core/attribution.cpp
    core/clock.cpp
    core/config.cpp
    core/pipeline.cpp
    core/record.cpp
)

# src/ is the one include root, and nothing below it. Every include is
# layer-qualified ("core/record.h"), so the layering stays visible at the point
# of use; adding core/ here would let a bare "record.h" resolve from anywhere
# and the structure would stop meaning anything.
target_include_directories(st_core PUBLIC ${ST_INCLUDE_ROOT})
target_link_libraries(st_core PUBLIC Threads::Threads)

# Plugins declare themselves via add_st_probe()/add_st_sink(); the manifest that
# registers them is generated from that set.
add_subdirectory(plugins)
st_generate_plugin_manifest(ST_MANIFEST_SOURCE ST_PLUGIN_TARGETS)

add_executable(slurm-tracer
    main.cpp
    daemon.cpp
    event_loop.cpp
    ${ST_MANIFEST_SOURCE}
)
target_link_libraries(slurm-tracer
    PRIVATE
        ${ST_PLUGIN_TARGETS}
)

# Keep the binary at the top of the build tree now that it is declared in a
# subdirectory: `make run` and the docs both expect build/<preset>/slurm-tracer.
set_target_properties(slurm-tracer PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)
