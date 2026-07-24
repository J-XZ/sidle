# D-SIDLE 公平对比改造计划

本文档（`PLAN.md`）是唯一有效的完整改造计划。核心立场：**允许直接修改基线
SIDLE/Masstree 源码；最终只要求改造后的分布式系统（D-SIDLE）可以运行，不保证
旧单机 DAX/memkind 入口与 ART 路径仍可运行**。不采用"可选钩子 + 默认旧行为
兼容"的双路径设计，热路径集成一律为零间接开销的就地替换。

**本文档同时是施工 agent 的任务书：下文所有"必须/禁止"都是对你（施工
agent）的直接指令。执行协议见紧随其后的「施工执行协议」。**

## 施工执行协议（施工 agent 必读；按此循环推进）

1. **推进单位 = 里程碑**（§八 M0→M8，顺序执行）。每个里程碑内：
   实现 → 构建（默认且唯一强制配置 = RelWithDebInfo，见第 4 条）→
   跑该里程碑退出标准列出的测试 → 全绿后 `git commit` 并记
   `修改日志.md`（SHA、测试命令与结果）→ 进入下一里程碑。
   架构关键节点（池首次 attach、分配器接通、副本首次命中、延迟模拟移植
   完成）即使在里程碑中途也应单独 commit。
2. **测试失败**：定位修复 → commit → 只重跑失败的测试目标及其直接相关
   目标；不必全量重跑已通过的无关套件。
3. **收官 = 多轮 e2e 循环**（M8）：跑 `run_dsidle_e2e_rounds.sh` 三套主
   套件各 ≥10 轮 + 四套补充各 ≥3 轮（§6.0/§6.3）。任一套件任一轮失败：
   修 bug → commit → 该套件从第 1 轮重跑满额；其余已达标套件不重跑。
   循环直到全部套件达标，然后执行 **§7.4 最终基线差异校验与清扫**
   （对基线全量 diff 审计、删除死代码与脚手架、复验），再完成 §6.5
   清单与文档收尾。
4. **构建配置**：默认与验收一律用 **RelWithDebInfo =
   `-O3 -g3 -march=native -flto=full`（与 cxlkv 对齐）**。Debug/ASAN/
   UBSAN 构建**可选**——仅在排查具体 bug（偶发失败、疑似 UAF）时临时
   使用，不是验收门槛，CMake 不必为其做特殊支持。
5. **防过度测试（硬性）**：只执行本文列明的测试与轮数；**禁止**自行加大
   轮数、扩测试矩阵、增加"保险性"重复验证；单测只在相关模块改动后重跑；
   不为每次小改动重跑 e2e。推进速度优先，验收以 §6.0 为准。

其余施工纪律：不保留、不引用已删除的旧改造日志或 `dist/` 时代文档。

对比目标是与同级仓库 `../cxlkv`（branch `my-work`）的公平评测。本仓库与
cxlkv 共享同一套实验合同（拓扑、trace、延迟注入、YCSB 流程）；D-SIDLE
保留自身数据面语义——**单共享命名空间上的偏移化 Masstree + 每 VM 本地
副本**，不引入 key 分区 owner。**工程交付上三方（本仓库、cxlkv、其他同级
改造仓）必须互不依赖**（§0.3）：合同可同构，允许拷贝 cxlkv 源码进本树，
禁止运行时/构建期直接调用，VM 镜像各自独立创建。施工与文档**不得引用**
其他同级改造仓库（其工作树可能仍是旧实现，易误导）。

============================================================
零、改造起点决策与源码拷贝政策
============================================================

## 0.1 起点决策

**结论：以基线提交 `1f1fa8ae9459935963a67a9beef6f30683c3c47b`（"final release"）
为唯一代码起点；当前 HEAD 上晚于基线的全部内容（含 `dist/`、`framework/`、
既有脚本与指南等）打 tag 存档后，工作分支回滚到该基线，再按本文从头实现。**

判断依据：

1. 晚于基线的共享 offset 树实现与原生 Masstree 的 suffix/layer/删除状态机
   不等价，属于另起炉灶的仿制品，违背"尊重原始实现"。
2. 热点策略、一致性协议（mailbox/逐条 ACK 等）相对原 SIDLE 已简化失真，且
   存在正确性与性能问题。
3. 双后端、等价性门槛、manifest 审计等基建服务于渐进演化，不产生最终系统
   价值；从基线就地改造更短、更可审查。

操作（须用户确认后执行）：

```text
git tag dsidle-legacy-archive HEAD
# 回滚前先把本 PLAN.md 拷到仓库外临时路径，回滚后再拷回工作树（基线不含本文件）
cp PLAN.md /tmp/dsidle-PLAN.md
git checkout -B main 1f1fa8ae9459935963a67a9beef6f30683c3c47b
cp /tmp/dsidle-PLAN.md PLAN.md
```

存档内容仅可作**设计参考**（预检项清单、产物目录习惯等），**禁止**复制其
树实现、策略实现或一致性协议代码。回滚后工作树应只有基线源码 + 本
`PLAN.md`（及随后新建的 `修改日志.md` 等），不得把旧 `dist/` 文档一并恢复。

## 0.2 源码修改与拷贝政策

**最终产物是单一系统：直接在基线 SIDLE/Masstree 上扩展改造。不维护双路径、
不做向后兼容；只要求扩展后的系统可以运行。**

1. **允许直接修改基线源码**，包括 `third_party/masstree-beta/`、
   `third_party/sidle_utils/`、`src/`、`CMakeLists.txt`。尊重原始实现的优先序：
   直接调用原代码 > 在原文件上做就地最小修改（保留原有算法、nodeversion 位
   布局、permutation/slice/layer/split/remove 语句顺序，改动处注释标注
   `// dsidle: <改动点>`）> 拷贝改编。禁止凭记忆重写语义等价但细节走样的
   "类 Masstree"。就地修改的 diff 必须小而可审查。
2. **不引入兼容层**：不做"可选钩子 + 默认旧行为"式双路径。热路径集成一律
   选零间接开销方式——直接替换实现、构造期绑定、内联函数；禁止为兜底旧行为
   引入函数指针/虚调用。**新增代码必须选性能最优合理方案**（§1.8），禁止
   故意使用更弱同步/分配/延迟实现“先求正确再优化”的双阶段烂路径。
3. 原始文件**只改不删**：不再参与构建的组件（`third_party/art/`、
   `src/kv/art_wrapper.h`、老 `benchmark/` target、`cxl_allocator.*` 等）源文件
   保留；若因本次修改导致编译失败，直接从 CMake 移除该 target，不花精力
   维持其可编译运行。
4. **新增代码保持克制**：能就地改造原组件解决的，不新建平行组件；新文件
   仅限原实现完全没有的职责（共享池、offset 类型、NodeControl、副本目录、
   SWCC 可见性、延迟模拟移植、实验脚本）。
5. **`../cxlkv` 源码允许直接拷贝进本仓库**（延迟模拟器、QEMU/YCSB 脚本、
   配置 schema 片段、审计文档模板等），拷贝后成为本树源文件并按需改路径/
   前缀。**严格禁止**构建期或运行期对 `../cxlkv`（或任何同级实验仓库）的
   **直接调用或依赖**：不 `#include` 其树外头文件、不链接其 `.a/.so`、不
   `source`/exec 其脚本、不以相对路径打开其配置/trace/镜像、不把其
   `CMAKE_PREFIX`/`PYTHONPATH` 指过去。每次拷贝记入 `搬运清单.md`（来源
   路径、cxlkv SHA、落点、改写方式、license）。
6. 对外 API 形态保留：`src/kv/masstree.h` 的 `MasstreeKV` 提供
   `get/insert/remove/scan`；实验 runner 另暴露与 cxlkv 同构的
   PUT/GET/DELETE/SCAN 入口。禁止 PutBatch 或任何隐式批量提交。

## 0.3 三项目互不依赖（硬性）

本仓库、`../cxlkv`、以及其他同级改造/实验仓库在**交付与日常运行上必须互不
依赖**。合同（字段名、拓扑语义、trace/YCSB 流程）可同构；工程树与产物路径
必须各自自洽。硬规则：

| 维度 | 允许 | 禁止 |
|------|------|------|
| 源码 | 人工只读对照 `../cxlkv`；将其源码/脚本**拷贝**进本树后改写 | `#include`/链接/`source`/exec/打开兄弟树路径；`CMAKE_PREFIX`/`PYTHONPATH` 指过去 |
| 构建/测试/YCSB/e2e | 单独 clone **仅本仓库**即可完成 | 要求兄弟目录存在、可写、或作为默认输入 |
| VM 镜像 | 本仓库独立创建机制（`dsidle_make_vm_img.sh` + 本树 `image/`），步骤与 cxlkv **基本一致**（可拷贝 mkosi 流程后再改） | 默认使用兄弟仓已造好的 `image/root.img`；软链/挂载/拷贝兄弟成品镜像作正式路径；exec 兄弟 make_img/init |
| 运行时产物 | 本仓库 `vm.storage_path` / build / trace | 共享兄弟 build、trace、pid、ssh 目录 |

应急导入成品镜像：仅当本仓库 mkosi **BLOCKED** 且用户显式授权时，可从**一次性
显式本地路径**导入，立刻记 SHA 到 `修改日志.md`，**不得**把该路径写进默认脚本。

验收：临时目录仅含本仓库 checkout（无兄弟树）时，`cmake` 构建 +
`dsidle_make_vm_img.sh` + init/check dry-run 必须可完成；记入 `修改日志.md`。

## 0.4 改造范围：仅 Masstree

**本改造只要求把 Masstree（及与之耦合的 SIDLE 策略/分配线程池）改造成可在
多 VM 共享内存上运行的 D-SIDLE。** 明确不在范围内：

| 不在范围 | 处置 |
|----------|------|
| ART（`third_party/art/`、`art_wrapper.h`） | 从构建摘除，文件保留 |
| 基线单机 DAX + memkind 路径 | 废弃；`cxl_allocator.*` 从构建摘除 |
| 老 `benchmark/` 单机基准 | 从构建摘除 |
| 多 key 事务、二级索引、SQL | 不实现 |
| key 分区 / partition owner / 请求转发 | 不引入（保持单共享树） |

SIDLE 策略（直方图、水位、五 worker）**保留选择逻辑**，仅把"节点迁移"动作
替换为"本 VM 本地副本复制/淘汰"；策略代码仍落在 `third_party/sidle_utils/`，
经就地修改接入副本目录。

============================================================
一、目标与不变约束
============================================================

## 1.1 目标

把基线 SIDLE/Masstree 改造成可与 `../cxlkv`（branch `my-work`）公平对比的
分布式共享内存 KV：

- 共享内存分为两个**物理上固定划分、大小可配置**的区域：HWCC（跨节点硬件
  缓存一致，典型 ≤ 1 GiB）与 SWCC/non-HWCC（无跨节点硬件缓存一致）。
- **单一共享 KV 命名空间**：任意 VM 对任意 key 直接操作同一棵偏移化
  Masstree；写返回后对所有 VM 立即可见。不引入 partition owner。
- 只要求强一致的单 KV 操作：PUT/GET/DELETE/SCAN；不做通用多 key 事务；
  **禁止 PutBatch**。
- 规范数据（树节点、值、外部 ksuf）全部在 SWCC；跨 VM 原子控制
  （nodeversion、根、分配器头、epoch）在 HWCC；每 VM 可用本地 DRAM 持有
  **计入副本预算**的自包含节点副本（命中路径不再回 SWCC 取值）。
- 与 cxlkv 并列的实验基础设施（§1.5）：独立同构的 VM 镜像/启动脚本、
  trace 生成与 YCSB load/A/B/C/D/E 运行脚本、相同语义的 Put/Get/Delete/Scan；
  以及软件延迟注入（4.6）。
- **质量门槛（§6.0）**：关键功能必须补单元测试且全部通过；至少三套端到端
  （模仿 cxlkv e2e_08 / e2e_09 / e2e_10，数据量同为 100k 级）各连续通过
  ≥10 轮，才可声称无已知 bug / 进入正式对比。

方案取舍总原则：**凡 Masstree/SIDLE 原有机制覆盖的部分，最大程度复用原始
实现（nodeversion、permutation、slice/layer、split/remove、叶链、RCU、
直方图与五 worker）；原实现缺失的部分（共享池、offset、NodeControl、副本
目录、延迟模拟、VM/YCSB 编排），按本文 §3–§4 钉死的方案实现（不得另起
未记载的平行机制），选性能最优实现，能照搬 cxlkv
的直接照搬。**

## 1.2 公平性硬约束

1. 与 cxlkv 使用同一套 `experiment_config.jsonc` **拓扑**语义：相同
   `shared_memory.size_mb`（须为 2 的幂；正式 5M YCSB 另按 §1.11 覆盖为
   65536）、相同 `shared_memory.hwcc/swcc`、相同 `path`/`device_path`、
   相同 VM 数/核数/NUMA、相同 worker 数、**同一份** YCSB-cpp 生成的
   trace（正式 YCSB fixed **32/32**）。`latency_inject` / `fixed_*` 在
   cxlkv 位于 delta_policy；本仓放在 `dsidle` 段但字段同构。路径取值以本
   仓库根目录配置为准，不硬编码宿主绝对路径。
2. HWCC 逻辑与物理使用均不得超过配置容量（YCSB 正式对比固定 1024 MB）；
   不得创建隐藏的第二共享内存池。
3. 所有共享内存分配必须归类为 HWCC 或 SWCC；统计中无未分类字节。
4. 本地 DRAM 副本受 `replica_budget_mb` 约束，预算与命中/提升/淘汰计入
   `DSIDLE_MEMORY_STATS`；不得持有未计入预算的完整数据镜像。
5. 吞吐计时只覆盖 workload replay，不含初始化/pool reset/barrier；单轮
   `ops_per_sec = ops_sum / (max(duration_us) / 1e6)`；多轮字段名按套件
   （§1.11 / §6.3），与 cxlkv 同口径。
6. 延迟注入开启仅允许 `RelWithDebInfo + verbose=false + extra_check=false`，
   否则 hard fail。正式 5M YCSB 主表使用 `--no-latency`（§1.11）。

## 1.3 实验模型术语

不得宣称拥有真实 CXL 硬件。准确描述为：多 VM 通过 QEMU/ivshmem 映射同一块
宿主机远端 NUMA DRAM；HWCC/SWCC 是协议与实验统计中的逻辑/物理区域类别；
底层 CPU coherence 可能掩盖 SWCC 协议错误，正确性由多进程不同基址测试与
可见性顺序专项测试兜底；结果称为 "NUMA-based CXL shared-memory emulation /
software latency-injected result"。

## 1.4 安全约束（不可违反）

- 禁止 push 到任何远程；允许本地 commit；禁止 `git reset --hard`、
  `git clean -fd`（回滚到基线的操作须用户明确确认后按 0.1 执行）；禁止
  覆盖用户已有未提交修改；禁止重新 clone。
- 禁止重启服务器；**未经用户明确允许禁止重启/重建 VM**；禁止
  mkfs/fdisk/parted/wipefs、remount/umount；禁止修改网络与
  SMT/turbo/governor/NUMA balancing/THP（host tuning 默认只检查并报告，见
  4.8）。
- `../cxlkv` 只读参考 / 允许拷贝源码进本树；禁止运行时调用、链接、或使用其
  成品镜像（§0.3）。禁止在 cxlkv 中构建覆盖或改配置。
