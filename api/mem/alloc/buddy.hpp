// This file is a part of the IncludeOS unikernel - www.includeos.org
//
// Copyright 2018 IncludeOS AS, Oslo, Norway
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
#include <memory_resource>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <new>
#include <print>
#include <stdexcept>
#include <string_view>
#include <vector>
#include <algorithm>

#include <mem/mem.hpp>

namespace os::mem {

struct buddy_config {
  std::size_t min_block = 64; // must be power of two
};


class buddy_resource final : public mem_resource {
public:
  buddy_resource(mem_config cfg, buddy_config bcfg = {})
    : mem_resource(cfg), cfg_(cfg), bcfg_(bcfg)
  {
    if (!std::has_single_bit(bcfg_.min_block)) {
      throw std::invalid_argument("buddy min_block must be a power of two");
    }

    resource_base_ = cfg_.region.start;
    if (resource_base_ >= cfg_.region.end) {
      throw std::bad_alloc();
    }

    const std::size_t total_avail = static_cast<std::size_t>(cfg_.region.end - resource_base_);
    const std::size_t provisional = std::bit_floor(total_avail);  // how many buds at max?
    if (provisional < bcfg_.min_block) {
      throw std::bad_alloc();
    }

    // these are provisional because the freelist might offset them slightly
    const int provisional_min_order_ = order_for(0);
    const int provisional_max_order_ = order_for(provisional);
    const int provisional_orders_ = static_cast<std::size_t>((provisional_max_order_ - provisional_min_order_) + 1);

    // this freelist is placed before the buddy tree
    free_ = reinterpret_cast<FreeNode**>(resource_base_);
    const std::size_t freelist_size = provisional_orders_ * sizeof(FreeNode*);

    // the pool's (i.e. the colletion of buds) base starts after the buddy's metadata
    pool_base_ = align_up(resource_base_ + freelist_size, bcfg_.min_block);
    if (pool_base_ >= cfg_.region.end) {
      throw std::bad_alloc();  // metadata took too much space
    }

    std::size_t avail = static_cast<std::size_t>(cfg_.region.end - pool_base_);
    pool_size_ = std::bit_floor(avail);
    if (pool_size_ < bcfg_.min_block) {
      throw std::bad_alloc();  // can't even fit one bud in remaining space
    }

    min_order_ = order_for(0);
    max_order_ = order_for(pool_size_);
    orders_ = static_cast<std::size_t>((max_order_ - min_order_) + 1);

    pool_end_ = pool_base_ + pool_size_;

    for (std::size_t i = 0; i < orders_; ++i) {
      free_[i] = nullptr;
    }

    // initial free block = whole pool
    push_free(pool_base_, max_order_);
  }

  // self-hosts a buddy_resource at the front of region, same as
  // arena_resource::create_at -- bcfg is forwarded to the constructor
  static buddy_resource* create_at(mem_region region, buddy_config bcfg = {}) {
    return create_resource_at<buddy_resource>(region, bcfg);
  }

  uintptr_t malloc(size_t bytes) {
    return reinterpret_cast<uintptr_t>(this->strat_allocate(bytes, alignof(max_align_t)));
  }

  void free(uintptr_t addr, size_t bytes) {
    this->strat_deallocate(addr, bytes, alignof(max_align_t));
  }


  size_t bytes_used() const noexcept override {
    return busy_bytes_;
  }

  size_t bytes_free() const noexcept override {
    return pool_size_ - busy_bytes_;
  }

  uintptr_t highest_used() const noexcept override {
    return peak_highest_used_;
  }

protected:
  std::string_view name() const noexcept override { return "buddy"; }

  void strat_summary() const noexcept override {
    std::println("  min_block:     {}", bcfg_.min_block);
    std::println("  resource_base: {:#x}", resource_base_);
    std::println("  pool_base:     {:#x}", pool_base_);
    std::println("  pool_end:      {:#x}", pool_end_);
    std::println("  pool_size:     {} bytes (2^{})", pool_size_, max_order_);
    std::println("  min_order:     {} (min bud = {} bytes)", min_order_, 1<<min_order_);
    std::println("  max_order:     {} (max bud = {} bytes)", max_order_, 1<<max_order_);

    std::println("  freelist (order: count x block_size):");
    std::size_t largest_free = 0;
    std::size_t free_blocks_total = 0;
    for (int order = max_order_; order >= min_order_; --order) {
      FreeNode* n = free_[idx(order)];
      if (n == nullptr) continue;

      std::size_t count = 0;
      do {
        ++count;
        n = n->next;
      } while (n != nullptr);

      const std::size_t block_size = std::size_t(1) << order;
      free_blocks_total += count;
      if (largest_free == 0)
        largest_free = block_size; // first hit will always be the largest

      std::println("    {:>2}: {:>6} x {} bytes", order, count, block_size);
    }

    const std::size_t free_bytes = bytes_free();
    const double fragmentation = (free_bytes > 0)
      ? 1.0 - (static_cast<double>(largest_free) / static_cast<double>(free_bytes))
      : 0.0;

    std::println("  free_blocks:   {}", free_blocks_total);
    std::println("  largest_free:  {} bytes", largest_free);
    std::println("  fragmentation: {:.4f} (1 - largest_free/bytes_free; 0 = no fragmentation)", fragmentation);
  }

