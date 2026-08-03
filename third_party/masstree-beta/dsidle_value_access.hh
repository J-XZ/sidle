#pragma once

// D-SIDLE value rows are allocated from the canonical SWCC pool.  The
// Masstree value implementations use a few bulk operations that cannot be
// expressed as a typed load/store.  Keep those observations in this local
// adapter so the real memcpy/memset remains immediately adjacent to its
// accounting, and so the same rule applies to every supported row variant.

#include "dsidle/latency_simulator.h"
#include "dsidle/shared_pool.h"

#include <cstddef>

namespace dsidle_masstree {

inline bool IsObservedSwccRange(const void* address, std::size_t bytes) {
  const auto features = latency_sim::HardwareSimulationFeaturesFast();
  if (features == 0 ||
      !(features & (latency_sim::kFixedLatency |
                    latency_sim::kHwccAccessCount)) ||
      address == nullptr || bytes == 0)
    return false;
  const auto begin = reinterpret_cast<std::uintptr_t>(address);
  const auto end = begin + bytes;
  if (end < begin)
    return false;
  const auto* pool = dsidle::CurrentSharedPoolOrNull();
  const auto* base = dsidle::SharedPoolBaseOrNull();
  if (pool == nullptr || base == nullptr)
    return false;
  const auto* header = pool->header();
  const auto pool_begin = reinterpret_cast<std::uintptr_t>(
      static_cast<const std::byte*>(base) +
      header->swcc_offset);
  const auto pool_end = pool_begin + header->swcc_bytes;
  return begin >= pool_begin && end <= pool_end;
}

template <typename T>
inline T LoadSwcc(const T* address) {
  const T result = *address;
  if (IsObservedSwccRange(address, sizeof(T)))
    latency_sim::GlobalLatencySimulator().RecordRange(
        latency_sim::PoolKind::kSwcc, latency_sim::AccessKind::kRead, address,
        sizeof(T));
  return result;
}

template <typename T>
inline void StoreSwcc(T* address, T value) {
  *address = value;
  if (IsObservedSwccRange(address, sizeof(T)))
    latency_sim::GlobalLatencySimulator().RecordRange(
        latency_sim::PoolKind::kSwcc, latency_sim::AccessKind::kWrite, address,
        sizeof(T));
}

inline void ReadSwcc(const void* address, std::size_t bytes) {
  if (!IsObservedSwccRange(address, bytes))
    return;
  latency_sim::GlobalLatencySimulator().RecordRange(
      latency_sim::PoolKind::kSwcc, latency_sim::AccessKind::kRead, address,
      bytes);
}

inline void WriteSwcc(const void* address, std::size_t bytes) {
  if (!IsObservedSwccRange(address, bytes))
    return;
  latency_sim::GlobalLatencySimulator().RecordRange(
      latency_sim::PoolKind::kSwcc, latency_sim::AccessKind::kWrite, address,
      bytes);
}

}  // namespace dsidle_masstree
