#pragma once

#include <array>
#include <delegate>
#include <util/bitops.hpp>
#include <mem/types.hpp>
#include <kernel/memory.hpp>
#include <cstdlib>
#include <expects>

namespace os::mem {
using namespace os::mem::types;
using namespace util::bitops;

/**
 * Sentinel physical address meaning "map at whatever address is already there".
 **/
static constexpr phys_addr_t any_addr{std::numeric_limits<uintptr_t>::max()};

/**
 * A single hardware page table entry.
 * Parameterized only on Arch so all levels share the same type —
 * this allows entry_r() to return a consistent pointer across the hierarchy.
 * AllowedFlags filtering is enforced at the table level, not in the entry.
 **/
template <typename Arch>
struct PageEntry {
  using Pflag = typename Arch::Flags;
  uintptr_t raw;

  constexpr PageEntry() noexcept = default;
  constexpr explicit PageEntry(uintptr_t v) noexcept : raw{v} {}

  constexpr uintptr_t flags_bits() const noexcept {
    return raw & static_cast<uintptr_t>(Pflag::all);
  }
  constexpr uintptr_t addr_bits() const noexcept {
    return raw & ~static_cast<uintptr_t>(Pflag::all);
  }
  constexpr Pflag flags() const noexcept {
    return static_cast<Pflag>(flags_bits());
  }
  constexpr bool has_flag(Pflag f) const noexcept {
    return util::has_flag(flags(), f);
  }
  constexpr bool empty() const noexcept {
    return addr_bits() == 0;
  }
  constexpr void add_flags(Pflag f) noexcept {
    raw |= static_cast<uintptr_t>(f);
  }
  constexpr void clear_flags(Pflag f) noexcept {
    raw &= ~static_cast<uintptr_t>(f);
  }
  constexpr void set_addr(phys_addr_t physical) noexcept {
    raw = physical.value;
  }
};

/**
 * Shared hardware-level page table mechanics.
 *
 * All page table levels share the same 4KiB-aligned 512-entry hardware layout.
 * This base captures that layout along with the non-recursive entry mechanics.
 *
 * Arch requirements:
 *   typename Arch::Flags   — flag enum (must have: all, none, present, writable,
 *                            huge, pdir, no_exec, permissive members)
 *   size_t  Arch::table_size    — entries per table (512 for x86_64)
 *   page_size_t Arch::min_pagesize
 *   page_size_t Arch::max_pagesize
 *   uintptr_t   Arch::max_memory
 **/
template <typename Arch, page_size_t Psz, typename Arch::Flags AllowedFlags>
class PageTableBase {
public:
  using Pflag        = typename Arch::Flags;
  using Map          = os::mem::Mapping<Pflag>;
  using page_entry_t = PageEntry<Arch>;

  static constexpr page_size_t page_size    = Psz;
  static constexpr page_size_t min_pagesize = Arch::min_pagesize;
  static constexpr mem_size_t  range_size   = page_size * Arch::table_size;
  static constexpr Pflag       allowed_flags = AllowedFlags;

  static constexpr bool is_page_aligned(uintptr_t addr) noexcept
  { return page_size.is_aligned(addr); }

  template <mem_addr_kind K>
  static bool is_range_aligned(mem_addr_t<K> addr) noexcept
  { return util::bits::is_aligned(range_size.value, addr.value); }

  /**
   * An entry is a page if it's at the leaf level (min page size) or has the
   * huge flag set, and is not a page directory pointer, and is page-aligned.
   * AllowedFlags masks out huge for levels that don't support it, so
   * is_huge naturally returns false at those levels.
   **/
  static bool is_page(page_entry_t entry) noexcept {
    const bool is_huge  = entry.has_flag(Pflag::huge & allowed_flags);
    const bool is_leaf  = (page_size == min_pagesize);
    const bool not_pdir = !entry.has_flag(Pflag::pdir);
    const bool aligned  = is_page_aligned(entry.addr_bits());
    return (is_huge or is_leaf) and not_pdir and aligned;
  }

