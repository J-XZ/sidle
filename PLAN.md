# D-SIDLE 当前维护计划

本文是短期维护入口，不是历史施工流水账。历史实现和实验过程由 Git 历史保存；
当前行为以源码、配置和 硬件模拟当前实现.md 为准。

## 固定延迟目标

唯一模拟公式是：

~~~text
pending_delay_ns += touched_swcc_lines * swcc_fixed_ns_per_line
pending_delay_ns += touched_hwcc_lines * hwcc_fixed_ns_per_line
~~~

实现必须保持以下性质：

1. 每次真实访问按地址覆盖 cache line 收费，重复访问重复收费；
2. wrapper 不保存访问历史，不维护 cache、序号、共享日志或访问 schema；
3. 原子操作保留真实返回值、CAS expected 和 memory order，不累计操作数；
4. 前台、replica 和其它后台 worker 各自建立 scope，在安全出口结算；
5. disabled 只执行进程本地不可变快速门，不访问 TLS、时钟或共享状态；
6. 启用时使用校准 TSC busy-wait，校准失败 hard fail；
7. SWCC visibility、epoch、replica、owner-private free-list 和 Masstree 协议保持
   为真实业务协议，不由延迟代码代替。

## 配置契约

dsidle.latency_inject 只包含 fixed_latency，且只允许：
enabled、cache_line_bytes、swcc_fixed_ns_per_line、hwcc_fixed_ns_per_line、
foreground_enabled、background_enabled。parser 对未知和重复字段拒绝。

## 工作顺序

### 源码

- 维护 latency_simulator 的固定延迟核心和 access wrapper；
- 审查所有 HWCC/SWCC 访问入口和每个异步 worker 的 scope；
- 保持共享池容量、allocator、epoch、Masstree visibility 和 replica 协议一致；
- 将静态审计和定向测试与每次接口变化一起更新。

### 验证

- Debug：完整 CTest、parser、协议和 disabled benchmark；
- RelWithDebInfo：完整 CTest、TSC canary 和 fixed latency 定向测试；
- R6525：本仓脚本创建干净四 VM，先无延迟，再运行短 fixed canary；
- 测试结束立即停止 VM 并清理本仓 backing。

### 文档和数据

README、VM 手册、YCSB 指南、实现说明和审计报告只描述当前 fixed-only
路径。原始日志、可重新生成 trace、旧 build 和缓存不作为仓库交付物；canonical
image、当前源码、必要 submodule 和至少一个最终 build 保留。
