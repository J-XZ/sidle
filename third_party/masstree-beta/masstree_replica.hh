#ifndef MASSTREE_REPLICA_HH
#define MASSTREE_REPLICA_HH

#include "masstree_struct.hh"

#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <type_traits>

namespace Masstree {

// A process-local, self-contained encoding of one canonical leaf.  It never
// embeds a SWCC virtual address: normal entries own a byte-for-byte row copy,
// external suffix bytes are copied, and layer entries retain only NodeRef.
template <typename P>
class leaf_replica {
 public:
  using leaf_type = leaf<P>;
  using key_type = typename leaf_type::key_type;
  using value_pointer = typename P::value_type;
  using value_type = typename std::remove_pointer<value_pointer>::type;
  using ikey_type = typename P::ikey_type;

  enum class result { kMiss, kValue, kLayer };

  struct header {
    std::uint32_t count{};
    std::uint32_t bytes{};
  };
  struct entry {
    ikey_type ikey{};
    std::uint8_t keylenx{};
    std::uint8_t is_layer{};
    std::uint16_t suffix_bytes{};
    std::uint32_t suffix_offset{};
    std::uint32_t value_bytes{};
    std::uint32_t value_offset{};
    dsidle::NodeRef layer_ref{};
  };

  static void* Create(const leaf_type& source, typename leaf_type::permuter_type permutation) {
    const int count = permutation.size();
    std::size_t bytes = sizeof(header) + static_cast<std::size_t>(count) * sizeof(entry);
    for (int index = 0; index < count; ++index) {
      const int slot = permutation[index];
      if (source.has_ksuf(slot)) bytes += source.ksuf_storage(slot).len;
      if (!source.is_layer(slot)) {
        const value_pointer value = source.lv_[slot].value();
        if (!value) throw std::runtime_error("cannot replicate null Masstree value");
        bytes = Align(bytes, alignof(value_type));
        bytes += value->size();
      }
    }
    if (bytes > UINT32_MAX) throw std::runtime_error("Masstree leaf replica exceeds 4GiB");
    auto* memory = static_cast<std::byte*>(std::malloc(bytes));
    if (!memory) throw std::bad_alloc();
    auto* out = new (memory) header{static_cast<std::uint32_t>(count), static_cast<std::uint32_t>(bytes)};
    auto* entries = reinterpret_cast<entry*>(memory + sizeof(header));
    std::size_t cursor = sizeof(header) + static_cast<std::size_t>(count) * sizeof(entry);
    for (int index = 0; index < count; ++index) {
      const int slot = permutation[index];
      entry& destination = entries[index];
      destination.ikey = source.ikey(slot);
      destination.keylenx = source.keylenx_[slot];
      destination.is_layer = source.is_layer(slot);
      if (source.has_ksuf(slot)) {
        const lcdf::Str suffix = source.ksuf_storage(slot);
        destination.suffix_bytes = static_cast<std::uint16_t>(suffix.len);
        destination.suffix_offset = static_cast<std::uint32_t>(cursor);
        std::memcpy(memory + cursor, suffix.s, suffix.len);
        cursor += suffix.len;
      }
      if (destination.is_layer) {
        destination.layer_ref = source.lv_[slot].layer()->control_ref();
      } else {
        const value_pointer value = source.lv_[slot].value();
        cursor = Align(cursor, alignof(value_type));
        destination.value_offset = static_cast<std::uint32_t>(cursor);
        destination.value_bytes = static_cast<std::uint32_t>(value->size());
        std::memcpy(memory + cursor, value, value->size());
        cursor += value->size();
      }
    }
    if (cursor != bytes) { std::free(memory); throw std::runtime_error("Masstree leaf replica size mismatch"); }
    return out;
  }

  static result Lookup(const void* replica, const key_type& key, const value_type*& value,
                       dsidle::NodeRef& layer_ref) {
    const auto* memory = static_cast<const std::byte*>(replica);
    const auto* data = static_cast<const header*>(replica);
    const auto* entries = reinterpret_cast<const entry*>(memory + sizeof(header));
    for (std::uint32_t index = 0; index < data->count; ++index) {
      const entry& candidate = entries[index];
      const int comparison = key.compare(candidate.ikey, candidate.keylenx);
      if (comparison < 0) return result::kMiss;
      if (comparison > 0) continue;
      if (candidate.suffix_bytes) {
        const lcdf::Str suffix(reinterpret_cast<const char*>(memory + candidate.suffix_offset),
                               candidate.suffix_bytes);
        if (suffix.len != key.suffix().len ||
            std::memcmp(suffix.s, key.suffix().s, suffix.len) != 0)
          return result::kMiss;
      }
      if (candidate.is_layer) { layer_ref = candidate.layer_ref; return result::kLayer; }
      value = reinterpret_cast<const value_type*>(memory + candidate.value_offset);
      return result::kValue;
    }
    return result::kMiss;
  }

 private:
  static std::size_t Align(std::size_t value, std::size_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
  }
};

}  // namespace Masstree

#endif
