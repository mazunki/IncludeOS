#include "common.hpp"
#include <util/pretty.hpp>
#include <kernel/memory.hpp>

// #define TRACE_MUNMAP
#if defined(TRACE_MUNMAP)
  #define MUNMAP_TRACE(fmt, ...) std::println("\033[0;94m{}\033[0m: \033[0;90m" fmt "\033[0m", __FUNCTION__, ##__VA_ARGS__)
#else
  #define MUNMAP_TRACE(...) do {} while(0)
#endif


os::mem::mem_resource& default_allocator = os::mem::raw_allocator();

static long munmap_failed(int err) {
  errno = err;
  return err;
}


static long __sys_munmap(uintptr_t addr, size_t length)
{
  if(UNLIKELY(length == 0))
    return munmap_failed(EINVAL);

  MUNMAP_TRACE("({}, {}) ", KVF(addr, "{:#x}"), KVA(length, util::format::Bytes{length}));

  default_allocator.deallocate(reinterpret_cast<void*>(addr), length);
  return 0;
}

static long sys_munmap(void* _addr, size_t length)
{
  const uintptr_t addr = reinterpret_cast<uintptr_t>(_addr);

  return __sys_munmap(addr, length);
}

extern "C"
long syscall_SYS_munmap(void *addr, size_t length)
{
  return strace(sys_munmap, "munmap", addr, length);
}
