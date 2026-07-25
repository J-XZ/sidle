#!/usr/bin/env bash
# D-SIDLE VM init wrapper. Implementation is cxlkv-aligned (see scripts/dsidle_init_vm.py):
# host tuning, shared-memory tmpfs mpol=bind, bridge/tap, dual-NIC QEMU,
# guest-built ivpci driver, BAR2 verification, pool --init-pool, taskset.
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
config="${DSIDLE_EXPERIMENT_CONFIG_JSONC:-$script_dir/experiment_config.jsonc}"
image="$script_dir/image/root.img"
pool_tool="$script_dir/build/dsidle_shared_pool"
dry_run=0
execute=0
allow_overlapping_numa=0
no_host_tuning=0
skip_guestmount=0

usage() {
  echo "usage: $0 [--config PATH] [--image PATH] [--pool-tool PATH] [--dry-run] [--allow-overlapping-numa] [--no-host-tuning] --execute" >&2
}
while (($#)); do
  case "$1" in
    --config|--image|--pool-tool)
      (($# >= 2)) || { usage; exit 2; }
      case "$1" in --config) config=$2;; --image) image=$2;; --pool-tool) pool_tool=$2;; esac
      shift 2;;
    --dry-run) dry_run=1; shift;;
    --execute) execute=1; shift;;
    --allow-overlapping-numa) allow_overlapping_numa=1; shift;;
    --no-host-tuning) no_host_tuning=1; shift;;
    --skip-guestmount) skip_guestmount=1; shift;;
    --apply-host-tuning)
      # Compatibility: cxlkv always tunes; this flag is now a no-op (tuning is default).
      shift;;
    --help) usage; exit 0;;
    *) usage; exit 2;;
  esac
done
((dry_run || execute)) || { echo "refusing VM/backing changes without --execute (use --dry-run)" >&2; exit 2; }
[[ -f "$config" ]] || { echo "missing config: $config" >&2; exit 2; }
[[ -f "$script_dir/scripts/dsidle_init_vm.py" ]] || { echo "missing scripts/dsidle_init_vm.py" >&2; exit 2; }

cmd=(python3 "$script_dir/scripts/dsidle_init_vm.py"
  --config "$config"
  --image "$image"
  --pool-tool "$pool_tool"
  --repo-root "$script_dir")
((dry_run)) && cmd+=(--dry-run)
((execute)) && cmd+=(--execute)
((allow_overlapping_numa)) && cmd+=(--allow-overlapping-numa)
((no_host_tuning)) && cmd+=(--no-host-tuning)
((skip_guestmount)) && cmd+=(--skip-guestmount)
exec "${cmd[@]}"
