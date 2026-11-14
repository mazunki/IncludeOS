#include <print>
#include <kernel/memory.hpp>
#include <mem/alloc/buddy.hpp>

using os::mem::Bytes;

static void print_allocator(std::string_view label, os::mem::mem_resource& alloc) {
  std::println("{}: total={} free={} largest_free={}",
               label,
               Bytes{alloc.stats().total_bytes},
               Bytes{alloc.bytes_free()},
               Bytes{alloc.largest_free()});
}

int main() {
  auto& primary = os::mem::raw_allocator();
  auto* secondary = os::mem::secondary_allocator();

  print_allocator("primary  ", primary);

  if (secondary == nullptr) {
    std::println("no secondary heap on this machine. give the VM more than 4GiB of RAM to get one");
    return 1;
  }
  print_allocator("secondary", *secondary);
  std::println();

  constexpr std::size_t big = std::size_t(6) << 30; // 6 GiB

  std::println("asking primary for a single {} allocation...", Bytes{big});
  try {
    auto region = primary.allocate_region(big);
    std::println("  unexpectedly succeeded at {:#x}. this shouldn't be possible!", region.start);
    primary.deallocate_region(region);
  } catch (const std::bad_alloc& e) {
    std::println("  failed, as expected: {}", e.what());
  }
  std::println();

  std::println("asking secondary for the same {} allocation...", Bytes{big});
  try {
    auto region = secondary->allocate_region(big);
    std::println("  succeeded at {:#x}", region.start);
    secondary->deallocate_region(region);
  } catch (const std::bad_alloc& e) {
    std::println("  failed: {} (try giving the VM more RAM)", e.what());
  }

  return 0;
}
