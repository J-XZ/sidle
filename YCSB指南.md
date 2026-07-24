# D-SIDLE YCSB 指南

## 当前可用范围

`dsidle_run_ycsb_experiment.sh` 支持 1、2 或 4 VM 实验。它校验 workload 矩阵、
物化配置和 `run_meta.json`，以本仓固定的 YCSB-cpp 子模块生成 trace，并调用
VM 回放编排。镜像只构建一次；二进制和 ivshmem 模块在宿主的 Jammy 根文件系统
中构建一次，然后由 rsync 分发到所选 VM。load 与 run 都是每 VM 多 worker 并发
回放，load 不存在串行降级路径。

## 约束

- workload 只接受小写的 `a,b,c,d,e`；使用 `a,b,c,d,e` 可覆盖 YCSB-E 的
  Scan 路径。
- 派生配置的 VM 数由 `--vm-count 1|2|4` 指定（默认 4），HWCC 为 1024MiB，SWCC 使用共享池其余空间；共享池大小
  必须是至少 2048MiB 的 2 的幂。
- 正式性能对比使用 RelWithDebInfo、32B key/value、4 worker/节点、5M
  record/operation、`--shared-size-mb 65536 --no-latency`。在运行这一合同前，
  应以派生的 64GiB 配置重新启动 VM；不能让 32GiB VM 映射回放 64GiB 配置。
- 派生配置会将延迟缓存模型设为 `none` 且关闭 cache hit；`--no-latency` 还会
  关闭所有延迟计费开关。
- 默认先执行 1 个不计入汇总的预热轮次，再执行 `--rounds` 指定的正式轮次；可用
  `--warmup-rounds 0` 仅用于开发冒烟，不能用于 §6.4 正式性能结果。

## 已验证命令

以下命令已在当前版本验证。`mktemp` 使准备示例不会改动仓库目录。

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
`run_meta.json`。本示例会生成 4 个 load、A 和 E worker trace；A 的 UPDATE 会
展开为独立 GET 与 PUT。后者记录 git SHA、配置 SHA256、参数和复现命令。

实际 100k 验收回放（4 VM × 4 worker、load/workloada、连续 10 轮）的命令为：

```bash
./dsidle_build_vm_artifacts.sh
./dsidle_run_ycsb_experiment.sh \
  --vm-count 4 --warmup-rounds 1 --rounds 10 \
  --record-count 100000 --operation-count 100000 \
  --threads-per-node 4 --workloads a --no-latency
```

runner 日志可用下列命令生成 JSON、CSV 与报告：

```bash
out=$(mktemp -d /tmp/dsidle-ycsb-summary.XXXXXX)
python3 scripts/summarize_ycsb_experiment.py \
  --log-dir tests/data/ycsb_logs --out-dir "$out"
```

## 中断与失败处理

准备模式只创建指定输出目录；中断后可以选择新的 `--out-dir` 重跑。若只需物化
配置，可加 `--skip-trace-gen`。非法数值、非法 NUMA 列表、非 2 的幂共享大小或
空/重复/非法 workload 会以退出码 2 失败。完整执行失败后保留远端和本地日志；
先以 `dsidle_check_vms.sh` 检查 VM，再用新的输出目录重跑，不在 VM 内编译。
