#include "common.hpp"
#include <cstdint>
#include <sys/mman.h>
#include <errno.h>
#include <mem/mem.hpp>
#include <mem/alloc/buddy.hpp>
#include <os>
#include <kernel/memory.hpp>
#include <kernel.hpp>
#include <kprint>
#include <new>
#include <cstring>

static os::mem::buddy_resource* buddy_alloc = nullptr;
static os::mem::mem_resource* alloc = nullptr;

os::mem::mem_resource& os::mem::raw_allocator() {
  Expects(alloc);
  return *alloc;
}

uintptr_t __init_mmap(uintptr_t addr_begin, size_t size)
{
  size_t align = PAGE_SIZE; // os::mem::min_psize();
  auto aligned_begin = (addr_begin + align - 1) & ~(align - 1);
  auto aligned_end   = (addr_begin + size) & ~(uintptr_t(align - 1));

  if (aligned_end <= aligned_begin) {
    return aligned_begin;
  }

  auto* self = reinterpret_cast<void*>(aligned_begin);
  uintptr_t usable_begin = aligned_begin + sizeof(os::mem::buddy_resource);

  os::mem::mem_config cfg{
    .region = { usable_begin, aligned_end },
    .overbooking = false,
  };

  os::mem::buddy_config bcfg{
    .min_block = align,
  };

  buddy_alloc = new (self) os::mem::buddy_resource(cfg, bcfg);
  alloc = buddy_alloc;

  return aligned_end;
}

extern "C" __attribute__((weak))
void* kalloc(size_t size) {
  Expects(kernel::heap_ready());
  return alloc->allocate(size);
}

extern "C" __attribute__((weak))
void* kalloc_aligned(size_t alignment, size_t size) {
  Expects(kernel::heap_ready());
  return alloc->allocate(size, alignment);
}

extern "C" __attribute__((weak))
void kfree (void* ptr, size_t size) {
  alloc->deallocate(ptr, size);
}

size_t mmap_bytes_used() {
  return alloc->bytes_used();
}

size_t mmap_bytes_free() {
  return alloc->bytes_free();
}

uintptr_t mmap_allocation_end() {
  return alloc->highest_used();
}

static void* sys_mmap(void * addr, size_t length, int /*prot*/, int flags,
                      int fd, off_t /*offset*/)
{

  // TODO: Implement minimal functionality to be POSIX compliant
  // https://pubs.opengroup.org/onlinepubs/009695399/functions/mmap.html
  if (length <= 0) {
    Expectsf(false, "Must always allocate at least 1 byte. Got {}", length);
    errno = EINVAL;
    return MAP_FAILED;
  }

  if (fd > -1) {
    // None of our file systems support memory mapping at the moment
    Expects(false && "Mapping to file descriptor not supported");
    errno = ENODEV;
    return MAP_FAILED;
  }

  if ((flags & MAP_ANONYMOUS) == 0) {
    Expects(false && "We only support MAP_ANONYMOUS calls to mmap()");
    errno = ENOTSUP;
    return MAP_FAILED;
  }

  if ((flags & MAP_FIXED) > 0) {
    Expects(false && "MAP_FIXED not supported.");
    errno = ENOTSUP;
    return MAP_FAILED;
  }

  if (((flags & MAP_PRIVATE) > 0) && ((flags & MAP_ANONYMOUS) == 0)) {
    Expects(false && "MAP_PRIVATE only supported for MAP_ANONYMOUS");
    errno = ENOTSUP;
    return MAP_FAILED;
  }

  if (((flags & MAP_PRIVATE) > 0) && (addr != 0)) {
    Expects(false && "MAP_PRIVATE only supported for new allocations (address=0).");
    errno = ENOTSUP;
    return MAP_FAILED;
  }

  if (((flags & MAP_SHARED) == 0) && ((flags & MAP_PRIVATE) == 0)) {
    Expects(false && "MAP_SHARED or MAP_PRIVATE must be set.");
    errno = ENOTSUP;
    return MAP_FAILED;
  }

  // If we get here, the following should be true:
  // MAP_ANONYMOUS set + MAP_SHARED or MAP_PRIVATE
  // fd should be 0, address should be 0 for MAP_PRIVATE
  // (address is in any case ignored)

  auto* res = kalloc(length);

  if (UNLIKELY(res == nullptr)) {
    errno = ENOMEM;
    return MAP_FAILED;
  }

  memset(res, 0, length);
  return res;
}

extern "C"
void* syscall_SYS_mmap(void *addr, size_t length, int prot, int flags,
                      int fd, off_t offset)
{
  return strace(sys_mmap, "mmap", addr, length, prot, flags, fd, offset);
}

/**
  The mmap2() system call provides the same interface as mmap(2),
  except that the final argument specifies the offset into the file in
  4096-byte units (instead of bytes, as is done by mmap(2)).  This
  enables applications that use a 32-bit off_t to map large files (up
  to 2^44 bytes).

  http://man7.org/linux/man-pages/man2/mmap2.2.html
**/

extern "C"
void* syscall_SYS_mmap2(void *addr, size_t length, int prot,
                        int flags, int fd, off_t offset) {
  return strace(sys_mmap, "mmap2", addr, length, prot, flags, fd, offset * 4096);
}
