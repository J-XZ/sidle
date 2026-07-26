#!/usr/bin/env python3
"""Normalize the frozen 4-VM/100k YCSB trace contract deterministically."""

import argparse
import hashlib
import json
import re
from collections import Counter
from pathlib import Path


TARGETS = {
    "workloada": Counter(GET=100000, PUT=50199),
    "workloadb": Counter(GET=95019, PUT=4981),
    "workloadc": Counter(GET=100000),
    "workloadd": Counter(GET=95072, PUT=4928),
    "workloade": Counter(SCAN=94920, PUT=5080),
}
COMMAND = re.compile(r"^(PUT|GET|SCAN) ([1-9][0-9]*) (.*)$")


def parse_command(line):
    match = COMMAND.fullmatch(line)
    if not match:
        return None
    operation, key_length_text, payload = match.groups()
    key_length = int(key_length_text)
    if len(payload) < key_length:
        raise SystemExit(f"trace key is shorter than declared length: {line!r}")
    argument, key = payload[:-key_length], payload[-key_length:]
    if not argument.isdigit():
        raise SystemExit(f"trace argument is not numeric: {line!r}")
    return operation, key_length, argument, key


def render(operation, key, argument=None):
    if argument is None:
        argument = "32" if operation == "PUT" else "0"
    return f"{operation} {len(key)} {argument}{key}"


