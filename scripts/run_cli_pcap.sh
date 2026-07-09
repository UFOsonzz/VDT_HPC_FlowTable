#!/usr/bin/env bash
set -euo pipefail

hugepage_mb="${HUGEPAGE_MB:-1024}"
mount_point="${HUGEPAGE_MOUNT:-/mnt/huge}"
pcap_file="${1:-${PCAP_FILE:-generated/spi_benchmark.pcap}}"
pcap_packets="${PCAP_PACKETS:-200000}"
pcap_flows="${PCAP_FLOWS:-100000}"
workers="${WORKERS:-4}"
packets="${PACKETS:-0}"
stats_interval="${STATS_INTERVAL:-1}"
lcore_end="${LCORE_END:-$workers}"
binary="${FLOWTABLE_BIN:-./build/flowtable}"

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

printf 'Opening FlowTable CLI for %s\n' "$pcap_file"
printf 'Commands: show dashboard, show statistics, show worker, show worker N, show traffic, quit\n'

env XDG_RUNTIME_DIR=/tmp "$binary" \
    -l "0-${lcore_end}" --huge-dir "$mount_point" --in-memory --no-pci \
    --vdev "net_pcap0,rx_pcap=$pcap_file" --no-telemetry -- \
    --mode ethdev --port 0 --workers "$workers" --max-workers "$workers" \
    --packets "$packets" --cli --stats-interval "$stats_interval" \
    --fixed-workers
