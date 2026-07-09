#!/usr/bin/env bash
set -euo pipefail

mkdir -p reports
output="reports/e2e_benchmark_results.csv"
printf 'profile,mode,workers,max_workers,total_packets,flows,processed,dropped,active_flows,seconds,pps\n' > "$output"

extract_field() {
    local line="$1"
    local key="$2"

    printf '%s\n' "$line" | tr ' ' '\n' | awk -F= -v key="$key" '$1 == key {print $2}'
}

append_result() {
    local profile="$1"
    local mode="$2"
    local workers="$3"
    local max_workers="$4"
    local packets="$5"
    local flows="$6"
    shift 6
    local summary
    local processed
    local dropped
    local active_flows
    local seconds
    local pps

    summary="$("$@" | awk '/^summary / {line=$0} END {print line}')"
    processed="$(extract_field "$summary" processed)"
    dropped="$(extract_field "$summary" dropped)"
    active_flows="$(extract_field "$summary" active_flows)"
    seconds="$(extract_field "$summary" seconds)"
    pps="$(extract_field "$summary" pps)"
    printf '%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
        "$profile" "$mode" "$workers" "$max_workers" "$packets" "$flows" \
        "$processed" "$dropped" "$active_flows" "$seconds" "$pps" >> "$output"
}

append_result synthetic-fixed synthetic 1 1 50000 5000 \
    env XDG_RUNTIME_DIR=/tmp ./build/flowtable \
        -l 0-1 --no-huge --in-memory --no-pci --no-telemetry -- \
        --mode synthetic --workers 1 --max-workers 1 \
        --packets 50000 --flows 5000 --flow-capacity 8192 --ring-size 1024

append_result synthetic-scale synthetic 1 2 50000 20000 \
    env XDG_RUNTIME_DIR=/tmp ./build/flowtable \
        -l 0-2 --no-huge --in-memory --no-pci --no-telemetry -- \
        --mode synthetic --workers 1 --max-workers 2 \
        --packets 50000 --flows 20000 --flow-capacity 32768 --ring-size 1024 \
        --scale-interval 10000

if [[ -f traffic_vlan.pcap ]]; then
    append_result pcap-vlan ethdev 1 1 10 0 \
        env XDG_RUNTIME_DIR=/tmp ./build/flowtable \
            -l 0-1 --no-huge --in-memory \
            --vdev net_pcap0,rx_pcap=traffic_vlan.pcap --no-telemetry -- \
            --mode ethdev --port 0 --workers 1 --max-workers 1 \
            --packets 10 --flow-capacity 1024 --ring-size 256
fi

printf 'Wrote %s\n' "$output"
