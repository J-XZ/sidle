#!/usr/bin/env bash
# Reusable local CTest sandbox for D-SIDLE.
#
# Every CTest that touches pool backings, generated configs, traces, logs or
# result directories runs inside one unique temporary root so repeated test
# runs cannot accumulate fixed-path artifacts in /tmp.  The wrapper:
#   * creates a fresh root under ${TMPDIR:-/tmp} (dsidle-ctest.XXXXXX);
#   * rewrites each --config-file into a copy inside the root with
#     shared_memory.path replaced by $root/pool.bin, exports it as
#     DSIDLE_TEST_CONFIG[_N], and pre-sizes the pool to the first config's
#     size (the same layout all D-SIDLE runner tests share);
#   * exports DSIDLE_TEST_ROOT / DSIDLE_TEST_POOL for the child;
#   * passes the child's output through (and to $root/test.log) so CTest gets
#     diagnostics on failure;
#   * removes the root on EXIT/INT/TERM/HUP even when the child aborts;
#   * keeps the root only when DSIDLE_KEEP_TEST_ARTIFACTS=1 (or --keep) and
#     then prints the path.
#
# Usage:
#   dsidle_ctest_wrapper.sh [--keep] [--config-file PATH]... -- cmd args...
set -u

keep=0
case "${DSIDLE_KEEP_TEST_ARTIFACTS:-0}" in
  1|true|yes) keep=1 ;;
esac

config_files=()
root=""
cleanup() {
  local status=$?
  if [[ -n "$root" ]]; then
    if ((keep)); then
      printf 'DSIDLE_TEST_ARTIFACTS=%s\n' "$root"
    else
      rm -rf -- "$root"
    fi
  fi
  exit "$status"
}
trap cleanup EXIT INT TERM HUP

while (($#)); do
  case "$1" in
    --keep) keep=1; shift ;;
    --config-file)
      (($# >= 2)) || { echo "missing value for --config-file" >&2; exit 2; }
      config_files+=("$2"); shift 2 ;;
    --) shift; break ;;
    *) echo "unknown wrapper option: $1" >&2; exit 2 ;;
  esac
done
(($#)) || { echo "usage: dsidle_ctest_wrapper.sh [--keep] [--config-file PATH]... -- cmd args..." >&2; exit 2; }

root="$(mktemp -d "${TMPDIR:-/tmp}/dsidle-ctest.XXXXXX")" || exit 2
pool="$root/pool.bin"
export DSIDLE_TEST_ROOT="$root"
export DSIDLE_TEST_POOL="$pool"
export DSIDLE_YCSB_GENERATOR_BUILD_DIR="$root/ycsb-build"

config_count=${#config_files[@]}
if ((config_count)); then
  index=0
  for src in "${config_files[@]}"; do
    [[ -f "$src" ]] || { echo "missing config: $src" >&2; exit 2; }
    out="$root/config_${index}.jsonc"
    python3 - "$src" "$out" "$pool" <<'PY' || exit 2
import json, sys
src, out, pool = sys.argv[1:]
text = open(src, encoding='utf-8').read()
# Strip // and /* */ comments outside strings, same as scripts/jsonc_utils.py.
res, in_str, esc, j = [], False, False, 0
while j < len(text):
    c = text[j]
    n = text[j + 1] if j + 1 < len(text) else ''
    if in_str:
        res.append(c)
        if esc:
            esc = False
        elif c == '\\':
            esc = True
        elif c == '"':
            in_str = False
    elif c == '"':
        in_str = True
        res.append(c)
    elif c == '/' and n == '/':
        j = text.find('\n', j)
        if j < 0:
            break
    elif c == '/' and n == '*':
        end = text.find('*/', j + 2)
        if end < 0:
            break
        j = end + 1
    else:
        res.append(c)
    j += 1
cfg = json.loads(''.join(res))
cfg['shared_memory']['path'] = pool
with open(out, 'w', encoding='utf-8') as f:
    json.dump(cfg, f, separators=(',', ':'))
    f.write('\n')
PY
    export "DSIDLE_TEST_CONFIG_${index}=$out"
    if ((index == 0)); then
      export DSIDLE_TEST_CONFIG="$out"
      size_mb=$(python3 - "$out" <<'PY'
import json, sys
cfg = json.load(open(sys.argv[1]))
print(cfg['shared_memory']['size_mb'])
PY
)
      truncate -s "$((size_mb * 1024 * 1024))" "$pool" || exit 2
    fi
    index=$((index + 1))
  done
fi

"$@" 2>&1 | tee "$root/test.log"
status=${PIPESTATUS[0]}
exit "$status"
