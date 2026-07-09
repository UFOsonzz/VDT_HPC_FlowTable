#!/usr/bin/env bash
set -euo pipefail

hugepage_mb="${HUGEPAGE_MB:-1024}"
mount_point="${HUGEPAGE_MOUNT:-/mnt/huge}"
pcap_file="${PCAP_FILE:-generated/spi_benchmark.pcap}"
pcap_packets="${PCAP_PACKETS:-1000000}"
pcap_flows="${PCAP_FLOWS:-100000}"
pcap_regenerate="${PCAP_REGENERATE:-}"
mq_dispatchers="${MQ_DISPATCHERS:-2}"
mq_workers="${MQ_WORKERS:-4}"
mq_packets="${MQ_PACKETS:-$pcap_packets}"
mq_flows="${MQ_FLOWS:-$pcap_flows}"
mq_pcap_prefix="${MQ_PCAP_PREFIX:-generated/spi_benchmark_mq}"
mq_regenerate="${MQ_REGENERATE:-}"
warmup_runs="${E2E_WARMUP_RUNS:-2}"
measure_runs="${E2E_RUNS:-5}"
cooldown_seconds="${E2E_COOLDOWN_SECONDS:-0}"
output="${E2E_BENCH_OUTPUT:-reports/e2e_benchmark_results.csv}"

mkdir -p reports

if [[ -z "$pcap_regenerate" ]]; then
    if [[ -v PCAP_FILE ]]; then
        pcap_regenerate=0
    else
        pcap_regenerate=1
    fi
fi
if [[ -z "$mq_regenerate" ]]; then
    if [[ -v MQ_PCAP_PREFIX ]]; then
        mq_regenerate=0
    else
        mq_regenerate=1
    fi
fi

is_uint() {
    [[ "$1" =~ ^[0-9]+$ ]]
}

if ! is_uint "$pcap_packets" || ! is_uint "$pcap_flows" ||
    ! is_uint "$mq_dispatchers" || ! is_uint "$mq_workers" ||
    ! is_uint "$mq_packets" || ! is_uint "$mq_flows" ||
    ! is_uint "$warmup_runs" || ! is_uint "$measure_runs" ||
    ! is_uint "$cooldown_seconds"; then
    echo "Benchmark count settings must be unsigned integers" >&2
    exit 1
fi

if ((pcap_packets <= 0 || pcap_flows <= 0 ||
     mq_dispatchers <= 0 || mq_workers <= 0 ||
     mq_packets <= 0 || mq_flows <= 0 ||
     measure_runs <= 0)); then
    echo "Packet, flow, worker, dispatcher and measured run counts must be positive" >&2
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

if [[ "$pcap_regenerate" == "1" || ! -f "$pcap_file" ]]; then
    python3 scripts/generate_spi_pcap.py \
        --output "$pcap_file" \
        --flows "$pcap_flows" \
        --packets "$pcap_packets"
fi

printf 'profile,mode,workers,max_workers,rx_queues,dispatchers,warmup_runs,measure_runs,median_run,total_packets,flows,processed,dropped,active_flows,seconds,pps\n' > "$output"

extract_field() {
    local line="$1"
    local key="$2"

    printf '%s\n' "$line" | tr ' ' '\n' | awk -F= -v key="$key" '$1 == key {print $2}'
}

run_profile_once() {
    local profile="$1"
    local phase="$2"
    local run="$3"
    local total="$4"
    shift 4
    local summary

    printf '[%s] %s %s/%s\n' "$profile" "$phase" "$run" "$total" >&2
    summary="$("$@" | awk '/^summary / {line=$0} END {print line}')"
    if [[ -z "$summary" ]]; then
        echo "Missing summary output for profile=$profile phase=$phase run=$run" >&2
        exit 1
    fi
    printf '%s\n' "$summary"
}

maybe_cooldown() {
    if ((cooldown_seconds > 0)); then
        sleep "$cooldown_seconds"
    fi
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
    local median
    local median_run
    local median_rank=$(((measure_runs + 1) / 2))
    local rows=()

    for ((run = 1; run <= warmup_runs; run++)); do
        run_profile_once "$profile" warmup "$run" "$warmup_runs" "$@" >/dev/null
        maybe_cooldown
    done
    for ((run = 1; run <= measure_runs; run++)); do
        summary="$(run_profile_once "$profile" measure "$run" "$measure_runs" "$@")"
        processed="$(extract_field "$summary" processed)"
        dropped="$(extract_field "$summary" dropped)"
        active_flows="$(extract_field "$summary" active_flows)"
        seconds="$(extract_field "$summary" seconds)"
        pps="$(extract_field "$summary" pps)"
        if [[ -z "$processed" || -z "$dropped" || -z "$active_flows" ||
              -z "$seconds" || -z "$pps" ]]; then
            echo "Cannot parse summary for profile=$profile run=$run: $summary" >&2
            exit 1
        fi
        rows+=("$pps,$run,$processed,$dropped,$active_flows,$seconds")
        printf '[%s] run=%s pps=%s seconds=%s processed=%s dropped=%s\n' \
            "$profile" "$run" "$pps" "$seconds" "$processed" "$dropped" >&2
        maybe_cooldown
    done
    median="$(printf '%s\n' "${rows[@]}" |
        sort -t, -k1,1n |
        awk -F, -v rank="$median_rank" 'NR == rank {print}')"
    IFS=, read -r pps median_run processed dropped active_flows seconds <<< "$median"
    printf '%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
        "$profile" "$mode" "$workers" "$max_workers" \
        "$rx_queues" "$dispatchers" "$warmup_runs" "$measure_runs" \
        "$median_run" "$packets" "$flows" \
        "$processed" "$dropped" "$active_flows" "$seconds" "$pps" >> "$output"
    printf '[%s] median_run=%s median_pps=%s\n' \
        "$profile" "$median_run" "$pps" >&2
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

append_result synthetic-fixed-huge synthetic 1 1 0 0 1000000 20000 \
    env XDG_RUNTIME_DIR=/tmp ./build/flowtable \
        -l 0-1 --huge-dir "$mount_point" --in-memory --no-pci --no-telemetry -- \
        --mode synthetic --workers 1 --max-workers 1 \
        --packets 1000000 --flows 20000 --flow-capacity 32768 --ring-size 2048 \
        --fixed-workers

append_result synthetic-scale-huge synthetic 1 4 0 0 1000000 100000 \
    env XDG_RUNTIME_DIR=/tmp ./build/flowtable \
        -l 0-4 --huge-dir "$mount_point" --in-memory --no-pci --no-telemetry -- \
        --mode synthetic --workers 1 --max-workers 4 \
        --packets 1000000 --flows 100000 --flow-capacity 131072 --ring-size 4096 \
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
