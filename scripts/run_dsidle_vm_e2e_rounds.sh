#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "$script_dir/.." && pwd)"
config="$repo_root/experiment_config.jsonc"
suite=""
rounds=1
out_dir=""
runner="${DSIDLE_VM_SUITE_RUNNER:-$repo_root/build/dsidle_e2e_suite_runner}"
pool_tool="${DSIDLE_POOL_TOOL:-$repo_root/build/dsidle_shared_pool}"
node_capacity=2097152
max_threads=16
round_timeout=7200
execute=0
formal_acceptance=0
cache_cpu_workers="${DSIDLE_E2E_CACHE_CPU_WORKERS:-4}"
cache_sweep_mb="${DSIDLE_E2E_CACHE_SWEEP_MB:-64}"
cache_helper="$script_dir/clear_dsidle_caches.py"
summarizer="$script_dir/summarize_vm_e2e.py"
acceptance_helper="$script_dir/acceptance_evidence.py"

usage() {
  echo "usage: $0 --suite 08|09 [--formal-acceptance] [--config PATH] [--rounds N] [--out-dir DIR] [--runner PATH] [--pool-tool PATH] [--node-control-capacity N] [--max-threads-per-vm N] [--round-timeout SEC] --execute" >&2
}
need_value() { (($# >= 2)) || { usage; exit 2; }; }
while (($#)); do
  case "$1" in
    --suite|--config|--rounds|--out-dir|--runner|--pool-tool|--node-control-capacity|--max-threads-per-vm|--round-timeout)
      need_value "$@"
      case "$1" in
        --suite) suite=$2;; --config) config=$2;; --rounds) rounds=$2;; --out-dir) out_dir=$2;;
        --runner) runner=$2;; --pool-tool) pool_tool=$2;; --node-control-capacity) node_capacity=$2;;
        --max-threads-per-vm) max_threads=$2;; --round-timeout) round_timeout=$2;;
      esac
      shift 2;;
    # The ivshmem driver is loaded by dsidle_init_vms.sh before this runner.
    --ivshmem-module) (($# >= 2)) || { usage; exit 2; }; shift 2;;
    --formal-acceptance) formal_acceptance=1; shift;;
    --execute) execute=1; shift;;
    --help) usage; exit 0;;
    *) usage; exit 2;;
  esac
done
[[ "$suite" == 08 || "$suite" == 09 ]] || { echo "--suite must be 08 or 09" >&2; exit 2; }
for value in "$rounds" "$node_capacity" "$max_threads" "$round_timeout" "$cache_cpu_workers" "$cache_sweep_mb"; do
  [[ "$value" =~ ^[1-9][0-9]*$ ]] || { echo "positive integer required" >&2; exit 2; }
done
[[ -f "$config" ]] || { echo "missing config: $config" >&2; exit 2; }
[[ -x "$runner" ]] || { echo "missing suite runner: $runner (build RelWithDebInfo target dsidle_e2e_suite_runner)" >&2; exit 2; }
[[ -x "$pool_tool" ]] || { echo "missing pool tool: $pool_tool" >&2; exit 2; }
runner=$(realpath "$runner")
pool_tool=$(realpath "$pool_tool")
[[ -x "$cache_helper" && -x "$summarizer" && -f "$acceptance_helper" ]] || {
  echo "missing e2e cache/summarizer/acceptance helper" >&2
  exit 2
}
((execute)) || { echo "refusing to start a VM experiment without --execute" >&2; exit 2; }
if ((formal_acceptance)) && [[ "$rounds" != 10 ]]; then
  echo "formal VM E2E acceptance requires exactly 10 rounds" >&2
  exit 2
fi

