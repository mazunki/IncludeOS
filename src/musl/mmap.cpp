#include "common.hpp"
#include <cstdint>
#include <sys/mman.hpp>
#include <errno.h>
#include <util/alloc_buddy.hpp>
#include <os>
#include <kernel/memory.hpp>
#include <kernel.hpp>
#include <kprint>

using Alloc = os::mem::Raw_allocator;
static Alloc* alloc;

Alloc& os::mem::raw_allocator() {
  Expects(alloc);
  return *alloc;
}

uintptr_t __init_mmap(uintptr_t addr_begin, size_t size)
{
  auto aligned_begin = (addr_begin + Alloc::align - 1) & ~(Alloc::align - 1);
  int64_t len = size & ~int64_t(Alloc::align - 1);

  alloc = Alloc::create((void*)aligned_begin, len);
  return aligned_begin + len;
}

extern "C" __attribute__((weak))
void* kalloc(size_t size) {
  Expects(kernel::heap_ready());
  return alloc->allocate(size);
}

extern "C" __attribute__((weak))
void* kalloc_aligned(size_t alignment, size_t size) {
  Expects(kernel::heap_ready());
  return alloc->do_allocate(size, alignment);
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

static void* sys_mmap(void * addr, size_t length, int /*prot*/, int _flags,
                      int _fd, off_t offset)
{
  // NOTE: `must` and `should` messages in this function refer to POSIX mmap(3p)
  using os::mem::Flags;

  const Flags flags = static_cast<Flags>(_flags);
  const std::optional<int> file_descriptor = (_fd == -1) ? std::nullopt : std::optional<int>(_fd);

  // TODO(mazunki): this could become a constexpr by making os::mem::Sharing a mutually exclusive type
  if (util::has_flag(flags, Flags::Private) == util::has_flag(flags, Flags::Shared)) {
    Expects(false && "sys_mmap: mapping must be either Private xor Shared");
    errno = EINVAL;
    return MAP_FAILED;
  }

  if (length == 0) {
    Expects(false && "Mapping must never allocate 0 bytes");
    errno = EINVAL;
    return MAP_FAILED;
  }

  if (util::has_flag(Flags::Anonymous)) {
    if (file_descriptor) {
      Expects(false && "Anonymous mappings must set fd=-1");  // TODO(mazunki): rename -1 when signature changes
      errno = EINVAL;
      return MAP_FAILED;
    }
    if (offset != 0) {
      Expects(false && "Anonymous mappings should have offset=0");
      errno = EINVAL;
      return MAP_FAILED;
    }
  }

  // NOTE: specifying an address with non-fixed allocation is, per spec, only
  // a hint. for now, we ignore this hint.
  if ((addr != 0) && util::missing_flag(flags, Flags::Fixed))  {
    addr = 0;
  }


  // TODO: Implement minimal functionality to be POSIX compliant
  // https://pubs.opengroup.org/onlinepubs/009695399/functions/mmap.html

  if (file_descriptor) {
    Expects(false && "Mapping to file descriptor is not yet implemented");
    errno = ENOTSUP;
    return MAP_FAILED;
  }

  if (util::missing_flag(flags, Flags::Anonymous)) {
    Expects(false && "Support for non-MAP_ANONYMOUS mappings is not yet implemented");
    errno = ENOTSUP;
    return MAP_FAILED;
  }

  if (util::has_flag(flags, Flags::Fixed)) {
    Expects(false && "Support for MAP_FIXED mappings is not yet implemented.");
    errno = ENOTSUP;
    return MAP_FAILED;
  }

  if (util::has_flag(flags, Flags::Private)) {
    if (util::missing_flag(flags, Flags::Anonymous)) {
      Expects(false && "Support for MAP_PRIVATE other than MAP_ANONYMOUS is not yet implemented");
      errno = ENOTSUP;
      return MAP_FAILED;
    }
    if (addr != 0) {
      Expects(false && "Support for MAP_PRIVATE other than for new allocations (addr=0) is not yet implemented");
      errno = ENOTSUP;
      return MAP_FAILED;
    }
  }

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
