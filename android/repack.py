#!/usr/bin/env python3
"""Repack the DDNet APK: replace/add entries, keep every other byte of entry data,
and lay stored entries out on their required alignment (4 for resources, 4096 for .so).

apksigner copies local entries verbatim and only appends the signing block, so
whatever alignment this produces is what ends up in the signed APK.

usage: repack.py <src.apk> <out.apk> <file>=<entryname> ...
"""
import struct
import sys
import zipfile
import zlib

src_path, out_path = sys.argv[1], sys.argv[2]
def deflate(raw):
    co = zlib.compressobj(9, zlib.DEFLATED, -15)
    return co.compress(raw) + co.flush()

repl = {}
for spec in sys.argv[3:]:
    local, entry = spec.split("=", 1)
    repl[entry] = open(local, "rb").read()

zin = zipfile.ZipFile(src_path)
src = open(src_path, "rb")
out = open(out_path, "wb")

entries = []
for info in zin.infolist():
    name = info.filename
    src.seek(info.header_offset)
    lh = src.read(30)
    if lh[:4] != b"PK\x03\x04":
        raise SystemExit("bad local header for " + name)
    (ver, flags, method, dostime, dosdate, crc, csize, usize,
     nlen, elen) = struct.unpack_from("<HHHHHIIIHH", lh, 4)
    lname = src.read(nlen)
    src.read(elen)
    if name in repl:
        raw = repl[name]
        method = 8
        crc = zlib.crc32(raw) & 0xFFFFFFFF
        usize = len(raw)
        comp = deflate(raw)
        csize = len(comp)
        name_bytes = name.encode()
    else:
        comp = src.read(csize)
        name_bytes = lname
    flags &= ~0x8
    entries.append([name, name_bytes, flags, method, dostime, dosdate, crc, csize, usize, comp,
                    info.external_attr, info.create_system, info.create_version, info.extract_version])

for name in repl:
    if any(e[0] == name for e in entries):
        continue
    raw = repl[name]
    comp = deflate(raw)
    entries.append([name, name.encode(), 0x800, 8, 0, 0x21, zlib.crc32(raw) & 0xFFFFFFFF,
                    len(comp), len(raw), comp, 0o100644 << 16, 3, 20, 20])

central = []
for (name, name_bytes, flags, method, dostime, dosdate, crc, csize, usize, comp,
     ext_attr, create_system, create_version, extract_version) in entries:
    stored = method == 0
    align = 4096 if (stored and name.endswith(".so")) else (4 if stored else 1)
    base = out.tell()
    if align > 1:
        pad = 4 + ((align - ((base + 30 + len(name_bytes) + 4) % align)) % align)
        extra = struct.pack("<HH", 0xD935, pad - 4) + b"\0" * (pad - 4)
    else:
        extra = b""
    out.write(struct.pack("<IHHHHHIIIHH", 0x04034B50, 20, flags, method, dostime, dosdate,
                          crc, csize, usize, len(name_bytes), len(extra)))
    out.write(name_bytes)
    out.write(extra)
    offset = out.tell()
    if align > 1 and offset % align:
        raise SystemExit("alignment failed for " + name)
    out.write(comp)
    central.append((name_bytes, flags, method, dostime, dosdate, crc, csize, usize, extra,
                    base, ext_attr, create_system, create_version, extract_version))

cd_start = out.tell()
for (name_bytes, flags, method, dostime, dosdate, crc, csize, usize, extra, offset,
     ext_attr, create_system, create_version, extract_version) in central:
    out.write(struct.pack("<IHHHHHHIIIHHHHHII", 0x02014B50,
                          (create_system << 8) | create_version, extract_version,
                          flags, method, dostime, dosdate, crc, csize, usize,
                          len(name_bytes), len(extra), 0, 0, 0, ext_attr, offset))
    out.write(name_bytes)
    out.write(extra)
cd_size = out.tell() - cd_start
out.write(struct.pack("<IHHHHIIH", 0x06054B50, 0, 0, len(central), len(central),
                      cd_size, cd_start, 0))
out.close()
print("repacked %s: %d entries" % (out_path, len(central)))
