#!/usr/bin/env bash
set -euo pipefail

hugepage_mb="${HUGEPAGE_MB:-1024}"
mount_point="${HUGEPAGE_MOUNT:-/mnt/huge}"

mkdir -p reports
output="reports/benchmark_results.csv"

if ! scripts/setup_hugepages_if_needed.sh \
    --check-only --mb "$hugepage_mb" --mount-point "$mount_point"; then
    cat >&2 <<EOF
Hugepages are not ready.
Prepare them first, for example:

  sudo scripts/setup_hugepages_if_needed.sh --mb $hugepage_mb --mount-point $mount_point

Then rerun:

  make benchmark
EOF
    exit 1
fi

printf 'profile,workers,total_packets,flows_per_worker,seconds,pps,checksum\n' > "$output"

for profile in hot-cache high-cardinality; do
    if [[ "$profile" == "hot-cache" ]]; then
        packets=5000000
        flows=4096
    else
        packets=2000000
        flows=32768
    fi
    for workers in 1 2 4; do
        last_core="$workers"
        result="$(
            XDG_RUNTIME_DIR=/tmp ./build/flowtable_benchmark \
                -l "0-${last_core}" --huge-dir "$mount_point" \
                --in-memory --no-pci --no-telemetry --log-level=eal,warning \
                -- --workers "$workers" --packets "$packets" --flows "$flows" \
                | tail -n 1
        )"
        printf '%s,%s\n' "$profile" "$result" >> "$output"
    done
done

printf 'Wrote %s\n' "$output"
