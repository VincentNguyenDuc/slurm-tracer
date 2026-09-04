# Sourced by the entrypoint scripts. Not executable on its own.

# Copies the shared secret into the image's own /etc/munge (rather than
# bind-mounting it there directly) because munged demands to chmod/chown its
# key file, and a read-only bind mount refuses both.
setup_munge() {
    mkdir -p /run/munge /var/lib/munge /var/log/munge
    cp /secrets/munge.key /etc/munge/munge.key
    chown -R munge:munge /etc/munge /run/munge /var/lib/munge /var/log/munge
    chmod 0400 /etc/munge/munge.key
    # munged daemonizes itself (no -F), so this returns once it has forked --
    # no need to background it ourselves.
    setpriv --reuid munge --regid munge --init-groups munged
    for _ in $(seq 1 50); do
        [ -S /run/munge/munge.socket.2 ] && return 0
        sleep 0.2
    done
    echo "common.sh: munged did not come up" >&2
    return 1
}

# Docker puts this container's own init process directly in the root of its
# delegated cgroup, which trips cgroup v2's "no internal process" rule: a
# controller can only be enabled in cgroup.subtree_control while the cgroup
# itself has no processes of its own, only children. Move ourselves into a
# child cgroup first so the controllers slurmd's cgroup/v2 plugin wants
# (cpuset, cpu, memory, pids) can be delegated down to system.slice, which it
# creates job/step scopes under.
setup_cgroup_delegation() {
    mkdir -p /sys/fs/cgroup/init
    echo $$ > /sys/fs/cgroup/init/cgroup.procs
    echo "+cpuset +cpu +memory +pids" > /sys/fs/cgroup/cgroup.subtree_control

    mkdir -p /sys/fs/cgroup/system.slice
    echo "+cpuset +cpu +memory +pids" > /sys/fs/cgroup/system.slice/cgroup.subtree_control
}

wait_for_binary() {
    local bin=/workspace/build/docker/slurm-tracer
    for _ in $(seq 1 150); do
        [ -x "$bin" ] && return 0
        sleep 0.2
    done
    echo "common.sh: $bin never appeared; did scripts/build.sh run first?" >&2
    return 1
}
