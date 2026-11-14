#include <cstddef>
#include <print>
#include <mem/alloc/buddy.hpp>

auto alloc(os::mem::buddy_resource& alloc, int bytes) {
  auto region = alloc.allocate_region(bytes);
  std::println("allocated {} bytes at offset [{:#x}, {:#x})", bytes,
               alloc.offset_of(region.start), alloc.offset_of(region.end));
  return region;
}

auto alloc_at(os::mem::buddy_resource& alloc, size_t offset, size_t bytes) {
  auto region = alloc.allocate_region_at(alloc.addr_at(offset), bytes, false);
  std::println("carved {} bytes at offset {:#x}: {}", bytes, offset, region.size() > 0 ? "OK" : "FAILED");
  return region;
}


void free(os::mem::buddy_resource& alloc, os::mem::mem_region mem) {
  std::println("freeeing {} bytes", mem.size());
  alloc.deallocate_region(mem);
}

inline void ex1(auto& buddy) {
  auto large0 = alloc(*buddy, 1 << 18);
  auto large1 = alloc(*buddy, 1 << 17);
  auto small0 = alloc(*buddy, 1 << 12);
  auto small1 = alloc(*buddy, 1 << 16);
  auto small2 = alloc(*buddy, 1 << 12);
  free(*buddy, small1);

  auto large2 = alloc(*buddy, 1 << 17);
  auto small3 = alloc(*buddy, 1 << 12);
  free(*buddy, small0);
  free(*buddy, small3);
  auto large3 = alloc(*buddy, 1 << 17);

  buddy->dump_state();
  buddy->draw_tiles();

  free(*buddy, small2);

  // buddy->dump_state();
  buddy->draw_tiles();

  auto biggest = buddy->allocate_largest();
  std::println("biggest free block: offset [{:#x}, {:#x}) = {} bytes",
               buddy->offset_of(biggest.start), buddy->offset_of(biggest.end), biggest.size());
  buddy->deallocate_region(biggest);

  auto midpoint = buddy->offset_of(biggest.start) + biggest.size() / 2;
  auto dumb0 = alloc_at(*buddy, midpoint, 1<<14);

  buddy->dump_state();
  buddy->draw_tiles();

  free(*buddy, large0);
  free(*buddy, large1);
  free(*buddy, large2);
  free(*buddy, large3);

  free(*buddy, dumb0);
}

inline void ex2(auto& buddy) {
  // buddy->dump_state();

  buddy->draw_tiles();

  auto mem0 = alloc(*buddy, 1<<20);
  buddy->draw_tiles();

  auto mem1 = alloc(*buddy, 1<<19);
  buddy->draw_tiles();

  auto mem2 = alloc(*buddy, 4);
  buddy->draw_tiles();

  free(*buddy, mem1);
  buddy->draw_tiles();

  free(*buddy, mem0);
  buddy->draw_tiles();

  auto mem3 = alloc_at(*buddy, 1<<20, 1<<18);
  buddy->draw_tiles();

  free(*buddy, mem2);
  buddy->draw_tiles();

  free(*buddy, mem3);
  buddy->draw_tiles();

  buddy->dump_state();
}

inline void ex3(auto& buddy) {
  std::println("--- start: one big free pool ---");
  buddy->draw_tiles();

  auto a = alloc(*buddy, 1<<20);
  auto b = alloc(*buddy, 1<<20);
  auto c = alloc(*buddy, 1<<20);
  auto d = alloc(*buddy, 1<<19);

  free(*buddy, a);
  free(*buddy, c);

  buddy->dump_state();
  buddy->draw_tiles();

  free(*buddy, b);
  free(*buddy, d);
}
