#include <os>
#include <service>
#include <kernel/memory.hpp>
#include <print>
#include <cstddef>
#include <sys/mman.h>

static void* try_mmap(std::size_t size) {
  try {
    return os::mem::raw_allocator().allocate(size);
  } catch (...) {
    return MAP_FAILED;
  }
}

static void try_munmap(void *p, size_t size) {
  if (p != MAP_FAILED) os::mem::raw_allocator().deallocate(p, size);
}

void Service::start()
{
  std::uintptr_t heap_start = 0;
  std::uintptr_t heap_end   = 0;

  for (const auto& [addr, entry] : os::mem::vmmap()) {
    if (entry.name() == std::string_view{"Heap"}) {
      heap_start = entry.addr_start();
      heap_end   = entry.addr_end();
      break;
    }
  }

  const std::size_t heap_size = heap_end - heap_start + 1;
  const std::size_t heap_mib  = heap_size / (1024UL * 1024UL);

  std::println("HEAP_START: {:#x}", heap_start);
  std::println("HEAP_END:   {:#x}", heap_end);
  std::println("HEAP_MiB:   {}", heap_mib);

  // small allocation, always expected to succeed
  {
    constexpr std::size_t small = 1UL * 1024UL * 1024UL; // 1 MiB
    std::println("bytes free: {}", os::mem::raw_allocator().bytes_free());

    void* p = try_mmap(small);
    std::println("HEAP_ALLOC_SMALL (1 MiB): {}", (p != MAP_FAILED) ? "OK" : "FAIL");
    try_munmap(p, small);
  }

  std::println("HEAP_FREE:  {}", heap_mib);
  // large allocation
  {
    const size_t PERCENTAGE = (heap_mib >= 256) ? 30 : 10;
    const std::size_t large     = heap_size * PERCENTAGE / 100;
    const std::size_t large_mib = large / (1024UL * 1024UL);
    void* p = try_mmap(large);

    std::println("bytes free: {}", os::mem::raw_allocator().bytes_free());
    std::println("HEAP_ALLOC_LARGE ({} MiB = {}% of heap): {}", large_mib, PERCENTAGE, (p != MAP_FAILED) ? "OK" : "FAIL");
    try_munmap(p, large);
  }

  // oversized allocation, must fail
  {
    const std::size_t oversize = os::mem::raw_allocator().bytes_free() + 16*1024UL*1024UL;  // + 16MiB
    const std::size_t oversize_mib = oversize / (1024UL * 1024UL);

    std::println("bytes free: {}", os::mem::raw_allocator().bytes_free());
    void* p = try_mmap(oversize);  // this should fail, i.e. MAP_FAILED => ok

    std::println("HEAP_ALLOC_OVERSIZED ({} MiB): {}", oversize_mib, (p == MAP_FAILED) ? "OK" : "FAIL");
  }

  std::println("Test done.");
  os::shutdown();
}
