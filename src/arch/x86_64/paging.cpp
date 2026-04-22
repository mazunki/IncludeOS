// -*-C++-*-
// This file is a part of the IncludeOS unikernel - www.includeos.org
//
// Copyright 2017 IncludeOS AS, Oslo, Norway
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
#include <os>
#include <arch/x86/paging.hpp>
#include <arch/x86/paging_utils.hpp>
#include <kernel/cpuid.hpp>
#include <kprint>

// #define DEBUG_X86_PAGING
#ifdef DEBUG_X86_PAGING
#define MEM_PRINT(X, ...) kprintf("<Paging> " X, __VA_ARGS__)
#else
#define MEM_PRINT(X, ...) kprintf("<Paging> " X, __VA_ARGS__)
#endif

using namespace os::mem;
using namespace util;

using Flags = x86::paging::Flags;
using Pml4  = x86::paging::Pml4;

static void allow_executable();

// TODO: -Wunused-function
// static void protect_pagetables_once();

// must be public symbols because of a unittest
#ifndef PLATFORM_UNITTEST
extern char _TEXT_START_;
extern char _EXEC_END_;
uintptr_t __exec_begin = (uintptr_t)&_TEXT_START_;
uintptr_t __exec_end = (uintptr_t)&_EXEC_END_;
#else
extern uintptr_t __exec_begin;
extern uintptr_t __exec_end;
#endif

/**
    IncludeOS default paging setup

    PML4 (512 * 512GB entries):
    - All 512 pointers allocated to prevent CPU reading garbage.
    - Entry 0 present, all other entries not present
    - Cost: ~1 page.

    PML3 (512 * 1GB entries):
    - 1 instances allocated (pointed to by PML4[0])
    - Entry 0 pointing to a PML2
    - All other entries identity mapped as present / writable / no_execute
    - Cost: ~1 page

    PML2 (512 * 2MB entries):
    - 1 instance allocated, 512 2MB entries covering the first GB
    - Entry 0 points to 4k page table for fine grained mapping of first 2MB
    - All other (1GB total) identity mapped as present / writable / no_execute
    - Cost: ~1 page

    PML1 (4k page table):
    - 1 instance for the first 2 MB (e.g. to protect the first 4k page)
    - instances as needed for covering the executable
    - Cost: 1 page + 1 page per 2 MB of executable

    Further instances dynamically allocated as needed, e.g. by calls to mem::map
**/

// The main page directory pointer
Pml4* __pml4;

__attribute__((weak))
void __arch_init_paging() {
  INFO("x86_64", "Initializing paging");
  auto default_fl = Flags::present | Flags::writable | Flags::huge | Flags::no_exec;
  __pml4 = new Pml4(0);
  Expects(__pml4 != nullptr);
  Expects(!__pml4->has_flag(0, Flags::present));

  INFO2("* Supported page sizes: %s", supported_page_sizes().to_string().c_str());

  INFO2("* Adding 512 1GiB entries @ 0x0 -> 0x%llx", 512_GiB);
  auto* pml3_0 =  __pml4->create_page_dir(0, 0, default_fl);

  Expects(pml3_0 != nullptr);
  Expects(pml3_0->has_flag(0,Flags::present));

  Expects(__pml4->has_flag(0, Flags::present));
  /*FIXME TODO this test is not sane when we do not control the heap in unittests
  Expects(__pml4->has_flag((uintptr_t)pml3_0, Flags::present));
  */

  if (not os::mem::supported_page_size(1_GiB_psz)) {
    auto first_range = __pml4->map_r({0,0,default_fl, 16_GiB});
    Expects(first_range && first_range.size >= 16_GiB);
    INFO2("* Identity mapping %s", first_range.to_string().c_str());
  }

  INFO2("* Marking page 0 as not present");
  auto zero_page = __pml4->map_r({0, 0, Flags::none, 4_KiB, page_sizes_t{4_KiB}});
  Expects(__pml4->has_flag(8_KiB, Flags::present | Flags::writable | Flags::no_exec));
  allow_executable();

  Expects(zero_page.size == 4_KiB);
  Expects(zero_page.page_sizes.contains(4_KiB_psz));
  Expects(__pml4->active_page_size(0LU) == 4_KiB);
  Expects(! __pml4->has_flag(0, Flags::present));

  Expects(! __pml4->has_flag((uintptr_t)__exec_begin, Flags::no_exec));
  Expects(__pml4->has_flag((uintptr_t)__exec_begin, Flags::present));

  // hack to see who overwrites the pagetables
  //protect_pagetables_once();

  INFO2("* Passing page tables to CPU");
  extern void __x86_init_paging(void*);
  __x86_init_paging(__pml4->data());
}

