#pragma once
#include "util/bitops.hpp"
#include <cstddef>
#include <sys/mman.h>

namespace os::mem::types {
  struct mem_size_t {
    size_t value;
    constexpr mem_size_t(size_t v) noexcept : value{v} {}
    explicit constexpr operator size_t() const noexcept { return value; }
    constexpr bool operator==(const mem_size_t&) const noexcept = default;
    constexpr auto operator<=>(const mem_size_t&) const noexcept = default;
    constexpr mem_size_t operator+(mem_size_t rhs) const noexcept {
        return value + rhs.value;
    }
    constexpr mem_size_t& operator+=(mem_size_t& rhs) noexcept {
        value += rhs.value;
        return *this;
    }
    constexpr mem_size_t operator-(mem_size_t rhs) const noexcept {
        return value - rhs.value;
    }
    constexpr mem_size_t& operator-=(mem_size_t& rhs) noexcept {
        value -= rhs.value;
        return *this;
    }
    constexpr mem_size_t operator*(mem_size_t rhs) const noexcept {
        return value * rhs.value;
    }
    constexpr mem_size_t operator/(size_t rhs) const noexcept {
        return value / rhs;  // how big of chunks?
    }
    constexpr size_t operator/(mem_size_t rhs) const noexcept {
        return value / rhs.value;  // how many chunks?
    }
  };

  enum class mem_addr_kind { physical, virtual_ };

  template <mem_addr_kind Kind>
  struct mem_addr_t {
    uintptr_t value;
    constexpr mem_addr_t(uintptr_t v) noexcept : value{v} {}
    explicit constexpr operator uintptr_t() const noexcept { return value; }

    constexpr mem_addr_t operator+(mem_size_t offset) const noexcept {
        return mem_addr_t{value + offset.value};
    }
    constexpr mem_addr_t& operator+=(mem_size_t offset) noexcept {
        value += offset.value;
        return *this;
    }
    constexpr mem_addr_t operator-(mem_size_t offset) const noexcept {
        return value - offset.value;
    }
    constexpr mem_size_t operator-(mem_addr_t rhs) const noexcept {
        return value - rhs.value;
    }

    constexpr bool operator==(const mem_addr_t&) const noexcept = default;
    constexpr auto operator<=>(const mem_addr_t&) const noexcept = default;
  };

  using phys_addr_t = mem_addr_t<mem_addr_kind::physical>;
  using virt_addr_t = mem_addr_t<mem_addr_kind::virtual_>;
  using addr_t      = virt_addr_t;
} // namespace os::mem::types

namespace os::mem::types {
  struct page_size_t {
    mem_size_t value;

    explicit constexpr page_size_t(size_t v) noexcept : value{v} {}

    constexpr bool is_aligned(uintptr_t addr) const noexcept {
        return util::bits::align_down(addr, static_cast<size_t>(value)) == addr;
    }
    constexpr bool is_aligned(mem_size_t sz) const noexcept {
        return util::bits::align_down(static_cast<size_t>(sz), static_cast<size_t>(value)) == static_cast<size_t>(sz);
    }
    template<mem_addr_kind K>
    constexpr bool is_aligned(mem_addr_t<K> addr) const noexcept {
        return util::bits::align_down(addr.value, static_cast<size_t>(value)) == addr.value;
    }
    template<mem_addr_kind K>
    constexpr mem_addr_t<K> align_up(mem_addr_t<K> x) const noexcept {
        return util::bits::align_up(x.value, static_cast<size_t>(value));
    }
    template<mem_addr_kind K>
    constexpr mem_addr_t<K> align_down(mem_addr_t<K> x) const noexcept {
        return util::bits::align_down(x.value, static_cast<size_t>(value));
    }
    constexpr mem_size_t align_up(mem_size_t x) const noexcept {
        return util::bits::align_up(static_cast<uintptr_t>(x), static_cast<size_t>(value));
    }
    constexpr mem_size_t align_down(mem_size_t x) const noexcept {
        return util::bits::align_down(static_cast<uintptr_t>(x), static_cast<size_t>(value));
    }

    constexpr bool operator==(const page_size_t&) const noexcept = default;
    constexpr bool operator!=(const page_size_t&) const noexcept = default;
    constexpr auto operator<=>(const page_size_t&) const noexcept = default;

