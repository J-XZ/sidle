#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "$script_dir/.." && pwd)"
config="$repo_root/experiment_config.jsonc"
phase="load"
rounds=10
round_timeout=7200
out_dir=""
runner="$repo_root/build/dsidle_e2e_trace_runner"
pool_tool="$repo_root/build/dsidle_shared_pool"
node_control_capacity=2097152
max_threads=16
execute=0
smoke=0

usage() {
  echo "usage: $0 [--config PATH] [--phase NAME] [--rounds N] [--round-timeout SEC] [--out-dir DIR] [--runner PATH] [--pool-tool PATH] [--node-control-capacity N] [--max-threads-per-vm N] [--smoke] [--execute] [--dry-run]" >&2
}

need_value() { (($# >= 2)) || { usage; exit 2; }; }
while (($#)); do
  case "$1" in
    --config|--phase|--rounds|--round-timeout|--out-dir|--runner|--pool-tool|--node-control-capacity|--max-threads-per-vm)
      need_value "$@"
      case "$1" in
        --config) config=$2;; --phase) phase=$2;; --rounds) rounds=$2;; --round-timeout) round_timeout=$2;;
        --out-dir) out_dir=$2;; --runner) runner=$2;; --pool-tool) pool_tool=$2;;
        --node-control-capacity) node_control_capacity=$2;; --max-threads-per-vm) max_threads=$2;;
      esac
      shift 2;;
    --smoke) smoke=1; shift;; --execute) execute=1; shift;; --dry-run) execute=0; shift;;
    --help) usage; exit 0;; *) usage; exit 2;;
  esac
done
for value in "$rounds" "$round_timeout" "$node_control_capacity" "$max_threads"; do
  [[ "$value" =~ ^[1-9][0-9]*$ ]] || { echo "positive integer required" >&2; exit 2; }
done
[[ "$phase" =~ ^[A-Za-z0-9_.-]+$ ]] || { echo "invalid phase name" >&2; exit 2; }
((smoke || rounds >= 10)) || { echo "--rounds must be at least 10 unless --smoke is set" >&2; exit 2; }
[[ -f "$config" ]] || { echo "missing config: $config" >&2; exit 2; }

mapfile -t layout < <(python3 - "$config" <<'PY'
import json,re,sys
text=re.sub(r'//[^\n]*', '', open(sys.argv[1]).read())
cfg=json.loads(text)
shared=cfg['shared_memory']
print(shared['path'])
print(int(shared['size_mb']) * 1024 * 1024)
print(int(cfg['vm']['count']))
print(int(cfg['e2e']['foreground_worker_count_per_vm']))
PY
)
shared_path=${layout[0]}
shared_bytes=${layout[1]}
vm_count=${layout[2]}
workers=${layout[3]}
[[ -n "$out_dir" ]] || out_dir="exp_data/e2e_${phase}_$(date -u +%Y%m%dT%H%M%SZ)"
stage=run
[[ "$phase" == load ]] && stage=load
config_sha256=$(sha256sum "$config" | awk '{print $1}')
git_sha=$(git -C "$repo_root" rev-parse HEAD)

print_command() { printf ' '; printf '%q ' "$@"; printf '\n'; }
echo "DSIDLE_E2E_PLAN phase=$phase rounds=$rounds smoke=$smoke vm_count=$vm_count workers_per_vm=$workers"
echo "DSIDLE_E2E_BACKING path=$shared_path bytes=$shared_bytes"
if (( ! execute )); then
  print_command truncate -s "$shared_bytes" "$shared_path"
  print_command "$pool_tool" --init-pool --config "$config" --node-control-capacity "$node_control_capacity" --max-threads-per-vm "$max_threads"
  for ((node = 0; node < vm_count; ++node)); do
    cmd=("$runner" --config "$config" --phase "$phase" --node "$node")
    ((node == 0)) && cmd+=(--bootstrap)
    print_command timeout "$round_timeout" "${cmd[@]}"
  done
  exit 0
fi
[[ -x "$runner" ]] || { echo "runner is not executable: $runner" >&2; exit 2; }
[[ -x "$pool_tool" ]] || { echo "pool tool is not executable: $pool_tool" >&2; exit 2; }
mkdir -p "$out_dir/logs" "$out_dir/round_logs"
python3 - "$out_dir/run_meta.json" "$phase" "$rounds" "$smoke" "$config" "$config_sha256" "$git_sha" "$vm_count" "$workers" <<'PY'
import json,sys
path,phase,rounds,smoke,config,config_sha,git_sha,nodes,workers=sys.argv[1:]
json.dump({'phase':phase,'rounds':int(rounds),'smoke':bool(int(smoke)),'config':config,'config_sha256':config_sha,'git_sha':git_sha,'nodes':int(nodes),'workers_per_vm':int(workers)}, open(path,'w'), indent=2)
open(path,'a').write('\n')
PY
for ((round = 1; round <= rounds; ++round)); do
  echo "DSIDLE_E2E_ROUND_START phase=$phase round=$round"
  truncate -s "$shared_bytes" "$shared_path"
  "$pool_tool" --init-pool --config "$config" --node-control-capacity "$node_control_capacity" --max-threads-per-vm "$max_threads" >"$out_dir/round_logs/${phase}_round_${round}_pool.log" 2>&1
  pids=()
  for ((node = 0; node < vm_count; ++node)); do
    log="$out_dir/logs/${phase}_round_${round}_${stage}_node${node}.log"
    cmd=("$runner" --config "$config" --phase "$phase" --node "$node")
    ((node == 0)) && cmd+=(--bootstrap)
    timeout "$round_timeout" "${cmd[@]}" >"$log" 2>&1 & pids+=("$!")
  done
  status=0
  for pid in "${pids[@]}"; do wait "$pid" || status=1; done
  if ((status)); then
    echo "DSIDLE_E2E_ROUND_FAIL phase=$phase round=$round" >&2
    exit 1
  fi
  for ((node = 0; node < vm_count; ++node)); do
    grep -q "E2E_TRACE_TIME_US phase=$phase node=$node" "$out_dir/logs/${phase}_round_${round}_${stage}_node${node}.log"
  done
  printf 'phase=%s round=%s git_sha=%s config_sha256=%s exit_code=0\n' "$phase" "$round" "$git_sha" "$config_sha256" >"$out_dir/round_logs/${phase}_round_${round}.meta"
  echo "DSIDLE_E2E_ROUND_PASS phase=$phase round=$round"
done
