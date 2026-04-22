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

#pragma once

#include <util/bitops.hpp>
#include <util/units.hpp>
#include <mem/paging.hpp>

//#define DEBUG_X86_PAGING
#ifdef DEBUG_X86_PAGING
#define PG_PRINT(X, ...) printf("<%s @ 0x%lx>" X, pg_name(page_size), start_addr(), ##__VA_ARGS__)
#else
#define PG_PRINT(X, ...) /* X */
#endif

inline const char* pg_name(os::mem::types::page_size_t psz){
  using namespace util::literals;
  switch (psz.bytes()) {
  case 4_KiB:
    return "PML1(4KiB)";
  case 2_MiB:
    return "PML2(2MiB)";
  case 1_GiB:
    return "PML3(1GiB)";
  case 512_GiB:
    return "PML4";
  default:
    return "N/A";
  }
};

namespace x86::paging {
using namespace util::literals;
using namespace os::mem::types;

/**
 * x86 page entry flags.
 * Intel manual Vol.3A 4-28
 **/
enum class Flags : uintptr_t {
  none       = 0,
  present    = 1 << 0,
  writable   = 1 << 1,
  user       = 1 << 2,
  write_thr  = 1 << 3,
  cache_dis  = 1 << 4,
  accessed   = 1 << 5,
  dirty      = 1 << 6,
  huge       = 1 << 7,
  global     = 1 << 8,
  pdir       = 1ULL << 52,  // Using ignored bit 52 as pdir marker
  ign_53     = 1ULL << 53,
  ign_54     = 1ULL << 54,
  ign_55     = 1ULL << 55,
  ign_56     = 1ULL << 56,
  ign_57     = 1ULL << 57,
  ign_58     = 1ULL << 58,
  pky_59     = 1ULL << 59,
  pky_60     = 1ULL << 60,
  pky_61     = 1ULL << 61,
  pky_62     = 1ULL << 62,
  no_exec    = 1ULL << 63,

  // Flag groups
  all = (0xfff | pdir | no_exec),
  permissive = (present | writable)
};

using Map = os::mem::Mapping<Flags>;

/** x86_64 specific types for page directories **/
struct x86_64 {
  using Flags = x86::paging::Flags;
  static constexpr page_size_t min_pagesize     = 4_KiB_psz;
  static constexpr size_t      table_size       = 512;
  static constexpr page_size_t max_pagesize     = 1_GiB_psz;
  static constexpr uintptr_t   max_memory = 512_GiB * 512;
};

using os::mem::any_addr;

}

/** Enable bitmask operators for paging flags **/
namespace util {
inline namespace bitops {
template<>
struct enable_bitmask_ops<x86::paging::Flags> {
  using type = std::underlying_type<x86::paging::Flags>::type;
  static constexpr bool enable = true;
};

template<>
struct enable_bitmask_ops<decltype(4_KiB)> {
  using type = decltype(4_KiB);
  static constexpr bool enable = true;
};
}
}

namespace x86::paging {
using namespace util::literals;
using namespace util::bitops;

/** Conversion from x86 paging flags to os::mem::Permission **/
os::mem::Permission to_permission(Flags f);

/** Conversion from mem::Permission flags to x86 paging flags **/
Flags to_x86(os::mem::Permission prot);

/** Summary of currently mapped page- and page directories **/
struct Summary {
  int pages_4k = 0;
  int pages_2m = 0;
  int pages_1g = 0;
  int dirs_2m = 0;
  int dirs_1g = 0;
  int dirs_512g = 0;

  void add_dir(page_size_t ps){
    switch (static_cast<uintptr_t>(ps.bytes())) {
    case 2_MiB:
      dirs_2m++;
      break;
    case 1_GiB:
      dirs_1g++;
      break;
    case 512_GiB:
      dirs_512g++;
    }
  }

  void add_page(size_t ps){
    switch (ps) {
    case 4_KiB:
        pages_4k++;
        break;
    case 2_MiB:
      pages_2m++;
      break;
    case 1_GiB:
      pages_1g++;
      break;
    }
  }

  Summary operator+=(Summary rhs){
    pages_4k   += rhs.pages_4k;
    pages_2m   += rhs.pages_2m;
    pages_1g   += rhs.pages_1g;
    dirs_2m    += rhs.dirs_2m;
    dirs_1g    += rhs.dirs_1g;
    dirs_512g  += rhs.dirs_512g;
    return *this;
  }
};

inline std::ostream& operator<<(std::ostream& out, const Summary& sum){
  out << "Pml4 (512 * 512 GiB) \n"
      << "\t* Mapped 512 GiB page dirs : " << sum.dirs_512g << "\n"
      << "Pml3 (512 GiB) \n"
      << "\t* Mapped 1G page dirs      : " << sum.dirs_1g << "\n"
      << "\t* Mapped 1G pages          : " << sum.pages_1g << "\n"
      << "Pml2 (1 GiB) \n"
      << "\t* Mapped 2 MiB page dirs   : " << sum.dirs_2m << "\n"
      << "\t* Mapped 2 MiB pages       : " << sum.pages_2m << "\n"
      << "Pml1 (2 MiB) \n"
      << "\t* Mapped 4 KiB pages       : " << sum.pages_4k << "\n";
  return out;
}

/**
 * x86 4-level page table hierarchy.
 * Pml1 is the leaf (4KiB pages); Pml2/3/4 are interior page directories.
 **/
using Pml1 = os::mem::PageTable<x86_64, Flags::all & ~(Flags::huge | Flags::pdir)>;
using Pml2 = os::mem::PageDirectory<x86_64, page_size_t{Pml1::range_size.value}, Pml1>;
using Pml3 = os::mem::PageDirectory<x86_64, page_size_t{Pml2::range_size.value}, Pml2>;
using Pml4 = os::mem::PageDirectory<x86_64, page_size_t{Pml3::range_size.value}, Pml3,
                                    Flags::all & ~Flags::huge>;


/** Invalidate page (e.g. flush TLB entry) **/
void invalidate(void *pageaddr);

} // namespace x86::paging
