#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 Pr0Luci
"""Build a board file from the user's own stock firmware module.

usage: python3 -I tools/provision/awvoice-board-extract.py --elf <stock module> --map <map file> --out <board file>

The board values (power-supply, line-feed, tone and audio presets of the board) are the line chip's
operating parameters - functional configuration constants, not executable code: the chip maker's DSP
patch never ships, and the daemon has no use for it. This tool builds a board file from the user's own
stock firmware on the user's own machine; a ready board file for this router also ships in this
repository (see the README for what ships and why). The map file lists
which objects of the stock module go into which section; the map for this router model ships as tools/provision/aw1000.map,
and a map for another board names that board's own objects and stays with that board's build.

Map file, one line per section ("#" starts a comment):
    <section id> <object name in the stock module> <size in bytes>
    <section id> file:<path of a local file> <size in bytes>
    section id: lower-case letters, digits, underscore, at most 15 characters
A file section carries values that are not stored as an object in the module (for example constants that the stock code
loads as immediate operands); such files are built on the user's own machine like the map file.

Board file (all numbers little endian):
    magic "AWBC" | u32 version (1) | u32 section count | 32 bytes sha256 of the stock module
    per section:  16 bytes id (zero padded) | u32 length | length bytes
    u32 CRC-32 of everything before it

Objects that contain relocations (pointers) are refused: their bytes are not final in a relocatable module.
"""
import argparse
import hashlib
import re
import struct
import sys
import zlib

MAGIC = b"AWBC"
VERSION = 1
SHT_SYMTAB, SHT_NOBITS, SHT_REL, SHT_RELA = 2, 8, 9, 4
ID_RX = re.compile(r"[a-z0-9_]{1,15}$")


class BoardError(Exception):
    pass


def parse_elf(data):
    """Return (sections, symbols) of a 32-bit little-endian ELF object: sections as dicts, symbols as name -> (shndx, value, size)."""
    if data[:4] != b"\x7fELF" or data[4] != 1 or data[5] != 1:
        raise BoardError("not a 32-bit little-endian ELF file")
    shoff, = struct.unpack_from("<I", data, 0x20)
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", data, 0x2E)
    raw = [struct.unpack_from("<IIIIIIIIII", data, shoff + i * shentsize) for i in range(shnum)]
    sections = [dict(name=r[0], type=r[1], offset=r[4], size=r[5], link=r[6], info=r[7]) for r in raw]

    def string(table, index):
        start = sections[table]["offset"] + index
        return data[start:data.index(b"\0", start)].decode("ascii", "replace")

    for s in sections:
        s["name"] = string(shstrndx, s["name"])
    symbols = {}
    for s in sections:
        if s["type"] != SHT_SYMTAB:
            continue
        for k in range(s["size"] // 16):
            name, value, size, _info, _other, shndx = struct.unpack_from("<IIIBBH", data, s["offset"] + k * 16)
            if name:
                symbols[string(s["link"], name)] = (shndx, value, size)
    return sections, symbols


def relocated(sections, data, target, start, end):
    """True if a relocation entry patches bytes of section index `target` inside [start, end)."""
    for s in sections:
        if s["type"] not in (SHT_REL, SHT_RELA) or s["info"] != target:
            continue
        step = 8 if s["type"] == SHT_REL else 12
        for k in range(s["size"] // step):
            offset, = struct.unpack_from("<I", data, s["offset"] + k * step)
            if start <= offset < end:
                return True
    return False


def read_map(text):
    entries = []
    for number, line in enumerate(text.splitlines(), 1):
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        parts = line.split()
        if len(parts) != 3 or not ID_RX.match(parts[0]) or not parts[2].isdigit():
            raise BoardError("map line %d: expected '<id> <object> <size>'" % number)
        if any(parts[0] == e[0] for e in entries):
            raise BoardError("map line %d: section id %s repeated" % (number, parts[0]))
        entries.append((parts[0], parts[1], int(parts[2])))
    if not entries:
        raise BoardError("map file lists no sections")
    return entries


def load_file(path):
    try:
        with open(path, "rb") as f:
            return f.read()
    except OSError as err:
        raise BoardError("cannot read %s: %s" % (path, err.strerror))


def build(elf, map_text, load=load_file):
    sections, symbols = parse_elf(elf)
    body = bytearray()
    entries = read_map(map_text)
    for ident, obj, size in entries:
        if obj.startswith("file:"):
            content = load(obj[5:])
            if len(content) != size:
                raise BoardError("section %s: file has %d bytes, the map says %d" % (ident, len(content), size))
            body += ident.encode().ljust(16, b"\0") + struct.pack("<I", size) + content
            continue
        if obj not in symbols:
            raise BoardError("object for section %s not found in the module" % ident)
        shndx, value, found = symbols[obj]
        if found != size:
            raise BoardError("section %s: object has %d bytes, the map says %d" % (ident, found, size))
        if shndx >= len(sections) or sections[shndx]["type"] == SHT_NOBITS:
            raise BoardError("section %s: object has no stored contents" % ident)
        start = sections[shndx]["offset"] + value
        if value + size > sections[shndx]["size"]:
            raise BoardError("section %s: object runs past its section" % ident)
        if relocated(sections, elf, shndx, value, value + size):
            raise BoardError("section %s: object contains relocations, its bytes are not final" % ident)
        body += ident.encode().ljust(16, b"\0") + struct.pack("<I", size) + elf[start:start + size]
    head = MAGIC + struct.pack("<II", VERSION, len(entries)) + hashlib.sha256(elf).digest()
    blob = head + bytes(body)
    return blob + struct.pack("<I", zlib.crc32(blob) & 0xFFFFFFFF)


def parse_board(blob):
    """Inverse of build(): return {id: bytes}; raises BoardError on any damage."""
    if len(blob) < 48 or blob[:4] != MAGIC:
        raise BoardError("not a board file")
    crc, = struct.unpack_from("<I", blob, len(blob) - 4)
    if crc != zlib.crc32(blob[:-4]) & 0xFFFFFFFF:
        raise BoardError("checksum mismatch")
    version, count = struct.unpack_from("<II", blob, 4)
    if version != VERSION:
        raise BoardError("unsupported version %d" % version)
    pos, out = 44, {}
    for _ in range(count):
        if pos + 20 > len(blob) - 4:
            raise BoardError("truncated section header")
        ident = blob[pos:pos + 16].rstrip(b"\0").decode()
        length, = struct.unpack_from("<I", blob, pos + 16)
        pos += 20
        if pos + length > len(blob) - 4:
            raise BoardError("truncated section %s" % ident)
        out[ident] = blob[pos:pos + length]
        pos += length
    if pos != len(blob) - 4:
        raise BoardError("trailing bytes")
    return out


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--elf", required=True)
    ap.add_argument("--map", required=True)
    ap.add_argument("--out", required=True)
    args = ap.parse_args(argv)
    try:
        with open(args.elf, "rb") as f:
            elf = f.read()
        with open(args.map, encoding="utf-8") as f:
            blob = build(elf, f.read())
        sections = parse_board(blob)
    except (OSError, BoardError) as err:
        print("error: %s" % err, file=sys.stderr)
        return 1
    with open(args.out, "wb") as f:
        f.write(blob)
    print("board file: %d sections, %d bytes, stock module sha256 %s" % (
        len(sections), len(blob), hashlib.sha256(elf).hexdigest()))
    for ident, body in sections.items():
        print("  %-15s %5d bytes" % (ident, len(body)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
