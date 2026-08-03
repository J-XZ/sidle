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
    "dsidle":{"replica_budget_mb":1,"hot_percentage_seed":1,"fixed_key_size":8,"fixed_value_size":8,"trace_dir":"/tmp","verbose":false,"extra_check":false,"latency_inject":{
      "fixed_latency":{"enabled":false,"cache_line_bytes":64,"swcc_fixed_ns_per_line":0,"hwcc_fixed_ns_per_line":0,"foreground_enabled":true,"background_enabled":true}
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
    if (fragment != nullptr)
      assert(std::string(error.what()).find(fragment) != std::string::npos);
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
  assert(!config.latency_inject.fixed_latency.enabled);
  assert(config.latency_inject.fixed_latency.cache_line_bytes == 64);

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
  const std::string compatible_json = CanonicalConfigJson();
  Write(compatible, compatible_json);
  const auto parsed = dsidle::LoadExperimentConfig(compatible);
  assert(!parsed.verbose && !parsed.extra_check);
  assert(parsed.shared_path == "/tmp/ivshmem_shared_mem");

  std::string exponent_json = compatible_json;
  const auto zero_delay =
      exponent_json.find("\"swcc_fixed_ns_per_line\":0");
  assert(zero_delay != std::string::npos);
  exponent_json.replace(zero_delay,
                        std::string("\"swcc_fixed_ns_per_line\":0").size(),
                        "\"swcc_fixed_ns_per_line\":1e2");
  Write(compatible, exponent_json);
  assert(dsidle::LoadExperimentConfig(compatible)
             .latency_inject.fixed_latency.swcc_fixed_ns_per_line == 100.0);

  std::string negative_json = compatible_json;
  const auto negative_delay =
      negative_json.find("\"hwcc_fixed_ns_per_line\":0");
  assert(negative_delay != std::string::npos);
  negative_json.replace(
      negative_delay,
      std::string("\"hwcc_fixed_ns_per_line\":0").size(),
      "\"hwcc_fixed_ns_per_line\":-1");
  Write(compatible, negative_json);
  ExpectRejected([&] { (void)dsidle::LoadExperimentConfig(compatible); },
                 "fixed latency");

  std::string delayed_stats_json = compatible_json;
  const auto fixed_close = delayed_stats_json.find(
      "\"background_enabled\":true}");
  assert(fixed_close != std::string::npos);
  delayed_stats_json.replace(
      fixed_close + std::string("\"background_enabled\":true").size(), 0,
      ",\"delayed_time_stats_enabled\":false");
  Write(compatible, delayed_stats_json);
  ExpectRejected([&] { (void)dsidle::LoadExperimentConfig(compatible); },
                 "unknown key");

  std::string old_section_json = compatible_json;
  const auto latency_close = old_section_json.rfind("}}");
  assert(latency_close != std::string::npos);
  old_section_json.replace(latency_close, 0,
      ",\"hwcc_access_count\":{\"enabled\":false}");
  Write(compatible, old_section_json);
  ExpectRejected([&] { (void)dsidle::LoadExperimentConfig(compatible); },
                 "unknown key");

  std::string duplicate_json = compatible_json;
  duplicate_json.replace(
      fixed_close + std::string("\"background_enabled\":true").size(), 0,
      ",\"enabled\":false");
  Write(compatible, duplicate_json);
  ExpectRejected([&] { (void)dsidle::LoadExperimentConfig(compatible); },
                 "duplicate key");

  std::string missing_json = compatible_json;
  const auto fixed_marker = missing_json.find("\"fixed_latency\":");
  assert(fixed_marker != std::string::npos);
  missing_json.replace(fixed_marker,
                       std::string("\"fixed_latency\":").size(),
                       "\"old_latency\":");
  Write(compatible, missing_json);
  ExpectRejected([&] { (void)dsidle::LoadExperimentConfig(compatible); },
                 "unknown key");

  std::string enabled_json = compatible_json;
  const auto enabled_marker =
      enabled_json.find("\"fixed_latency\":{\"enabled\":false");
  assert(enabled_marker != std::string::npos);
  enabled_json.replace(
      enabled_marker,
      std::string("\"fixed_latency\":{\"enabled\":false").size(),
      "\"fixed_latency\":{\"enabled\":true");
  Write(compatible, enabled_json);
  if (std::string(DSIDLE_CMAKE_BUILD_TYPE) == "RelWithDebInfo")
    (void)dsidle::LoadExperimentConfig(compatible);
  else
    ExpectRejected([&] { (void)dsidle::LoadExperimentConfig(compatible); },
                   "exact RelWithDebInfo");

  std::remove(compatible);
  std::cout << "D-SIDLE fixed-latency config contract OK\n";
}
