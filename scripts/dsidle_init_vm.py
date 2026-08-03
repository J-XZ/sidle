#!/usr/bin/env python3
"""D-SIDLE VM init aligned with D-SIDLE init_scripts_env_3_init_vm.fish + rust init_vm.

Prepares the D-SIDLE guest runtime from this repository's own inputs:
  host tuning → validate → kill old VMs → tmpfs mpol=bind on shared NUMA →
  ivshmem-plain file → dsidle_shared_pool --init-pool → bridge/tap →
  prepare root.img (network + SSH) → QEMU (dual NIC + ivshmem-plain) →
  wait SSH → guest-build ivpci module → verify BAR2 /dev/ivpci0 → taskset QEMU.
"""
from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import signal
import socket
import subprocess
import sys
import time
from pathlib import Path
from typing import Iterable, List, Sequence, Set


def load_jsonc(path: Path) -> dict:
    text = path.read_text()
    out, in_string, escaped, i = [], False, False, 0
    while i < len(text):
        char = text[i]
        nxt = text[i + 1] if i + 1 < len(text) else ""
        if in_string:
            out.append(char)
            if escaped:
                escaped = False
            elif char == "\\":
                escaped = True
            elif char == '"':
                in_string = False
            i += 1
        elif char == '"':
            in_string = True
            out.append(char)
            i += 1
        elif char == "/" and nxt == "/":
            i = text.find("\n", i)
            if i < 0:
                break
        else:
            out.append(char)
            i += 1
    return json.loads("".join(out))


def as_list(value) -> list:
    return value if isinstance(value, list) else [value]


def online_cpus(text: str) -> Set[int]:
    result: Set[int] = set()
    for item in text.strip().split(","):
        if not item:
            continue
        first, *last = item.split("-")
        end = int(last[0]) if last else int(first)
        result.update(range(int(first), end + 1))
    return result


def shlex_quote(text: str) -> str:
    if re.search(r"[^A-Za-z0-9_@%+=:,./-]", text):
        return "'" + text.replace("'", "'\\''") + "'"
    return text


def command_text(command: Sequence[str]) -> str:
    return " ".join(shlex_quote(str(item)) for item in command)


class Runner:
    def __init__(self, dry: bool):
        self.dry = dry

    def run(self, command: Sequence[str], *, check: bool = True, capture: bool = False,
            input_text: str | None = None, env: dict | None = None) -> subprocess.CompletedProcess:
        print("DSIDLE_VM_CMD " + command_text(command), flush=True)
        if self.dry:
            return subprocess.CompletedProcess(command, 0, stdout="", stderr="")
        kwargs = {"check": check, "text": True, "env": env}
        if capture:
            kwargs["capture_output"] = True
        if input_text is not None:
            kwargs["input"] = input_text
        return subprocess.run(list(command), **kwargs)

    def shell(self, command: str, *, check: bool = True) -> subprocess.CompletedProcess:
        print("DSIDLE_VM_CMD " + command, flush=True)
        if self.dry:
            return subprocess.CompletedProcess(["bash", "-c", command], 0)
        return subprocess.run(["bash", "-c", command], check=check, text=True)


def cpu_is_online(cpu: int) -> bool:
    path = Path(f"/sys/devices/system/cpu/cpu{cpu}/online")
    if path.exists():
        return path.read_text().strip() == "1"
    return Path(f"/sys/devices/system/cpu/cpu{cpu}").is_dir()


def cpu_numa_node(cpu: int) -> int:
    cpu_path = Path(f"/sys/devices/system/cpu/cpu{cpu}")
    for link in cpu_path.glob("node*"):
        return int(link.name.removeprefix("node"))
    raise SystemExit(f"cannot determine NUMA node for CPU {cpu}")


def host_tuning_specs():
    yield "nmi_watchdog", Path("/proc/sys/kernel/nmi_watchdog"), "0", {"0"}
    yield "aslr", Path("/proc/sys/kernel/randomize_va_space"), "0", {"0"}
    yield "ksm", Path("/sys/kernel/mm/ksm/run"), "0", {"0"}
    yield "numa_balancing", Path("/proc/sys/kernel/numa_balancing"), "0", {"0"}
    yield (
        "transparent_hugepages",
        Path("/sys/kernel/mm/transparent_hugepage/enabled"),
        "never",
        {"never"},
    )
    yield (
        "smt",
        Path("/sys/devices/system/cpu/smt/control"),
        "off",
        {"off", "forceoff", "notsupported"},
    )
    yield (
        "intel_turbo",
        Path("/sys/devices/system/cpu/intel_pstate/no_turbo"),
        "1",
        {"1"},
    )
    yield (
        "amd_boost",
        Path("/sys/devices/system/cpu/cpufreq/boost"),
        "0",
        {"0"},
    )
    for governor_path in sorted(
        Path("/sys/devices/system/cpu").glob("cpu*/cpufreq/scaling_governor")
    ):
        match = re.search(r"/cpu(\d+)/", str(governor_path))
        if match and not cpu_is_online(int(match.group(1))):
            continue
        yield "performance_governor", governor_path, "performance", {"performance"}