namespace x86 {
namespace paging {

inline os::mem::Permission to_permission(x86::paging::Flags flags)
{
  using Perm = os::mem::Permission;
  using Arch = x86::paging::Flags;

  if (util::bitops::missing_flag(flags, Arch::present)) {
    return Perm::Forbidden;
  }

  const bool writable = has_flag(flags, Arch::writable);
  const bool executable = missing_flag(flags, Arch::no_exec);

  if (writable && executable) {
    return Perm::CodeBuffer;
  }

  if (writable) {
    return Perm::Data;
  }

  if (executable) {
    return Perm::Code;
  }

  return Perm::ReadOnly;
}

inline x86::paging::Flags to_x86(os::mem::Permission perm)
{
  using Perm = os::mem::Permission;
  using Arch = x86::paging::Flags;

  switch (perm) {
    case Perm::Forbidden:
      return Arch::none;

    case Perm::ReadOnly:
      return Arch::present | Arch::no_exec;

    case Perm::WriteOnly:
      // x86 has no write-only page permission; writable pages are also readable
      return Arch::present | Arch::writable | Arch::no_exec;

    case Perm::ExecuteOnly:
      // x86 has no execute-only page permission; executable pages are also readable
      return Arch::present;

    case Perm::Code:
      return Arch::present;

    case Perm::Data:
      return Arch::present | Arch::writable | Arch::no_exec;

    case Perm::CodeBuffer:
      return Arch::present | Arch::writable;

    case Perm::Open:
      return Arch::present | Arch::writable;
  }

  __builtin_unreachable();
}

__attribute__((weak))
void invalidate(void *pageaddr){
  asm volatile("invlpg (%0)" ::"r" (pageaddr) : "memory");
}

}} // x86::paging

namespace os {
namespace mem {

using Map_x86 = Mapping<x86::paging::Flags>;

#define mem_fail_fast(reason)                                               \
  throw Memory_exception(std::string(__FILE__) + ":" + std::to_string(__LINE__) \
                         + ": " + reason)

} // mem
} // os

using namespace os;

page_sizes_t mem::supported_page_sizes() {
  static size_t res = 0;
  if (res == 0) {
    res = 4_KiB | 2_MiB;
    if (CPUID::has_feature(CPUID::Feature::PDPE1GB))
      res |= 1_GiB;
  }
  return page_sizes_t{res};
}

bool mem::supported_page_size(page_size_t size)
{
  return bits::is_pow2(size.value.value) and (supported_page_sizes().contains(size)) != 0;
}

Map to_mmap(Map_x86 map){
  Permission perms = x86::paging::to_permission(map.attrs);
  return {map.lin, map.phys, perms, map.size, map.page_sizes};
}

Map_x86 to_x86(Map map){
  Flags flags = x86::paging::to_x86(map.attrs);
  return {map.lin, map.phys, flags, map.size, map.page_sizes};
}

phys_addr_t mem::virt_to_phys(virt_addr_t linear)
{
  auto* ent = __pml4->entry_r(linear);
  if (ent == nullptr) return phys_addr_t{0};
  return phys_addr_t{ent->addr_bits()};
}

Permission mem::protect_page(virt_addr_t linear, Permission flags)
{
  MEM_PRINT("::protect_page 0x%lx\n", linear.value);
  x86::paging::Flags xflags = x86::paging::to_x86(flags);
  auto f = __pml4->set_flags_r(linear, xflags);
  x86::paging::invalidate(reinterpret_cast<void*>(linear.value));
  return x86::paging::to_permission(f);
};

