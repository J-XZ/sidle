#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "$script_dir/.." && pwd)"
config="$repo_root/experiment_config.jsonc"
phase="load"
suite=""
load_config=""
run_config=""
rounds=5
round_timeout=7200
out_dir=""
runner="$repo_root/build/dsidle_e2e_trace_runner"
pool_tool="$repo_root/build/dsidle_shared_pool"
node_control_capacity=2097152
max_threads=16
execute=0
smoke=0

usage() {
  echo "usage: $0 [--suite 08|09|ycsb] [--config PATH] [--load-config PATH --run-config PATH] [--phase NAME] [--rounds N] [--round-timeout SEC] [--out-dir DIR] [--runner PATH] [--pool-tool PATH] [--node-control-capacity N] [--max-threads-per-vm N] [--smoke] [--execute] [--dry-run]" >&2
}

need_value() { (($# >= 2)) || { usage; exit 2; }; }
while (($#)); do
  case "$1" in
    --suite|--config|--load-config|--run-config|--phase|--rounds|--round-timeout|--out-dir|--runner|--pool-tool|--node-control-capacity|--max-threads-per-vm)
      need_value "$@"
      case "$1" in
        --suite) suite=$2;; --config) config=$2;; --load-config) load_config=$2;; --run-config) run_config=$2;;
        --phase) phase=$2;; --rounds) rounds=$2;; --round-timeout) round_timeout=$2;;
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
if [[ -n "$suite" ]]; then
  case "$suite" in 08|09|ycsb) ;; *) echo "unknown suite: $suite" >&2; exit 2;; esac
  if [[ "$suite" == ycsb ]]; then
    [[ -n "$load_config" && -n "$run_config" ]] || { echo "--suite ycsb requires --load-config and --run-config" >&2; exit 2; }
    config=$load_config
  elif [[ "$phase" == load ]]; then
    phase="e2e_$suite"
  fi
fi
minimum_rounds=5
((smoke || rounds >= minimum_rounds)) || { echo "--rounds must be at least $minimum_rounds unless --smoke is set" >&2; exit 2; }
[[ -f "$config" ]] || { echo "missing config: $config" >&2; exit 2; }
if [[ -n "$suite" && "$suite" == ycsb ]]; then
  [[ -f "$run_config" ]] || { echo "missing run config: $run_config" >&2; exit 2; }
  python3 - "$load_config" "$run_config" <<'PY'
import json,re,sys
def layout(path):
    cfg=json.loads(re.sub(r'//[^\n]*', '', open(path).read()))
    return (cfg['shared_memory']['path'], cfg['shared_memory']['size_mb'], cfg['vm']['count'], cfg['e2e']['foreground_worker_count_per_vm'])
if layout(sys.argv[1]) != layout(sys.argv[2]): raise SystemExit('YCSB load/run configs must have identical pool topology')
PY
fi

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
[[ -n "$out_dir" ]] || out_dir="exp_data/e2e_${suite:-$phase}_$(date -u +%Y%m%dT%H%M%SZ)"
config_sha256=$(sha256sum "$config" | awk '{print $1}')
git_sha=$(git -C "$repo_root" rev-parse HEAD)
declare -a phase_names phase_configs phase_bootstrap phase_stages
if [[ "$suite" == ycsb ]]; then
  phase_names=(load workloada); phase_configs=("$load_config" "$run_config"); phase_bootstrap=(1 0); phase_stages=(load run)
elif [[ "$suite" == 08 || "$suite" == 09 ]]; then
  suite_phase="e2e${suite}"
  if [[ "$suite" == 08 ]]; then
    phase_names=("${suite_phase}_fill" "${suite_phase}_read")
    phase_configs=("$config" "$config")
    phase_bootstrap=(1 0); phase_stages=(fill read)
  else
    phase_names=("${suite_phase}_fill" "${suite_phase}_update" "${suite_phase}_read")
    phase_configs=("$config" "$config" "$config")
    phase_bootstrap=(1 0 0); phase_stages=(fill update read)
  fi
else
  phase_names=("$phase"); phase_configs=("$config"); phase_bootstrap=(1); phase_stages=([0]=$([[ "$phase" == load ]] && echo load || echo run))
