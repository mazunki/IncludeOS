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
#include <algorithm>

#include <mem/mem.hpp>

namespace os::mem {

struct snake_config {
  std::size_t align = alignof(std::max_align_t); // every allocation's reserved size is rounded up to a multiple of this
  std::size_t max_pending = 1024; // max concurrent live allocations + wrap-gaps, tracked in a self-hosted queue
};

class snake_resource final : public mem_resource {
public:
  snake_resource(mem_config cfg, snake_config rcfg = {})
    : mem_resource(cfg), cfg_(cfg), rcfg_(rcfg)
  {
    if (!std::has_single_bit(rcfg_.align)) {
      throw std::invalid_argument("snake align must be a power of two");
    }
    if (rcfg_.max_pending == 0) {
      throw std::invalid_argument("snake max_pending must be > 0");
    }

    resource_base_ = cfg_.region.start;
    spans_ = reinterpret_cast<Span*>(resource_base_);
    const std::size_t spans_bytes = rcfg_.max_pending * sizeof(Span);

    pool_base_ = align_up(resource_base_ + spans_bytes, rcfg_.align);
    if (pool_base_ >= cfg_.region.end) {
      throw std::bad_alloc(); // metadata took too much space
    }
    capacity_ = cfg_.region.end - pool_base_;
  }

  static snake_resource* create_at(mem_region region, snake_config rcfg = {}) {
    return create_resource_at<snake_resource>(region, rcfg);
  }

  /**
   * carves at least `capacity` usable bytes out of parent and self-hosts a
   * child snake in it. the header overhead is added on top automatically.
   */
  static snake_resource* create_from(mem_resource& parent, std::size_t capacity, snake_config rcfg = {}) {
    const std::size_t total = sizeof(snake_resource) + alignof(snake_resource) + capacity;
    mem_region region = parent.allocate_region(total, alignof(snake_resource));
    snake_resource* child = create_at(region, rcfg);
    if (!child) {
      parent.deallocate_region(region, alignof(snake_resource));
    }
    return child;
  }

  size_t bytes_used() const noexcept override { return busy_bytes_; }
  size_t bytes_free() const noexcept override { return capacity_ - busy_bytes_; }
  uintptr_t highest_used() const noexcept override { return peak_highest_used_; }

  std::size_t largest_free() const noexcept override {
    if (busy_bytes_ == 0) return capacity_;
    if (busy_bytes_ == capacity_) return 0;

    const std::size_t phys_tail = tail_off_ % capacity_;
    const std::size_t phys_head = head_off_ % capacity_;

    if (phys_tail < phys_head) {
      // live span is the single contiguous run [phys_tail, phys_head);
      // free space is split across the two ends of the pool
      return std::max(capacity_ - phys_head, phys_tail);
    }
    // live span wraps around the end of the pool; free space is the
    // single contiguous run in the middle, [phys_head, phys_tail)
    return phys_tail - phys_head;
  }

protected:
  std::string_view name() const noexcept override { return "snake"; }

  void strat_summary() const noexcept override {
    std::println("  capacity:   {} bytes", capacity_);
    std::println("  pool_base:  {:#x}", pool_base_);
    std::println("  head_off:   {}", head_off_);
    std::println("  tail_off:   {}", tail_off_);
    std::println("  pending:    {} spans (of {} max)", span_count_, rcfg_.max_pending);
  }

  std::size_t strat_good_size(std::size_t bytes, std::size_t alignment) const noexcept override {
    return align_up(std::max(bytes, alignment), rcfg_.align);
  }

  std::size_t strat_alignment() const noexcept override {
    return rcfg_.align;
  }

