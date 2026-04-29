#include <kernel/memory.hpp>
#include <sys/mman.h>
#include <mem/flags.hpp>
#include <util/pretty.hpp>

#define TRACE_MEM_PROTECT
#if defined(TRACE_MEM_PROTECT)
  #define MEM_PROTECT_TRACE(fmt, ...) std::println("\033[0;94m{}\033[0m: \033[0;90m" fmt "\033[0m", __FUNCTION__, ##__VA_ARGS__)
#else
  #define MEM_PROTECT_TRACE(...) do {} while(0)
#endif

static inline os::mem::Permission to_permission(int prot)
{
  static constexpr os::mem::Permission table[8] = {
    os::mem::Permission::Forbidden,   // 0: PROT_NONE
    os::mem::Permission::ReadOnly,    // 1: PROT_READ
    os::mem::Permission::WriteOnly,   // 2: PROT_WRITE
    os::mem::Permission::Data,        // 3: PROT_READ|PROT_WRITE
    os::mem::Permission::ExecuteOnly, // 4: PROT_EXEC
    os::mem::Permission::Code,        // 5: PROT_READ|PROT_EXEC
    os::mem::Permission::CodeBuffer,  // 6: PROT_WRITE|PROT_EXEC
    os::mem::Permission::Open,        // 7: PROT_READ|PROT_WRITE|PROT_EXEC
  };

  if (UNLIKELY(prot >= 8)) {
    throw std::invalid_argument("illegal protection flag");
  }

  return table[prot & 7];
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

