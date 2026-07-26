#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

rounds=1
warmup_rounds=1
record_count=100000
operation_count=100000
threads_per_node=4
vm_count=4
round_timeout=7200
out_dir=""
workloads="a,b,c,d,e"
base_config=experiment_config.jsonc
shared_numa=""
shared_reserve_mb=4096
shared_size_mb=""
cache_flush_mb=512
replica_budget_mb=""
prepare_only=0
no_latency=0
skip_build=0
skip_vm_init=0
skip_trace_gen=0
skip_standalone_load=0
value_seed=4851300051586183745
sidle_background_roles=5
sidle_background_epoch_slots=4
heartbeat_threads=1

usage() { echo "usage: $0 [--vm-count 1|2|4] [--warmup-rounds N] [--rounds N] [--record-count N] [--operation-count N] [--threads-per-node N] ..." >&2; }
while (($#)); do
  case "$1" in
    --vm-count|--warmup-rounds|--rounds|--record-count|--operation-count|--threads-per-node|--round-timeout|--out-dir|--workloads|--base-config|--shared-numa|--shared-reserve-mb|--shared-size-mb|--cache-flush-mb|--replica-budget-mb)
      (($# >= 2)) || { usage; exit 2; }
      case "$1" in
        --vm-count) vm_count=$2;; --warmup-rounds) warmup_rounds=$2;; --rounds) rounds=$2;; --record-count) record_count=$2;;
        --operation-count) operation_count=$2;; --threads-per-node) threads_per_node=$2;;
        --round-timeout) round_timeout=$2;; --out-dir) out_dir=$2;; --workloads) workloads=$2;;
        --base-config) base_config=$2;; --shared-numa) shared_numa=$2;;
        --shared-reserve-mb) shared_reserve_mb=$2;; --shared-size-mb) shared_size_mb=$2;;
        --cache-flush-mb) cache_flush_mb=$2;;
        --replica-budget-mb) replica_budget_mb=$2;;
      esac
      shift 2;;
    --no-latency) no_latency=1; shift;;
    --skip-build) skip_build=1; shift;; --skip-vm-init) skip_vm_init=1; shift;;
    --skip-trace-gen) skip_trace_gen=1; shift;; --skip-standalone-load) skip_standalone_load=1; shift;;
    --prepare-only) prepare_only=1; shift;; --help) usage; exit 0;; *) usage; exit 2;;
  esac
done
for value in "$rounds" "$record_count" "$operation_count" "$threads_per_node" "$round_timeout" "$shared_reserve_mb" "$cache_flush_mb"; do [[ "$value" =~ ^[1-9][0-9]*$ ]] || { echo "positive integer required" >&2; exit 2; }; done
[[ "$warmup_rounds" =~ ^[0-9]+$ ]] || { echo "--warmup-rounds must be a non-negative integer" >&2; exit 2; }
[[ "$vm_count" =~ ^(1|2|4)$ ]] || { echo "--vm-count must be 1, 2, or 4" >&2; exit 2; }
[[ -z "$shared_size_mb" || "$shared_size_mb" =~ ^[1-9][0-9]*$ ]] || { echo "--shared-size-mb must be a positive integer" >&2; exit 2; }
[[ -z "$replica_budget_mb" || "$replica_budget_mb" =~ ^[1-9][0-9]*$ ]] || { echo "--replica-budget-mb must be a positive integer" >&2; exit 2; }
[[ -f "$base_config" ]] || { echo "missing base config: $base_config" >&2; exit 2; }
if [[ -n "$shared_numa" ]]; then
  [[ "$shared_numa" =~ ^[0-9]+(,[0-9]+)*$ ]] || { echo "--shared-numa must be comma-separated NUMA node numbers" >&2; exit 2; }
