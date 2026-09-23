#pragma once
#include <memory_resource>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string_view>
#include <bit>
#include <cstring>
#include <algorithm>
#include <new>
#include <print>
#include <type_traits>
#ifdef INCLUDEOS_SMP_ENABLE
#include <smp>
#include <mutex>
#endif

namespace os::mem {

// rounds p up to the nearest multiple of alignment (which must be a power of two)
inline constexpr std::uintptr_t align_up(std::uintptr_t p, std::size_t alignment) noexcept {
  const std::uintptr_t a = static_cast<std::uintptr_t>(alignment);
  return (p + (a - 1)) & ~(a - 1);
}

struct mem_stats {
  std::size_t total_bytes{};          // total capacity of the allocator
  std::size_t busy_bytes{};           // including overhead of the underlying implementation
  std::size_t peak_busy_bytes{};

  std::size_t requested_bytes{};       // sum of all requested bytes
  std::size_t peak_requested_bytes{};

  // uint64_t should be big enough. could also use uintmax_t, but this could
  // cause unpredictable behaviour on specific architectures
  std::uint64_t count_alloc{};
  std::uint64_t count_alloc_at{};
  std::uint64_t count_dealloc{};
  std::uint64_t count_realloc{};
  std::uint64_t count_expand{}; // only counts successful in-place expansions
  std::uint64_t count_shrink{};
  std::uint64_t count_allocate_all{};
  std::uint64_t count_allocate_largest{};
};

struct mem_region {
  std::uintptr_t start;
  std::uintptr_t end; // one-past

  std::size_t size() const noexcept { return end - start; }

  std::byte* start_ptr() const noexcept { return reinterpret_cast<std::byte*>(start); }
  std::byte* end_ptr()   const noexcept { return reinterpret_cast<std::byte*>(end); }

  bool owns(std::uintptr_t addr) const noexcept {
    return addr >= start && addr < end;
  }
};

struct mem_config {
  mem_region region;
  bool overbooking{false};
};

class allocator_error : public std::bad_alloc {
public:
    explicit allocator_error(const char* msg) : msg_(msg) {}

    const char* what() const noexcept override {
        return msg_;
    }

private:
    const char* msg_;
};

class mem_resource : public std::pmr::memory_resource {
public:
  explicit mem_resource(mem_config cfg) : config_(cfg) {
    if (config_.region.start > config_.region.end) {
      throw std::invalid_argument("allocator can't end before it begins");
    }
    stats_.total_bytes = config_.region.size();
  }
  virtual ~mem_resource() = default;

  const mem_stats& stats() const noexcept {
    return stats_;
  }

  /**
   * std::pmr::memory_resource
   * https://en.cppreference.com/w/cpp/memory/memory_resource.html
   *
   *   void* allocate(size_t bytes, size_t alignment)
   *   void deallocate(void *p, size_t bytes, size_t alignment)
   *   bool is_equal(const memory_resource& other)
   *
   * this interface does not provide allocate_at(), which is necessary for
   * MAP_FIXED allocations
   */
  void* allocate_at(void* where, std::size_t bytes, bool permit_override, std::size_t alignment=alignof(std::max_align_t)) {
    return do_allocate_at(where, bytes, alignment, permit_override);
  }

  /*
   * see API by Andrei Alexandrescu in 'allocator Is to Allocation what vector Is to Vexation'
   */
  bool owns(const void* p) const noexcept {
    return config_.region.owns(reinterpret_cast<std::uintptr_t>(p));
  }

  /**
   * how much would be reserved as a lower bound for a request of this size
   */
  std::size_t good_size(std::size_t bytes, std::size_t alignment=alignof(std::max_align_t)) const noexcept {
    return strat_good_size(bytes, alignment);
  }

  // this strategy's own natural/minimum alignment
  std::size_t alignment() const noexcept {
    return strat_alignment();
  }

