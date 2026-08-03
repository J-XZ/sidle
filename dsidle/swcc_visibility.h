#pragma once

#include <cstddef>
#include <cstdint>
#include <immintrin.h>
#include "dsidle/latency_simulator.h"

namespace dsidle {

constexpr std::size_t kSwccCacheLineBytes = 64;

inline void FlushSwccRange(const void* address, std::size_t bytes) {
  const auto begin = reinterpret_cast<std::uintptr_t>(address) & ~(kSwccCacheLineBytes - 1);
  const auto end = reinterpret_cast<std::uintptr_t>(address) + bytes;
  for (auto line = begin; line < end; line += kSwccCacheLineBytes)
    _mm_clflush(reinterpret_cast<const void*>(line));
  _mm_sfence();
  // The flush is a real SWCC access. Charge its covered lines after the
  // protocol instruction completes; ordinary stores are charged separately.
  latency_sim::GlobalLatencySimulator().RecordRange(
      latency_sim::PoolKind::kSwcc, latency_sim::AccessKind::kFlush, address,
      bytes);
}

inline void InvalidateSwccRange(const void* address, std::size_t bytes) {
  const auto begin = reinterpret_cast<std::uintptr_t>(address) & ~(kSwccCacheLineBytes - 1);
  const auto end = reinterpret_cast<std::uintptr_t>(address) + bytes;
  for (auto line = begin; line < end; line += kSwccCacheLineBytes)
    _mm_clflush(reinterpret_cast<const void*>(line));
  _mm_mfence();
}

}  // namespace dsidle
