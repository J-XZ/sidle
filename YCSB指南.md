# D-SIDLE YCSB 指南

## 当前可用范围

`dsidle_run_ycsb_experiment.sh` 固定面向 4 VM 实验。它会校验 workload
矩阵、物化本轮配置和 `run_meta.json`，并为将来的 trace/VM 回放保留统一的
产物目录。当前工作树固定了计划要求的 YCSB-cpp 子模块；首次使用前以递归方式
初始化它。`--prepare-only` 会生成 trace 和实验材料，且已验证。VM 初始化脚本
尚未实现实际 QEMU 启动，因此普通执行会在 trace 生成后明确失败而不会启动或清理 VM。

## 约束

- workload 只接受小写的 `a,b,c,d,e`；使用 `a,b,c,d,e` 可覆盖 YCSB-E 的
  Scan 路径。
- 派生配置固定 4 VM、HWCC 为 1024MiB，SWCC 使用共享池其余空间；共享池大小
  必须是至少 2048MiB 的 2 的幂。
- 正式性能对比应使用 RelWithDebInfo、32B key/value、4 worker/节点、5M
  record/operation、`--shared-size-mb 65536 --no-latency`。该完整流程尚未在
  本仓执行，不能将准备输出当作性能结果。
- 派生配置会将延迟缓存模型设为 `none` 且关闭 cache hit；`--no-latency` 还会
  关闭所有延迟计费开关。

## 已验证命令

以下初始化、帮助和准备命令已在当前版本执行验证。`mktemp` 使示例不会改动仓库目录。

```bash
git submodule update --init --recursive
./dsidle_run_ycsb_experiment.sh --help

out=$(mktemp -d /tmp/dsidle-ycsb-guide.XXXXXX)
./dsidle_run_ycsb_experiment.sh \
  --prepare-only --out-dir "$out/result" \
  --workloads a,e --record-count 10 --operation-count 20 \
  --threads-per-node 2 --round-timeout 30 \
  --shared-numa 1,2 --shared-size-mb 4096 --no-latency
```

成功时会输出 `DSIDLE_YCSB_PREPARED`。目录内包含 `configs/`、`traces/`、
`logs/`、`round_logs/`、派生的 `experiment_config_ycsb_4vm.jsonc` 和
`run_meta.json`。示例会生成 4 个 load、A 和 E worker trace；A 的 UPDATE 会
展开为独立 GET 与 PUT。后者记录 git SHA、配置 SHA256、参数和复现命令。

已有 runner 日志可通过下列已验证汇总命令生成 JSON、CSV 与报告：

```bash
out=$(mktemp -d /tmp/dsidle-ycsb-summary.XXXXXX)
python3 scripts/summarize_ycsb_experiment.py \
  --log-dir tests/data/ycsb_logs --out-dir "$out"
```

## 中断与失败处理

准备模式只创建指定输出目录；中断后可以选择新的 `--out-dir` 重跑。若只需物化
配置，可加 `--skip-trace-gen`。非法数值、
非法 NUMA 列表、非 2 的幂共享大小或空/重复/非法 workload 会以退出码 2 在
创建输出目录前失败。完整执行失败时不应手工假定 VM 已被启动：先使用
`dsidle_check_vms.sh --dry-run` 检查预期拓扑，待 VM 启动实现后再执行运行阶段。
