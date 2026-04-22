// -*-C++-*-
// This file is a part of the IncludeOS unikernel - www.includeos.org
//
// Copyright 2017 IncludeOS AS, Oslo, Norway
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

#include <arch.hpp>
#include <info>
#include <kernel/memory.hpp>

__attribute__((weak))
void __arch_init_paging()
{
  INFO("x86", "Paging not enabled by default on 32-bit");
}

namespace os::mem {
  __attribute__((weak))
  Map map(Map m, const char* name) {  // FIXME: use params, remove or mark unused
    return {};
  }

  Map protect(virt_addr_t linear, mem_size_t len, Permission flags)
  {
    return {};
  }

  Map protect_existing(virt_addr_t linear, mem_size_t len, Permission flags)
  {
    return {};
  }

  Permission permissions(virt_addr_t)
  {
    return Permission::Open;  // permissive lie
  }

  page_sizes_t supported_page_sizes() {
    return page_sizes_t{4096};
  }
}