- 禁止无差别 pkill；只能按 PID 文件或精确可执行路径终止本轮测试进程。
- 协议错误、越界、allocator OOM、HWCC 预算超限必须 hard fail；禁止静默
  fallback、假 pass、空实现。

## 1.5 并列实验基础设施合同（相对 cxlkv 的交付硬门槛）

本仓库与 cxlkv 各自维护**独立完备**的脚本与二进制，但对外合同**同构**：
同一配置字段、同一 trace 字节格式、同一 YCSB 阶段集合、同一 KV 操作语义。
允许的唯一差异：脚本语言（bash 重写 fish/Rust）与路径/前缀 `dsidle_`。
工程上遵守 §0.3：**可拷贝 cxlkv 源码进本树，禁止任何运行时/构建期直接调用
或依赖兄弟仓库**（含成品 VM 镜像）。

### 1.5.1 VM 镜像与启动（独立脚本 + 基本一致机制；禁止共用成品镜像）

本仓库必须在项目根提供与 cxlkv **职责一一对应、实现各自独立**的脚本（仅
前缀不同）。机制与参数集与 cxlkv **基本一致**（mkosi/rootfs、ivshmem、
NUMA 绑定、SSH），但**必须在本仓库内闭环创建**，不得 exec 兄弟仓库的
make_img/init 脚本，也不得默认使用兄弟仓库已生成的 `image/root.img`。

| 职责 | cxlkv 参考（只读对照 / 可拷贝源后改） | 本仓库交付 |
|------|--------------------------------------|------------|
| 制作 guest 镜像 | `xz_scripts/init_scripts_env_2_make_vm_img.fish` | `dsidle_make_vm_img.sh` + 本仓库 `image/` 配置 |
| 启动/绑定多 VM + 共享内存 | `xz_scripts/init_scripts_env_3_init_vm.fish` + rust `init_vm` | `dsidle_init_vms.sh`（bash 自实现） |
| 精确停止 | cxlkv 同款 kill 逻辑 | `dsidle_kill_vms.sh` |
| 只读检查 | cxlkv 无独立 check 脚本（预检在 `init_vm` 内） | `dsidle_check_vms.sh`（**本仓增强**：含 `numa_maps` 抽样；非 cxlkv 对等物） |

配置与产物要求：

1. 拓扑字段与 cxlkv **同名同语义**（见 §1.6）；正式对比取值**必须**等于
   §1.11 钉死数字，禁止施工时另选"差不多"的值。
2. QEMU 参数集、ivshmem-plain、SSH hostfwd、taskset、guest 加载 ivshmem
   与 cxlkv（§1.11 钉死 SHA）**基本同构**：参数名/取值一致，仅路径与前缀
   不同（允许 bash 重写）。**刻意分歧（须在修改日志声明）**：(a) cxlkv
   `init_vm` 会**直接应用** host tuning（SMT/turbo/governor/NUMA balancing/
   THP 等）；本仓库默认**只检查并报告**，`--apply-host-tuning` 须显式授权；
   (b) 相位屏障见 §4.7（不假装与 cxlkv tap+TCP `sdl::notify` 同构）。
3. **镜像独立性**：`dsidle_make_vm_img.sh` 的**唯一主产物**为本仓库
   `image/root.img`；由 `dsidle_init_vms.sh` 再复制到
   `$vm.storage_path/vm_*/root.img`（对齐 cxlkv `copy_root_img` 语义），
   不得把 storage 副本当 mkosi 主输出。允许对照 cxlkv 的 mkosi 步骤
   **拷贝脚本进本树**
   后改写；**禁止** `cp ../cxlkv/image/root.img …` 或软链/挂载兄弟镜像作为
   正式交付路径。应急仅当本仓库 mkosi 环境 BLOCKED 时，经用户授权从**显式
   路径**导入一次，必须立刻记 SHA 到 `修改日志.md`，且不得把该路径写进默
   认脚本。
4. 产物落在本仓库 `vm.storage_path`；不得调用 `../cxlkv` 启动器。

### 1.5.2 Trace 生成与 YCSB 运行（独立脚本 + 同构合同）

| 职责 | cxlkv 参考 | 本仓库交付 |
|------|------------|------------|
| Trace 生成器 | `thirdparty_libs/YCSB-cpp/scripts/generate_cxlkv_trace.sh`（仅 submodule 内；仓库根无同名脚本） | 同 SHA 的 submodule（gitlink 见 §1.11）+ submodule 内同名生成脚本为唯一入口（不做 `scripts/` 包装，不得依赖 `../cxlkv` 路径） |
| 一键实验 | `scripts/run_ycsb_trace_experiment.sh` | `dsidle_run_ycsb_experiment.sh` |
| 汇总 | `scripts/summarize_ycsb_trace_experiment.py` | `scripts/summarize_ycsb_experiment.py` |
| 指南 | `doc/YCSB指南.md` | 根目录 `YCSB指南.md` |

必须支持的阶段：**`load` + `a` + `b` + `c` + `d` + `e`**。

- `--workloads` 允许集合为 `a,b,c,d,e`（封闭、小写）；默认 `a,b,c,d`（与
  cxlkv 相同）；传 `e` 时必须生成并回放 workloade（SCAN+PUT）。
- D-SIDLE **必须实现 Scan**，因此 **必须支持 YCSB-E**（不得以“可选”为由
  缺实现）。正式对比矩阵默认仍跑 A–D；E 作为显式扩展组一并交付能力。
- load / A / B / C / D / E 的生成参数、UPDATE→GET+PUT、INSERT→PUT 映射与
  cxlkv 生成器一致，保证在相同 `record-count`/`operation-count`/
  `threads-per-node`/`fixed_key_size`/`fixed_value_size` 下 trace **逐字节
  可比**（理想情况可直接互换 worker 文件）。

### 1.5.3 与 cxlkv 相同语义的实验 KV 接口

trace runner 对引擎只调用下列单 key 强一致操作（语义对齐 cxlkv
`CxlTree` 前台 API；本仓库内部可映射到 `MasstreeKV::insert/get/remove/scan`）：

| Op | 语义 |
|----|------|
| `Put(key, value)` | load/run 均为 upsert（与 cxlkv `CxlTree::Put` 相同：已存在则覆盖；**禁止**把 load 重复 key 标成 hard fail）；**禁止 batch** |
| `Get(key)` | 读当前值；不存在则 miss/空；不强制校验 value 字节 |
| `Delete(key)` | 删除；之后 Get 为 miss |
| `Scan(start_key, limit)` | 从 `start_key` 起按键序最多返回 `limit` 条（`limit==0` 表示不限制，与 cxlkv 一致）；无端键；无跨操作强快照 |

额外合同：写返回后任意 VM 立即可见；定长 key/value 由
`fixed_key_size`/`fixed_value_size` 约束；runner 侧 FixedTraceKey/
FixedTraceValue 行为与 cxlkv 相同。

## 1.6 NUMA / 拓扑配置合同（严格模仿 cxlkv）

**目标**：与 cxlkv 一样，仅通过配置文件（及同构脚本选项）决定 VM 跑在哪
些 NUMA 节点、共享内存落在哪些 NUMA 节点、宿主机核如何分工——**禁止**在
脚本里写死 NUMA 拓扑。施工前必读 `../cxlkv` 的 `AGENTS.md`（xz_scripts /
experiment_config 段）与根 `experiment_config.jsonc`。

### 1.6.1 配置字段（同名同语义；整型或整型数组）

与 cxlkv 根配置同构（未知字段 hard fail；缺字段 hard fail）：

```jsonc
{
  "shared_memory": {
    "size_mb": 32768,                 // 须为 2 的幂
    "path": "/mnt/xz_shared_mem",
    "device_path": "/dev/ivpci0",
    "numa_node": [1],                 // int 或 int[]：共享池绑定的 NUMA
    "hwcc": { "offset_mb": 0, "size_mb": 1024 },
    "swcc": { "offset_mb": 1024, "size_mb": 31744 }
  },
  "vm": {
    "count": 4,
    "core_count_per_vm": 8,
    "mem_size_mb_per_vm": 2048,
    "storage_path": "/mnt/xz_vm_storage",
    "ssh_base_port": 10022,
    "numa_node": [0],                 // int 或 int[]：VM CPU/内存绑定
    "local_ssh_pub_key": "...",
    "first_ip": "192.168.100.2",       // 与 cxlkv 同名字段保留
    "bridge_tap_ip": "192.168.100.1"
  },
  "host_cpu": {
    "reserved_cores": [34],           // 宿主机脚本/SSH
    "ivshmem_server_cores": [32, 33],
    "vm_cores": [0, 1, /* … */ 31]    // 长度 ≥ count * core_count_per_vm
  },
  "e2e": {
    "foreground_worker_count_per_vm": 4
    // 注意：cxlkv 根配置的 e2e **仅有**上键。fixed_key/value_size、trace_dir、
    // latency_inject 在 cxlkv 中位于 **delta_policy / e2e_trace 配置**，不在
    // experiment_config.jsonc。本仓库为单文件便利，将 runner/策略键放在
    // `dsidle` 段（字段名与 cxlkv policy 同构，文件位置刻意不同，见 §4.6/§4.7）。
  },
  // 下列 cxlkv 根键若出现：parse-and-ignore（保持 jsonc 可互换），不得 hard fail
  // "network": {...}, "sync": {...}, "vm.copy_root_img": false, "vm.use_ivshmem_doorbell": false
  "dsidle": {
    "replica_budget_mb": 1536,        // 每 VM；正式对比钉此值
    "hot_percentage_seed": 50,        // 替代废弃 cxl_percentage；仅热点种子，不再做 stride 混合
    "fixed_key_size": 32,             // 正式 YCSB/e2e_ycsb 默认 32；e2e_08 覆盖为 8；e2e_09 value=1000
    "fixed_value_size": 32,
    "trace_dir": "...",               // 本仓 runner 配置；非 cxlkv 根 e2e 字段
    "latency_inject": { /* §4.6 全字段；schema ≡ cxlkv LatencyInjectPolicyConfig */ }
  }
}
```

`dsidle` 段以上表为封闭集合；未知键 hard fail。

规则（对齐 cxlkv `AGENTS.md` + `init_scripts_env_3_init_vm` 预检）：

1. `shared_memory.numa_node` / `vm.numa_node`：**支持单个整数或整数数组**；
   解析后规范化为有序列表；节点号必须在宿主机存在。
2. **性能实验硬规则**：宿主机具备 ≥2 个 NUMA 时，共享内存必须绑到与
   VM CPU/内存**不同且尽可能远**的 NUMA；**禁止**为提速把共享池绑到与 VM
   相同或更近的节点。`shared_memory.numa_node` ∩ `vm.numa_node` ≠ ∅ 时：
   正式性能 / YCSB 对比 / §6.0 e2e **hard fail**；仅本地功能调试允许
   `--allow-overlapping-numa` 并在报告声明（对齐 cxlkv“单 NUMA 退化须说明”）。
3. `host_cpu` 三组核列表互不重叠、在线、且每个核落在 `vm.numa_node` 覆盖的
   NUMA 上；`vm_cores` 长度 ≥ `vm.count * vm.core_count_per_vm`。
4. 配置选择环境变量：`DSIDLE_EXPERIMENT_CONFIG_JSONC`（语义同 cxlkv
   `CXLKV_EXPERIMENT_CONFIG_JSONC`）指向另一份完整 jsonc；默认
   `experiment_config.jsonc`。
5. 交付示例配置（可并列、无优劣）：
   `configs/numa/experiment_config_2_numa_version.jsonc`（VM∈NUMA0、
   shared∈NUMA1，对齐 cxlkv `cloudlab/r6525/…_2_numa_version.jsonc` 语义）、
   以及本机实测拓扑的默认根配置。
6. 根级 cxlkv-only 键（`network`/`sync`/`vm.copy_root_img`/
   `vm.use_ivshmem_doorbell`）**parse-and-ignore**（保持 jsonc 可互换，
   不得 hard fail，也不得实现其功能）。

### 1.6.2 脚本接口（与 cxlkv 同名选项）

| 接口 | 行为（模仿 cxlkv） |
|------|-------------------|
| `dsidle_init_vms.sh [--config PATH]` | 读配置；预检；按 `shared_memory.numa_node` `numactl --membind` 准备 backing；按 `vm.numa_node` + `host_cpu.vm_cores` 切片启动并 `taskset` 钉核 |
| `dsidle_check_vms.sh` | **本仓增强**（cxlkv 无对等独立脚本）：校验 QEMU cmdline / taskset / SSH / 设备节点；另抽样 `numa_maps` 核对共享页 NUMA ∈ 配置 shared 列表 |
| `dsidle_run_ycsb_experiment.sh --base-config PATH --shared-numa N[,…]` | 从 base 生成轮次配置，**改写** `shared_memory.numa_node`（及 size 等），再 init/check；选项名与 cxlkv 一键脚本同名 |
| `--dry-run` | 打印解析后的 NUMA 列表、核切片、完整 QEMU 行，不落地 |

共享文件 memory-backend 必须经配置的 NUMA 绑定创建/预故障；禁止“随便落在
当前节点”的隐式默认。

## 1.7 硬件一致性与原子性假设（对齐 cxlkv `AGENTS.md`）

施工 agent **必须先读** `../cxlkv/AGENTS.md` 的「项目模型 / 最高优先级约束 /
Epoch 与 Flush / 延迟模拟约束」，再实现本系统映射。下列假设与 cxlkv
**完全相同**（数据面结构可不同，区域语义不可削弱）：

| 假设 | 含义 |
|------|------|
| HWCC | 跨节点**有**硬件 cache coherence；可承载跨节点原子同步（CAS/load-acq/store-rel） |
| SWCC | 跨节点**无**硬件 cache coherence；**不能**依赖普通 store 或 C++ 原子提供跨节点一致性 |
| 原子性 | 启动时校验 64 位跨进程原子 lock-free；不满足拒绝启动 |
| 偏移 | 跨进程持久引用只用区域内 64 位 offset，禁止裸指针 |
| Hard fail | 池 OOM / 协议违规 / 预算超限 hard fail，禁止静默降级 |

**D-SIDLE 落位纪律（SWCC/HWCC 合理使用）**：

- **必须进 HWCC**：`NodeControl.version_and_state`（及全部跨 VM 可变同步位）、
  `RootControl`、分配器 shard 头 / remote-free 栈头、分布式 epoch 槽、
  诊断计数（非相位屏障）。跨节点冲突只经 HWCC CAS/乐观版本解决。
- **必须进 SWCC**：Masstree 规范节点体、ksuf、value_bag、分配器 slab/空闲链
  节点体。已发布且可能被他 VM 读的 SWCC 对象：**禁止**在无协议保护下依赖
  “原子写可见”；修改后必须 `writeback/flush+fence`，再经 HWCC 版本/栈头发布。
- **禁止**：把跨节点同步可变状态放进 SWCC；把 SWCC 当 coherent 共享内存读；
  跳过或缩小必要 flush；在未确认 HWCC 版本稳定时采纳 SWCC 节点内容。
- **本地 DRAM 副本**：非跨节点权威；命中必须校验 HWCC `{NodeRef,gen,version}`；
  不计软件延迟（与 cxlkv “本地生命周期门不进 HWCC” 精神一致）。

