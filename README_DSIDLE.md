# D-SIDLE VM 操作手册

本手册只覆盖当前 D-SIDLE 分布式路径。VM 使用一块业务 ivshmem：
/dev/ivpci0；共享池由 dsidle_shared_pool --init-pool 初始化。没有额外设备或
backing。

## 干净启动

~~~bash
cd /root/code/sidle
git submodule update --init --recursive
./dsidle_make_vm_img.sh
cmake -S . -B build-jammy -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build-jammy --parallel 4
./dsidle_kill_vms.sh --config experiment_config.jsonc --execute
./dsidle_init_vms.sh --config experiment_config.jsonc --execute
./dsidle_check_vms.sh --config experiment_config.jsonc
~~~

dsidle_init_vms.sh 使用 experiment_config.jsonc、image/root.img、本仓
pool tool 和本仓 QEMU 运行目录。测试前应确认其它项目的 QEMU、ivshmem 服务和
PID 文件已停止；测试后立即执行 kill 脚本。

根 experiment_config.jsonc 的默认设备是 R6525 2-NUMA：VM 使用 NUMA0 连续 CPU
`0..31`，共享池使用 NUMA1，共 4 VM、每 VM 8 vCPU 和 4 个前台 worker。配置中的
HWCC 为 1024 MiB，SWCC 为剩余容量；共享池元数据只占普通
业务 metadata。

## E2E

先跑一轮无延迟代表性套件：

~~~bash
scripts/run_dsidle_vm_e2e_rounds.sh --execute --suite 08 --rounds 1 \
  --config experiment_config.jsonc --runner build-jammy/dsidle_e2e_suite_runner \
  --pool-tool build-jammy/dsidle_shared_pool
~~~

需要固定延迟 canary 时，复制本仓配置并只修改六个 fixed latency 字段；使用
小 trace、短 timeout 和一轮回放。canary 需要证明前台操作和 replica/merge
后台线程都能退出，不需要访问次数或模拟统计输出。

## 清理

~~~bash
./dsidle_kill_vms.sh --config experiment_config.jsonc --execute
~~~

该脚本停止本仓 QEMU，卸载本仓共享内存并清理本仓 VM writable copies。它不触碰
其它项目目录。若脚本拒绝执行，先检查配置中的绝对路径和本仓 PID 文件。

## 常用检查

~~~bash
git diff --check
ctest --test-dir build-jammy --output-on-failure
~~~

固定延迟只能在 RelWithDebInfo 启用；Debug 用于协议和 parser 测试。