  /**
   * Base: entries are never page directories (leaf behaviour).
   * PageDirectory shadows this with the real pdir-bit check.
   **/
  static bool is_page_dir(page_entry_t) noexcept { return false; }

  constexpr size_t size() { return tbl_.size(); }

  PageTableBase() = default;
  PageTableBase(virt_addr_t vstart) : linear_addr_start_{vstart} {
    static_assert(std::is_pod<decltype(tbl_)>::value,
                  "Page table array must have standard layout");
    static_assert(offsetof(PageTableBase, tbl_) == 0,
                  "tbl_ must be first member for CPU compatibility");
    static_assert(alignof(PageTableBase) == Arch::min_pagesize.bytes(),
                  "Page table must be aligned to min page size");
    Expects((reinterpret_cast<uintptr_t>(this) & (Arch::min_pagesize.bytes() - 1)) == 0);
  }

  PageTableBase(virt_addr_t lin, Pflag flags) : PageTableBase(lin) { id_map(flags); }

  PageTableBase(virt_addr_t lin, phys_addr_t phys, Pflag flags)
    : PageTableBase(lin) { map_all(phys, flags); }

  PageTableBase(const PageTableBase&)  = delete;
  PageTableBase(PageTableBase&&)       = delete;
  PageTableBase& operator=(const PageTableBase&) = delete;
  PageTableBase& operator=(PageTableBase&&)      = delete;

  virt_addr_t start_addr() const { return linear_addr_start_; }

  bool within_range(virt_addr_t addr) {
    return addr >= start_addr()
      and addr < (start_addr() + page_size * tbl_.size());
  }

  int indexof(virt_addr_t addr) {
    if (not within_range(addr)) return -1;
    int64_t i = (addr - start_addr()) / page_size;
    Ensures(i >= 0 and static_cast<size_t>(i) < tbl_.size());
    return i;
  }

  page_entry_t* entry(virt_addr_t addr) {
    auto index = indexof(addr);
    if (index < 0) return nullptr;
    return &tbl_.at(index);
  }

  void map_all(phys_addr_t phys, Pflag flags) {
    Expects(is_range_aligned(phys));
    for (auto& page : tbl_) {
      page.raw = phys.value | static_cast<uintptr_t>(flags);
      phys += page_size;
    }
  }

  void id_map(Pflag flags) { map_all(start_addr(), flags); }

  /**
   * Set flags on a given entry (leaf-safe: no is_page_dir check).
   * PageDirectory provides an override that also accepts page dir entries.
   **/
  Pflag set_flags(page_entry_t& ent, Pflag flags) {
    Expects(&ent >= tbl_.begin() && &ent < tbl_.end());
    const Pflag masked = flags & allowed_flags;
    page_entry_t new_entry{ent.addr_bits() | static_cast<uintptr_t>(masked)};
    Expects(is_page_aligned(new_entry.addr_bits()));

    if (util::has_flag(flags, Pflag::present) and !is_page(new_entry))
      return Pflag::none;

    ent = new_entry;
    Ensures(ent.flags() == masked);
    return ent.flags();
  }

  Pflag set_flags(virt_addr_t addr, Pflag flags) {
    page_entry_t* ent = entry(addr);
    Expects(ent != nullptr);
    return set_flags(*ent, flags);
  }

  Pflag set_page_flags(page_entry_t& ent, Pflag flags) {
    flags |= Pflag::huge;
    return set_flags(ent, flags);
  }

  bool is_empty() {
    for (const auto& ent : tbl_)
      if (!ent.empty() && ent.has_flag(Pflag::present))
        return false;
    return true;
  }

