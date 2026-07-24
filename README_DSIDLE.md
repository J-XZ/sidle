# D-SIDLE

D-SIDLE 将 Masstree 改造为单一共享命名空间的多 VM 共享内存 KV。规范节点和
value 位于 SWCC，跨 VM 控制项位于 HWCC；每台 VM 可以有受预算约束的本地副本。
实验环境是 QEMU ivshmem 映射同一块宿主 NUMA DRAM 的模拟，不是实际 CXL 硬件。

## 从空白宿主机启动四台 VM

以下命令均在本仓根目录执行。镜像仅构建一次；D-SIDLE 程序在宿主用镜像内的
Jammy 工具链构建一次，再 rsync 到来宾，来宾不编译程序。

```bash
git submodule update --init --recursive
./dsidle_make_vm_img.sh
./dsidle_init_vms.sh
./dsidle_check_vms.sh
./dsidle_build_vm_artifacts.sh
```

`dsidle_init_vms.sh` 使用本仓 `image/root.img`、`experiment_config.jsonc` 和
ivshmem-plain 创建 VM；不读取或执行任何同级仓库的镜像、脚本或构建产物。
结束时用 `./dsidle_kill_vms.sh` 精确停止由该脚本记录 PID 的 VM。

## 端到端套件

08/09 的 VM 数据面套件使用 100k key。下面使用已经构建的宿主产物；每个命令
会把程序、内核模块和来宾配置 rsync 到四个 VM。

```bash
scripts/run_dsidle_vm_e2e_rounds.sh --execute --suite 08 --rounds 10 \
  --runner build-jammy/dsidle_e2e_suite_runner \
  --ivshmem-module build-jammy/ivshmem_driver.ko \
  --pool-tool build/dsidle_shared_pool
scripts/run_dsidle_vm_e2e_rounds.sh --execute --suite 09 --rounds 10 \
  --runner build-jammy/dsidle_e2e_suite_runner \
  --ivshmem-module build-jammy/ivshmem_driver.ko \
  --pool-tool build/dsidle_shared_pool
```

YCSB 的 load 也是并发写入：4 VM、每 VM 4 worker 同时回放；没有串行 load 路径。

```bash
./dsidle_run_ycsb_experiment.sh --vm-count 4 --warmup-rounds 1 --rounds 10 \
  --record-count 100000 --operation-count 100000 \
  --threads-per-node 4 --workloads a --no-latency
```

完整参数、正式 5M 对比矩阵和输出汇总见 [YCSB指南.md](YCSB指南.md)。验收主套件
使用 100k；5M 是性能对比矩阵，不替代正确性验收。

## 验证与边界

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
```

吞吐只计入 replay，不包含 pool 初始化或相位屏障；默认预热轮次也不会写入汇总。YCSB 汇总使用每轮节点最大耗时
和各轮均值；08/09 使用其各自的多轮字段。延迟注入仅在 RelWithDebInfo、关闭
verbose 和额外检查时允许开启；正式 YCSB 使用 `--no-latency`。