  /*
   * safer versions of the std::pmr::memory_resource interface
   *
   *   mem_region allocate_region(size_t bytes, size_t alignment);
   *   void deallocate_region(const mem_region& blk, size_t alignment);
   *   void reallocate(const mem_region& blk, std::size_t new_bytes, size_t alignment);
   *
   * by using the raw void pointers, we would trust the consumers won't make mistakes:
   *   void* p = res.allocate(bytes, alignment);
   *   mem_region blk{ (uintptr_t)p, (uintptr_t)p + bytes }; // wrong if alignment > bytes
   **/
  mem_region allocate_region(std::size_t bytes, std::size_t alignment=alignof(std::max_align_t)) {
    void* p = this->allocate(bytes, alignment);
    const uintptr_t addr = reinterpret_cast<uintptr_t>(p);
    return mem_region{ addr, addr + good_size(bytes, alignment) };
  }

  void deallocate_region(const mem_region& blk, std::size_t alignment=alignof(std::max_align_t)) {
    this->deallocate(blk.start_ptr(), blk.size(), alignment);
  }

  /**
   * try to allocate the entiry of the remaining space in a single allocation.
   * allocator has no guarantee that this is possible
   */
  mem_region allocate_all() noexcept {
    // note: doesn't update protection flags
    mem_region result = strat_allocate_all();
    if (result.size() == 0)
      return result;

    stats_.requested_bytes += result.size();
    if (stats_.requested_bytes > stats_.peak_requested_bytes) {
      stats_.peak_requested_bytes = stats_.requested_bytes;
    }
    stats_.count_allocate_all++;
    return result;
  }

  // the single largest block currently satisfiable, not necessarily all
  // remaining capacity.
  mem_region allocate_largest() noexcept {
    // note: doesn't update protection flags
    mem_region result = strat_allocate_largest();
    if (result.size() == 0)
      return result;

    stats_.requested_bytes += result.size();
    if (stats_.requested_bytes > stats_.peak_requested_bytes) {
      stats_.peak_requested_bytes = stats_.requested_bytes;
    }
    stats_.count_allocate_largest++;
    return result;
  }

  void deallocate_all() {
    strat_deallocate_all();
    stats_.requested_bytes = 0;
  }

  // might move the address
  // note: doesn't update protection flags
  void reallocate(mem_region& blk, std::size_t new_bytes, std::size_t alignment=alignof(std::max_align_t)) {
    strat_reallocate(blk, new_bytes, alignment);
    stats_.count_realloc++;
  }

  // grow in place only, never moves.
  // returns false if there is no room right now, blk remains unchanged
  // note: doesn't update protection flags
  bool expand(mem_region& blk, std::size_t delta) noexcept {
    if (!strat_expand(blk, delta))
      return false;

    stats_.requested_bytes += delta;
    if (stats_.requested_bytes > stats_.peak_requested_bytes) {
      stats_.peak_requested_bytes = stats_.requested_bytes;
    }
    stats_.count_expand++;
    return true;
  }

  // shrink in place by delta bytes, same start address. false if delta is
  // bigger than the block itself; otherwise should always succeed
  // note: doesn't update protection flags
  bool shrink(mem_region& blk, std::size_t delta) noexcept {
    if (delta > blk.size())
      return false;
    if (!strat_shrink(blk, delta))
      return false;

    stats_.requested_bytes -= delta;
    stats_.count_shrink++;
    return true;
  }

  virtual size_t bytes_used() const noexcept = 0;
  virtual size_t bytes_free() const noexcept = 0;
  virtual uintptr_t highest_used() const noexcept = 0;

  void dump_state() const noexcept {
    std::println("{}:", name());
    std::println("  region:        [{:#x}, {:#x}) ({} bytes)",
                 config_.region.start, config_.region.end, config_.region.size());
    std::println("  overbooking:   {}", config_.overbooking);
    std::println("  bytes_used:    {}", bytes_used());
    std::println("  bytes_free:    {}", bytes_free());
    std::println("  highest_used:  {:#x}", highest_used());
    strat_summary();
  }

protected:
  /**
   * these are the *strategy hooks*, need to be implemented by your allocator
   */
  virtual uintptr_t strat_allocate(std::size_t bytes, std::size_t alignment) = 0;
  virtual void  strat_deallocate(uintptr_t p, std::size_t bytes, std::size_t alignment) noexcept = 0;
  virtual std::size_t strat_good_size(std::size_t bytes, std::size_t alignment) const noexcept = 0;
  virtual std::size_t strat_alignment() const noexcept = 0;