  /** Flat (non-recursive) map: fill entries starting at req.lin **/
  Map map(Map req) {
    auto offs = indexof(req.lin);
    if (offs < 0) return Map();

    Map res{req.lin, req.phys, req.attrs,
            mem_size_t{0}, page_sizes_t{page_size.bytes()}};

    for (auto it = tbl_.begin() + offs;
         it != tbl_.end() and res.size < req.size;
         it++, res.size += page_size)
    {
      if (req.phys != any_addr) {
        it->set_addr(req.phys);
        req.phys += page_size;
      }
      set_page_flags(*it, req.attrs);
    }
    Ensures(res);
    Ensures(page_size.is_aligned(res.size));
    return res;
  }

  Pflag flags_r(virt_addr_t addr) {
    auto* ent = entry(addr);
    if (ent == nullptr) return Pflag::none;
    return ent->flags();
  }

  bool has_flag(virt_addr_t addr, Pflag f) {
    return util::has_flag(flags_r(addr), f);
  }

  /** CPU entry point: physical address handed to hardware **/
  void* data() { return &tbl_; }

  page_entry_t& at(int i) { return tbl_.at(i); }

protected:
  alignas(Arch::min_pagesize.bytes())
  std::array<page_entry_t, Arch::table_size> tbl_ {};
  virt_addr_t linear_addr_start_{0};
};


/**
 * Leaf page table: 512 entries mapping virtual addresses to physical page frames.
 *
 * Entries are never page directory pointers. map_r/entry_r/set_flags_r do not
 * recurse; they resolve at this level.
 *
 * Corresponds to x86 PML1 / Intel "Page Table".
 **/
template <typename Arch, typename Arch::Flags AllowedFlags = Arch::Flags::all>
class PageTable : public PageTableBase<Arch, Arch::min_pagesize, AllowedFlags> {
public:
  using Base         = PageTableBase<Arch, Arch::min_pagesize, AllowedFlags>;
  using Pflag        = typename Arch::Flags;
  using Map          = os::mem::Mapping<Pflag>;
  using page_entry_t = typename Base::page_entry_t;

  using Base::page_size;
  using Base::tbl_;
  using Base::entry;
  using Base::set_flags;
  using Base::set_page_flags;
  using Base::is_page;
  using Base::map;

  PageTable() = default;
  PageTable(virt_addr_t lin) : Base(lin) {}
  PageTable(virt_addr_t lin, Pflag flags) : Base(lin, flags) {}
  PageTable(virt_addr_t lin, phys_addr_t phys, Pflag flags) : Base(lin, phys, flags) {}

  PageTable(const PageTable&)  = delete;
  PageTable(PageTable&&)       = delete;
  PageTable& operator=(const PageTable&) = delete;
  PageTable& operator=(PageTable&&)      = delete;

  page_entry_t* entry_r(virt_addr_t addr) { return entry(addr); }

  Pflag set_flags_r(virt_addr_t addr, Pflag flags) { return set_flags(addr, flags); }

  page_size_t active_page_size(virt_addr_t addr) {
    auto* e = entry(addr);
    if (e == nullptr) return 0_psz;
    return page_size;
  }

  Map map_r(Map req) {
    Expects(req);
    if (req.size == 0)
      return Map();
    if (!req.page_sizes.contains(page_size))
      return Map();
    auto res = map(req);
    Expects(res.size.value);
    Ensures(res.lin == req.lin);
    return res;
  }

  ssize_t bytes_allocated() { return sizeof(PageTable); }
  ssize_t purge_unused()    { return sizeof(PageTable); }

  void traverse(delegate<void(void*, size_t)>) { /* leaf: no subdirs */ }
};


/**
 * Interior page directory: 512 entries that each point to either a sub-level
 * page table/directory, or a large page at this level (if AllowedFlags permits
 * the huge bit).
 *
 * Corresponds to x86 PML2/PML3/PML4.
 *
 * Sub must be a PageTable or PageDirectory at the next level down.
 **/
template <typename Arch, page_size_t Psz, typename Sub,
          typename Arch::Flags AllowedFlags = Arch::Flags::all>
class PageDirectory : public PageTableBase<Arch, Psz, AllowedFlags> {
public:
  using Base         = PageTableBase<Arch, Psz, AllowedFlags>;
  using Pflag        = typename Arch::Flags;
  using Map          = os::mem::Mapping<Pflag>;
  using Subdir       = Sub;
  using page_entry_t = typename Base::page_entry_t;

