#!/usr/bin/env python3
"""Summarize D-SIDLE runner logs into reproducible YCSB artifacts."""
import argparse
import csv
import hashlib
import json
import math
import re
from pathlib import Path
from statistics import mean

TRACE = re.compile(
    r"E2E_TRACE_TIME_US phase=(?P<phase>\S+) node=(?P<node>\d+) "
    r"ops=(?P<ops>\d+) duration_us=(?P<duration>\d+) "
    r"trace_first=(?P<trace_first>\d+) trace_workers=(?P<trace_workers>\d+) "
    r"batch_ops=(?P<batch_ops>\d+)"
)
NAME = re.compile(
    r"(?P<case>load|workload[abcde])_round_(?P<round>\d+)_"
    r"(?P<stage>load|run)_node(?P<node>\d+)\.log$"
)

def percentile(values, fraction):
    """Nearest-rank percentile; deterministic even for a single formal round."""
    ordered = sorted(values)
    return ordered[max(0, math.ceil(len(ordered) * fraction) - 1)]

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--log-dir", required=True, type=Path)
    parser.add_argument("--out-dir", required=True, type=Path)
    parser.add_argument("--metadata", type=Path)
    args = parser.parse_args()
    metadata = json.loads(args.metadata.read_text()) if args.metadata else {}
    trace_manifest = None
    if metadata:
        manifest_path = Path(metadata["trace_manifest"])
        if not manifest_path.is_file():
            manifest_path = args.metadata.parent / "trace_manifest.json"
        content = manifest_path.read_bytes()
        expected_hash = metadata.get("trace_manifest_sha256")
        if expected_hash and hashlib.sha256(content).hexdigest() != expected_hash:
            raise SystemExit(f"trace manifest hash mismatch: {manifest_path}")
        trace_manifest = json.loads(content)
    raw = []
    for path in sorted(args.log_dir.glob("*_node*.log")):
        match = NAME.match(path.name)
        if not match: continue
        matches = []
        for line in path.read_text(errors="replace").splitlines():
            item = TRACE.fullmatch(line)
            if item:
                matches.append(item)
        if len(matches) != 1:
            raise SystemExit(f"{path}: expected exactly one complete E2E_TRACE_TIME_US row")
        item = matches[0]
        filename_node = int(match["node"])
        result_node = int(item["node"])
        if filename_node != result_node:
            raise SystemExit(f"{path}: filename node {filename_node} != result node {result_node}")
        raw.append({
            "case": match["case"],
            "round": int(match["round"]),
            "stage": match["stage"],
            "phase": item["phase"],
            "node": result_node,
            "ops": int(item["ops"]),
            "duration_us": int(item["duration"]),
            "trace_first": int(item["trace_first"]),
            "trace_workers": int(item["trace_workers"]),
            "batch_ops": int(item["batch_ops"]),
            "log": str(path),
        })
    if not raw: raise SystemExit("no E2E_TRACE_TIME_US rows found")
    grouped = {}
    for row in raw: grouped.setdefault((row["case"], row["round"], row["stage"]), []).append(row)
    if metadata:
        expected_groups = set()
        formal_rounds = range(1, int(metadata["rounds"]) + 1)
        if not metadata.get("skip_standalone_load"):
            expected_groups.update(("load", round_id, "load") for round_id in formal_rounds)
        for workload in metadata["workloads"]:
            case = f"workload{workload}"
            expected_groups.update((case, round_id, stage) for round_id in formal_rounds for stage in ("load", "run"))
        actual_groups = set(grouped)
        if actual_groups != expected_groups:
            missing = sorted(expected_groups - actual_groups)
            unexpected = sorted(actual_groups - expected_groups)
            raise SystemExit(f"formal result groups mismatch: missing={missing} unexpected={unexpected}")
    rounds = []
    for (case, round_id, stage), rows in sorted(grouped.items()):
        nodes = [row["node"] for row in rows]
        if len(nodes) != len(set(nodes)):
            raise SystemExit(f"{case} round {round_id} {stage}: duplicate node result")
        if metadata:
            expected_nodes = list(range(int(metadata["nodes"])))
            if sorted(nodes) != expected_nodes:
                raise SystemExit(
                    f"{case} round {round_id} {stage}: expected nodes {expected_nodes}, got {sorted(nodes)}"
                )
            phase = "load" if stage == "load" else case
            phase_manifest = trace_manifest["phases"][phase]
            workers = int(metadata["threads_per_node"])
            for row in rows:
                expected_ops = sum(
                    phase_manifest["workers"][str(worker)]["physical_command_count"]
                    for worker in range(row["node"] * workers, (row["node"] + 1) * workers)
                )
                expected_values = {
                    "phase": phase,
                    "ops": expected_ops,
                    "trace_first": row["node"] * workers,
                    "trace_workers": workers,
                }
                for key, value in expected_values.items():
                    if row[key] != value:
                        raise SystemExit(
                            f"{row['log']}: {key} mismatch: expected {value}, got {row[key]}"
                        )
                if row["batch_ops"] <= 0:
                    raise SystemExit(f"{row['log']}: invalid batch_ops={row['batch_ops']}")
        max_us = max(row["duration_us"] for row in rows)
        rounds.append({"case": case, "round": round_id, "stage": stage, "nodes": len(rows), "ops_sum": sum(row["ops"] for row in rows), "duration_sec_max": max_us / 1e6, "avg_duration_sec": mean(row["duration_us"] for row in rows) / 1e6})
    cases = []
    wanted = [row for row in rounds if (row["case"] == "load" and row["stage"] == "load") or (row["case"].startswith("workload") and row["stage"] == "run")]
    for key in sorted(set((row["case"], row["stage"]) for row in wanted)):
        rows = [row for row in wanted if (row["case"], row["stage"]) == key]
        avg_ops_sum = mean(row["ops_sum"] for row in rows)
        avg_duration_sec = mean(row["duration_sec_max"] for row in rows)
        per_round_ops = [row["ops_sum"] / row["duration_sec_max"] for row in rows]
        cases.append({"case": key[0], "stage": key[1], "rounds": len(rows), "ops_sum": avg_ops_sum, "duration_sec_max": avg_duration_sec, "avg_ops_sum": avg_ops_sum, "avg_duration_sec": avg_duration_sec, "ops_per_sec": avg_ops_sum / avg_duration_sec, "ops_per_sec_p50": percentile(per_round_ops, 0.50), "ops_per_sec_p90": percentile(per_round_ops, 0.90)})
    args.out_dir.mkdir(parents=True, exist_ok=True)
    (args.out_dir / "ycsb_summary.json").write_text(json.dumps({"metadata": metadata, "rounds": rounds, "cases": cases}, indent=2) + "\n")
    with (args.out_dir / "ycsb_summary.csv").open("w", newline="") as output:
        writer = csv.DictWriter(output, fieldnames=list(cases[0])); writer.writeheader(); writer.writerows(cases)
    lines = ["# YCSB 实验报告", "", "| case | stage | rounds | avg_ops_sum | avg_duration_sec | ops_per_sec | p50 | p90 |", "| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |"]
    lines += [f"| {row['case']} | {row['stage']} | {row['rounds']} | {row['avg_ops_sum']:.0f} | {row['avg_duration_sec']:.6f} | {row['ops_per_sec']:.2f} | {row['ops_per_sec_p50']:.2f} | {row['ops_per_sec_p90']:.2f} |" for row in cases]
    (args.out_dir / "YCSB实验报告.md").write_text("\n".join(lines) + "\n")

if __name__ == "__main__": main()
