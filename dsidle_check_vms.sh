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
python3 - "$script_dir/scripts" "$config" "$dry_run" "$ssh_user" <<'PY'
import json, os, re, subprocess, sys
from pathlib import Path

sys.path.insert(0, sys.argv[1])
from jsonc_utils import load_jsonc

config, dry, ssh_user = sys.argv[2:]
dry = bool(int(dry))
cfg = load_jsonc(config); vm, shared, cpu = cfg['vm'], cfg['shared_memory'], cfg['host_cpu']
backing = Path(shared['path'])
if backing.is_dir(): backing /= 'ivshmem_shared_mem'
nodes = shared['numa_node'] if isinstance(shared['numa_node'], list) else [shared['numa_node']]
vm_cores = cpu['vm_cores'] if isinstance(cpu['vm_cores'], list) else [cpu['vm_cores']]
device_path = shared['device_path']
for index in range(int(vm['count'])):
    vm_dir = Path(vm['storage_path']) / f'vm_{index}'
    pidfile = vm_dir / 'qemu.pid'
    port = int(vm['ssh_base_port']) + index
    wanted_cores = set(map(int, vm_cores[index * int(vm['core_count_per_vm']):(index + 1) * int(vm['core_count_per_vm'])]))
    if len(wanted_cores) != int(vm['core_count_per_vm']):
        raise SystemExit(
            f'vm_{index} has {len(wanted_cores)} configured host CPUs, '
            f'expected {int(vm["core_count_per_vm"])}'
        )
    if dry:
        print(f'DRY_RUN verify pid={pidfile} qemu_cmdline=ivshmem:{backing} taskset={",".join(map(str, sorted(wanted_cores)))} ssh={ssh_user}@127.0.0.1:{port} device={device_path} driver=ivpci numa_nodes={",".join(map(str, nodes))}')
        continue
    if not pidfile.is_file(): raise SystemExit(f'missing pid file: {pidfile}')
    try: pid = int(pidfile.read_text().strip()); os.kill(pid, 0)
    except (ValueError, ProcessLookupError): raise SystemExit(f'stale or invalid pid file: {pidfile}')
    cmdline = Path(f'/proc/{pid}/cmdline').read_bytes().replace(b'\0', b' ').decode(errors='replace')
    if 'qemu-system-x86_64' not in cmdline or str(backing) not in cmdline or 'ivshmem-plain' not in cmdline:
        raise SystemExit(f'pid {pid} is not the expected ivshmem QEMU process')
    checked_threads = 0
    for task in sorted(Path(f'/proc/{pid}/task').iterdir()):
        try:
            actual_cores = os.sched_getaffinity(int(task.name))
        except ProcessLookupError:
            continue
        checked_threads += 1
        if actual_cores != wanted_cores:
            raise SystemExit(
                f'pid {pid} thread {task.name} CPU affinity '
                f'{sorted(actual_cores)} differs from configured '
                f'{sorted(wanted_cores)}'
            )
    if not checked_threads:
        raise SystemExit(f'pid {pid} has no live threads to verify CPU affinity')
    ssh = ['ssh', '-o', 'BatchMode=yes', '-o', 'ConnectTimeout=5', '-o', 'StrictHostKeyChecking=no', '-o', 'UserKnownHostsFile=/dev/null', '-p', str(port), f'{ssh_user}@127.0.0.1']
    # Require custom ivpci driver (not uio_pci_generic) and the configured device node.
    subprocess.run(ssh + ['bash', '-lc',
        f'dev=$(for d in /sys/bus/pci/devices/*; do [ "$(cat "$d/vendor")" = 0x1af4 ] && [ "$(cat "$d/device")" = 0x1110 ] && basename "$d"; done); '
        f'[ -n "$dev" ]; [ "$(basename "$(readlink -f "/sys/bus/pci/devices/$dev/driver")")" = ivpci ]; test -c {device_path}'],
        check=True)
    mapped_nodes = set()
    backing_lines = []
    for line in Path(f'/proc/{pid}/numa_maps').read_text(errors='replace').splitlines():
        if str(backing) not in line: continue
        backing_lines.append(line)
        mapped_nodes.update(int(item.split('=')[0][1:]) for item in line.split() if re.fullmatch(r'N\d+=\d+', item))
    if not backing_lines: raise SystemExit(f'pid {pid} has no numa_maps entry for {backing}')
    # Fresh sparse/tmpfs backing may have no resident pages yet; once faulted in,
    # observed NUMA nodes must obey shared_memory.numa_node.
    if mapped_nodes and not mapped_nodes <= set(map(int, nodes)):
        raise SystemExit(f'pid {pid} backing NUMA nodes {sorted(mapped_nodes)} outside configured {nodes}')
print('DSIDLE_VM_CHECK_DRY_RUN_OK' if dry else 'DSIDLE_VM_CHECK_OK')
PY
