# D-SIDLE YCSB 指南

## 开发验证

YCSB 脚本默认一轮，适合默认 CloudLab R6525 2-NUMA 设备的短验证；根配置使用 VM
NUMA0、共享池 NUMA1，无延迟是默认比较基线：

~~~bash
cd /root/code/sidle
./dsidle_run_ycsb_experiment.sh \
  --out-dir /tmp/dsidle-ycsb-canary \
  --vm-count 4 --threads-per-node 1 \
  --record-count 10000 --operation-count 4000 \
  --workloads a --warmup-rounds 0 --rounds 1 \
  --shared-size-mb 32768 --no-latency
./dsidle_kill_vms.sh --config experiment_config.jsonc --execute
~~~

测试前必须使用本仓 kill/init 脚本重建四台 VM；不复用其它项目的镜像、backing、
trace 或运行目录。非零固定延迟 canary 使用本仓独立配置和本仓生成的 trace。

## 配置和 trace

vm-count 支持 1、2、4；threads-per-node 是前台回放 worker 数。共享池大小必须
是至少 2048 MiB 的 2 的幂，HWCC 固定为 1024 MiB，SWCC 使用剩余容量。固定
key/value 默认 32B。prepare-only 只生成派生配置、trace 和 manifest，不启动 VM：

~~~bash
out=$(mktemp -d /tmp/dsidle-ycsb-prepare.XXXXXX)
./dsidle_run_ycsb_experiment.sh --prepare-only --out-dir "$out/result" \
  --vm-count 1 --threads-per-node 1 --record-count 10 --operation-count 20 \
  --workloads a,e --shared-size-mb 2048 --warmup-rounds 0 --rounds 1 \
  --no-latency
~~~

生成的 worker trace、派生 JSONC 和 manifest 都属于本仓输出。脚本会检查命令数、
worker 划分、固定 key/value 大小和文件 SHA256。

## 真实回放

~~~bash
./dsidle_run_ycsb_experiment.sh \
  --vm-count 4 --threads-per-node 4 \
  --record-count 100000 --operation-count 100000 \
  --workloads a,b,c,d,e --warmup-rounds 1 --rounds 1 \
  --shared-size-mb 32768 --no-latency
~~~

正式性能矩阵不是本任务的默认验证路径。只有遇到偶发并发问题时才把同一命令的
rounds 扩到 3–5，并保留每轮日志。固定延迟配置只能在 RelWithDebInfo、
verbose=false、extra_check=false 下启用。

## 输出

runner 输出 E2E_TRACE_TIME_US、E2E_TRACE_HEARTBEAT 和 DSIDLE_MEMORY_STATS。
前者用于操作耗时和进度，后者只描述业务容量/副本字节；不会输出硬件访问次数、
原子次数或共享模拟日志。

结果汇总：

~~~bash
python3 scripts/summarize_ycsb_experiment.py \
  --log-dir tests/data/ycsb_logs --out-dir /tmp/dsidle-ycsb-summary
~~~