读路径固定：HWCC 取稳定版本 →（失配则）invalidate SWCC → 读节点 → 二次验版。
写路径固定：HWCC 取锁 → invalidate → 改 SWCC → flush/fence → HWCC 发布新版本。

## 1.8 性能最大化红线（禁止故意弱方案）

除为与 cxlkv **公平对比**所必需的合同（双区域、延迟注入、NUMA 远端共享、
trace/YCSB 同构）外：

1. **尊重原实现优先**：Masstree OLC、permutation/SMO 顺序、叶链、RCU、SIDLE
   直方图/五 worker/水位公式不得换成更粗粒度锁、全局树锁、mailbox ACK、或
   “重写类 Masstree”。
2. **新增代码必须选当前已知最快合理方案**：热路径零间接（内联/构造期绑定，
   禁止虚调用/函数指针兜底）；分配器 per-thread cache + size-class，禁止
   全局锁 next-fit 环扫；延迟模拟必须 TSC busy-spin，**禁止** `sleep_for`/
   yield 冒充；统计用 TLS/relaxed，禁止热路径互斥累加。
3. **严格禁止故意弱化**：额外全局锁、不必要的 msync 当行协议、故意关闭
   编译优化、缩小缓存行对齐制造伪共享、用页级同步替代 cacheline flush、
   在持锁路径插入延迟自旋。
4. 若某“更慢”选择不可避免（如值强制 SWCC），必须在 `修改日志.md` 写明
   **原因=正确性/公平合同**，不得以“先跑通”为由保留。

## 1.9 施工 agent 参考清单（防偏差）

1. 读序：本文 → `../cxlkv` **branch `origin/my-work`** 的 `AGENTS.md` →
   `experiment_config.jsonc` → `doc/延迟模拟/*` → 基线 Masstree/SIDLE 源码。
   **禁止**用 cxlkv `main` 的旧数据面文档指导对比合同。
2. 每里程碑：测试绿 → 本地 commit → `修改日志.md`（含 SHA）。
3. 对象落位速查见 §3.2 / §4.1 / §4.4；延迟插入点见 §4.6 表；NUMA 预检见
   §1.6 / §4.8；并发/回收总则见 §1.10。
4. 禁止从已删除的旧 `dist/` 文档或仿制树拷代码；禁止运行时依赖 `../cxlkv`。
5. 易偏清单（出现即打回）：对 SWCC `next_` CAS；副本命中无 post-read 双检；
   epoch 槽用 `0` 当空闲；每 op 全局 `rcu_quiesce`；目录无同步就 free
   `local_ptr`；Scan 不进 epoch；控制项半成品入树；共用兄弟 `root.img`。

## 1.10 并发正确性 / 高并发 / 安全回收（施工红线）

里程碑评审单独核对；违反即不可进入下一里程碑：

1. **并发正确性**：跨 VM 可变同步只在 HWCC；SWCC 发布序
   invalidate→mutate→flush/fence→HWCC 版本；读者与副本命中同等版本双检；
   副本目录 **per-slot seqlock（唯一方案）**；禁止全局树锁/scan gate；
   叶链禁止 SWCC CAS。
2. **高并发**：每 VM 跑满配置的 foreground workers；分配器 per-thread
   cache；`stable()` 单次行读复用元组；epoch **推进/quiesce** 离开热路径
   （每 50 op，对齐基线）；禁止为正确性引入全树 mutex。
3. **安全回收**：分布式 epoch + limbo（带 owner_shard）+ `generation`；
   `INACTIVE=UINT64_MAX` 不参与 min；未 quiesce 禁止复用；NodeControl 与
   SWCC 体成对分配/retire；半成品 `NodeRef` 禁止入树。

## 1.11 钉死数值与唯一方案表（施工不得另选；改动须先改本表）

对照基准（拷贝与公平合同均以此为准，写入 `搬运清单.md`）：

- cxlkv 对照 = branch `my-work` @ `e282a65a7e4f76ac1f9f772f99301d96f7fca5de`；
- YCSB-cpp submodule gitlink = `746415127173e7711f134944dbcd92b8216c47e7`
  （url `https://github.com/J-XZ/YCSB-cpp.git`）；
- 基线 sidle worktree（策略对拍）= `1f1fa8ae9459935963a67a9beef6f30683c3c47b`。
- 若上游漂移：先更新本表 SHA 并记录 diff 影响，再动工。

### 默认拓扑（根 `experiment_config.jsonc`；与 cxlkv 根配置逐项相等）

| 键 | 值 |
|----|----|
| `shared_memory.size_mb` | 32768 |
| `shared_memory.hwcc` | offset 0 / size 1024 |
| `shared_memory.swcc` | offset 1024 / size 31744 |
| `shared_memory.numa_node` | [1]（与 `vm.numa_node=[0]` 分离） |
| `vm.count` / `core_count_per_vm` / `mem_size_mb_per_vm` | 4 / 8 / 2048 |
| `vm.ssh_base_port` / `first_ip` / `bridge_tap_ip` | 10022 / 192.168.100.2 / 192.168.100.1 |
| `host_cpu` 三组核数 | reserved 1、ivshmem 2、vm_cores 32 |
| `e2e.foreground_worker_count_per_vm` | 4 |
| `dsidle.replica_budget_mb` | 1536 |
| 构建 | 默认与验收 = RelWithDebInfo `-O3 -g3 -march=native -flto=full`（与 cxlkv 对齐；禁关 LTO 做正式对比）；Debug/ASAN 可选、非门槛 |
| HWCC 占用 | 静态核算 ≈128MB（NodeControl slab 128MB 预留为主，见 §4.1 预算表）；上限 1024MB，倾向更小，剩余保持未用 |

### 正式 5M YCSB 公平对比（对齐 cxlkv `doc/YCSB指南.md` §5.1 / runbook；覆盖根配置）

| 项 | 钉死值 |
|----|--------|
| record/operation | 5000000 / 5000000 |
| threads_per_node / workloads | 4 / `a,b,c,d`（zipfian 常数取库默认 0.99） |
| `fixed_key_size` / `fixed_value_size` | **32 / 32**（cxlkv e2e_trace / e2e_10 / YCSB 默认；**不是** e2e_08 的 8/8） |
| `--shared-size-mb` | **65536**（HWCC 仍 1024，SWCC = 64512；一键脚本按此改写本轮 experiment_config） |
| `--shared-numa`（2-NUMA 正式机） | `1`（VM∈0；与 cxlkv r6525 2-NUMA runbook 一致） |
| 延迟 | **`--no-latency`**（正式主表；与 cxlkv 指南正式命令一致） |
| 一键脚本副作用 | 与 cxlkv `run_ycsb_trace_experiment.sh` 相同：**无条件**把生成 policy 的 `cache_model` 置 `none`、`cache_hits_enabled=false`；`--no-latency` 仅关闭 `enabled`/`foreground_enabled`/`merge_enabled`/`stats_enabled`（无计费） |
| 多轮吞吐字段 | 由 `ops_sum` / `duration_sec_max`（或 `avg_ops_sum` / `avg_duration_sec`）推导 `ops_per_sec`；**不要**要求 YCSB 汇总产出 `ops_per_sec_from_avg_round_max`（该字段仅 e2e_08/09） |

### 套件定长与延迟参考（勿与正式 YCSB 混用）

| 套件 | key/value | 延迟 |
|------|-----------|------|
| `dsidle_e2e_08` | 8 / 8 | 按该套件 policy；非正式 YCSB |
| `dsidle_e2e_09` | 32 / 1000 | 同上 |
| `dsidle_e2e_ycsb`（≡e2e_10） | 32 / 32 | 冒烟可用默认；正式 5M 见上表 |
| e2e_11 延迟参考档 | （cxlkv 专用） | swcc 25ns/线、hwcc 117ns/线、原子 117ns、`per_thread_lru` 4096×8；**仅**作注入对比附录，**不是**正式 5M YCSB 默认 |
| e2e_08/09 多轮主字段 | — | `ops_per_sec_from_avg_round_max` |

### 验收轮数 vs cxlkv 脚本默认

本仓 §6.0 强制 ≥10 轮（补充套件 ≥3）是**本仓验收门槛**；cxlkv `run_e2e08/09/10_rounds.sh` 默认 3、YCSB 一键默认 1——施工时对本仓脚本显式传 `--rounds 10`，勿把 cxlkv 脚本默认当成验收轮数。

唯一方案速查（正文已展开，此处防漏）：副本目录 per-slot seqlock；
`access_count` 分配时清零（无双键）；线程退出自排空 limbo（30s 超时
hard fail）；OOM 统一 hard fail；rounds 统一入口脚本；trace 生成唯一入口
submodule 脚本；HWCC 布局 `hwcc.offset_mb=0` 且 hwcc/swcc 相接（项目/
YCSB 约定，见 §4.8；非 cxlkv `init_vm` 断言）。

============================================================
二、基线事实与现状诊断
============================================================

以下已对照基线源码核实；施工时若发现与源码不符，先修订本节再动工。

## 2.1 内存管理

- `third_party/cxl_utils/cxl_allocator.c`：`cxl_init` 打开 DAX 设备、
  `mmap(MAP_SHARED)`，memkind `memkind_create_fixed` 包装远端池；
  `cxl_percentage` 驱动 per-thread stride 混合分配。本地内存为普通 libc 堆。
- `kvthread.hh/.cc`：每线程双池——`pool_[]`（本地 `posix_memalign` 2MiB
  slab）与 `remote_pool_[]`（CXL 补给）；按 memtag 路由 leaf/internode。
  空闲链表串在对象内部（裸指针）。
- 节点落位由 `sidle_strategy::decide_new_node_position(parent_type, depth)`
  决定，经 `leaf/internode::make_with_cxl_policy` 选 memtag。
- **值与外部键后缀不走 SIDLE 策略**：`value_bag`、`ksuf_` 经 stride 混合
  分配，且不随节点迁移。

## 2.2 Masstree / SIDLE 元数据与迁移

- 叶内 `access_time`（uint16）；`record_access()` 在 `reach_leaf` 末尾自增。
- `nodeversion` 含 `migration_bit = 1U<<28`；读者屏蔽该位。
- 迁移是拷贝 + 换指针（`masstree_sidle.hh`）：新建 → `memcpy` → 改父槽 →
  叶链 `change_link` → 旧节点 `mark_deleted` + `deallocate_rcu`。
- 已知缺口（新设计须显式处理）：内部节点迁移在 `parent==nullptr && is_root`
  路径不更新 `basic_table::root_`；`make_root` 硬编码 depth=1；全局
  `operator new` 覆盖缺 return；本地记账在 free/remove 路径不对称。

## 2.3 SIDLE 策略（选择逻辑必须逐项保留）

`third_party/sidle_utils/`：

1. 16 桶指数直方图（`log2(access+1)`，上限 15）。
2. 动态阈值：覆盖 `hot_watermark%`（默认 5%）→ hot；覆盖
   `(100-cold_watermark)%`（默认 80%）→ cold；默认初值 128 / 4。
3. 本地预算 95%/85% 上下水位。
4. 本地层数估算：
   `threshold = (log_W N + log_{W/2}((N+1)/2) + 1) / 2`（W=叶扇出 16）。
5. MoodyCamel 队列；长度 > 10 或周期超时才唤醒 executor。
6. 五后台 worker：migration trigger（周期 `basic_interval × 5`）、promotion
   executor、demotion executor、cooler（默认 2000ms，`access_time >>= 1`）、
   threshold adjuster（默认 100ms，紧张/正常/充足三态，紧张时最多 3 轮强制
   调整）。
7. 提升叶后递归提升远端祖先；降级仅当父不再拥有本地子且
   `depth > demotion_depth_threshold`；**depth==1 的根永远钉在本地**。

## 2.4 API 与并发

- `MasstreeKV`：`get/insert/remove/scan`；`lower_bound/range_scan` 未实现。
- 并发：Masstree nodeversion（乐观读 + 节点锁）+ epoch RCU；无行锁、无 MVCC。
- 基线基准每 50 op 调一次 `epochinc + rcu_quiesce`。

## 2.5 指针清单（偏移化完整对象集）

| 位置 | 指针字段 |
|------|----------|
| `basic_table` | `root_` |
| `internode` | `child_[width+1]`、`parent_` |
| `leaf` | `lv_[width]`、`ksuf_`、`next_.ptr`、`prev_`、`parent_` |
| 值 | `row_type*`（value_bag） |
| 空闲链 / RCU limbo | 对象指针 |
| 策略队列 | `uint64_t` 节点地址 |
| 全局 | `node_base::strategy_manager`（进程本地，不入共享区） |

基线无 offset 机制；tier 判定靠地址落在 DAX mmap 区间内。

============================================================
三、目标架构总览
============================================================

## 3.1 共享内存物理布局

一个 backing（路径与 `device_path` 来自 `experiment_config.jsonc`，与 cxlkv
对齐）单次 mmap，布局：

```text
共享池 [0, size_mb)
├── HWCC 区  [hwcc.offset_mb, +hwcc.size_mb)     典型 [0, 1024MB)
│   ├── PoolHeader（magic/layout_version/config_hash/两区 offset·size）
│   │   ※ PoolHeader 是 HWCC 区首个对象（要求 hwcc.offset_mb=0），计入 HWCC
│   ├── RootControl
│   ├── NodeControl slab（每节点 64B 独占 cacheline）
│   ├── 分配器 shard 头 / remote-free 栈头
│   ├── 分布式 epoch 槽（per-VM × per-thread）
│   └── 诊断计数（非相位屏障；相位屏障见 §4.7，默认 ivshmem/host，非 TCP notify）
└── SWCC 区  [swcc.offset_mb, +swcc.size_mb)     典型 [1024MB, end)
    ├── 偏移化 Masstree 内部节点与叶（含 iksuf、permutation）
    ├── 外部 ksuf stringbag、value_bag
    └── 分配器 slab / 空闲链（offset 链）/ 待回收对象
```

- schema **必须**与 cxlkv 同构嵌套对象，禁止扁平 `hwcc_size_mb`：
  `shared_memory.hwcc.{offset_mb,size_mb}` /
  `shared_memory.swcc.{offset_mb,size_mb}`。
- 持久引用一律区域内 offset；**禁止**跨进程裸指针。各 VM 映射基址不同。
- attach 时强校验 magic/version/config_hash/两区边界。

## 3.2 三类存储职责（施工落位表）

| 区域 | 内容 | 跨 VM 同步方式 |
|------|------|----------------|
| SWCC | 规范树全部节点体、iksuf、外部 ksuf、value_bag、分配器 slab/空闲链体 | **无**硬件一致；写后 flush，由 HWCC 版本/栈头发布 |
| HWCC | NodeControl（含原 nodeversion 全位）、RootControl、shard 头、remote-free 栈头、epoch 槽、诊断原子（非相位屏障） | HWCC 原子 / CAS；非原子复合字段仍须协议 |
| 每 VM 本地 DRAM | 自包含节点副本、副本目录、access_count/直方图、策略队列、threadinfo、延迟 TLS | 不跨 VM；命中前必须核对 HWCC 版本 |

**禁止落位**：跨 VM 可变同步状态 → SWCC；完整未计入预算的数据镜像 → 本地
DRAM；规范树节点 → 本地 malloc。

## 3.3 组件复用矩阵

