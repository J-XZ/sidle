#!/usr/bin/env bash
# D-SIDLE wrapper for the repository-local image/make_vm_img.sh flow.
# Build image/root.img when the canonical image is missing.
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
target_img="$root/image/root.img"
force=0
dry_run=0

while (($#)); do
  case "$1" in
    --force) force=1; shift ;;
    --dry-run) dry_run=1; shift ;;
    --config)
      # Accepted for API compatibility with other dsidle_* scripts; image build
      # injects SSH keys from the host ~/.ssh, not from experiment config.
      (($# >= 2)) || { echo "usage: $0 [--config PATH] [--force] [--dry-run]" >&2; exit 2; }
      shift 2
      ;;
    --help)
      echo "usage: $0 [--config PATH] [--force] [--dry-run]"
      echo "Build image/root.img via the repository-local mkosi flow."
      echo "Skips when image/root.img already exists unless --force is set."
      exit 0
      ;;
    *)
      echo "usage: $0 [--config PATH] [--force] [--dry-run]" >&2
      exit 2
      ;;
  esac
done

mkdir -p "$root/image"

if ((dry_run)); then
  printf 'DRY_RUN '
  printf '%q ' bash "$root/image/make_vm_img.sh"
  printf '\n'
  exit 0
fi

if ((force == 0)) && [[ -f "$target_img" ]]; then
  img_size=$(stat -c %s "$target_img" 2>/dev/null || echo 0)
  if [[ -n "$img_size" && "$img_size" -gt 0 ]]; then
    echo "[make_vm_img] $target_img already exists ($img_size bytes); skip build."
    exit 0
  fi
  echo "[make_vm_img] WARN: $target_img exists but is empty; rebuilding." >&2
fi

[[ -x "$root/image/make_vm_img.sh" ]] || {
  echo "missing executable: $root/image/make_vm_img.sh" >&2
  exit 1
}

(
  cd "$root/image"
  ./make_vm_img.sh
)

[[ -s "$target_img" ]] || {
  echo "make_vm_img.sh did not produce $target_img" >&2
  exit 1
}