def selected_host_tunable_value(path: Path) -> str:
    value = path.read_text().strip()
    selected = re.search(r"\[([^\]]+)\]", value)
    return selected.group(1) if selected else value


def check_host_perf_tuning() -> None:
    """Report host performance state without mutating it."""
    for label, path, expected, accepted in host_tuning_specs():
        if not path.exists():
            print(
                f"DSIDLE_HOST_TUNING_CHECK label={label} path={path} "
                f"expected={expected} actual=unavailable status=unavailable"
            )
            continue
        actual = selected_host_tunable_value(path)
        status = "ok" if actual in accepted else "mismatch"
        print(
            f"DSIDLE_HOST_TUNING_CHECK label={label} path={path} "
            f"expected={expected} actual={actual} status={status}"
        )


def write_host_tunable(label: str, path: Path, value: str, runner: Runner) -> None:
    if not path.exists():
        print(f"[init_vm] host tuning skip {label}: {path} not found")
        return
    print(f"[init_vm] host tuning {label}: {path} <- {value}")
    if runner.dry:
        return
    if os.geteuid() == 0:
        path.write_text(value + "\n")
    else:
        runner.run(["sudo", "tee", str(path)], input_text=value + "\n", capture=True)


def apply_host_perf_tuning(runner: Runner) -> None:
    print("[init_vm] applying explicitly authorized host performance tuning after preflight")
    for label, path, expected, _ in host_tuning_specs():
        write_host_tunable(label, path, expected, runner)


def validate_numa_separation(
    shared_nodes: Sequence[int], vm_nodes: Sequence[int], allow_overlap: bool
) -> None:
    overlap = sorted(set(shared_nodes) & set(vm_nodes))
    if not overlap:
        return
    if not allow_overlap:
        raise SystemExit(
            "shared_memory.numa_node and vm.numa_node overlap on "
            f"{overlap}; performance runs require disjoint NUMA nodes "
            "(use --allow-overlapping-numa only for declared functional runs)"
        )
    print(
        "[init_vm] warning: explicitly allowed shared/VM NUMA overlap "
        f"{overlap}; this run is functional-only and not comparable",
        file=sys.stderr,
    )


