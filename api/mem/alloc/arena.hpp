
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
#include <stdexcept>
#include <string_view>
#include <algorithm>

#include <mem/mem.hpp>

namespace os::mem {

class arena_resource final : public mem_resource {
public:
  explicit arena_resource(mem_config cfg)
    : mem_resource(cfg), cfg_(cfg), cursor_(cfg.region.start)
  {}

  static arena_resource* create_at(mem_region region) noexcept {
    return create_resource_at<arena_resource>(region);
  }

  /**
   * carves at least `capacity` usable bytes out of parent and self-hosts a child
   * arena in it. the header overhead is added on top automatically, not something
   * the caller has to compute.
   */
  static arena_resource* create_from(mem_resource& parent, std::size_t capacity) {
    const std::size_t total = sizeof(arena_resource) + alignof(arena_resource) + capacity;
    mem_region region = parent.allocate_region(total, alignof(arena_resource));
    arena_resource* child = create_at(region);
    if (!child) {
      parent.deallocate_region(region, alignof(arena_resource));
    }
    return child;
  }

  size_t bytes_used() const noexcept override {
    return cursor_ - cfg_.region.start;
  }

  size_t bytes_free() const noexcept override {
    return cfg_.region.end - cursor_;
  }

  uintptr_t highest_used() const noexcept override {
    return peak_cursor_;
  }

protected:
  std::string_view name() const noexcept override { return "arena"; }

  void strat_summary() const noexcept override {
    std::println("  cursor:        {:#x}", cursor_);
    std::println("  peak_cursor:   {:#x}", peak_cursor_);
  }

  // you get exactly what you asked for
  std::size_t strat_good_size(std::size_t bytes, std::size_t /*alignment*/) const noexcept override {
    return bytes;
  }

  // no minimum :)
  std::size_t strat_alignment() const noexcept override {
    return 1;
  }

  std::uintptr_t strat_allocate(std::size_t bytes, std::size_t alignment) override {
    const std::uintptr_t aligned = align_up(cursor_, alignment);
    if (aligned + bytes > cfg_.region.end || aligned + bytes < aligned) {
      throw std::bad_alloc(); // out of space, or overflow
    }
    cursor_ = aligned + bytes;
    peak_cursor_ = std::max(peak_cursor_, cursor_);
    return aligned;
  }

  // we only support freeing the most recent allocation, not
  // arbitrary allocations in the middle
  //
  // beware of dragons!
  //   p1 = arena->allocate(4096);
  //   p2 = arena->allocate(4096);
  //   arena->deallocate(p1);  // semantically invalid turns into no-op
  //   arena->deallocate(p2);  // arena is set to invalid p1
  //   arena->deallocate(p1);  // arena is back to a valid state (not a double free)
  void strat_deallocate(uintptr_t addr, std::size_t bytes, std::size_t) noexcept override {
    if (addr + bytes == cursor_) {
      cursor_ = addr;
    }
  }

  // just rewind the cursor
  void strat_deallocate_all() noexcept override {
    cursor_ = cfg_.region.start;
  }

  // works only if blk is the most recent allocation
  bool strat_expand(mem_region& blk, std::size_t delta) noexcept override {
    if (blk.end != cursor_) {
      return false; // something else was allocated after blk
    }
    const std::uintptr_t new_end = cursor_ + delta;
    if (new_end > cfg_.region.end || new_end < cursor_) {
      return false; // out of space
    }

    cursor_ = new_end;
    peak_cursor_ = std::max(peak_cursor_, cursor_);
    blk = mem_region{ blk.start, new_end };
    return true;
  }

  // beware of dragons! ideally only used on the last allocation
  bool strat_shrink(mem_region& blk, std::size_t delta) noexcept override {
    if (blk.end == cursor_) {
      cursor_ -= delta;
    }
    blk = mem_region{ blk.start, blk.end - delta };
    return true;
  }

  mem_region strat_allocate_all() noexcept override { return take_remaining(); }

  mem_region strat_allocate_largest() noexcept override { return take_remaining(); }

private:
  mem_region take_remaining() noexcept {
    const std::uintptr_t start = cursor_;
    const std::uintptr_t end = cfg_.region.end;
    if (start >= end) {
      return mem_region{};
    }
    cursor_ = end;
    peak_cursor_ = std::max(peak_cursor_, cursor_);
    return mem_region{ start, end };
  }

  mem_config cfg_;

  std::uintptr_t cursor_;
  std::uintptr_t peak_cursor_{cursor_};
};

} // namespace os::mem
