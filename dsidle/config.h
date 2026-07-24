#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "dsidle/latency_simulator.h"

namespace dsidle {

struct MemoryRegionConfig {
  std::uint64_t offset_mb{};
  std::uint64_t size_mb{};
};

struct ExperimentConfig {
  std::uint64_t shared_size_mb{};
  std::string shared_path;
  std::string device_path;
  std::vector<int> shared_numa_nodes;
  MemoryRegionConfig hwcc;
  MemoryRegionConfig swcc;
  std::uint32_t vm_count{};
  std::uint32_t core_count_per_vm{};
  std::uint32_t foreground_worker_count_per_vm{};
  std::uint64_t replica_budget_mb{};
  std::uint32_t fixed_key_size{};
  std::uint32_t fixed_value_size{};
  std::string trace_dir;
  latency_sim::Config latency_inject{};
};

// dsidle: The parser owns the experiment contract.  Unknown dsidle fields and
// malformed or incomplete configurations are errors rather than fallbacks.
ExperimentConfig LoadExperimentConfig(const std::string& path);
std::string DefaultExperimentConfigPath();

}  // namespace dsidle
