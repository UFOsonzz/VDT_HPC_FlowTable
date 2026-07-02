#!/usr/bin/env bash
set -euo pipefail

PORT_ID="${1:-0}"
PACKETS="${2:-100}"
DEVICE="${3:-}"

EAL_ARGS=(
  -l 0-1
  --huge-dir=/dev/hugepages
  --no-telemetry
)

if [[ -n "${DEVICE}" ]]; then
  if [[ "${DEVICE}" =~ ^([0-9a-fA-F]{4}:)?[0-9a-fA-F]{2}:[0-9a-fA-F]{2}\.[0-7]$ ]]; then
    EAL_ARGS+=(-a "${DEVICE}")
  else
    EAL_ARGS+=(--no-pci --vdev "net_af_packet0,iface=${DEVICE}")
  fi
fi

sudo ./build/rx_demo \
  "${EAL_ARGS[@]}" \
  -- \
  --port "${PORT_ID}" \
  --packets "${PACKETS}" \
  --burst 32 \
  --promisc