| 子系统 | 采用 | 来源 | 改动量 |
|--------|------|------|--------|
| Masstree 结构/查找/插入/分裂/删除/扫描 | 原 `.hh` 就地改 | `third_party/masstree-beta/` | 指针→offset、版本→NodeControl、插入可见性动作 |
| nodeversion 位语义 | 原位布局迁入 HWCC | `nodeversion.hh` | 存储访问器模板化 |
| 叶链 | **持锁写 + SWCC flush**（禁止跨 VM 对 SWCC 链字 CAS） | `btree_leaflink.hh` | next/prev→`NodeRef`（SWCC 字）；发布经叶/父 `NodeControl` |
| SIDLE 策略选择 | 原公式与五 worker | `third_party/sidle_utils/` | 队列元素与执行回调改为副本 |
| kvthread 双池结构 | 原结构换后端 | `kvthread.hh/.cc` | remote→SWCC shard；epoch→HWCC |
| 共享池 / offset / NodeControl / 副本目录 | **新写** | `dsidle/*` | 全新 |
| 软件延迟模拟 | **照搬 cxlkv** | `../cxlkv` 的 `src/utils/.../latency_simulator.*` → `dsidle/` | 移植 |
| VM / YCSB 脚本 | **照搬 cxlkv 流程** | `../cxlkv` 的 `xz_scripts/*`、`scripts/run_ycsb_trace_experiment.sh` | 新 bash |

## 3.4 数据流（单共享树 + 本地副本）

```text
Get/Put/Delete/Scan
  → 经 RootControl / NodeRef 走 HWCC 控制项
  → 副本目录命中：按 §4.3.2 规范序（s1 → read-side 拷贝 → 退出 read-side →
     s2 双检）；失败走规范路径（陈旧副本仅靠版本失配惰性失效，见 §4.5）
  → 否则读 SWCC 规范节点（按已见版本精确 invalidate）→ 乐观版本双检
  → 写路径：叶控制项 CAS 取锁 → invalidate → 改 SWCC → flush/fence → release 新版本
  → 后台：SIDLE 选择热/冷 → 复制到本地 / 淘汰本地副本（不改规范树结构）
```

跨 VM 一致性不引入 mailbox/逐条 ACK：依赖 (a) 副本命中**前后**强制校验控制项
版本；(b) SWCC flush 先于版本发布；(c) 叶链/父槽等 SWCC 结构字只在持锁下
非原子写 + flush，跨 VM 冲突只经 HWCC `NodeControl`。

============================================================
四、分项设计与改造步骤
============================================================

## 4.1 共享池与双区域分配器

新文件 `dsidle/shared_pool.h/.cc`、`dsidle/shard_allocator.h/.cc`。

1. **池 attach**：宿主文件模式与 guest `device_path`（配置字段）双模式；
   PoolHeader 记录两区边界；启动校验 64 位跨进程原子 lock-free、TSC 稳定、
   各 VM 架构一致，不满足拒绝启动。
2. **SWCC**：按 VM 分 shard；保留 kvthread 2MiB slab 补给形态，slab 从本
   shard bump 区切出（shard 头在 HWCC）；per-size 空闲链 next 改 offset。
   跨 VM 释放推入目标 shard 的 remote-free Treiber 栈（栈头 HWCC，ABA 用
   generation 防护）；补给前收割本 shard remote-free，**每次最多收割
   64 个对象**（余量留在栈上，防止一次补给同步排空超长链）。
3. **规范树路径不再使用本地池**：`make_with_cxl_policy` 一律走 SWCC（tag
   归一 remote 系列）；本地 DRAM 只服务副本与瞬时对象（普通 malloc）。
4. **`memtag_value` / `ksuffixes` 改 SWCC 分配**（相对基线的必要差异：值必须
   跨 VM 可读），记入 `修改日志.md`。
5. **HWCC**：NodeControl 定长 slab + 空闲链，generation 递增；epoch/诊断/
   shard 头静态落在 PoolHeader 之后。预算按 64B/节点启动核算，超限 hard fail，
   **不做压缩**（避免伪共享）。
6. 废弃 `third_party/cxl_utils/cxl_allocator.c` 与 memkind 链接；`cxl_init`
   调用点换成共享池 attach。

**分配器 SWCC 可见性（硬性，与树协议同级）**：

- 修改 SWCC 上空闲链 `next`、对象头、或 remote-free 链节点后，必须
  writeback/flush 对应 cacheline 并 fence，之后才发布任何使其他 VM 可见
  的 HWCC 栈头/bump 指针更新。**批量释放时对全部脏行逐行
  `clwb`/`clflushopt` 后只做一次 `sfence` 即可（性能硬要求：禁止每行一
  fence），再一次性 CAS 发布拼接后的子链。**
- 从 remote-free / 空闲链取下对象前，若本线程已见代际与 HWCC 公布代际
  不符，先对该对象头精确 invalidate，再读 `next`/generation。
- 新切出的 slab 在首次把其中对象交给树之前，须对已初始化头部做 flush
  （或整 slab 预置零后 fence）；禁止未 flush 的半初始化对象被
  `NodeRef`/`ValueRef` 引用。

**分配失败与发布原子性**：

- SWCC 对象与对应 `NodeControl` 的分配是一对：任一侧失败则回滚另一侧，
  **禁止**把悬空 `NodeRef` 或“有控制项无 canonical 体”的半成品挂进树。
- 前台路径遵循原 Masstree 重试/失败语义；allocator OOM 与 HWCC 控制项耗尽
  **统一 hard fail**（可观测计数器 + 错误日志），禁止吞掉，禁止“返回假
  成功后静默降级”。瞬时 CAS 冲突走原重试，不属 OOM。

施工硬合同（不是建议）：

- `SwccOffset<T>` / `HwccOffset<T>` 仅保存 `uint64_t`；`get(base)` 内联加法；
  `0` 为空引用。
- remote-free 节点复用被释放对象头 16B 存 `{next_offset, generation}`。
- limbo 元素必须为 `{swcc_offset, size_or_class, tag, retire_epoch,
  owner_shard}`；`rcu_quiesce` 后按 `owner_shard` remote-free（批量：
  逐行 flush 全部链节点 → 单次 fence → 一次发布 HWCC 栈头）。

**PoolHeader / HWCC 静态布局（施工必须按此序）**：`PoolHeader` **落在 HWCC
区间内**（**必须** `hwcc.offset_mb=0`，PoolHeader 从池起点开始）。字节序：
`PoolHeader → RootControl → NodeControl slab 头/空闲链 → per-VM shard 头 →
remote-free 栈头 → epoch 槽矩阵 → 诊断计数`。SWCC 仅从
`swcc.offset_mb` 起；禁止把根控制或 epoch 槽放到 SWCC。

**HWCC 空间预算合同（硬性；节约 HWCC，≤1024MB 且倾向更小）**：
`--init-pool` 时按下表核算全部 HWCC 对象总和，超过 `hwcc.size_mb`
hard fail；核算结果（各项字节数与总占用）打印并写入 `修改日志.md`。

| 项 | 公式 | 正式规模（5M key，扇出≈15，4 VM）估算 |
|----|------|--------------------------------------|
| PoolHeader + RootControl | 各 64B 对齐 | <1KB |
| NodeControl slab | `slab_capacity × 64B`；`slab_capacity` 默认 **2,097,152 槽（=128MB）** | 实际约 36 万节点 ≈23MB，128MB 预留含 SMO/删除余量 |
| per-VM shard 头 + remote-free 栈头 | `vm_count × 64B` 各一组 | <1KB |
| epoch 槽矩阵 | `vm_count × max_threads_per_vm × 64B`（每槽独占 cacheline） | 4×16×64B = 4KB |
| 诊断计数 | 固定 ≤4KB | 4KB |
| **合计** | | **≈128MB ≪ 1024MB** |

规则：HWCC 逐 key/逐 op 增长的只有 NodeControl（受 slab 容量硬界）；
禁止把任何随数据量增长的其他结构放进 HWCC；剩余 HWCC 保持未用（不
自动扩 slab）；`slab_capacity` 可配但正式对比用默认值。

## 4.2 偏移、NodeRef 与 NodeControl

```text
NodeRef        = HwccOffset<NodeControl>   # 8B，替换所有节点指针
SwccOffset<T>  = SWCC 带类型偏移
ValueRef       = SwccOffset<row_type>
QueuedNodeRef  = {NodeRef, generation}     # 策略队列 / 副本目录键
```

`NodeControl`（`alignas(64)`，整行独占一 cacheline）：

```text
version_and_state       # uint64_t 原子；原 nodeversion 全部位语义
canonical_swcc_offset   # uint64_t；规范节点 SWCC 偏移（平台上 8B 原子读）
generation              # uint64_t；控制项复用代际
retire_epoch            # uint64_t；配对 SWCC 体/控制项回收纪元
node_type / alloc_state # 非读者热路径；ALLOCATING/PUBLISHED/RETIRING/FREE
（padding 至 64B）
```

`RootControl`（`alignas(64)`，HWCC）：

```text
root_ref                # NodeRef
root_generation         # 根控制代际（ABA）
version                 # uint64_t 原子；双检读根
```

根替换：flush 新根节点 → store `root_ref/root_generation` →
`version` store-release；读者 `v1 → 读 root_ref → v2`，`v1==v2` 才采用。

规则：

1. `root_`、`child_/parent_`、`next_/prev_`、layer 引用全部换 8B `NodeRef`；
   `stable()` **一次 acquire 读**控制行，返回 `{v, gen, swcc_off}` 元组并复用
   至 v2 校验——禁止为同一节点反复拆行读（§1.8）。不采用 24/32B 复合句柄。
2. `leafvalue` 保留"值或 layer"联合：值存 `ValueRef`，layer 存 `NodeRef`，
   显式 tag 区分。
3. `ksuf_` 换 `SwccOffset<stringbag>`；空闲链/limbo 改 offset。
4. 裸指针只作栈上瞬时变量；不得写回共享对象、队列或长期缓存。
5. 策略队列与副本目录必须带 generation（防 ABA）。
6. **`canonical_swcc_offset` / `generation` / `retire_epoch` 只在控制项不可
   读态修改**：`alloc_state≠PUBLISHED`，或持写锁 / `RETIRING`。读者仅在
   `stable()` 双检通过（unlocked、未删、`v1==v2`）后才使用这些字段。
7. **控制项复用发布序**：从 FREE 取出 → `alloc_state=ALLOCATING` → 写
   `canonical_swcc_offset`、递增 `generation`、清 `retire_epoch` → fence →
   初始化 SWCC 体并 flush → 写 unlocked `version_and_state` →
   `alloc_state=PUBLISHED`。禁止先挂树边再填 offset。

**nodeversion → NodeControl 就地映射**（改动最深处）：

- `nodeversion.hh` 位布局与 `stable()/lock()/unlock()/mark_insert()/
  mark_split()/mark_deleted()` 语义原样保留；存储从节点内搬到
  `NodeControl.version_and_state`。
- 节点结构体内原 `nodeversion` 字段改为指向自身控制项的 `NodeRef`
  （构造期写入）。
- `migration_bit` 保留位定义与读者屏蔽逻辑；规范树不再做结构迁移，但位
  语义不删。
- 所有 `n->version()` 访问点经 `NodeRef` 解析后做原子操作，并作为 HWCC
  延迟记账插入点（4.6）。

施工硬合同（`NodeVersionAccessor`/`StableView` 为唯一访问器，必做）：

```cpp
struct StableView { uint64_t v, gen, swcc_off; };
template <class T>
struct NodeVersionAccessor {
  NodeRef ref;
  // 一次 HWCC 行读：version acquire + 同线 gen/offset；RecordHwcc*
  StableView stable() const;
  void lock();                          // CAS 置锁位
  void unlock_release(uint64_t new_v);  // store-release
};
```

读者协议：`s1 = stable()` → 用 `s1.swcc_off/gen` →（读完）`s2 = stable()`；
仅当 `s1.v==s2.v` 且 gen 不变且未锁未删才采纳。写者持锁修改、release 发布。

## 4.3 Masstree 就地改造（唯一数据结构改造面）

**范围声明：只改 Masstree 相关翻译单元与其直接依赖；不移植 ART，不新建第二
套树。**

### 4.3.1 文件级改动表

| 文件 | 修改 | 保持不变（审查红线） |
|------|------|----------------------|
| `nodeversion.hh` | 存储经 `NodeRef` 的 HWCC 原子 | 全部位定义与函数语义 |
| `masstree_struct.hh` | 指针→`NodeRef/SwccOffset`；`make_with_cxl_policy`→一律 SWCC+分配控制项；`record_access`→本地计数数组 | 节点宽度/permutation/iksuf、原有断言 |
| `masstree.hh` | `basic_table::root_`→`RootControl` | initialize 流程形态 |
| `masstree_get.hh` | 解引用→epoch 内 offset 解析；版本→控制项；副本命中短路 | 查找重试结构 |
| `masstree_insert.hh` / `masstree_split.hh` / `masstree_remove.hh` / `masstree_tcursor.hh` | 同上 + SWCC 可见性 + epoch retire | 锁定/验证/重试/发布语句顺序 |
| `masstree_scan.hh` | 叶链 `NodeRef` 行走；版本化扫描 + 每 KV 值契约 | 无全范围强快照、无 scan gate |
| `btree_leaflink.hh` | next/prev→`NodeRef`（SWCC 字） | **禁止**跨 VM SWCC CAS；持锁写+flush+经版本发布 |
| `stringbag.hh`、value_bag | 引用改 offset；分配走 SWCC | 内部布局 |
| `kvthread.hh/.cc` | remote 池→shard；limbo 元素→offset；epoch→HWCC | 双池/limbo/quiesce 结构 |
| `kvrow.hh`、`query_masstree.*` | `row_type*`→`ValueRef` | `run_get1/replace/remove/scan` 流程 |
| `masstree_sidle.hh` | 迁移执行器→副本复制/淘汰 | traverse 结构、回调签名形态 |
| `sidle_policy.hh` / `sidle_worker.*` | 预算→副本预算；队列→`QueuedNodeRef`；访问计数→本地数组 | 全部阈值/水位/层数公式与五 worker 控制流 |
| `src/kv/masstree.h` | ctor 接共享池 + 配置；启动副本 worker | 对外 API 形态 |
| `src/helper.cpp` | 绑核按配置/在线核数，去掉硬编码核数 | 其余 |
| `CMakeLists.txt` | 摘除 memkind/DAX/ART/老基准；接入 `dsidle/` 与测试 | — |

### 4.3.2 读路径（Get / 内部 lower 定位）实现要点

整次 Get/定位必须落在**同一个分布式 epoch 临界区**内（进 epoch → 完成所有
节点与值读取 → 出 epoch），禁止“验版后出 epoch 再按 ValueRef 读值”。

1. 读 `RootControl`：`v1 → root_ref → v2`（§4.2）。
2. 对当前 `NodeRef`：`s1 = stable()`（不得采用带锁/已删版本）。
3. **副本命中路径（规范序；§3.4/§4.5 均以本序为准）**：

