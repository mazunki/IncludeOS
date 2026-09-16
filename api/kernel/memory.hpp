// -*- C++ -*-
// This file is a part of the IncludeOS unikernel - www.includeos.org
//
// Copyright 2017 Oslo and Akershus University College of Applied Sciences
// and Alfred Bratterud
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

#ifndef KERNEL_MEMORY_HPP
#define KERNEL_MEMORY_HPP

#include "util/pretty.hpp"
#include <util/bitops.hpp>
#include <util/units.hpp>
#include <mem/alloc/buddy.hpp>
#include <mem/allocator.hpp>
#include <mem/flags.hpp>
#include <mem/types.hpp>
#include <sstream>
#include <expects>
#include <kernel/memmap.hpp>
#include <fmt/format.h>
#include <fmt/std.h>


namespace os::mem {
  struct page_sizes_t {
    uintptr_t mask = 0; // bitmask of allowed page sizes

    constexpr bool empty() const {
      return mask == 0;
    }

    constexpr bool some() const {
      return mask != 0;
    }

    constexpr bool intersects(page_sizes_t rhs) const {
      return (mask & rhs.mask) != 0;
    }

    constexpr bool contains(types::page_size_t ps) const {
      return (mask & ps.bytes()) != 0;
    }

    constexpr types::page_size_t min() const {
      return static_cast<types::page_size_t>(util::bits::keepfirst(mask));
    }

    constexpr types::page_size_t max() const {
      return static_cast<types::page_size_t>(util::bits::keeplast(mask));
    }

    constexpr bool operator==(const page_sizes_t&) const noexcept = default;
    constexpr bool operator!=(const page_sizes_t&) const noexcept = default;

    constexpr page_sizes_t operator|(page_sizes_t rhs) const {
      return { mask | rhs.mask };
    }

    constexpr page_sizes_t& operator|=(page_sizes_t rhs) {
      mask |= rhs.mask;
      return *this;
    }

    std::string to_string() const {
      using namespace util::literals;
      if (mask == 0) return "None";

      std::string out;
      uintptr_t bits = mask;
      while (bits) {
        auto ps = util::bits::keepfirst(bits);
        bits &= ~ps;
        out += util::Byte_r(ps).to_string();
        if (bits) out += ", ";
      }
      return out;
    }

    constexpr types::page_size_t min()
    {
      size_t res = util::bits::keepfirst(mask);
      return static_cast<types::page_size_t>(res);
    }

    constexpr types::page_size_t max()
    {
      size_t res = util::bits::keeplast(mask);
      return static_cast<types::page_size_t>(res);
    }

  };

  /** Get bitfield with bit set for each supported page size */
  page_sizes_t supported_page_sizes();

  inline constexpr size_t min_psize() {
    return supported_page_sizes().min().bytes();
  }

  inline constexpr size_t max_psize() {
    return supported_page_sizes().max().bytes();
  }

  /** Determine if size is a supported page size */
  bool supported_page_size(types::page_size_t size);
} // os::mem

namespace os::mem {
  using namespace types;
  /**
   * Generic virtual-to-physical memory mapping description.
   *
   * Used as a building block for virtual memory APIs and backend-specific
   * mappings.
   *
   * This is a lightweight value type that does not enforce invariants.
   * In particular, page size restrictions, alignment, and attribute validity
   * are not checked at construction time: they are merely described
   *
   * Attributes are additional information passed to the mapping: different
   * mappings/subsystems will want to store different information here
   */
  template <typename Attributes>
  struct Mapping
  {
    virt_addr_t lin  = 0;
    phys_addr_t phys = 0;
    Attributes attrs {};
    mem_size_t size = 0;

    // Bitmask of allowed page sizes for this mapping
    //   - `any_size` means no restriction
    //   - `0` is invalid
    page_sizes_t page_sizes;


    // empty or invalid mapping sentinel
    constexpr Mapping() = default;

