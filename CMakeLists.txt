cmake_minimum_required(VERSION 3.16)
project(slurm-tracer C CXX)

set(CMAKE_C_FLAGS_RELEASE   "-O2 -DNDEBUG")
set(CMAKE_CXX_FLAGS_RELEASE "-O2 -DNDEBUG")

set(CMAKE_C_FLAGS_DEBUG   "-O0 -g")
set(CMAKE_CXX_FLAGS_DEBUG "-O0 -g")

# RelWithDebInfo = profile build: optimized with frame pointers for flamegraphs
set(CMAKE_C_FLAGS_RELWITHDEBINFO   "-O2 -g -fno-omit-frame-pointer")
set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "-O2 -g -fno-omit-frame-pointer")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
include(BpfProgram)

# libbpf comes from the distro (libbpf-dev): it must match the bpftool used for
# skeleton generation. It is the project's only external dependency.
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBBPF REQUIRED IMPORTED_TARGET libbpf>=1.0)

find_package(Threads REQUIRED)

add_bpf_program(proc_lifecycle
    SOURCE src/plugins/probes/proc_lifecycle/proc_lifecycle.bpf.c
    INCLUDES src
)

# Everything that does not touch libbpf lives here, so the tests can exercise
# the resolver and the record model without loading BPF programs or needing
# CAP_BPF.
add_library(st_core STATIC
    src/core/attribution.cpp
    src/core/record.cpp
    src/core/stdout_json_sink.cpp
)

# src/ is the one include root, and nothing below it. Every include is
# layer-qualified ("core/record.h"), so the layering stays visible at the point
# of use; adding src/core here would let a bare "record.h" resolve from
# anywhere and the structure would stop meaning anything.
target_include_directories(st_core PUBLIC src)
target_link_libraries(st_core PUBLIC Threads::Threads)

add_executable(slurm-tracer src/main.cpp)
target_link_libraries(slurm-tracer
    PRIVATE
        st_core
        bpf::proc_lifecycle
        PkgConfig::LIBBPF
)

option(SLURM_TRACER_BUILD_TESTS "Build the test suite" ON)
if(SLURM_TRACER_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
