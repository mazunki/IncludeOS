#pragma once
#include <cstdint>
#include <string_view>
#include <type_traits>
#include <sys/mman.h>
#include <util/bitops.hpp>
#include <format>

namespace os::mem {
  enum class Access : uint8_t {
    none    = 0,
    read    = 1,
    write   = 2,
    execute = 4
  };

  enum class Permission {
    // These permissions express kernel-level semantic intent
    //
    // Architectural backends are not required to represent them exactly, and
    // some architectures may collapse multiple semantic permissions into the
    // same hardware protection state.

    Forbidden,    // inaccessible
    ReadOnly,     // .rodata, constants, immutable pages
    WriteOnly,    // loggers, MMIO
    ExecuteOnly,  // executable bytecode
    Code,         // usually R+X
    Data,         // usually R+W
    CodeBuffer,   // usually W+X, remounted as Code when to be used
    Open,         // no restrictions
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
  } // os::mem::alloc
} // os::mem

template <>
struct std::formatter<os::mem::Permission> : std::formatter<std::string_view> {
    auto format(os::mem::Permission p, auto& ctx) const {
        using enum os::mem::Permission;
        std::string_view name;
        switch (p) {
            case Forbidden:   name = "Forbidden";   break;
            case ReadOnly:    name = "ReadOnly";     break;
            case WriteOnly:   name = "WriteOnly";    break;
            case ExecuteOnly: name = "ExecuteOnly";  break;
            case Code:        name = "Code";         break;
            case Data:        name = "Data";         break;
            case CodeBuffer:  name = "CodeBuffer";   break;
            case Open:        name = "Open";         break;
        }
        return std::formatter<std::string_view>::format(name, ctx);
    }
};

template <>
struct std::formatter<os::mem::alloc::Flags> : std::formatter<std::string_view> {
    auto format(os::mem::alloc::Flags f, auto& ctx) const {
        using enum os::mem::alloc::Sharing;
        using T = std::underlying_type_t<os::mem::alloc::Flags>;

        std::string result;
        auto append = [&](os::mem::alloc::Flags flag, std::string_view name) {
            if ((static_cast<T>(f) & static_cast<T>(flag)) == static_cast<T>(flag)) {
                if (!result.empty()) result += '|';
                result += name;
            }
        };
        append(Anonymous,     "Anonymous");
        append(Shared,        "Shared");
        append(Private,       "Private");
        append(FixedOverride, "FixedOverride");
        append(FixedFriendly, "FixedFriendly");
        if (result.empty()) result = "none";
        return std::formatter<std::string_view>::format(result, ctx);
    }
};

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

