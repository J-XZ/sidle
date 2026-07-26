#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "$script_dir/.." && pwd)"
prepared_dir=""
rounds=1
warmup_rounds=0
runner="${DSIDLE_VM_TRACE_RUNNER:-$repo_root/build/dsidle_e2e_trace_runner}"
pool_tool="${DSIDLE_POOL_TOOL:-$repo_root/build/dsidle_shared_pool}"
timeout_sec=7200
cache_flush_mb=512

usage() { echo "usage: $0 --prepared-dir DIR [--warmup-rounds N] [--rounds N] [--runner PATH] [--pool-tool PATH] [--round-timeout SEC] [--cache-flush-mb MB]" >&2; }
while (($#)); do case "$1" in
  --prepared-dir|--warmup-rounds|--rounds|--runner|--pool-tool|--round-timeout|--cache-flush-mb)
    (($# >= 2)) || { usage; exit 2; }
    case "$1" in --prepared-dir) prepared_dir=$2;; --warmup-rounds) warmup_rounds=$2;; --rounds) rounds=$2;; --runner) runner=$2;; --pool-tool) pool_tool=$2;; --round-timeout) timeout_sec=$2;; --cache-flush-mb) cache_flush_mb=$2;; esac; shift 2;;
  # Compatibility: ivshmem driver is loaded by dsidle_init_vms.sh.
  --ivshmem-module) (($# >= 2)) || { usage; exit 2; }; shift 2;;
  --help) usage; exit 0;; *) usage; exit 2;; esac; done
[[ -n "$prepared_dir" && -d "$prepared_dir" && "$warmup_rounds" =~ ^[0-9]+$ && "$rounds" =~ ^[1-9][0-9]*$ && "$timeout_sec" =~ ^[1-9][0-9]*$ && "$cache_flush_mb" =~ ^[1-9][0-9]*$ ]] || { usage; exit 2; }
prepared_dir=$(realpath "$prepared_dir")
[[ -x "$runner" && -x "$pool_tool" ]] || { echo "missing VM runner or pool tool under build/" >&2; exit 2; }
base="$prepared_dir/configs/experiment_config_ycsb_4vm.jsonc"
[[ -f "$base" ]] || { echo "missing prepared config" >&2; exit 2; }
mapfile -t topology < <(python3 - "$base" <<'PY'
import json,sys
c=json.load(open(sys.argv[1])); print(c['vm']['count']); print(c['vm']['ssh_base_port']); print(c['vm']['core_count_per_vm']); print(c['shared_memory']['device_path'])
PY
)
vm_count=${topology[0]}
device_path=${topology[3]}
[[ "$vm_count" =~ ^(1|2|4)$ ]] || { echo "requires 1, 2, or 4 VMs" >&2; exit 2; }
ports=(); for ((node=0; node<vm_count; ++node)); do ports+=("$((topology[1]+node))"); done
mkdir -p "$prepared_dir/guest_configs" "$prepared_dir/round_logs" "$prepared_dir/logs"
mapfile -t phases < <(python3 - "$prepared_dir/run_meta.json" <<'PY'
import json,sys
m=json.load(open(sys.argv[1])); print('load'); [print('workload'+x) for x in m['workloads']]
PY
)
mapfile -t run_control < <(python3 - "$prepared_dir/run_meta.json" <<'PY'
import json,sys
m=json.load(open(sys.argv[1]))
print(1 if m.get('skip_standalone_load') else 0)
print(m['threads_per_node'])
print(m['nodes'])
print(m['value_seed'])
print(m['epoch_slots_per_node'])
PY
)
skip_standalone_load=${run_control[0]}
trace_workers_per_node=${run_control[1]}
metadata_nodes=${run_control[2]}
value_seed=${run_control[3]}
epoch_slots_per_node=${run_control[4]}
[[ "$metadata_nodes" == "$vm_count" ]] || { echo "run metadata/config VM count mismatch" >&2; exit 2; }
trace_manifest="$prepared_dir/trace_manifest.json"
[[ -f "$trace_manifest" ]] || { echo "missing trace manifest: $trace_manifest" >&2; exit 2; }
manifest_sha=$(sha256sum "$trace_manifest" | awk '{print $1}')
recorded_manifest_sha=$(python3 - "$prepared_dir/run_meta.json" <<'PY'
import json,sys
print(json.load(open(sys.argv[1]))['trace_manifest_sha256'])
PY
)
[[ "$manifest_sha" == "$recorded_manifest_sha" ]] || {
  echo "trace manifest changed after run metadata was written" >&2
  exit 1
}
for phase in "${phases[@]}"; do
  python3 - "$trace_manifest" "$phase" \
    "$prepared_dir/round_logs/trace_sha256_${phase}.txt" <<'PY'
