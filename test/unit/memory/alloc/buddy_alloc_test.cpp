// This file is a part of the IncludeOS unikernel - www.includeos.org
//
// Copyright 2018 IncludeOS AS, Oslo, Norway
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// #define DEBUG_UNIT

#include <common.cxx>
#include <expects>
#include <util/units.hpp>
#include <mem/alloc/buddy.hpp>
#include <mem/allocator.hpp>
#include <vector>
#include <memory>
#include <cstring>

// Backs a buddy_resource with a real, aligned heap buffer. buddy_resource
// itself just manages the region -- it doesn't own or place itself in it.
struct Pool {
  explicit Pool(size_t sz, size_t min_block = 64) : size{sz} {
    int res = posix_memalign(&mem, min_block, sz);
    Expects(res == 0);
    os::mem::mem_config cfg{
      .region = { reinterpret_cast<uintptr_t>(mem), reinterpret_cast<uintptr_t>(mem) + sz },
      .overbooking = false,
    };
    alloc = std::make_unique<os::mem::buddy_resource>(cfg, os::mem::buddy_config{ .min_block = min_block });
  }

  ~Pool() { free(mem); }

  bool owns(uintptr_t addr) const noexcept {
    return addr >= reinterpret_cast<uintptr_t>(mem) && addr < reinterpret_cast<uintptr_t>(mem) + size;
  }

  size_t size;
  void* mem = nullptr;
  std::unique_ptr<os::mem::buddy_resource> alloc;
};

CASE("mem::buddy init allocator"){
  using namespace util;
  Pool pool(64_KiB);
  auto& alloc = *pool.alloc;

  EXPECT(alloc.bytes_used() == 0);
  auto addr = alloc.malloc(4_KiB);
  EXPECT(addr);
  EXPECT(alloc.bytes_used() == 4_KiB);
  alloc.free(addr, 4_KiB);
  EXPECT(alloc.bytes_used() == 0);
}

CASE("mem::buddy basic allocation / deallocation"){
  using namespace util;

  Pool pool(64_KiB, 4_KiB);
  auto& alloc = *pool.alloc;

  size_t sum = 0;
  std::vector<std::pair<uintptr_t, size_t>> allocations;

  // Allocate every power-of-two size that fits
  for (size_t sz = 4_KiB; sz < alloc.bytes_free(); sz *= 2) {
    auto addr = alloc.malloc(sz);
    EXPECT(addr);
    EXPECT(pool.owns(addr));
    EXPECT(alloc.highest_used() == addr + sz);
    allocations.emplace_back(addr, sz);
    sum += sz;
    EXPECT(alloc.bytes_used() == sum);
  }

  // Deallocate in the same order
  for (auto [addr, sz] : allocations) {
    sum -= sz;
    alloc.free(addr, sz);
    EXPECT(alloc.bytes_used() == sum);
  }

  EXPECT(alloc.bytes_used() == 0);
}

CASE("mem::buddy random ordered allocation then deallocation"){
  using namespace util;

  Pool pool(32_MiB);
  auto& alloc = *pool.alloc;

  size_t sum = 0;
  std::vector<std::pair<uintptr_t, size_t>> allocations;

  for (auto rnd : test::random_1k) {
    if (alloc.bytes_free() == 0)
      break;

    const size_t want = std::max<size_t>(64, rnd % (32_KiB));
    const size_t sz = std::bit_ceil(want);

    if (sz > alloc.bytes_free())
      continue;

    uintptr_t addr;
    try {
      addr = alloc.malloc(sz);
    } catch (const std::bad_alloc&) {
      continue;
    }
    EXPECT(addr);
    EXPECT(pool.owns(addr));
    allocations.emplace_back(addr, sz);
    sum += sz;
    EXPECT(alloc.bytes_used() == sum);
  }

  auto highest = std::max_element(allocations.begin(), allocations.end(),
    [](auto& a, auto& b) { return a.first < b.first; });
  EXPECT(highest != allocations.end());
  EXPECT(alloc.highest_used() == highest->first + highest->second);

  // Deallocate
  for (auto [addr, sz] : allocations) {
    sum -= sz;
    alloc.free(addr, sz);
    EXPECT(alloc.bytes_used() == sum);
  }

  EXPECT(alloc.bytes_used() == 0);
}

struct Allocation {
  uintptr_t addr = 0;
  size_t size = 0;
  char data = 0;

