#!/usr/bin/env python3
"""Require review when shared-memory access moves into a new source file."""

from pathlib import Path
import sys


SOURCE_ROOT = Path(sys.argv[1]).resolve()
AUDIT_ROOTS = (
    SOURCE_ROOT / "dsidle",
    SOURCE_ROOT / "third_party" / "masstree-beta",
)
TOKENS = (
    "SharedPoolBase",
    "SwccOffset<",
    "NodeRef",
    "static_layout()",
    "header()",
    "diagnostics()",
    ".base()",
)
ALLOWLIST = {
    "dsidle/e2e_suite_runner.cc",
    "dsidle/e2e_trace_runner.cc",
    "dsidle/node_control.h",
    "dsidle/replica_directory.cc",
    "dsidle/replica_directory.h",
    "dsidle/shard_allocator.cc",
    "dsidle/shard_allocator.h",
    "dsidle/shared_pool.cc",
    "dsidle/shared_pool.h",
    "third_party/masstree-beta/kvthread.cc",
    "third_party/masstree-beta/kvthread.hh",
    "third_party/masstree-beta/dsidle_value_access.hh",
    "third_party/masstree-beta/masstree.hh",
    "third_party/masstree-beta/masstree_get.hh",
    "third_party/masstree-beta/masstree_insert.hh",
    "third_party/masstree-beta/masstree_internal_replica.hh",
    "third_party/masstree-beta/masstree_remove.hh",
    "third_party/masstree-beta/masstree_replica.hh",
    "third_party/masstree-beta/masstree_replica_worker.hh",
    "third_party/masstree-beta/masstree_root_replica.hh",
    "third_party/masstree-beta/masstree_struct.hh",
    "third_party/masstree-beta/masstree_tcursor.hh",
    "third_party/masstree-beta/nodeversion.hh",
}


unexpected = []
for audit_root in AUDIT_ROOTS:
    for path in audit_root.rglob("*"):
        if path.suffix not in {".cc", ".h", ".hh"}:
            continue
        text = path.read_text(errors="replace")
        if not any(token in text for token in TOKENS):
            continue
        relative = path.relative_to(SOURCE_ROOT).as_posix()
        if relative not in ALLOWLIST:
            unexpected.append(relative)

if unexpected:
    raise SystemExit(
        "shared-memory access files require latency-wrapper review and "
        f"allowlisting: {', '.join(sorted(unexpected))}"
    )
