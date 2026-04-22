#include <kernel/memory.hpp>
#include <sys/mman.h>
#include <mem/flags.hpp>
#include <util/pretty.hpp>

// #define TRACE_MEM_PROTECT
#if defined(TRACE_MEM_PROTECT)
  #define MEM_PROTECT_TRACE(fmt, ...) std::println("\033[0;94m{}\033[0m: \033[0;90m" fmt "\033[0m", __FUNCTION__, ##__VA_ARGS__)
#else
  #define MEM_PROTECT_TRACE(...) do {} while(0)
#endif

static inline os::mem::Permission to_permission(int prot)
{
  switch (prot & (PROT_READ | PROT_WRITE | PROT_EXEC)) {
    case PROT_NONE:
      return os::mem::Permission::Forbidden;
    case PROT_READ:
      return os::mem::Permission::ReadOnly;
    case PROT_WRITE:
      return os::mem::Permission::WriteOnly;
    case PROT_EXEC:
      return os::mem::Permission::ExecuteOnly;
    case PROT_READ | PROT_WRITE:
      return os::mem::Permission::Data;
    case PROT_READ | PROT_EXEC:
      return os::mem::Permission::Code;
    case PROT_WRITE | PROT_EXEC:
      return os::mem::Permission::CodeBuffer;
    case PROT_READ | PROT_WRITE | PROT_EXEC:
      return os::mem::Permission::Open;
    default:
      throw std::invalid_argument("illegal protection flag");
  }
}

static inline void __sys_mprotect(uintptr_t addr, size_t length, os::mem::Permission perms)
{
  MEM_PROTECT_TRACE("({}, {}, {})",
                    KVF(addr,   "{:#x}"),
                    KVA(length, util::format::Bytes{length}),
                    KV(perms));

  const auto linear = reinterpret_cast<uintptr_t>(addr);

  if (!util::bits::is_aligned(PAGE_SIZE, linear)) {
    throw std::runtime_error("mprotect() requires aligned addresses");
  }

  const size_t page_sz = PAGE_SIZE;
  const size_t len_rounded = util::bits::align_up(length, page_sz);

  [[maybe_unused]] const auto before = os::mem::permissions(linear);
  MEM_PROTECT_TRACE("before={} => requested={}", before, perms);

  const auto res = os::mem::protect_existing(linear, len_rounded, perms);
  if (!res) {
    throw std::runtime_error("failed to protect mapping");
  }
}