  std::size_t strat_good_size(std::size_t bytes, std::size_t alignment) const noexcept override {
    // the good size will be an exact size since we split any bigger
    // chunk down until it's a tight fit

    const std::size_t aligned_bytes = std::max(bytes, alignment);
    const int want = order_for(aligned_bytes);
    if (want > max_order_) {
      return 0;
    }
    return std::size_t(1) << want;
  }

  std::size_t strat_alignment() const noexcept override {
    return bcfg_.min_block;
  }

  /**
   * resets the freelist to a single top-level block spanning the whole
   * pool, discarding every outstanding allocation at once.
   */
  void strat_deallocate_all() noexcept override {
    for (std::size_t i = 0; i < orders_; ++i) {
      free_[i] = nullptr;
    }
    push_free(pool_base_, max_order_);
    busy_bytes_ = 0;
    highest_used_ = 0;
  }

  // works only if blk is the left child at every order between its
  // current one and the target, and each successive buddy is free.
  bool strat_expand(mem_region& blk, std::size_t delta) noexcept override {
    const std::size_t old_bytes = blk.size();
    const std::size_t new_bytes = old_bytes + delta;

    const int have = order_for(old_bytes);
    const int want = order_for(new_bytes);

    if (want == have) {
      // same buddy, caused by alignment rounding
      blk = mem_region{ blk.start, blk.start + new_bytes };
      return true;
    }

    for (int level = have; level < want; ++level) {
      const std::uintptr_t bud = buddy_of(blk.start, level);
      if (bud < blk.start || !is_free(bud, level)) {
        return false;  // how rude of them
      }
    }

    // we know there's nobody in our way, so we can just claim the space
    // note: this assumption is not thread-safe
    for (int level = have; level < want; ++level) {
      remove_if_free(buddy_of(blk.start, level), level);
    }

    const std::size_t old_reserved = std::size_t(1) << have;
    const std::size_t new_reserved = std::size_t(1) << want;
    busy_bytes_ += (new_reserved - old_reserved);
    peak_busy_bytes_ = std::max(peak_busy_bytes_, busy_bytes_);

    const std::uintptr_t new_end = blk.start + new_reserved;
    highest_used_ = std::max(highest_used_, new_end);
    peak_highest_used_ = std::max(peak_highest_used_, new_end);

    blk = mem_region{ blk.start, blk.start + new_bytes };
    return true;
  }

  // only works if the whole pool is still a single free top-level block 🙃
  mem_region strat_allocate_all() noexcept override {
    if (free_[idx(max_order_)] == nullptr) {
      return mem_region{}; // fragmented, no single top-level block
    }

    const std::uintptr_t addr = pop_free(max_order_);
    const std::uintptr_t end = addr + pool_size_;

    busy_bytes_ += pool_size_;
    peak_busy_bytes_ = std::max(peak_busy_bytes_, busy_bytes_);
    highest_used_ = std::max(highest_used_, end);
    peak_highest_used_ = std::max(peak_highest_used_, end);

    return mem_region{ addr, end };
  }

  mem_region strat_allocate_largest() noexcept override {
    for (int order = max_order_; order >= min_order_; --order) {
      if (free_[idx(order)] == nullptr) continue;

      const std::uintptr_t addr = pop_free(order);
      const std::size_t reserved = std::size_t(1) << order;
      const std::uintptr_t end = addr + reserved;

      busy_bytes_ += reserved;
      peak_busy_bytes_ = std::max(peak_busy_bytes_, busy_bytes_);
      highest_used_ = std::max(highest_used_, end);
      peak_highest_used_ = std::max(peak_highest_used_, end);

      return mem_region{ addr, end };
    }
    return mem_region{}; // nothing free at all
  }

  // note: assumes blk.size() is the actual reserved size
  bool strat_shrink(mem_region& blk, std::size_t delta) noexcept override {
    const std::size_t old_bytes = blk.size();
    const std::size_t new_bytes = old_bytes - delta;

    const int have = order_for(old_bytes);
    const int want = order_for(std::max(new_bytes, bcfg_.min_block));

    if (want >= have) {
      // still the same size class, nothing to give back
      blk = mem_region{ blk.start, blk.end - delta };
      return true;
    }

    split_left(blk.start, have, want);

    const std::size_t freed = (std::size_t(1) << have) - (std::size_t(1) << want);
    busy_bytes_ -= freed;

    blk = mem_region{ blk.start, blk.end - delta };
    return true;
  }

