add_st_probe(sched_latency
    SOURCES probe.cpp
    BPF     sched_latency.bpf.c
)