import json,sys
from pathlib import Path
manifest_path,phase,output=sys.argv[1:]
manifest=json.load(open(manifest_path))
workers=manifest['phases'][phase]['workers']
Path(output).write_text(''.join(
    f'{workers[str(worker)]["sha256"]}  worker{worker}.txt\n'
    for worker in range(manifest['total_workers'])
))
PY
done
for phase in "${phases[@]}"; do
  python3 - "$prepared_dir/configs/experiment_config_ycsb_${phase}.jsonc" "$prepared_dir/guest_configs/${phase}.jsonc" "$phase" <<'PY'
import json,sys
s,t,p=sys.argv[1:]; c=json.load(open(s)); c['shared_memory']['path']=c['shared_memory']['device_path']; c['dsidle']['trace_dir']='/root/dsidle-ycsb/traces/'+p
open(t,'w').write(json.dumps(c,separators=(',',':'))+'\n')
PY
done
ssh_opts=(-o BatchMode=yes -o UserKnownHostsFile=/dev/null -o StrictHostKeyChecking=no -o ConnectTimeout=10 -o ServerAliveInterval=30 -o ServerAliveCountMax=20)
for port in "${ports[@]}"; do
  ssh "${ssh_opts[@]}" -p "$port" root@127.0.0.1 'mkdir -p /root/dsidle-ycsb/traces'
  rsync -a -e "ssh -o BatchMode=yes -o UserKnownHostsFile=/dev/null -o StrictHostKeyChecking=no -o ConnectTimeout=10 -p $port" \
    "$runner" "$prepared_dir/guest_configs/" root@127.0.0.1:/root/dsidle-ycsb/
  runner_sha=$(sha256sum "$runner" | awk '{print $1}')
  guest_runner_sha=$(ssh "${ssh_opts[@]}" -p "$port" root@127.0.0.1 \
    'sha256sum /root/dsidle-ycsb/dsidle_e2e_trace_runner' | awk '{print $1}')
  [[ "$runner_sha" == "$guest_runner_sha" ]] || {
    echo "guest runner hash mismatch on port $port" >&2
    exit 1
  }
  ssh "${ssh_opts[@]}" -p "$port" root@127.0.0.1 \
    "dev=\$(for d in /sys/bus/pci/devices/*; do [ \"\$(cat \"\$d/vendor\")\" = 0x1af4 ] && [ \"\$(cat \"\$d/device\")\" = 0x1110 ] && basename \"\$d\"; done); [ -n \"\$dev\" ]; [ \"\$(basename \"\$(readlink -f \"/sys/bus/pci/devices/\$dev/driver\")\")\" = ivpci ]; test -c $device_path"
done

reset_pool() {
  local tag=$1
  "$pool_tool" --init-pool --config "$base" --node-control-capacity 2097152 \
    --max-threads-per-vm "$epoch_slots_per_node" \
    >"$prepared_dir/round_logs/pool_${tag}.log" 2>&1
}

sync_phase_trace() {
  local phase=$1
  local port
  local checks="$prepared_dir/round_logs/trace_sha256_${phase}.txt"
  (
    cd "$prepared_dir/traces/$phase"
    sha256sum --status -c "$checks"
  ) || {
    echo "host trace hash mismatch: phase=$phase" >&2
    exit 1
  }
  for port in "${ports[@]}"; do
    ssh "${ssh_opts[@]}" -p "$port" root@127.0.0.1 \
      "rm -rf /root/dsidle-ycsb/traces && mkdir -p /root/dsidle-ycsb/traces/$phase"
    rsync -a -e "ssh -o BatchMode=yes -o UserKnownHostsFile=/dev/null -o StrictHostKeyChecking=no -o ConnectTimeout=10 -p $port" \
      "$prepared_dir/traces/$phase/" "root@127.0.0.1:/root/dsidle-ycsb/traces/$phase/"
    rsync -a -e "ssh -o BatchMode=yes -o UserKnownHostsFile=/dev/null -o StrictHostKeyChecking=no -o ConnectTimeout=10 -p $port" \
      "$checks" "root@127.0.0.1:/root/dsidle-ycsb/traces/$phase/.manifest.sha256"
    ssh "${ssh_opts[@]}" -p "$port" root@127.0.0.1 \
      "cd /root/dsidle-ycsb/traces/$phase && sha256sum --status -c .manifest.sha256" \
      >"$prepared_dir/logs/trace_verify_${phase}_port${port}.log" 2>&1 || {
      echo "guest trace hash mismatch: phase=$phase port=$port" >&2
      exit 1
    }
  done
}

clear_vm_caches() {
  local tag=$1
  local node
  local -a pids=()
  for ((node=0; node<vm_count; ++node)); do
    ssh "${ssh_opts[@]}" -p "${ports[$node]}" root@127.0.0.1 \
      "sync; echo 3 > /proc/sys/vm/drop_caches; python3 -c 'b=bytearray(${cache_flush_mb}*1024*1024); s=0
for i in range(0, len(b), 64): b[i]=(i >> 6) & 255; s ^= b[i]
print(s)' >/dev/null" \
      >"$prepared_dir/logs/clear_cache_${tag}_node${node}.log" 2>&1 &
    pids+=("$!")
  done
  local status=0
  local pid
  for pid in "${pids[@]}"; do wait "$pid" || status=1; done
  ((status==0)) || { echo "VM cache reset failed: $tag" >&2; exit 1; }
}