fi
[[ -n "$workloads" && "$workloads" != ,* && "$workloads" != *, && "$workloads" != *,,* ]] || {
  echo "--workloads must be a comma-separated non-empty list" >&2; exit 2;
}
IFS=, read -r -a requested <<< "$workloads"
((${#requested[@]})) || { echo "--workloads must not be empty" >&2; exit 2; }
declare -A seen=()
for workload in "${requested[@]}"; do
  [[ "$workload" =~ ^[abcde]$ ]] || { echo "invalid workload: $workload" >&2; exit 2; }
  [[ -z "${seen[$workload]:-}" ]] || { echo "duplicate workload: $workload" >&2; exit 2; }
  seen[$workload]=1
done
if [[ -z "$out_dir" ]]; then out_dir="exp_data/ycsb_dsidle_$(date -u +%Y%m%dT%H%M%SZ)"; fi
mkdir -p "$out_dir"/{configs,traces,logs,round_logs}
experiment_config="$out_dir/configs/experiment_config_ycsb_4vm.jsonc"
python3 - "$base_config" "$experiment_config" "$shared_numa" "$shared_size_mb" "$no_latency" "$vm_count" "$threads_per_node" "$replica_budget_mb" <<'PY'
import json, re, sys

source, output, numa_csv, size_mb, no_latency, vm_count, workers, replica_budget = sys.argv[1:]
text = open(source).read()
text = re.sub(r'//[^\n]*', '', text)
config = json.loads(text)
config['vm']['count'] = int(vm_count)
config['e2e']['foreground_worker_count_per_vm'] = int(workers)
config['dsidle']['fixed_key_size'] = 32
config['dsidle']['fixed_value_size'] = 32
if replica_budget: config['dsidle']['replica_budget_mb'] = int(replica_budget)
shared = config['shared_memory']
if numa_csv:
    shared['numa_node'] = [int(value) for value in numa_csv.split(',')]
if size_mb:
    shared['size_mb'] = int(size_mb)
size = int(shared['size_mb'])
if size < 2048 or size & (size - 1):
    raise SystemExit('shared_memory.size_mb must be a power of two and at least 2048MB')
hwcc = shared.setdefault('hwcc', {})
swcc = shared.setdefault('swcc', {})
hwcc['offset_mb'], hwcc['size_mb'] = 0, 1024
swcc['offset_mb'], swcc['size_mb'] = 1024, size - 1024
latency = config.setdefault('dsidle', {}).setdefault('latency_inject', {})
latency['cache_model'] = 'none'
latency['cache_hits_enabled'] = False
if int(no_latency):
    for key in ('enabled', 'foreground_enabled', 'merge_enabled', 'stats_enabled'):
        latency[key] = False
open(output, 'w').write(json.dumps(config, separators=(',', ':')) + '\n')
PY
vm_cores=$(python3 - "$experiment_config" <<'PY'
import json,sys
print(json.load(open(sys.argv[1]))['vm']['core_count_per_vm'])
PY
)
((threads_per_node <= vm_cores)) || {
  echo "--threads-per-node exceeds vm.core_count_per_vm=$vm_cores" >&2
  exit 2
}
epoch_slots_per_vm=$((threads_per_node + sidle_background_epoch_slots))
runnable_threads_per_vm=$((threads_per_node + sidle_background_roles + heartbeat_threads))
if ((runnable_threads_per_vm > vm_cores)); then
  echo "warning: each VM runs $threads_per_node foreground + $sidle_background_roles SIDLE background + $heartbeat_threads heartbeat = $runnable_threads_per_vm threads on $vm_cores vCPUs; all original SIDLE roles are retained" >&2
fi
if (( ! skip_trace_gen )); then
  generator="$script_dir/third_party/YCSB-cpp/scripts/generate_cxlkv_trace.sh"
  [[ -x "$generator" ]] || { echo "missing executable YCSB trace generator: $generator (initialize submodules)" >&2; exit 1; }
  generator_root="$script_dir/third_party/YCSB-cpp"
  "$generator" --output-dir "$out_dir/traces" --workload "$generator_root/workloads/workloada" \
    --phase load --nodes "$vm_count" --threads-per-node "$threads_per_node" --record-count "$record_count" \
    --operation-count "$operation_count" --field-length 32 --request-distribution zipfian --force
  cp "$out_dir/traces/manifest.txt" "$out_dir/logs/manifest_load.txt"
  for workload in "${requested[@]}"; do
    generator_args=(--output-dir "$out_dir/traces" --workload "$generator_root/workloads/workload$workload" \
      --run-name "workload$workload" --phase run --nodes "$vm_count" --threads-per-node "$threads_per_node" \
      --record-count "$record_count" --operation-count "$operation_count" --field-length 32 --force)
    if [[ "$workload" == d ]]; then
      generator_args+=(--request-distribution latest)
    else
      generator_args+=(--request-distribution zipfian)
    fi
    [[ "$workload" == a ]] && generator_args+=(--update-read-before-write)
    "$generator" "${generator_args[@]}"
    cp "$out_dir/traces/manifest.txt" "$out_dir/logs/manifest_workload${workload}.txt"
  done
fi
trace_manifest="$out_dir/trace_manifest.json"
python3 - "$out_dir/traces" "$trace_manifest" "$threads_per_node" "$vm_count" \
  "$record_count" "$operation_count" "$value_seed" "$experiment_config" "${requested[@]}" <<'PY'
import json
import sys
from collections import Counter
from pathlib import Path

root, output, threads, nodes, records, operations, value_seed, config_path, *workloads = sys.argv[1:]
root = Path(root)
threads = int(threads)
nodes = int(nodes)
records = int(records)
operations = int(operations)
expected = int(nodes) * int(threads)
config = json.loads(Path(config_path).read_text())
fixed_key_size = int(config['dsidle']['fixed_key_size'])
fixed_value_size = int(config['dsidle']['fixed_value_size'])
if (fixed_key_size, fixed_value_size) != (32, 32):
    raise SystemExit(
        f'formal YCSB requires fixed 32B keys and values, got '
        f'{fixed_key_size}B/{fixed_value_size}B'
    )
phase_names = ['load'] + [f'workload{item}' for item in workloads]
allowed = {
    'load': {'PUT'},
    'workloada': {'GET', 'PUT'},
    'workloadb': {'GET', 'PUT'},
    'workloadc': {'GET'},
    'workloadd': {'GET', 'PUT'},
    'workloade': {'SCAN', 'PUT'},
}
manifest = {
    'nodes': nodes,
    'threads_per_node': threads,
    'total_workers': expected,
    'record_count': records,
    'operation_count': operations,
    'value_seed': int(value_seed),
    'fixed_key_size_bytes': fixed_key_size,
    'fixed_value_size_bytes': fixed_value_size,
    'put_trace_length_ignored': True,
    'generator': {
        'field_count': 10,
        'field_length': 32,
        'key_prefix': 'user',
        'zero_padding': 16,
    },
    'phases': {},
}
for phase in phase_names:
    directory = root / phase
    files = sorted(directory.glob('worker*.txt'))
    names = {item.name for item in files}
    wanted = {f'worker{index}.txt' for index in range(expected)}
    if names != wanted or any(item.stat().st_size == 0 for item in files):
        raise SystemExit(f'invalid trace worker set for {directory}: expected {expected} non-empty worker files')
    phase_counts = Counter()
    worker_rows = {}
    for worker in range(expected):
        path = directory / f'worker{worker}.txt'
        counts = Counter()
        for line_number, line in enumerate(path.read_text(errors='strict').splitlines(), 1):
            stripped = line.lstrip()
            if not stripped or stripped.startswith('#'):
                continue
            op = stripped.split(None, 1)[0]
            if op not in {'PUT', 'GET', 'DELETE', 'SCAN'}:
                raise SystemExit(f'{path}:{line_number}: invalid trace op {op!r}')
            counts[op] += 1
        if not counts:
            raise SystemExit(f'{path}: trace contains no replay commands')
        phase_counts.update(counts)
        worker_rows[str(worker)] = {
            'physical_command_count': sum(counts.values()),
            'op_counts': dict(sorted(counts.items())),
        }
    unexpected = set(phase_counts) - allowed[phase]
    if unexpected:
        raise SystemExit(f'{phase}: unexpected trace ops {sorted(unexpected)}')
    physical = sum(phase_counts.values())
    if phase == 'load':
        if physical != records or phase_counts != Counter({'PUT': records}):
            raise SystemExit(f'load trace count mismatch: expected {records} PUT, got {dict(phase_counts)}')
    elif phase == 'workloada':
        # Each YCSB UPDATE is expanded to GET+PUT; all original reads remain GET.
        if physical != operations + phase_counts['PUT']:
            raise SystemExit(
                f'workloada physical count mismatch: physical={physical} '
                f'operations={operations} put={phase_counts["PUT"]}'
            )
    elif physical != operations:
        raise SystemExit(f'{phase} trace count mismatch: expected {operations}, got {physical}')
    manifest['phases'][phase] = {
        'physical_command_count': physical,
        'op_counts': dict(sorted(phase_counts.items())),
        'request_distribution': 'latest' if phase == 'workloadd' else 'zipfian',
        'update_reads_before_write': phase == 'workloada',
        'workers': worker_rows,
    }
Path(output).write_text(json.dumps(manifest, indent=2, sort_keys=True) + '\n')
PY
python3 - "$experiment_config" "$out_dir/configs" "$out_dir/traces" "${requested[@]}" <<'PY'
import json, sys
from pathlib import Path

base, config_dir, trace_root, *workloads = sys.argv[1:]
config = json.loads(Path(base).read_text())
for phase in ['load'] + [f'workload{item}' for item in workloads]:
    phase_config = json.loads(json.dumps(config))
    phase_config['dsidle']['trace_dir'] = str(Path(trace_root) / phase)
    (Path(config_dir) / f'experiment_config_ycsb_{phase}.jsonc').write_text(json.dumps(phase_config, separators=(',', ':')) + '\n')
PY
printf -v reproduce_command '%q ' "$0" --warmup-rounds "$warmup_rounds" --rounds "$rounds" --record-count "$record_count" --operation-count "$operation_count" --threads-per-node "$threads_per_node" --round-timeout "$round_timeout" --out-dir "$out_dir" --workloads "$workloads" --base-config "$base_config" --shared-reserve-mb "$shared_reserve_mb" --cache-flush-mb "$cache_flush_mb"
printf -v reproduce_command '%s--vm-count %q ' "$reproduce_command" "$vm_count"
[[ -n "$replica_budget_mb" ]] && printf -v reproduce_command '%s--replica-budget-mb %q ' "$reproduce_command" "$replica_budget_mb"
[[ -n "$shared_numa" ]] && printf -v reproduce_command '%s--shared-numa %q ' "$reproduce_command" "$shared_numa"
[[ -n "$shared_size_mb" ]] && printf -v reproduce_command '%s--shared-size-mb %q ' "$reproduce_command" "$shared_size_mb"
((no_latency)) && reproduce_command+='--no-latency '
((skip_build)) && reproduce_command+='--skip-build '
((skip_vm_init)) && reproduce_command+='--skip-vm-init '
((skip_trace_gen)) && reproduce_command+='--skip-trace-gen '
((skip_standalone_load)) && reproduce_command+='--skip-standalone-load '
((prepare_only)) && reproduce_command+='--prepare-only '
python3 - "$out_dir/run_meta.json" "$warmup_rounds" "$rounds" "$record_count" "$operation_count" "$threads_per_node" "$round_timeout" "$workloads" "$base_config" "$experiment_config" "$no_latency" "$shared_numa" "$shared_reserve_mb" "$shared_size_mb" "$cache_flush_mb" "$skip_build" "$skip_vm_init" "$skip_trace_gen" "$skip_standalone_load" "$reproduce_command" "$vm_count" "$replica_budget_mb" "$trace_manifest" "$value_seed" "$sidle_background_roles" "$sidle_background_epoch_slots" "$heartbeat_threads" "$epoch_slots_per_vm" "$runnable_threads_per_vm" "$vm_cores" <<'PY'
import hashlib,json,subprocess,sys
from pathlib import Path
(path,warmup_rounds,rounds,records,ops,threads,timeout,workloads,base_config,experiment_config,no_latency,shared_numa,reserve,size,flush,skip_build,skip_vm_init,skip_trace_gen,skip_load,reproduce_command,nodes,replica_budget,trace_manifest,value_seed,background_roles,background_epoch_slots,heartbeat_threads,epoch_slots_per_vm,runnable_threads_per_vm,vm_cores)=sys.argv[1:]
try: git_sha=subprocess.check_output(['git','rev-parse','HEAD'], text=True).strip()
except Exception: git_sha='unknown'
phase_names=['load'] + [f'workload{item}' for item in workloads.split(',')]
config_dir=Path(experiment_config).parent
meta={"warmup_rounds":int(warmup_rounds),"rounds":int(rounds),"record_count":int(records),"operation_count":int(ops),"threads_per_node":int(threads),"round_timeout_sec":int(timeout),"nodes":int(nodes),"total_trace_workers":int(threads)*int(nodes),"vm_vcpus_per_node":int(vm_cores),"sidle_background_roles_per_node":int(background_roles),"heartbeat_threads_per_node":int(heartbeat_threads),"runnable_threads_per_node":int(runnable_threads_per_vm),"background_epoch_slots_per_node":int(background_epoch_slots),"epoch_slots_per_node":int(epoch_slots_per_vm),"replica_budget_mb":int(replica_budget) if replica_budget else None,"workloads":workloads.split(','),"base_config":base_config,"experiment_config":experiment_config,"experiment_config_sha256":hashlib.sha256(open(experiment_config,'rb').read()).hexdigest(),"phase_configs":{phase:str(config_dir/f'experiment_config_ycsb_{phase}.jsonc') for phase in phase_names},"trace_manifest":trace_manifest,"trace_manifest_sha256":hashlib.sha256(open(trace_manifest,'rb').read()).hexdigest(),"value_seed":int(value_seed),"git_sha":git_sha,"shared_numa":shared_numa.split(',') if shared_numa else None,"shared_reserve_mb":int(reserve),"shared_size_mb":int(size) if size else None,"cache_flush_mb":int(flush),"latency_inject_enabled":not bool(int(no_latency)),"skip_build":bool(int(skip_build)),"skip_vm_init":bool(int(skip_vm_init)),"skip_trace_gen":bool(int(skip_trace_gen)),"skip_standalone_load":bool(int(skip_load)),"reproduce_command":reproduce_command.rstrip()}
open(path,'w').write(json.dumps(meta,indent=2)+"\n")
PY
if ((prepare_only)); then echo "DSIDLE_YCSB_PREPARED out_dir=$out_dir"; exit 0; fi
if (( ! skip_build )); then
  # Host RelWithDebInfo build (ivpci driver is guest-built by dsidle_init_vms.sh).
  cmake -S "$script_dir" -B "$script_dir/build" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
  cmake --build "$script_dir/build" --target dsidle_e2e_trace_runner dsidle_shared_pool -j"$(nproc)"
fi
if (( ! skip_vm_init )); then
  "$script_dir/dsidle_init_vms.sh" --config "$experiment_config" --execute
else
  "$script_dir/dsidle_check_vms.sh" --config "$experiment_config"
fi
"$script_dir/scripts/run_dsidle_vm_ycsb_rounds.sh" \
  --prepared-dir "$out_dir" --warmup-rounds "$warmup_rounds" \
  --rounds "$rounds" --round-timeout "$round_timeout" \
  --cache-flush-mb "$cache_flush_mb"