  std::uintptr_t strat_allocate(std::size_t bytes, std::size_t alignment) override {
    // mem_resource already validated alignment is power-of-two, this is a requirement

    // buddy needs a block that is at least the size of the alignment
    const std::size_t aligned_bytes = std::max(bytes, alignment);  // should we enforce this from outside?

    const int want = order_for(aligned_bytes);
    if (want > max_order_) {
      throw std::bad_alloc();  // our tree is not big enough
    }

    // find smallest available order >= want
    int have = want;
    while (have <= max_order_ && free_[idx(have)] == nullptr) {
      ++have;
    }

    if (have > max_order_) {
      throw std::bad_alloc();  // couldn't find a free node of this size
    }

    // pop a block of order 'have' and split down to 'want'
    std::uintptr_t addr = pop_free(have);
    addr = split_left(addr, have, want);

    /* tracing metainfo */
    {
      const std::size_t reserved = std::size_t(1) << want;
      busy_bytes_ += reserved;
      peak_busy_bytes_ = std::max(peak_busy_bytes_, busy_bytes_);

      const std::uintptr_t end = addr + reserved;
      highest_used_ = std::max(highest_used_, end);
      peak_highest_used_ = std::max(peak_highest_used_, end);
    }

    return addr;
  }

  void strat_deallocate(std::uintptr_t addr, std::size_t bytes, std::size_t alignment) noexcept override {
    // this assumes addr is both valid and allocated

    if (addr < pool_base_ || addr >= pool_end_) {
      // if caller frees something outside the pool, ignore (we avoid throwing for performance)
      return;
    }

    const std::size_t need = std::max(bytes, alignment);
    int order = order_for(need);
    if (order > max_order_) {
      // the caller tried to deallocate something bigger than us... this is bad!
      return;
    }

    /* tracing metainfo */
    {
      const std::size_t reserved = std::size_t(1) << order;
      if (busy_bytes_ >= reserved) {
        busy_bytes_ -= reserved;
      } else {
        busy_bytes_ = 0;
      }
    }

    // merge while buddy is free at same order
    while (order < max_order_) {
      const std::uintptr_t bud = buddy_of(addr, order);

      // buddies must also be within pool; if not, stop. (TODO: can this happen?)
      if (bud < pool_base_ || bud >= pool_end_) {
        break;
      }

      // if buddy block is currently free at this order, remove it
      if (!remove_if_free(bud, order)) {
        // sadly, the bud wasn't free. we have introduced fragmentation (TODO: add stats for this?)
        break;
      }

      // and merge!
      addr = std::min(addr, bud);
      ++order;
    }

    push_free(addr, order);
  }

  std::uintptr_t strat_allocate_at(std::uintptr_t addr, std::size_t bytes, std::size_t alignment, bool permit_override) override {
    // mem_resource already validated:
    // - addr is aligned to 'alignment'
    // - addr is within [region.start, region.end)
    // - [addr, addr+bytes) fits in region
    //
    // buddy adds: we can only place at the beginning of a buddy block,
    // and that exact block must be currently free
    //
    // TODO: permit overriding

    if (addr < pool_base_ || addr >= pool_end_) {
      throw std::bad_alloc();  // we have no control over the address requested!
    }

    const std::size_t need = std::max(bytes, alignment);
    const int order = order_for(need);
    if (order > max_order_) {
      throw std::bad_alloc();  // the requested size is too big to fit in our pool
    }

    const std::uintptr_t block_size = (std::uintptr_t(1) << order);
    if ( ((addr - pool_base_) & (block_size - 1)) != 0 ) {
      throw std::bad_alloc();  // address isn't aligned to the requested size
    }

    std::uintptr_t base = 0;
    int have = order;
    if (!permit_override) {
      if (!block_is_free(addr, order, base, have)) {
        throw os::mem::allocator_error("os::mem::buddy_allocator::strat_allocate_at: no free block contains requested address");
      }
    }

    /* tracing metainfo */
    {
      // TODO: fix tracing to not double count overriden data
      const std::size_t reserved = std::size_t(1) << order;
      busy_bytes_ += reserved;
      peak_busy_bytes_ = std::max(peak_busy_bytes_, busy_bytes_);

      const std::uintptr_t end = addr + reserved;
      highest_used_ = std::max(highest_used_, end);
      peak_highest_used_ = std::max(peak_highest_used_, end);
    }

    return addr;
  }

private:
  struct FreeNode { FreeNode* next; }; // named as such because... they're free to be used

