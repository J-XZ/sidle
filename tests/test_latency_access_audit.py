#!/usr/bin/env python3
"""Guards the fixed-latency access coverage for D-SIDLE.

The file-level allowlist is only a discovery gate for new files.  This script
also enforces site-level rules for the known coverage gaps so they cannot be
silently reintroduced:

* the SWCC value-range classifier (dsidle_value_access.hh) must never
  dereference the pool header/static layout (it only reads the thread-local
  range cache published at binding);
* the allocator's free-object generation read must go through the SWCC
  wrapper (one real access, one charge);
* E2E runners must not wrap whole replays or whole phases in one scope, and
  the trailing rcu_drain() must have its own short scope;
* every worker binding after the simulator is enabled must run inside a
  short scope.

Runtime proofs for the same gaps live in the C++ tests (binding without a
scope hard fails; binding with a scope charges HWCC; remap only recognizes
the new SWCC range; HarvestRemote's generation read charges exactly one SWCC
access; the fixed-latency trace-runner smoke passes without no-scope fails).
"""

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
    "third_party/masstree-beta/btree_leaflink.hh",
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

errors = []


def fail(message):
    errors.append(message)


def lines_of(relative):
    return (
        SOURCE_ROOT / relative
    ).read_text(errors="replace").splitlines()


def classify():
    # 1. The value-range classifier must never touch the pool header/static
    #    layout: those are HWCC reads that would add hidden shared-memory
    #    traffic (and recursive/repeated charging) on every value operation.
    classifier_tokens = (
        "header()",
        "static_layout()",
        "swcc_offset",
        "swcc_bytes",
        "SharedPoolBase",
        "CurrentSharedPool",
    )
    for index, line in enumerate(
        lines_of("third_party/masstree-beta/dsidle_value_access.hh"), 1
    ):
        if any(token in line for token in classifier_tokens):
            fail(
                "dsidle_value_access.hh:%d dereferences the pool header/"
                "static layout; the classifier must only read the cached "
                "thread-local SWCC range: %s" % (index, line.strip())
            )

    # 2. Every free-object generation access in the allocator must go through
    #    the typed wrapper so the read is charged exactly once.
    allocator_lines = lines_of("dsidle/shard_allocator.cc")
    for index, line in enumerate(allocator_lines, 1):
        if "->generation" not in line:
            continue
        window = "\n".join(
            allocator_lines[max(0, index - 2):index + 1]
        )
        if "FixedLatencyMemory" not in window:
            fail(
                "shard_allocator.cc:%d raw FreeObjectHeader generation "
                "dereference; use FixedLatencyMemoryLoad(kSwcc, ...): %s"
                % (index, line.strip())
            )

    trace_lines = lines_of("dsidle/e2e_trace_runner.cc")
    suite_lines = lines_of("dsidle/e2e_suite_runner.cc")

    # 3. No whole-replay or whole-phase scopes: they would swallow every
    #    per-operation/per-task scope and defer all settling to program end.
    for index, line in enumerate(trace_lines, 1):
        if "worker_scope" in line or "runner_scope" in line:
            fail(
                "e2e_trace_runner.cc:%d coarse replay/phase scope would "
                "swallow per-operation scopes: %s" % (index, line.strip())
            )
    for index, line in enumerate(suite_lines, 1):
        if "runner_scope" in line:
            fail(
                "e2e_suite_runner.cc:%d coarse phase scope would swallow "
                "per-operation scopes: %s" % (index, line.strip())
            )

    # 4. The trailing rcu_drain() in ReplayFile needs its own short scope that
    #    settles after the drain returns.
    for index, line in enumerate(trace_lines, 1):
        if "ti->rcu_drain()" not in line:
            continue
        window = "\n".join(trace_lines[max(0, index - 4):index])
        if "ScopeGuard" not in window:
            fail(
                "e2e_trace_runner.cc:%d rcu_drain() must run inside a short "
                "scope that settles after the drain: %s"
                % (index, line.strip())
            )

    # 5. Every worker binding after the simulator is enabled must run inside
    #    a short scope.  Main-thread bindings before ConfigureLatencySimulator
    #    ForPool are the explicit startup boundary and are exempt.
    def binding_scope_check(lines, relative):
        enable_line = next(
            (
                index
                for index, line in enumerate(lines, 1)
                if "ConfigureLatencySimulatorForPool" in line
            ),
            None,
        )
        for index, line in enumerate(lines, 1):
            if "ConfigureCurrentSwccAllocator" not in line:
                continue
            if enable_line is not None and index < enable_line:
                continue  # pre-enable startup boundary on the main thread
            window = "\n".join(lines[max(0, index - 6):index])
            if "ScopeGuard" not in window:
                fail(
                    "%s:%d worker binding outside a short scope while "
                    "fixed latency is enabled: %s"
                    % (relative, index, line.strip())
                )

    binding_scope_check(trace_lines, "dsidle/e2e_trace_runner.cc")
    binding_scope_check(suite_lines, "dsidle/e2e_suite_runner.cc")
    binding_scope_check(
        lines_of("third_party/masstree-beta/masstree_replica_worker.hh"),
        "third_party/masstree-beta/masstree_replica_worker.hh",
    )


def allowlist_gate():
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
        fail(
            "shared-memory access files require latency-wrapper review and "
            "allowlisting: %s" % ", ".join(sorted(unexpected))
        )


classify()
allowlist_gate()
if errors:
    raise SystemExit("\n".join(errors))
