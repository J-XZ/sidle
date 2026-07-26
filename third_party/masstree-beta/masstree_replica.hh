#ifndef MASSTREE_REPLICA_HH
#define MASSTREE_REPLICA_HH

#include "masstree_struct.hh"
#include "dsidle/replica_directory.h"

#include <array>
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
    struct source_entry {
      ikey_type ikey{};
      std::uint8_t keylenx{};
      bool is_layer{};
      lcdf::Str suffix{};
      value_pointer value{};
      dsidle::NodeRef layer_ref{};
    };

    const int count = permutation.size();
    std::array<source_entry, leaf_type::width> snapshot{};
    std::size_t bytes = sizeof(header) + static_cast<std::size_t>(count) * sizeof(entry);
    for (int index = 0; index < count; ++index) {
      const int slot = permutation[index];
      source_entry item;
      item.ikey = source.ikey(slot);
      item.keylenx = source.keylenx_[slot];
      item.is_layer = source.is_layer(slot);
      if (source.has_ksuf(slot)) {
        item.suffix = source.ksuf_storage(slot);
        bytes += item.suffix.len;
      }
      if (item.is_layer) {
        item.layer_ref = source.lv_[slot].layer_ref();
      } else {
        item.value = source.lv_[slot].value();
        if (!item.value)
          throw std::runtime_error("cannot replicate null Masstree value");
        bytes = Align(bytes, alignof(value_type));
        bytes += item.value->size();
      }
      snapshot[static_cast<std::size_t>(index)] = item;
    }
    if (bytes > UINT32_MAX) throw std::runtime_error("Masstree leaf replica exceeds 4GiB");
    auto* memory = static_cast<std::byte*>(std::malloc(bytes));
    if (!memory) throw std::bad_alloc();
    auto* out = new (memory) header{static_cast<std::uint32_t>(count), static_cast<std::uint32_t>(bytes)};
    auto* entries = reinterpret_cast<entry*>(memory + sizeof(header));
    std::size_t cursor = sizeof(header) + static_cast<std::size_t>(count) * sizeof(entry);
    for (int index = 0; index < count; ++index) {
      const source_entry& item = snapshot[static_cast<std::size_t>(index)];
      entry& destination = entries[index];
      destination.ikey = item.ikey;
      destination.keylenx = item.keylenx;
      destination.is_layer = item.is_layer;
      if (item.suffix.len) {
        destination.suffix_bytes = static_cast<std::uint16_t>(item.suffix.len);
        destination.suffix_offset = static_cast<std::uint32_t>(cursor);
        std::memcpy(memory + cursor, item.suffix.s, item.suffix.len);
        cursor += item.suffix.len;
      }
      if (destination.is_layer) {
        destination.layer_ref = item.layer_ref;
      } else {
        cursor = Align(cursor, alignof(value_type));
        destination.value_offset = static_cast<std::uint32_t>(cursor);
        destination.value_bytes =
            static_cast<std::uint32_t>(item.value->size());
        std::memcpy(memory + cursor, item.value, item.value->size());
        cursor += item.value->size();
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

  // Snapshot-copy-publish protocol for a canonical leaf. The caller obtains a
  // stable version first; publication is rejected if the leaf changed while
  // its suffix/value bytes were copied.
  static bool Promote(const leaf_type& source, typename leaf_type::nodeversion_type version,
                      dsidle::ReplicaDirectory& directory, bool budgeted = true) {
    const auto ref = source.control_ref();
    const auto generation = dsidle::LoadNodeGeneration(ref);
    // A leaf's permutation and values are changed by foreground writers.  An
    // optimistic version check after Create() is too late: a writer can
    // change a value length between Create's sizing and copying passes.  Do
    // not make promotion contend with writers; skip this candidate unless we
    // can immediately own the leaf for the whole snapshot.
    auto& mutable_source = const_cast<leaf_type&>(source);
    if (!mutable_source.try_lock()) return false;
    const auto locked_version = mutable_source;
    const auto published_version = locked_version.unlocked_version_value();
    if (published_version != version.unlocked_version_value()) {
      mutable_source.unlock();
      return false;
    }
    void* buffer = nullptr;
    try {
      buffer = Create(source, source.permutation());
    } catch (...) {
      mutable_source.unlock();
      throw;
    }
    bool has_layer = false;
    const auto permutation = source.permutation();
    for (int index = 0; index < permutation.size(); ++index)
      has_layer = has_layer || source.is_layer(permutation[index]);
    const auto bytes = static_cast<const header*>(buffer)->bytes;
    void* old = nullptr;
    const dsidle::ReplicaSnapshot snapshot{
        buffer, generation, published_version, bytes,
        has_layer ? dsidle::ReplicaKind::kLayerLeaf
                  : dsidle::ReplicaKind::kValueLeaf};
    if (budgeted) {
      if (!directory.TryPublish(ref, snapshot, &old)) {
        std::free(buffer);
        mutable_source.unlock();
        return false;
      }
    } else {
      old = directory.Publish(ref, snapshot);
    }
    // Publishing before unlock is safe: readers cannot acquire this replica
    // while the canonical version is locked, and unlock releases exactly the
    // version recorded above.
    mutable_source.unlock();
    std::free(old);
    return true;
  }

 private:
  static std::size_t Align(std::size_t value, std::size_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
  }
};

}  // namespace Masstree

#endif
