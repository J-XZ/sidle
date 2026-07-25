#!/usr/bin/env bash
# D-SIDLE VM clear aligned with cxlkv init_scripts_env_99_clear_vm_data.fish:
# kill QEMU/ivshmem → wipe vm storage contents → remove ivshmem artifacts → umount shared tmpfs.
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
config="${DSIDLE_EXPERIMENT_CONFIG_JSONC:-$script_dir/experiment_config.jsonc}"
dry_run=0
execute=0

usage() {
  echo "usage: $0 [--config PATH] [--dry-run] --execute" >&2
}

while (($#)); do
  case "$1" in
    --config)
      (($# >= 2)) || { usage; exit 2; }
      config=$2
      shift 2
      ;;
    --dry-run) dry_run=1; shift;;
    --execute) execute=1; shift;;
    --help) usage; exit 0;;
    *) usage; exit 2;;
  esac
done

((dry_run || execute)) || {
  echo "refusing VM/storage changes without --execute (use --dry-run)" >&2
  exit 2
}
[[ -f "$config" ]] || { echo "missing config: $config" >&2; exit 2; }

# Resolve storage + shared paths from experiment_config.jsonc (cxlkv args.fish equivalent).
eval "$(python3 - "$config" <<'PY'
import json, re, sys
from pathlib import Path

def load_jsonc(path: Path) -> dict:
    text = path.read_text()
    out, in_string, escaped, i = [], False, False, 0
    while i < len(text):
        char = text[i]
        nxt = text[i + 1] if i + 1 < len(text) else ""
        if in_string:
            out.append(char)
            if escaped:
                escaped = False
            elif char == "\\":
                escaped = True
            elif char == '"':
                in_string = False
            i += 1
        elif char == '"':
            in_string = True
            out.append(char)
            i += 1
        elif char == "/" and nxt == "/":
            i = text.find("\n", i)
            if i < 0:
                break
        else:
            out.append(char)
            i += 1
    return json.loads("".join(out))

cfg = load_jsonc(Path(sys.argv[1]))
storage = str(cfg["vm"]["storage_path"]).strip()
shared = str(cfg["shared_memory"]["path"]).strip()
if not storage:
    raise SystemExit("vm.storage_path is empty")
print(f"storage={storage!r}")
print(f"shared={shared!r}")
PY
)"

is_dangerous_path() {
  case "$1" in
    /|/root|/home|/mnt|/usr|/var|/etc|/tmp|.) return 0;;
    *) return 1;;
  esac
}

kill_pids_safely() {
  local pid
  for pid in "$@"; do
    [[ "$pid" =~ ^[0-9]+$ ]] || continue
    [[ "$pid" == "$$" ]] && continue
    if ((dry_run)); then
      echo "DRY_RUN kill -9 $pid"
    else
      kill -9 "$pid" 2>/dev/null || true
    fi
  done
}

storage_real="$(realpath -- "$storage" 2>/dev/null || true)"
if [[ -z "$storage_real" ]]; then
  echo "ERROR: VM storage path does not exist or cannot be resolved: $storage" >&2
  exit 1
fi
if is_dangerous_path "$storage_real"; then
  echo "ERROR: Refuse to clear dangerous path: $storage_real" >&2
  exit 1
fi

# 1) Prefer precise kills from qemu pidfiles.
shopt -s nullglob
for pid_file in "$storage_real"/vm_*/qemu.pid; do
  [[ -f "$pid_file" ]] || continue
  pid="$(tr -d '[:space:]' <"$pid_file" || true)"
  kill_pids_safely "$pid"
done

# 2) Fallback: cmdline feature match (cxlkv uses pgrep -f).
mapfile -t pids < <(pgrep -f -- 'qemu-system-x86_64' 2>/dev/null || true)
kill_pids_safely "${pids[@]:-}"
mapfile -t pids < <(pgrep -f -- 'ivshmem-server' 2>/dev/null || true)
kill_pids_safely "${pids[@]:-}"

