# The cgroup watcher: attribution's own event feed, not a plugin. Every other
# probe's records depend on it, so it is built and linked unconditionally --
# it is not named in ST_PLUGINS and cannot be disabled by config the way an
# actual probe can. See core/attribution.h and cgroup_watcher.h.
add_library(st_attribution STATIC
    cgroup_watcher.cpp
    translate.cpp
)
target_link_libraries(st_attribution PUBLIC st_core)

add_bpf_program(cgroup_watcher SOURCE cgroup_watcher.bpf.c INCLUDES "${ST_INCLUDE_ROOT}")
target_link_libraries(st_attribution PUBLIC bpf::cgroup_watcher PkgConfig::LIBBPF)
