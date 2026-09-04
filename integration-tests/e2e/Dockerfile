# One image for every role in the test cluster: slurmctld, the compute nodes,
# and the build of slurm-tracer that runs alongside slurmd.
#
# Debian bookworm ships Slurm 22.05, which is the floor the design targets
# (docs/DESIGN.md §4) and the first release with the cgroup/v2 hierarchy.
FROM debian:bookworm-slim

ENV LANG=C.UTF-8 LC_ALL=C.UTF-8 DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        slurm-wlm slurmctld slurmd munge \
        build-essential cmake make clang llvm \
        bpftool libbpf-dev libelf-dev zlib1g-dev pkg-config \
        procps iproute2 jq ca-certificates \
    && rm -rf /var/lib/apt/lists/*

# A fixed munge key so every container in the cluster agrees without a shared
# volume or a provisioning step. This is a throwaway test cluster on a private
# compose network; never reuse this pattern for anything real.
RUN mkdir -p /etc/munge \
    && head -c 1024 /dev/zero | tr '\0' 'x' > /etc/munge/munge.key \
    && chown munge:munge /etc/munge/munge.key \
    && chmod 400 /etc/munge/munge.key

# A non-root submitter, so records carry a real uid and the uid -> user lookup
# is actually exercised rather than always resolving to root.
RUN useradd -m -u 2000 -s /bin/bash alice

RUN mkdir -p /var/spool/slurmctld /var/spool/slurmd /var/log/slurm \
    && chown slurm:slurm /var/spool/slurmctld /var/log/slurm

# The build context is the repository root, so the tracer's sources and the
# cluster's configuration come from one place and cannot drift apart.
COPY integration-tests/e2e/slurm.conf integration-tests/e2e/cgroup.conf /etc/slurm/
COPY integration-tests/e2e/entrypoint.sh /usr/local/bin/entrypoint.sh
RUN chmod +x /usr/local/bin/entrypoint.sh

COPY src/ /build/src/
COPY include/ /build/include/
COPY bpf/ /build/bpf/
COPY cmake/ /build/cmake/
COPY tests/ /build/tests/
COPY CMakeLists.txt /build/

# The BPF object needs a vmlinux.h. The build host's /sys/kernel/btf/vmlinux is
# not readable during docker build, so the object is compiled at container
# start instead; see entrypoint.sh.

ENTRYPOINT ["/usr/local/bin/entrypoint.sh"]
