# SIDLE: Tree-structure Aware Indexes for CXL-based Heterogeneous Memory

> D-SIDLE status: the upstream single-node `benchmark/`, `src/kv/`, and
> `src/helper.*` facade below is retained only as architecture reference. It is
> intentionally not built because it still assumes the retired allocator,
> fixed host-core map, and pre-distributed constructor contracts. D-SIDLE/cxlkv
> comparisons must use the root CMake targets `dsidle_e2e_trace_runner` and
> `dsidle_e2e_suite_runner`. The remaining upstream instructions are historical
> and do not describe the current distributed experiment path.

<!-- > This repository is fully anonymized.  -->
This repository contains the implementation of SIDLE. The baselines can be found in the [sidle-baselines](https://github.com/sidle-project/sidle-baselines) repo.

## Part1. Environment Setup
<!-- > The following instructions are for Fedora 38. For other Linux distributions, please refer to the official installation guide. -->
>  NOTE: We only test the building procedure on Fedora 38 & Ubuntu 22.04 LTS with Linux 6.2.x.
### 1.0 OS Version
- Linux 6.2.x

### 1.1 Install dependencies
- gcc v13.2.1
- cmake v3.27.7
- jemalloc v5.2.1
- memkind v1.14.0
- daxctl 78

### 1.2 Setup CXL device
Using the following command to check the information of the CXL device in the system.
```shell
daxctl list
# example output: 
# [
#   {
#     "chardev":"dax0.0",
#     "size":137438953472,
#     "target_node":2,
#     "align":2097152,
#     "mode":"devdax"
#   }
# ]
```
1. Make sure the `mode` of the CXL device is `devdax`.
2. Replace the value of `path` defined in `third_party/cxl_utils/cxl_allocator.c` with the `chardev` of the CXL device. (default: `dax0.0`)
3. Replace the value of `CXL_MAX_SIZE` defined in `third_party/cxl_utils/cxl_allocator.h` with the `size` of the CXL device.
4. Replace the value of `CXL_MIN_SIZE` defined in `third_party/cxl_utils/cxl_allocator.c` with the `align` of the CXL device.

Change the permission of the CXL device to ensure the program can acccess it.
```shell
sudo chmod 777 /dev/[cxl_path]
```
### 1.3 Setting up core bindings
Currently, we hardcoded the binding from thread id used in the implementation to the CPU id.
To avoid performance issues, please modify `bind_to_physical_cores` in `src/helper.cpp` accordingly.

Specifically, change `const int physical_core_n` and `int mapping[physical_core_n]` according to your machine.
The physical CPU id can be found by running lscpu in the terminal.
In our machine, we see the following output:
```shell
...
NUMA node0 CPU(s):     0-47
NUMA node1 CPU(s):     48-95
...
```
### 1.4 Build binaries
Use the following command to clone the source code of SIDLE and build:
```
git clone https://github.com/sidle-project/sidle.git
cd sidle
mkdir build
cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j 24
```
Now, all the runnable benchmarks are in the `build/` folder.

### 1.5 Verify binaries
To make sure the binary is runnable, run the following steps that instantiate a S-ART instance with a small dataset, and run a read-write workload for 10s.
```shell
./rw_ycsb_origin \
    -a 0.5 \
    -b 0.5 \
    --target art \
    --runtime 10 \
    --warmup 0 \
    --fg 2 \
    --bg 0 \
    --table-size 200000 \
    --cxl-percentage 80 \
    --max-local-memory-usage 110
```
You should be able to see the following output:
```shell
[Overall] MACRO: rw_ycsb origin
Read:Insert:Delete:Scan:RMW:Inplace = 0.5:0.5:0:0:0:0
target: art
...
```
### 1.6 Prepare dataset
We have provided the compressed file of the dataset required for the real-world workload evaluation. 
Run the following commands to extract it.
```shell
cd sidle/
find block_trace -name "*.tar.gz" -name '*.tar.gz' -execdir tar -xzf {} \;
```
## Part2. Step-by-step instructions
### 2.1 Important notes
All results in the paper are obtained on dual Intel® Xeon® Platinum 8468V CPUs, equipped with 395 MiB CPU cache, 32 GiB Local DRAM, and 32 GiB CXL memory (connected via a CXL memory expansion card).

Most of our experiments will use 34 **physical** cores. Using fewer cores or hyper-threads might produce different performance results.
If you set a smaller number of cores in Part 1.3, errors might appear.


### 2.2 Directory structure
```shell
├── block_trace                             # Alibaba trace dataset 
├── benchmark                               # Benchmark source code
│   ├── CMakeLists.txt
│   ├── real_world                          # Real-world workload benchmarks
│   └── synthetic                           # Micro and macro benchmarks
├── CMakeLists.txt                          
├── src
│   ├── helper.cpp
│   ├── helper.h
│   └── kv                                  # The interface of S-ART and S-Masstree
└── third_party
    ├── art                                 # ART source code and integration with SIDLE
    ├── cxl_utils                           # CXL memory allocator (Unimalloc) 
    ├── masstree-beta                       # Masstree source code and integration with SIDLE
    └── sidle_utils                         # SIDLE framework source code
```

### 2.3 Benchmark Commands
#### 2.3.1 Microbenchmark
Execute the following commands to conduct a microbenchmark evaluation:
```shell
cd build
# Update Heavy / Read Mostly / With Insert 
./skewed_partition \
    -a $read_ratio \
    -b $write_ratio \
    --target $target \
    --runtime $runtime \
    --warmup $warmup \
    --fg $fgn \
    --bg 0 \
    --table-size $size --cxl-percentage $cxl_percentage \
    --hot-data-ratio $hot_data_ratio --hot-query-ratio $hot_query_ratio \
    --hot-data-start $hot_data_start --max-local-memory-usage $local_memory_usage
# With Insert 
./skewed_partition_dynamic \
    -a $read_ratio \
    -b $write_ratio \
    --target $target \
    --runtime $target \
    --warmup $warmup \
    --fg $fgn \
    --bg 0 \
    --table-size $size --cxl-percentage $cxl_percentage \
    --hot-data-ratio $hot_data_ratio --hot-query-ratio $hot_query_ratio \
    --hot-data-start $hot_data_start --max-local-memory-usage $local_memory_usage
```
- `$read_ratio`: The read ratio of the workload, ranging from 0 to 1.
- `$write_ratio`: The write ratio of the workload, ranging from 0 to 1.
- `$target`: The target data structure, either `art` or `masstree`.
- `$runtime`: The total runtime of the workload in seconds.
- `$warmup`: The warmup time of the workload in seconds, the default value is 0.
- `$fgn`: The number of foreground threads.
- `$size`: The number of keys loaded during the initialization.
- `$cxl_percentage`: The percentage of the CXL memory used in the workload.
- `$hot_data_ratio`: The ratio of hot data in the workload, ranging from 0 to 100, the default value is 5.
- `$hot_query_ratio`: The ratio of hot queries in the workload, ranging from 0 to 100, the default value is 90.
- `$hot_data_start`: The starting position of the hot region in the key space, the default value is 5.
- `$local_memory_usage`: The maximum local DRAM usage of the workload in MiB.

#### 2.3.2 Macrobenchmark
Execute the following commands to conduct a macrobenchmark evaluation:
```shell
# YCSB A/B/C
./rw_ycsb_origin \
    -a $read_ratio \
    -b $write_ratio \
    --target $target \
    --runtime $runtime \
    --warmup $warmup \
    --fg $fgn \
    --bg 0 \
    --table-size $size --cxl-percentage $cxl_percentage \
    --max-local-memory-usage $local_memory_usage
# YCSB D
./read_latest_origin \
    --target $target \
    --runtime $runtime \
    --warmup $warmup \
    --fg $fgn \
    --bg 0 \
    --table-size $size --cxl-percentage $cxl_percentage \
    --max-local-memory-usage $local_memory_usage
# YCSB E
./short_range_origin \
    --target $target \
    --runtime $runtime \
    --warmup $warmup \
    --fg $fgn \
    --bg 0 \
    --table-size $size --cxl-percentage $cxl_percentage \
    --max-local-memory-usage
# YCSB F
./read_modify_write_origin \
    --target $target \
    --runtime $runtime \
    --warmup $warmup \
    --fg $fgn \
    --bg 0 \
    --table-size $size --cxl-percentage $cxl_percentage \
    --max-local-memory-usage $local_memory_usage
```
- `$read_ratio`: The read ratio of the workload, ranging from 0 to 1.
- `$write_ratio`: The write ratio of the workload, ranging from 0 to 1.
- `$target`: The target data structure, either `art` or `masstree`.
- `$runtime`: The total runtime of the workload in seconds.
- `$warmup`: The warmup time of the workload in seconds; the default value is 0.
- `$fgn`: The number of foreground threads.
- `$size`: The number of keys loaded during the initialization.
- `$cxl_percentage`: The percentage of the CXL memory used in the workload.
- `$local_memory_usage`: The maximum local DRAM usage of the workload in MiB.

#### 2.3.3 TPC-C benchmark
Execute the following commands to conduct a TPC-C benchmark evaluation:
```shell
cd build
./tpcc \
    --warehouse $warehouse \
    --runtime $runtime \
    --warmup $warmup \
    --fg $fgn \
    --bg 0 \
    -n 1 \
    -o $cxl_percentage
```
- `$warehouse`: The number of warehouses in the TPC-C benchmark.
- `$runtime`: The total runtime of the workload in seconds.
- `$warmup`: The warmup time of the workload in seconds, the default value is 0.
- `$fgn`: The number of foreground threads.
- `$cxl_percentage`: The percentage of the CXL memory used in the workload.
- NOTE: By default, the TPCC benchmark targets Masstree. To benchmark ART, please comment out lines 30-31 and uncomment lines 32-33 in `benchmark/real_world/tpcc_loader.h`.

## User guide for adapting existing tree-structure indexes

The SIDLE framework requires developers to adapt their tree-structure indexes in three steps: 
1. adding SIDLE metadata to the original tree nodes;
2. calling SIDLE's frontend helper functions when allocating new nodes and accessing leaf nodes;
3. implementing the interfaces related to the tree-specific logic for the background module.

We present the details of this framework in [guide.pdf](./guide.pdf). Additionally, we use a B+ tree as an example in this documentation to show how to integrate SIDLE with existing tree-structure indexes.

## Unimalloc
We develop *Unimalloc* (the source code is under `third_party/cxl_utils/`), a unified memory allocator based on [memkind](https://github.com/memkind/memkind), to support both ratio-based and specific-type memory allocation across CXL and local DRAM.
Unimalloc offers equivalent replacements for commonly used libc memory allocation interfaces.
It provides two types of interfaces: one for allocating memory between CXL and local DRAM in a specified ratio, and another for allocating memory specifically to CXL.
Please check the [Unimalloc Interface](./third_party/cxl_utils/cxl_allocator.h) for more details.
