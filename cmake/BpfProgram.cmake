# BPF CO-RE build support.
#
# Compiles *.bpf.c into BPF objects and generates libbpf skeleton headers.
# Exposes add_bpf_program(), which creates an INTERFACE target carrying the
# include path of the generated <name>.skel.h.

find_program(BPF_CLANG clang REQUIRED)
find_program(BPF_BPFTOOL bpftool REQUIRED)

# Map the host machine to the __TARGET_ARCH_* macro the libbpf headers expect.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64")
    set(BPF_TARGET_ARCH "x86")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
    set(BPF_TARGET_ARCH "arm64")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "ppc64le")
    set(BPF_TARGET_ARCH "powerpc")
else()
    message(FATAL_ERROR "Unsupported BPF target arch: ${CMAKE_SYSTEM_PROCESSOR}")
endif()

# BPF ISA version. v3 needs kernel >= 5.1; lower it for older compute nodes.
set(BPF_CPU "v3" CACHE STRING "BPF ISA version passed to clang -mcpu")

set(BPF_GEN_DIR "${CMAKE_BINARY_DIR}/bpf")
file(MAKE_DIRECTORY "${BPF_GEN_DIR}")

# vmlinux.h is dumped from the running kernel's BTF by default. Point this at a
# vendored header when building for a kernel other than the build host's.
set(BPF_VMLINUX_BTF "/sys/kernel/btf/vmlinux" CACHE FILEPATH "BTF source for vmlinux.h")
set(BPF_VMLINUX_H "${BPF_GEN_DIR}/vmlinux.h")

if(NOT EXISTS "${BPF_VMLINUX_BTF}")
    message(FATAL_ERROR
        "No kernel BTF at ${BPF_VMLINUX_BTF}. Build a kernel with CONFIG_DEBUG_INFO_BTF=y "
        "or set -DBPF_VMLINUX_BTF=/path/to/btf")
endif()

add_custom_command(
    OUTPUT "${BPF_VMLINUX_H}"
    COMMAND "${BPF_BPFTOOL}" btf dump file "${BPF_VMLINUX_BTF}" format c > "${BPF_VMLINUX_H}"
    DEPENDS "${BPF_VMLINUX_BTF}"
    COMMENT "Generating vmlinux.h from ${BPF_VMLINUX_BTF}"
    VERBATIM
)
add_custom_target(bpf_vmlinux DEPENDS "${BPF_VMLINUX_H}")

# add_bpf_program(<name> SOURCE <file.bpf.c> [INCLUDES <dir>...])
#
# Produces <name>.bpf.o and <name>.skel.h, and defines the INTERFACE target
# bpf::<name> so userspace code can #include "<name>.skel.h".
function(add_bpf_program name)
    cmake_parse_arguments(ARG "" "SOURCE" "INCLUDES" ${ARGN})
    if(NOT ARG_SOURCE)
        message(FATAL_ERROR "add_bpf_program(${name}): SOURCE is required")
    endif()

    get_filename_component(src "${ARG_SOURCE}" ABSOLUTE)
    set(obj "${BPF_GEN_DIR}/${name}.bpf.o")
    set(skel "${BPF_GEN_DIR}/${name}.skel.h")

    set(include_flags "-I${BPF_GEN_DIR}")
    foreach(dir IN LISTS ARG_INCLUDES)
        get_filename_component(abs "${dir}" ABSOLUTE)
        list(APPEND include_flags "-I${abs}")
    endforeach()

    add_custom_command(
        OUTPUT "${obj}"
        COMMAND "${BPF_CLANG}"
                -g -O2 -Wall -Werror
                -target bpf
                -D__TARGET_ARCH_${BPF_TARGET_ARCH}
                -mcpu=${BPF_CPU}
                ${include_flags}
                -c "${src}" -o "${obj}"
        DEPENDS "${src}" "${BPF_VMLINUX_H}"
        COMMENT "Building BPF object ${name}.bpf.o"
        VERBATIM
    )

    add_custom_command(
        OUTPUT "${skel}"
        COMMAND "${BPF_BPFTOOL}" gen skeleton "${obj}" name ${name} > "${skel}"
        DEPENDS "${obj}"
        COMMENT "Generating BPF skeleton ${name}.skel.h"
        VERBATIM
    )

    add_custom_target(${name}_bpf DEPENDS "${skel}")

    add_library(bpf_${name} INTERFACE)
    target_include_directories(bpf_${name} INTERFACE "${BPF_GEN_DIR}")
    add_dependencies(bpf_${name} ${name}_bpf)
    add_library(bpf::${name} ALIAS bpf_${name})
endfunction()
