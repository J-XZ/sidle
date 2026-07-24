#!/usr/bin/env bash
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
force=0
dry_run=0
for arg in "$@"; do
  case "$arg" in --force) force=1;; --dry-run) dry_run=1;; *) echo "usage: $0 [--force] [--dry-run]" >&2; exit 2;; esac
done
image="$root/image/root.img"
if [[ -s "$image" && $force -eq 0 ]]; then echo "D-SIDLE image already exists: $image"; exit 0; fi
cmd=(mkosi -C "$root/image" build)
if ((dry_run)); then printf 'DRY_RUN '; printf '%q ' "${cmd[@]}"; printf '\n'; exit 0; fi
command -v mkosi >/dev/null || { echo "mkosi is required" >&2; exit 1; }
mkdir -p "$root/image"
"${cmd[@]}"
[[ -s "$image" ]] || { echo "mkosi did not produce $image" >&2; exit 1; }