def validate_host_cpu_topology(
    vm_nodes: Sequence[int],
    count: int,
    cores: int,
    reserved: Sequence[int],
    vm_cores: Sequence[int],
) -> None:
    online = online_cpus(Path("/sys/devices/system/cpu/online").read_text())
    role_pairs = []
    for core in reserved:
        if core not in online or not cpu_is_online(core):
            raise SystemExit(f"host_cpu.reserved_cores CPU {core} invalid/offline")
        role_pairs.append(("reserved", core))
    if len(vm_cores) < count * cores:
        raise SystemExit("insufficient vm_cores")
    used = vm_cores[: count * cores]
    for slot, core in enumerate(used):
        if core not in online or not cpu_is_online(core):
            raise SystemExit(f"host_cpu.vm_cores[{slot}]={core} invalid/offline")
        expected = vm_nodes[(slot // cores) % len(vm_nodes)]
        actual = cpu_numa_node(core)
        if actual != expected:
            raise SystemExit(
                f"host_cpu.vm_cores[{slot}]={core} is on NUMA {actual}, "
                f"expected {expected}"
            )
        role_pairs.append((f"vm[{slot}]", core))
    seen = {}
    for role, core in role_pairs:
        if core in seen:
            raise SystemExit(
                f"host CPU role overlap: {seen[core]} and {role} both use CPU {core}"
            )
        seen[core] = role


def kill_existing_vms(storage: Path, runner: Runner) -> None:
    print("[init_vm] stopping existing qemu/ivshmem processes before re-init")
    for pid_file in storage.glob("vm_*/qemu.pid"):
        try:
            pid = int(pid_file.read_text().strip())
        except ValueError:
            continue
        if runner.dry:
            print(f"DSIDLE_VM_CMD kill -9 {pid}")
            continue
        try:
            os.kill(pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
    for name in ("qemu-system-x86", "qemu-system-x86_64", "ivshmem-server"):
        runner.shell(f"pgrep -x -- {shlex_quote(name)} | xargs -r kill -9 || true", check=False)
    if not runner.dry:
        time.sleep(1)
    for name in ("qemu-system-x86", "qemu-system-x86_64", "ivshmem-server"):
        runner.shell(f"pgrep -x -- {shlex_quote(name)} | xargs -r kill -9 || true", check=False)


def setup_shared_memory_tmpfs(shared_path: Path, size_mb: int, shared_nodes: Sequence[int], runner: Runner) -> None:
    """D-SIDLE setup_shared_memory: mount tmpfs with mpol=bind on shared NUMA."""
    if shared_path.is_file():
        raise SystemExit(f"shared memory path is a file, expected a directory: {shared_path}")
    if not runner.dry:
        shared_path.mkdir(parents=True, exist_ok=True)
    if runner.dry:
        numa_csv = ",".join(str(n) for n in shared_nodes)
        mount_size_mb = size_mb + 100
        print(
            f"DSIDLE_VM_CMD sudo mount -t tmpfs -o size={mount_size_mb}M,mpol=bind:{numa_csv},rw,nosuid,nodev "
            f"tmpfs {shared_path}"
        )
        print(f"[init_vm] mounted tmpfs size={mount_size_mb}M mpol=bind:{numa_csv} at {shared_path}")
        return
    mp = runner.run(["mountpoint", "-q", "--", str(shared_path)], check=False)
    if mp.returncode == 0:
        print(f"[init_vm] shared memory path already mounted, unmounting: {shared_path}")
        runner.run(["sudo", "umount", "--", str(shared_path)])
        mp2 = runner.run(["mountpoint", "-q", "--", str(shared_path)], check=False)
        if mp2.returncode == 0 and not runner.dry:
            raise SystemExit(f"shared memory path remains mounted after unmount: {shared_path}")
    elif mp.returncode not in (0, 32) and not runner.dry:
        # util-linux returns 32 for ordinary directory
        if mp.returncode != 32:
            raise SystemExit(f"failed to inspect shared memory mountpoint: {shared_path}")
    mount_size_mb = size_mb + 100
    numa_csv = ",".join(str(n) for n in shared_nodes)
    opts = f"size={mount_size_mb}M,mpol=bind:{numa_csv},rw,nosuid,nodev"
    runner.run(["sudo", "mount", "-t", "tmpfs", "-o", opts, "tmpfs", str(shared_path)])
    if not runner.dry:
        fstype = subprocess.check_output(
            ["findmnt", "-n", "-T", str(shared_path), "-o", "FSTYPE"], text=True
        ).strip()
        if fstype != "tmpfs":
            raise SystemExit(f"shared memory path is not a tmpfs after mount: {shared_path}")
    print(f"[init_vm] mounted tmpfs size={mount_size_mb}M mpol=bind:{numa_csv} at {shared_path}")


def prepare_plain_ivshmem_file(backing: Path, size_mb: int, runner: Runner) -> None:
    if not runner.dry and backing.exists():
        backing.unlink()
    if runner.dry:
        print(f"DSIDLE_VM_CMD truncate -s {size_mb}M {backing}")
        return
    backing.parent.mkdir(parents=True, exist_ok=True)
    with open(backing, "wb") as handle:
        handle.truncate(size_mb * 1024 * 1024)
    print(f"[init_vm] prepared ivshmem-plain backing: {backing} size={size_mb}M")


def setup_bridge_tap_network(bridge_tap_ip: str, vm_count: int, runner: Runner) -> None:
    bridge_name = "br_xz"
    print(f"[init_vm] setting up bridge tap network ip={bridge_tap_ip} name={bridge_name}")
    runner.shell("sudo modprobe tun tap")
    runner.run(["sudo", "sysctl", "-w", "net.ipv4.ip_forward=1"])
    runner.shell(f"sudo ip link add name {bridge_name} type bridge || true", check=False)
    runner.run(["sudo", "ip", "link", "set", bridge_name, "up"])
    runner.run(["sudo", "ip", "addr", "flush", "dev", bridge_name])
    runner.run(["sudo", "ip", "addr", "add", f"{bridge_tap_ip}/24", "dev", bridge_name])
    for index in range(vm_count):
        tap = f"tap_xz_{index}"
        runner.shell(f"sudo ip link delete {tap} || true", check=False)
        runner.run(["sudo", "ip", "tuntap", "add", "dev", tap, "mode", "tap"])
        runner.run(["sudo", "ip", "link", "set", tap, "up"])
        runner.run(["sudo", "ip", "link", "set", tap, "master", bridge_name])


def vm_bridge_mac(index: int) -> str:
    return f"de:ad:be:ef:10:{index & 0xff:02x}"


def vm_user_ssh_mac(index: int) -> str:
    return f"de:ad:be:ef:20:{index & 0xff:02x}"


def vm_ip(first_ip: str, index: int) -> str:
    octets = [int(part) for part in first_ip.split(".")]
    octets[3] += index
    return ".".join(str(part) for part in octets)


def detect_cpu_model() -> str:
    try:
        text = Path("/proc/cpuinfo").read_text()
    except OSError:
        return "host"
    if "EPYC" in text:
        return "EPYC,topoext"
    return "host"


def prepare_vm_disk(
    repo_root: Path,
    image: Path,
    vm_dir: Path,
    index: int,
    first_ip: str,
    bridge_tap_ip: str,
    ssh_pub_key: str,
    copy_root_img: bool,
    assets: Path,
    runner: Runner,
) -> None:
    disk = vm_dir / "root.img"
    mnt = vm_dir / "mnt"
    if not runner.dry:
        vm_dir.mkdir(parents=True, exist_ok=True)
    need_copy = copy_root_img or not disk.exists()
    if need_copy:
        runner.shell(
            f"cp --reflink=auto --sparse=always {shlex_quote(str(image))} {shlex_quote(str(disk))}"
        )
    # Network + SSH injection via guestmount (D-SIDLE config_vm_files).
    hosts_template = (assets / "etc_hosts_template").read_text() if (assets / "etc_hosts_template").exists() else "@ADDR@\n"
    hosts_content = hosts_template.replace("@ADDR@", vm_ip(first_ip, index))
    net_bridge = (
        f"[Match]\nMACAddress={vm_bridge_mac(index)}\n\n"
        f"[Network]\nAddress={vm_ip(first_ip, index)}/24\nGateway={bridge_tap_ip}\n"
    )
    net_user = (
        f"[Match]\nMACAddress={vm_user_ssh_mac(index)}\n\n"
        f"[Network]\nDHCP=yes\n"
    )
    if runner.dry:
        print(f"[init_vm] dry-run guestmount configure vm_{index}")
        return
    runner.shell(
        f"if mountpoint -q {shlex_quote(str(mnt))}; then sudo umount {shlex_quote(str(mnt))}; fi; "
        f"sudo rm -rf {shlex_quote(str(mnt))}; mkdir -p {shlex_quote(str(mnt))}"
    )
    runner.run(["sudo", "guestmount", "-a", str(disk), "-i", str(mnt)])
    try:
        net_dir = mnt / "etc/systemd/network"
        etc_dir = mnt / "etc"
        runner.run(["sudo", "mkdir", "-p", str(net_dir)])
        for name, content in (
            ("20-wired.network", net_bridge),
            ("30-wired.network", net_user),
        ):
            tmp = vm_dir / name
            tmp.write_text(content)
            runner.run(["sudo", "cp", str(tmp), str(net_dir / name)])
            tmp.unlink(missing_ok=True)
        runner.shell(
            f"sudo chown root:root {shlex_quote(str(net_dir))}/*.network; "
            f"sudo chmod 0644 {shlex_quote(str(net_dir))}/*.network"
        )
        hosts_tmp = vm_dir / "hosts"
        hosts_tmp.write_text(hosts_content)
        runner.run(["sudo", "cp", str(hosts_tmp), str(etc_dir / "hosts")])
        hosts_tmp.unlink(missing_ok=True)
        gai = assets / "gai.conf"
        if gai.exists():
            runner.run(["sudo", "cp", str(gai), str(etc_dir / "gai.conf")])
        key = ssh_pub_key.strip()
        if key:
            auth = mnt / "root/.ssh/authorized_keys"
            runner.run(["sudo", "mkdir", "-p", str(auth.parent)])
            runner.shell(
                f"sudo touch {shlex_quote(str(auth))}; sudo chmod 600 {shlex_quote(str(auth))}; "
                f"sudo grep -Fxq {shlex_quote(key)} {shlex_quote(str(auth))} || "
                f"printf '%s\\n' {shlex_quote(key)} | sudo tee -a {shlex_quote(str(auth))} >/dev/null"
            )
    finally:
        # D-SIDLE config_vm_files: umount must succeed (do not swallow failures).
        runner.shell(f"sync; sudo umount {shlex_quote(str(mnt))}")


def tcp_port_open(port: int, timeout_sec: float = 0.2) -> bool:
    try:
        with socket.create_connection(("127.0.0.1", port), timeout=timeout_sec):
            return True
    except OSError:
        return False


def wait_for_ssh_ports_free(ports: Sequence[int], timeout_sec: float = 10.0) -> None:
    """D-SIDLE wait_for_ssh_ports_free: refuse to start QEMU while hostfwd ports are busy."""
    deadline = time.time() + timeout_sec
    while True:
        busy = [port for port in ports if tcp_port_open(port)]
        if not busy:
            return
        if time.time() >= deadline:
            raise SystemExit(
                "VM SSH port(s) still occupied before QEMU start: "
                f"{busy}. Stop the process using these host ports or adjust vm.ssh_base_port."
            )
        print(f"[init_vm] waiting for old VM SSH hostfwd port(s) to close: {busy}")
        time.sleep(1)


def assert_shared_path_is_tmpfs(shared_path: Path) -> None:
    """D-SIDLE check_env_arg / post-mount FSTYPE recheck."""
    fstype = subprocess.check_output(
        ["findmnt", "-n", "-T", str(shared_path), "-o", "FSTYPE"], text=True
    ).strip()
    if fstype != "tmpfs":
        raise SystemExit(f"shared memory path is not a tmpfs: {shared_path} (FSTYPE={fstype or 'unknown'})")


def reclaimable_old_qemu_rss_mb(storage: Path) -> int:
    total = 0
    for pid_file in storage.glob("vm_*/qemu.pid"):
        try:
            pid = int(pid_file.read_text().strip())
        except ValueError:
            continue
        status = Path(f"/proc/{pid}/status")
        if not status.is_file():
            continue
        for line in status.read_text().splitlines():
            if line.startswith("VmRSS:"):
                total += int(line.split()[1]) // 1024
                break
    return total


def require_host_mem_for_vms(storage: Path, count: int, mem_mb: int) -> None:
    """D-SIDLE MemAvailable gate including reclaimable old QEMU RSS."""
    required = count * mem_mb
    available_mb = int(
        next(
            line.split()[1]
            for line in Path("/proc/meminfo").read_text().splitlines()
            if line.startswith("MemAvailable:")
        )
    ) // 1024
    reclaimable = reclaimable_old_qemu_rss_mb(storage)
    effective = available_mb + reclaimable
    if required > effective:
        raise SystemExit(
            "host available memory is too small for experiment_config.jsonc: "
            f"vm.count={count} vm.mem_size_mb_per_vm={mem_mb} needs {required} MiB for VM RAM, "
            f"but MemAvailable is {available_mb} MiB and old VM QEMU RSS reclaimable by this "
            f"script is {reclaimable} MiB"
        )


def ssh_opts(port: int) -> List[str]:
    return [
        "ssh",
        "-o", "BatchMode=yes",
        "-o", "UserKnownHostsFile=/dev/null",
        "-o", "StrictHostKeyChecking=no",
        "-o", "ConnectTimeout=5",
        "-p", str(port),
        "root@127.0.0.1",
    ]


def ssh_cmd(port: int, remote: str) -> List[str]:
    """Pass remote command as one argv so OpenSSH does not re-split it."""
    return ssh_opts(port) + [remote]


def wait_ssh(port: int, pidfile: Path, runner: Runner, timeout_sec: int = 180) -> None:
    if runner.dry:
        print(f"[init_vm] dry-run wait SSH port={port}")
        return
    deadline = time.time() + timeout_sec
    while time.time() < deadline:
        if pidfile.exists():
            try:
                pid = int(pidfile.read_text().strip())
                os.kill(pid, 0)
            except (ValueError, ProcessLookupError, OSError):
                raise SystemExit(f"QEMU died while waiting for SSH on port {port}")
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.5):
                pass
            result = subprocess.run(
                ssh_cmd(port, "true"),
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
            if result.returncode == 0:
                print(f"[init_vm] SSH ready on port {port}")
                return
        except OSError:
            pass
        time.sleep(1)
    raise SystemExit(f"VM did not become reachable by SSH on port {port}")


def prepare_kernel_module(repo_root: Path, ports: Sequence[int], runner: Runner) -> None:
    """D-SIDLE prepare_kernel_module: rsync sources, guest make, modprobe."""
    module_dir = repo_root / "third_party" / "ivshmem-kernel"
    if not (module_dir / "ivshmem_driver.c").exists():
        raise SystemExit(f"missing ivshmem kernel sources: {module_dir}")
    install = (
        "cd /ivshmem-kernel && "
        "cp ./ivshmem_driver.ko /lib/modules/$(uname -r)/kernel/drivers/misc/ && "
        "depmod -a && modprobe ivshmem_driver"
    )
    for port in ports:
        print(f"[init_vm] sync/build/load ivpci on ssh port {port}")
        runner.run(ssh_cmd(port, "mkdir -p /ivshmem-kernel"))
        rsync = [
            "rsync", "-a", "--delete",
            "-e", f"ssh -o BatchMode=yes -o UserKnownHostsFile=/dev/null -o StrictHostKeyChecking=no -o ConnectTimeout=10 -p {port}",
            f"{module_dir}/",
            f"root@127.0.0.1:/ivshmem-kernel/",
        ]
        # Exclude host-built objects so guest rebuilds cleanly for guest kernel.
        rsync[2:2] = ["--exclude=*.o", "--exclude=*.ko", "--exclude=*.mod", "--exclude=*.mod.c",
                       "--exclude=Module.symvers", "--exclude=modules.order", "--exclude=.*.cmd"]
        runner.run(rsync)
        runner.run(ssh_cmd(port, "cd /ivshmem-kernel && make clean && make"), check=True)
        runner.run(ssh_cmd(port, install), check=True)


def check_ivshmem_device(ports: Sequence[int], size_mb: int, device_path: str, runner: Runner) -> None:
    expected = size_mb * 1024 * 1024
    for port in ports:
        print(f"[init_vm] verify ivshmem device on port {port}")
        if runner.dry:
            continue
        lspci = subprocess.check_output(
            ssh_cmd(port, "lspci | grep -i 'shared memory' || true"),
            text=True,
        )
        if "Inter-VM shared memory" not in lspci and "shared memory" not in lspci.lower():
            raise SystemExit(f"ivshmem PCI device not found on port {port}: {lspci!r}")
        pci_addr = lspci.strip().split()[0]
        resource = subprocess.check_output(
            ssh_cmd(port, f"cat /sys/bus/pci/devices/0000:{pci_addr}/resource"),
            text=True,
        )
        lines = [line for line in resource.splitlines() if line.strip()]
        if len(lines) < 3:
            raise SystemExit(f"unexpected PCI resource layout on port {port}")
        parts = lines[2].split()
        start = int(parts[0], 16)
        end = int(parts[1], 16)
        bar2 = end - start + 1
        if bar2 < expected:
            raise SystemExit(
                f"BAR2 size {bar2} < configured shared_memory.size_mb={size_mb}MiB on port {port}"
            )
        driver = subprocess.check_output(
            ssh_cmd(
                port,
                f"basename $(readlink -f /sys/bus/pci/devices/0000:{pci_addr}/driver)",
            ),
            text=True,
        ).strip()
        if driver != "ivpci":
            raise SystemExit(f"expected ivpci driver, got {driver!r} on port {port}")
        subprocess.run(ssh_cmd(port, f"test -c {shlex_quote(device_path)}"), check=True)
        print(f"[init_vm] ivshmem OK port={port} BAR2={bar2} driver=ivpci device={device_path}")


def launch_qemu(
    index: int,
    vm_node: int,
    cores: int,
    mem_mb: int,
    shared_size_mb: int,
    storage: Path,
    backing: Path,
    ssh_port: int,
    runner: Runner,
) -> List[str]:
    vm_dir = storage / f"vm_{index}"
    pidfile = vm_dir / "qemu.pid"
    disk = vm_dir / "root.img"
    cpu_model = detect_cpu_model()
    guest_numa_cpu = []
    for core_id in range(cores):
        guest_numa_cpu.extend(
            ["-numa", f"cpu,node-id=0,socket-id=0,core-id={core_id},thread-id=0"]
        )
    qemu = [
        "numactl", f"--cpunodebind={vm_node}", f"--membind={vm_node}", "--",
        "qemu-system-x86_64",
        "-machine", "q35,accel=kvm,mem-merge=off",
        "-cpu", cpu_model,
        "-D", str(vm_dir / "qemu.log"),
        "-m", f"{mem_mb}M,maxmem={mem_mb}M",
        "-object", f"memory-backend-ram,id=vmram0,size={mem_mb}M,host-nodes={vm_node},policy=bind,prealloc=on",
        "-numa", "node,nodeid=0,memdev=vmram0",
        *guest_numa_cpu,
        "-numa", "dist,src=0,dst=0,val=10",
        "-smp", f"{cores},maxcpus={cores},sockets=1,cores={cores},threads=1",
        "-enable-kvm",
        "-display", "none",
        "-chardev", f"socket,id=serial0,path={vm_dir}/serial.sock,server=on,wait=off,logfile={vm_dir}/serial.log",
        "-serial", "chardev:serial0",
        "-daemonize",
        "-device", "virtio-rng-pci",
        "-pidfile", str(pidfile),
        "-device", "virtio-blk-pci,packed=on,num-queues=1,drive=drive0,id=virblk0",
        "-drive", f"if=none,file={disk},format=raw,media=disk,id=drive0,cache=none,aio=native",
        "-device", f"virtio-net-pci,mq=on,packed=on,netdev=network{index},mac={vm_bridge_mac(index)}",
        "-netdev", f"tap,id=network{index},vhost=on,ifname=tap_xz_{index},script=no,downscript=no",
        "-device", f"virtio-net-pci,netdev=netssh{index},mac={vm_user_ssh_mac(index)}",
        "-netdev", f"user,id=netssh{index},hostfwd=tcp:127.0.0.1:{ssh_port}-:22",
        "-device", "ivshmem-plain,memdev=ivshmem",
        "-object", f"memory-backend-file,size={shared_size_mb}M,share=on,mem-path={backing},id=ivshmem",
    ]
    print(f"DSIDLE_VM_QEMU vm={index} numa={vm_node} ssh_port={ssh_port}")
    if not runner.dry:
        for stale in (vm_dir / "qemu.log", vm_dir / "serial.log", vm_dir / "serial.sock", pidfile):
            stale.unlink(missing_ok=True)
    runner.run(qemu)
    return qemu


def pin_pid_to_cpuset(label: str, pid: int, cpuset: str, runner: Runner) -> None:
    print(f"[init_vm] pin {label} pid={pid} threads to host CPUs {cpuset}")
    runner.run(["taskset", "-apc", cpuset, str(pid)])


def main() -> int:
    parser = argparse.ArgumentParser(description="D-SIDLE VM init (D-SIDLE-aligned)")
    parser.add_argument("--config", required=True)
    parser.add_argument("--image", required=True)
    parser.add_argument("--pool-tool", required=True)
    parser.add_argument("--repo-root", required=True)
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--execute", action="store_true")
    parser.add_argument("--allow-overlapping-numa", action="store_true",
                        help="Allow shared/VM NUMA overlap for declared functional-only runs.")
    parser.add_argument("--apply-host-tuning", action="store_true",
                        help="Apply host tuning after all non-mutating preflight checks pass.")
    parser.add_argument("--no-host-tuning", action="store_true",
                        help="Skip even the default read-only host tuning report.")
    parser.add_argument("--skip-guestmount", action="store_true",
                        help="Skip guestmount disk injection (debug only).")
    args = parser.parse_args()
    if not args.dry_run and not args.execute:
        print("refusing VM/backing changes without --execute (use --dry-run)", file=sys.stderr)
        return 2
    if args.apply_host_tuning and args.no_host_tuning:
        print(
            "--apply-host-tuning and --no-host-tuning are mutually exclusive",
            file=sys.stderr,
        )
        return 2

    dry = bool(args.dry_run)
    runner = Runner(dry=dry)
    repo_root = Path(args.repo_root).resolve()
    config_path = Path(args.config).resolve()
    image = Path(args.image).resolve()
    pool_tool = Path(args.pool_tool).resolve()
    assets = repo_root / "scripts" / "dsidle_vm_assets"

    cfg = load_jsonc(config_path)
    shared, vm, cpu = cfg["shared_memory"], cfg["vm"], cfg["host_cpu"]
    shared_nodes = list(map(int, as_list(shared["numa_node"])))
    vm_nodes = list(map(int, as_list(vm["numa_node"])))
    count = int(vm["count"])
    cores = int(vm["core_count_per_vm"])
    mem_mb = int(vm["mem_size_mb_per_vm"])
    size_mb = int(shared["size_mb"])
    storage = Path(vm["storage_path"])
    shared_dir = Path(shared["path"])
    if shared_dir.is_file():
        raise SystemExit("shared_memory.path must be a directory (D-SIDLE contract)")
    backing = shared_dir / "ivshmem_shared_mem"
    device_path = str(shared["device_path"])
    ssh_base = int(vm["ssh_base_port"])
    first_ip = str(vm["first_ip"])
    bridge_tap_ip = str(vm["bridge_tap_ip"])
    ssh_pub = str(vm.get("local_ssh_pub_key", ""))
    reserved = list(map(int, as_list(cpu["reserved_cores"])))
    ivshmem_cores = list(map(int, as_list(cpu["ivshmem_server_cores"])))
    vm_cores = list(map(int, as_list(cpu["vm_cores"])))
    print(
        "[init_vm] host_cpu.ivshmem_server_cores is a D-SIDLE-compatible "
        f"ignored field for ivshmem-plain: {ivshmem_cores}"
    )

    if count < 1:
        raise SystemExit("vm.count must be >= 1")
    if not shared_nodes or not vm_nodes:
        raise SystemExit("NUMA node lists must not be empty")
    if size_mb < 2048 or size_mb & (size_mb - 1):
        raise SystemExit("shared size_mb must be a power of two and at least 2048MB")
    hwcc, swcc = shared["hwcc"], shared["swcc"]
    if int(hwcc["offset_mb"]) != 0 or int(swcc["offset_mb"]) != int(hwcc["size_mb"]) or int(hwcc["size_mb"]) + int(swcc["size_mb"]) != size_mb:
        raise SystemExit("invalid HWCC/SWCC layout")
    if int(hwcc["size_mb"]) != 1024:
        raise SystemExit("D-SIDLE VM contract requires 1024MiB HWCC")

    for node in set(shared_nodes + vm_nodes):
        if not Path(f"/sys/devices/system/node/node{node}").is_dir():
            raise SystemExit(f"NUMA node {node} does not exist")
    if not any(Path("/sys/devices/system/node").glob("node[0-9]*")):
        raise SystemExit("host has no NUMA node* entries under /sys/devices/system/node")
    validate_numa_separation(
        shared_nodes, vm_nodes, args.allow_overlapping_numa
    )
    topology_args = (vm_nodes, count, cores, reserved, vm_cores)
    validate_host_cpu_topology(*topology_args)
    if not args.no_host_tuning:
        check_host_perf_tuning()

    if not dry:
        require_host_mem_for_vms(storage, count, mem_mb)
    for tool in ("qemu-system-x86_64", "numactl", "taskset", "ssh", "rsync", "guestmount"):
        if not shutil.which(tool):
            raise SystemExit(f"missing required tool: {tool}")
    if not dry:
        if not image.is_file() or image.stat().st_size == 0:
            raise SystemExit(f"missing VM image: {image}")
        if not pool_tool.is_file() or not os.access(pool_tool, os.X_OK):
            raise SystemExit(f"missing shared-pool tool: {pool_tool}")
        if not ssh_pub.strip():
            raise SystemExit("vm.local_ssh_pub_key must be configured before actual VM launch")

    print("DSIDLE_VM_PREFLIGHT_VALIDATED")
    if args.apply_host_tuning:
        apply_host_perf_tuning(runner)
        # SMT/governor changes can alter CPU availability. Revalidate the
        # configured CPU contract after real tuning and before any VM state is
        # destroyed or rewritten.
        if not dry:
            validate_host_cpu_topology(*topology_args)
            check_host_perf_tuning()

    print(
        f"DSIDLE_VM_PREFLIGHT_OK config={config_path} shared_numa={','.join(map(str, shared_nodes))} "
        f"vm_numa={','.join(map(str, vm_nodes))}"
    )

    kill_existing_vms(storage, runner)
    setup_shared_memory_tmpfs(shared_dir, size_mb, shared_nodes, runner)
    prepare_plain_ivshmem_file(backing, size_mb, runner)
    # D-SIDLE-specific: write pool metadata into the shared backing before QEMU maps it.
    # Trigger, promotion, demotion, and cooler use epoch slots.  The fifth
    # original SIDLE role (adjuster) and the runner heartbeat do not.
    epoch_slots_per_vm = int(cfg["e2e"]["foreground_worker_count_per_vm"]) + 4
    runner.run(
        [
            str(pool_tool),
            "--init-pool",
            "--config",
            str(config_path),
            "--node-control-capacity",
            "2097152",
            "--max-threads-per-vm",
            str(epoch_slots_per_vm),
        ]
    )
    if not dry:
        assert_shared_path_is_tmpfs(shared_dir)
    setup_bridge_tap_network(bridge_tap_ip, count, runner)

    ports = [ssh_base + index for index in range(count)]
    for index in range(count):
        vm_dir = storage / f"vm_{index}"
        if not args.skip_guestmount:
            prepare_vm_disk(
                repo_root,
                image,
                vm_dir,
                index,
                first_ip,
                bridge_tap_ip,
                ssh_pub,
                copy_root_img=True,
                assets=assets,
                runner=runner,
            )
        elif not dry:
            vm_dir.mkdir(parents=True, exist_ok=True)
            disk = vm_dir / "root.img"
            if not disk.exists():
                shutil.copyfile(image, disk)

    # D-SIDLE config_vm_files: sync + settle after all guestmount injections.
    if not dry and not args.skip_guestmount:
        runner.shell("sync")
        time.sleep(3)

    if not dry:
        wait_for_ssh_ports_free(ports)

    for index in range(count):
        vm_node = vm_nodes[index % len(vm_nodes)]
        port = ports[index]
        launch_qemu(
            index,
            vm_node,
            cores,
            mem_mb,
            size_mb,
            storage,
            backing,
            port,
            runner,
        )

    for index, port in enumerate(ports):
        wait_ssh(port, storage / f"vm_{index}" / "qemu.pid", runner)

    # D-SIDLE prepare_for_network: high-metric bridge default then delete it so
    # DHCP/user NIC remains the preferred default route.
    for port in ports:
        runner.run(
            ssh_cmd(
                port,
                f"ip -4 route replace default via {bridge_tap_ip} dev enp0s4 metric 2048",
            ),
            check=True,
        )
        runner.run(
            ssh_cmd(
                port,
                f"ip -4 route del default via {bridge_tap_ip} dev enp0s4",
            ),
            check=True,
        )

    prepare_kernel_module(repo_root, ports, runner)
    check_ivshmem_device(ports, size_mb, device_path, runner)

    for index in range(count):
        pidfile = storage / f"vm_{index}" / "qemu.pid"
        core_slice = vm_cores[index * cores : (index + 1) * cores]
        if dry:
            print(f"[init_vm] dry-run taskset vm_{index} -> {core_slice}")
            continue
        if not pidfile.exists():
            raise SystemExit(f"missing pidfile: {pidfile}")
        pid = int(pidfile.read_text().strip())
        pin_pid_to_cpuset(f"vm_{index} qemu", pid, ",".join(map(str, core_slice)), runner)

    print("DSIDLE_VM_INIT_DRY_RUN_OK" if dry else "DSIDLE_VM_INIT_OK")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except subprocess.CalledProcessError as error:
        print(f"dsidle_init_vm: command failed: {error}", file=sys.stderr)
        raise SystemExit(1)