  using Base::page_size;
  using Base::min_pagesize;
  using Base::allowed_flags;
  using Base::tbl_;
  using Base::entry;
  using Base::indexof;
  using Base::is_page;
  using Base::set_page_flags;
  using Base::map;

  PageDirectory() = default;
  PageDirectory(virt_addr_t lin) : Base(lin) {}
  PageDirectory(virt_addr_t lin, Pflag flags) : Base(lin, flags) {}
  PageDirectory(virt_addr_t lin, phys_addr_t phys, Pflag flags) : Base(lin, phys, flags) {}

  PageDirectory(const PageDirectory&)  = delete;
  PageDirectory(PageDirectory&&)       = delete;
  PageDirectory& operator=(const PageDirectory&) = delete;
  PageDirectory& operator=(PageDirectory&&)      = delete;

  ~PageDirectory() {
    for (auto& ent : tbl_)
      if (is_page_dir(ent))
        delete page_dir(ent);
  }

  /** An entry is a page directory pointer if it has the pdir marker bit set **/
  static bool is_page_dir(page_entry_t entry) noexcept {
    const bool has_pdir = entry.has_flag(Pflag::pdir);
    const bool not_page = !is_page(entry);
    const bool has_addr = !entry.empty();
    return has_pdir && not_page && has_addr;
  }

  /**
   * Override base set_flags to also allow page directory entries to be
   * marked present (base only allows pages).
   **/
  Pflag set_flags(page_entry_t& ent, Pflag flags) {
    Expects(&ent >= tbl_.begin() && &ent < tbl_.end());
    const Pflag masked = flags & allowed_flags;
    page_entry_t new_entry{ent.addr_bits() | static_cast<uintptr_t>(masked)};
    Expects(Base::is_page_aligned(new_entry.addr_bits()));

    if (util::has_flag(flags, Pflag::present)
        and !is_page(new_entry)
        and !is_page_dir(new_entry))
      return Pflag::none;

    ent = new_entry;
    Ensures(ent.flags() == masked);
    return ent.flags();
  }

  Pflag set_flags(virt_addr_t addr, Pflag flags) {
    page_entry_t* ent = entry(addr);
    Expects(ent != nullptr);
    return set_flags(*ent, flags);
  }

  Sub* page_dir(page_entry_t& ent) {
    Expects(&ent >= tbl_.begin() && &ent < tbl_.end());
    Expects(is_page_dir(ent));
    if (ent.empty()) return nullptr;
    return reinterpret_cast<Sub*>(ent.addr_bits());
  }

  /**
   * Allocate a sub-level page table/directory for the range starting at lin,
   * identity-mapped to phys with the given flags.
   **/
  Sub* create_page_dir(virt_addr_t lin, phys_addr_t phys, Pflag flags = Pflag::none) {
    Expects(page_size.is_aligned(lin));
    Expects(page_size.is_aligned(phys));

    page_entry_t* ent = entry(lin);
    Expects(ent != nullptr);

    Sub* sub = new Sub(lin, phys, flags);
    Expects(sub == sub->data());
    Expects(Arch::min_pagesize.is_aligned(reinterpret_cast<uintptr_t>(sub)));

    *ent = page_entry_t{
        reinterpret_cast<uintptr_t>(sub)
      | static_cast<uintptr_t>(flags & ~Pflag::huge)
      | static_cast<uintptr_t>(Pflag::present)
      | static_cast<uintptr_t>(Pflag::pdir)
    };
    return sub;
  }

  /** Recursively descend to the innermost entry covering addr **/
  page_entry_t* entry_r(virt_addr_t addr) {
    auto* ent = entry(addr);
    if (ent == nullptr) return nullptr;
    if (is_page_dir(*ent))
      return page_dir(*ent)->entry_r(addr);
    return ent;
  }

