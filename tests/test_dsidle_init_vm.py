#!/usr/bin/env python3
"""Unit tests for dsidle_init_vm host CPU topology validation."""

from __future__ import annotations

import sys
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO_ROOT / "scripts"))

from dsidle_init_vm import (  # noqa: E402
    as_list,
    cpu_is_online,
    cpu_numa_node,
    load_jsonc,
    online_cpus,
    validate_host_cpu_topology,
)


def load_experiment_topology(config_path: Path) -> tuple[list[int], int, int, list[int], list[int]]:
    cfg = load_jsonc(config_path)
    vm = cfg["vm"]
    cpu = cfg["host_cpu"]
    vm_nodes = list(map(int, as_list(vm["numa_node"])))
    count = int(vm["count"])
    cores = int(vm["core_count_per_vm"])
    reserved = list(map(int, as_list(cpu["reserved_cores"])))
    vm_cores = list(map(int, as_list(cpu["vm_cores"])))
    return vm_nodes, count, cores, reserved, vm_cores


class HostCpuTopologyTests(unittest.TestCase):
  def test_experiment_config_topology_is_valid_on_host(self) -> None:
    config = REPO_ROOT / "experiment_config.jsonc"
    topology_args = load_experiment_topology(config)
    validate_host_cpu_topology(*topology_args)

  def test_rejects_duplicate_vm_core_assignment(self) -> None:
    vm_nodes, count, cores, reserved, vm_cores = load_experiment_topology(
        REPO_ROOT / "experiment_config.jsonc"
    )
    bad_vm_cores = list(vm_cores)
    bad_vm_cores[1] = bad_vm_cores[0]
    with self.assertRaises(SystemExit):
      validate_host_cpu_topology(vm_nodes, count, cores, reserved, bad_vm_cores)

  def test_rejects_vm_core_on_wrong_numa(self) -> None:
    vm_nodes, count, cores, reserved, vm_cores = load_experiment_topology(
        REPO_ROOT / "experiment_config.jsonc"
    )
    online = online_cpus(Path("/sys/devices/system/cpu/online").read_text())
    candidates = [
        core
        for core in sorted(online)
        if cpu_is_online(core) and cpu_numa_node(core) != vm_nodes[0]
    ]
    if not candidates:
      self.skipTest("host has no online CPU outside configured vm.numa_node")
    wrong_core = candidates[0]
    bad_vm_cores = list(vm_cores)
    bad_vm_cores[0] = wrong_core
    with self.assertRaises(SystemExit):
      validate_host_cpu_topology(vm_nodes, count, cores, reserved, bad_vm_cores)

  def test_apply_host_tuning_revalidates_same_topology_tuple(self) -> None:
    source = (REPO_ROOT / "scripts" / "dsidle_init_vm.py").read_text()
    self.assertIn("topology_args = (vm_nodes, count, cores, reserved, vm_cores)", source)
    self.assertEqual(source.count("validate_host_cpu_topology(*topology_args)"), 2)


if __name__ == "__main__":
  unittest.main()