mapfile -t topology < <(python3 - "$repo_root/scripts" "$config" <<'PY'
import sys
sys.path.insert(0, sys.argv[1])
from jsonc_utils import load_jsonc
cfg=load_jsonc(sys.argv[2])
print(cfg['vm']['count'])
print(cfg['vm']['ssh_base_port'])
print(cfg['shared_memory']['device_path'])
print(cfg['vm']['core_count_per_vm'])
print(cfg['e2e']['foreground_worker_count_per_vm'])
PY
)
vm_count=${topology[0]}
ssh_base_port=${topology[1]}
device_path=${topology[2]}
vm_cores=${topology[3]}
foreground_workers=${topology[4]}
[[ "$vm_count" == 4 ]] || { echo "VM E2E runner requires four VMs" >&2; exit 2; }
[[ "$vm_cores" == 8 && "$foreground_workers" == 4 ]] || {
  echo "formal VM E2E requires 4 foreground workers and 8 vCPUs per VM" >&2
  exit 2
}
if ((formal_acceptance)); then
  git -C "$repo_root" diff --quiet HEAD -- || {
    echo "formal VM E2E acceptance requires a clean tracked worktree" >&2
    exit 2
  }
  python3 - "$repo_root/scripts" "$config" <<'PY'
import sys
sys.path.insert(0, sys.argv[1])
from jsonc_utils import load_jsonc
cfg=load_jsonc(sys.argv[2])
shared=cfg['shared_memory']
checks = {
    'shared_memory.size_mb': (shared['size_mb'], 32768),
    'shared_memory.hwcc.offset_mb': (shared['hwcc']['offset_mb'], 0),
    'shared_memory.hwcc.size_mb': (shared['hwcc']['size_mb'], 1024),
    'shared_memory.swcc.offset_mb': (shared['swcc']['offset_mb'], 1024),
    'shared_memory.swcc.size_mb': (shared['swcc']['size_mb'], 31744),
    'dsidle.verbose': (cfg['dsidle']['verbose'], False),
    'dsidle.extra_check': (cfg['dsidle']['extra_check'], False),
}
for field, (actual, expected) in checks.items():
    if actual != expected:
        raise SystemExit(
            f'formal VM E2E contract mismatch for {field}: '
            f'expected {expected!r}, got {actual!r}')
latency = cfg['dsidle']['latency_inject']
if latency['fixed_latency']['enabled']:
    raise SystemExit(
        'formal VM E2E contract mismatch for '
        'dsidle.latency_inject.fixed_latency.enabled: expected False')
PY
fi
[[ -n "$out_dir" ]] || out_dir="$repo_root/exp_data/vm_e2e${suite}_$(date -u +%Y%m%dT%H%M%SZ)"
out_dir=$(realpath -m "$out_dir")
if [[ -e "$out_dir/acceptance.meta" || -e "$out_dir/run_complete.meta" ]]; then
  echo "refusing to reuse a completed output directory: $out_dir" >&2
  exit 2
fi
mkdir -p "$out_dir/logs" "$out_dir/configs" "$out_dir/round_logs"
host_config="$out_dir/configs/e2e${suite}_host.jsonc"
cp -- "$config" "$host_config"
config="$host_config"
guest_config="$out_dir/configs/e2e${suite}_guest.jsonc"
python3 - "$repo_root/scripts" "$config" "$guest_config" "$suite" <<'PY'
import json,sys
sys.path.insert(0, sys.argv[1])
from jsonc_utils import load_jsonc
source,target,suite=sys.argv[2:]
cfg=load_jsonc(source)
cfg['shared_memory']['path']=cfg['shared_memory']['device_path']
cfg['dsidle']['fixed_key_size']=8 if suite == '08' else 32
cfg['dsidle']['fixed_value_size']=8 if suite == '08' else 1000
with open(target, 'w') as out:
    out.write('// Generated for the guest ivpci BAR; do not use for host pool initialization.\n')
    json.dump(cfg, out, separators=(',', ':'))
    out.write('\n')
PY