fi

print_command() { printf ' '; printf '%q ' "$@"; printf '\n'; }
echo "DSIDLE_E2E_PLAN suite=${suite:-none} phase=$phase rounds=$rounds smoke=$smoke vm_count=$vm_count workers_per_vm=$workers"
echo "DSIDLE_E2E_BACKING path=$shared_path bytes=$shared_bytes"
if (( ! execute )); then
  print_command truncate -s "$shared_bytes" "$shared_path"
  print_command "$pool_tool" --init-pool --config "$config" --node-control-capacity "$node_control_capacity" --max-threads-per-vm "$max_threads"
  for ((part = 0; part < ${#phase_names[@]}; ++part)); do
    for ((node = 0; node < vm_count; ++node)); do
      cmd=("$runner" --config "${phase_configs[$part]}" --phase "${phase_names[$part]}" --node "$node")
      ((node == 0 && phase_bootstrap[part])) && cmd+=(--bootstrap)
      print_command timeout "$round_timeout" "${cmd[@]}"
    done
  done
  exit 0
fi
[[ -x "$runner" ]] || { echo "runner is not executable: $runner" >&2; exit 2; }
[[ -x "$pool_tool" ]] || { echo "pool tool is not executable: $pool_tool" >&2; exit 2; }
mkdir -p "$out_dir/logs" "$out_dir/round_logs"
python3 - "$out_dir/run_meta.json" "$suite" "$phase" "$rounds" "$smoke" "$config" "$config_sha256" "$git_sha" "$vm_count" "$workers" "${phase_configs[@]}" <<'PY'
import json,sys
path,suite,phase,rounds,smoke,config,config_sha,git_sha,nodes,workers,*phase_configs=sys.argv[1:]
json.dump({'suite':suite or None,'phase':phase,'rounds':int(rounds),'smoke':bool(int(smoke)),'config':config,'config_sha256':config_sha,'phase_configs':phase_configs,'git_sha':git_sha,'nodes':int(nodes),'workers_per_vm':int(workers)}, open(path,'w'), indent=2)
open(path,'a').write('\n')
PY
for ((round = 1; round <= rounds; ++round)); do
  echo "DSIDLE_E2E_ROUND_START suite=${suite:-none} round=$round"
  truncate -s "$shared_bytes" "$shared_path"
  "$pool_tool" --init-pool --config "$config" --node-control-capacity "$node_control_capacity" --max-threads-per-vm "$max_threads" >"$out_dir/round_logs/${suite:-$phase}_round_${round}_pool.log" 2>&1
  for ((part = 0; part < ${#phase_names[@]}; ++part)); do
    pids=()
    for ((node = 0; node < vm_count; ++node)); do
      log="$out_dir/logs/${phase_names[$part]}_round_${round}_${phase_stages[$part]}_node${node}.log"
      cmd=("$runner" --config "${phase_configs[$part]}" --phase "${phase_names[$part]}" --node "$node")
      ((node == 0 && phase_bootstrap[part])) && cmd+=(--bootstrap)
      timeout "$round_timeout" "${cmd[@]}" >"$log" 2>&1 & pids+=("$!")
    done
    status=0
    for pid in "${pids[@]}"; do wait "$pid" || status=1; done
    if ((status)); then echo "DSIDLE_E2E_ROUND_FAIL phase=${phase_names[$part]} round=$round" >&2; exit 1; fi
    for ((node = 0; node < vm_count; ++node)); do grep -q "E2E_TRACE_TIME_US phase=${phase_names[$part]} node=$node" "$out_dir/logs/${phase_names[$part]}_round_${round}_${phase_stages[$part]}_node${node}.log"; done
  done
  printf 'suite=%s phase=%s round=%s git_sha=%s config_sha256=%s exit_code=0\n' "${suite:-none}" "$phase" "$round" "$git_sha" "$config_sha256" >"$out_dir/round_logs/${suite:-$phase}_round_${round}.meta"
  echo "DSIDLE_E2E_ROUND_PASS suite=${suite:-none} round=$round"
done
