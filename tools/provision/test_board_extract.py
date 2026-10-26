#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 Pr0Luci
"""Tests for awvoice-board-extract.py: python3 -I tools/provision/test_board_extract.py"""
import importlib.util
import os
import struct
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("extract", os.path.join(HERE, "awvoice-board-extract.py"))
ex = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ex)


def make_elf(objects, relocate=None, nobits=False):
    """Tiny relocatable ELF: .data holding `objects` (name -> bytes), .symtab, .strtab, .shstrtab, optional .rel.data."""
    data, syms, strtab = b"", [], b"\0"
    for name, blob in objects.items():
        syms.append((len(strtab), len(data), len(blob)))
        strtab += name.encode() + b"\0"
        data += blob
    symtab = b"\0" * 16 + b"".join(struct.pack("<IIIBBH", n, v, s, 0x11, 0, 1) for n, v, s in syms)
    shstr = b"\0.data\0.symtab\0.strtab\0.shstrtab\0.rel.data\0"
    pos = {n: shstr.index(n.encode()) for n in (".data", ".symtab", ".strtab", ".shstrtab", ".rel.data")}
    rel = struct.pack("<II", relocate, 0x0102) if relocate is not None else b""
    body, offs = b"", {}
    for key, blob in (("data", data), ("sym", symtab), ("str", strtab), ("shs", shstr), ("rel", rel)):
        offs[key] = 0x34 + len(body)
        body += blob
    shoff = 0x34 + len(body)
    headers = [(0,) * 10,
               (pos[".data"], 8 if nobits else 1, 3, 0, offs["data"], len(data), 0, 0, 4, 0),
               (pos[".symtab"], 2, 0, 0, offs["sym"], len(symtab), 3, 1, 4, 16),
               (pos[".strtab"], 3, 0, 0, offs["str"], len(strtab), 0, 0, 1, 0),
               (pos[".shstrtab"], 3, 0, 0, offs["shs"], len(shstr), 0, 0, 1, 0),
               (pos[".rel.data"], 9, 0, 0, offs["rel"], len(rel), 2, 1, 4, 8)]
    ident = b"\x7fELF" + bytes([1, 1, 1]) + bytes(9)
    head = ident + struct.pack("<HHIIIIIHHHHHH", 1, 40, 1, 0, 0, shoff, 0, 52, 0, 0, 40, len(headers), 4)
    return head + body + b"".join(struct.pack("<IIIIIIIIII", *h) for h in headers)


class BoardExtract(unittest.TestCase):
    objs = {"first_object": bytes(range(8)), "second_object": b"\xaa" * 12}
    mapping = "alpha first_object 8\nbeta second_object 12  # comment\n"

    def test_round_trip(self):
        blob = ex.build(make_elf(self.objs), self.mapping)
        self.assertEqual(ex.parse_board(blob), {"alpha": self.objs["first_object"], "beta": self.objs["second_object"]})

    def test_file_section(self):
        files = {"words.bin": b"\x01\x02\x03\x04"}
        blob = ex.build(make_elf(self.objs), self.mapping + "gamma file:words.bin 4\n", load=files.__getitem__)
        self.assertEqual(ex.parse_board(blob)["gamma"], files["words.bin"])

    def test_file_section_wrong_size_or_missing_is_refused(self):
        with self.assertRaises(ex.BoardError):
            ex.build(make_elf(self.objs), "gamma file:words.bin 5\n", load=lambda path: b"1234")
        with self.assertRaises(ex.BoardError):
            ex.build(make_elf(self.objs), "gamma file:/nonexistent/words.bin 4\n")

    def test_wrong_size_is_refused(self):
        with self.assertRaises(ex.BoardError):
            ex.build(make_elf(self.objs), "alpha first_object 9\n")

    def test_missing_object_is_refused(self):
        with self.assertRaises(ex.BoardError):
            ex.build(make_elf(self.objs), "alpha nothing_here 8\n")

    def test_relocated_object_is_refused(self):
        with self.assertRaises(ex.BoardError):
            ex.build(make_elf(self.objs, relocate=4), "alpha first_object 8\n")

    def test_relocation_elsewhere_is_fine(self):
        ex.build(make_elf(self.objs, relocate=12), "alpha first_object 8\n")

    def test_contentless_object_is_refused(self):
        with self.assertRaises(ex.BoardError):
            ex.build(make_elf(self.objs, nobits=True), "alpha first_object 8\n")

    def test_bad_map_lines(self):
        for text in ("", "Alpha first_object 8\n", "alpha first_object\n", "a x 1\na y 2\n"):
            with self.assertRaises(ex.BoardError):
                ex.read_map(text)

    def test_damage_is_detected(self):
        blob = bytearray(ex.build(make_elf(self.objs), self.mapping))
        blob[60] ^= 1
        with self.assertRaises(ex.BoardError):
            ex.parse_board(bytes(blob))
        with self.assertRaises(ex.BoardError):
            ex.parse_board(b"nope")


if __name__ == "__main__":
    unittest.main()
