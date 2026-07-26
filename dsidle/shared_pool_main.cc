#include "dsidle/config.h"
#include "dsidle/shared_pool.h"
#include "dsidle/shard_allocator.h"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
std::uint64_t ParsePositive(const char* value, const char* name) {
  try {
    const auto parsed = std::stoull(value);
    if (parsed) return parsed;
  } catch (const std::exception&) {
  }
  throw std::runtime_error(std::string("invalid ") + name);
}
}  // namespace

int main(int argc, char** argv) {
  try {
    std::string config_path = dsidle::DefaultExperimentConfigPath();
    bool init_pool = false;
    std::uint64_t node_capacity = 2'097'152;
    std::uint32_t max_threads = 16;
    for (int index = 1; index < argc; ++index) {
      const std::string argument = argv[index];
      if (argument == "--init-pool") init_pool = true;
      else if (argument == "--config" && index + 1 < argc) config_path = argv[++index];
      else if (argument == "--node-control-capacity" && index + 1 < argc)
        node_capacity = ParsePositive(argv[++index], "node-control capacity");
      else if (argument == "--max-threads-per-vm" && index + 1 < argc)
        max_threads = static_cast<std::uint32_t>(ParsePositive(argv[++index], "max threads per VM"));
      else throw std::runtime_error("usage: dsidle_shared_pool --init-pool [--config PATH] [--node-control-capacity N] [--max-threads-per-vm N]");
    }
    if (!init_pool) throw std::runtime_error("--init-pool is required");
    const auto config = dsidle::LoadExperimentConfig(config_path);
    const dsidle::PoolLayout layout{config.shared_size_mb << 20, config.hwcc.offset_mb << 20,
                                    config.hwcc.size_mb << 20, config.swcc.offset_mb << 20,
                                    config.swcc.size_mb << 20};
    auto pool = dsidle::SharedPool::InitializeExisting(config.shared_path, layout);
    dsidle::InitializePoolMetadata(pool, {config.vm_count, max_threads, node_capacity});
    dsidle::FixedBlockShardAllocator::InitializeAll(pool, config.vm_count);
    dsidle::FinalizePoolInitialization(pool);
    std::cout << dsidle::DescribeHwccBudget(pool) << '\n';
  } catch (const std::exception& error) {
    std::cerr << "dsidle_shared_pool: " << error.what() << '\n';
    return 2;
  }
}