    /** Construct with no page size restrictions */
    constexpr Mapping(virt_addr_t linear, phys_addr_t physical, Attributes attributes, mem_size_t sz) noexcept
      : lin{linear}, phys{physical}, attrs{attributes}, size{sz}, page_sizes{supported_page_sizes()} {}

    /** Construct with explicit allowed page size mask */
    constexpr Mapping(virt_addr_t linear, phys_addr_t physical, Attributes attributes, mem_size_t sz, page_sizes_t psz) noexcept
      : lin{linear}, phys{physical}, attrs{attributes}, size{sz}, page_sizes{psz} {}


    /** Truthy if non-empty and has page-size constraints */
    [[nodiscard]] constexpr explicit operator bool() const noexcept {
      return size != 0 && !page_sizes.empty();
    }

    /** Empty / invalid sentinel */
    [[nodiscard]] constexpr bool empty() const noexcept {
      return size == 0 || page_sizes.empty();
    }

    [[nodiscard]] constexpr bool operator==(const Mapping&) const noexcept = default;
    [[nodiscard]] constexpr bool operator!=(const Mapping&) const noexcept = default;

    /** end of linear range (exclusive) */
    [[nodiscard]] constexpr virt_addr_t lin_end() const noexcept {
      return lin + size;
    }

    /** end of physical range (exclusive) */
    [[nodiscard]] constexpr phys_addr_t phys_end() const noexcept {
      return phys + size;
    }

    /** smallest allowed page size */
    [[nodiscard]] constexpr types::page_size_t min_psize() const noexcept {
      return page_sizes.min();
    }

    /** largest allowed page size */
    [[nodiscard]] constexpr types::page_size_t max_psize() const noexcept {
      return page_sizes.max();
    }

    /** linear adjacency check */
    [[nodiscard]] constexpr bool contiguous_with(const Mapping& rhs) const noexcept {
      return (this->lin_end() == rhs.lin) || (rhs.lin_end() == this->lin);
    }

    /** physical adjacency check */
    [[nodiscard]] constexpr bool physically_contiguous_with(const Mapping& rhs) const noexcept {
      return (this->phys_end() == rhs.phys) || (rhs.phys_end() == this->phys);
    }

    /** attribute equality */
    [[nodiscard]] constexpr bool same_attrs_as(const Mapping& rhs) const noexcept {
      return attrs == rhs.attrs;
    }

    [[nodiscard]] static constexpr Mapping merge_maps(const Mapping& lhs, const Mapping& rhs) noexcept {
      if (!rhs) return lhs;
      if (!lhs) return rhs;

      if (!lhs.contiguous_with(rhs)) {
        std::printf("WARNING: os:mem::merge_maps: failed to merge non-contiguous linear mappings!\n");
        return {};
      }
      if (!lhs.physically_contiguous_with(rhs)) {
        // without this, we can't rely on the size to know the physical ending
        // std::printf("WARNING: os:mem::merge_maps: merging non-contiguous physical mappings!\n");
        // return {};
      }
      if (!lhs.same_attrs_as(rhs)) {
        std::printf("WARNING: os:mem::merge_maps: failed to merge mappings with different attributes!\n");
        return {};
      }

      Mapping m = {
        std::min(lhs.lin, rhs.lin),
        std::min(lhs.phys, rhs.phys),
        lhs.attrs,
        lhs.size + rhs.size,
        lhs.page_sizes | rhs.page_sizes
      };

      // std::println("merged <{}> and <{}> to <{}>", lhs.to_string(), rhs.to_string(), m.to_string());

      return m;
    }

    [[nodiscard]] constexpr Mapping merge_with(const Mapping& rhs) const noexcept {
      return merge_maps(*this, rhs);
    }

    std::string to_string() const;
  };
} // os::mem


namespace os::mem {
  using Raw_allocator = os::mem::mem_resource;

  /** Get default allocator for untyped allocations */
  os::mem::mem_resource& raw_allocator();

  template <typename T>
  using Typed_allocator = Allocator<T, os::mem::mem_resource>;