  /*
   * default implementation is a reasonable default:
   *   tries shrink()/expand() before falling back to allocate+copy+free
   **/
  virtual void strat_reallocate(mem_region& blk, std::size_t new_bytes, std::size_t alignment) {
    if (new_bytes <= blk.size()) {
      this->shrink(blk, blk.size() - new_bytes);
      return;
    }

    if (this->expand(blk, new_bytes - blk.size())) {
      return;
    }

    // fallback: needs temporary space, and causes a memcpy :(
    void* new_ptr = this->allocate(new_bytes, alignment);
    std::memcpy(new_ptr, blk.start_ptr(), std::min(blk.size(), new_bytes));
    this->deallocate(blk.start_ptr(), blk.size(), alignment);
    const uintptr_t new_addr = reinterpret_cast<uintptr_t>(new_ptr);
    blk = mem_region{ new_addr, new_addr + new_bytes };
  }

  virtual bool strat_expand(mem_region& blk, std::size_t delta) noexcept = 0;

  virtual mem_region strat_allocate_all() noexcept = 0;

  virtual mem_region strat_allocate_largest() noexcept {
    /*
     * poorly optimized default implementation
     *   binary search on size via allocate()/deallocate()
     * allocators should ideally implement their own version of this
     */
    std::size_t lo = 0, hi = this->bytes_free();
    void* best_ptr = nullptr;
    std::size_t best_size = 0;

    while (lo < hi) {
      const std::size_t mid = lo + (hi - lo + 1) / 2;
      void* p = nullptr;
      try {
        p = this->allocate(mid, alignof(std::max_align_t));
      } catch (...) {
        hi = mid - 1;
        continue;
      }

      if (best_ptr) {
        this->deallocate(best_ptr, best_size, alignof(std::max_align_t));
      }

      best_ptr = p;
      best_size = mid;
      lo = mid;
    }

    if (!best_ptr) return mem_region{};
    const uintptr_t addr = reinterpret_cast<uintptr_t>(best_ptr);
    return mem_region{ addr, addr + best_size };
  }

  virtual bool strat_shrink(mem_region& blk, std::size_t delta) noexcept {
    /*
     * the default implementation is always valid, but allocators should probably
     * implement their own strategies so the gap can be reclaimed
     *
     * delta is assumed to be valid here (i.e. delta is smaller than the block's size)
     */
    blk = mem_region{ blk.start, blk.end - delta };
    return true;
  }

  virtual void strat_deallocate_all() {
    /*
     * optional feature, but probably desired
     **/
    throw std::runtime_error("mem_resource::deallocate_all: not supported by this allocator strategy");
  }

  virtual uintptr_t strat_allocate_at(uintptr_t where, std::size_t bytes, std::size_t alignment, bool permit_override) {
    /*
     * the default implementation explicitly ignores the requested position in order
     * to make it easier to implement different strategies
     **/
    (void)where;
    (void) permit_override;
    return strat_allocate(bytes, alignment);
  }

  virtual std::string_view name() const noexcept = 0;

  /**
   * optional allocat-specific addition to dump_state()
   */
  virtual void strat_summary() const noexcept {}

private:
  /**
   * the default implementation from std::pmr::memory_resource are the following wrappers
   *   allocate => do_allocate(size_t bytes, size_t alignment)
   *   deallocate => void do_deallocate(void* p, size_t bytes, size_t alignment)
   *   is_equal => bool do_is_equal(size_t bytes, size_t alignment)
   *
   *  we override these here to handle statistics, final to guarantee nobody
   *  accidentally overrides these
   *
   *  we also add do_allocate_at() here for consistency
   */

