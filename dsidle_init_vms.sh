#!/usr/bin/env bash
set -euo pipefail
config=experiment_config.jsonc
dry_run=0
execute=0
while (($#)); do
  case "$1" in --config) config=$2; shift 2;; --dry-run) dry_run=1; shift;; --execute) execute=1; shift;; --allow-overlapping-numa) shift;; *) echo "usage: $0 [--config PATH] [--dry-run] [--allow-overlapping-numa] --execute" >&2; exit 2;; esac
done
python3 - "$config" "$dry_run" "$execute" <<'PY'
import json,re,sys,os
s=re.sub(r'//.*','',open(sys.argv[1]).read()); c=json.loads(s); dry=int(sys.argv[2]); execute=int(sys.argv[3])
sh=c['shared_memory']; vm=c['vm']; cpu=c['host_cpu']
if sh['hwcc']['offset_mb'] != 0 or sh['swcc']['offset_mb'] != sh['hwcc']['size_mb'] or sh['hwcc']['size_mb']+sh['swcc']['size_mb'] != sh['size_mb']: raise SystemExit('invalid HWCC/SWCC layout')
if sh['size_mb'] & (sh['size_mb']-1): raise SystemExit('shared size_mb must be a power of two')
if len(cpu['vm_cores']) < vm['count']*vm['core_count_per_vm']: raise SystemExit('insufficient vm_cores')
if not (dry or execute): raise SystemExit('refusing VM/backing changes without --execute (use --dry-run)')
print('DRY_RUN create backing:', sh['path'], sh['size_mb'], 'MiB')
for i in range(vm['count']):
 cores=cpu['vm_cores'][i*vm['core_count_per_vm']:(i+1)*vm['core_count_per_vm']]
 print('DRY_RUN qemu vm=%d cores=%s ssh_port=%d ivshmem=%s' % (i, ','.join(map(str,cores)), vm['ssh_base_port']+i, sh['path']))
if not dry: raise SystemExit('VM launch implementation requires image/root.img and qemu provisioning; run --dry-run for preflight')
PY
