#!/usr/bin/env bash
# Starts one role of the test cluster. Roles: ctld, node.
set -euo pipefail

ROLE="${1:?usage: entrypoint.sh <ctld|node>}"

mkdir -p /run/munge
chown munge:munge /run/munge
runuser -u munge -- /usr/sbin/munged --force
# munged forks; give it a moment to create its socket before anything
# authenticates against it.
for _ in $(seq 1 50); do
    [ -S /run/munge/munge.socket.2 ] && break
    sleep 0.1
done

case "$ROLE" in
ctld)
    exec /usr/sbin/slurmctld -D
    ;;

node)
    # Delegate cgroup controllers to Slurm.
    #
    # cgroup v2 forbids a cgroup from holding member processes *and* enabling
    # controllers for its children. The container's processes start in the
    # container's own root cgroup, so `echo +memory > cgroup.subtree_control`
    # fails there with EBUSY, and slurmd then dies with:
    #     error: memory cgroup controller is not available.
    #     fatal: Unable to initialize jobacct_gather
    # Moving everything into a leaf cgroup first is what makes the root
    # delegable. This is the standard dance for running a cgroup manager
    # inside a container.
    if [ ! -w /sys/fs/cgroup/cgroup.subtree_control ]; then
        echo "cgroup2 root is not writable - is this container privileged?" >&2
        exit 1
    fi

    mkdir -p /sys/fs/cgroup/init
    # Snapshot the pid list: moving a process rewrites the file being read.
    for pid in $(cat /sys/fs/cgroup/cgroup.procs); do
        echo "$pid" > /sys/fs/cgroup/init/cgroup.procs 2>/dev/null || true
    done
    echo "+cpuset +cpu +memory" > /sys/fs/cgroup/cgroup.subtree_control

    # slurmstepd creates its scope under system.slice but does not create the
    # slice itself, and there is no systemd in here to have done it already.
    mkdir -p /sys/fs/cgroup/system.slice
    echo "+cpuset +cpu +memory" > /sys/fs/cgroup/system.slice/cgroup.subtree_control
    echo "cgroup controllers delegated: $(cat /sys/fs/cgroup/system.slice/cgroup.subtree_control)"

    # slurm-tracer is built here rather than during `docker build` because the
    # BPF object needs the running kernel's BTF, and /sys/kernel/btf/vmlinux is
    # not available to a build container.
    if [ ! -x /build/build/slurm-tracer ]; then
        echo "building slurm-tracer against this kernel's BTF..."
        cmake -S /build -B /build/build -DCMAKE_BUILD_TYPE=Release >/dev/null
        cmake --build /build/build -j"$(nproc)" >/dev/null
        echo "build complete"
    fi

    # Wait for the controller before registering, or slurmd logs a burst of
    # connection failures on every cluster start.
    for _ in $(seq 1 60); do
        if scontrol ping >/dev/null 2>&1; then break; fi
        sleep 1
    done

    exec /usr/sbin/slurmd -D -N "$(hostname -s)"
    ;;

*)
    echo "unknown role: $ROLE" >&2
    exit 1
    ;;
esac
