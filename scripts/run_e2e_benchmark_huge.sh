#!/usr/bin/env bash
set -euo pipefail

hugepage_mb="${HUGEPAGE_MB:-1024}"
mount_point="${HUGEPAGE_MOUNT:-/mnt/huge}"
pcap_file="${PCAP_FILE:-generated/spi_benchmark.pcap}"
pcap_packets="${PCAP_PACKETS:-200000}"
output="reports/e2e_benchmark_huge_results.csv"

mkdir -p reports

if ! scripts/setup_hugepages_if_needed.sh \
    --check-only --mb "$hugepage_mb" --mount-point "$mount_point"; then
    cat >&2 <<EOF
Hugepages are not ready.
Prepare them first, for example:

  sudo scripts/setup_hugepages_if_needed.sh --mb $hugepage_mb --mount-point $mount_point

Then rerun:

  scripts/run_e2e_benchmark_huge.sh
EOF
    exit 1
fi

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

append_result synthetic-fixed-huge synthetic 1 1 200000 20000 \
    env XDG_RUNTIME_DIR=/tmp ./build/flowtable \
        -l 0-1 --no-pci --no-telemetry -- \
        --mode synthetic --workers 1 --max-workers 1 \
        --packets 200000 --flows 20000 --flow-capacity 32768 --ring-size 2048

append_result synthetic-scale-huge synthetic 1 4 400000 100000 \
    env XDG_RUNTIME_DIR=/tmp ./build/flowtable \
        -l 0-4 --no-pci --no-telemetry -- \
        --mode synthetic --workers 1 --max-workers 4 \
        --packets 400000 --flows 100000 --flow-capacity 131072 --ring-size 4096 \
        --scale-interval 100000

if [[ -f "$pcap_file" ]]; then
    append_result pcap-spi-huge ethdev 1 4 "$pcap_packets" 0 \
        env XDG_RUNTIME_DIR=/tmp ./build/flowtable \
            -l 0-4 --vdev "net_pcap0,rx_pcap=$pcap_file" --no-telemetry -- \
            --mode ethdev --port 0 --workers 1 --max-workers 4 \
            --packets "$pcap_packets" --flow-capacity 131072 --ring-size 4096 \
            --scale-interval 0
else
    echo "skip pcap-spi-huge: $pcap_file does not exist" >&2
    echo "generate one with: python3 scripts/generate_spi_pcap.py --output $pcap_file" >&2
fi

printf 'Wrote %s\n' "$output"
