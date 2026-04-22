#include "common.hpp"
#include <kernel/memory.hpp>
#include <util/bitops.hpp>
#include <sys/mman.h>
#include <mem/flags.hpp>
#include <mem/protect.hpp>

static long sys_mprotect(void* _addr, size_t length, int prot)
{
  const uintptr_t addr = reinterpret_cast<uintptr_t>(_addr);
  const os::mem::Permission perms = to_permission(prot);

  try {
    __sys_mprotect(addr, length, perms);
  } catch (...) {
    return -ENOMEM;
  }

  return 0; // ok
}

extern "C"
long syscall_SYS_mprotect(void *addr, size_t len, int prot)
{
  return strace(sys_mprotect, "mprotect", addr, len, prot);
}
