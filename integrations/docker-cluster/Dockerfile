# One image for every role (controller, worker, one-shot builder) in the
# integration cluster. The source tree is bind-mounted at runtime rather than
# copied in: the BPF build needs the *running* kernel's BTF
# (/sys/kernel/btf/vmlinux), which a `docker build` step does not have access
# to, only a running container does. See scripts/build.sh.
FROM debian:bookworm-slim

ENV DEBIAN_FRONTEND=noninteractive \
    LANG=C.UTF-8 \
    LC_ALL=C.UTF-8

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        build-essential \
        cmake \
        ninja-build \
        clang \
        llvm \
        bpftool \
        libbpf-dev \
        libelf-dev \
        zlib1g-dev \
        pkg-config \
        slurmd \
        slurmctld \
        slurm-client \
        munge \
        procps \
        psmisc \
        jq \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /workspace