  /**
   * Propagate permissive flags (writable, present) up to a page dir entry so
   * they take effect on lower-level pages.  no_exec is cleared if needed.
   **/
  void permit_flags(page_entry_t& ent, Pflag flags) {
    Expects(&ent >= tbl_.begin() && &ent < tbl_.end());
    const auto curfl        = ent.flags();
    const bool had_no_exec  = util::has_flag(curfl, Pflag::no_exec);
    const bool want_no_exec = util::has_flag(flags, Pflag::no_exec);
    ent.add_flags(flags & allowed_flags & Pflag::permissive);
    if (had_no_exec && !want_no_exec) {
      ent.clear_flags(Pflag::no_exec);
      Ensures(!ent.has_flag(Pflag::no_exec));
    }
  }

  Pflag set_flags_r(virt_addr_t addr, Pflag flags) {
    auto* ent = entry(addr);
    Expects(ent != nullptr);

    if (!is_page_dir(*ent)) {
      if (!is_page(*ent)) return Pflag::none;
      auto applied = set_page_flags(*ent, flags);
      Ensures(util::bitops::has_flag(applied, flags));
      return applied;
    }

    permit_flags(*ent, flags);
    return page_dir(*ent)->set_flags_r(addr, flags);
  }

  page_size_t active_page_size(virt_addr_t addr) {
    auto index = indexof(addr);
    if (index < 0) return 0_psz;
    auto* ent = &tbl_.at(index);
    if (!is_page_dir(*ent)) return page_size;
    return page_dir(*ent)->active_page_size(addr);
  }

  Pflag flags_r(virt_addr_t addr) {
    auto* ent = entry_r(addr);
    if (ent == nullptr) return Pflag::none;
    return ent->flags();
  }

  bool has_flag(virt_addr_t addr, Pflag f) {
    return util::has_flag(flags_r(addr), f);
  }

  ssize_t bytes_allocated() {
    ssize_t bytes = sizeof(PageDirectory);
    for (auto& ent : tbl_)
      if (is_page_dir(ent))
        bytes += page_dir(ent)->bytes_allocated();
    return bytes;
  }

  bool is_empty() {
    for (const auto& ent : tbl_)
      if (!ent.empty() && ent.has_flag(Pflag::present))
        return false;
    return true;
  }

  ssize_t purge_unused() {
    ssize_t bytes = sizeof(PageDirectory);
    for (page_entry_t& ent : tbl_) {
      if (is_page_dir(ent)) {
        Sub* dir = page_dir(ent);
        if (dir->is_empty()) {
          delete dir;
          ent = {};
          bytes += sizeof(dir);
        } else {
          bytes += dir->purge_unused();
        }
      }
    }
    return bytes;
  }

  /** Map a single entry without descending into sub-tables **/
  Map map_entry(page_entry_t& ent, Map req) {
    Expects(&ent >= tbl_.begin() && &ent < tbl_.end());
    Expects(req);
    Expects(!is_page_dir(ent));

    if (req.phys != any_addr)
      ent.raw = static_cast<uintptr_t>(req.phys);

    req.attrs      = set_page_flags(ent, req.attrs);
    req.size       = static_cast<mem_size_t>(page_size);
    req.page_sizes = page_sizes_t{page_size.bytes()};

    Ensures(ent.addr_bits() == req.phys.value);
    return req;
  }

