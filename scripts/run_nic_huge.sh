#!/usr/bin/env bash
set -euo pipefail

PORT_ID="${1:-0}"
PACKETS="${2:-100}"

sudo ./build/rx_demo \
  -l 0-1 \
  --huge-dir=/dev/hugepages \
  --no-telemetry \
  -- \
  --port "${PORT_ID}" \
  --packets "${PACKETS}" \
  --burst 32 \
  --promisc