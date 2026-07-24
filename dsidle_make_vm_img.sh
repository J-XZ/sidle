#!/usr/bin/env bash
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
force=0
dry_run=0
config="${DSIDLE_EXPERIMENT_CONFIG_JSONC:-$root/experiment_config.jsonc}"
while (($#)); do
  case "$1" in
    --force) force=1; shift;; --dry-run) dry_run=1; shift;;
    --config) (($# >= 2)) || { echo "usage: $0 [--config PATH] [--force] [--dry-run]" >&2; exit 2; }; config=$2; shift 2;;
    --help) echo "usage: $0 [--config PATH] [--force] [--dry-run]" >&2; exit 0;;
    *) echo "usage: $0 [--config PATH] [--force] [--dry-run]" >&2; exit 2;;
  esac
done
image="$root/image/root.img"
if [[ -s "$image" && $force -eq 0 ]]; then echo "D-SIDLE image already exists: $image"; exit 0; fi
[[ -f "$config" ]] || { echo "missing config: $config" >&2; exit 2; }
cmd=(mkosi -C "$root/image")
((force)) && cmd+=(--force)
cmd+=(build)
if ((dry_run)); then printf 'DRY_RUN '; printf '%q ' "${cmd[@]}"; printf '\n'; exit 0; fi
command -v mkosi >/dev/null || { echo "mkosi is required" >&2; exit 1; }
mkdir -p "$root/image"
key_path="$root/image/rootfs/root/.ssh/authorized_keys"
python3 - "$config" "$key_path" <<'PY'
import json,re,sys
from pathlib import Path
text=re.sub(r'//[^\n]*', '', Path(sys.argv[1]).read_text())
key=json.loads(text)['vm']['local_ssh_pub_key'].strip()
target=Path(sys.argv[2])
if key:
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(key+'\n')
PY
"${cmd[@]}"
[[ -s "$image" ]] || { echo "mkosi did not produce $image" >&2; exit 1; }