  /**
   * Map a single entry, descending into (or creating) a sub-directory
   * if the request cannot be satisfied at this level.
   **/
  Map map_entry_r(page_entry_t& ent, Map req) {
    Expects(this->within_range(req.lin));

    // Satisfy locally if alignment and page size constraints are met
    if (!is_page_dir(ent)
        and req.size >= page_size
        and req.page_sizes.contains(page_size)
        and page_size.is_aligned(req.lin)
        and page_size.is_aligned(req.phys))
    {
      auto res = map_entry(ent, req);
      Ensures(res and res.size == page_size);
      return res;
    }

    // Unmap: clear entry and optionally delete sub-directory
    bool is_unmap = req.phys == phys_addr_t{0} and req.attrs == Pflag::none;
    if (is_unmap) {
      Expects(min_pagesize.is_aligned(req.size));
      if (is_page_dir(ent)) {
        auto* pdir = page_dir(ent);
        if (req.size >= page_size)
          delete pdir;
        else
          return pdir->map_r(req);
      }
      if (req.size >= page_size) {
        ent = {};
        req.size = static_cast<mem_size_t>(page_size);
        return req;
      }
    }

    // Can't go deeper than min page size
    if (req.min_psize() >= page_size)
      return Map();

    // Fragment current entry into a sub-directory to enable finer mapping
    if (!is_page_dir(ent)) {
      virt_addr_t aligned_addr = page_size.align_down(req.lin);
      phys_addr_t current_addr{ent.addr_bits()};
      Pflag       current_flags = ent.flags();
      create_page_dir(aligned_addr, current_addr, current_flags);
    }

    Ensures(is_page_dir(ent));
    permit_flags(ent, req.attrs);

    auto* pdir = page_dir(ent);
    Expects(pdir != nullptr);

    auto res = pdir->map_r(req);

    if (res) {
      Ensures(res.size <= page_size);
      Ensures((req.attrs & res.attrs) == req.attrs);
    } else {
      auto sub_psize        = pdir->page_size;
      const bool wants_here = req.page_sizes.contains(page_size);
      const bool wants_sub  = req.page_sizes.contains(sub_psize);
      const bool sub_valid  = req.page_sizes.intersects(page_sizes_t{sub_psize.bytes()});
      Ensures(!wants_here or wants_sub or sub_valid);
    }

    return res;
  }

  /**
   * Recursively map linear address to phys with the given flags.
   * Descends through sub-directories, allocating new ones as needed.
   * Different page sizes may be used to satisfy the request.
   **/
  Map map_r(Map req) {
    Expects(req);
    Expects(this->within_range(req.lin));
    Expects(min_pagesize.is_aligned(req.lin));
    Expects(req.min_psize().is_aligned(req.lin));
    Expects(req.lin < Arch::max_memory);

    if (req.phys != any_addr) {
      Expects(min_pagesize.is_aligned(req.phys));
      Expects(req.min_psize().is_aligned(req.phys));
      Expects(req.phys < Arch::max_memory);
    }
    Expects(req.page_sizes.some());

    Map res{};

    for (auto i = tbl_.begin() + indexof(req.lin); i != tbl_.end(); i++) {
      page_entry_t* ent = entry(req.lin + res.size);

      const virt_addr_t ent_lin = req.lin + res.size;
      const phys_addr_t ent_phy = (req.phys == any_addr)
                                    ? any_addr
                                    : req.phys + res.size;

      Map sub{ent_lin, ent_phy, req.attrs, req.size - res.size, req.page_sizes};

      res = Map::merge_maps(res, map_entry_r(*ent, sub));
      if (!res) return res;
      if (res.size >= req.size) break;
    }

    Ensures(res);
    Ensures(req.page_sizes.intersects(res.page_sizes));
    Ensures(res.size <= mem_size_t{util::bits::roundto(min_pagesize.bytes(), req.size.value)});
    Ensures(res.lin == req.lin);
    if (req.phys != any_addr)
      Ensures(res.phys == req.phys);

    if (res.phys == any_addr)
      res.phys = phys_addr_t{entry_r(req.lin)->addr_bits()};

    return res;
  }

  void traverse(delegate<void(void*, size_t)> callback) {
    callback(this, sizeof(PageDirectory));
    for (auto& ent : tbl_)
      if (is_page_dir(ent))
        page_dir(ent)->traverse(callback);
  }
};

} // namespace os::mem
