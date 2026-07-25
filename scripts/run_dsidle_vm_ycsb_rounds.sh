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

usage() { echo "usage: $0 --prepared-dir DIR [--warmup-rounds N] [--rounds N] [--runner PATH] [--pool-tool PATH] [--round-timeout SEC]" >&2; }
while (($#)); do case "$1" in
  --prepared-dir|--warmup-rounds|--rounds|--runner|--pool-tool|--round-timeout)
    (($# >= 2)) || { usage; exit 2; }
    case "$1" in --prepared-dir) prepared_dir=$2;; --warmup-rounds) warmup_rounds=$2;; --rounds) rounds=$2;; --runner) runner=$2;; --pool-tool) pool_tool=$2;; --round-timeout) timeout_sec=$2;; esac; shift 2;;
  # Compatibility: ivshmem driver is loaded by dsidle_init_vms.sh.
  --ivshmem-module) (($# >= 2)) || { usage; exit 2; }; shift 2;;
  --help) usage; exit 0;; *) usage; exit 2;; esac; done
[[ -n "$prepared_dir" && -d "$prepared_dir" && "$warmup_rounds" =~ ^[0-9]+$ && "$rounds" =~ ^[1-9][0-9]*$ && "$timeout_sec" =~ ^[1-9][0-9]*$ ]] || { usage; exit 2; }
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
  ssh "${ssh_opts[@]}" -p "$port" root@127.0.0.1 \
    "dev=\$(for d in /sys/bus/pci/devices/*; do [ \"\$(cat \"\$d/vendor\")\" = 0x1af4 ] && [ \"\$(cat \"\$d/device\")\" = 0x1110 ] && basename \"\$d\"; done); [ -n \"\$dev\" ]; [ \"\$(basename \"\$(readlink -f \"/sys/bus/pci/devices/\$dev/driver\")\")\" = ivpci ]; test -c $device_path"
done
run_round() {
  local label=$1
  "$pool_tool" --init-pool --config "$base" --node-control-capacity 2097152 --max-threads-per-vm "${topology[2]}" >"$prepared_dir/round_logs/pool_${label}.log" 2>&1
  for phase in "${phases[@]}"; do
    for port in "${ports[@]}"; do
      ssh "${ssh_opts[@]}" -p "$port" root@127.0.0.1 "rm -rf /root/dsidle-ycsb/traces && mkdir -p /root/dsidle-ycsb/traces/$phase"
      rsync -a -e "ssh -o BatchMode=yes -o UserKnownHostsFile=/dev/null -o StrictHostKeyChecking=no -o ConnectTimeout=10 -p $port" "$prepared_dir/traces/$phase/" "root@127.0.0.1:/root/dsidle-ycsb/traces/$phase/"
    done
    pids=(); stage=run; [[ "$phase" == load ]] && stage=load
    for ((node=0; node<vm_count; ++node)); do
      flag=(); [[ "$phase" == load && $node == 0 ]] && flag=(--bootstrap)
      ssh "${ssh_opts[@]}" -p "${ports[$node]}" root@127.0.0.1 "timeout $timeout_sec /root/dsidle-ycsb/dsidle_e2e_trace_runner --config /root/dsidle-ycsb/${phase}.jsonc --phase $phase --node $node ${flag[*]}" >"$prepared_dir/logs/${phase}_${label}_${stage}_node${node}.log" 2>&1 & pids+=("$!")
    done
    status=0; for pid in "${pids[@]}"; do wait "$pid" || status=1; done; ((status==0)) || { echo "VM YCSB failed: $phase $label" >&2; exit 1; }
  done
}
for ((round=1; round<=warmup_rounds; ++round)); do
  run_round "warmup_${round}"
done
for ((round=1; round<=rounds; ++round)); do
  run_round "round_${round}"
done
python3 "$script_dir/summarize_ycsb_experiment.py" --log-dir "$prepared_dir/logs" --out-dir "$prepared_dir" --metadata "$prepared_dir/run_meta.json"
echo "DSIDLE_VM_YCSB_OK out_dir=$prepared_dir"
