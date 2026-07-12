#!/usr/bin/env bash
set -euo pipefail

hugepage_mb="${HUGEPAGE_MB:-1024}"
mount_point="${HUGEPAGE_MOUNT:-/mnt/huge}"
pcap_file="${1:-${PCAP_FILE:-generated/spi_benchmark.pcap}}"
pcap_packets="${PCAP_PACKETS:-200000}"
pcap_flows="${PCAP_FLOWS:-100000}"
pcap_infinite_rx="${PCAP_INFINITE_RX:-1}"
pcap_rx_mbufs="${PCAP_RX_MBUFS:-$((pcap_packets + 8192))}"
workers="${WORKERS:-2}"
max_workers="${MAX_WORKERS:-4}"
packets="${PACKETS:-0}"
stats_interval="${STATS_INTERVAL:-0}"
fixed_workers="${FIXED_WORKERS:-0}"
lcore_end="${LCORE_END:-$max_workers}"
binary="${FLOWTABLE_BIN:-./build/flowtable}"
pcap_vdev="net_pcap0,rx_pcap=$pcap_file"
extra_app_args=()

if [[ ! -x "$binary" ]]; then
    make -j
fi

if ! scripts/setup_hugepages_if_needed.sh \
    --check-only --mb "$hugepage_mb" --mount-point "$mount_point"; then
    cat >&2 <<EOF
Hugepages are not ready.
Prepare them first, for example:

  sudo scripts/setup_hugepages_if_needed.sh --mb $hugepage_mb --mount-point $mount_point

Then rerun:

  scripts/run_cli_pcap.sh
EOF
    exit 1
fi

if [[ ! -f "$pcap_file" ]]; then
    python3 scripts/generate_spi_pcap.py \
        --output "$pcap_file" \
        --flows "$pcap_flows" \
        --packets "$pcap_packets"
fi

if [[ "$pcap_infinite_rx" == "1" ]]; then
    pcap_vdev="${pcap_vdev},infinite_rx=1"
fi
if [[ "$fixed_workers" == "1" ]]; then
    extra_app_args+=(--fixed-workers)
fi

printf 'Opening FlowTable CLI for %s\n' "$pcap_file"
printf 'PCAP infinite_rx=%s packets=%s flows=%s\n' \
    "$pcap_infinite_rx" "$pcap_packets" "$pcap_flows"
printf 'Active workers=%s max workers=%s fixed_workers=%s\n' \
    "$workers" "$max_workers" "$fixed_workers"
printf 'Dynamic scaling rebalances active flows when worker count changes.\n'
printf 'CLI is quiet by default; run show dashboard for realtime views.\n'
printf 'Commands: show dashboard, show statistics, show worker, show worker N, show traffic, scale up, scale down, quit\n'

env XDG_RUNTIME_DIR=/tmp "$binary" \
    -l "0-${lcore_end}" --huge-dir "$mount_point" --in-memory --no-pci \
    --vdev "$pcap_vdev" --no-telemetry -- \
    --mode ethdev --port 0 --workers "$workers" --max-workers "$max_workers" \
    --packets "$packets" --rx-mbufs "$pcap_rx_mbufs" --cli \
    --stats-interval "$stats_interval" "${extra_app_args[@]}"