  void* do_allocate(std::size_t bytes, std::size_t alignment) final {
#ifdef INCLUDEOS_SMP_ENABLE
    std::lock_guard<Spinlock> guard(alloc_lock_);
#endif
    // pre: validate alignment
    if (!std::has_single_bit(alignment))
      throw std::invalid_argument("alignment must be a power of 2");

    // pre: validate overbooking
    if (!config_.overbooking && stats_.busy_bytes + bytes > stats_.total_bytes) {
        // HACK: using requested bytes here is wrong, just a temporary workaround
        // if (!config_.overbooking && stats_.busy_bytes + bytes > stats_.total_bytes)
        throw std::bad_alloc();
    }

    uintptr_t p = strat_allocate(bytes, alignment);

    // post: update stats
    stats_.requested_bytes += bytes;
    if (stats_.requested_bytes > stats_.peak_requested_bytes) {
      stats_.peak_requested_bytes = stats_.requested_bytes;
    }
    stats_.count_alloc++;

    return reinterpret_cast<void*>(p);
  }

  void* do_allocate_at(void* where, std::size_t bytes, std::size_t alignment, bool permit_override) {  // not final since it's not a virtual: can't be overriden anyway
#ifdef INCLUDEOS_SMP_ENABLE
    std::lock_guard<Spinlock> guard(alloc_lock_);
#endif
    auto addr = reinterpret_cast<std::uintptr_t>(where);

    // pre: validate alignment
    if (!std::has_single_bit(alignment))
      throw std::invalid_argument("alignment must be a power of 2");
    if (addr & (alignment - 1))
      throw std::invalid_argument("address must be aligned");

    // pre: validate range
    if (addr < config_.region.start || addr >= config_.region.end)
      throw std::invalid_argument("address must be in range");
    if (bytes > config_.region.end - addr)
      throw os::mem::allocator_error("os::mem::mem_resource:do_allocate_at: allocation won't fit at requested address");

    uintptr_t p = strat_allocate_at(reinterpret_cast<uintptr_t>(where), bytes, alignment, permit_override);

    // post: update stats
    stats_.requested_bytes += bytes;
    if (stats_.requested_bytes > stats_.peak_requested_bytes) {
      stats_.peak_requested_bytes = stats_.requested_bytes;
    }
    stats_.count_alloc_at++;

    return reinterpret_cast<void*>(p);
  }

  void do_deallocate(void* p, std::size_t bytes, std::size_t alignment) final {
#ifdef INCLUDEOS_SMP_ENABLE
    std::lock_guard<Spinlock> guard(alloc_lock_);
#endif
    // pre: TODO: verify region was actually allocated?

    if (p != nullptr)
      strat_deallocate(reinterpret_cast<uintptr_t>(p), bytes, alignment);

    // post: update stats
    if (bytes > stats_.requested_bytes) stats_.requested_bytes = 0;  // this branch shouldn't be necessary
    else stats_.requested_bytes -= bytes;
    stats_.count_dealloc++;
  }

  bool do_is_equal(const std::pmr::memory_resource& other) const noexcept final {
    return this == &other;
  }

  mem_stats stats_{};
  mem_config config_{};
#ifdef INCLUDEOS_SMP_ENABLE
  Spinlock alloc_lock_;
#endif
};

/**
 * places an allocator inside the region, with the allocator being slightly
 * smaller than the region to make room for the header
 *
 * returns nullptr if region is too small to even hold the header
 * Args are forwarded to the allocator constructor
 */
template <typename Strategy, typename... Args>
Strategy* create_resource_at(mem_region region, Args&&... args)
  noexcept(std::is_nothrow_constructible_v<Strategy, mem_config, Args...>)
{
  static_assert(std::is_base_of_v<mem_resource, Strategy>, "Strategy must derive from mem_resource");

  const std::uintptr_t self_addr = align_up(region.start, alignof(Strategy));
  const std::uintptr_t alloc_start = align_up(self_addr + sizeof(Strategy), alignof(std::max_align_t));

  if (alloc_start >= region.end) {
    return nullptr;
  }

  mem_config cfg{ .region = { alloc_start, region.end }, .overbooking = false };
  return new (reinterpret_cast<void*>(self_addr)) Strategy(cfg, std::forward<Args>(args)...);
}

} // namespace os::mem
