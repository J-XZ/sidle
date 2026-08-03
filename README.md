# D-SIDLE

D-SIDLE 是基于 Masstree 的多 VM 共享内存 KV 实验实现。规范节点和值位于
SWCC，共享控制项、epoch 和 phase barrier 位于 HWCC；副本目录和策略线程使用
进程本地 DRAM。QEMU 通过一块业务 ivshmem 映射共享池，配置中的 HWCC/SWCC
容量就是实际业务布局，不包含隐藏的额外预留。

当前硬件模拟只有固定延迟机制，完整约束见
[硬件模拟当前实现.md](硬件模拟当前实现.md) 和
[延迟插入审计报告.md](延迟插入审计报告.md)。配置解析器对未知、重复、缺失和
遗留字段 hard fail。

## 构建

~~~bash
git submodule update --init --recursive
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel 4
ctest --test-dir build --output-on-failure
~~~

启用固定延迟必须使用 RelWithDebInfo，并保持 dsidle.verbose=false 和
dsidle.extra_check=false：

~~~bash
cmake -S . -B build-jammy -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build-jammy --parallel 4
ctest --test-dir build-jammy --output-on-failure
~~~

## 固定延迟配置

dsidle.latency_inject 只接受下面的 fixed_latency 对象：

~~~jsonc
"latency_inject": {
  "fixed_latency": {
    "enabled": false,
    "cache_line_bytes": 64,
    "swcc_fixed_ns_per_line": 0,
    "hwcc_fixed_ns_per_line": 0,
    "foreground_enabled": true,
    "background_enabled": true
  }
}
~~~

每次真实 SWCC/HWCC 访问按覆盖的 cache line 数累加线程本地 pending delay：

~~~text
pending_delay_ns += touched_swcc_lines * swcc_fixed_ns_per_line
pending_delay_ns += touched_hwcc_lines * hwcc_fixed_ns_per_line
~~~

延迟在退出业务锁、RCU、epoch、allocator 和发布临界区后的 ScopeGuard 安全点
使用校准 TSC busy-wait 补齐。前台和后台线程各自拥有 scope 和 pending delay。
关闭时只保留一个进程本地不可变 feature gate，不创建 TLS、校准时钟或共享状态。

## R6525 VM 验证

experiment_config.jsonc 是本机双 NUMA R6525 配置：VM 使用 NUMA0 偶数 CPU，
共享池使用 NUMA1，业务设备只有 /dev/ivpci0。使用本仓自己的镜像、pool tool
和脚本：

~~~bash
./dsidle_make_vm_img.sh
cmake -S . -B build-jammy -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build-jammy --parallel 4
./dsidle_init_vms.sh --config experiment_config.jsonc --execute
./dsidle_check_vms.sh --config experiment_config.jsonc
scripts/run_dsidle_vm_e2e_rounds.sh --execute --suite 08 --rounds 1 \
  --config experiment_config.jsonc --runner build-jammy/dsidle_e2e_suite_runner \
  --pool-tool build-jammy/dsidle_shared_pool
./dsidle_kill_vms.sh --config experiment_config.jsonc --execute
~~~

YCSB 只在需要时生成本仓 trace；开发验证默认一轮。无延迟回归后，可以用
configs/latency/fixed_latency_canary.jsonc 做小规模非零延迟验证。

## 目录说明

- dsidle/：共享池、分配器、epoch、复制和 runner 运行时。
- third_party/masstree-beta/：D-SIDLE 所需的 Masstree 访问适配。
- scripts/：trace、YCSB、VM 和结果汇总脚本。
- tests/：固定延迟、共享池、协议、脚本和静态审计测试。
- image/root.img：本仓 canonical guest image；其它镜像和缓存可重新生成。

上游单机 benchmark 目录保留为参考，不是当前分布式构建或 VM 验证入口。
