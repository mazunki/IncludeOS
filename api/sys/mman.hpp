/*
 * provides namespaced types for `sys_mman(0p)` values
 */
#ifndef	_SYS_MMAN_HPP
#define	_SYS_MMAN_HPP

#include "os.hpp"
#include <cstdint>
#include <sys/mman.h>
#include <util/bitops.hpp>
#include <type_traits>

namespace os::mem {
  // enum class Sharing : uint8_t {};  // TODO(mazunki): Flags::{SHARED,PRIVATE} are mutually exclusive

  enum class Flags : uint8_t {
    None      = 0,
    Shared    = MAP_SHARED,     // Changes are visible to other mappings of the same region, and file-backed allocations are valid
    Private   = MAP_PRIVATE,    // Creates a private copy-on-write mapping, does not write to underlying file. Changes to file after mmap is unspecified.
    Fixed     = MAP_FIXED,      // Place mapping specifically at `addr` (must be aligned, i.e. multiple of PAGE_SIZE)
    Anonymous = MAP_ANONYMOUS,  // Not backed by any file, content initialized to zero, requires: fd=-1, offset is ignored
  };

  enum class Permission : uint8_t {  // TODO(mazunki): consider making Permission::{Read,Write,Execute} private or standalone class
    Read    = PROT_READ,
    Write   = PROT_WRITE,
    Execute = PROT_EXEC,

    Data    = Read | Write,
    Code    = Read | Execute,

    Any     = 0,  // TODO(mazunki): this should really be R|W|X; but requires some refactoring
    RWX     = Read|Write|Execute,  // TODO(mazunki): temporary, remove me. references should use Permission::Any

    // None    = 0,  // TODO(mazunki): implement this after Any is properly implemented (to avoid confusion with old Access::none which had a different meaning). should block all access (best used for unmapped stuff, potentially tests)
  };
} // os::mmap


namespace util {
  inline namespace bitops {
    template<> struct enable_bitmask_ops<os::mem::Flags> {
      using type = std::underlying_type<os::mem::Flags>::type;
      static constexpr bool enable = true;
    };
  }

  inline namespace bitops {
    template<>
    struct enable_bitmask_ops<os::mem::Permission> {
      using type = typename std::underlying_type<os::mem::Permission>::type;
      static constexpr bool enable = true;
    };
  }
}
#endif // _SYS_MMAN_HPP
