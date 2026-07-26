#!/usr/bin/env python3
"""Targeted checks for the real-VM e2e evidence helpers."""

import hashlib
import json
import subprocess
import sys
import tempfile
from pathlib import Path


def write_run(root: Path, config: Path, suite: str, rounds: int) -> Path:
    log_dir = root / "logs"
    round_dir = root / "round_logs"
    log_dir.mkdir(parents=True)
    round_dir.mkdir()
    git_sha = "0123456789abcdef"
    config_sha = hashlib.sha256(config.read_bytes()).hexdigest()
    metadata = {
        "suite": suite,
        "rounds": rounds,
        "config": str(config),
        "config_sha256": config_sha,
        "git_sha": git_sha,
        "nodes": 4,
        "workers_per_vm": 4,
    }
    metadata_path = root / "run_meta.json"
    metadata_path.write_text(json.dumps(metadata) + "\n")
    stages = ("fill", "read") if suite == "08" else ("fill", "update", "read")
    for round_id in range(1, rounds + 1):
        (round_dir / f"e2e{suite}_round_{round_id}.meta").write_text(
            f"suite={suite} round={round_id} git_sha={git_sha} "
            f"config_sha256={config_sha} exit_code=0\n"
        )
        for stage in stages:
            phase = f"e2e{suite}_{stage}"
            for node in range(4):
                duration = round_id * 1000 + node
                (log_dir / f"{phase}_round_{round_id}_node{node}.log").write_text(
                    f"E2E_TRACE_TIME_US phase={phase} node={node} ops=25000 "
                    f"duration_us={duration} trace_first={node * 4} "
                    "trace_workers=4 batch_ops=0\n"
                    "DSIDLE_MEMORY_STATS hwcc_bytes=1 swcc_bytes=2 "
                    "replica_bytes=3\n"
                    f"DSIDLE_E2E_SUITE_VERIFY suite={suite} phase={phase} "
                    f"node={node} status=ok\n"
                )
    return metadata_path


def main() -> None:
    config = Path(sys.argv[1]).resolve()
    summarizer = Path(sys.argv[2]).resolve()
    cache_helper = Path(sys.argv[3]).resolve()
    with tempfile.TemporaryDirectory(prefix="dsidle-vm-e2e-harness.") as temporary:
        root = Path(temporary)
        for suite in ("08", "09"):
            run = root / f"e2e{suite}"
            metadata = write_run(run, config, suite, rounds=2)
            subprocess.run(
                [
                    sys.executable,
                    str(summarizer),
                    "--suite",
                    suite,
                    "--log-dir",
                    str(run / "logs"),
                    "--out-dir",
                    str(run),
                    "--metadata",
                    str(metadata),
                ],
                check=True,
            )
            summary = json.loads((run / f"e2e{suite}_summary.json").read_text())
            for row in summary["round_phase_max_summary"]:
                assert row["round_max_duration_us_avg"] == 1503
                assert row["round_max_duration_us_max"] == 2003
                assert row["op_count"] == 100000
            assert (run / f"e2e{suite}_summary.md").stat().st_size
            assert (run / f"e2e{suite}_phase_rows.csv").stat().st_size

        broken = root / "e2e08" / "logs" / "e2e08_read_round_2_node3.log"
        broken.write_text(broken.read_text().replace("ops=25000", "ops=24999"))
        failed = subprocess.run(
            [
                sys.executable,
                str(summarizer),
                "--suite",
                "08",
                "--log-dir",
                str(root / "e2e08" / "logs"),
                "--out-dir",
                str(root / "e2e08"),
                "--metadata",
                str(root / "e2e08" / "run_meta.json"),
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        assert failed.returncode != 0
        assert "ops mismatch" in failed.stderr

        dry_run = subprocess.run(
            [
                sys.executable,
                str(cache_helper),
                "--config",
                str(config),
                "--target",
                "all",
                "--cpu-workers",
                "4",
                "--cpu-sweep-mb",
                "64",
                "--dry-run",
            ],
            check=True,
            stdout=subprocess.PIPE,
            text=True,
        ).stdout
        assert "target=host workers=4 sweep_mb=64" in dry_run
        for node in range(4):
            assert f"target=vm{node} workers=4 sweep_mb=64" in dry_run


if __name__ == "__main__":
    main()
