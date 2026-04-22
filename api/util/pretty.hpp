#ifndef UTIL_PRETTY_HPP
#define UTIL_PRETTY_HPP

#include "mem/types.hpp"
#include <format>
#include <string>

namespace util::format {
  struct Bytes {
    std::size_t value;
    constexpr Bytes(std::size_t v) noexcept : value{v} {}
    constexpr explicit Bytes(os::mem::types::page_size_t v) noexcept : value{static_cast<os::mem::types::mem_size_t>(v)} {}
  };

  inline std::string to_human_size(std::size_t bytes) {
    constexpr std::string_view size_suffixes[] = {
      "B", "KiB", "MiB", "GiB", "TiB", "PiB"
    };
    double value = static_cast<double>(bytes);
    std::size_t exponent = 0;

    while (value >= 1024.0 && exponent + 1 < std::size(size_suffixes)) {
      value /= 1024.0;
      exponent++;
    }

    if (exponent == 0)
      return std::format("{} {}", static_cast<std::uint64_t>(value), size_suffixes[exponent]);
    else
      return std::format("{:.2f} {}", value, size_suffixes[exponent]);
  }

  inline std::string with_thousands_sep(uint64_t value, char sep = '_', char every = 3) {
      std::string s = std::to_string(value);
      for (int i = s.size() - every; i > 0; i -= every)
          s.insert(i, 1, sep);
      return s;
  }
}  // namespace util

template <>
struct std::formatter<util::format::Bytes> : std::formatter<std::string> {
  auto format(util::format::Bytes b, auto& ctx) const {
    return std::formatter<std::string>::format(
      util::format::to_human_size(b.value), ctx);
  }
};


namespace util::format {
  template <typename V>
  inline std::string kv(std::string_view key, const V& value) {
    return std::format("\033[96m{}\033[0m=\033[95m{}\033[0m", key, value);
  }

  template <typename V, typename A>
  inline std::string kva(std::string_view key, const V& value, const A& annotation) {
    return std::format("\033[96m{}\033[0m=\033[95m{}\033[0m \033[90m({})\033[0m", key, value, annotation);
  }
}
#define KV(expr)              util::format::kv(#expr, expr)
#define KVF(expr, fmt_str)    util::format::kv(#expr, std::format(fmt_str, expr))
#define KVE(name, val)        util::format::kv(name, val)
#define KVA(expr, annotation) util::format::kva(#expr, expr, annotation)

#endif // UTIL_PRETTY_HPP

