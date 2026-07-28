# D-SIDLE YCSB 指南

## 最小可用命令：

cd /root/code/sidle
./dsidle_make_vm_img.sh
./dsidle_init_vms.sh --apply-host-tuning --execute

OUT=exp_data/dsidle-ycsb-1m-4vm4t-$(date -u +%Y%m%dT%H%M%SZ)
./dsidle_run_ycsb_experiment.sh \
  --out-dir "$OUT" \
  --vm-count 4 --threads-per-node 4 \
  --record-count 1000000 --operation-count 1000000 \
  --workloads a,b,c,d,e \
  --shared-size-mb 32768 \
  --warmup-rounds 0 --rounds 1 --round-timeout 14400 \
  --no-latency

# 结束后
./dsidle_kill_vms.sh --execute

## 当前可用范围

`dsidle_run_ycsb_experiment.sh` 支持 1、2 或 4 VM 实验。它校验 workload 矩阵、
物化配置和 `run_meta.json`，以本仓固定的 YCSB-cpp 子模块生成 trace，并调用
VM 回放编排。镜像只构建一次；二进制和 ivshmem 模块在宿主的 Jammy 根文件系统
中构建一次，然后由 rsync 分发到所选 VM。load 与 run 都是每 VM 多 worker 并发
回放，load 不存在串行降级路径。

## 约束

- workload 只接受小写的 `a,b,c,d,e`；使用 `a,b,c,d,e` 可覆盖 YCSB-E 的
  Scan 路径。
- 派生配置的 VM 数由 `--vm-count 1|2|4` 指定（默认 4），HWCC 为 1024MiB，
  SWCC 使用共享池其余空间。共享池大小取自 base config，或由
  `--shared-size-mb` 显式覆盖；最终值必须是至少 2048MiB 的 2 的幂。正式对比
  须显式固定相同池大小。
- 本轮与 cxlkv 的默认验收合同使用 RelWithDebInfo、固定 32B key/value、
  4 worker/节点、100,000 record/operation、每 VM 8 vCPU。更大数据集属于另行
  声明的扩展实验，不能混入本轮 100k 结果。
- 派生配置会将延迟缓存模型设为 `none` 且关闭 cache hit；`--no-latency` 还会
  关闭所有延迟计费开关。
- 默认先执行 1 个不计入汇总的预热轮次，再执行 `--rounds` 指定的正式轮次；可用
  `--warmup-rounds 0` 仅用于开发冒烟，不能用于 §6.4 正式性能结果。
- 每 VM 保留原 SIDLE 的 5 个后台角色（Trigger、Promotion、Demotion、Cooler、
  Adjuster），另有 1 个 heartbeat 线程。4 foreground/8 vCPU 合同因此实际有
  10 个回放进程线程，会发生明确披露的 guest 调度过订；为尊重原实现，不通过
  删除后台角色来隐藏该成本。只有前四个后台角色进入 epoch，所以共享池精确分配
  `--threads-per-node + 4` 个 epoch slot，而不是旧文档所写的 `+2`。
- 独立 load（除非指定 `--skip-standalone-load`）和每个 workload 的每一轮都会
  使用新的共享池；workload 轮执行 `reset pool → clear VM caches → load → run`，
  不会让 A–E 在同一棵累积状态的树上连续回放。

## 已验证命令

以下命令已在当前版本验证。`mktemp` 使准备示例不会改动仓库目录。

```bash
git submodule update --init --recursive
./dsidle_run_ycsb_experiment.sh --help

out=$(mktemp -d /tmp/dsidle-ycsb-guide.XXXXXX)
./dsidle_run_ycsb_experiment.sh --formal-acceptance \
  --prepare-only --out-dir "$out/result" \
  --workloads a,e --record-count 10 --operation-count 20 \
  --threads-per-node 2 --round-timeout 30 \
  --shared-numa 1 --shared-size-mb 4096 --no-latency
```

成功时会输出 `DSIDLE_YCSB_PREPARED`。目录内包含 `configs/`、`traces/`、
`logs/`、`round_logs/`、派生的 `experiment_config_ycsb_4vm.jsonc` 和
`run_meta.json` 与 `trace_manifest.json`。本示例会分别生成 8 个 load、A 和 E
worker trace；A 的 UPDATE 会展开为独立 GET 与 PUT。manifest 记录每个 worker
的物理命令数、操作类型、SHA256、固定 32B key/value、value seed、生成参数与
整个 trace 集合的 SHA256；
`run_meta.json` 还记录 git/config/manifest SHA256、线程与 epoch 槽预算及复现命令。

实际 100k 验收回放（4 VM × 4 worker、独立 load 与 A–E 各 10 个正式轮）的命令为：

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j"$(nproc)" --target dsidle_e2e_trace_runner dsidle_shared_pool
./dsidle_run_ycsb_experiment.sh \
  --vm-count 4 --warmup-rounds 1 --rounds 10 \
  --record-count 100000 --operation-count 100000 \
  --threads-per-node 4 --workloads a,b,c,d,e \
  --shared-size-mb 32768 --no-latency
```

YCSB-cpp 的混合操作选择由 `random_device` 播种；为复现本轮冻结合同，脚本仅在
4 VM×4 worker 且 record/operation 均为 100,000 时执行显式 deterministic
normalization：

- load：100,000 PUT；
- A：100,000 GET + 50,199 PUT，共 150,199 条物理命令；
- B：95,019 GET + 4,981 PUT；
- C：100,000 GET；
- D：95,072 GET + 4,928 PUT；
- E：94,920 SCAN + 5,080 PUT。

`trace_normalization.json` 保存上游随机样本、目标、实际修改数和归一化前后
trace-set SHA；`trace_manifest.json` 再严格核对上述计数及逐 worker SHA。公平
比较必须只生成一次，并把同一组 worker 文件原样交给 cxlkv 与 D-SIDLE。其他规模
不做归一化，manifest 记录其原始随机划分。

`--formal-acceptance` 是 fail-closed 开关：它要求 tracked worktree 干净，并
锁定 1 个预热轮、10 个正式轮、4 VM×4 worker、每 VM 8 vCPU、32GiB、
独立 load、A–E、100k/100k 与 `--no-latency`。完成后才生成
`acceptance.meta`，其中绑定 Git SHA、runner/pool/config/trace/normalization、
每个 phase 的逐轮状态、四节点日志与汇总 SHA256；普通开发运行只生成
`run_complete.meta`。已有任一完成标记的目录不会被复用。

runner 日志可用下列命令生成 JSON、CSV 与报告：

```bash
out=$(mktemp -d /tmp/dsidle-ycsb-summary.XXXXXX)
python3 scripts/summarize_ycsb_experiment.py \
  --log-dir tests/data/ycsb_logs --out-dir "$out"
```

汇总的 `ops_per_sec` 使用各轮节点最大耗时的均值；`ops_per_sec_p50` 与
`ops_per_sec_p90` 则基于单轮吞吐计算，用于报告离散度。预热轮不参与这些字段。

## 中断与失败处理

准备模式只创建指定输出目录；中断后可以选择新的 `--out-dir` 重跑。
`--skip-trace-gen` 只用于复用同一输出目录中已经存在且通过 manifest 命令数校验
的 trace，不能在空目录中充当“仅物化配置”。非法数值、非法 NUMA 列表、缺失/
不匹配 trace、非 2 的幂共享大小或空/重复/非法 workload 会失败。完整执行失败后
保留远端和本地日志；先以 `dsidle_check_vms.sh` 检查 VM，再用新的输出目录重跑，
不在 VM 内编译。
