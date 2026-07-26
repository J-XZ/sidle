# D-SIDLE

D-SIDLE 将 Masstree 改造为单一共享命名空间的多 VM 共享内存 KV。规范节点和
value 位于 SWCC，跨 VM 控制项位于 HWCC；每台 VM 可以有受预算约束的本地副本。
实验环境是 QEMU ivshmem 映射同一块宿主 NUMA DRAM 的模拟，不是实际 CXL 硬件。

## 从空白宿主机启动四台 VM

以下命令均在本仓根目录执行。镜像仅构建一次。VM init 与 cxlkv 对齐：共享内存
tmpfs NUMA 绑定、bridge/tap、guest 内编译加载 `ivpci` 驱动、QEMU taskset。

```bash
git submodule update --init --recursive
./dsidle_make_vm_img.sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j"$(nproc)"
./dsidle_init_vms.sh --apply-host-tuning --execute
./dsidle_check_vms.sh
```

`dsidle_make_vm_img.sh` 与 cxlkv 同构：调用本仓 `image/make_vm_img.sh`
（mkosi Jammy、`RootSize=38G`、钉死内核与完整 postinst）。

`dsidle_init_vms.sh` 使用本仓 `image/root.img`、`experiment_config.jsonc`。
默认会校验并报告宿主调优状态；从空白宿主启动时，上述
`--apply-host-tuning` 显式应用与 cxlkv 一致的 SMT/THP/NUMA balancing 等设置：

- 宿主机性能调优预检与可选应用
- 在 `shared_memory.numa_node` 上挂载 tmpfs（`mpol=bind`）并创建 ivshmem backing
- `dsidle_shared_pool --init-pool` 写入池元数据
- bridge/tap + 双网卡 QEMU + ivshmem-plain
- 向 guest rsync `third_party/ivshmem-kernel`，guest 内 `make` + `modprobe ivshmem_driver`
- 校验 BAR2 与 `/dev/ivpci0`（`ivpci` 驱动），再 taskset QEMU

结束时用 `./dsidle_kill_vms.sh --execute` 停止 QEMU、清空 `vm.storage_path`
内容、删除 ivshmem 产物并卸载共享内存 tmpfs（对齐 cxlkv `clear_vm_data`）。

## 端到端套件

08/09 的 VM 数据面套件使用 100k key。驱动已在 init 阶段装好；runner 使用宿主
`build/` 产物并 rsync 到 guest。

```bash
scripts/run_dsidle_vm_e2e_rounds.sh --execute --formal-acceptance \
  --suite 08 --rounds 10 \
  --runner build/dsidle_e2e_suite_runner \
  --pool-tool build/dsidle_shared_pool
scripts/run_dsidle_vm_e2e_rounds.sh --execute --formal-acceptance \
  --suite 09 --rounds 10 \
  --runner build/dsidle_e2e_suite_runner \
  --pool-tool build/dsidle_shared_pool
```

脚本每轮重置 pool，并在 host 与四台 guest 清 page cache、执行 4×64MiB CPU
cache sweep。它严格校验每个节点的 `TIME/ops/workers` 与 suite marker，记录
git/config SHA256 和每轮 exit code，并输出与 cxlkv 同口径的
`avg-round-max` JSON/CSV/Markdown 汇总。只有显式
`--formal-acceptance` 且精确满足冻结合同的运行才原子生成
`acceptance.meta`；开发运行只生成 `run_complete.meta`。正式清单绑定 git、
runner、pool tool、host/guest 配置、逐轮元数据、节点日志和汇总文件的 SHA256。

YCSB 的 load 也是并发写入：4 VM、每 VM 4 worker 同时回放；没有串行 load 路径。

```bash
./dsidle_run_ycsb_experiment.sh --formal-acceptance \
  --vm-count 4 --warmup-rounds 1 --rounds 10 \
  --record-count 100000 --operation-count 100000 \
  --threads-per-node 4 --workloads a,b,c,d,e \
  --shared-size-mb 32768 --no-latency
```

正式 4×4/100k 入口会将 YCSB 的随机操作划分显式归一化到冻结命令数，并在
`trace_normalization.json` 与 `trace_manifest.json` 中保存归一化前后计数和
逐文件/整体 SHA256；与 cxlkv 对比时必须复用这一组 worker trace。

完整参数、正式 100k 对比矩阵和输出汇总见 [YCSB指南.md](YCSB指南.md)。验收主套件
使用 100k；5M 是性能对比矩阵，不替代正确性验收。

## 验证与边界

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure --repeat until-fail:10
```

吞吐只计入 replay，不包含 pool 初始化或相位屏障；默认预热轮次也不会写入汇总。YCSB 汇总使用每轮节点最大耗时
和各轮均值；08/09 使用其各自的多轮字段。延迟注入仅在 RelWithDebInfo、关闭
verbose 和额外检查时允许开启；正式 YCSB 使用 `--no-latency`。
