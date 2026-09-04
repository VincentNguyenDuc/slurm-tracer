#!/bin/bash
set -euo pipefail
source /opt/st/scripts/common.sh

setup_munge
mkdir -p /var/spool/slurmctld /var/log/slurm
chown slurm:slurm /var/spool/slurmctld /var/log/slurm

# setpriv execs slurmctld directly in this process (no fork/wait wrapper), so
# it stays PID 1 and receives `docker compose down`'s SIGTERM itself.
exec setpriv --reuid slurm --regid slurm --init-groups slurmctld -D -vv
