#!/usr/bin/env python3
"""Clear host/guest page caches and evict CPU caches for VM experiments."""

import argparse
import base64
import json
import multiprocessing
import os
import shlex
import subprocess
import sys
from pathlib import Path

from jsonc_utils import load_jsonc as parse_jsonc


def load_jsonc(path: Path):
    return parse_jsonc(path)


def sweep_worker(cpu: int, size_mb: int) -> None:
    try:
        os.sched_setaffinity(0, {cpu})
    except (AttributeError, OSError):
        pass
    data = bytearray(size_mb * 1024 * 1024)
    checksum = 0
    for offset in range(0, len(data), 64):
        data[offset] = (offset >> 6) & 0xFF
    for offset in range(0, len(data), 64):
        checksum ^= data[offset]
    if checksum < 0:
        raise RuntimeError("unreachable checksum")


def sweep_cpu_caches(workers: int, size_mb: int) -> None:
    cpus = sorted(os.sched_getaffinity(0)) if hasattr(os, "sched_getaffinity") else list(range(os.cpu_count() or 1))
    if not cpus:
        cpus = [0]
    processes = [
        multiprocessing.Process(
            target=sweep_worker, args=(cpus[index % len(cpus)], size_mb)
        )
        for index in range(min(workers, len(cpus)))
    ]
    for process in processes:
        process.start()
    for process in processes:
        process.join()
        if process.exitcode:
            raise RuntimeError(f"CPU cache sweep worker exited {process.exitcode}")


def drop_local_page_cache() -> None:
    if os.geteuid() == 0:
        os.sync()
        Path("/proc/sys/vm/drop_caches").write_text("3\n")
        return
    subprocess.run(
        ["sudo", "-n", "sh", "-c", "sync; echo 3 > /proc/sys/vm/drop_caches"],
        check=True,
    )


REMOTE_PROGRAM = r"""
import multiprocessing
import os
import sys
from pathlib import Path

def worker(cpu, size_mb):
    try:
        os.sched_setaffinity(0, {cpu})
    except (AttributeError, OSError):
        pass
    data = bytearray(size_mb * 1024 * 1024)
    checksum = 0
    for offset in range(0, len(data), 64):
        data[offset] = (offset >> 6) & 255
    for offset in range(0, len(data), 64):
        checksum ^= data[offset]
    if checksum < 0:
        raise RuntimeError("unreachable checksum")

workers = int(sys.argv[1])
size_mb = int(sys.argv[2])
os.sync()
Path("/proc/sys/vm/drop_caches").write_text("3\n")
cpus = sorted(os.sched_getaffinity(0)) if hasattr(os, "sched_getaffinity") else list(range(os.cpu_count() or 1))
if not cpus:
    cpus = [0]
processes = [
    multiprocessing.Process(target=worker, args=(cpus[index % len(cpus)], size_mb))
    for index in range(min(workers, len(cpus)))
]
for process in processes:
    process.start()
for process in processes:
    process.join()
    if process.exitcode:
        raise SystemExit(f"CPU cache sweep worker exited {process.exitcode}")
"""


def clear_vms(config, workers: int, size_mb: int, dry_run: bool) -> None:
    vm = config["vm"]
    encoded = base64.b64encode(REMOTE_PROGRAM.encode()).decode()
    remote = (
        f"python3 -c {shlex.quote(f'import base64;exec(base64.b64decode({encoded!r}))')} "
        f"{workers} {size_mb}"
    )
    base = int(vm["ssh_base_port"])
    commands = []
    for node in range(int(vm["count"])):
        command = [
            "ssh",
            "-o",
            "BatchMode=yes",
            "-o",
            "UserKnownHostsFile=/dev/null",
            "-o",
            "StrictHostKeyChecking=no",
            "-o",
            "ConnectTimeout=10",
            "-p",
            str(base + node),
            "root@127.0.0.1",
            remote,
        ]
        if dry_run:
            print(f"DSIDLE_CACHE_CLEAR_DRY_RUN target=vm{node} workers={workers} sweep_mb={size_mb}")
        else:
            commands.append((node, subprocess.Popen(command)))
    failures = []
    for node, process in commands:
        if process.wait():
            failures.append(node)
    if failures:
        raise SystemExit(f"cache clear failed on VMs {failures}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--config", required=True, type=Path)
    parser.add_argument("--target", choices=("local", "vms", "all"), default="all")
    parser.add_argument("--cpu-workers", type=int, default=4)
    parser.add_argument("--cpu-sweep-mb", type=int, default=64)
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()
    if args.cpu_workers <= 0 or args.cpu_sweep_mb <= 0:
        parser.error("CPU sweep parameters must be positive")
    config = load_jsonc(args.config)
    if args.target in ("local", "all"):
        if args.dry_run:
            print(
                "DSIDLE_CACHE_CLEAR_DRY_RUN "
                f"target=host workers={args.cpu_workers} sweep_mb={args.cpu_sweep_mb}"
            )
        else:
            drop_local_page_cache()
            sweep_cpu_caches(args.cpu_workers, args.cpu_sweep_mb)
    if args.target in ("vms", "all"):
        clear_vms(config, args.cpu_workers, args.cpu_sweep_mb, args.dry_run)
    if not args.dry_run:
        print(
            "DSIDLE_CACHE_CLEAR_OK "
            f"target={args.target} workers={args.cpu_workers} "
            f"sweep_mb={args.cpu_sweep_mb}"
        )
    return 0


if __name__ == "__main__":
    sys.exit(main())