```text
s1 = stable();
进入目录 read-side（per-slot seqlock 偶数快照；唯一方案，禁止本地 RCU）;
槽匹配 {NodeRef, gen==s1.gen, cached_version==s1.v} ? 拷贝 local_ptr 所需
  字节到栈/调用方缓冲（性能要求：只拷本次查找实际需要的部分——值叶命中
  只拷 value 字节，内部节点只拷键区/child 槽，勿整节点 memcpy）: 记为 miss;
退出目录 read-side;            // 命中与否都必须退出，否则 demote 会饿死
命中时：s2 = stable(); 要求 s1.v==s2.v、gen 不变、未锁未删；
  否则按 miss 处理（可惰性清槽），走步骤 4。
```

   命中成功：叶 value 命中则 `access_count[node_id]++`（与 generation
   绑定），**不得**再解引用 SWCC `ValueRef`。
4. **规范路径**：若 `seen[node_id].gen != s1.gen` **或**
   `seen[node_id].version != s1.v`，对该节点覆盖的 SWCC cacheline 精确
   invalidate；再读规范节点体；然后 `seen = {s1.gen, s1.v}`。
5. **值读取契约（值无独立控制项）**：从叶解出 `ValueRef` 后，必须在**同一对**
   叶版本校验之间完成 value 字节拷贝到调用方缓冲：
   `s1.v →（必要时 invalidate 值对象行）→ memcpy value → s2.v`，且相等、
   未锁、未删；否则整段重试。禁止把 `ValueRef` 存出 epoch 后再解引用。
6. 步骤 5 的 `s2` **就是**离开节点校验；除非整节点重试，不得对同一节点做
   第三次 `stable()`（§1.8：一次行读元组复用到 v2）。
7. 普通读不取任何树锁、不碰全局结构；目录同步见 §4.5。

**跨 VM 副本失效模型（硬性，防臆造广播协议）**：写者只改 HWCC 版本 + SWCC
体，**从不**触碰其他 VM 的副本目录（本地 DRAM 不可达也不允许 mailbox/
广播 invalidate——§1.8/§3.4 明令禁止）。其他 VM 的陈旧副本仅在下次命中时
因 `gen/version` 失配而失效（惰性），随后由淘汰或重新 promote 处理。
**同 VM** 写者在发布新版本后，应把本 VM 目录中该 `node_id` 的槽清除或标记
失效（write-side 下），避免预算被永久 miss 的槽占住。

施工硬合同：`stable()` + 副本命中逻辑**内联**在现有 `reach_leaf`/查找路径
中（`NodeVersionAccessor`/`StableView` 是唯一访问器）；**不得**另建
`resolve_node` 式包装层引入双重间接。裸 `NodeView.raw` 仅栈上瞬时使用。

### 4.3.3 写路径（无 SMO 的 Put/Delete）

1. 原流程搜到目标叶并记录版本（epoch 内）。
2. 叶控制项上 CAS 取写锁（原 `lock()` 语义；`stable()` 对读者屏蔽锁定版本）。
3. 锁内：invalidate 将读改的 SWCC 行；复验 generation/键区/permutation；
   **成对**预留新 `value_bag`（及必要时 ksuf）——分配失败则不改叶、解锁重试。
4. 按原 permutation 发布顺序更新叶槽；对实际修改的节点体、新值、新 ksuf
   做 writeback/flush（全部脏行逐行 `clwb` 后**单次** fence，不必每行
   fence）。
5. **可见性完成后**才 release 发布新稳定版本并解锁（线性化点）。
6. **旧值/旧 ksuf 回收（精确序）**：发布新版本并解锁**之后**，由写者线程把
   旧对象压入自己的 limbo：`{swcc_offset, size/class, tag,
   retire_epoch=globalepoch, owner_shard}`；仅当 `active_epoch >
   retire_epoch` 才 remote-free 回 owner shard（§4.1/§4.3.6）。**禁止**在
   持叶锁期间 free；value_bag/ksuf 无控制项，只走 SWCC retire。
7. 任何失败在发布前完整回滚 reservation，不留下半挂接对象。

### 4.3.4 SMO（split / root / layer / remove）

逐句保留原 `masstree_insert/split/remove.hh` 的锁定、验证、重试与发布顺序
（含 locked-parent 获取关系），只替换三类语句：

1. 指针 → `NodeRef` / `SwccOffset`；
2. 版本操作 → 控制项原子；
3. 新增 SWCC invalidate/writeback 与 epoch retire。

根替换经 HWCC `RootControl` 版本化发布（修复基线内部迁移不更新 `root_`
的缺口；新系统无规范迁移，但 root split / layer root 仍走此路径）。

硬性发布顺序：

1. 分配并初始化新节点 SWCC 体 + 配对 `NodeControl`（失败则成对回滚）；
2. flush 新节点（及新值若有）；
3. 父槽写入 `NodeRef`（非裸 SWCC 偏移）；
4. flush 父节点修改行；
5. 解锁/发布相关版本——**子可见 → 父发布 → 解锁**，与原 Masstree 一致。

### 4.3.5 Scan

移植原生 `masstree_scan.hh` 的版本化叶链扫描；**整次 Scan 落在同一 epoch
临界区内**（与 Get 相同）。规范：

1. `next_/prev_` 为 `NodeRef`；跨叶前对下一叶 `stable()`，再读 SWCC 体。
2. 每条发出的 KV 遵守 §4.3.2 值拷贝契约（`v1→memcpy→v2`）；禁止持
   `ValueRef` 跨叶/跨重试点。
3. 叶链前进：读 `next_` 前确认当前叶版本仍稳定；链接更新见 §4.3.7。
4. Scan 允许命中本地副本，但每个节点必须走与 Get 完全相同的 §4.3.2
   副本双检序；不允许"整段扫描只验首叶"。
5. 不提供全范围强快照，不引入 scan gate（与基线一致）。
6. 支撑 §1.5.3 / YCSB-E；**必须实现**，不得标 `unsupported`。

### 4.3.6 Epoch / RCU（安全内存回收；施工硬合同）

**槽状态（HWCC 矩阵，每 VM×每前台/后台线程一槽）**：

| 值 | 含义 |
|----|------|
| `INACTIVE = UINT64_MAX` | 未注册或已退出；**不参与** `min` |
| `ACTIVE = e` | 读侧临界区内，观察纪元 `e` |

启动：每 worker `register_epoch_slot()`。**槽的合法值只有 `INACTIVE` 与
`globalepoch` 快照**；任何路径（含移植基线 `rcu_start/rcu_stop`）都不得把
槽写成 `0`——基线用 `0` 表示"不在 RCU 内"的语义一律映射为 `INACTIVE`，
否则 `min` 永远卡在 0，回收停摆。

**读侧**：

- 前台**每个** API/trace op：`enter`（槽=`globalepoch`）→ 完成全部节点/
  值访问 → `exit`（槽=`INACTIVE`）。禁止中途 exit 后再解引用共享 offset。
- 后台 SIDLE worker：每次遍历/复制同理进出。

**线程退出（防孤儿 limbo）**：线程 join 前必须 (1) 反复 `rcu_quiesce` 直到
自己的 limbo 清空（唯一方案；不实现移交线程；超 30s 未空 hard fail）；
(2) 槽写 `INACTIVE` 并注销。limbo 永远归属某个活着的已注册线程。

**推进与回收（与读侧分离，禁止每 op 全量 quiesce）**：

- `globalepoch` 递增 + `rcu_quiesce`：默认每线程每 **50** 次 op（对齐基线）
  以及后台 worker tick；**禁止**在热路径 spin 等待全局 quiesce。
- `active_epoch = min({槽 | 槽 ≠ INACTIVE})`；若无 ACTIVE 槽则
  `active_epoch = globalepoch`。min 扫描只发生在 `rcu_quiesce`（每 50
  op），不在每 op 路径。**每个 epoch 槽独占 cacheline（`alignas(64)`，
  防跨 VM 伪共享）**，HWCC 预算按 64B/槽核算。
- limbo 在本地 DRAM：元素见 §4.1；仅当 `active_epoch > retire_epoch` 才
  remote-free / 归还分配器。
- **NodeControl 与配对 SWCC 体成对 retire**：先从树摘除并 flush 父边 → 控制项
  `alloc_state=RETIRING`、记 `retire_epoch` → 入 limbo；复用控制项仅当
  epoch 安全 **且** 配对 SWCC 已可复用；复用时 `generation++`（§4.2）。
- ABA：`generation` + epoch；禁止仅靠指针相等认定同一逻辑节点。

### 4.3.7 叶链更新（修正基线“CAS on pointer”在 SWCC 上的不适用）

基线 `btree_leaflink.hh` 对 `next_/prev_` 做 CAS。D-SIDLE 中这些字在 **SWCC
节点体**，**禁止**跨 VM 对其做 C++/硬件 CAS/RMW（§1.7）。施工规范：

1. 链接修改仅在持有相关叶（及原协议要求的父）`NodeControl` 写锁时进行；
2. 顺序：`invalidate` 链字段行 → 普通 store `NodeRef` → `writeback/fence` →
   解锁/发布版本；
3. 读者经 `stable()` 看见新版本后才采信新 `next_`；
4. 单测必须覆盖：两 VM 并发 split 链更新，一端若误用 SWCC CAS 则
   NonCoherent/注入后端应失败。

### 4.3.8 基线缺口处置

| 基线事实 | 处置 |
|----------|------|
| 内部节点迁移不更新 `root_` | 规范迁移删除；根变更走 `RootControl` |
| `make_root` 硬编码 depth=1 | 保留（与策略一致），加注释 |
| 全局 `operator new` 缺 return | 随 DAX 后端一并废弃 |
| 本地记账不对称 | 副本目录集中记账（发布 +、淘汰 −） |
| `cxl_percentage` 双重用途 | 废弃 stride；配置改名 `hot_percentage_seed` |
| `lower_bound/range_scan` 未实现 | `Scan(start,n)` 用 `run_scan`；lower_bound 不补 |

## 4.4 SWCC 可见性

新文件 `dsidle/swcc_visibility.h`：

- 精确到对象覆盖 cacheline 的 `invalidate` / `writeback`（`clflushopt`/`clwb`
  + 内存屏障）；指令不可用时回退 `clflush`。
- 每线程已见表：**按 `node_id` 下标的稠密/分段数组**（与 `access_count`
  同款；禁止 hash map——它在每次节点访问的热路径上），元素
  `{generation, version}`；generation 变化视为未见过，必须 invalidate。
- 树节点固定顺序：**锁内 invalidate → 修改 → flush/fence → 发布版本**。
- 值对象与 stringbag 不设控制项，由所属叶版本 + §4.3.2 值读取契约保护；
  叶发布前必须 flush 新值/新 ksuf 覆盖行。
- 分配器链节点的可见性顺序见 §4.1（与树协议同级，不得省略）。

## 4.5 SIDLE 策略：迁移 → 副本

| 原语义 | D-SIDLE 语义 |
|--------|----------------|
| local 节点 | 本 VM 有有效本地副本 |
| remote 节点 | 仅有 SWCC 规范节点 |
| 提升热叶 | 从稳定规范快照复制到本 VM DRAM（规则见下） |
| 递归提升远端祖先 | 复制热叶到首个已有副本祖先之间的路径 |
| 降级冷叶 | 淘汰本 VM 冷叶副本 |
| 父无本地子才继续降级 | 父副本不再被本地子路径引用才继续向上淘汰 |
| 根钉在本地 | 每 VM 固定保留有效根副本 |
| 本地内存 95%/85% 水位 | 每 VM `replica_budget_mb` 同款水位 |
| `decide_new_node_position` | 新规范节点一律 SWCC；depth ≤ 阈值时创建后立即在本 VM 发副本 |
| 队列 64 位地址 | `QueuedNodeRef{NodeRef, generation}` |

必须原样保留：16 桶直方图、hot/cold/warm、水位公式、层数估算、
`queue_waiting_threshold=10`、cooler 减半、trigger ×5、threshold adjuster
三态、五 worker 职责与唤醒条件。

允许的表示差异（逻辑不变）：

1. 每 VM 在自己的访问流上独立跑同一套选择算法。
2. `access_time` 改为本 VM 本地数组；`leaf_metadata` 中字段保留但读路径不
   再更新。

**访问计数与 generation（硬性）**：

- 唯一方案：`NodeControl` 从 FREE→ALLOCATING 时把本 VM
  `access_count[node_id]` **清零**并丢弃该 `node_id` 上 generation 不匹配
  的副本槽；计数数组按 `node_id` 单维下标存放，**不实现**
  `(node_id, generation)` 双键。
- 禁止槽位复用后沿用旧热点计数喂直方图。

**副本内容规则（硬性）**：

| 节点类型 | 复制内容 | 命中后行为 |
|----------|----------|------------|
| 内部节点 | 节点体（含 child `NodeRef` 槽、permutation/iksuf） | 比较键后沿 `NodeRef` 继续；不深拷贝子树 |
| 叶且 `lv_` 为 **value** | 节点体 + 外部 ksuf（若有）+ **value 字节** | 命中后不再解引用 SWCC `ValueRef` |
| 叶且 `lv_` 为 **layer 根** | 节点体 + ksuf（若有）；`lv_` 保留 `NodeRef` | **禁止**把 layer 指针当 value 拷贝；命中后继续解析下层 `NodeRef` |

执行：`masstree_leaf_migration/internode_migration` 就地改写为
`masstree_leaf_replicate/internode_replicate`——读稳定版本 → 按上表拷贝 →
二次验版 → 登记副本目录；**不改规范树父指针/叶链、不 RCU 释放规范节点**。
`masstree_leaf_traverse` 改为 epoch 内走规范叶链，读本 VM 访问计数喂直方图。

**副本目录并发协议（硬性；防 UAF / 撕裂指针）**：

```text
ReplicaSlot {
  seq;                // 本地 DRAM；per-slot seqlock（唯一方案）
  generation;
  cached_version;
  local_ptr;          // 指向自包含副本缓冲
  bytes;
  kind;               // internal / value_leaf / layer_leaf
}
```

- 下标 = NodeControl slab index；结构 = 按 slab index 的分段稠密数组
  （每段 4096 槽惰性分配，唯一方案）；**禁止** `unordered_map`。
- **发布（promote）**：分配新缓冲并填满 → 写 `generation/cached_version/
  bytes/kind` → 发布 `local_ptr` → seqlock 偶数解锁（禁止 epoch 发布变体）；
  旧缓冲仅在**无本地读者**后 `free`。
- **Get 命中**：按 §4.3.2 步骤 3 的规范序执行（`s1` 预匹配 → read-side 拷贝
  → **无论命中与否退出 read-side** → `s2` 双检）；拷贝期间禁止释放该
  `local_ptr`。
- **淘汰（demote）**：标记无效 → 等待无读者 → `free` → 预算 −；不得在 Get
  拷贝窗口释放。
- **失效来源**（§4.3.2）：远端 VM 写入只能靠版本失配惰性失效；本 VM 写者
  发布新版本后清除/失效本 VM 该 `node_id` 槽。禁止跨 VM 写他人目录。
- 预算记账在发布/淘汰时更新，对接 `sidle_policy::update_local_memory_usage`。
- **根副本钉住 = 跟随当前根**：钉住对象是 `RootControl.root_ref` 当前值；
  检测到 `RootControl.version` 变化（root split/layer root 替换）时刷新钉住
  槽指向新根，旧根 `NodeRef` 变为普通可淘汰节点。

## 4.6 软件延迟模拟（照搬 cxlkv）

