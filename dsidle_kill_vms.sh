#!/usr/bin/env bash
set -euo pipefail
config=experiment_config.jsonc
execute=0
dry_run=0
while (($#)); do
  case "$1" in --config) config=$2; shift 2;; --execute) execute=1; shift;; --dry-run) dry_run=1; shift;; *) echo "usage: $0 [--config PATH] [--dry-run] --execute" >&2; exit 2;; esac
done
storage=$(python3 - "$config" <<'PY'
import json,re,sys
s=re.sub(r'//.*','',open(sys.argv[1]).read()); print(json.loads(s)['vm']['storage_path'])
PY
)
for pidfile in "$storage"/vm_*/qemu.pid; do
  [[ -f "$pidfile" ]] || continue
  pid=$(<"$pidfile")
  [[ "$pid" =~ ^[0-9]+$ ]] || { echo "invalid pid file: $pidfile" >&2; exit 1; }
  if ((dry_run || !execute)); then echo "DRY_RUN kill $pid ($pidfile)"; else kill "$pid"; fi
done
