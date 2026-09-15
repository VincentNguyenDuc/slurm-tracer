# Sourced by scenarios/*.sh; not meant to be run directly.
cd "$(dirname "${BASH_SOURCE[0]}")/.."

NODES=(c1 c2)

# record_scenario <name> <job_id> <raw_output>
#
# Snapshots this scenario's job metadata plus the *current* contents of every
# node's stdout_json output and the collector's http-received output into
# $OUT_DIR/scenarios/<name>/, so each scenario's data is self-contained and
# can be analyzed on its own afterwards without cross-referencing which
# records belong to which job. live/<node>/<node>.jsonl and
# live/collector/received.jsonl are the raw, cumulative sources for the whole
# run (see docker-compose.yml's bind mounts) -- run.sh resets them once up
# front, not per scenario, so a later scenario's snapshot also carries every
# earlier scenario's records -- harmless, since job_id still tells them apart.
# OUT_DIR is set by run.sh to out/<branch>_<commit>_<timestamp>; it falls back
# to plain "out" so a scenario run standalone still lands somewhere.
record_scenario() {
    local name="$1" job_id="$2" raw_output="$3"
    local dir="${OUT_DIR:-out}/scenarios/$name"
    mkdir -p "$dir"

    jq -n --argjson job_id "$job_id" --arg raw_output "$raw_output" \
        '{job_id: $job_id, raw_output: $raw_output}' > "$dir/job.json"

    for node in "${NODES[@]}"; do
        cp "live/${node}/${node}.jsonl" "$dir/${node}.jsonl"
    done
    cp live/collector/received.jsonl "$dir/collector.jsonl"
}
