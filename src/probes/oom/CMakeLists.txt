add_st_probe(oom
    SOURCES probe.cpp translate.cpp
    BPF     oom.bpf.c
)
