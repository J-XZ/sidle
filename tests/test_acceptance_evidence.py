#!/usr/bin/env python3
"""Fail-closed contract and provenance tests for formal acceptance artifacts."""

from __future__ import annotations

import hashlib
import json
import subprocess
import sys
import tempfile
from pathlib import Path


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def write_json(path: Path, value) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2) + "\n")


def formal_config() -> dict:
    return {
        "shared_memory": {
            "size_mb": 32768,
            "path": "/formal/ivshmem",
            "device_path": "/dev/ivpci0",
            "hwcc": {"offset_mb": 0, "size_mb": 1024},
            "swcc": {"offset_mb": 1024, "size_mb": 31744},
        },
        "vm": {"count": 4, "core_count_per_vm": 8},
        "e2e": {"foreground_worker_count_per_vm": 4},
        "dsidle": {
            "verbose": False,
            "extra_check": False,
            "fixed_key_size": 32,
            "fixed_value_size": 32,
            "latency_inject": {
                "fixed_latency": {
                    "enabled": False,
                    "foreground_enabled": False,
                    "background_enabled": False,
                },
            },
        },
    }


def write_e2e_case(root: Path, helper: Path) -> None:
    config = root / "e2e-host.jsonc"
    guest = root / "e2e-guest.jsonc"
    runner = root / "e2e-runner"
    pool = root / "pool-tool"
    write_json(config, formal_config())
    guest_config = formal_config()
    guest_config["shared_memory"]["path"] = "/dev/ivpci0"
    guest_config["dsidle"]["fixed_key_size"] = 8
    guest_config["dsidle"]["fixed_value_size"] = 8
    write_json(guest, guest_config)
    runner.write_bytes(b"formal e2e runner")
    pool.write_bytes(b"formal pool tool")
    metadata = {
        "formal_acceptance": True,
        "suite": "08",
        "rounds": 10,
        "nodes": 4,
        "workers_per_vm": 4,
        "vm_vcpus_per_node": 8,
        "total_keys": 100000,
        "git_sha": "0" * 40,
        "git_tracked_clean": True,
        "config": str(config),
        "config_sha256": digest(config),
        "guest_config": str(guest),
        "guest_config_sha256": digest(guest),
        "runner": str(runner),
        "runner_sha256": digest(runner),
        "pool_tool": str(pool),
        "pool_tool_sha256": digest(pool),
        "cache_clear": {
            "target": "host_and_vms",
            "cpu_workers": 4,
            "cpu_sweep_mb_per_worker": 64,
            "page_cache": True,
        },
    }
    metadata_path = root / "run_meta.json"
    write_json(metadata_path, metadata)
    round_dir = root / "round_logs"
    log_dir = root / "logs"
    round_dir.mkdir()
    log_dir.mkdir()
    for round_id in range(1, 11):
        (round_dir / f"e2e08_round_{round_id}.meta").write_text(
            f"suite=08 round={round_id} git_sha={'0' * 40} "
            f"config_sha256={metadata['config_sha256']} "
            f"guest_config_sha256={metadata['guest_config_sha256']} "
            f"runner_sha256={metadata['runner_sha256']} "
            f"pool_tool_sha256={metadata['pool_tool_sha256']} exit_code=0\n"
        )
        for stage in ("fill", "read"):
            phase = f"e2e08_{stage}"
            for node in range(4):
                (log_dir / f"{phase}_round_{round_id}_node{node}.log").write_text(
                    f"{phase} round={round_id} node={node}\n"
                )
    summaries = []
    for name in ("summary.json", "rows.csv", "summary.md"):
        path = root / name
        path.write_text(f"{name}\n")
        summaries.append(path)
    output = root / "acceptance.meta"
    command = [
        sys.executable,
        str(helper),
        "finalize",
        "--kind",
        "vm-e2e",
        "--metadata",
        str(metadata_path),
    ]
    for summary in summaries:
        command += ["--summary", str(summary)]
    command += ["--output", str(output)]
    subprocess.run(command, check=True)
    acceptance = json.loads(output.read_text())
    assert acceptance["formal_acceptance"] is True
    assert acceptance["kind"] == "vm-e2e"
    assert acceptance["metadata"]["sha256"] == digest(metadata_path)
    assert len(acceptance["execution_evidence"]) == 90

    duplicate = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    assert duplicate.returncode != 0
    assert "refusing to overwrite" in duplicate.stderr

    pool.write_bytes(b"mutated pool tool")
    changed = subprocess.run(
        [
            sys.executable,
            str(helper),
            "validate",
            "--kind",
            "vm-e2e",
            "--metadata",
            str(metadata_path),
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    assert changed.returncode != 0
    assert "pool_tool_sha256" in changed.stderr


def write_ycsb_case(root: Path, helper: Path) -> None:
    config = root / "configs" / "experiment_config_ycsb_4vm.jsonc"
    write_json(config, formal_config())
    phase_names = ["load", "workloada", "workloadb", "workloadc", "workloadd", "workloade"]
    phase_configs = {}
    phase_hashes = {}
    guest_hashes = {}
    for phase in phase_names:
        phase_path = root / "configs" / f"experiment_config_ycsb_{phase}.jsonc"
        phase_config = formal_config()
        phase_config["dsidle"]["trace_dir"] = str(root / "traces" / phase)
        write_json(phase_path, phase_config)
        phase_configs[phase] = str(phase_path)
        phase_hashes[phase] = digest(phase_path)
        guest_path = root / "guest_configs" / f"{phase}.jsonc"
        guest_config = phase_config
        guest_config["shared_memory"]["path"] = "/dev/ivpci0"
        guest_config["dsidle"]["trace_dir"] = f"/root/dsidle-ycsb/traces/{phase}"
        write_json(guest_path, guest_config)
        guest_hashes[phase] = digest(guest_path)

    runner = root / "trace-runner"
    pool = root / "pool-tool"
    runner.write_bytes(b"formal trace runner")
    pool.write_bytes(b"formal pool tool")
    trace_set_sha = "a" * 64
    normalization = root / "trace_normalization.json"
    write_json(normalization, {"output_trace_set_sha256": trace_set_sha})
    formal_counts = {
        "load": {"PUT": 100000},
        "workloada": {"GET": 100000, "PUT": 50199},
        "workloadb": {"GET": 95019, "PUT": 4981},
        "workloadc": {"GET": 100000},
        "workloadd": {"GET": 95072, "PUT": 4928},
        "workloade": {"SCAN": 94920, "PUT": 5080},
    }
    manifest = root / "trace_manifest.json"
    write_json(
        manifest,
        {
            "formal_frozen_count_contract": True,
            "nodes": 4,
            "threads_per_node": 4,
            "total_workers": 16,
            "record_count": 100000,
            "operation_count": 100000,
            "fixed_key_size_bytes": 32,
            "fixed_value_size_bytes": 32,
            "trace_set_sha256": trace_set_sha,
            "normalization": {
                "path": str(normalization),
                "sha256": digest(normalization),
                "output_trace_set_sha256": trace_set_sha,
            },
            "phases": {
                phase: {"op_counts": counts}
                for phase, counts in formal_counts.items()
            },
        },
    )
    guest_set_sha = hashlib.sha256(
        "".join(f"{phase}:{guest_hashes[phase]}\n" for phase in sorted(guest_hashes)).encode()
    ).hexdigest()
    metadata = {
        "formal_acceptance": True,
        "rounds": 10,
        "warmup_rounds": 1,
        "record_count": 100000,
        "operation_count": 100000,
        "nodes": 4,
        "threads_per_node": 4,
        "total_trace_workers": 16,
        "vm_vcpus_per_node": 8,
        "workloads": ["a", "b", "c", "d", "e"],
        "skip_standalone_load": False,
        "latency_inject_enabled": False,
        "shared_size_mb": 32768,
        "cache_flush_mb": 512,
        "git_sha": "1" * 40,
        "git_tracked_clean": True,
        "experiment_config": str(config),
        "experiment_config_sha256": digest(config),
        "phase_configs": phase_configs,
        "phase_config_sha256": phase_hashes,
        "guest_config_sha256": guest_hashes,
        "guest_config_set_sha256": guest_set_sha,
        "runner": str(runner),
        "runner_sha256": digest(runner),
        "pool_tool": str(pool),
        "pool_tool_sha256": digest(pool),
        "trace_manifest": str(manifest),
        "trace_manifest_sha256": digest(manifest),
    }
    metadata_path = root / "run_meta.json"
    write_json(metadata_path, metadata)
    round_dir = root / "round_logs"
    log_dir = root / "logs"
    round_dir.mkdir(exist_ok=True)
    log_dir.mkdir(exist_ok=True)
    labels = ["warmup_1"] + [f"round_{round_id}" for round_id in range(1, 11)]
    cases = [("load", ("load",))]
    cases += [(f"workload{item}", ("load", "run")) for item in "abcde"]
    for case, stages in cases:
        for label in labels:
            for stage in stages:
                phase = "load" if stage == "load" else case
                (round_dir / f"{case}_{label}_{stage}.meta").write_text(
                    f"case={case} label={label} stage={stage} phase={phase} "
                    f"status=success manifest_sha256={metadata['trace_manifest_sha256']} "
                    f"runner_sha256={metadata['runner_sha256']} "
                    f"pool_tool_sha256={metadata['pool_tool_sha256']} "
                    f"guest_config_set_sha256={guest_set_sha}\n"
                )
                for node in range(4):
                    (log_dir / f"{case}_{label}_{stage}_node{node}.log").write_text(
                        f"{case} {label} {stage} node={node}\n"
                    )
    summaries = []
    for name in ("ycsb_summary.json", "ycsb_summary.csv", "YCSB实验报告.md"):
        path = root / name
        path.write_text(f"{name}\n")
        summaries.append(path)
    output = root / "acceptance.meta"
    command = [
        sys.executable,
        str(helper),
        "finalize",
        "--kind",
        "ycsb",
        "--metadata",
        str(metadata_path),
    ]
    for summary in summaries:
        command += ["--summary", str(summary)]
    command += ["--output", str(output)]
    subprocess.run(command, check=True)
    acceptance = json.loads(output.read_text())
    assert acceptance["kind"] == "ycsb"
    assert len(acceptance["execution_evidence"]) == 605

    metadata["formal_acceptance"] = False
    write_json(metadata_path, metadata)
    rejected = subprocess.run(
        [
            sys.executable,
            str(helper),
            "validate",
            "--kind",
            "ycsb",
            "--metadata",
            str(metadata_path),
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    assert rejected.returncode != 0
    assert "formal_acceptance" in rejected.stderr


def check_shell_preflight(
    config: Path, e2e_script: Path, ycsb_script: Path, root: Path
) -> None:
    e2e_out = root / "invalid-e2e"
    e2e = subprocess.run(
        [
            str(e2e_script),
            "--suite",
            "08",
            "--formal-acceptance",
            "--rounds",
            "1",
            "--config",
            str(config),
            "--out-dir",
            str(e2e_out),
            "--runner",
            "/bin/true",
            "--pool-tool",
            "/bin/true",
            "--execute",
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    assert e2e.returncode == 2
    assert "exactly 10 rounds" in e2e.stderr
    assert not e2e_out.exists()

    ycsb_out = root / "invalid-ycsb"
    ycsb = subprocess.run(
        [
            str(ycsb_script),
            "--formal-acceptance",
            "--prepare-only",
            "--rounds",
            "10",
            "--warmup-rounds",
            "1",
            "--record-count",
            "100000",
            "--operation-count",
            "100000",
            "--threads-per-node",
            "4",
            "--vm-count",
            "4",
            "--workloads",
            "a,b,c,d,e",
            "--shared-size-mb",
            "32768",
            "--no-latency",
            "--base-config",
            str(config),
            "--out-dir",
            str(ycsb_out),
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    assert ycsb.returncode == 2
    assert "real execution" in ycsb.stderr
    assert not ycsb_out.exists()


def main() -> None:
    helper = Path(sys.argv[1]).resolve()
    config = Path(sys.argv[2]).resolve()
    e2e_script = Path(sys.argv[3]).resolve()
    ycsb_script = Path(sys.argv[4]).resolve()
    with tempfile.TemporaryDirectory(prefix="dsidle-acceptance-evidence.") as temporary:
        root = Path(temporary)
        write_e2e_case(root / "e2e", helper)
        write_ycsb_case(root / "ycsb", helper)
        check_shell_preflight(config, e2e_script, ycsb_script, root)


if __name__ == "__main__":
    main()