Permission mem::protect_range(virt_addr_t linear, Permission flags)
{
  MEM_PRINT("::protect 0x%lx \n", linear.value);
  x86::paging::Flags xflags = x86::paging::to_x86(flags);

  auto key = os::mem::vmmap().in_range(linear.value);

  // Throws if entry wasn't previously mapped.
  auto map_ent = os::mem::vmmap().at(key);

  MEM_PRINT("Found entry: %s\n", map_ent.to_string().c_str());
  mem_size_t sz_prot = 0;
  x86::paging::Flags fl{};

  // TOOD: Optimize. No need to re-traverse for each page
  //       set_flags_r should probably just take size.
  while (sz_prot < map_ent.size()){
    auto page = linear + sz_prot;
    auto psize = active_page_size(page);
    MEM_PRINT("Protecting page 0x%lx", page.value);
    fl |= __pml4->set_flags_r(page, xflags);
    sz_prot += psize;
    x86::paging::invalidate(reinterpret_cast<void*>(page.value));
  }
  return x86::paging::to_permission(fl);
};

Map mem::protect(virt_addr_t linear, mem_size_t len, Permission perms)
{
  if (UNLIKELY(len < supported_page_sizes().min()))
    mem_fail_fast("Can't map less than a page");

  if (UNLIKELY(linear == 0))
    mem_fail_fast("Can't map to address 0");

  MEM_PRINT("::protect 0x%lx \n", linear.value);
  auto key = os::mem::vmmap().in_range(linear.value);
  if (key == 0)
    mem_fail_fast("Can't protect unmapped / untracked range");

  MEM_PRINT("Found key: 0x%zx\n", key);

  // Throws if entry wasn't previously mapped.
  const Fixed_memory_range& map_ent = os::mem::vmmap().at(key);
  MEM_PRINT("Found entry: %s\n", map_ent.to_string().c_str());

  Flags flags = x86::paging::to_x86(perms);
  phys_addr_t physical = x86::paging::any_addr;

  x86::paging::Map m{linear, physical, flags, len};
  MEM_PRINT("Wants: %s \n", m.to_string().c_str());

  Expects(m);
  auto res = __pml4->map_r(m);
  return to_mmap(res);
}

Map mem::protect_existing(virt_addr_t linear, mem_size_t len, Permission perms)
{
  if (linear == 0)
    mem_fail_fast("Can't protect address 0");

  const page_size_t page_sz = supported_page_sizes().min();
  if (!page_sz.is_aligned(linear))
    mem_fail_fast("Linear address must be page-aligned");

  len = page_sz.align_up(len);

  Flags xflags = x86::paging::to_x86(perms);

  mem_size_t done{0};
  while (done < len) {
    virt_addr_t page = linear + done;
    page_size_t psize = active_page_size(page);

    if (psize == 0_psz)
      mem_fail_fast("Tried to protect unmapped page");

    __pml4->set_flags_r(page, xflags);
    x86::paging::invalidate(reinterpret_cast<void*>(page.value));

    const size_t page_bytes = psize.bytes();
    const auto offset_in_page = page.value & (page_bytes - 1);
    mem_size_t step = std::min<mem_size_t>(page_bytes - offset_in_page, len - done);

    done += step;
  }

  return {linear, virt_to_phys(linear), perms, len, page_sizes_t{page_sz.bytes()}};
}

Permission mem::permissions(virt_addr_t addr)
{
  return x86::paging::to_permission(__pml4->flags_r(addr));
}

