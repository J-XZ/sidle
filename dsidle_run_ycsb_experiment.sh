#!/usr/bin/env bash
set -euo pipefail

rounds=1
record_count=100000
operation_count=100000
threads_per_node=4
round_timeout=7200
out_dir=""
workloads="a,b,c,d"
base_config=experiment_config.jsonc
shared_numa=""
shared_reserve_mb=4096
shared_size_mb=""
cache_flush_mb=512
prepare_only=0
no_latency=0
skip_build=0
skip_vm_init=0
skip_trace_gen=0
skip_standalone_load=0

usage() { echo "usage: $0 [--rounds N] [--record-count N] [--operation-count N] [--threads-per-node N] [--round-timeout SEC] [--out-dir DIR] [--workloads a,b,c,d,e] [--base-config PATH] [--shared-numa N[,N...]] [--shared-reserve-mb MB] [--shared-size-mb MB] [--cache-flush-mb MB] [--no-latency] [--skip-build] [--skip-vm-init] [--skip-trace-gen] [--skip-standalone-load] [--prepare-only]" >&2; }
while (($#)); do
  case "$1" in
    --rounds|--record-count|--operation-count|--threads-per-node|--round-timeout|--out-dir|--workloads|--base-config|--shared-numa|--shared-reserve-mb|--shared-size-mb|--cache-flush-mb)
      (($# >= 2)) || { usage; exit 2; }
      case "$1" in
        --rounds) rounds=$2;; --record-count) record_count=$2;;
        --operation-count) operation_count=$2;; --threads-per-node) threads_per_node=$2;;
        --round-timeout) round_timeout=$2;; --out-dir) out_dir=$2;; --workloads) workloads=$2;;
        --base-config) base_config=$2;; --shared-numa) shared_numa=$2;;
        --shared-reserve-mb) shared_reserve_mb=$2;; --shared-size-mb) shared_size_mb=$2;;
        --cache-flush-mb) cache_flush_mb=$2;;
      esac
      shift 2;;
    --no-latency) no_latency=1; shift;;
    --skip-build) skip_build=1; shift;; --skip-vm-init) skip_vm_init=1; shift;;
    --skip-trace-gen) skip_trace_gen=1; shift;; --skip-standalone-load) skip_standalone_load=1; shift;;
    --prepare-only) prepare_only=1; shift;; --help) usage; exit 0;; *) usage; exit 2;;
  esac
done
for value in "$rounds" "$record_count" "$operation_count" "$threads_per_node" "$round_timeout" "$shared_reserve_mb" "$cache_flush_mb"; do [[ "$value" =~ ^[1-9][0-9]*$ ]] || { echo "positive integer required" >&2; exit 2; }; done
[[ -z "$shared_size_mb" || "$shared_size_mb" =~ ^[1-9][0-9]*$ ]] || { echo "--shared-size-mb must be a positive integer" >&2; exit 2; }
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
python3 - "$base_config" "$experiment_config" "$shared_numa" "$shared_size_mb" "$no_latency" <<'PY'
import json, re, sys

source, output, numa_csv, size_mb, no_latency = sys.argv[1:]
text = open(source).read()
text = re.sub(r'//[^\n]*', '', text)
config = json.loads(text)
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
open(output, 'w').write(json.dumps(config, indent=2) + '\n')
PY
printf -v reproduce_command '%q ' "$0" --rounds "$rounds" --record-count "$record_count" --operation-count "$operation_count" --threads-per-node "$threads_per_node" --round-timeout "$round_timeout" --out-dir "$out_dir" --workloads "$workloads" --base-config "$base_config" --shared-reserve-mb "$shared_reserve_mb" --cache-flush-mb "$cache_flush_mb"
[[ -n "$shared_numa" ]] && printf -v reproduce_command '%s--shared-numa %q ' "$reproduce_command" "$shared_numa"
[[ -n "$shared_size_mb" ]] && printf -v reproduce_command '%s--shared-size-mb %q ' "$reproduce_command" "$shared_size_mb"
((no_latency)) && reproduce_command+='--no-latency '
((skip_build)) && reproduce_command+='--skip-build '
((skip_vm_init)) && reproduce_command+='--skip-vm-init '
((skip_trace_gen)) && reproduce_command+='--skip-trace-gen '
((skip_standalone_load)) && reproduce_command+='--skip-standalone-load '
((prepare_only)) && reproduce_command+='--prepare-only '
python3 - "$out_dir/run_meta.json" "$rounds" "$record_count" "$operation_count" "$threads_per_node" "$round_timeout" "$workloads" "$base_config" "$experiment_config" "$no_latency" "$shared_numa" "$shared_reserve_mb" "$shared_size_mb" "$cache_flush_mb" "$skip_build" "$skip_vm_init" "$skip_trace_gen" "$skip_standalone_load" "$reproduce_command" <<'PY'
import hashlib,json,subprocess,sys
(path,rounds,records,ops,threads,timeout,workloads,base_config,experiment_config,no_latency,shared_numa,reserve,size,flush,skip_build,skip_vm_init,skip_trace_gen,skip_load,reproduce_command)=sys.argv[1:]
try: git_sha=subprocess.check_output(['git','rev-parse','HEAD'], text=True).strip()
except Exception: git_sha='unknown'
meta={"rounds":int(rounds),"record_count":int(records),"operation_count":int(ops),"threads_per_node":int(threads),"round_timeout_sec":int(timeout),"nodes":4,"total_trace_workers":int(threads)*4,"workloads":workloads.split(','),"base_config":base_config,"experiment_config":experiment_config,"experiment_config_sha256":hashlib.sha256(open(experiment_config,'rb').read()).hexdigest(),"git_sha":git_sha,"shared_numa":shared_numa.split(',') if shared_numa else None,"shared_reserve_mb":int(reserve),"shared_size_mb":int(size) if size else None,"cache_flush_mb":int(flush),"latency_inject_enabled":not bool(int(no_latency)),"skip_build":bool(int(skip_build)),"skip_vm_init":bool(int(skip_vm_init)),"skip_trace_gen":bool(int(skip_trace_gen)),"skip_standalone_load":bool(int(skip_load)),"reproduce_command":reproduce_command.rstrip()}
open(path,'w').write(json.dumps(meta,indent=2)+"\n")
PY
if ((prepare_only)); then echo "DSIDLE_YCSB_PREPARED out_dir=$out_dir"; exit 0; fi
echo "trace generation and VM replay require the in-tree YCSB-cpp generator and prepared VM image; use --prepare-only to validate inputs" >&2
exit 1
