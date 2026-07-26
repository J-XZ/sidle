# Archived upstream benchmarks

This directory preserves the original single-node SIDLE workloads as design
reference. It is not a D-SIDLE build entry point.

The old workloads depend on `src/kv/` and `src/helper.*`, whose constructor,
allocator, background-worker, and fixed CPU-affinity contracts no longer match
the distributed implementation. They are deliberately isolated instead of
being partially repaired into a second, semantically different benchmark
stack. Use the repository-root CMake build and the `dsidle_e2e_*_runner`
targets for all D-SIDLE/cxlkv comparisons.
