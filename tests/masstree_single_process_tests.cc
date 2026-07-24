#include "dsidle/shared_pool.h"
#include "dsidle/shard_allocator.h"

#include "query_masstree.hh"
#include "masstree_struct.hh"
#include "masstree_tcursor.hh"
#include "masstree_get.hh"
#include "masstree_insert.hh"

#include <cassert>
#include <cstring>
#include <unistd.h>

int main() {
  char path[] = "/tmp/dsidle-masstree-test.XXXXXX";
  const int fd = mkstemp(path);
  assert(fd >= 0);
  close(fd);
  unlink(path);

  constexpr std::uint64_t kPoolBytes = 128ULL << 20;
  auto pool = dsidle::SharedPool::Create(path, {kPoolBytes, 0, 32ULL << 20, 32ULL << 20, 96ULL << 20});
  dsidle::InitializePoolMetadata(pool, {1, 2, 1024});
  dsidle::FixedBlockShardAllocator::InitializeAll(pool, 1);
  dsidle::ConfigureCurrentSwccAllocator(pool, 1, 0);

  threadinfo* ti = threadinfo::make(threadinfo::TI_MAIN, 0);
  Masstree::default_table table;
  table.initialize(*ti, 80);
  query<row_type> query;
  assert(query.run_replace(table.table(), lcdf::Str("key", 3), lcdf::Str("value", 5), *ti) == Inserted);

  lcdf::Str value;
  assert(query.run_get1(table.table(), lcdf::Str("key", 3), 0, value, *ti));
  assert(value.len == 5 && std::memcmp(value.s, "value", 5) == 0);

  pool.Close();
  assert(unlink(path) == 0);
}
