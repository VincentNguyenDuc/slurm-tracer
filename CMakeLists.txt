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
include(StPlugin)

# The single include root. Every include below src/ is layer-qualified against
# this: "core/record.h", "plugins/probes/<name>/...".
set(ST_INCLUDE_ROOT "${CMAKE_SOURCE_DIR}/src")

# libbpf comes from the distro (libbpf-dev): it must match the bpftool used for
# skeleton generation. It is the project's only external dependency.
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBBPF REQUIRED IMPORTED_TARGET libbpf>=1.0)

find_package(Threads REQUIRED)

add_subdirectory(src)

option(SLURM_TRACER_BUILD_TESTS "Build the test suite" ON)
if(SLURM_TRACER_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
