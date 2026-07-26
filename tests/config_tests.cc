#include "dsidle/config.h"

#include <cassert>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <stdexcept>

int main() {
  const auto config = dsidle::LoadExperimentConfig(dsidle::DefaultExperimentConfigPath());
  assert(config.shared_size_mb == 32768);
  assert(config.hwcc.offset_mb == 0 && config.hwcc.size_mb == 1024);
  assert(config.swcc.offset_mb == 1024 && config.swcc.size_mb == 31744);
  assert(config.vm_count == 4 && config.core_count_per_vm == 8);
  assert(config.hot_percentage_seed == 50);
  assert(config.fixed_key_size == 32 && config.fixed_value_size == 32);
  const char* invalid = "/tmp/dsidle-invalid-config.jsonc";
  std::ofstream output(invalid);
  output << R"json({
    "shared_memory":{"size_mb":2048,"path":"/tmp/pool","device_path":"/dev/null","numa_node":[0],"hwcc":{"offset_mb":0,"size_mb":1024},"swcc":{"offset_mb":1024,"size_mb":1024}},
    "vm":{"count":4,"core_count_per_vm":1,"mem_size_mb_per_vm":1,"storage_path":"/tmp","ssh_base_port":10022,"numa_node":[0],"local_ssh_pub_key":"","first_ip":"127.0.0.1","bridge_tap_ip":"127.0.0.1"},
    "host_cpu":{"reserved_cores":[0],"ivshmem_server_cores":[1],"vm_cores":[2,3,4,5]},
    "e2e":{"foreground_worker_count_per_vm":1},
    "dsidle":{"replica_budget_mb":1,"hot_percentage_seed":1,"fixed_key_size":8,"fixed_value_size":8,"trace_dir":"/tmp","latency_inject":{"enabled":false,"foreground_enabled":true,"merge_enabled":true,"stats_enabled":false,"cache_line_bytes":64,"swcc_read_ns_per_line":0,"swcc_write_ns_per_line":0,"swcc_flush_ns_per_line":0,"hwcc_read_ns_per_line":0,"hwcc_write_ns_per_line":0,"hwcc_atomic_load_ns":0,"hwcc_atomic_store_ns":0,"hwcc_atomic_rmw_ns":0,"cache_model":"none","cache_hits_enabled":true,"cache_capacity_lines":1,"cache_associativity":1,"cache_fixed_hit_rate":0,"cache_hit_extra_ns":0}},
    "unexpected":true
  })json";
  output.close();
  bool rejected = false;
  try { (void)dsidle::LoadExperimentConfig(invalid); }
  catch (const std::runtime_error&) { rejected = true; }
  std::remove(invalid);
  assert(rejected);
  std::cout << "D-SIDLE config contract OK\n";
}
