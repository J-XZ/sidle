#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
config="${DSIDLE_EXPERIMENT_CONFIG_JSONC:-$script_dir/experiment_config.jsonc}"
image="$script_dir/image/root.img"
pool_tool="$script_dir/build/dsidle_shared_pool"
dry_run=0
execute=0
allow_overlapping_numa=0
apply_host_tuning=0

usage() {
  echo "usage: $0 [--config PATH] [--image PATH] [--pool-tool PATH] [--dry-run] [--allow-overlapping-numa] [--apply-host-tuning] --execute" >&2
}
while (($#)); do
  case "$1" in
    --config|--image|--pool-tool)
      (($# >= 2)) || { usage; exit 2; }
      case "$1" in --config) config=$2;; --image) image=$2;; --pool-tool) pool_tool=$2;; esac
      shift 2;;
    --dry-run) dry_run=1; shift;; --execute) execute=1; shift;;
    --allow-overlapping-numa) allow_overlapping_numa=1; shift;;
    --apply-host-tuning) apply_host_tuning=1; shift;;
    --help) usage; exit 0;; *) usage; exit 2;;
  esac
done
((dry_run || execute)) || { echo "refusing VM/backing changes without --execute (use --dry-run)" >&2; exit 2; }
[[ -f "$config" ]] || { echo "missing config: $config" >&2; exit 2; }

python3 - "$config" "$image" "$pool_tool" "$dry_run" "$execute" "$allow_overlapping_numa" "$apply_host_tuning" <<'PY'
import json, os, re, shutil, signal, subprocess, sys, time
from pathlib import Path

config_path, image, pool_tool, dry, execute, allow_overlap, apply_tuning = sys.argv[1:]
dry, execute, allow_overlap, apply_tuning = map(int, (dry, execute, allow_overlap, apply_tuning))

def load_jsonc(path):
    text = Path(path).read_text()
    out, in_string, escaped, i = [], False, False, 0
    while i < len(text):
        char, nxt = text[i], text[i + 1] if i + 1 < len(text) else ''
        if in_string:
            out.append(char)
            if escaped: escaped = False
            elif char == '\\': escaped = True
            elif char == '"': in_string = False
            i += 1
        elif char == '"': in_string = True; out.append(char); i += 1
        elif char == '/' and nxt == '/':
            i = text.find('\n', i)
            if i < 0: break
        else: out.append(char); i += 1
    return json.loads(''.join(out))

def as_list(value): return value if isinstance(value, list) else [value]
def online_cpus(text):
    result = set()
    for item in text.strip().split(','):
        first, *last = item.split('-'); result.update(range(int(first), int(last[0]) + 1 if last else int(first) + 1))
    return result
def command_text(command): return ' '.join(shlex_quote(str(item)) for item in command)
def shlex_quote(text):
    return "'" + text.replace("'", "'\\''") + "'" if re.search(r'[^A-Za-z0-9_@%+=:,./-]', text) else text
def run(command, **kwargs):
    print('DSIDLE_VM_CMD ' + command_text(command))
    if not dry: subprocess.run(command, check=True, **kwargs)

cfg = load_jsonc(config_path)
shared, vm, cpu = cfg['shared_memory'], cfg['vm'], cfg['host_cpu']
shared_nodes, vm_nodes = list(map(int, as_list(shared['numa_node']))), list(map(int, as_list(vm['numa_node'])))
count, cores, mem_mb = int(vm['count']), int(vm['core_count_per_vm']), int(vm['mem_size_mb_per_vm'])
if count != 4: raise SystemExit('dsidle_init_vms is intentionally fixed to 4 VM')
if not vm_nodes or not shared_nodes: raise SystemExit('NUMA node lists must not be empty')
if int(shared['size_mb']) < 2048 or int(shared['size_mb']) & (int(shared['size_mb']) - 1): raise SystemExit('shared size_mb must be a power of two and at least 2048MB')
hwcc, swcc = shared['hwcc'], shared['swcc']
if int(hwcc['offset_mb']) != 0 or int(swcc['offset_mb']) != int(hwcc['size_mb']) or int(hwcc['size_mb']) + int(swcc['size_mb']) != int(shared['size_mb']): raise SystemExit('invalid HWCC/SWCC layout')
if int(hwcc['size_mb']) != 1024: raise SystemExit('D-SIDLE VM contract requires 1024MiB HWCC')
for node in set(shared_nodes + vm_nodes):
    if not Path(f'/sys/devices/system/node/node{node}').is_dir(): raise SystemExit(f'NUMA node {node} does not exist')
if len(shared_nodes) > 1: shared_bind = ','.join(map(str, shared_nodes))
else: shared_bind = str(shared_nodes[0])
if set(shared_nodes) & set(vm_nodes) and len(set(shared_nodes + vm_nodes)) > 1 and not allow_overlap:
    raise SystemExit('shared_memory.numa_node overlaps vm.numa_node; pass --allow-overlapping-numa to override')