  uintptr_t strat_allocate(std::size_t bytes, std::size_t alignment) override {
    const std::size_t size = strat_good_size(bytes, alignment);
    if (size == 0 || size > capacity_) {
      throw os::mem::allocator_error("os::mem::snake_resource::strat_allocate: requested size exceeds the snake's total capacity");
    }

    const std::size_t phys_off = head_off_ % capacity_;
    const std::size_t remaining_before_wrap = capacity_ - phys_off;

    std::size_t gap = 0;
    std::uintptr_t addr;
    if (size <= remaining_before_wrap) {
      addr = pool_base_ + phys_off;
    } else {
      // waste the trailing gap and wrap to the start
      gap = remaining_before_wrap;
      addr = pool_base_;
    }

    const std::size_t total_needed = gap + size;
    if (total_needed > bytes_free()) {
      throw os::mem::allocator_error("os::mem::snake_resource::strat_allocate: not enough free space, wrapping would run into still-live data");
    }
    if (span_count_ + (gap > 0 ? 2u : 1u) > rcfg_.max_pending) {
      throw os::mem::allocator_error("os::mem::snake_resource::strat_allocate: pending-span bookkeeping queue is full (increase snake_config::max_pending)");
    }

    if (gap > 0) {
      push_span(gap, Span::Gap, 0);
    }
    push_span(size, Span::Live, addr);

    head_off_ += total_needed;
    busy_bytes_ += total_needed;
    peak_busy_bytes_ = std::max(peak_busy_bytes_, busy_bytes_);

    const uintptr_t end = addr + size;
    highest_used_ = std::max(highest_used_, end);
    peak_highest_used_ = std::max(peak_highest_used_, end);

    return addr;
  }

  // we support deallocation in any order
  void strat_deallocate(uintptr_t addr, std::size_t bytes, std::size_t alignment) noexcept override {
    const std::size_t size = strat_good_size(bytes, alignment);

    std::size_t idx = span_tail_idx_;
    for (std::size_t n = 0; n < span_count_; ++n, idx = (idx + 1) % rcfg_.max_pending) {
      Span& s = spans_[idx];
      if (s.state == Span::Live && s.addr == addr && s.size == size) {
        s.state = Span::Done;
        break;
      }
    }

    // reclaim the longest current run of done/gap spans from the tail,
    // stopping at the first still-live one
    while (span_count_ > 0 && spans_[span_tail_idx_].state != Span::Live) {
      const std::size_t sz = spans_[span_tail_idx_].size;
      pop_span();
      tail_off_ += sz;
      busy_bytes_ -= sz;
    }
  }

  // clears everything at once -- unlike a single out-of-order free, this
  // is always well-defined for a ring
  void strat_deallocate_all() noexcept override {
    head_off_ = 0;
    tail_off_ = 0;
    span_head_idx_ = 0;
    span_tail_idx_ = 0;
    span_count_ = 0;
    busy_bytes_ = 0;
  }

  // in-place growth/shrink of an arbitrary live span isn't meaningful for
  // a ring (only the tail and head ends are ever touched) -- not supported
  bool strat_expand(mem_region&, std::size_t) noexcept override {
    return false;
  }

  // "the whole pool as one allocation" doesn't fit the FIFO model either
  mem_region strat_allocate_all() noexcept override {
    return mem_region{};
  }

private:
  struct Span {
    enum State { Live, Gap, Done };
    std::size_t size;
    std::uintptr_t addr; // unused for Gap spans
    State state;
  };

  void push_span(std::size_t size, Span::State state, std::uintptr_t addr) noexcept {
    spans_[span_head_idx_] = { size, addr, state };
    span_head_idx_ = (span_head_idx_ + 1) % rcfg_.max_pending;
    ++span_count_;
  }

  void pop_span() noexcept {
    span_tail_idx_ = (span_tail_idx_ + 1) % rcfg_.max_pending;
    --span_count_;
  }

  mem_config cfg_;
  snake_config rcfg_;

  std::uintptr_t resource_base_{0};
  std::uintptr_t pool_base_{0};
  std::size_t capacity_{0};

  Span* spans_{nullptr};
  std::size_t span_head_idx_{0};
  std::size_t span_tail_idx_{0};
  std::size_t span_count_{0};

  std::size_t head_off_{0}; // monotonic-ish logical offset (mod capacity_ for the physical address)
  std::size_t tail_off_{0};

  std::size_t busy_bytes_{0};
  std::size_t peak_busy_bytes_{0};
  std::uintptr_t highest_used_{0};
  std::uintptr_t peak_highest_used_{0};
};

} // namespace os::mem
