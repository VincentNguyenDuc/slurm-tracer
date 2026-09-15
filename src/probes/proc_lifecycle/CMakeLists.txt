add_st_probe(proc_lifecycle
    SOURCES probe.cpp translate.cpp
    BPF     proc_lifecycle.bpf.c
)