  mem_config cfg_;
  buddy_config bcfg_;

  std::uintptr_t resource_base_{0};
  std::uintptr_t pool_base_{0};
  std::uintptr_t pool_end_{0};
  std::size_t    pool_size_{0};

  int min_order_{0};  // represents the order for the tiniest bud available
  int max_order_{0};  // represents the order for the biggest bud available
  std::size_t orders_{0};  // max-min, i.e. how many valid orders. used for freelist

  FreeNode** free_{nullptr};

  std::size_t busy_bytes_{0};
  std::size_t peak_busy_bytes_{0};
  std::uintptr_t highest_used_{0};
  std::uintptr_t peak_highest_used_{0};

  /**
   * gets the corresponding order k big enough to hold the requested size
   *
   * since buddy is a binary tree, we get:
   *   order k => individual blocks of 2^k bytes
   *
   *  bytes should be aligned
   */
  int order_for(std::size_t bytes) const noexcept {
    std::size_t n = std::max(bytes, bcfg_.min_block);
    if (!std::has_single_bit(n)) {
      n = std::bit_ceil(n);
    }
    return static_cast<int>(std::countr_zero(n));
  }

  /**
   * gets the matching buddy for a given buddy at a given order
   *
   * [                  ]   parent spans 2^(order+1) bytes
   * [ left    ][ right ]   <= order
   * ^          ^
   * addr       addr+2^order
   */
  std::uintptr_t buddy_of(std::uintptr_t addr, int order) const noexcept {
    const std::uintptr_t off = addr - pool_base_;
    return pool_base_ + (off ^ (std::uintptr_t(1) << order));
  }

  /**
   * converts an order into an index of the free-list.
   * this prevents allocating free-nodes for illegal nodes
   */
  std::size_t idx(int order) const noexcept {
    return static_cast<std::size_t>(order - min_order_);
  }

  void push_free(std::uintptr_t p, int order) noexcept {
    auto* n = reinterpret_cast<FreeNode*>(p);
    n->next = free_[idx(order)];
    free_[idx(order)] = n;
  }

  std::uintptr_t pop_free(int order) noexcept {
    FreeNode* n = free_[idx(order)];
    free_[idx(order)] = n->next;
    return reinterpret_cast<std::uintptr_t>(n);
  }

  /*
   * removes a node at the specified addr, for the specific order
   *
   * returns true if it was removed
   * returns false if the node was busy
   * */
  bool remove_if_free(std::uintptr_t addr, int order) noexcept {
    auto* target = reinterpret_cast<FreeNode*>(addr);

    FreeNode** cur = &free_[idx(order)];
    while (*cur) {
      if (*cur == target) {
        *cur = (*cur)->next;
        return true;
      }
      cur = &((*cur)->next);
    }
    return false;
  }

  // same search as remove_if_free, but read-only
  bool is_free(std::uintptr_t addr, int order) const noexcept {
    const auto* target = reinterpret_cast<const FreeNode*>(addr);
    const FreeNode* cur = free_[idx(order)];
    while (cur) {
      if (cur == target) return true;
      cur = cur->next;
    }
    return false;
  }

  std::uintptr_t split_left(std::uintptr_t addr, int have, int want) noexcept {
    while (have > want) {
      --have;
      // split into [addr, addr+2^have) and [addr+2^have, addr+2^(have+1))
      const std::uintptr_t right = addr + (std::uintptr_t(1) << have);
      push_free(right, have);

      // keep left half as addr
    }
    return addr;
  }

  /* keep dividing down a bud until the desired address is adequatly split into buds */
  std::uintptr_t split_towards(std::uintptr_t base, int have, int want, std::uintptr_t target) noexcept {
    while (have > want) {
      --have;
      const std::uintptr_t half = (std::uintptr_t(1) << have);
      const std::uintptr_t right = base + half;

      if (target < right) {
        push_free(right, have);
      } else {
        push_free(base, have);
        base = right;
      }
    }
    return base;
  }

  /* check if there is some free block that contains the requested target address */
  bool block_is_free(std::uintptr_t target, int want, std::uintptr_t& base, int& have) noexcept {
    for (have = want; have <= max_order_; ++have) {
      FreeNode** cur = &free_[idx(have)];
      while (*cur) {
        const auto candidate = reinterpret_cast<std::uintptr_t>(*cur);
        const auto size = (std::uintptr_t(1) << have);

        if (target >= candidate && target < candidate + size) {
          FreeNode* hit = *cur;
          *cur = hit->next;
          base = candidate;
          return true;
        }

        cur = &((*cur)->next);
      }
    }
    return false;
  }
};

} // namespace os::mem
