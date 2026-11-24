#!/usr/bin/env python3
"""Make libDDNet.so pull in libmusor.so: add one DT_NEEDED entry.

The name goes into the 32-byte gap between .eh_frame and .text (mapped, read-only,
inside the first PT_LOAD), the entry replaces DT_FLAGS_1 (DF_1_NOW, which duplicates
the DT_FLAGS BIND_NOW that stays in place), and DT_STRSZ is widened so the name sits
inside the string table the loader believes it has.
"""
import struct
import sys

SRC = "/tmp/libDDNet.so"
DST = sys.argv[1] if len(sys.argv) > 1 else "/tmp/libDDNet.patched.so"
NAME = b"libmusor.so\x00"

data = bytearray(open(SRC, "rb").read())

# gcc_except_table start = .rodata end; .text start from the section table
import subprocess
import re
sec = subprocess.run(["readelf", "-SW", SRC], capture_output=True, text=True).stdout
def addr_of(name):
    for line in sec.splitlines():
        m = re.match(r"\s*\[\s*\d+\]\s+(\S+)\s+(\S+)\s+([0-9a-f]+)\s+([0-9a-f]+)\s+([0-9a-f]+)", line)
        if m and m.group(1) == name:
            return int(m.group(3), 16), int(m.group(4), 16), int(m.group(5), 16)
    raise SystemExit("section not found: " + name)

eh_addr, eh_off, eh_size = addr_of(".eh_frame")
text_addr, text_off, text_size = addr_of(".text")
str_addr, str_off, str_size = addr_of(".dynstr")
hole = eh_off + eh_size
if text_off - hole < len(NAME):
    raise SystemExit("no gap before .text")
if any(data[hole:hole + len(NAME)]):
    raise SystemExit("gap is not empty")

data[hole:hole + len(NAME)] = NAME
name_off = hole - str_off  # vaddr - strtab vaddr, since .eh_frame vaddr == offset in this file

dyn = subprocess.run(["readelf", "-dW", SRC], capture_output=True, text=True).stdout
# .dynamic file offset via section table
dyn_addr, dyn_off, dyn_size = addr_of(".dynamic")
entries = dyn_size // 16
replaced = None
strsz_slot = None
for i in range(entries):
    tag, val = struct.unpack_from("<qQ", data, dyn_off + 16 * i)
    if tag == 0x6FFFFFFB:  # DT_FLAGS_1
        replaced = i
    if tag == 10:  # DT_STRSZ
        strsz_slot = i
    if tag == 0:
        break
if replaced is None or strsz_slot is None:
    raise SystemExit("dynamic entries not found")

struct.pack_into("<qQ", data, dyn_off + 16 * replaced, 1, name_off)          # DT_NEEDED
struct.pack_into("<qQ", data, dyn_off + 16 * strsz_slot, 10,
                 (hole + len(NAME)) - str_off)                                # DT_STRSZ

open(DST, "wb").write(data)
print("libDDNet patched -> %s" % DST)
print("  name vaddr 0x%x off 0x%x strtab 0x%x" % (hole, hole, str_addr))
print("  dt_needed slot %d, dt_strsz slot %d" % (replaced, strsz_slot))
