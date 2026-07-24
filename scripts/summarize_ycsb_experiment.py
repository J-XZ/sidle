#!/usr/bin/env python3
"""Summarize D-SIDLE runner logs into reproducible YCSB artifacts."""
import argparse
import csv
import json
import re
from pathlib import Path
from statistics import mean

TRACE = re.compile(r"E2E_TRACE_TIME_US phase=(?P<phase>\S+) node=(?P<node>\d+) ops=(?P<ops>\d+) duration_us=(?P<duration>\d+)")
NAME = re.compile(r"(?P<case>load|workload[abcde])_round_(?P<round>\d+)_(?P<stage>load|run)_node\d+\.log$")

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--log-dir", required=True, type=Path)
    parser.add_argument("--out-dir", required=True, type=Path)
    parser.add_argument("--metadata", type=Path)
    args = parser.parse_args()
    metadata = json.loads(args.metadata.read_text()) if args.metadata else {}
    raw = []
    for path in sorted(args.log_dir.glob("*_node*.log")):
        match = NAME.match(path.name)
        if not match: continue
        for line in path.read_text(errors="replace").splitlines():
            item = TRACE.search(line)
            if item:
                raw.append({"case": match["case"], "round": int(match["round"]), "stage": match["stage"], "node": int(item["node"]), "ops": int(item["ops"]), "duration_us": int(item["duration"]), "log": str(path)})
    if not raw: raise SystemExit("no E2E_TRACE_TIME_US rows found")
    grouped = {}
    for row in raw: grouped.setdefault((row["case"], row["round"], row["stage"]), []).append(row)
    rounds = []
    for (case, round_id, stage), rows in sorted(grouped.items()):
        max_us = max(row["duration_us"] for row in rows)
        rounds.append({"case": case, "round": round_id, "stage": stage, "nodes": len(rows), "ops_sum": sum(row["ops"] for row in rows), "duration_sec_max": max_us / 1e6, "avg_duration_sec": mean(row["duration_us"] for row in rows) / 1e6})
    cases = []
    wanted = [row for row in rounds if (row["case"] == "load" and row["stage"] == "load") or (row["case"].startswith("workload") and row["stage"] == "run")]
    for key in sorted(set((row["case"], row["stage"]) for row in wanted)):
        rows = [row for row in wanted if (row["case"], row["stage"]) == key]
        avg_ops_sum = mean(row["ops_sum"] for row in rows)
        avg_duration_sec = mean(row["duration_sec_max"] for row in rows)
        cases.append({"case": key[0], "stage": key[1], "rounds": len(rows), "ops_sum": avg_ops_sum, "duration_sec_max": avg_duration_sec, "avg_ops_sum": avg_ops_sum, "avg_duration_sec": avg_duration_sec, "ops_per_sec": avg_ops_sum / avg_duration_sec})
    args.out_dir.mkdir(parents=True, exist_ok=True)
    (args.out_dir / "ycsb_summary.json").write_text(json.dumps({"metadata": metadata, "rounds": rounds, "cases": cases}, indent=2) + "\n")
    with (args.out_dir / "ycsb_summary.csv").open("w", newline="") as output:
        writer = csv.DictWriter(output, fieldnames=list(cases[0])); writer.writeheader(); writer.writerows(cases)
    lines = ["# YCSB 实验报告", "", "| case | stage | rounds | avg_ops_sum | avg_duration_sec | ops_per_sec |", "| --- | --- | ---: | ---: | ---: | ---: |"]
    lines += [f"| {row['case']} | {row['stage']} | {row['rounds']} | {row['avg_ops_sum']:.0f} | {row['avg_duration_sec']:.6f} | {row['ops_per_sec']:.2f} |" for row in cases]
    (args.out_dir / "YCSB实验报告.md").write_text("\n".join(lines) + "\n")

if __name__ == "__main__": main()