ssh_opts=(-o BatchMode=yes -o UserKnownHostsFile=/dev/null -o StrictHostKeyChecking=no -o ConnectTimeout=10 -o ServerAliveInterval=30 -o ServerAliveCountMax=20)
remote_dir=/root/dsidle-bin
remote_runner="$remote_dir/dsidle_e2e_suite_runner"
remote_config="$remote_dir/e2e${suite}_guest.jsonc"
runner_sha256=$(sha256sum "$runner" | awk '{print $1}')
pool_tool_sha256=$(sha256sum "$pool_tool" | awk '{print $1}')
config_sha256=$(sha256sum "$config" | awk '{print $1}')
guest_config_sha256=$(sha256sum "$guest_config" | awk '{print $1}')
git_sha=$(git -C "$repo_root" rev-parse HEAD)
git_tracked_clean=1
for ((node = 0; node < vm_count; ++node)); do
  port=$((ssh_base_port + node))
  ssh "${ssh_opts[@]}" -p "$port" root@127.0.0.1 "mkdir -p $remote_dir"
  rsync -a -e "ssh -o BatchMode=yes -o UserKnownHostsFile=/dev/null -o StrictHostKeyChecking=no -o ConnectTimeout=10 -p $port" \
    "$runner" "$guest_config" "root@127.0.0.1:$remote_dir/"
  guest_runner_sha256=$(ssh "${ssh_opts[@]}" -p "$port" root@127.0.0.1 \
    "sha256sum $remote_runner" | awk '{print $1}')
  guest_deployed_config_sha256=$(ssh "${ssh_opts[@]}" -p "$port" root@127.0.0.1 \
    "sha256sum $remote_config" | awk '{print $1}')
  [[ "$guest_runner_sha256" == "$runner_sha256" ]] || {
    echo "guest runner hash mismatch on node $node" >&2
    exit 1
  }
  [[ "$guest_deployed_config_sha256" == "$guest_config_sha256" ]] || {
    echo "guest config hash mismatch on node $node" >&2
    exit 1
  }
  # Driver must already be loaded by dsidle_init_vms.sh (ivpci + full BAR2).
  ssh "${ssh_opts[@]}" -p "$port" root@127.0.0.1 \
    "dev=\$(for d in /sys/bus/pci/devices/*; do [ \"\$(cat \"\$d/vendor\")\" = 0x1af4 ] && [ \"\$(cat \"\$d/device\")\" = 0x1110 ] && basename \"\$d\"; done); [ -n \"\$dev\" ]; [ \"\$(basename \"\$(readlink -f \"/sys/bus/pci/devices/\$dev/driver\")\")\" = ivpci ]; test -c $device_path"
done

python3 - "$out_dir/run_meta.json" "$suite" "$rounds" "$config" \
  "$config_sha256" "$guest_config" "$guest_config_sha256" "$runner" \
  "$git_sha" "$vm_count" "$foreground_workers" "$vm_cores" \
  "$cache_cpu_workers" "$cache_sweep_mb" "$runner_sha256" \
  "$pool_tool" "$pool_tool_sha256" "$formal_acceptance" "$git_tracked_clean" <<'PY'
import json,sys
(path,suite,rounds,config,config_sha,guest_config,guest_sha,runner,git_sha,
 nodes,workers,vm_cores,cache_workers,cache_mb,runner_sha,pool_tool,
 pool_tool_sha,formal_acceptance,git_tracked_clean)=sys.argv[1:]
meta={
    'suite':suite,'rounds':int(rounds),'config':config,
    'config_sha256':config_sha,'guest_config':guest_config,
    'guest_config_sha256':guest_sha,'runner':runner,'git_sha':git_sha,
    'runner_sha256':runner_sha,'pool_tool':pool_tool,
    'pool_tool_sha256':pool_tool_sha,
    'nodes':int(nodes),'workers_per_vm':int(workers),
    'vm_vcpus_per_node':int(vm_cores),'total_keys':100000,
    'formal_acceptance':bool(int(formal_acceptance)),
    'git_tracked_clean':bool(int(git_tracked_clean)),
    'cache_clear':{
        'target':'host_and_vms','cpu_workers':int(cache_workers),
        'cpu_sweep_mb_per_worker':int(cache_mb),'page_cache':True,
    },
}
open(path,'w').write(json.dumps(meta,indent=2)+'\n')
PY
if ((formal_acceptance)); then
  python3 "$acceptance_helper" validate --kind vm-e2e \
    --metadata "$out_dir/run_meta.json"
