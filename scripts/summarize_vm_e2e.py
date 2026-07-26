#!/usr/bin/env python3
"""Strictly validate and summarize D-SIDLE's real-VM e2e08/09 rounds."""

import argparse
import csv
import hashlib
import json
import re
from pathlib import Path
from statistics import mean


TIME = re.compile(
    r"E2E_TRACE_TIME_US phase=(?P<phase>\S+) node=(?P<node>\d+) "
    r"ops=(?P<ops>\d+) duration_us=(?P<duration>\d+) "
    r"trace_first=(?P<trace_first>\d+) trace_workers=(?P<trace_workers>\d+) "
    r"batch_ops=(?P<batch_ops>\d+)"
)
MEMORY = re.compile(
    r"DSIDLE_MEMORY_STATS hwcc_bytes=(\d+) swcc_bytes=(\d+) replica_bytes=(\d+)"
)


def start_for_part(total: int, parts: int, part: int) -> int:
    return total * part // parts


def count_for_part(total: int, parts: int, part: int) -> int:
    return start_for_part(total, parts, part + 1) - start_for_part(total, parts, part)


def one_fullmatch(path: Path, prefix: str, pattern: re.Pattern):
    rows = [
        pattern.fullmatch(line)
        for line in path.read_text(errors="replace").splitlines()
        if line.startswith(prefix)
    ]
    if len(rows) != 1 or rows[0] is None:
        raise SystemExit(f"{path}: expected exactly one complete {prefix.strip()} row")
    return rows[0]


def write_csv(path: Path, rows) -> None:
    if not rows:
        return
    with path.open("w", newline="") as output:
        writer = csv.DictWriter(output, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--suite", required=True, choices=("08", "09"))
    parser.add_argument("--log-dir", required=True, type=Path)
    parser.add_argument("--out-dir", required=True, type=Path)
    parser.add_argument("--metadata", required=True, type=Path)
    args = parser.parse_args()

    metadata = json.loads(args.metadata.read_text())
    if str(metadata["suite"]) != args.suite:
        raise SystemExit("suite differs between command and run metadata")
    rounds = int(metadata["rounds"])
    nodes = int(metadata["nodes"])
    workers = int(metadata["workers_per_vm"])
    if nodes != 4 or workers != 4:
        raise SystemExit(f"formal VM e2e requires 4 nodes x 4 workers, got {nodes}x{workers}")
    config = Path(metadata["config"])
    if hashlib.sha256(config.read_bytes()).hexdigest() != metadata["config_sha256"]:
        raise SystemExit(f"config hash mismatch: {config}")

    stages = ("fill", "read") if args.suite == "08" else ("fill", "update", "read")
    rows = []
    for round_id in range(1, rounds + 1):
        round_meta = args.out_dir / "round_logs" / f"e2e{args.suite}_round_{round_id}.meta"
        evidence = dict(
            item.split("=", 1)
            for item in round_meta.read_text().split()
            if "=" in item
        )
        expected_evidence = {
            "suite": args.suite,
            "round": str(round_id),
            "git_sha": metadata["git_sha"],
            "config_sha256": metadata["config_sha256"],
            "exit_code": "0",
        }
        for key, value in expected_evidence.items():
            if evidence.get(key) != value:
                raise SystemExit(
                    f"{round_meta}: {key} mismatch: expected {value}, got {evidence.get(key)}"
                )
        for stage in stages:
            phase = f"e2e{args.suite}_{stage}"
            for node in range(nodes):
                path = args.log_dir / f"{phase}_round_{round_id}_node{node}.log"
                result = one_fullmatch(path, "E2E_TRACE_TIME_US ", TIME)
                one_fullmatch(path, "DSIDLE_MEMORY_STATS ", MEMORY)
                verify = (
                    f"DSIDLE_E2E_SUITE_VERIFY suite={args.suite} "
                    f"phase={phase} node={node} status=ok"
                )
                if path.read_text(errors="replace").splitlines().count(verify) != 1:
                    raise SystemExit(f"{path}: expected exactly one verification marker")
                expected = {
                    "phase": phase,
                    "node": node,
                    "ops": count_for_part(100000, nodes, node),
                    "trace_first": node * workers,
                    "trace_workers": workers,
                    "batch_ops": 0,
                }
                actual = {
                    "phase": result["phase"],
                    "node": int(result["node"]),
                    "ops": int(result["ops"]),
                    "trace_first": int(result["trace_first"]),
                    "trace_workers": int(result["trace_workers"]),
                    "batch_ops": int(result["batch_ops"]),
                }
                for key, value in expected.items():
                    if actual[key] != value:
                        raise SystemExit(
                            f"{path}: {key} mismatch: expected {value}, got {actual[key]}"
                        )
                duration = int(result["duration"])
                if duration <= 0:
                    raise SystemExit(f"{path}: duration_us must be positive")
                rows.append(
                    {
                        "round": round_id,
                        "phase": stage,
                        "node": node,
                        "ops": actual["ops"],
                        "duration_us": duration,
                        "log": str(path),
                    }
                )

    summary = []
    for stage in stages:
        maxima = [
            max(
                row["duration_us"]
                for row in rows
                if row["phase"] == stage and row["round"] == round_id
            )
            for round_id in range(1, rounds + 1)
        ]
        average_max = mean(maxima)
        summary.append(
            {
                "phase": stage,
                "rounds": rounds,
                "round_max_duration_us_avg": average_max,
                "round_max_duration_us_max": max(maxima),
                "op_count": 100000,
                "ops_per_sec_from_avg_round_max": 100000 / (average_max / 1_000_000),
            }
        )

    args.out_dir.mkdir(parents=True, exist_ok=True)
    prefix = f"e2e{args.suite}"
    write_csv(args.out_dir / f"{prefix}_phase_rows.csv", rows)
    write_csv(args.out_dir / f"{prefix}_round_phase_max_summary.csv", summary)
    (args.out_dir / f"{prefix}_summary.json").write_text(
        json.dumps(
            {
                "run_meta": metadata,
                "round_count": rounds,
                "phase_rows": rows,
                "round_phase_max_summary": summary,
            },
            indent=2,
        )
        + "\n"
    )
    lines = [
        f"# D-SIDLE e2e{args.suite} real-VM summary",
        "",
        "| phase | rounds | avg_round_max_s | max_round_max_s | op_count | ops_per_sec |",
        "| --- | ---: | ---: | ---: | ---: | ---: |",
    ]
    lines.extend(
        f"| {row['phase']} | {row['rounds']} | "
        f"{row['round_max_duration_us_avg'] / 1_000_000:.6f} | "
        f"{row['round_max_duration_us_max'] / 1_000_000:.6f} | "
        f"{row['op_count']} | {row['ops_per_sec_from_avg_round_max']:.3f} |"
        for row in summary
    )
    (args.out_dir / f"{prefix}_summary.md").write_text("\n".join(lines) + "\n")


if __name__ == "__main__":
    main()
