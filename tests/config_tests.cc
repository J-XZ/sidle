#include "dsidle/config.h"

#include <cassert>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

std::string CanonicalConfigJson() {
  return R"json({
    "shared_memory":{"size_mb":2048,"path":"/tmp","device_path":"/dev/null","numa_node":[0],"hwcc":{"offset_mb":0,"size_mb":1024},"swcc":{"offset_mb":1024,"size_mb":1024}},
    "vm":{"count":1,"core_count_per_vm":1,"mem_size_mb_per_vm":1,"storage_path":"/tmp","ssh_base_port":10022,"numa_node":[0],"copy_root_img":true,"use_ivshmem_doorbell":false,"local_ssh_pub_key":"","first_ip":"127.0.0.1","bridge_tap_ip":"127.0.0.1"},
    "host_cpu":{"reserved_cores":[0],"ivshmem_server_cores":[1],"vm_cores":[2]},
    "e2e":{"foreground_worker_count_per_vm":1},
    "network":{"sriov_nic":"ignored"},"sync":{"something_else":"ignored"},
    "dsidle":{"replica_budget_mb":1,"hot_percentage_seed":1,"fixed_key_size":8,"fixed_value_size":8,"trace_dir":"/tmp","verbose":false,"extra_check":false,"latency_inject":{
      "fixed_latency":{"enabled":false,"cache_line_bytes":64,"swcc_fixed_ns_per_line":0,"hwcc_fixed_ns_per_line":0,"foreground_enabled":true,"background_enabled":true,"delayed_time_stats_enabled":false},
      "hwcc_access_count":{"enabled":false,"cache_line_bytes":64,"read_enabled":true,"write_enabled":true,"operation_count_enabled":true,"line_count_enabled":true,"byte_count_enabled":false,"breakdown_by_scope_enabled":false,"breakdown_by_tag_enabled":false,"max_tags":32},
      "atomic_count":{"enabled":false,"hwcc_enabled":true,"owner_private_swcc_enabled":false,"local_dram_enabled":true,"load_enabled":true,"store_enabled":true,"cas_enabled":true,"exchange_enabled":true,"fetch_arithmetic_enabled":true,"fetch_bitwise_enabled":true,"result_breakdown_enabled":true,"fence_enabled":false,"wait_notify_enabled":false,"memory_order_breakdown_enabled":false,"scope_breakdown_enabled":false,"tag_breakdown_enabled":false,"max_tags":32},
      "remote_cache_invalidation":{"enabled":false,"dirty_handoff_enabled":true,"clean_copy_invalidation_enabled":true,"dirty_eviction_writeback_enabled":true,"swcc_explicit_visibility_handoff_enabled":true,"cache_line_bytes":64,"node_count":1,"cache_size_bytes_per_node":41943040,"total_cpu_cache_size_bytes":0,"cache_size_bytes_by_node":[],"cache_instances_per_node":1,"associativity":16,"capacity_mode":"per_node","replacement_policy":"lru","lfu_counter_bits":16,"lfu_aging_interval_accesses":1048576,"lfu_tie_breaker":"lru","scope_breakdown_enabled":false,"tag_breakdown_enabled":false,"max_tags":32,"shared_sequencer_offset":192,"event_log_capacity":4096}
    }}
  })json";
}

void Write(const char* path, const std::string& json) {
  std::ofstream output(path);
  assert(output);
  output << json;
}

template <typename Function>
void ExpectRejected(Function&& function, const char* fragment = nullptr) {
  bool rejected = false;
  try {
    function();
  } catch (const std::runtime_error& error) {
    rejected = true;
    if (fragment != nullptr) {
      assert(std::string(error.what()).find(fragment) != std::string::npos);
    }
  }
  assert(rejected);
}

}  // namespace