  uintptr_t addr_begin() const { return addr; }
  uintptr_t addr_end() const { return addr + size; }

  bool overlaps(uintptr_t other) const {
    return other >= addr_begin() and other < addr_end();
  }

  bool overlaps(const Allocation& other) const {
    return overlaps(other.addr_begin()) or overlaps(other.addr_end() - 1);
  }

  bool verify_data() const {
    auto buf = std::make_unique<char[]>(size);
    memset(buf.get(), data, size);
    return memcmp(buf.get(), reinterpret_cast<void*>(addr), size) == 0;
  }
};

CASE("mem::buddy random chaos with data verification"){
  using namespace util;

  Pool pool(1_GiB);
  auto& alloc = *pool.alloc;

  EXPECT(alloc.bytes_used() == 0);

  std::vector<Allocation> allocs;

  for (auto rnd : test::random_1k) {
    const size_t sz = std::max<size_t>(64, std::bit_ceil<size_t>(rnd % 64_KiB));

    if (sz <= alloc.bytes_free()) {
      Allocation a;
      a.size = sz;
      uintptr_t addr;
      try {
        addr = alloc.malloc(sz);
      } catch (const std::bad_alloc&) {
        addr = 0;
      }
      if (addr != 0) {
        a.addr = addr;
        a.data = 'A' + (rnd % ('Z' - 'A'));
        EXPECT(pool.owns(a.addr));

        auto overlap = std::find_if(allocs.begin(), allocs.end(),
          [&a](const Allocation& other) { return other.overlaps(a); });
        EXPECT(overlap == allocs.end());

        memset(reinterpret_cast<void*>(a.addr), a.data, a.size);
        allocs.emplace_back(a);
      }
    }

    // Deallocate a random allocation most of the time
    if ((rnd % 3 != 0 or alloc.bytes_free() == 0) and not allocs.empty()) {
      auto it = allocs.begin() + (rnd % allocs.size());
      EXPECT(it->verify_data());
      auto use_pre = alloc.bytes_used();
      alloc.free(it->addr, it->size);
      EXPECT(alloc.bytes_used() == use_pre - it->size);
      allocs.erase(it);
    }
  }

  for (auto& a : allocs) {
    alloc.free(a.addr, a.size);
  }

  EXPECT(alloc.bytes_used() == 0);
}

CASE("mem::buddy as pmr::memory_resource") {
  using namespace util;

  Pool pool(1_GiB, 8);
  auto* resource = pool.alloc.get();

  std::pmr::polymorphic_allocator<int> alloc(resource);
  std::pmr::vector<int> numbers(alloc);

  EXPECT(resource->bytes_used() == 0);
  numbers.push_back(10);
  EXPECT(resource->bytes_used() > 0);
  numbers.push_back(20);
  numbers.push_back(30);
  numbers.push_back(40);

  // Force the vector to return memory
  numbers.clear();
  numbers.shrink_to_fit();
  EXPECT(resource->bytes_used() == 0);

  // Make sure it still works as expected
  numbers.push_back(20);
  numbers.push_back(30);
  numbers.push_back(40);

  EXPECT((numbers == std::pmr::vector<int>{20,30,40}));

  numbers.clear();
  numbers.shrink_to_fit();
  EXPECT(resource->bytes_used() == 0);
}

CASE("mem::buddy as std::allocator") {
  using namespace util;

  Pool pool(1_GiB, 8);
  auto* resource = pool.alloc.get();

  std::vector<int, os::mem::Allocator<int, os::mem::buddy_resource>> numbers(*resource);

  EXPECT(resource->bytes_used() == 0);
  numbers.push_back(10);
  EXPECT(resource->bytes_used() > 0);
  numbers.push_back(20);
  numbers.push_back(30);
  numbers.push_back(40);

  // Force the vector to return memory
  numbers.clear();
  numbers.shrink_to_fit();
  EXPECT(resource->bytes_used() == 0);

  // Make sure it still works as expected
  numbers.push_back(20);
  numbers.push_back(30);
  numbers.push_back(40);

  EXPECT(numbers[0] == 20);
  EXPECT(numbers[1] == 30);
  EXPECT(numbers[2] == 40);

  numbers.clear();
  numbers.shrink_to_fit();
  EXPECT(resource->bytes_used() == 0);
}