__attribute__((weak))
Map mem::map(Map m, const char* name)
{
  using namespace x86::paging;
  using namespace util;
  MEM_PRINT("Wants : %s size: %li \n", m.to_string().c_str(), m.size.value);

  if (UNLIKELY(!m))
    mem_fail_fast("Provided map was empty");

  if (UNLIKELY(m.lin == virt_addr_t{0} or m.phys == phys_addr_t{0}))
    mem_fail_fast("Can't map the 0-page");

  if (UNLIKELY(!supported_page_sizes().min().is_aligned(m.lin)))
    mem_fail_fast("linear must be aligned to supported page size");

  if (UNLIKELY(!supported_page_sizes().min().is_aligned(m.phys)))
    mem_fail_fast("physical must be aligned to supported page size");

  if (UNLIKELY(!m.min_psize().is_aligned(m.lin)))
    mem_fail_fast("linear must be aligned to requested page size");

  if (UNLIKELY(!m.min_psize().is_aligned(m.phys)))
    mem_fail_fast("physical must be aligned to requested page size");

if (UNLIKELY(!m.page_sizes.intersects(os::mem::supported_page_sizes())))
    mem_fail_fast("Requested page size(s) not supported");

  if (std::strcmp(name, "") == 0)
    name = "mem::map";

  // Align size to minimal page size;
  const mem_size_t rounded_size{
    util::bits::roundto(m.min_psize().bytes(), m.size.value)
  };
  const virt_addr_t req_addr_end = m.lin + rounded_size - mem_size_t{1};

  os::mem::vmmap().assign_range({m.lin.value, req_addr_end.value, name});

  auto new_map = __pml4->map_r(to_x86(m));
  if (new_map) {
    MEM_PRINT("Gets  : %s size: %li\n", new_map.to_string().c_str(), new_map.size.value);

    // Size should match requested size rounded up to smallest requested page size
    Ensures(new_map.size == rounded_size);
    Ensures(to_permission(new_map.attrs) == m.attrs);
    Ensures(bits::is_aligned<4_KiB>(new_map.page_sizes.mask));
  }

  return to_mmap(new_map);
};

Map mem::unmap(virt_addr_t lin){
  auto key = os::mem::vmmap().in_range(lin.value);
  Map_x86 m;
  if (key) {
    MEM_PRINT("mem::unmap %p \n", reinterpret_cast<void*>(lin.value));
    auto map_ent = os::mem::vmmap().at(key);
    m.lin = lin;
    m.phys = 0;
    m.size = map_ent.size();

    m = __pml4->map_r({key, 0, x86::paging::to_x86(Permission::Forbidden), (size_t)map_ent.size()});

    Ensures(m.size == util::bits::roundto<4_KiB>(map_ent.size()));
    os::mem::vmmap().erase(key);
  }

  return to_mmap(m);
}

page_size_t mem::active_page_size(virt_addr_t addr){
  return __pml4->active_page_size(addr);
}

void allow_executable()
{
  // this sets the region where the unikernel's executable code is
  // loaded into by the linker (src/arch/x86_64/linker.ld) as executable

  INFO2("* Allowing execute on %p -> %p",
        (void*) __exec_begin, (void*)__exec_end);

  Expects(bits::is_aligned<4_KiB>(__exec_begin));
  Expects(__exec_end > __exec_begin);

  auto exec_size = __exec_end - __exec_begin;

  os::mem::Map m;
  m.lin        = __exec_begin;
  m.phys       = __exec_begin;
  m.size       = exec_size;
  m.page_sizes = os::mem::supported_page_sizes();
  m.attrs      = os::mem::Permission::Code;

  os::mem::map(m, "ELF Executable (Unikernel service)");
}

/* TODO: Compiler warning unused
  void protect_pagetables_once()
{
  struct ptinfo {
    void*  addr;
    size_t len;
  };
  std::vector<ptinfo> table_entries;

  __pml4->traverse(
    [&table_entries] (void* ptr, size_t len) {
      table_entries.push_back({ptr, len});
    });


  for (auto it = table_entries.rbegin(); it != table_entries.rend(); ++it)
  {
    const auto& entry = *it;
    const x86::paging::Map m {
        (uintptr_t) entry.addr, x86::paging::any_addr,
        x86::paging::Flags::present, static_cast<size_t>(entry.len)
      };
    assert(m);
    MEM_PRINT("Protecting table: %p, size %zu\n", entry.addr, entry.len);
    __pml4->map_r(m);
  }
}
*/
