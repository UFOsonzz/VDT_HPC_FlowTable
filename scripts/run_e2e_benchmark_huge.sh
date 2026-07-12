#!/usr/bin/env bash
set -euo pipefail

export E2E_BENCH_OUTPUT="${E2E_BENCH_OUTPUT:-reports/e2e_benchmark_huge_results.csv}"
exec scripts/run_e2e_benchmark.sh "$@"