**决策：逐文件移植 `../cxlkv` 的 `src/utils/include/latency_simulator.h` 与
`src/utils/src/latency_simulator.cc` 到 `dsidle/latency_simulator.{h,cc}`，
保留 `latency_sim` 命名空间与全部实现细节。** 记入 `搬运清单.md`。

必须保持一致的细节：

1. **TSC 校准**：`std::call_once` + 4ms `steady_clock` 窗口 `_mm_pause`，
   `__rdtsc` 算 `ticks_per_ns`；正式性能模式 TSC 不可用则拒绝启动。
2. **忙等补齐**：`DelaySpinNs` 按目标 tsc busy-spin + `_mm_pause`。
3. **线程本地状态**：pending_delay_ns、xorshift64、组相联 LRU；Configure
   递增 generation 使旧线程状态失效。
4. **作用域协议**：`BeginScope` 嵌套计深；访问只累积 pending；
   `EndScopeAndDelay` 在最外层、**释放全部节点锁/epoch 之后**一次补齐。
5. **快路径闸门**：进程级 relaxed 原子；关闭时 Record* 单分支返回。
6. **行粒度记账**：`[addr, addr+bytes)` 折算 cacheline；PoolKind×AccessKind
   查 ns/line；`none` / `fixed_hit_rate` / `per_thread_lru` 三模型；
   `cache_hits_enabled=false` 时命中按 miss 计费。
7. **统计**：导出 `LATENCY_SIM_STATS`，字段名与 cxlkv 相同；恒等式
   `raw=hits+misses`、`delayed_ns=swcc+hwcc` 必须验证。

配置：本仓库将 `latency_inject` 放在根 `experiment_config.jsonc` 的
`dsidle.latency_inject`（**文件位置与 cxlkv 不同**：cxlkv 放在
`delta_policy_config_*.jsonc` / `LatencyInjectPolicyConfig`）。字段名与全集
必须与 cxlkv **同名同构且全部必填**：
`enabled, foreground_enabled, merge_enabled, stats_enabled, cache_line_bytes,
swcc_{read,write,flush}_ns_per_line, hwcc_{read,write}_ns_per_line,
hwcc_atomic_{load,store,rmw}_ns, cache_model, cache_hits_enabled,
cache_capacity_lines, cache_associativity, cache_fixed_hit_rate,
cache_hit_extra_ns`。`merge_enabled` 映射为后台副本/回收路径开关。提供
`configs/latency/cxlkv_e2e11_reference.jsonc`（25ns SWCC、117ns HWCC、
`per_thread_lru`——**仅**注入对比附录）与全零 / `--no-latency` 正式档。
正式 5M YCSB 默认走 `--no-latency`（§1.11），不得把 e2e_11 档冒充正式主表。

**与 cxlkv 机制同一性（施工核对用）**：

- API：`latency_sim::LatencySimulator::{Configure,BeginScope,EndScopeAndDelay,
  RecordRange,RecordLine,SnapshotStats,TakeStatsAndReset}`；`PoolKind::
  {kSwcc,kHwcc}`；`AccessKind` 含 read/write/flush/atomic_*。
- 禁止：在持有节点锁 / epoch 临界区内 `EndScopeAndDelay`；把模拟器逻辑
  散落进 Masstree 状态机；用 `sleep_for` 替代 TSC spin；裸共享访问绕过
  wrapper（须审计 allowlist，照搬 cxlkv 延迟审计思路，施工中新建
  `延迟插入审计报告.md`）。
- 精度边界声明与 cxlkv 迁移指南相同：不模拟预取/MLP/跨节点 invalidation
  流量；报告必须并列 raw/hit/miss/delayed_ns。

插入点统一经 `dsidle/latency_simulator.h` 中的薄内联 wrapper
（禁止散落裸调用）：

| 访问 | PoolKind | AccessKind | 位置 |
|------|----------|------------|------|
| 规范节点体读/写 | kSwcc | kRead/kWrite | masstree 各节点访问点 |
| ksuf / value_bag | kSwcc | kRead/kWrite | assign_ksuf、值拷贝与发布 |
| 精确 invalidate/writeback | kSwcc | kFlush | `swcc_visibility.h` |
| 分配器 slab/free 链 | kSwcc | kRead/kWrite | shard_allocator |
| `version_and_state` 读/CAS/写 | kHwcc | kAtomic* | nodeversion 访问器 |
| RootControl / epoch / shard 头 / 诊断 | kHwcc | 对应原子类型 | 各自实现 |

作用域：runner 一个 trace op = `BeginScope(kForeground)` … 退出 epoch/释放锁
… `EndScopeAndDelay()`；trigger/cooler/executor 用后台 scope。本地 DRAM
副本读写、直方图、队列不计费。

验证：`DelaySpinNs(1000ns)` 误差 < 20%（RelWithDebInfo）；仅 SWCC / 仅 HWCC
两组 profile 各自只让对应池 `delayed_ns` 非零；Debug 或 verbose/extra_check
开启时 `enabled=true` hard fail。

## 4.7 实验接口层（与 cxlkv 对齐）

| 资产 | 要求 |
|------|------|
| `experiment_config.jsonc` | **拓扑 schema 见 §1.6.1**（与 cxlkv 根配置同构的部分）；选择器 `DSIDLE_EXPERIMENT_CONFIG_JSONC`。`e2e` 仅强制 `foreground_worker_count_per_vm`（同 cxlkv）。`dsidle{replica_budget_mb,hot_percentage_seed,fixed_key_size,fixed_value_size,trace_dir,latency_inject}` 为本仓策略/runner 段（字段名对齐 cxlkv policy，**位置不同**）。未知/缺字段 hard fail |
| trace 格式 | **与 cxlkv 逐字节同构**：`<OP> <KEY_LEN> <LEN><KEY>`（`LEN` 与 `KEY` 紧挨、无空格）；文件 `<trace_dir>/worker<N>.txt`；OP∈PUT/GET/DELETE/SCAN；GET/DELETE 要求 `LEN=0`；SCAN 的 `LEN`=limit。解析器对齐 cxlkv `ParseTraceLine` 语义（本仓库可命名为 `ParseTraceLine`，勿另造格式） |
| PUT 值 | trace **不含 value 正文**；runner 用与 cxlkv 相同的 `FixedTraceValue`（字符集 `'!'..'~'`，长度=`fixed_value_size`，每 worker 独立 RNG）；忽略 PUT 行内 `LEN` 作为实际写入长度 |
| key 定长 | runner 将 key 右填空格至 `fixed_key_size`（与 cxlkv `FixedTraceKey` 一致） |
| `dsidle/e2e_trace_runner` | 每 VM 一进程；按 `foreground_worker_count_per_vm` 起 N 线程，线程 t 回放 `worker{node*N+t}.txt`；将每条 trace op 映射为 §1.5.3 的 Put/Get/Delete/Scan；**跨 VM 相位屏障**：cxlkv = bridge+tap + guest IP 上 `sdl::notify`；本仓默认 **ivshmem/共享内存 barrier 或 host 侧 SSH 编排**（刻意分歧，须在修改日志声明；**禁止**在无 tap/guest 互通时声称 TCP `sdl::notify` 同构）；屏障耗时**不计入**应力窗口。输出与 cxlkv 逐字段对齐：`E2E_TRACE_HEARTBEAT phase=<p> node=<n> ops=<delta> total=<cum> elapsed_s=<s>`（**每 ≥1s 一次，禁止每 op 打印**）、`E2E_TRACE_TIME_US phase=<p> node=<n> ops=<ops> duration_us=<us> trace_first=<f> trace_workers=<w> batch_ops=<b>`、`LATENCY_SIM_STATS`、`DSIDLE_MEMORY_STATS`；可选本仓诊断行不得冒充 cxlkv 字段名（**禁止**发明 `E2E_TRACE_OP_COUNTS` 并标为 cxlkv 对齐）。全部计数用 TLS，仅相位边界聚合，HWCC 诊断计数只在相位边界写 |
| YCSB-cpp | 见 §1.5.2 / 4.9：同 SHA submodule + 本仓库独立生成入口 |
| 正式性能约束 | RelWithDebInfo、verbose=false、extra_check=false；不满足拒绝打正式性能标记 |

## 4.8 VM 镜像制作与启动脚本

交付满足 §0.3 / §1.5.1。基线无 VM 设施。新增四个脚本放**项目根目录**，
`dsidle_` 前缀；语义照搬 `../cxlkv` 的 make_img/init 流程与 QEMU 命令行（可
拷贝脚本进本树后改），用 bash 独立实现，不依赖 fish/Rust，**不调用**
`../cxlkv` 运行时路径。与 cxlkv **职责同构、配置字段同名**；正式对比尽量
采用相同拓扑**取值**（数字同构，不是共用镜像/产物）。照搬关系记入
`搬运清单.md`。

1. **`dsidle_make_vm_img.sh`**（§0.3 / §1.5.1）：在本仓库用 mkosi（或已拷贝
   进本树的等价流程）产出 `image/root.img`；配置在本仓库 `image/`；已存在
   且非空则跳过（`--force` 重建）；guest 预装构建依赖；注入
   `vm.local_ssh_pub_key`。**不调用** `../cxlkv` 的 make_img，**不读取**其
   成品 `root.img` 作为默认输入。属重操作，执行前须用户允许。
2. **`dsidle_init_vms.sh`**（配置/预检/绑定细节见 §1.6）：
   - 读配置：`$DSIDLE_EXPERIMENT_CONFIG_JSONC` 或 `--config`，默认
     `experiment_config.jsonc`；python3 剥注释后 `json.load`；`numa_node`
     规范化为 int 列表（单值与数组同构）。
   - **预检（对齐 cxlkv，正式路径 hard fail）**：
     1. shared/vm NUMA 集合均存在于宿主机；
     2. 多 NUMA 主机上 shared∩vm 必须为空（重叠仅当 `--allow-overlapping-numa`）；
     3. `host_cpu` 三组互斥、在线、核∈ vm NUMA；
     4. `len(vm_cores) ≥ count×core_count`；
     5. MemAvailable（含可回收旧 QEMU RSS）≥ 全部 VM RAM；
     6. `size_mb` 为 2 的幂；
     7. **本仓/YCSB 布局约定**（与 cxlkv 根配置及 YCSB 改写器一致，但
        **不是** cxlkv `init_vm` 的硬断言——cxlkv init 只保证 hwcc/swcc 不
        重叠）：`hwcc.offset_mb==0`、`swcc.offset_mb==hwcc.size_mb`、
        `hwcc.size_mb+swcc.size_mb==size_mb`，任一违反 hard fail。
   - **host tuning（与 cxlkv 刻意分歧）**：cxlkv `init_vm` **直接应用**
     host tuning；本仓默认**只检查并报告**；`--apply-host-tuning` 须显式
     授权（见 §1.4 / §1.5.1）。
   - **清旧 VM**：按 `$VM_STORAGE/vm_*/qemu.pid` 精确 kill（须用户允许）。
   - **共享 backing**：`numactl --membind=<shared_numa_csv>` 创建/截断/
     prefault/清零 → `dsidle_shared_pool --init-pool` 写 PoolHeader
     （hwcc/swcc 边界）。
     多节点列表时与 cxlkv 相同传 CSV 给 numactl。
   - **每 VM QEMU 启动**（照搬 cxlkv 参数集；VM 侧 membind/cpunodebind=
     该 VM 对应的 `vm.numa_node`；`host-nodes` 与之一致）：

     ```text
     numactl --cpunodebind=<vm_numa> --membind=<vm_numa> -- qemu-system-x86_64 \
       -machine q35,accel=kvm,mem-merge=off -cpu <model> \
       -m <mem>M,maxmem=<mem>M \
       -object memory-backend-ram,id=vmram0,size=<mem>M,host-nodes=<vm_numa>,policy=bind,prealloc=on \
       -numa node,nodeid=0,memdev=vmram0 [-numa cpu,...] \
       -smp <core>,maxcpus=<core>,sockets=1,cores=<core>,threads=1 \
       -enable-kvm -display none -daemonize \
       -chardev socket,id=serial0,path=<vmdir>/serial.sock,server=on,wait=off,logfile=<vmdir>/serial.log \
       -serial chardev:serial0 -device virtio-rng-pci \
       -pidfile <vmdir>/qemu.pid -D <vmdir>/qemu.log \
       -device virtio-blk-pci,packed=on,num-queues=1,drive=drive0,id=virblk0 \
       -drive if=none,file=<vmdir>/root.img,format=raw,media=disk,id=drive0,cache=none,aio=native \
       -device virtio-net-pci,netdev=netssh<idx>,mac=<mac> \
       -netdev user,id=netssh<idx>,hostfwd=tcp:127.0.0.1:<ssh_base+idx>-:22 \
       -device ivshmem-plain,memdev=ivshmem \
       -object memory-backend-file,size=<shared_mb>M,share=on,mem-path=<shared_path>,id=ivshmem
     ```

     `<model>`：默认 `host`，AMD EPYC 用 `EPYC,topoext`（同 cxlkv）。
   - **收尾**：SSH 就绪；按 `host_cpu.vm_cores` 切片 `taskset -apc`；guest
     加载 ivshmem 模块；设备节点 = `device_path`。
3. **`dsidle_kill_vms.sh`**：按 pid 文件精确终止。
4. **`dsidle_check_vms.sh`**：**本仓增强**（cxlkv 无独立 check 脚本）。只读
   检查 QEMU cmdline、taskset、SSH、设备节点；另抽样 **numa_maps** 核对
   共享页 NUMA ∈ 配置 shared 列表（此条为增强，勿标成 cxlkv 对等）。

验证：`bash -n` + shellcheck 零 error；支持 `--dry-run`；实际重建须用户允许；
成功标准 = check 全绿 + 4 VM e2e 冒烟通过。

## 4.9 Trace 生成、一键 YCSB 与指南

交付满足 §1.5.2 / §1.5.3。

### Trace 生成器（本仓库独立完备）

- submodule：`thirdparty_libs/YCSB-cpp`，gitlink SHA **与 cxlkv 相同**。
- 调用入口：`thirdparty_libs/YCSB-cpp/scripts/generate_cxlkv_trace.sh`（或本仓库
  唯一入口，**不再做** `scripts/` 薄包装）；**禁止** `../cxlkv/.../generate_*.sh`。
- 必须能生成：`load`、`workloada`、`workloadb`、`workloadc`、`workloadd`、
  `workloade` 全套 worker 文件；参数与 cxlkv 一键脚本相同
  （zipfian load、A 的 UPDATE→GET+PUT、E 的 SCAN+PUT 等）。

### 脚本 `dsidle_run_ycsb_experiment.sh`（项目根）

逐节对齐 `../cxlkv` 的 `scripts/run_ycsb_trace_experiment.sh` 的选项、步骤与
产物布局（允许照搬 bash 后替换项目路径；记入搬运清单）：

