#pragma once

// D-SIDLE value rows are allocated from the canonical SWCC pool.  The
// Masstree value implementations use a few bulk operations that cannot be
// expressed as a typed load/store.  Keep those observations in this local
// adapter so the real memcpy/memset remains immediately adjacent to its
// accounting, and so the same rule applies to every supported row variant.
//
// Classification reads only the immutable thread-local SWCC range snapshot
// published by dsidle::ConfigureCurrentSwccAllocator's explicit binding
// stage.  It never dereferences the pool header or static layout: those are
// HWCC reads that would otherwise add hidden shared-memory traffic on every
// value operation and risk recursive/repeated charging at the classification
// entry itself.

#include "dsidle/latency_simulator.h"
#include "dsidle/shared_pool.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace dsidle_masstree {

inline bool IsObservedSwccRange(const void* address, std::size_t bytes) {
  // Disabled fast path: no TLS, no address arithmetic, no classification.
  if (!latency_sim::FixedLatencyEnabledFast()) [[likely]] return false;
  if (address == nullptr || bytes == 0)
    return false;
  const auto begin = reinterpret_cast<std::uintptr_t>(address);
  if (bytes > std::numeric_limits<std::uintptr_t>::max() - begin)
    throw std::runtime_error(
        "D-SIDLE SWCC classifier address range overflows uintptr_t");
  const auto end = begin + bytes;
  // Enabled path: the classifier requires a valid thread-local binding and
  // hard fails otherwise instead of silently treating a potential SWCC
  // access as local DRAM.
  const auto& range = dsidle::CurrentSwccRangeCache();
  if (begin >= range.swcc_begin && end <= range.swcc_end)
    return true;  // fully inside the SWCC mapping
  if (end <= range.swcc_begin || begin >= range.swcc_end)
    return false;  // fully outside (process-local DRAM)
  throw std::runtime_error(
      "D-SIDLE SWCC classifier range partially crosses the SWCC mapping "
      "boundary; refusing to classify a straddling access as local DRAM");
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
