#!/usr/bin/env bash
set -euo pipefail

rounds=1
record_count=100000
operation_count=100000
threads_per_node=4
out_dir=""
workloads="a,b,c,d"
base_config=experiment_config.jsonc
prepare_only=0
no_latency=0

usage() { echo "usage: $0 [--rounds N] [--record-count N] [--operation-count N] [--threads-per-node N] [--out-dir DIR] [--workloads a,b,c,d,e] [--base-config PATH] [--no-latency] [--prepare-only]" >&2; }
while (($#)); do
  case "$1" in
    --rounds|--record-count|--operation-count|--threads-per-node|--out-dir|--workloads|--base-config)
      (($# >= 2)) || { usage; exit 2; }
      case "$1" in
        --rounds) rounds=$2;; --record-count) record_count=$2;;
        --operation-count) operation_count=$2;; --threads-per-node) threads_per_node=$2;;
        --out-dir) out_dir=$2;; --workloads) workloads=$2;; --base-config) base_config=$2;;
      esac
      shift 2;;
    --no-latency) no_latency=1; shift;;
    --prepare-only) prepare_only=1; shift;; --help) usage; exit 0;; *) usage; exit 2;;
  esac
done
for value in "$rounds" "$record_count" "$operation_count" "$threads_per_node"; do [[ "$value" =~ ^[1-9][0-9]*$ ]] || { echo "positive integer required" >&2; exit 2; }; done
[[ -f "$base_config" ]] || { echo "missing base config: $base_config" >&2; exit 2; }
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
printf -v reproduce_command '%q ' "$0" --rounds "$rounds" --record-count "$record_count" --operation-count "$operation_count" --threads-per-node "$threads_per_node" --out-dir "$out_dir" --workloads "$workloads" --base-config "$base_config"
((no_latency)) && reproduce_command+='--no-latency '
((prepare_only)) && reproduce_command+='--prepare-only '
python3 - "$out_dir/run_meta.json" "$rounds" "$record_count" "$operation_count" "$threads_per_node" "$workloads" "$base_config" "$no_latency" "$reproduce_command" <<'PY'
import json,sys
path,rounds,records,ops,threads,workloads,config,no_latency,reproduce_command=sys.argv[1:]
meta={"rounds":int(rounds),"record_count":int(records),"operation_count":int(ops),"threads_per_node":int(threads),"nodes":4,"total_trace_workers":int(threads)*4,"workloads":workloads.split(','),"base_config":config,"latency_inject_enabled":not bool(int(no_latency)),"reproduce_command":reproduce_command.rstrip()}
open(path,'w').write(json.dumps(meta,indent=2)+"\n")
PY
if ((prepare_only)); then echo "DSIDLE_YCSB_PREPARED out_dir=$out_dir"; exit 0; fi
echo "trace generation and VM replay require the in-tree YCSB-cpp generator and prepared VM image; use --prepare-only to validate inputs" >&2
exit 1
