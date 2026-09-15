add_st_probe(cgroup_lifecycle
    SOURCES probe.cpp translate.cpp
    BPF     cgroup_lifecycle.bpf.c
)
