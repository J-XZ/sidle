#!/usr/bin/env bash
set -euo pipefail
config=experiment_config.jsonc
dry_run=0
while (($#)); do case "$1" in --config) config=$2; shift 2;; --dry-run) dry_run=1; shift;; *) echo "usage: $0 [--config PATH] [--dry-run]" >&2; exit 2;; esac; done
python3 - "$config" "$dry_run" <<'PY'
import json,re,sys,os
s=re.sub(r'//.*','',open(sys.argv[1]).read()); c=json.loads(s); dry=int(sys.argv[2])
vm=c['vm']; shared=c['shared_memory']; storage=vm['storage_path']
for i in range(vm['count']):
 p=f'{storage}/vm_{i}/qemu.pid'
 if dry: print(f'DRY_RUN verify pid={p}, ssh=127.0.0.1:{vm["ssh_base_port"]+i}, device={shared["device_path"]}')
 elif not os.path.isfile(p): raise SystemExit(f'missing pid file: {p}')
print('DSIDLE_VM_CHECK_OK' if not dry else 'DSIDLE_VM_CHECK_DRY_RUN_OK')
PY