if ((dry_run)); then
  echo "DRY_RUN sleep 1"
else
  sleep 1
fi

mapfile -t pids < <(pgrep -f -- 'qemu-system-x86_64' 2>/dev/null || true)
kill_pids_safely "${pids[@]:-}"
mapfile -t pids < <(pgrep -f -- 'ivshmem-server' 2>/dev/null || true)
kill_pids_safely "${pids[@]:-}"

# 3) Refuse to wipe while processes still live.
if ((dry_run)); then
  echo "DRY_RUN refuse-clear gate if qemu/ivshmem-server still running"
else
  if pgrep -f -- 'qemu-system-x86_64' >/dev/null 2>&1; then
    echo "ERROR: qemu-system-x86_64 is still running, refuse to clear data." >&2
    exit 1
  fi
  if pgrep -f -- 'ivshmem-server' >/dev/null 2>&1; then
    echo "ERROR: ivshmem-server is still running, refuse to clear data." >&2
    exit 1
  fi
fi

echo "Clearing VM storage safely: $storage_real"
if ((dry_run)); then
  echo "DRY_RUN find $storage_real -mindepth 1 -maxdepth 1 -exec rm -rf -- {} +"
else
  find "$storage_real" -mindepth 1 -maxdepth 1 -exec rm -rf -- {} +
fi

# Shared-memory cleanup: remove known ivshmem artifacts; umount tmpfs; keep the directory.
if [[ -n "${shared:-}" ]]; then
  shared_real="$(realpath -- "$shared" 2>/dev/null || true)"
  if [[ -z "$shared_real" ]]; then
    echo "WARN: SHARED_MEM_PATH cannot be resolved, skip: $shared"
  elif is_dangerous_path "$shared_real"; then
    echo "WARN: Refuse to clear dangerous SHARED_MEM_PATH: $shared_real"
  else
    echo "Clearing shared-memory artifacts in: $shared_real"
    if ((dry_run)); then
      echo "DRY_RUN rm -f -- $shared_real/ivshmem_shared_mem"
      echo "DRY_RUN find $shared_real -maxdepth 1 -type s -name 'ivshmem*' -delete"
      # D-SIDLE legacy backing name (pre-cxlkv-aligned init).
      echo "DRY_RUN rm -f -- $shared_real/dsidle_shared.pool"
    else
      rm -f -- "$shared_real/ivshmem_shared_mem" 2>/dev/null || true
      rm -f -- "$shared_real/dsidle_shared.pool" 2>/dev/null || true
      find "$shared_real" -maxdepth 1 -type s -name 'ivshmem*' -delete 2>/dev/null || true
    fi

    if ((dry_run)); then
      if mountpoint -q -- "$shared_real"; then
        echo "DRY_RUN sudo umount -- $shared_real"
      else
        echo "SHARED_MEM_PATH is not mounted, skip umount: $shared_real"
      fi
    else
      if mountpoint -q -- "$shared_real"; then
        echo "Unmounting shared-memory path: $shared_real"
        if ! sudo umount -- "$shared_real"; then
          echo "ERROR: failed to unmount SHARED_MEM_PATH: $shared_real" >&2
          exit 1
        fi
      else
        echo "SHARED_MEM_PATH is not mounted, skip umount: $shared_real"
      fi
      # After umount, remove any backing files that were hidden under the tmpfs
      # (cxlkv only deletes on the mount; D-SIDLE also clears legacy host files).
      rm -f -- "$shared_real/ivshmem_shared_mem" 2>/dev/null || true
      rm -f -- "$shared_real/dsidle_shared.pool" 2>/dev/null || true
    fi
  fi
fi

if ((dry_run)); then
  echo "DSIDLE_VM_CLEAR_DRY_RUN_OK"
else
  echo "VM data clear completed."
  echo "DSIDLE_VM_CLEAR_OK"
fi
