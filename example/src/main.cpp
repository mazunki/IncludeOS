#include <print>
#include <random>
#include <vector>
#include <deque>
#include <span>
#include <kernel/memory.hpp>
#include <mem/alloc/buddy.hpp>
#include <mem/alloc/arena.hpp>
#include <mem/alloc/snake.hpp>

using os::mem::Bytes;

static constexpr std::size_t PACKET_SIZES[] = {64, 512, 4096, 16384};

template <typename Callback = void(*)(os::mem::mem_resource*)>
static void run_scratch_workload(os::mem::mem_resource* alloc, unsigned seed, int n_tasks,
                                  int min_allocs, int max_allocs,
                                  std::span<const std::size_t> sizes,
                                  Callback on_task_end = [](os::mem::mem_resource*){}) {
  std::mt19937 rng(seed);
  std::uniform_int_distribution<int> n_allocs(min_allocs, max_allocs);
  std::uniform_int_distribution<std::size_t> size_pick(0, sizes.size() - 1);

  for (int t = 0; t < n_tasks; ++t) {
    const int k = n_allocs(rng);
    for (int i = 0; i < k; ++i) {
      alloc->allocate_region(sizes[size_pick(rng)]);
    }
    on_task_end(alloc);
    alloc->deallocate_all();
  }
}

template <typename Callback = void(*)(os::mem::mem_resource*)>
static void run_window_workload(os::mem::mem_resource* alloc, unsigned seed, int n_steps, int window,
                                 std::span<const std::size_t> sizes,
                                 Callback on_step = [](os::mem::mem_resource*){}) {
  std::mt19937 rng(seed);
  std::uniform_int_distribution<std::size_t> size_pick(0, sizes.size() - 1);
  std::deque<os::mem::mem_region> live;

  for (int i = 0; i < n_steps; ++i) {
    live.push_back(alloc->allocate_region(sizes[size_pick(rng)]));
    if (static_cast<int>(live.size()) > window) {
      alloc->deallocate_region(live.front());
      live.pop_front();
    }
    on_step(alloc);
  }
  for (auto& r : live) alloc->deallocate_region(r);
}

