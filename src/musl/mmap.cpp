#include "common.hpp"
#include <cstdint>
#include <sys/mman.h>
#include <errno.h>
#include <mem/mem.hpp>
#include <mem/flags.hpp>
#include <mem/alloc/buddy.hpp>
#include <mem/protect.hpp>
#include <os>
#include <kernel/memory.hpp>
#include <kernel.hpp>
#include <kprint>
#include <new>
#include <cstring>
#include <util/pretty.hpp>

// #define TRACE_MMAP
#if defined(TRACE_MMAP)
  #define MMAP_TRACE(fmt, ...) std::println("\033[0;94m{}\033[0m: \033[0;90m" fmt "\033[0m", __FUNCTION__, ##__VA_ARGS__)
#else
  #define MMAP_TRACE(...) do {} while(0)
#endif

static os::mem::buddy_resource* buddy_alloc = nullptr;
static os::mem::mem_resource* default_allocator = nullptr;

os::mem::mem_resource& os::mem::raw_allocator() {
  Expects(default_allocator);
  return *default_allocator;
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
  default_allocator = buddy_alloc;

  return aligned_end;
}

extern "C" __attribute__((weak))
void* kalloc(size_t size) {
  Expects(kernel::heap_ready());
  return default_allocator->allocate(size);
}

extern "C" __attribute__((weak))
void* kalloc_aligned(size_t alignment, size_t size) {
  Expects(kernel::heap_ready());
  return default_allocator->allocate(size, alignment);
}

extern "C" __attribute__((weak))
void* kalloc_fixed(void* addr, size_t size, bool override)
{
  Expects(kernel::heap_ready());
  return default_allocator->allocate_at(addr, size, override);
}

extern "C" __attribute__((weak))
void kfree (void* ptr, size_t size) {
  default_allocator->deallocate(ptr, size);
}

size_t mmap_bytes_used() {
  return default_allocator->bytes_used();
}

size_t mmap_bytes_free() {
  return default_allocator->bytes_free();
}

uintptr_t mmap_allocation_end() {
  return default_allocator->highest_used();
}

static void* mmap_failed(int err) {
  errno = err;
  return MAP_FAILED;
}

using Fd = std::optional<int>;

static void* __sys_mmap(uintptr_t addr, size_t length, os::mem::Permission perms, os::mem::alloc::Flags flags, Fd file_descriptor, off_t offset)
{
  using os::mem::alloc::Flags;

  MMAP_TRACE("({}, {}, {}, {}, {}, {})",
             KVF(addr,   "{:#x}"),
             KVA(length, util::format::Bytes{length}),
             KV(perms),
             KV(flags),
             KVE("fd",   file_descriptor.value_or(-1)),
             KV(offset));

  if (util::has_flag(flags, Flags::Private) ==
      util::has_flag(flags, Flags::Shared))
  {
    Expects(false && "Mapping must be either Private xor Shared");
    return mmap_failed(EINVAL);
  }

  if (length == 0) {
    Expects(false && "Mapping must never allocate 0 bytes");
    return mmap_failed(EINVAL);
  }

  if (util::has_flag(flags, Flags::Anonymous)) {
    if (file_descriptor) {
      Expects(false && "Anonymous mappings must set fd=-1");
      return mmap_failed(EINVAL);
    }
    if (offset != 0) {
      Expects(false && "Anonymous mappings should have offset=0");
      return mmap_failed(EINVAL);
    }
  }

  // non-fixed address is only a hint for now, so ignore it
  if (addr != 0 &&
      util::missing_flag(flags, Flags::FixedFriendly) &&
      util::missing_flag(flags, Flags::FixedOverride))
  {
    addr = 0;
  }

  if (file_descriptor) {
    Expects(false && "Mapping to file descriptor is not yet implemented");
    return mmap_failed(ENOTSUP);
  }

  if (util::missing_flag(flags, Flags::Anonymous)) {
    Expects(false && "Support for non-anonymous mappings is not yet implemented");
    return mmap_failed(ENOTSUP);
  }

  void* res = nullptr;

  if (util::has_flag(flags, Flags::FixedFriendly) ||
      util::has_flag(flags, Flags::FixedOverride))
  {
    const size_t page_sz = PAGE_SIZE;
    const size_t len_rounded = util::bits::align_up(page_sz, length);

    if (addr == 0) {
      return mmap_failed(EINVAL);
    }

    if (!util::bits::is_aligned(page_sz, addr)) {
      return mmap_failed(EINVAL);
    }

    const bool do_override = util::has_flag(flags, Flags::FixedOverride);

    res = kalloc_fixed(reinterpret_cast<void*>(addr), len_rounded, do_override);
    MMAP_TRACE("kalloc_fixed => {:#x} (requested {:#x})", reinterpret_cast<uintptr_t>(res), addr);
    __sys_mprotect(addr, len_rounded, perms);
  }
  else {
    if (util::has_flag(flags, Flags::Private)) {
      if (util::missing_flag(flags, Flags::Anonymous)) {
        Expects(false && "Support for MAP_PRIVATE other than MAP_ANONYMOUS is not yet implemented");
        return mmap_failed(ENOTSUP);
      }

      if (addr != 0) {
        Expects(false && "Support for MAP_PRIVATE with non-zero hint address is not yet implemented");
        return mmap_failed(ENOTSUP);
      }

      res = kalloc(length);
      MMAP_TRACE("kalloc => {:#x} (requested {:#x})", reinterpret_cast<uintptr_t>(res), addr);
      __sys_mprotect(reinterpret_cast<uintptr_t>(res), length, perms);
    }
  }

  if (UNLIKELY(res == nullptr)) {
    return mmap_failed(ENOMEM);
  }

  // std::memset(res, 0, length);
  return res;
}

static void* sys_mmap(void* _addr, size_t length, int _prot, int _flags, int _fd, off_t offset)
{
  const auto flags = static_cast<os::mem::alloc::Flags>(_flags);
  const auto perms = to_permission(_prot);
  const Fd file_descriptor = (_fd == -1) ? std::nullopt : Fd(_fd);
  const uintptr_t addr = reinterpret_cast<uintptr_t>(_addr);

  return __sys_mmap(addr, length, perms, flags, file_descriptor, offset);
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
