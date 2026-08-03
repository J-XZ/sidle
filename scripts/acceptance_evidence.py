#!/usr/bin/env python3
"""Validate and atomically publish formal D-SIDLE acceptance evidence."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import tempfile
from pathlib import Path
from typing import Any

from jsonc_utils import load_jsonc as parse_jsonc


FORMAL_ROUNDS = 10
FORMAL_WORKLOADS = ["a", "b", "c", "d", "e"]
FORMAL_COUNTS = {
    "load": {"PUT": 100000},
    "workloada": {"GET": 100000, "PUT": 50199},
    "workloadb": {"GET": 95019, "PUT": 4981},
    "workloadc": {"GET": 100000},
    "workloadd": {"GET": 95072, "PUT": 4928},
    "workloade": {"SCAN": 94920, "PUT": 5080},
}


def fail(message: str) -> None:
    raise SystemExit(message)


def require(condition: bool, message: str) -> None:
    if not condition:
        fail(message)


def sha256(path: Path) -> str:
    require(path.is_file(), f"missing evidence artifact: {path}")
    return hashlib.sha256(path.read_bytes()).hexdigest()


def load_json(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text())
    except (OSError, json.JSONDecodeError) as error:
        fail(f"cannot read JSON evidence {path}: {error}")
    require(isinstance(value, dict), f"JSON evidence must be an object: {path}")
    return value


def load_jsonc(path: Path) -> dict[str, Any]:
    try:
        value = parse_jsonc(path)
    except (OSError, json.JSONDecodeError) as error:
        fail(f"cannot read JSONC config {path}: {error}")
    require(isinstance(value, dict), f"config must be an object: {path}")
    return value


def resolve(metadata_path: Path, value: str) -> Path:
    path = Path(value)
    if path.is_absolute() or path.exists():
        return path
    return metadata_path.parent / path


def require_equal(actual: Any, expected: Any, field: str) -> None:
    require(actual == expected, f"formal contract mismatch for {field}: expected {expected!r}, got {actual!r}")


def validate_common_config(config: dict[str, Any]) -> None:
    shared = config["shared_memory"]
    require_equal(shared["size_mb"], 32768, "shared_memory.size_mb")
    require_equal(shared["hwcc"]["offset_mb"], 0, "shared_memory.hwcc.offset_mb")
    require_equal(shared["hwcc"]["size_mb"], 1024, "shared_memory.hwcc.size_mb")
    require_equal(shared["swcc"]["offset_mb"], 1024, "shared_memory.swcc.offset_mb")
    require_equal(shared["swcc"]["size_mb"], 31744, "shared_memory.swcc.size_mb")
    require_equal(config["vm"]["count"], 4, "vm.count")
    require_equal(config["vm"]["core_count_per_vm"], 8, "vm.core_count_per_vm")
    require_equal(
        config["e2e"]["foreground_worker_count_per_vm"],
        4,
        "e2e.foreground_worker_count_per_vm",
    )
    policy = config["dsidle"]
    require_equal(policy["verbose"], False, "dsidle.verbose")
    require_equal(policy["extra_check"], False, "dsidle.extra_check")
    latency = policy["latency_inject"]
    for section in ("fixed_latency", "hwcc_access_count", "atomic_count",
                    "remote_cache_invalidation"):
        require_equal(
            latency[section]["enabled"],
            False,
            f"dsidle.latency_inject.{section}.enabled",
        )


def validate_recorded_file(
    metadata_path: Path,
    metadata: dict[str, Any],
    path_field: str,
    sha_field: str,
) -> tuple[Path, str]:
    require(path_field in metadata, f"missing run metadata field: {path_field}")
    require(sha_field in metadata, f"missing run metadata field: {sha_field}")
    path = resolve(metadata_path, str(metadata[path_field]))
    digest = sha256(path)
    require_equal(digest, metadata[sha_field], sha_field)
    return path, digest


def validate_e2e(metadata_path: Path, metadata: dict[str, Any]) -> dict[str, str]:
    require_equal(metadata.get("formal_acceptance"), True, "formal_acceptance")
    require_equal(metadata.get("git_tracked_clean"), True, "git_tracked_clean")
    require(
        re.fullmatch(r"[0-9a-f]{40,64}", str(metadata.get("git_sha", ""))) is not None,
        "formal acceptance requires a full hexadecimal git SHA",
    )
    require(metadata.get("suite") in ("08", "09"), "formal e2e suite must be 08 or 09")
    require_equal(metadata.get("rounds"), FORMAL_ROUNDS, "rounds")
    require_equal(metadata.get("nodes"), 4, "nodes")
    require_equal(metadata.get("workers_per_vm"), 4, "workers_per_vm")
    require_equal(metadata.get("vm_vcpus_per_node"), 8, "vm_vcpus_per_node")
    require_equal(metadata.get("total_keys"), 100000, "total_keys")
    cache = metadata.get("cache_clear", {})
    require_equal(cache.get("target"), "host_and_vms", "cache_clear.target")
    require_equal(cache.get("cpu_workers"), 4, "cache_clear.cpu_workers")
    require_equal(cache.get("cpu_sweep_mb_per_worker"), 64, "cache_clear.cpu_sweep_mb_per_worker")
    require_equal(cache.get("page_cache"), True, "cache_clear.page_cache")

    config_path, config_sha = validate_recorded_file(
        metadata_path, metadata, "config", "config_sha256"
    )
    guest_path, guest_sha = validate_recorded_file(
        metadata_path, metadata, "guest_config", "guest_config_sha256"
    )
    runner_path, runner_sha = validate_recorded_file(
        metadata_path, metadata, "runner", "runner_sha256"
    )
    pool_path, pool_sha = validate_recorded_file(
        metadata_path, metadata, "pool_tool", "pool_tool_sha256"
    )
    config = load_jsonc(config_path)
    validate_common_config(config)
    guest = load_jsonc(guest_path)
    validate_common_config(guest)
    require_equal(
        guest["shared_memory"]["path"],
        guest["shared_memory"]["device_path"],
        "guest shared_memory.path",
    )
    expected_sizes = (8, 8) if metadata["suite"] == "08" else (32, 1000)
    require_equal(
        (guest["dsidle"]["fixed_key_size"], guest["dsidle"]["fixed_value_size"]),
        expected_sizes,
        "guest fixed key/value sizes",
    )
    return {
        str(config_path): config_sha,
        str(guest_path): guest_sha,
        str(runner_path): runner_sha,
        str(pool_path): pool_sha,
    }


def validate_ycsb(metadata_path: Path, metadata: dict[str, Any]) -> dict[str, str]:
    require_equal(metadata.get("formal_acceptance"), True, "formal_acceptance")
    require_equal(metadata.get("git_tracked_clean"), True, "git_tracked_clean")
    require(
        re.fullmatch(r"[0-9a-f]{40,64}", str(metadata.get("git_sha", ""))) is not None,
        "formal acceptance requires a full hexadecimal git SHA",
    )
    require_equal(metadata.get("rounds"), FORMAL_ROUNDS, "rounds")
    require_equal(metadata.get("warmup_rounds"), 1, "warmup_rounds")
    require_equal(metadata.get("record_count"), 100000, "record_count")
    require_equal(metadata.get("operation_count"), 100000, "operation_count")
    require_equal(metadata.get("nodes"), 4, "nodes")
    require_equal(metadata.get("threads_per_node"), 4, "threads_per_node")
    require_equal(metadata.get("total_trace_workers"), 16, "total_trace_workers")
    require_equal(metadata.get("vm_vcpus_per_node"), 8, "vm_vcpus_per_node")
    require_equal(metadata.get("workloads"), FORMAL_WORKLOADS, "workloads")
    require_equal(metadata.get("skip_standalone_load"), False, "skip_standalone_load")
    require_equal(metadata.get("latency_inject_enabled"), False, "latency_inject_enabled")
    require_equal(metadata.get("shared_size_mb"), 32768, "shared_size_mb")
    require_equal(metadata.get("cache_flush_mb"), 512, "cache_flush_mb")

    config_path, config_sha = validate_recorded_file(
        metadata_path, metadata, "experiment_config", "experiment_config_sha256"
    )
    runner_path, runner_sha = validate_recorded_file(
        metadata_path, metadata, "runner", "runner_sha256"
    )
    pool_path, pool_sha = validate_recorded_file(
        metadata_path, metadata, "pool_tool", "pool_tool_sha256"
    )
    config = load_jsonc(config_path)
    validate_common_config(config)
    require_equal(config["dsidle"]["fixed_key_size"], 32, "dsidle.fixed_key_size")
    require_equal(config["dsidle"]["fixed_value_size"], 32, "dsidle.fixed_value_size")
    require_equal(
        config["dsidle"]["latency_inject"]["fixed_latency"]["foreground_enabled"],
        False,
        "dsidle.latency_inject.fixed_latency.foreground_enabled",
    )
    require_equal(
        config["dsidle"]["latency_inject"]["fixed_latency"]["background_enabled"],
        False,
        "dsidle.latency_inject.fixed_latency.background_enabled",
    )

    manifest_path, manifest_sha = validate_recorded_file(
        metadata_path, metadata, "trace_manifest", "trace_manifest_sha256"
    )
    manifest = load_json(manifest_path)
    require_equal(manifest.get("formal_frozen_count_contract"), True, "trace formal contract")
    require_equal(manifest.get("nodes"), 4, "trace nodes")
    require_equal(manifest.get("threads_per_node"), 4, "trace threads_per_node")
    require_equal(manifest.get("total_workers"), 16, "trace total_workers")
    require_equal(manifest.get("record_count"), 100000, "trace record_count")
    require_equal(manifest.get("operation_count"), 100000, "trace operation_count")
    require_equal(manifest.get("fixed_key_size_bytes"), 32, "trace fixed_key_size_bytes")
    require_equal(manifest.get("fixed_value_size_bytes"), 32, "trace fixed_value_size_bytes")
    require_equal(set(manifest.get("phases", {})), set(FORMAL_COUNTS), "trace phases")
    for phase, expected_counts in FORMAL_COUNTS.items():
        require_equal(
            manifest["phases"][phase].get("op_counts"),
            expected_counts,
            f"{phase} op_counts",
        )
    normalization = manifest.get("normalization")
    require(isinstance(normalization, dict), "formal trace manifest lacks normalization evidence")
    normalization_path = resolve(metadata_path, str(normalization.get("path", "")))
    normalization_sha = sha256(normalization_path)
    require_equal(normalization_sha, normalization.get("sha256"), "normalization sha256")
    require_equal(
        normalization.get("output_trace_set_sha256"),
        manifest.get("trace_set_sha256"),
        "normalized trace-set sha256",
    )

    phase_paths = metadata.get("phase_configs")
    phase_hashes = metadata.get("phase_config_sha256")
    guest_hashes = metadata.get("guest_config_sha256")
    require(isinstance(phase_paths, dict), "missing phase_configs metadata")
    require(isinstance(phase_hashes, dict), "missing phase_config_sha256 metadata")
    require(isinstance(guest_hashes, dict), "missing guest_config_sha256 metadata")
    expected_phases = set(FORMAL_COUNTS)
    require_equal(set(phase_paths), expected_phases, "phase_configs keys")
    require_equal(set(phase_hashes), expected_phases, "phase_config_sha256 keys")
    require_equal(set(guest_hashes), expected_phases, "guest_config_sha256 keys")
    artifacts = {
        str(config_path): config_sha,
        str(runner_path): runner_sha,
        str(pool_path): pool_sha,
        str(manifest_path): manifest_sha,
        str(normalization_path): normalization_sha,
    }
    guest_dir = metadata_path.parent / "guest_configs"
    for phase in sorted(expected_phases):
        phase_path = resolve(metadata_path, str(phase_paths[phase]))
        phase_sha = sha256(phase_path)
        require_equal(phase_sha, phase_hashes[phase], f"{phase} config sha256")
        guest_path = guest_dir / f"{phase}.jsonc"
        guest_sha = sha256(guest_path)
        require_equal(guest_sha, guest_hashes[phase], f"{phase} guest config sha256")
        artifacts[str(phase_path)] = phase_sha
        artifacts[str(guest_path)] = guest_sha
    return artifacts


def parse_meta(path: Path) -> dict[str, str]:
    require(path.is_file(), f"missing execution evidence: {path}")
    fields = {}
    for token in path.read_text().split():
        if "=" in token:
            key, value = token.split("=", 1)
            fields[key] = value
    return fields


def validate_execution_evidence(
    kind: str, metadata_path: Path, metadata: dict[str, Any]
) -> dict[str, str]:
    evidence: dict[str, str] = {}
    round_dir = metadata_path.parent / "round_logs"
    log_dir = metadata_path.parent / "logs"
    if kind == "vm-e2e":
        suite = metadata["suite"]
        stages = ("fill", "read") if suite == "08" else ("fill", "update", "read")
        for round_id in range(1, FORMAL_ROUNDS + 1):
            path = round_dir / f"e2e{suite}_round_{round_id}.meta"
            fields = parse_meta(path)
            expected = {
                "suite": suite,
                "round": str(round_id),
                "git_sha": str(metadata["git_sha"]),
                "config_sha256": str(metadata["config_sha256"]),
                "guest_config_sha256": str(metadata["guest_config_sha256"]),
                "runner_sha256": str(metadata["runner_sha256"]),
                "pool_tool_sha256": str(metadata["pool_tool_sha256"]),
                "exit_code": "0",
            }
            for field, value in expected.items():
                require_equal(fields.get(field), value, f"{path.name}:{field}")
            evidence[str(path)] = sha256(path)
            for stage in stages:
                phase = f"e2e{suite}_{stage}"
                for node in range(4):
                    log_path = log_dir / f"{phase}_round_{round_id}_node{node}.log"
                    evidence[str(log_path)] = sha256(log_path)
    else:
        labels = [f"warmup_{index}" for index in range(1, 2)]
        labels += [f"round_{index}" for index in range(1, FORMAL_ROUNDS + 1)]
        cases = [("load", ("load",))]
        cases += [(f"workload{item}", ("load", "run")) for item in FORMAL_WORKLOADS]
        for case, stages in cases:
            for label in labels:
                for stage in stages:
                    phase = "load" if stage == "load" else case
                    path = round_dir / f"{case}_{label}_{stage}.meta"
                    fields = parse_meta(path)
                    expected = {
                        "case": case,
                        "label": label,
                        "stage": stage,
                        "phase": phase,
                        "status": "success",
                        "manifest_sha256": str(metadata["trace_manifest_sha256"]),
                        "runner_sha256": str(metadata["runner_sha256"]),
                        "pool_tool_sha256": str(metadata["pool_tool_sha256"]),
                        "guest_config_set_sha256": str(metadata["guest_config_set_sha256"]),
                    }
                    for field, value in expected.items():
                        require_equal(fields.get(field), value, f"{path.name}:{field}")
                    evidence[str(path)] = sha256(path)
                    for node in range(4):
                        log_path = log_dir / f"{case}_{label}_{stage}_node{node}.log"
                        evidence[str(log_path)] = sha256(log_path)
    return evidence


def validate(kind: str, metadata_path: Path) -> tuple[dict[str, Any], dict[str, str]]:
    metadata = load_json(metadata_path)
    if kind == "vm-e2e":
        artifacts = validate_e2e(metadata_path, metadata)
    else:
        artifacts = validate_ycsb(metadata_path, metadata)
    return metadata, artifacts


def finalize(kind: str, metadata_path: Path, summaries: list[Path], output: Path) -> None:
    require(not output.exists(), f"refusing to overwrite existing acceptance evidence: {output}")
    metadata, artifacts = validate(kind, metadata_path)
    require(summaries, "at least one summary artifact is required")
    summary_hashes = {str(path): sha256(path) for path in summaries}
    execution_hashes = validate_execution_evidence(kind, metadata_path, metadata)
    manifest = {
        "status": "success",
        "kind": kind,
        "formal_acceptance": True,
        "git_sha": metadata["git_sha"],
        "metadata": {
            "path": str(metadata_path),
            "sha256": sha256(metadata_path),
        },
        "artifacts": dict(sorted(artifacts.items())),
        "execution_evidence": dict(sorted(execution_hashes.items())),
        "summaries": dict(sorted(summary_hashes.items())),
    }
    output.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary = tempfile.mkstemp(
        prefix=f".{output.name}.", dir=output.parent, text=True
    )
    try:
        with os.fdopen(descriptor, "w") as stream:
            json.dump(manifest, stream, indent=2, sort_keys=True)
            stream.write("\n")
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, output)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("action", choices=("validate", "finalize"))
    parser.add_argument("--kind", required=True, choices=("vm-e2e", "ycsb"))
    parser.add_argument("--metadata", required=True, type=Path)
    parser.add_argument("--summary", action="append", type=Path, default=[])
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    if args.action == "validate":
        validate(args.kind, args.metadata)
        return
    require(args.output is not None, "finalize requires --output")
    finalize(args.kind, args.metadata, args.summary, args.output)


if __name__ == "__main__":
    main()
