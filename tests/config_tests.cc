#include "dsidle/config.h"

#include <cassert>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

int main() {
  const auto config = dsidle::LoadExperimentConfig(dsidle::DefaultExperimentConfigPath());
  assert(config.shared_size_mb == 32768);
  assert(config.hwcc.offset_mb == 0 && config.hwcc.size_mb == 1024);
  assert(config.swcc.offset_mb == 1024 && config.swcc.size_mb == 31744);
  assert(config.vm_count == 4 && config.core_count_per_vm == 8);
  assert(config.hot_percentage_seed == 50);
  assert(config.fixed_key_size == 32 && config.fixed_value_size == 32);
  for (const char* path : {
           "tests/data/e2e09_4vm_config.jsonc",
           "tests/data/e2e_runner_2vm_config.jsonc",
           "tests/data/e2e_runner_4vm_config.jsonc",
           "tests/data/e2e_runner_config.jsonc",
           "tests/data/e2e_runner_replica_read_config.jsonc",
           "tests/data/e2e_runner_replica_seed_config.jsonc",
       })
    (void)dsidle::LoadExperimentConfig(path);

  const char* compatible = "/tmp/dsidle-compatible-config.jsonc";
  const std::string compatible_json = R"json({
    "shared_memory":{"size_mb":2048,"path":"/tmp","device_path":"/dev/null","numa_node":[0],"hwcc":{"offset_mb":0,"size_mb":1024},"swcc":{"offset_mb":1024,"size_mb":1024}},
    "vm":{"count":1,"core_count_per_vm":1,"mem_size_mb_per_vm":1,"storage_path":"/tmp","ssh_base_port":10022,"numa_node":[0],"copy_root_img":true,"use_ivshmem_doorbell":false,"local_ssh_pub_key":"","first_ip":"127.0.0.1","bridge_tap_ip":"127.0.0.1"},
    "host_cpu":{"reserved_cores":[0],"ivshmem_server_cores":[1],"vm_cores":[2]},
    "e2e":{"foreground_worker_count_per_vm":1},
    "network":{"sriov_nic":"ignored"},"sync":{"something_else":"ignored"},
    "dsidle":{"replica_budget_mb":1,"hot_percentage_seed":1,"fixed_key_size":8,"fixed_value_size":8,"trace_dir":"/tmp","verbose":false,"extra_check":false,"latency_inject":{"enabled":false,"foreground_enabled":true,"merge_enabled":true,"stats_enabled":false,"cache_line_bytes":64,"swcc_read_ns_per_line":0,"swcc_write_ns_per_line":0,"swcc_flush_ns_per_line":0,"hwcc_read_ns_per_line":0,"hwcc_write_ns_per_line":0,"hwcc_atomic_load_ns":0,"hwcc_atomic_store_ns":0,"hwcc_atomic_rmw_ns":0,"cache_model":"none","cache_hits_enabled":true,"cache_capacity_lines":1,"cache_associativity":1,"cache_fixed_hit_rate":0,"cache_hit_extra_ns":0}}
  })json";
  {
    std::ofstream output(compatible);
    output << compatible_json;
  }
  const auto parsed_compatible = dsidle::LoadExperimentConfig(compatible);
  assert(!parsed_compatible.verbose && !parsed_compatible.extra_check);
  assert(parsed_compatible.shared_path == "/tmp/ivshmem_shared_mem");

  std::string missing_mode_json = compatible_json;
  auto required_verbose = missing_mode_json.find("\"verbose\":false,");
  assert(required_verbose != std::string::npos);
  missing_mode_json.erase(required_verbose, std::string("\"verbose\":false,").size());
  {
    std::ofstream output(compatible);
    output << missing_mode_json;
  }
  bool missing_mode_rejected = false;
  try {
    (void)dsidle::LoadExperimentConfig(compatible);
  } catch (const std::runtime_error& error) {
    missing_mode_rejected =
        std::string(error.what()).find("missing key in dsidle: verbose") !=
        std::string::npos;
  }
  assert(missing_mode_rejected);

  std::string invalid_latency_json = compatible_json;
  auto enabled = invalid_latency_json.find("\"enabled\":false");
  auto verbose = invalid_latency_json.find("\"verbose\":false");
  assert(enabled != std::string::npos && verbose != std::string::npos);
  invalid_latency_json.replace(enabled, std::string("\"enabled\":false").size(),
                               "\"enabled\":true");
  invalid_latency_json.replace(verbose, std::string("\"verbose\":false").size(),
                               "\"verbose\":true");
  {
    std::ofstream output(compatible);
    output << invalid_latency_json;
  }
  bool invalid_latency_rejected = false;
  try {
    (void)dsidle::LoadExperimentConfig(compatible);
  } catch (const std::runtime_error& error) {
    invalid_latency_rejected =
        std::string(error.what()).find("verbose=false") != std::string::npos;
  }
  assert(invalid_latency_rejected);

  std::string build_type_latency_json = compatible_json;
  enabled = build_type_latency_json.find("\"enabled\":false");
  assert(enabled != std::string::npos);
  build_type_latency_json.replace(
      enabled, std::string("\"enabled\":false").size(), "\"enabled\":true");
  {
    std::ofstream output(compatible);
    output << build_type_latency_json;
  }
  bool build_type_latency_accepted = true;
  try {
    (void)dsidle::LoadExperimentConfig(compatible);
  } catch (const std::runtime_error& error) {
    build_type_latency_accepted = false;
    assert(std::string(error.what()).find("exact RelWithDebInfo") !=
           std::string::npos);
  }
  assert(build_type_latency_accepted ==
         (std::string(DSIDLE_CMAKE_BUILD_TYPE) == "RelWithDebInfo"));
  std::remove(compatible);

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