def evenly_spaced(items, count):
    if count < 0 or count > len(items):
        raise SystemExit(
            f"cannot select {count} commands from {len(items)} candidates"
        )
    if not count:
        return []
    return [items[((2 * index + 1) * len(items)) // (2 * count)]
            for index in range(count)]


def fnv_hash64(value):
    result = 0xCBF29CE484222325
    for _ in range(8):
        result ^= value & 0xFF
        result = (result * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
        value >>= 8
    return result


def ycsb_key(sequence):
    value = str(fnv_hash64(sequence))
    return "user" + ("0" * max(0, 16 - len(value))) + value


def load_phase(directory, workers):
    paths = [directory / f"worker{worker}.txt" for worker in range(workers)]
    if any(not path.is_file() for path in paths):
        raise SystemExit(f"{directory}: expected worker0..worker{workers - 1}")
    lines = {path: path.read_text(errors="strict").splitlines() for path in paths}
    return paths, lines


def commands(paths, lines):
    result = []
    for path in paths:
        for index, line in enumerate(lines[path]):
            parsed = parse_command(line)
            if parsed:
                result.append((path, index, parsed))
    return result


def counts(paths, lines):
    return Counter(item[2][0] for item in commands(paths, lines))


def normalize_a(paths, lines, target_puts):
    current = counts(paths, lines)["PUT"]
    if current > target_puts:
        pairs = []
        for path, index, parsed in commands(paths, lines):
            if parsed[0] != "PUT" or index == 0:
                continue
            previous = parse_command(lines[path][index - 1])
            if previous and previous[0] == "GET" and previous[3] == parsed[3]:
                pairs.append((path, index))
        for path, index in evenly_spaced(pairs, current - target_puts):
            lines[path][index] = None
    elif current < target_puts:
        standalone = []
        for path, index, parsed in commands(paths, lines):
            if parsed[0] != "GET":
                continue
            following = (
                parse_command(lines[path][index + 1])
                if index + 1 < len(lines[path])
                else None
            )
            if not following or following[0] != "PUT" or following[3] != parsed[3]:
                standalone.append((path, index, parsed[3]))
        selected = evenly_spaced(standalone, target_puts - current)
        by_path = {}
        for path, index, key in selected:
            by_path.setdefault(path, []).append((index, key))
        for path, additions in by_path.items():
            for index, key in reversed(additions):
                lines[path].insert(index + 1, render("PUT", key))
    return abs(current - target_puts)


def normalize_update_mix(paths, lines, target_puts):
    current = counts(paths, lines)["PUT"]
    source = "PUT" if current > target_puts else "GET"
    destination = "GET" if source == "PUT" else "PUT"
    candidates = [
        (path, index, parsed[3])
        for path, index, parsed in commands(paths, lines)
        if parsed[0] == source
    ]
    for path, index, key in evenly_spaced(candidates, abs(current - target_puts)):
        lines[path][index] = render(destination, key)
    return abs(current - target_puts)


def normalize_insert_mix(phase, paths, lines, target_puts, record_count):
    current = counts(paths, lines)["PUT"]
    changes = abs(current - target_puts)
    if current < target_puts:
        read_operation = "GET" if phase == "workloadd" else "SCAN"
        candidates = [
            (path, index)
            for path, index, parsed in commands(paths, lines)
            if parsed[0] == read_operation
        ]
        for offset, (path, index) in enumerate(
            evenly_spaced(candidates, target_puts - current)
        ):
            lines[path][index] = render(
                "PUT", ycsb_key(record_count + current + offset)
            )
    elif current > target_puts:
        operation = "GET" if phase == "workloadd" else "SCAN"
        existing_key = ycsb_key(0)
        candidates = [
            (path, index)
            for path, index, parsed in commands(paths, lines)
            if parsed[0] == "PUT"
        ]
        for path, index in evenly_spaced(candidates, current - target_puts):
            argument = "32" if operation == "SCAN" else None
            lines[path][index] = render(operation, existing_key, argument)
    return changes


def trace_set_hash(root, phases, workers):
    rows = []
    for phase in phases:
        for worker in range(workers):
            path = root / phase / f"worker{worker}.txt"
            rows.append(
                f"{phase}/worker{worker}.txt:"
                f"{hashlib.sha256(path.read_bytes()).hexdigest()}\n"
            )
    return hashlib.sha256("".join(rows).encode()).hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--trace-root", required=True, type=Path)
    parser.add_argument("--workloads", required=True)
    parser.add_argument("--nodes", required=True, type=int)
    parser.add_argument("--threads-per-node", required=True, type=int)
    parser.add_argument("--record-count", required=True, type=int)
    parser.add_argument("--operation-count", required=True, type=int)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    workloads = args.workloads.split(",")
    if (
        args.nodes,
        args.threads_per_node,
        args.record_count,
        args.operation_count,
    ) != (4, 4, 100000, 100000):
        raise SystemExit("normalization is restricted to the formal 4x4/100k contract")
    if not workloads or any(item not in "abcde" for item in workloads):
        raise SystemExit("workloads must be a comma-separated subset of a,b,c,d,e")

    workers = args.nodes * args.threads_per_node
    phases = ["load"] + [f"workload{item}" for item in workloads]
    before_hash = trace_set_hash(args.trace_root, phases, workers)
    report = {
        "algorithm": "dsidle-ycsb-formal-count-normalization-v1",
        "scope": "4 VMs x 4 workers, recordcount=operationcount=100000 only",
        "reason": (
            "YCSB-cpp operation selection is random_device-seeded; normalize "
            "the generated sample to the frozen cross-system physical counts"
        ),
        "input_trace_set_sha256": before_hash,
        "phases": {},
    }
    for workload in workloads:
        phase = f"workload{workload}"
        paths, lines = load_phase(args.trace_root / phase, workers)
        before = counts(paths, lines)
        target = TARGETS[phase]
        if workload == "a":
            changed = normalize_a(paths, lines, target["PUT"])
        elif workload == "b":
            changed = normalize_update_mix(paths, lines, target["PUT"])
        elif workload in ("d", "e"):
            changed = normalize_insert_mix(
                phase, paths, lines, target["PUT"], args.record_count
            )
        else:
            changed = 0
        for path in paths:
            path.write_text(
                "\n".join(line for line in lines[path] if line is not None) + "\n"
            )
        after = counts(paths, lines)
        if after != target:
            raise SystemExit(f"{phase}: expected {dict(target)}, got {dict(after)}")
        report["phases"][phase] = {
            "before": dict(sorted(before.items())),
            "target": dict(sorted(target.items())),
            "after": dict(sorted(after.items())),
            "changed_logical_operations": changed,
        }
    report["output_trace_set_sha256"] = trace_set_hash(
        args.trace_root, phases, workers
    )
    args.output.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")


if __name__ == "__main__":
    main()