- **选项集（与 cxlkv 同名；默认值以本列表钉死，与 cxlkv 的差异仅
  record/operation 冒烟默认）**：`--rounds`(1)、
  `--record-count`(冒烟默认 100000，正式对比显式传 5000000)、
  `--operation-count`(同上)、`--threads-per-node`(4)、
  `--out-dir`(`exp_data/ycsb_dsidle_<timestamp>`)、`--round-timeout`(7200)、
  `--base-config`(experiment_config.jsonc)、`--shared-numa`、
  `--shared-reserve-mb`(4096，同 cxlkv)、
  `--shared-size-mb`(自动向上取 2 的幂；**正式 5M 对比显式传 65536**)、
  `--workloads`(默认 **`a,b,c,d`**；允许集合 **`a,b,c,d,e`**)、
  `--no-latency`（**正式 5M 主表必须加此开关**，对齐 cxlkv 指南）、
  `--cache-flush-mb`(512)、`--skip-build`、`--skip-vm-init`、
  `--skip-trace-gen`、`--skip-standalone-load`、`--prepare-only`。
  固定 4 VM；HWCC 固定 1024MB、其余给 SWCC。生成 policy 时**无条件**像
  cxlkv 一样把 `cache_model` 置 `none`、`cache_hits_enabled=false`；
  `--no-latency` 仅关 enabled 族标志。
- **workload 封闭集合**：小写、逗号分隔、不去重；非法输入在任何副作用前
  stderr 报错退出 2。必须支持 load + A/B/C/D/E 全阶段回放；E 映射为
  `SCAN`+单条 `PUT`；UPDATE/INSERT 均映射为独立单条 `PUT`。D-SIDLE 有 Scan，
  **禁止**将 E 标为 unsupported。正式对比默认跑 A–D，E 用
  `--workloads a,b,c,d,e` 显式打开。
- **步骤**：清旧进程 → 生成本轮 `experiment_config_ycsb_4vm.jsonc`（正式
  对比时按 §1.11 覆盖 `size_mb=65536` / SWCC=64512 / `--shared-numa`）→
  生成 trace config 与 `run_meta.json`
  （参数 + git SHA + 复现命令 + 配置 SHA256）→ 调本仓库 YCSB-cpp 生成
  load + 所选 workload（**fixed 32/32**）→ 只读校验 trace → RelWithDebInfo
  构建 → VM 检查
  （默认 `dsidle_check_vms.sh`；`--reinit-vms` 且用户授权才
  `dsidle_init_vms.sh`）→ sync 进 guest 构建 → 每轮清 cache → pool reset →
  load → run → 收集 → 汇总。
- **汇总 `scripts/summarize_ycsb_experiment.py`**：对齐 cxlkv
  `summarize_ycsb_trace_experiment.py` 字段：`ops_sum`、
  `duration_sec_max`/`avg_duration_sec`、`avg_ops_sum`；推导
  `ops_per_sec=ops_sum/duration_sec_max`（多轮用 avg）；**不**产出
  `ops_per_sec_from_avg_round_max`（该名仅 e2e_08/09）。附
  `DSIDLE_MEMORY_STATS` 与 `LATENCY_SIM_STATS`；产出 `YCSB实验报告.md` +
  `ycsb_summary.json` + CSV。
- **产物布局**与 cxlkv 指南同构（run_meta / runner.log / 报告 / csv / json /
  configs/ / traces/ / logs/ / round_logs/）。

### 文档 `YCSB指南.md`

章节对齐 `../cxlkv` 的 `doc/YCSB指南.md`：脚本概述、约束（固定 4 VM、
RelWithDebInfo、2 的幂池、HWCC 1024MB、NUMA、延迟注入构建约束、独立
benchmark 语义）、选项表、load 说明、快速命令、中断恢复、输出结构、失败
处理。**文档中每条命令必须实际跑通或 dry-run 验证后才可写入。**

### 脚本自检

1. `bash -n` + shellcheck 零 error；
2. `--prepare-only` 断言 worker 文件数与 manifest/trace 行数一致；
3. 冒烟 `--record-count 10000 --operation-count 10000 --rounds 1 --workloads a`
   端到端跑通，报告/CSV/JSON 一致；
4. 非法 `--workloads` 矩阵（`f`、`workloada`、`A`、`a,`、`a,,b`、`a,a`、
   `load,a`、空串）全部退出 2 且不产生 `--out-dir`；
5. `--skip-*` 恢复路径至少各验证一次。

============================================================
五、文件级任务清单
============================================================

**新建（`dsidle/`、`tests/`；文件数已按"克制"原则折叠，禁止再拆碎）**

| 文件 | 职责 | 估算 |
|------|------|------|
| `dsidle/shared_pool.h/.cc` | 池创建/attach、PoolHeader、双模式 mmap、**分布式 epoch 槽**、`--init-pool` CLI 入口（宿主侧池初始化，不另建 tools/） | ~550 |
| `dsidle/node_control.h` | **offset 类型族（SwccOffset/HwccOffset/NodeRef/ValueRef/QueuedNodeRef）**、64B NodeControl、RootControl、控制项 slab | ~400 |
| `dsidle/swcc_visibility.h` | invalidate/writeback/fence、已见版本表 | ~150 |
| `dsidle/shard_allocator.h/.cc` | SWCC shard/bump/remote-free、limbo 记录 | ~350 |
| `dsidle/replica_directory.h/.cc` | 分段副本目录（seqlock）、预算记账 | ~350 |
| `dsidle/latency_simulator.h/.cc` | 照搬 cxlkv；**Record 薄内联包装并入头文件**（不另建 mem_access.h） | ~650（照搬为主） |
| `dsidle/config.h/.cc` | `experiment_config.jsonc` 严格解析 | ~250 |
| `dsidle/e2e_trace_runner.cc` | trace 回放（含 `ParseTraceLine`/FixedTraceKey/Value，不另建 trace.h）、ivshmem/host 相位屏障（刻意非 TCP `sdl::notify`）、统计 | ~500 |
| `tests/…` | §六测试集；**优先拷贝改编 cxlkv e2e_08/09/10 与 latency 测试骨架**，禁止从零搭平行 harness | ~2000 |

新增 C/C++（除 latency 照搬与 `tests/`）**≤3000 行**，以上表为上限；超限
须先改本表再动工。**新增代码克制红线**：能就地改 Masstree/sidle_utils 的
不新建文件；不引入新第三方依赖；不做投机抽象/接口层；脚本以拷贝 cxlkv
后改 `dsidle_` 前缀为主，禁止自创第二套编排。

**根目录脚本**：`dsidle_make_vm_img.sh`、`dsidle_init_vms.sh`、
`dsidle_kill_vms.sh`、`dsidle_check_vms.sh`、`dsidle_run_ycsb_experiment.sh`、
`scripts/run_dsidle_e2e_rounds.sh`（统一 rounds 入口）、
`scripts/summarize_ycsb_experiment.py`、`YCSB指南.md`。

**就地修改清单**：见 4.3.1。每处 `// dsidle:` 标注。

============================================================
六、测试计划
============================================================

构建：**唯一强制配置 = RelWithDebInfo（`-O3 -g3 -march=native
-flto=full`，与 cxlkv 对齐）**，CTest 全接入。Debug / ASAN / UBSAN 构建
**可选**（仅排查具体 bug 时临时使用），不作验收门槛，不要求 CMake 特殊
支持（见「施工执行协议」第 4 条）。

## 6.0 完成判定硬门槛（无 bug 声明前提；不可降级）

改造完成后，**只有同时满足下列全部条件**，才允许在文档/口头上声称
“关键路径无已知 bug / 可进入正式对比”：

1. **全部单元测试通过**（§6.1，RelWithDebInfo；CTest 零失败）。
2. **三套强制端到端测试各连续通过 ≥10 轮**（§6.3；任一轮失败即整套不计
   通过，须修 bug 后从第 1 轮重计）。轮数默认脚本参数不得低于 10；禁止用
   冒烟规模冒充正式 e2e。
3. **四套强制补充 e2e 各连续通过 ≥3 轮**（§6.3 补充表：replica /
   scan_stress / reclaim / concurrency）。
4. 轮次日志（每轮 exit code、关键统计行、git SHA、配置哈希）写入
   `修改日志.md`；缺记录视为未验收。

数据量与场景**对齐 `../cxlkv`（branch `my-work`）** 对应套件，不得擅自缩小：

| 强制 E2E | 模仿 cxlkv | 规模与形态（默认值，可 env 放大不可缩小验收口径） |
|----------|------------|--------------------------------------------------|
| `dsidle_e2e_08` | `src/tree/test/e2e_08` | **100000** key；fixed key **8B**、value **8B**；多 VM 填充 + 跨 VM 读校验（及本系统必要的 delete/scan 断言） |
| `dsidle_e2e_09` | `src/tree/test/e2e_09` | **100000** key；fixed key **32B**、value **1000B**；填充 + 更新 + 跨 VM 读回校验 |
| `dsidle_e2e_ycsb` | `src/tree/test/e2e_10` | YCSB-cpp 生成：**recordcount=100000**、**operationcount=100000**、`threads_per_vm=4`、zipfian；阶段 **load + workloada**（UPDATE→GET+PUT）；经本仓库 `e2e_trace_runner` 多 VM 回放 |

说明：正式性能对比仍可用 5e6 等更大矩阵（§6.4）；**无 bug 门槛以本表
100k 三套各 ≥10 轮为准**。小规模冒烟（如 1e4）只作开发便利，**不计入**
§6.0。

## 6.1 单元测试（关键功能必须补齐；CTest 强制）

下列关键功能**必须有独立单元/单机多进程测试**（缺测 = 未完成，禁止用
e2e 代替）。RelWithDebInfo 全绿即验收。`ctest --repeat until-fail:10` 与
ASAN/UBSAN 仅作排查偶发失败/疑似 UAF 的**可选**手段，不是门槛。

1. offset/句柄（大小、trivial-copy、空值、generation ABA）；
2. NodeControl / RootControl（sizeof/alignof=64、lock-free、nodeversion
   对拍、控制项复用发布序、双检读根）；
3. shard 分配器（并发 alloc/free、remote-free、slab、epoch 回收、**SWCC
   链/free 可见性**、limbo 字段完整性、INACTIVE 槽不参与 min）；
4. 偏移化 Masstree：随机 Put/Get/Delete/Scan 对拍 `std::map`；长键 layer、
   共同前缀、二进制零、8B 边界、split/remove/root split、叶链扫描；
5. **Get 契约**：值拷贝落在叶版本双检与同一 epoch 内（含“验版后延迟读值”
   负向用例必须失败/不可达）；
6. **副本**：layer 叶不把 `NodeRef` 当 value；value 叶命中不回 SWCC；
   generation 复用后 `access_count` 清零、旧 gen 不可命中；**命中后 Put
   并发不得返回线性化点之后的陈旧值**（双检专项）；目录 publish/evict 与
   Get 并发无 UAF（可用 ASAN 辅助排查）；**跨 VM 惰性失效**：VM A 写后不触碰 VM B 目录，
   B 无陈旧读且槽仅在本地失效/淘汰；**同 VM 写后清槽**：写者发布后本 VM
   槽失效，后续 Get 不永久 miss；**根钉住跟随**：root split 后钉住槽指向
   新根、旧根可淘汰；
7. **叶链**：多进程 split 更新 `next_` 无 SWCC CAS；缺 flush 时注入后端失败；
8. **Scan**：整 Scan 在同一 epoch；Scan∥Delete 无 UAF；发出 KV 遵守值契约；
   Scan 命中副本走同一双检；
9. **多进程不同基址**：同一 backing 多进程不同地址映射并发读写、关闭重挂载；
10. **回收专项**：覆盖写（Put-replace）路径旧值回收（不只 Delete）；layer
    创建/清空后成对 retire 无泄漏；线程退出前 limbo 排空（唯一方案）；
    op 退出与线程退出后 epoch 槽为 `INACTIVE`（禁 0）；
11. 延迟模拟（4.6 验证项）；
12. 策略对拍（**一次性验证**：M5 期间跑一次通过并记日志即可，不进日常
    回归集）：`git worktree add` 基线提交构建原始单机版，喂相同访问序列，
    断言 hot/warm/cold 分类与提升/降级候选序一致（仅"迁移 vs 复制"与每 VM
    容量为登记差异）；
13. **高并发冒烟（单机多线程）**：每 VM 模拟 ≥`core_count` 线程混合
    Put/Get/Delete/Scan，对拍 `std::map` 或交叉校验，运行 ≥30s 无死锁/
    无泄漏（used 回基线±分配器缓存界）。

## 6.2 脚本与工具

1. 全部新脚本 `bash -n` + shellcheck 零 error；
2. `dsidle_init_vms.sh --dry-run` 打印完整 QEMU 命令与预检；
3. `--prepare-only` 产物断言；
4. 小规模 e2e 冒烟（开发用）；非法 workload 矩阵。

## 6.3 强制端到端测试（≥3 套；每套 ≥10 轮）

在 4 VM（与 `experiment_config.jsonc` 正式拓扑一致）上交付并可一键复跑：

1. **`dsidle_e2e_08`**：见 §6.0 表；输出对齐 cxlkv 风格的相位耗时 /
   延迟采样 / 内存统计行；断言跨 VM 读一致性与无泄漏；**必须含跨 VM
   Delete→Get miss** 与至少一轮 Scan 抽样校验（可在 fill/read 相位外附加
   短相位，不得缩小 100k 口径）。
2. **`dsidle_e2e_09`**：见 §6.0 表；大 value 路径必须真实分配/拷贝/读回。
3. **`dsidle_e2e_ycsb`**：见 §6.0 表（等价 cxlkv e2e_10）；断言 load 与
   workloada 均成功、`E2E_TRACE_TIME_US` 可汇总、无 hard fail。

编排要求：唯一交付
`scripts/run_dsidle_e2e_rounds.sh --suite {08|09|ycsb|replica|scan_stress|reclaim|concurrency}`，
默认 `--rounds 10`（补充四套默认 3）；禁止再拆平行 rounds 脚本；失败立即
停并保留该轮日志。失败修复后只从第 1 轮重跑**该套件**，其余达标套件不
重跑（见「施工执行协议」）。

吞吐口径（按套件；勿混用字段名）：
- **单轮**（所有套件）：`ops_sum / (max-across-nodes duration_us / 1e6)`；
- **e2e_08/09 多轮主字段**：`ops_per_sec_from_avg_round_max`（与 cxlkv
  summarize_e2e08/09 同名；p50/p90 仅附录）；
- **YCSB / e2e_ycsb 多轮**：用 `avg_ops_sum` / `avg_duration_sec`（或等价
  由每轮 max-across-nodes 再平均）推导 `ops_per_sec`；**禁止**要求 YCSB
  汇总输出 `ops_per_sec_from_avg_round_max`。
应力相位不含 init/barrier/drain。本仓 rounds 默认 ≥10 是验收门槛（cxlkv
脚本默认 3/1，见 §1.11）。

**强制补充集成（计入完成判定；各 ≥3 轮；不替代上表三套）**：

| 套件 | 断言 |
|------|------|
| `dsidle_e2e_replica` | 副本命中∥跨 VM Put：无陈旧读；promote/demote∥Get 无崩溃/无错值 |
| `dsidle_e2e_scan_stress` | Scan∥Put/Delete/SMO；YCSB-E 冒烟 `--workloads e` 1 轮通过 |
| `dsidle_e2e_reclaim` | 大量 Delete 后 epoch drain，SWCC/HWCC used 回基线±缓存界；控制项复用 generation 单调 |
| `dsidle_e2e_concurrency` | 4 VM × 每 VM 满 `foreground_worker_count` 混合负载 ≥60s 无死锁 |

### 6.3.1 公平性核对表（相对 cxlkv `origin/my-work`；数据面可不同）

施工验收时逐行打勾记入 `修改日志.md`（对照仓库必须是 **my-work**，勿用
过时 `main` 数据面文档）：

