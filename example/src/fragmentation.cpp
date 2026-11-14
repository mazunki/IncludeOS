#include <sys/mman.h>
#include <print>
#include <cstdlib>
#include <random>
#include <deque>
#include <iterator>
#include <string_view>
#include <kernel/memory.hpp>
#include <mem/alloc/buddy.hpp>
#include <mem/alloc/snake.hpp>
#include "channel.hpp"

inline void print_summary(os::mem::buddy_resource* buddy) {
  std::println("used={} free_blocks={} (ideal={})",
               os::mem::Bytes{buddy->bytes_used()},
               buddy->free_blocks(),
               buddy->ideal_free_blocks());

  const std::size_t free_bytes = buddy->bytes_free();
  const double largest_pct = 100.0 * os::mem::safe_div(buddy->largest_free(), free_bytes);
  const double ideal_pct = 100.0 * os::mem::safe_div(buddy->ideal_size_allocatable(), free_bytes);

  std::println("largest_free={} ({:.2f}% of free bytes)", os::mem::Bytes{buddy->largest_free()}, largest_pct);
  std::println("ideal_size_allocatable={} ({:.2f}% of free bytes)", os::mem::Bytes{buddy->ideal_size_allocatable()}, ideal_pct);
  std::println("block_fragmentation={:.2f}%", buddy->block_fragmentation() * 100.0);

  buddy->draw_tiles();
  std::println();
}

static constexpr int PACKET_SIZES[] = {64, 512, 4096, 16384}; // bytes

Task packet_producer(auto allocator, Channel<os::mem::mem_region>& ch, unsigned seed, int n_packets) {
  std::mt19937 rng(seed);
  std::uniform_int_distribution<int> size_pick(0, static_cast<int>(std::size(PACKET_SIZES)) - 1);
  for (int i = 0; i < n_packets; ++i) {
    auto region = allocator->allocate_region(PACKET_SIZES[size_pick(rng)]);
    co_await ch.send(region);
  }
  ch.close();
}

static constexpr int RESULT_SIZE = 32;

Task packet_consumer(auto allocator, auto main_allocator, Channel<os::mem::mem_region>& ch, int window) {
  std::deque<os::mem::mem_region> held;
  bool snapshotted = false;

  while (auto region = co_await ch.receive()) {
    held.push_back(*region);

    if (static_cast<int>(held.size()) >= window) {
      if (!snapshotted) {
        std::println("--- snapshot: window first filled ({} packets in flight) ---", held.size());
        allocator->dump_state();
        snapshotted = true;
      }
      allocator->deallocate_region(held.front());
      held.pop_front();

      main_allocator->allocate_region(RESULT_SIZE);
    }
  }
  for (auto& r : held) allocator->deallocate_region(r);
}

static constexpr int WORKLOAD_N_PACKETS = 16000;
static constexpr int WORKLOAD_WINDOW = 48;

inline void packet_workload_demo(auto allocator, auto main_allocator, unsigned seed, std::string_view label) {
  std::println("=== workload: {} packets, sizes {{64,128,256,512,1024,1460}}, window={} ===", WORKLOAD_N_PACKETS, WORKLOAD_WINDOW);
  std::println("--- using {} for temporary allocations ---", label);

  Channel<os::mem::mem_region> ch(WORKLOAD_WINDOW);
  auto producer = packet_producer(allocator, ch, seed, WORKLOAD_N_PACKETS);
  auto consumer = packet_consumer(allocator, main_allocator, ch, WORKLOAD_WINDOW);
  producer.start();
  consumer.start();
}

int main() {
  auto* secondary_ptr = os::mem::secondary_allocator();
  if (secondary_ptr == nullptr) {
    std::println("no secondary heap on this machine");
    return 1;
  }
  auto& secondary = *secondary_ptr;

  auto* heap = os::mem::buddy_resource::create_from(secondary, 1 << 21 /* 2 MiB */, os::mem::buddy_config{.min_block = 64});
  if (heap == nullptr) {
    std::println("failed to make buddy allocator");
    return 1;
  }

  auto* snake = os::mem::snake_resource::create_from(secondary, 1 << 22 /* 4 MiB */);
  if (snake == nullptr) {
    std::println("failed to make snake allocator");
    return 1;
  }

  constexpr unsigned seed = 42;

  print_summary(heap);

  packet_workload_demo(heap, heap, seed, "heap");
  print_summary(heap);

  heap->deallocate_all();

  packet_workload_demo(snake, heap, seed, "snake");
  print_summary(heap);

  return 0;
}
