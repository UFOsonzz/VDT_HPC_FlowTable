#!/usr/bin/env bash
set -euo pipefail

required_mb=1024
required_pages=""
mount_point="/mnt/huge"
check_only=0
dry_run=0

usage() {
    cat <<'EOF'
Usage: scripts/setup_hugepages_if_needed.sh [options]

Options:
  --mb N             Required hugepage memory in MB (default: 1024)
  --pages N          Required number of hugepages; overrides --mb
  --mount-point DIR  hugetlbfs mount point (default: /mnt/huge)
  --check-only       Only verify hugepages and hugetlbfs mount
  --dry-run          Print actions without changing the system
  -h, --help         Show this help

Examples:
  scripts/setup_hugepages_if_needed.sh --check-only --mb 1024
  sudo scripts/setup_hugepages_if_needed.sh --mb 2048 --mount-point /mnt/huge
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
    --mb)
        required_mb="$2"
        shift 2
        ;;
    --pages)
        required_pages="$2"
        shift 2
        ;;
    --mount-point)
        mount_point="$2"
        shift 2
        ;;
    --check-only)
        check_only=1
        shift
        ;;
    --dry-run)
        dry_run=1
        shift
        ;;
    -h|--help)
        usage
        exit 0
        ;;
    *)
        echo "unknown option: $1" >&2
        usage >&2
        exit 2
        ;;
    esac
done

read_meminfo_value() {
    local key="$1"

    awk -v key="$key" '$1 == key ":" {print $2}' /proc/meminfo
}

is_hugetlbfs_mounted() {
    local target="$1"

    awk -v target="$target" '$2 == target && $3 == "hugetlbfs" {found=1}
        END {exit found ? 0 : 1}' /proc/mounts
}

run_or_print() {
    if [[ "$dry_run" -eq 1 ]]; then
        printf 'dry-run:'
        printf ' %q' "$@"
        printf '\n'
    else
        "$@"
    fi
}

page_kb="$(read_meminfo_value Hugepagesize)"
current_total="$(read_meminfo_value HugePages_Total)"
current_free="$(read_meminfo_value HugePages_Free)"

if [[ -z "$page_kb" || -z "$current_total" || -z "$current_free" ]]; then
    echo "cannot read hugepage information from /proc/meminfo" >&2
    exit 1
fi

if [[ -z "$required_pages" ]]; then
    required_pages=$(( (required_mb * 1024 + page_kb - 1) / page_kb ))
fi

printf 'hugepage_size_kb=%s current_total=%s current_free=%s required_pages=%s mount_point=%s\n' \
    "$page_kb" "$current_total" "$current_free" "$required_pages" "$mount_point"

pages_ok=0
mount_ok=0
if (( current_total >= required_pages )); then
    pages_ok=1
fi
if is_hugetlbfs_mounted "$mount_point"; then
    mount_ok=1
fi

if [[ "$pages_ok" -eq 1 && "$mount_ok" -eq 1 ]]; then
    echo "hugepages already ready"
    exit 0
fi

if [[ "$check_only" -eq 1 ]]; then
    if [[ "$pages_ok" -ne 1 ]]; then
        echo "not enough hugepages: current_total=$current_total required=$required_pages" >&2
    fi
    if [[ "$mount_ok" -ne 1 ]]; then
        echo "hugetlbfs is not mounted at $mount_point" >&2
    fi
    exit 1
fi

if [[ "${EUID:-$(id -u)}" -ne 0 ]]; then
    echo "setup requires root; rerun with sudo" >&2
    exit 1
fi

nr_hugepages_path="/sys/kernel/mm/hugepages/hugepages-${page_kb}kB/nr_hugepages"
if [[ "$pages_ok" -ne 1 ]]; then
    if [[ ! -w "$nr_hugepages_path" ]]; then
        echo "cannot write $nr_hugepages_path" >&2
        exit 1
    fi
    echo "setting hugepages to $required_pages"
    if [[ "$dry_run" -eq 1 ]]; then
        echo "dry-run: write $required_pages to $nr_hugepages_path"
    else
        printf '%s\n' "$required_pages" > "$nr_hugepages_path"
    fi
fi

if [[ "$mount_ok" -ne 1 ]]; then
    run_or_print mkdir -p "$mount_point"
    run_or_print mount -t hugetlbfs nodev "$mount_point"
fi

echo "hugepage setup complete"