fi
printf 'suite=%s rounds=%s config=%s config_sha256=%s guest_config_sha256=%s runner=%s runner_sha256=%s pool_tool_sha256=%s git_sha=%s vm_count=%s workers_per_vm=%s vm_vcpus_per_node=%s\n' \
  "$suite" "$rounds" "$config" "$config_sha256" "$guest_config_sha256" \
  "$runner" "$runner_sha256" "$pool_tool_sha256" "$git_sha" "$vm_count" \
  "$foreground_workers" "$vm_cores" \
  >"$out_dir/run.meta"
phase_prefix="e2e${suite}"
if [[ "$suite" == 08 ]]; then
  phases=(fill read)
else
  phases=(fill update read)
fi
verify_local_artifacts() {
  [[ "$(sha256sum "$runner" | awk '{print $1}')" == "$runner_sha256" ]] || {
    echo "runner binary changed during VM E2E" >&2
    return 1
  }
  [[ "$(sha256sum "$pool_tool" | awk '{print $1}')" == "$pool_tool_sha256" ]] || {
    echo "pool tool changed during VM E2E" >&2
    return 1
  }
  [[ "$(sha256sum "$config" | awk '{print $1}')" == "$config_sha256" ]] || {
    echo "host config changed during VM E2E" >&2
    return 1
  }
  [[ "$(sha256sum "$guest_config" | awk '{print $1}')" == "$guest_config_sha256" ]] || {
    echo "guest config changed during VM E2E" >&2
    return 1
  }
}
verify_guest_deployment() {
  local node port remote_hash
  for ((node = 0; node < vm_count; ++node)); do
    port=$((ssh_base_port + node))
    remote_hash=$(ssh "${ssh_opts[@]}" -p "$port" root@127.0.0.1 \
      "sha256sum $remote_runner" | awk '{print $1}')
    [[ "$remote_hash" == "$runner_sha256" ]] || {
      echo "guest runner changed on node $node" >&2
      return 1
    }
    remote_hash=$(ssh "${ssh_opts[@]}" -p "$port" root@127.0.0.1 \
      "sha256sum $remote_config" | awk '{print $1}')
    [[ "$remote_hash" == "$guest_config_sha256" ]] || {
      echo "guest config changed on node $node" >&2
      return 1
    }
  done
}
for ((round = 1; round <= rounds; ++round)); do
  echo "DSIDLE_VM_E2E_ROUND_START suite=$suite round=$round"
  round_meta="$out_dir/round_logs/${phase_prefix}_round_${round}.meta"
  if ! verify_local_artifacts || ! verify_guest_deployment; then
    printf 'suite=%s round=%s git_sha=%s config_sha256=%s guest_config_sha256=%s runner_sha256=%s pool_tool_sha256=%s exit_code=1 failed_stage=provenance\n' \
      "$suite" "$round" "$git_sha" "$config_sha256" "$guest_config_sha256" \
      "$runner_sha256" "$pool_tool_sha256" >"$round_meta"
    exit 1
  fi
  if ! "$pool_tool" --init-pool --config "$config" \
      --node-control-capacity "$node_capacity" \
      --max-threads-per-vm "$max_threads" \
      >"$out_dir/logs/pool_round_${round}.log" 2>&1; then
    printf 'suite=%s round=%s git_sha=%s config_sha256=%s guest_config_sha256=%s runner_sha256=%s pool_tool_sha256=%s exit_code=1 failed_stage=pool_init\n' \
      "$suite" "$round" "$git_sha" "$config_sha256" "$guest_config_sha256" \
      "$runner_sha256" "$pool_tool_sha256" >"$round_meta"
    exit 1
  fi
  if ! "$cache_helper" --config "$config" --target all \
      --cpu-workers "$cache_cpu_workers" --cpu-sweep-mb "$cache_sweep_mb" \
      >"$out_dir/logs/cache_round_${round}.log" 2>&1; then
    printf 'suite=%s round=%s git_sha=%s config_sha256=%s guest_config_sha256=%s runner_sha256=%s pool_tool_sha256=%s exit_code=1 failed_stage=cache_clear\n' \
      "$suite" "$round" "$git_sha" "$config_sha256" "$guest_config_sha256" \
      "$runner_sha256" "$pool_tool_sha256" >"$round_meta"
    exit 1
  fi
  for stage in "${phases[@]}"; do
    pids=()
    for ((node = 0; node < vm_count; ++node)); do
      port=$((ssh_base_port + node))
      command=(timeout "$round_timeout" "$remote_runner" --config "$remote_config" --phase "${phase_prefix}_${stage}" --node "$node")
      ((node == 0)) && [[ "$stage" == fill ]] && command+=(--bootstrap)
      ssh "${ssh_opts[@]}" -p "$port" root@127.0.0.1 "$(printf '%q ' "${command[@]}")" >"$out_dir/logs/${phase_prefix}_${stage}_round_${round}_node${node}.log" 2>&1 &
      pids+=("$!")
    done
    status=0
    for pid in "${pids[@]}"; do wait "$pid" || status=1; done
    if ((status)); then
      printf 'suite=%s round=%s git_sha=%s config_sha256=%s guest_config_sha256=%s runner_sha256=%s pool_tool_sha256=%s exit_code=1 failed_stage=%s\n' \
        "$suite" "$round" "$git_sha" "$config_sha256" "$guest_config_sha256" \
        "$runner_sha256" "$pool_tool_sha256" "$stage" >"$round_meta"
      echo "DSIDLE_VM_E2E_ROUND_FAIL suite=$suite round=$round stage=$stage" >&2
      exit 1
    fi
    for ((node = 0; node < vm_count; ++node)); do
      if ! grep -q "DSIDLE_E2E_SUITE_VERIFY suite=$suite phase=${phase_prefix}_${stage} node=$node status=ok" \
          "$out_dir/logs/${phase_prefix}_${stage}_round_${round}_node${node}.log"; then
        printf 'suite=%s round=%s git_sha=%s config_sha256=%s guest_config_sha256=%s runner_sha256=%s pool_tool_sha256=%s exit_code=1 failed_stage=%s_verify_node%s\n' \
          "$suite" "$round" "$git_sha" "$config_sha256" "$guest_config_sha256" \
          "$runner_sha256" "$pool_tool_sha256" "$stage" "$node" >"$round_meta"
        echo "DSIDLE_VM_E2E_ROUND_FAIL suite=$suite round=$round stage=$stage node=$node missing_verify=1" >&2
        exit 1
      fi
    done
  done
  printf 'suite=%s round=%s git_sha=%s config_sha256=%s guest_config_sha256=%s runner_sha256=%s pool_tool_sha256=%s exit_code=0\n' \
    "$suite" "$round" "$git_sha" "$config_sha256" "$guest_config_sha256" \
    "$runner_sha256" "$pool_tool_sha256" >"$round_meta"
  echo "DSIDLE_VM_E2E_ROUND_PASS suite=$suite round=$round"
done
"$summarizer" --suite "$suite" --log-dir "$out_dir/logs" \
  --out-dir "$out_dir" --metadata "$out_dir/run_meta.json"
verify_local_artifacts
if ((formal_acceptance)); then
  python3 "$acceptance_helper" finalize --kind vm-e2e \
    --metadata "$out_dir/run_meta.json" \
    --summary "$out_dir/${phase_prefix}_summary.json" \
    --summary "$out_dir/${phase_prefix}_phase_rows.csv" \
    --summary "$out_dir/${phase_prefix}_round_phase_max_summary.csv" \
    --summary "$out_dir/${phase_prefix}_summary.md" \
    --output "$out_dir/acceptance.meta"
else
  printf 'status=success kind=development suite=%s rounds=%s git_sha=%s config_sha256=%s runner_sha256=%s pool_tool_sha256=%s\n' \
    "$suite" "$rounds" "$git_sha" "$config_sha256" "$runner_sha256" \
    "$pool_tool_sha256" >"$out_dir/run_complete.meta"
fi
echo "DSIDLE_VM_E2E_OK suite=$suite rounds=$rounds out_dir=$out_dir"