validate_phase_results() {
  local case_name=$1
  local label=$2
  local stage=$3
  local phase=$4
  python3 - "$prepared_dir/logs" "$trace_manifest" "$case_name" "$label" "$stage" \
    "$phase" "$vm_count" "$trace_workers_per_node" <<'PY'
import json
import re
import sys
from pathlib import Path

log_dir, manifest_path, case_name, label, stage, phase, nodes, workers = sys.argv[1:]
log_dir = Path(log_dir)
manifest = json.loads(Path(manifest_path).read_text())
nodes = int(nodes)
workers = int(workers)
pattern = re.compile(
    r'E2E_TRACE_TIME_US phase=(\S+) node=(\d+) ops=(\d+) duration_us=(\d+) '
    r'trace_first=(\d+) trace_workers=(\d+) batch_ops=(\d+)'
)
phase_manifest = manifest['phases'][phase]
for node in range(nodes):
    path = log_dir / f'{case_name}_{label}_{stage}_node{node}.log'
    rows = [
        pattern.fullmatch(line)
        for line in path.read_text(errors='replace').splitlines()
        if line.startswith('E2E_TRACE_TIME_US ')
    ]
    if len(rows) != 1 or rows[0] is None:
        raise SystemExit(f'{path}: expected exactly one complete E2E_TRACE_TIME_US row')
    row = rows[0]
    expected_ops = sum(
        phase_manifest['workers'][str(worker)]['physical_command_count']
        for worker in range(node * workers, (node + 1) * workers)
    )
    actual = {
        'phase': row.group(1),
        'node': int(row.group(2)),
        'ops': int(row.group(3)),
        'trace_first': int(row.group(5)),
        'trace_workers': int(row.group(6)),
        'batch_ops': int(row.group(7)),
    }
    expected = {
        'phase': phase,
        'node': node,
        'ops': expected_ops,
        'trace_first': node * workers,
        'trace_workers': workers,
    }
    for key, value in expected.items():
        if actual[key] != value:
            raise SystemExit(f'{path}: {key} mismatch: expected {value}, got {actual[key]}')
    if actual['batch_ops'] <= 0:
        raise SystemExit(f'{path}: invalid batch_ops={actual["batch_ops"]}')
PY
}

run_phase() {
  local case_name=$1
  local label=$2
  local stage=$3
  local phase=$4
  local reset_caches=$5
  sync_phase_trace "$phase"
  ((reset_caches==0)) || clear_vm_caches "${case_name}_${label}_${stage}"
  local -a pids=()
  local node
  for ((node=0; node<vm_count; ++node)); do
    local -a flag=()
    [[ "$phase" == load && $node == 0 ]] && flag=(--bootstrap)
    ssh "${ssh_opts[@]}" -p "${ports[$node]}" root@127.0.0.1 \
      "timeout $timeout_sec /root/dsidle-ycsb/dsidle_e2e_trace_runner --config /root/dsidle-ycsb/${phase}.jsonc --phase $phase --node $node --value-seed $value_seed ${flag[*]}" \
      >"$prepared_dir/logs/${case_name}_${label}_${stage}_node${node}.log" 2>&1 &
    pids+=("$!")
  done
  local status=0
  local pid
  for pid in "${pids[@]}"; do wait "$pid" || status=1; done
  ((status==0)) || { echo "VM YCSB failed: case=$case_name phase=$phase $label" >&2; exit 1; }
  validate_phase_results "$case_name" "$label" "$stage" "$phase"
}

run_case() {
  local case_name=$1
  local label=$2
  reset_pool "${case_name}_${label}"
  run_phase "$case_name" "$label" load load 1
  if [[ "$case_name" != load ]]; then
    run_phase "$case_name" "$label" run "$case_name" 0
  fi
}

if ((skip_standalone_load==0)); then
  for ((round=1; round<=warmup_rounds; ++round)); do run_case load "warmup_${round}"; done
  for ((round=1; round<=rounds; ++round)); do run_case load "round_${round}"; done
fi
for phase in "${phases[@]}"; do
  [[ "$phase" == load ]] && continue
  for ((round=1; round<=warmup_rounds; ++round)); do run_case "$phase" "warmup_${round}"; done
  for ((round=1; round<=rounds; ++round)); do run_case "$phase" "round_${round}"; done
done
python3 "$script_dir/summarize_ycsb_experiment.py" --log-dir "$prepared_dir/logs" --out-dir "$prepared_dir" --metadata "$prepared_dir/run_meta.json"
echo "DSIDLE_VM_YCSB_OK out_dir=$prepared_dir"
