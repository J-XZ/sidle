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
  // A flush is a protocol event, not a second ordinary SWCC write. It is
  // intentionally recorded after the real flush and carries no fixed delay.
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

inline void RecordSwccExplicitHandoff(const void* address, std::size_t bytes,
                                       std::uint32_t from_node,
                                       std::uint32_t to_node,
                                       std::uint32_t tag = 0) {
  latency_sim::GlobalLatencySimulator().RecordSwccExplicitHandoff(
      latency_sim::PoolKind::kSwcc, address, bytes, from_node, to_node, tag);
}

}  // namespace dsidle