    // how many bytes is so many pages
    friend constexpr mem_size_t operator*(page_size_t lhs, size_t rhs) noexcept {
        return lhs.value * rhs;
    }
    friend inline constexpr mem_size_t operator*(size_t lhs, page_size_t rhs) noexcept {
        return rhs.value * lhs;
    }

    // how many pages in a memory region
    friend constexpr size_t operator/(mem_size_t size, page_size_t ps) noexcept {
        return size / ps.value;
    }

    // adding pages to pointers makes sense
    template <mem_addr_kind K>
    friend constexpr mem_addr_t<K> operator+(mem_addr_t<K> lhs, page_size_t rhs) noexcept {
        return lhs + rhs.value;
    }
    template <mem_addr_kind K>
    friend constexpr mem_addr_t<K>& operator+=(mem_addr_t<K>& lhs, page_size_t rhs) noexcept {
        return lhs += rhs.value;
    }
    // but not to sizes
    // friend constexpr size_t operator+(size_t, page_size_t) noexcept = delete;
    // friend constexpr size_t& operator+=(size_t&, page_size_t) noexcept = delete;

    explicit constexpr operator mem_size_t() const noexcept { return value; }
    constexpr size_t bytes() const noexcept { return static_cast<size_t>(value); }
  };

  inline constexpr page_size_t default_page_size { PAGE_SIZE };

  template <mem_addr_kind K>
  inline constexpr mem_addr_t<K> align_up(mem_addr_t<K> x, page_size_t ps = default_page_size) noexcept {
      return mem_addr_t<K>{ps.align_up(x.value)};
  }
  template <mem_addr_kind K>
  inline constexpr mem_addr_t<K> align_down(mem_addr_t<K> x, page_size_t ps = default_page_size) noexcept {
      return mem_addr_t<K>{ps.align_down(x.value)};
  }
  template <mem_addr_kind K>
  inline constexpr bool is_page_aligned(mem_addr_t<K> addr, page_size_t ps = default_page_size) noexcept {
      return ps.is_aligned(addr);
  }
} // namespace os::mem::types

namespace os::mem::types {
  constexpr mem_size_t operator+(mem_size_t lhs, page_size_t rhs) noexcept {
      return lhs + rhs.value;
  }
  constexpr mem_size_t& operator+=(mem_size_t& lhs, page_size_t rhs)  noexcept {
      return lhs += rhs.value;
  }
  constexpr bool operator==(mem_size_t lhs, page_size_t rhs) noexcept {
      return lhs.value == rhs.value.value;
  }
  constexpr bool operator==(page_size_t lhs, mem_size_t rhs) noexcept {
      return lhs.value.value == rhs.value;
  }
  constexpr auto operator<=>(mem_size_t lhs, page_size_t rhs) noexcept {
      return lhs.value <=> rhs.value.value;
  }
  constexpr auto operator<=>(page_size_t lhs, mem_size_t rhs) noexcept {
      return lhs.value.value <=> rhs.value;
  }
} // namespace os::mem::types

template <os::mem::types::mem_addr_kind K>
struct std::formatter<os::mem::types::mem_addr_t<K>> : std::formatter<uintptr_t> {
    auto format(os::mem::types::mem_addr_t<K> addr, auto& ctx) const {
        return std::formatter<uintptr_t>::format(addr.value, ctx);
    }
};

template <>
struct std::formatter<os::mem::types::mem_size_t> : std::formatter<size_t> {
    auto format(os::mem::types::mem_size_t sz, auto& ctx) const {
        return std::formatter<size_t>::format(sz.value, ctx);
    }
};

template <>
struct std::formatter<os::mem::types::page_size_t> : std::formatter<size_t> {
    auto format(os::mem::types::page_size_t ps, auto& ctx) const {
        return std::formatter<size_t>::format(ps.bytes(), ctx);
    }
};

consteval os::mem::types::page_size_t operator""_psz(unsigned long long x) noexcept {
    return os::mem::types::page_size_t{static_cast<size_t>(x)};
}
consteval os::mem::types::page_size_t operator""_KiB_psz(unsigned long long x) noexcept {
    return operator""_psz(x * 1024ULL);
}
consteval os::mem::types::page_size_t operator""_MiB_psz(unsigned long long x) noexcept {
    return operator""_psz(x * 1024ULL * 1024ULL);
}
consteval os::mem::types::page_size_t operator""_GiB_psz(unsigned long long x) noexcept {
    return operator""_psz(x * 1024ULL * 1024ULL * 1024ULL);
}