int main() {
  const auto config = dsidle::LoadExperimentConfig(
      dsidle::DefaultExperimentConfigPath());
  assert(config.shared_size_mb == 32768);
  assert(config.hwcc.offset_mb == 0 && config.hwcc.size_mb == 1024);
  assert(config.swcc.offset_mb == 1024 && config.swcc.size_mb == 31744);
  assert(config.vm_count == 4 && config.core_count_per_vm == 8);
  assert(config.hot_percentage_seed == 50);
  assert(config.fixed_key_size == 32 && config.fixed_value_size == 32);
  assert(!config.latency_inject.AnyModuleEnabled());
  assert(config.latency_inject.remote_cache_invalidation.node_count == 4);
  for (const char* path : {
           "tests/data/e2e09_4vm_config.jsonc",
           "tests/data/e2e_runner_2vm_config.jsonc",
           "tests/data/e2e_runner_4vm_config.jsonc",
           "tests/data/e2e_runner_config.jsonc",
           "tests/data/e2e_runner_replica_read_config.jsonc",
           "tests/data/e2e_runner_replica_seed_config.jsonc",
       })
    (void)dsidle::LoadExperimentConfig(path);
  if (std::string(DSIDLE_CMAKE_BUILD_TYPE) == "RelWithDebInfo")
    (void)dsidle::LoadExperimentConfig(
        "configs/latency/cxlkv_e2e11_reference.jsonc");

  const char* compatible = "/tmp/dsidle-compatible-config.jsonc";
  const std::string compatible_json = CanonicalConfigJson();
  Write(compatible, compatible_json);
  const auto parsed_compatible = dsidle::LoadExperimentConfig(compatible);
  assert(!parsed_compatible.verbose && !parsed_compatible.extra_check);
  assert(parsed_compatible.shared_path == "/tmp/ivshmem_shared_mem");
  assert(parsed_compatible.latency_inject.remote_cache_invalidation
             .event_log_capacity == 4096);

  std::string exponent_latency_json = compatible_json;
  const auto zero_delay =
      exponent_latency_json.find("\"swcc_fixed_ns_per_line\":0");
  assert(zero_delay != std::string::npos);
  exponent_latency_json.replace(
      zero_delay, std::string("\"swcc_fixed_ns_per_line\":0").size(),
      "\"swcc_fixed_ns_per_line\":1e2");
  Write(compatible, exponent_latency_json);
  assert(dsidle::LoadExperimentConfig(compatible)
             .latency_inject.fixed_latency.swcc_fixed_ns_per_line == 100.0);

  std::string negative_latency_json = compatible_json;
  const auto negative_delay =
      negative_latency_json.find("\"hwcc_fixed_ns_per_line\":0");
  assert(negative_delay != std::string::npos);
  negative_latency_json.replace(
      negative_delay,
      std::string("\"hwcc_fixed_ns_per_line\":0").size(),
      "\"hwcc_fixed_ns_per_line\":-1");
  Write(compatible, negative_latency_json);
  ExpectRejected([&] { (void)dsidle::LoadExperimentConfig(compatible); },
                 "fixed latency");

  std::string old_model_json = compatible_json;
  const auto fixed_end = old_model_json.find(
      "\"delayed_time_stats_enabled\":false}");
  assert(fixed_end != std::string::npos);
  old_model_json.replace(fixed_end + std::string(
                                       "\"delayed_time_stats_enabled\":false")
                                   .size(),
                         0, ",\"cache_model\":\"none\"");
  Write(compatible, old_model_json);
  ExpectRejected([&] { (void)dsidle::LoadExperimentConfig(compatible); },
                 "unknown key");

  std::string duplicate_key_json = compatible_json;
  const auto fixed_close = duplicate_key_json.find(
      "\"delayed_time_stats_enabled\":false}");
  assert(fixed_close != std::string::npos);
  duplicate_key_json.replace(
      fixed_close + std::string("\"delayed_time_stats_enabled\":false")
                            .size(),
      0, ",\"enabled\":false");
  Write(compatible, duplicate_key_json);
  ExpectRejected([&] { (void)dsidle::LoadExperimentConfig(compatible); },
                 "duplicate key");

  std::string remote_capacity_json = compatible_json;
  const auto remote_open = remote_capacity_json.find(
      "\"remote_cache_invalidation\":{\"enabled\":false");
  assert(remote_open != std::string::npos);
  remote_capacity_json.replace(remote_open,
                               std::string("\"remote_cache_invalidation\":{\"enabled\":false")
                                   .size(),
                               "\"remote_cache_invalidation\":{\"enabled\":true");
  const auto capacity = remote_capacity_json.find(
      "\"cache_size_bytes_per_node\":41943040");
  assert(capacity != std::string::npos);
  remote_capacity_json.replace(
      capacity, std::string("\"cache_size_bytes_per_node\":41943040").size(),
      "\"cache_size_bytes_per_node\":0");
  Write(compatible, remote_capacity_json);
  if (std::string(DSIDLE_CMAKE_BUILD_TYPE) == "RelWithDebInfo") {
    ExpectRejected([&] { (void)dsidle::LoadExperimentConfig(compatible); },
                   "remote cache capacity");
  } else {
    ExpectRejected([&] { (void)dsidle::LoadExperimentConfig(compatible); },
                   "exact RelWithDebInfo");
  }

  std::string missing_module_json = compatible_json;
  const auto latency_open = missing_module_json.find("\"latency_inject\":{");
  assert(latency_open != std::string::npos);
  missing_module_json.replace(latency_open, std::string("\"latency_inject\":{").size(),
                              "\"latency_inject\":{}");
  const auto remainder = missing_module_json.find(
      "\"fixed_latency\":", latency_open + 18);
  (void)remainder;
  // Replacing the object opener leaves the old body after the new empty
  // object; use a deliberately minimal complete root instead for this case.
  missing_module_json = compatible_json;
  const auto fixed_marker = missing_module_json.find("\"fixed_latency\":");
  assert(fixed_marker != std::string::npos);
  missing_module_json.replace(fixed_marker, std::string("\"fixed_latency\":").size(),
                              "\"unknown_module\":");
  Write(compatible, missing_module_json);
  ExpectRejected([&] { (void)dsidle::LoadExperimentConfig(compatible); },
                 "missing section");

  std::string enabled_json = compatible_json;
  const auto enabled_marker = enabled_json.find("\"fixed_latency\":{\"enabled\":false");
  assert(enabled_marker != std::string::npos);
  enabled_json.replace(enabled_marker,
                       std::string("\"fixed_latency\":{\"enabled\":false").size(),
                       "\"fixed_latency\":{\"enabled\":true");
  Write(compatible, enabled_json);
  bool accepted = true;
  try {
    (void)dsidle::LoadExperimentConfig(compatible);
  } catch (const std::runtime_error& error) {
    accepted = false;
    assert(std::string(error.what()).find("exact RelWithDebInfo") !=
           std::string::npos);
  }
  assert(accepted == (std::string(DSIDLE_CMAKE_BUILD_TYPE) == "RelWithDebInfo"));

  std::remove(compatible);

  const char* invalid = "/tmp/dsidle-invalid-config.jsonc";
  auto invalid_json = CanonicalConfigJson();
  const auto root_close = invalid_json.rfind("}");
  assert(root_close != std::string::npos);
  invalid_json.insert(root_close, ",\"unexpected\":true");
  Write(invalid, invalid_json);
  ExpectRejected([&] { (void)dsidle::LoadExperimentConfig(invalid); },
                 "unknown key in root");
  std::remove(invalid);
  std::cout << "D-SIDLE config contract OK\n";
}
