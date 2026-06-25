#!/usr/bin/env bash
set -euo pipefail

PCAP_FILE="${1:-traffic.pcap}"
PACKETS="${2:-10}"

XDG_RUNTIME_DIR=/tmp ./build/rx_demo \
  -l 0-1 \
  --no-huge \
  --in-memory \
  --no-pci \
  --no-telemetry \
  --vdev "net_pcap0,rx_pcap=${PCAP_FILE}" \
  -- \
  --port 0 \
  --packets "${PACKETS}" \
  --burst 32