| 项 | 要求 |
|----|------|
| 拓扑 | `shared_memory`/`vm`/`host_cpu` 字段同名；默认根配置 32G；**正式 5M YCSB 覆盖 64G**（§1.11）；4×8、HWCC 1024MB、跨 NUMA |
| 构建 | RelWithDebInfo = `-O3 -g3 -march=native -flto=full`（与 cxlkv `CxlkvBuildOptions.cmake` 一致）；**禁止**关 LTO 做正式对比 |
| worker 文件 | `worker{node*N+t}.txt` 命名与数量；正式 YCSB fixed **32/32**；trace 字节可与 cxlkv 互换 |
| 屏障相位 | 屏障耗时不计入应力吞吐；机制可为 ivshmem/host 编排（**刻意**与 cxlkv tap+TCP `sdl::notify` 不同，见 §4.7） |
| 统计行 | `E2E_TRACE_TIME_US` / `E2E_TRACE_HEARTBEAT` 与 cxlkv 同名；e2e_08/09 多轮用 `ops_per_sec_from_avg_round_max`；YCSB 用 `ops_sum`/`duration_*`/`avg_*`（§6.3） |
| 读校验 | e2e_08 read 相位**逐字节**校验 value（cxlkv `VerifyValue` 同构），非仅存在性 |
| `latency_inject` | 字段全集同构（位置可在 `dsidle.`）；正式 5M YCSB = `--no-latency`；e2e_11 档仅附录；仅 RelWithDebInfo+verbose=false+extra_check=false 可 enable |
| Scan 合同 | `(start, limit)`，`limit==0` 不限制；trace 无端键（cxlkv 公开 API 的 `end_exclusive` 在 trace 中恒为空，语义兼容） |
| CPU 预算 | 报告 foreground worker 数；对照 cxlkv 前台 + merge 池（e2e_08 leader 另有 4 aux 线程）说明两侧后台线程预算 |
| host tuning / check | host tuning 默认只检查（与 cxlkv 应用分歧）；`check_vms`+`numa_maps` 为本仓增强 |
| 独立仓 | §0.3：无运行时依赖兄弟仓/共用成品镜像 |

## 6.4 性能矩阵（与 cxlkv 并列对比；在 §6.0 通过之后）

- 同宿主机、同 VM 拓扑、**同一份 trace**；
- **正式主表**：对齐 cxlkv 指南——`--no-latency`、`--shared-size-mb 65536`、
  record/operation=5000000、fixed 32/32、workloads A–D（§1.11）；
- **注入附录**（可选，不进正式主表）：关闭计费 / e2e_11 档（25ns SWCC+
  117ns HWCC + LRU）/ 仅 SWCC / 仅 HWCC；
- 维度：YCSB load + A/B/C/D（主对比表）；另必须交付一组
  `--workloads a,b,c,d,e` 单轮冒烟产物（不进主表，证明 E 能力）；
  1/2/4 VM；每 VM 1/2/4 线程；不同副本预算；
- 每配置预热 1 轮 + 正式 ≥5 轮；YCSB 主字段用 `avg_ops_sum`/
  `avg_duration_sec`（或等价）；e2e_08/09 才用
  `ops_per_sec_from_avg_round_max`；附 p50/p90 离散度；
- 附分项：锁等待/版本重试、SWCC flush 字节、HWCC 原子次数、副本命中率、
  提升/淘汰次数。

## 6.5 验收清单

1. **§6.0 硬门槛已满足**：全部单元测试通过；`dsidle_e2e_08` /
   `dsidle_e2e_09` / `dsidle_e2e_ycsb` 各 ≥10 轮通过且日志入修改日志；
2. §1.6–§1.8：NUMA 配置/脚本与 cxlkv 同构；HWCC/SWCC 落位与一致性假设对齐
   `AGENTS.md`；无故意弱性能路径；延迟模拟与 cxlkv 机制同一；
3. 共享持久结构零裸指针（静态审计 + 不同基址测试）；
4. 普通 Get 不取锁；无 SMO 的 Put/Delete 只锁目标叶；无全局树锁/scan gate；
5. HWCC 预算按 64B/节点核算不超限；
6. Masstree 各 `.hh` 锁定/重试/发布顺序与基线逐句可对照；
7. SIDLE 策略参数与控制关系与基线一致（对拍过）；
8. 延迟注入恒等式与单池独立性成立；
9. §0.3 / §1.5：与 cxlkv（及任何同级仓）互不依赖；可拷贝源码但零运行时调用；
   VM 镜像由本仓库独立机制创建（非共用兄弟成品镜像）；trace/YCSB 一键脚本
   独立完备；load/A/B/C/D/E 与 Put/Get/Delete/Scan 语义对齐；
10. VM 四脚本 dry-run + 实跑全绿；YCSB 冒烟与非法输入矩阵全过；`--workloads e`
    冒烟通过；`check_vms`（本仓增强）验证 shared 页 NUMA 符合配置；
    正式 5M 命令含 `--no-latency --shared-size-mb 65536`（§1.11）；
11. 文档与最终代码一致；改造范围未引入 ART 或第二套树；
12. 分配器 SWCC 链/remote-free 可见性专项（跨进程 free→realloc→读）通过；
13. Get 值拷贝严格落在叶版本双检与同一 epoch 内（注入“验版后延迟读值”的
    负向测试必须失败/不可达）；
14. NodeControl 槽复用后 access_count 清零、旧 generation 副本不可命中；
15. layer 叶副本不把 `NodeRef` 当 value 拷贝；value 叶命中不回 SWCC 取 value；
16. **并发正确性**：无全局树锁；Get 乐观；Put/Delete 仅锁必要节点；叶链无
    SWCC CAS；多线程/多 VM 下线性一致（§4.3 + §6.1/6.3）；
17. **安全回收**：epoch 槽 INACTIVE 语义正确；retire 后仅 quiesce 才复用；
    控制项与 SWCC 体成对；`dsidle_e2e_reclaim` 通过；
18. **高并发**：§6.1-13 与 `dsidle_e2e_concurrency` 通过；热路径无每 op
    全局 quiesce、无虚调用兜底；
19. §6.3.1 公平性核对表全部打勾；§6.0 全部门槛（含补充四套）满足；
    §1.10 三条红线在里程碑日志中有核对记录。
20. §7.4 最终基线差异校验与清扫完成：对基线全量 diff 逐文件核对、死代码
    与脚手架已删、清扫后复验通过、清单入 `修改日志.md`。

============================================================
七、文档更新与死代码清理
============================================================

## 7.1 文档（交付时新建于仓库根，不改基线 README.md 与 guide.pdf）

| 文档 | 内容 |
|------|------|
| `PLAN.md` | 本文；随进度可标注完成状态，禁止把未完成工作写成既成事实 |
| `修改日志.md` | **施工中新建并持续追加**（仓库不预置历史日志）；见 §7.3 |
| `搬运清单.md` | 施工中新建：cxlkv 照搬件与 YCSB-cpp submodule 来源 |
| `延迟插入审计报告.md` | 插入点覆盖/不覆盖、与 cxlkv 对照 |
| `YCSB指南.md` | 施工中按 4.9 新建（勿沿用已删除的旧指南） |
| `README_DSIDLE.md` | 架构总览、从空白宿主机到 4 VM YCSB 的命令序列、精度边界说明 |
| `configs/numa/*.jsonc` | 2-NUMA 等示例拓扑（§1.6） |

## 7.2 死代码清理

1. 回滚使晚于基线的 `dist/`、`framework/` 等从工作树消失；历史经
   `dsidle-legacy-archive` tag 保留。
2. 基线内死代码：`migration_struct.hh` 保留不删；`cxl_allocator.*`、
   `cxl_cpp_allocator.*`、ART、老基准从构建摘除，文件保留。
3. 里程碑过渡桩必须在对应阶段收尾删除，不留双路径。

## 7.3 Git 检查点与修改日志（强制）

改造过程中必须保留可回溯检查点，禁止长时间堆未提交改动：

1. **每个里程碑（M0–M8）退出前**：对应测试绿 → **本地 `git commit`**（勿
   push，除非用户明确要求）→ 立即在 `修改日志.md` 追加一条记录。
2. **架构关键节点**（例如首次打通跨进程读写、首次接通副本策略、首次接入延迟
   模拟）即使未到里程碑末尾，也应单独 commit + 记日志。
3. **`修改日志.md` 每条至少包含**：日期、里程碑/检查点名、commit SHA、改动
   文件摘要、与基线的必要差异、测试命令与结果（通过/失败原因）。
4. M0 第一步：在仓库根**新建空的** `修改日志.md`（仅含标题与基线 SHA），再
   开始功能改动；禁止从已删除的旧日志或旧 `dist/` 文档拷贝叙述。
5. 清理死代码用独立 commit，不与功能改动混合；清理列表写入 `修改日志.md`。

## 7.4 最终基线差异校验与清扫（全部任务完成后强制）

§6.0 全部达标后、宣布"改造完成"前，执行一次全量差异审计与清扫：

1. **生成差异清单**：`git diff --stat
   1f1fa8ae9459935963a67a9beef6f30683c3c47b..HEAD` 全量列出相对基线的
   改动/新增文件，清单写入 `修改日志.md`。
2. **逐文件核对**：每个改动/新增文件必须能对应到本 PLAN 的某一节（§四/
   §五/§六/§7.1）或 `修改日志.md` 的某条记录；对应不上的即为可疑残留——
   删除，或补记存在理由后保留。
3. **删除死代码与脚手架**（仅限晚于基线新增的内容；基线文件只改不删，
   §0.2/§7.2 红线不变）：
   - 施工期临时调试代码：临时打印/计时、一次性 DEBUG 开关、被注释掉的
     代码块、TODO/占位桩；
   - 里程碑过渡桩与双路径残留；未被任何交付路径或 §六 测试引用的新增
     函数/头文件/CMake target；
   - 一次性实验脚本与中间产物（§7.1 交付文档与 §六 测试集除外）。
4. **清扫后复验**：RelWithDebInfo 全量重编 + CTest 全绿 + 三套 e2e 各
   1 轮冒烟（清扫不改语义，不必重跑 10 轮；复验失败则修复后重验）。
5. 清扫以**独立 commit** 提交；删除清单与差异审计结论记入
   `修改日志.md`。本节完成前不得声称"改造完成 / 可交付"。

红线：不得为绕过测试关闭断言或缩小数据量；不得以 PutBatch、弱化一致性或
跳过 flush 达成性能目标；不得让构建或运行依赖 `../cxlkv` 或其他同级仓库
（§0.3）；声称“无 bug / 可正式对比”前 **§6.0 硬门槛与 §6.5 验收清单必须
全绿**（缺 10 轮 e2e 或缺关键单测均不算完成）。

============================================================
八、实施顺序与里程碑
============================================================

执行方式见文首「施工执行协议」：逐里程碑实现 → RelWithDebInfo 构建 →
跑退出条件测试 → commit + 日志；M8 收官进入多轮 e2e 修复循环直到
§6.0 全部达标。

| 阶段 | 内容 | 退出条件 |
|------|------|----------|
| M0 回滚与骨架 | tag 存档、回滚基线；新建 `修改日志.md`；`dsidle/` 骨架、config 解析、CMake 摘除 DAX/ART；`experiment_config.jsonc`；本地 commit | 基线 + 空骨架可编译；日志含 M0 SHA |
| M1 池与分配器 | shared_pool（含 epoch 槽与 `--init-pool`）、node_control（含 offset 类型族）、shard 分配器；含 SWCC 链/remote-free 可见性 | 分配器单测 + 多进程不同基址 attach + free/reuse 可见性专项；commit + 日志 |
| M2 Masstree 偏移化 | 4.3 各 `.hh` 与 kvthread 就地修改（先单进程） | 单进程对拍 `std::map`；重挂载通过；commit + 日志 |
| M3 跨进程并发 | HWCC 原子、SWCC 可见性、根发布；**预填充树上的 Get + 覆盖 Put/Delete**（无 split） | 多进程线性化 + **规范路径值契约双检**过（副本属 M5）；commit + 日志 |
| M4 SMO 与 scan | split/remove/layer/叶链（无 SWCC CAS）/scan + epoch 回收 | 高冲突 SMO + Scan∥Delete + reclaim + 线程退出 limbo 排空过；commit + 日志 |
| M5 副本与策略 | 副本目录（seqlock）、replicate/evict、五 worker、per-VM 计数 | 策略对拍 + **副本命中双检/目录 UAF/根钉住跟随** 专项过；commit + 日志 |
| M6 延迟模拟 | 照搬移植 + 插入点 + scope | 4.6 验证项全过；commit + 日志 |
| M7 实验设施 | e2e_trace_runner、VM 四脚本、YCSB 脚本 + 指南；落地 `dsidle_e2e_08/09/ycsb` 与 rounds 脚本 | 4.8/4.9 验证过；三套 e2e 可跑通 1 轮冒烟；commit + 日志 |
| M8 对比与收尾 | §6.0 硬门槛（单测全绿 + 三套 e2e 各 ≥10 轮 + 补充四套各 ≥3 轮，按「施工执行协议」第 3 条循环至达标）、性能矩阵、**§7.4 基线差异校验与死代码/脚手架清扫**、文档 | §6.0 + §6.5 全绿 + §7.4 完成；commit + 日志 |

依赖严格线性（M5 依赖 M4；脚本骨架可自 M2 后并行起草）。禁止跨阶段堆未
验证且未提交的代码。

============================================================
九、风险与备选方案
============================================================

| 风险 | 应对 |
|------|------|
| HWCC 控制项间接访问抵消收益 | M3 设硬门槛；控制项预取、64B 独占布局；不达标先查伪共享与解析路径 |
| 偏移化遗漏隐藏指针 | §2.5 清单驱动分类型转换、不同基址测试、静态审计 |
| SWCC 可见性顺序错误（含分配器链） | §4.1/§4.4 固定顺序 + 跨进程 free/reuse 注入测试 |
| 值对象 UAF / 验版后延迟读 | §4.3.2 契约 + 负向测试；整 Get 单 epoch |
| 控制项复用污染直方图 | generation 绑定清零 access_count（§4.5） |
| layer 叶误当 value 复制 | §4.5 类型表 + 单测 |
| 半成品 NodeRef 入树 | 成对分配/回滚（§4.1/§4.3.4） |
| 副本陈旧读 / 目录 UAF | §4.3.2 命中后双检 + §4.5 seqlock；`dsidle_e2e_replica` |
| SWCC 叶链误用 CAS | §4.3.7 持锁写+flush；多进程链更新单测 |
| epoch 槽/每 op quiesce | §4.3.6 INACTIVE + 每 50 op；`dsidle_e2e_reclaim` |
| Scan 值 UAF | §4.3.5 整 Scan 同 epoch + 值契约 |
| 策略移植走样 | 对拍测试以基线 worktree 为准绳 |
| 模拟延迟未真实生效 | TSC 校准单测 + wall time 方向验证 |
| mkosi 镜像环境不可用 | 优先修本仓库 mkosi；仅用户授权下可从**显式本地路径**导入一次并记 SHA（禁止脚本默认指向 `../cxlkv/image/root.img`）；仍不可行则 BLOCKED |
| 重建 VM 影响其他实验 | 默认 `dsidle_check_vms.sh` 复用拓扑；重建须用户允许 |