online = online_cpus(Path('/sys/devices/system/cpu/online').read_text())
reserved = set(map(int, as_list(cpu['reserved_cores']))); ivshmem = set(map(int, as_list(cpu['ivshmem_server_cores']))); vm_cores = list(map(int, as_list(cpu['vm_cores'])))
if reserved & ivshmem or reserved & set(vm_cores) or ivshmem & set(vm_cores): raise SystemExit('host_cpu reserved/ivshmem/vm core sets overlap')
if not (reserved | ivshmem | set(vm_cores)) <= online: raise SystemExit('host_cpu contains an offline core')
if len(vm_cores) < count * cores: raise SystemExit('insufficient vm_cores')
for index, core in enumerate(vm_cores[:count * cores]):
    node = vm_nodes[index // cores % len(vm_nodes)]
    if core not in online_cpus(Path(f'/sys/devices/system/node/node{node}/cpulist').read_text()): raise SystemExit(f'vm core {core} is not in configured NUMA node {node}')
available_kb = int(next(line.split()[1] for line in Path('/proc/meminfo').read_text().splitlines() if line.startswith('MemAvailable:')))
if available_kb // 1024 < count * mem_mb: raise SystemExit('MemAvailable is below total VM RAM requirement')
for tool in ('qemu-system-x86_64', 'numactl', 'taskset', 'ssh'):
    if not shutil.which(tool): raise SystemExit(f'missing required tool: {tool}')
if not dry and (not Path(image).is_file() or Path(image).stat().st_size == 0): raise SystemExit(f'missing VM image: {image}')
if not dry and (not Path(pool_tool).is_file() or not os.access(pool_tool, os.X_OK)): raise SystemExit(f'missing shared-pool tool: {pool_tool}')
if apply_tuning: print('DSIDLE_VM_HOST_TUNING requested=true (no tuning keys are declared in experiment config)')
else: print('DSIDLE_VM_HOST_TUNING requested=false (check/report only)')

storage, backing = Path(vm['storage_path']), Path(shared['path'])
print(f'DSIDLE_VM_PREFLIGHT_OK config={config_path} shared_numa={shared_bind} vm_numa={",".join(map(str, vm_nodes))}')
if execute or dry:
    if not dry: backing.parent.mkdir(parents=True, exist_ok=True)
    run(['numactl', f'--membind={shared_bind}', '--', 'dd', 'if=/dev/zero', f'of={backing}', 'bs=1M', f'count={shared["size_mb"]}', 'conv=fsync', 'status=none'])
    run([pool_tool, '--init-pool', '--config', config_path, '--node-control-capacity', '2097152', '--max-threads-per-vm', str(cores)])
for index in range(count):
    vm_node = vm_nodes[index % len(vm_nodes)]
    core_slice = vm_cores[index * cores:(index + 1) * cores]
    vm_dir = storage / f'vm_{index}'
    pidfile = vm_dir / 'qemu.pid'
    disk = vm_dir / 'root.img'
    port = int(vm['ssh_base_port']) + index
    mac = f'52:54:00:1d:51:{index:02x}'
    qemu = ['numactl', f'--cpunodebind={vm_node}', f'--membind={vm_node}', '--', 'qemu-system-x86_64', '-machine', 'q35,accel=kvm,mem-merge=off', '-cpu', 'host', '-m', f'{mem_mb}M,maxmem={mem_mb}M', '-object', f'memory-backend-ram,id=vmram0,size={mem_mb}M,host-nodes={vm_node},policy=bind,prealloc=on', '-numa', 'node,nodeid=0,memdev=vmram0', '-smp', f'{cores},maxcpus={cores},sockets=1,cores={cores},threads=1', '-enable-kvm', '-display', 'none', '-daemonize', '-chardev', f'socket,id=serial0,path={vm_dir}/serial.sock,server=on,wait=off,logfile={vm_dir}/serial.log', '-serial', 'chardev:serial0', '-device', 'virtio-rng-pci', '-pidfile', str(pidfile), '-D', str(vm_dir / 'qemu.log'), '-device', 'virtio-blk-pci,packed=on,num-queues=1,drive=drive0,id=virblk0', '-drive', f'if=none,file={disk},format=raw,media=disk,id=drive0,cache=none,aio=native', '-device', f'virtio-net-pci,netdev=netssh{index},mac={mac}', '-netdev', f'user,id=netssh{index},hostfwd=tcp:127.0.0.1:{port}-:22', '-device', 'ivshmem-plain,memdev=ivshmem', '-object', f'memory-backend-file,size={shared["size_mb"]}M,share=on,mem-path={backing},id=ivshmem']
    print(f'DSIDLE_VM_QEMU vm={index} cores={",".join(map(str, core_slice))} ssh_port={port}')
    print('DSIDLE_VM_CMD ' + command_text(qemu))
    if execute and not dry:
        vm_dir.mkdir(parents=True, exist_ok=True)
        if pidfile.exists():
            pid = int(pidfile.read_text().strip())
            try: os.kill(pid, signal.SIGTERM)
            except ProcessLookupError: pass
            time.sleep(0.2)
        if not disk.exists(): shutil.copyfile(image, disk)
        subprocess.run(qemu, check=True)
        for _ in range(50):
            if pidfile.exists(): break
            time.sleep(0.1)
        if not pidfile.exists(): raise SystemExit(f'QEMU did not create pidfile: {pidfile}')
        subprocess.run(['taskset', '-apc', ','.join(map(str, core_slice)), pidfile.read_text().strip()], check=True)

print('DSIDLE_VM_INIT_DRY_RUN_OK' if dry else 'DSIDLE_VM_INIT_OK')
PY
