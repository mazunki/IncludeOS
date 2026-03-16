#pragma once
#include <cstdint>
#include <type_traits>
#include <sys/mman.h>
#include <util/bitops.hpp>

namespace os::mem {
  enum class Access : uint8_t {
    none    = 0,
    read    = 1,
    write   = 2,
    execute = 4
  };

  namespace alloc {
    enum class Sharing : uint64_t {
      Anonymous     = MAP_ANONYMOUS,       // not backed by any file
      Shared        = MAP_SHARED,          // shared with other processes, changes are carried to underlying file
      Private       = MAP_PRIVATE,         // copy-on-write. (note: changes to underlying files post-mapping is unspecified)
      FixedOverride = MAP_FIXED,           // place mapping at address. underlying [data:data+length) is discarded
      FixedFriendly = MAP_FIXED_NOREPLACE  // same as fixed, but fails if region was occuppied
    };

    using Flags = Sharing;               // FIXME: ::Sharing should be its own type for exclusivity
    using Protection = os::mem::Access;  // HACK: works since we're assuming x86
  } // os::mem::alloc
} // os::mem


namespace util {
inline namespace bitops {
  template<>
  struct enable_bitmask_ops<os::mem::Access> {
    using type = std::underlying_type_t<os::mem::Access>;
    static constexpr bool enable = true;
  };

  template<>
  struct enable_bitmask_ops<os::mem::alloc::Flags> {
    using type = std::underlying_type_t<os::mem::alloc::Flags>;
    static constexpr bool enable = true;
  };

} // util::bitops
} // util

