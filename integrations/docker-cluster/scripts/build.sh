#!/bin/bash
# One-shot build of slurm-tracer, run via `docker compose run --rm builder`
# before the cluster comes up. A separate step (not part of each container's
# own entrypoint) so both workers start from an already-built binary instead
# of racing each other through cmake.
#
# Builds into build/docker rather than the debug/release presets a host
# checkout might already have under build/, so this never collides with a
# developer's own build tree.
set -euo pipefail

cd /workspace
cmake -S . -B build/docker -DCMAKE_BUILD_TYPE=Release -DSLURM_TRACER_BUILD_TESTS=OFF
cmake --build build/docker -j"$(nproc)"
echo "build.sh: built build/docker/slurm-tracer"
