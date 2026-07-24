#include "dsidle/config.h"

#include <cassert>
#include <iostream>

int main() {
  const auto config = dsidle::LoadExperimentConfig(dsidle::DefaultExperimentConfigPath());
  assert(config.shared_size_mb == 32768);
  assert(config.hwcc.offset_mb == 0 && config.hwcc.size_mb == 1024);
  assert(config.swcc.offset_mb == 1024 && config.swcc.size_mb == 31744);
  assert(config.vm_count == 4 && config.core_count_per_vm == 8);
  assert(config.fixed_key_size == 32 && config.fixed_value_size == 32);
  std::cout << "D-SIDLE config contract OK\n";
}
