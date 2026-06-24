#!/usr/bin/env bash
set -euo pipefail

# Số lượng hugepage 2 MB.
HUGEPAGE_COUNT="${1:-512}"

echo "Allocating ${HUGEPAGE_COUNT} hugepages..."

echo "$HUGEPAGE_COUNT" |
    sudo tee /proc/sys/vm/nr_hugepages >/dev/null

# Ubuntu thường đã có /dev/hugepages.
sudo mkdir -p /dev/hugepages

if ! mountpoint -q /dev/hugepages; then
    sudo mount -t hugetlbfs nodev /dev/hugepages
fi

echo
grep -E 'HugePages_Total|HugePages_Free|Hugepagesize' /proc/meminfo
echo
findmnt /dev/hugepages