  /** Get default std::allocator for typed allocations */
  template <typename T>
  Typed_allocator<T> system_allocator() { return Typed_allocator<T>(raw_allocator()); }

  using Map = Mapping<Permission>;

  /** Exception class possibly used by various ::mem functions. **/
  class Memory_exception : public std::runtime_error
  { using runtime_error::runtime_error; };


  /**
   * Map linear address to physical memory, according to provided Mapping.
   * Provided map.page_size will be ignored, but the returned map.page_size
   * will have one bit set for each page size used
   */
  Map map(Map, const char* name = "mem::map");

  /**
   * Unmap the memory mapped by linear address,
   * effectively freeing the underlying physical memory.
   * The behavior is undefined if addr was not mapped with a call to map
   **/
  Map unmap(virt_addr_t addr);

  /** Get permission flags for page enclosing a given address */
  Permission permissions(virt_addr_t addr);

  /** Determine active page size of a given linear address **/
  page_size_t active_page_size(virt_addr_t addr);
  inline page_size_t active_page_size(void* addr) { return active_page_size(virt_addr_t{reinterpret_cast<uintptr_t>(addr)}); }


  /**
   * Set and return access flags for a given linear address range.
   * The range must be a subset of a range mapped by a previous call to map.
   * The page sizes will be adjusted to match len as closely as possible,
   * creating new page tables as needed.
   * Uniform page sizes across the range is not guaranteed unless the enclosing
   * range was mapped with a page size restriction. E.g. A len of 2MiB + 4KiB
   * might result in 513 4KiB pages or 1 2MiB page and 1 4KiB page getting
   * protected.
   **/
  Map protect(virt_addr_t linear, mem_size_t len, Permission perms = Permission::ReadOnly);

  /**
   * Set and return access flags for a given linear address range
   * The range is expected to be mapped by a previous call to map.
   **/
  Permission protect_range(virt_addr_t linear, Permission perms = Permission::ReadOnly);

  /**
   * Set and return permission flags for a page starting at linear.
   * @note : the page size can be any of the supported sizes and
   *         protection will apply for that whole page.
   **/
  Permission protect_page(virt_addr_t linear, Permission perms = Permission::ReadOnly);

  Map protect_existing(virt_addr_t linear, mem_size_t len, Permission perms = Permission::ReadOnly);

  /** Get the physical address to which linear address is mapped **/
  phys_addr_t virt_to_phys(virt_addr_t linear);

  void virtual_move(virt_addr_t src, mem_size_t size, virt_addr_t dst, const char* label);

  /** Virtual memory map **/
  inline Memory_map& vmmap() {
    // TODO Move to machine
    static Memory_map memmap;
    return memmap;
  };

  bool heap_ready();

} // os::mem


namespace os::mem {
  template <typename Attributes>
  inline std::string Mapping<Attributes>::to_string() const
  {
    fmt::basic_memory_buffer<char, 1024> buf;
    std::format_to(std::back_inserter(buf), "{:#x} -> {:#x}, size {}, attrs {:#x}", lin, phys, size, static_cast<unsigned long long>(attrs));

    const bool is_single_psize = util::bits::popcount(page_sizes.mask) == 1;

    if (is_single_psize) {
      std::format_to(std::back_inserter(buf), " ({} pages of {})", size / page_sizes.min(), page_sizes.min().bytes());
    } else {
      fmt::format_to(std::back_inserter(buf), " (page sizes: {})", page_sizes.to_string());
    }

    return std::string(buf.data(), buf.size());
  }

  inline void
  virtual_move(uintptr_t src, size_t size, uintptr_t dst, const char* label)
  {
    // note that this assumes that the source mapping has uniform permissions
    const Permission perms = os::mem::permissions(src);

    // setup @dst as new virt area for @src
    os::mem::map({dst, src, perms, size}, label);

    // invalidate @src
    os::mem::protect(src, size, os::mem::Permission::Forbidden);
  }
}


#endif
