#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
config="${DSIDLE_EXPERIMENT_CONFIG_JSONC:-$script_dir/experiment_config.jsonc}"
dry_run=0
ssh_user=root
while (($#)); do
  case "$1" in
    --config|--ssh-user) (($# >= 2)) || { echo "missing value for $1" >&2; exit 2; }; [[ "$1" == --config ]] && config=$2 || ssh_user=$2; shift 2;;
    --dry-run) dry_run=1; shift;; --help) echo "usage: $0 [--config PATH] [--ssh-user USER] [--dry-run]" >&2; exit 0;;
    *) echo "usage: $0 [--config PATH] [--ssh-user USER] [--dry-run]" >&2; exit 2;;
  esac
done
[[ -f "$config" ]] || { echo "missing config: $config" >&2; exit 2; }
python3 - "$config" "$dry_run" "$ssh_user" <<'PY'
import json, os, re, subprocess, sys
from pathlib import Path

config, dry, ssh_user = sys.argv[1:]
dry = bool(int(dry))
text = re.sub(r'//[^\n]*', '', Path(config).read_text())
cfg = json.loads(text); vm, shared, cpu = cfg['vm'], cfg['shared_memory'], cfg['host_cpu']
backing = Path(shared['path'])
if backing.is_dir(): backing /= 'ivshmem_shared_mem'
nodes = shared['numa_node'] if isinstance(shared['numa_node'], list) else [shared['numa_node']]
vm_nodes = vm['numa_node'] if isinstance(vm['numa_node'], list) else [vm['numa_node']]
vm_cores = cpu['vm_cores'] if isinstance(cpu['vm_cores'], list) else [cpu['vm_cores']]
for index in range(int(vm['count'])):
    vm_dir = Path(vm['storage_path']) / f'vm_{index}'
    pidfile = vm_dir / 'qemu.pid'
    port = int(vm['ssh_base_port']) + index
    wanted_cores = set(map(int, vm_cores[index * int(vm['core_count_per_vm']):(index + 1) * int(vm['core_count_per_vm'])]))
    if dry:
        print(f'DRY_RUN verify pid={pidfile} qemu_cmdline=ivshmem:{backing} taskset={",".join(map(str, sorted(wanted_cores)))} ssh={ssh_user}@127.0.0.1:{port} device={shared["device_path"]} numa_nodes={",".join(map(str, nodes))}')
        continue
    if not pidfile.is_file(): raise SystemExit(f'missing pid file: {pidfile}')
    try: pid = int(pidfile.read_text().strip()); os.kill(pid, 0)
    except (ValueError, ProcessLookupError): raise SystemExit(f'stale or invalid pid file: {pidfile}')
    cmdline = Path(f'/proc/{pid}/cmdline').read_bytes().replace(b'\0', b' ').decode(errors='replace')
    if 'qemu-system-x86_64' not in cmdline or str(backing) not in cmdline or 'ivshmem-plain' not in cmdline:
        raise SystemExit(f'pid {pid} is not the expected ivshmem QEMU process')
    actual_cores = os.sched_getaffinity(pid)
    if not wanted_cores <= actual_cores:
        raise SystemExit(f'pid {pid} CPU affinity {sorted(actual_cores)} misses configured cores {sorted(wanted_cores)}')
    subprocess.run(['ssh', '-o', 'BatchMode=yes', '-o', 'ConnectTimeout=5', '-o', 'StrictHostKeyChecking=no', '-p', str(port), f'{ssh_user}@127.0.0.1', f'test -e {shared["device_path"]}'], check=True)
    mapped_nodes = set()
    backing_lines = []
    for line in Path(f'/proc/{pid}/numa_maps').read_text(errors='replace').splitlines():
        if str(backing) not in line: continue
        backing_lines.append(line)
        mapped_nodes.update(int(item.split('=')[0][1:]) for item in line.split() if re.fullmatch(r'N\d+=\d+', item))
    if not backing_lines: raise SystemExit(f'pid {pid} has no numa_maps entry for {backing}')
    # A freshly truncated ivshmem file has no resident pages yet.  Once a
    # workload faults shared pages in, their observed NUMA nodes must obey the
    # configured shared-memory placement.
    if mapped_nodes and not mapped_nodes <= set(map(int, nodes)):
        raise SystemExit(f'pid {pid} backing NUMA nodes {sorted(mapped_nodes)} outside configured {nodes}')
print('DSIDLE_VM_CHECK_DRY_RUN_OK' if dry else 'DSIDLE_VM_CHECK_OK')
PY
