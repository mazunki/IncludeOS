#!/usr/bin/env python3
from __future__ import print_function
import sys
import re
from vmrunner import vmrunner

IMAGE = "memory_available.elf.bin"

CONFIGS = [
    {
        "name":           "default",
        "config":         "vm-default.json",
        "heap_start_min": 0x1_000_000,  # leaves room for LiveUpdate and SystemLog
        "heap_start_max": 0xFFFF_FFFF,  # stays below the 32-bit boundary
        "heap_mib_min":   64,    # observed to be 79 MiB
        "heap_mib_max":   128,   # heap can't be bigger than the provided memory
    },
    {
        "name":           "128 MiB",  # should match default
        "config":         "vm-128m.json",
        "heap_start_min": 0x1_000_000,
        "heap_start_max": 0xFFFF_FFFF,
        "heap_mib_min":   64,
        "heap_mib_max":   128,
    },
    {
        "name":           "2 GiB",
        "config":         "vm-2g.json",
        "heap_start_min": 0x1_000_000,
        "heap_start_max": 0xFFFF_FFFF,
        "heap_mib_min":   1400,  # observed to be 1519 MiB
        "heap_mib_max":   2048,
    },
    {
        "name":           "4 GiB",
        "config":         "vm-4g.json",
        "heap_start_min": 0x1_000_000,
        "heap_start_max": 0xFFFF_FFFF,
        "heap_mib_min":   2048,          # observed to be 2287 MiB
        "heap_mib_max":   4096,
    },
    {
        "name":           "6 GiB",
        "config":         "vm-6g.json",
        "heap_start_min": 0x1_0000_0000, # above 4G: heap will be located after the 32-bit area
        "heap_start_max": None,
        "heap_mib_min":   2800,  # observed to be 3072 MiB
        "heap_mib_max":   6144,
    },
    # 16GiB fails because buddy doesn't support large allocations
    # {
    #     "name":           "16 GiB",
    #     "config":         "vm-16g.json",
    #     "heap_start_min": 0x1_0000_0000,
    #     "heap_start_max": None,
    #     "heap_mib_min":   12288,  # observed to be 13312 MiB
    #     "heap_mib_max":   16384,
    # },
]


def boot_chain(configs):
    if not configs: return
    cfg, *rest = configs

    vm = vmrunner.vm(config=cfg["config"])

    def on_heap_start(line):
        m = re.search(r'HEAP_START:\s+(0x[0-9a-fA-F]+)', line)
        if not m: return
        addr = int(m.group(1), 16)

        assert addr >= cfg["heap_start_min"], \
            f"[{cfg['name']}] Heap start {addr:#x} below minimum {cfg['heap_start_min']:#x}"

        if cfg["heap_start_max"] is not None:
            assert addr <= cfg["heap_start_max"], \
                f"[{cfg['name']}] Heap start {addr:#x} unexpectedly above 4G"

    def on_heap_mib(line):
        m = re.search(r'HEAP_MiB:\s+(\d+)', line)
        if not m: return
        mib = int(m.group(1))

        assert mib >= cfg["heap_mib_min"], \
            f"[{cfg['name']}] Heap size {mib} MiB below minimum {cfg['heap_mib_min']} MiB"
        if cfg["heap_mib_max"] is not None:
            assert mib <= cfg["heap_mib_max"], \
                f"[{cfg['name']}] Heap size {mib} MiB above maximum {cfg['heap_mib_max']} MiB; " \
                f"default config should give same heap as explicit 128m"

    def on_alloc_small(line):
        assert "HEAP_ALLOC_SMALL (1 MiB): OK" in line, \
            f"[{cfg['name']}] Small allocation failed: {line}"

    def on_alloc_large(line):
        assert re.search(r'HEAP_ALLOC_LARGE \(\d+ MiB = \d+% of heap\): OK', line), \
            f"[{cfg['name']}] Large allocation failed: {line}"

    def on_alloc_oversize(line):
        m = re.search(r'HEAP_ALLOC_OVERSIZED \(\d+ MiB\): (OK|FAIL)', line)
        # TODO: hard-fail here after buddy is replaced
        if m and m.group(1) == "FAIL":
            print("WARNING: oversized allocation succeeded --- known limitation of current buddy allocator")

    vm.on_output("HEAP_START:", on_heap_start)
    vm.on_output("HEAP_MiB:",   on_heap_mib)
    vm.on_output("HEAP_ALLOC_SMALL", on_alloc_small)
    vm.on_output("HEAP_ALLOC_LARGE", on_alloc_large)
    vm.on_output("HEAP_ALLOC_OVERSIZED", on_alloc_oversize)
    vm.on_exit_success(lambda: boot_chain(rest))

    print(f"Booting VM {len(CONFIGS) - len(configs) + 1}/{len(CONFIGS)}: {cfg['name']}")
    vm.boot(image_name=IMAGE)
    return vm



boot_chain(CONFIGS)
