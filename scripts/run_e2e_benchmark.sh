#!/usr/bin/env bash
set -euo pipefail

hugepage_mb="${HUGEPAGE_MB:-1024}"
mount_point="${HUGEPAGE_MOUNT:-/mnt/huge}"
pcap_file="${PCAP_FILE:-generated/spi_benchmark.pcap}"
pcap_packets="${PCAP_PACKETS:-200000}"
pcap_flows="${PCAP_FLOWS:-100000}"
mq_dispatchers="${MQ_DISPATCHERS:-2}"
mq_workers="${MQ_WORKERS:-4}"
mq_packets="${MQ_PACKETS:-$pcap_packets}"
mq_flows="${MQ_FLOWS:-$pcap_flows}"
mq_pcap_prefix="${MQ_PCAP_PREFIX:-generated/spi_benchmark_mq}"
mq_regenerate="${MQ_REGENERATE:-0}"
output="${E2E_BENCH_OUTPUT:-reports/e2e_benchmark_results.csv}"

mkdir -p reports

if ((mq_dispatchers <= 0 || mq_workers <= 0 || mq_packets <= 0 || mq_flows <= 0)); then
    echo "MQ_DISPATCHERS, MQ_WORKERS, MQ_PACKETS and MQ_FLOWS must be positive" >&2
    exit 1
fi

if ! scripts/setup_hugepages_if_needed.sh \
    --check-only --mb "$hugepage_mb" --mount-point "$mount_point"; then
    cat >&2 <<EOF
Hugepages are not ready.
Prepare them first, for example:

  sudo scripts/setup_hugepages_if_needed.sh --mb $hugepage_mb --mount-point $mount_point

Then rerun:

  make benchmark-e2e
EOF
    exit 1
fi

if [[ ! -f "$pcap_file" ]]; then
    python3 scripts/generate_spi_pcap.py \
        --output "$pcap_file" \
        --flows "$pcap_flows" \
        --packets "$pcap_packets"
fi

printf 'profile,mode,workers,max_workers,rx_queues,dispatchers,total_packets,flows,processed,dropped,active_flows,seconds,pps\n' > "$output"

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
    local rx_queues="$5"
    local dispatchers="$6"
    local packets="$7"
    local flows="$8"
    shift 8
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
    printf '%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
        "$profile" "$mode" "$workers" "$max_workers" \
        "$rx_queues" "$dispatchers" "$packets" "$flows" \
        "$processed" "$dropped" "$active_flows" "$seconds" "$pps" >> "$output"
}

generate_mq_pcaps() {
    local vdev="net_pcap0"
    local base_packets=$((mq_packets / mq_dispatchers))
    local packet_remainder=$((mq_packets % mq_dispatchers))
    local base_flows=$((mq_flows / mq_dispatchers))
    local flow_remainder=$((mq_flows % mq_dispatchers))
    local flow_offset=0

    for ((queue = 0; queue < mq_dispatchers; queue++)); do
        local packets_this="$base_packets"
        local flows_this="$base_flows"
        local shard="${mq_pcap_prefix}_q${queue}.pcap"

        if [[ "$queue" -eq $((mq_dispatchers - 1)) ]]; then
            packets_this=$((packets_this + packet_remainder))
            flows_this=$((flows_this + flow_remainder))
        fi
        if [[ "$mq_regenerate" == "1" || ! -f "$shard" ]]; then
            python3 scripts/generate_spi_pcap.py \
                --output "$shard" \
                --flows "$flows_this" \
                --packets "$packets_this" \
                --seed "$((queue + 1))" \
                --flow-offset "$flow_offset" >/dev/null
        fi
        vdev="${vdev},rx_pcap=${shard}"
        flow_offset=$((flow_offset + flows_this))
    done
    printf '%s\n' "$vdev"
}

append_result synthetic-fixed-huge synthetic 1 1 0 0 200000 20000 \
    env XDG_RUNTIME_DIR=/tmp ./build/flowtable \
        -l 0-1 --huge-dir "$mount_point" --in-memory --no-pci --no-telemetry -- \
        --mode synthetic --workers 1 --max-workers 1 \
        --packets 200000 --flows 20000 --flow-capacity 32768 --ring-size 2048 \
        --fixed-workers

append_result synthetic-scale-huge synthetic 1 4 0 0 400000 100000 \
    env XDG_RUNTIME_DIR=/tmp ./build/flowtable \
        -l 0-4 --huge-dir "$mount_point" --in-memory --no-pci --no-telemetry -- \
        --mode synthetic --workers 1 --max-workers 4 \
        --packets 400000 --flows 100000 --flow-capacity 131072 --ring-size 4096 \
        --scale-interval 100000

append_result pcap-spi-4w-huge ethdev 4 4 1 1 "$pcap_packets" "$pcap_flows" \
    env XDG_RUNTIME_DIR=/tmp ./build/flowtable \
        -l 0-4 --huge-dir "$mount_point" --in-memory --no-pci \
        --vdev "net_pcap0,rx_pcap=$pcap_file" --no-telemetry -- \
        --mode ethdev --port 0 --workers 4 --max-workers 4 \
        --packets "$pcap_packets" --flow-capacity 131072 --ring-size 4096 \
        --scale-interval 0 --fixed-workers

mq_vdev="$(generate_mq_pcaps)"
mq_lcore_end=$((mq_workers + mq_dispatchers))
append_result "pcap-spi-mq-${mq_dispatchers}d${mq_workers}w-huge" \
    ethdev "$mq_workers" "$mq_workers" "$mq_dispatchers" "$mq_dispatchers" \
    "$mq_packets" "$mq_flows" \
    env XDG_RUNTIME_DIR=/tmp ./build/flowtable \
        -l "0-${mq_lcore_end}" --huge-dir "$mount_point" --in-memory --no-pci \
        --vdev "$mq_vdev" --no-telemetry -- \
        --mode ethdev --port 0 --workers "$mq_workers" \
        --max-workers "$mq_workers" --packets "$mq_packets" \
        --flow-capacity 131072 --ring-size 4096 --scale-interval 0 \
        --rx-queues "$mq_dispatchers" --dispatchers "$mq_dispatchers" \
        --fixed-workers

printf 'Wrote %s\n' "$output"