int main() {
  auto* secondary_ptr = os::mem::secondary_allocator();
  if (secondary_ptr == nullptr) {
    std::println("no secondary (post-4G) heap on this machine");
    return 1;
  }
  auto& secondary = *secondary_ptr;

  constexpr unsigned seed = 42;
  constexpr std::size_t KiB = std::size_t(1) << 10;

  // step 1
  auto* heap = os::mem::buddy_resource::create_from(secondary, 256 * KiB, os::mem::buddy_config{.min_block = 64});
  if (heap == nullptr) {
    std::println("failed to carve heap");
    return 1;
  }
  run_scratch_workload(heap, seed, 50, 2, 5, PACKET_SIZES);
  std::println("heap (standalone):   used={} highest_used={}",
               Bytes{heap->bytes_used()}, Bytes{heap->offset_of(heap->highest_used())});
  heap->deallocate_all();

  // step 2
  auto* arena = os::mem::arena_resource::create_from(secondary, 256 * KiB);
  if (arena == nullptr) {
    std::println("failed to carve arena");
    return 1;
  }
  run_scratch_workload(arena, seed, 50, 2, 5, PACKET_SIZES);
  const std::size_t arena_peak = arena->offset_of(arena->highest_used());
  std::println("arena (standalone):  used={} highest_used={}",
               Bytes{arena->bytes_used()}, Bytes{arena_peak});

  // step 2b
  constexpr std::size_t temp_region_size = 256 * KiB;
  auto temp_buddy_region = secondary.allocate_region(temp_region_size, alignof(std::max_align_t));
  auto* temp_buddy = os::mem::buddy_resource::create_at(temp_buddy_region, os::mem::buddy_config{.min_block = 64});
  if (temp_buddy == nullptr) {
    std::println("failed to carve temp buddy");
    return 1;
  }
  {
    std::size_t min_free = temp_buddy->bytes_free();
    std::size_t min_largest = temp_buddy->largest_free();
    std::println("temp buddy (standalone, {} raw region, {} usable) per-task free/largest_free:",
                 Bytes{temp_region_size}, Bytes{temp_buddy->bytes_free()});
    int task_i = 0;
    run_scratch_workload(temp_buddy, seed, 50, 2, 5, PACKET_SIZES,
      [&](os::mem::mem_resource* a) {
        const std::size_t free_now = a->bytes_free();
        const std::size_t largest_now = a->largest_free();
        min_free = std::min(min_free, free_now);
        min_largest = std::min(min_largest, largest_now);
        std::println("  task {:>2}: free={} largest_free={}", task_i++, Bytes{free_now}, Bytes{largest_now});
      });
    std::println("temp buddy (standalone):  min_free={} min_largest_free={}", Bytes{min_free}, Bytes{min_largest});
  }

  // step 2c
  auto temp_snake_region = secondary.allocate_region(temp_region_size, alignof(std::max_align_t));
  auto* temp_snake = os::mem::snake_resource::create_at(temp_snake_region, os::mem::snake_config{.max_pending = 32});
  if (temp_snake == nullptr) {
    std::println("failed to carve temp snake");
    return 1;
  }
  {
    std::size_t min_free = temp_snake->bytes_free();
    std::size_t min_largest = temp_snake->largest_free();
    std::println("temp snake (standalone, {} raw region, {} usable) per-task free/largest_free:",
                 Bytes{temp_region_size}, Bytes{temp_snake->bytes_free()});
    int task_i = 0;
    run_scratch_workload(temp_snake, seed, 50, 2, 5, PACKET_SIZES,
      [&](os::mem::mem_resource* a) {
        const std::size_t free_now = a->bytes_free();
        const std::size_t largest_now = a->largest_free();
        min_free = std::min(min_free, free_now);
        min_largest = std::min(min_largest, largest_now);
        std::println("  task {:>2}: free={} largest_free={}", task_i++, Bytes{free_now}, Bytes{largest_now});
      });
    std::println("temp snake (standalone):  min_free={} min_largest_free={}", Bytes{min_free}, Bytes{min_largest});
  }

  // step 2d
  auto window_buddy_region = secondary.allocate_region(temp_region_size, alignof(std::max_align_t));
  auto* window_buddy = os::mem::buddy_resource::create_at(window_buddy_region, os::mem::buddy_config{.min_block = 64});
  auto window_snake_region = secondary.allocate_region(temp_region_size, alignof(std::max_align_t));
  auto* window_snake = os::mem::snake_resource::create_at(window_snake_region, os::mem::snake_config{.max_pending = 64});
  if (window_buddy == nullptr || window_snake == nullptr) {
    std::println("failed to carve window buddy/snake");
    return 1;
  }
  constexpr int window = 8;
  constexpr int n_steps = 300;
  std::println("window workload (buddy, {} usable, window={}) free/largest_free over {} steps:",
               Bytes{window_buddy->bytes_free()}, window, n_steps);
  {
    int step_i = 0;
    run_window_workload(window_buddy, seed, n_steps, window, PACKET_SIZES,
      [&](os::mem::mem_resource* a) {
        std::println("  step {:>3}: free={} largest_free={}", step_i++, Bytes{a->bytes_free()}, Bytes{a->largest_free()});
      });
  }
  std::println("window workload (snake, {} usable, window={}) free/largest_free over {} steps:",
               Bytes{window_snake->bytes_free()}, window, n_steps);
  {
    int step_i = 0;
    run_window_workload(window_snake, seed, n_steps, window, PACKET_SIZES,
      [&](os::mem::mem_resource* a) {
        std::println("  step {:>3}: free={} largest_free={}", step_i++, Bytes{a->bytes_free()}, Bytes{a->largest_free()});
      });
  }

  // step 3
  std::println("\nheap (before control): used={} free={} largest_free={}",
               Bytes{heap->bytes_used()}, Bytes{heap->bytes_free()}, Bytes{heap->largest_free()});
  heap->draw_tiles();
  auto* control_arena = os::mem::arena_resource::create_from(*heap, arena_peak);
  if (control_arena == nullptr) {
    std::println("failed to carve control arena from heap");
    return 1;
  }
  run_scratch_workload(control_arena, seed, 50, 2, 5, PACKET_SIZES);
  std::println("control (in heap):   used={} highest_used={}",
               Bytes{control_arena->bytes_used()}, Bytes{control_arena->offset_of(control_arena->highest_used())});
  heap->deallocate_all(); // undo the control carve, back to a clean, fully-free heap

  // step 4
  constexpr std::size_t block = 2 * KiB;
  constexpr int n_blocks = 128; // 128 * 2 KiB = 256 KiB = heap's full capacity
  std::vector<os::mem::mem_region> blocks;
  for (int i = 0; i < n_blocks; ++i) {
    blocks.push_back(heap->allocate_region(block));
  }
  for (std::size_t i = 1; i < blocks.size(); i += 2) {
    heap->deallocate_region(blocks[i]);
  }
  std::println("heap (fragmented):   used={} free={} largest_free={}",
               Bytes{heap->bytes_used()}, Bytes{heap->bytes_free()}, Bytes{heap->largest_free()});
  heap->draw_tiles();

  // step 5
  std::println("\ncarving a {} temp arena from the fragmented heap (bytes_free={} >= this, but largest_free={} < this)...",
               Bytes{arena_peak}, Bytes{heap->bytes_free()}, Bytes{heap->largest_free()});
  auto* temp_arena = os::mem::arena_resource::create_from(*heap, arena_peak); // expected to throw, this is the point

  std::println("unexpectedly succeeded: {}", static_cast<void*>(temp_arena));
  return 0;
}
