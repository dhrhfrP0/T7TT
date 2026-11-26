"""Read Tekken 7 movesets straight from tkdata.bin (no game process needed).

Format follows T7DiskMoveset.cpp of TKMovesets (Kiloutre, GPL-3.0).
"""
import os
import re
import struct
import zlib
from Crypto.Cipher import AES

DATA_ROOT = 0x2E8
# set by the launcher to the installed game's file (…/TEKKEN 7/TekkenGame/Content/Binary/tkdata.bin)
TKDATA = os.environ.get("T7TT_TKDATA", "")
_cipher = None

# MovesetTable order and strides (t7-structures.md)
TABLES = ["reactions", "requirement", "hitCondition", "projectile", "pushback", "pushbackExtra",
          "cancel", "groupCancel", "cancelExtra", "extraProp", "startProp", "endProp", "move",
          "voiceclip", "inputSequence", "input", "parry", "cameraData", "throwCamera"]
STRIDES = [0x70, 0x08, 0x18, 0xA8, 0x10, 0x02, 0x28, 0x28, 0x04, 0x0C, 0x10, 0x10, 0xB0,
           0x04, 0x10, 0x08, 0x04, 0x0C, 0x10]
NONE = 0xFFFFFFFFFFFFFFFF


def _pad(data: bytes) -> bytes:
    return data + b"\0" * (-len(data) % 16)


def _toc(raw: bytes, cipher):
    """the decrypted table of contents, or None when the cipher is not the right one"""
    toc = cipher.decrypt(_pad(raw))[:len(raw)]
    if toc[0] == 0x78:
        try:
            toc = _inflate(toc)
        except zlib.error:
            return None
    return toc if toc[:8] == b"tsLniBNB" else None


def _open_cipher(raw: bytes, path: str):
    """The data file belongs to the installed game, and so does what opens it: it is looked up in that installation's own
    executable and recognised by the table of contents becoming readable. Nothing of it is part of this program."""
    global _cipher
    if _cipher is not None:
        return
    exe = os.path.normpath(os.path.join(os.path.dirname(path), "..", "..", "Binaries", "Win64", "TekkenGame-Win64-Shipping.exe"))
    with open(exe, "rb") as f:
        data = f.read()
    first = raw[:16]
    for m in re.finditer(rb"[!-~]{32,}", data):
        run = m.group(0)
        for i in range(len(run) - 31):
            k = run[i:i + 32]
            if sum(not (48 <= c < 58 or 65 <= c < 91 or 97 <= c < 123) for c in k) not in range(2, 12):
                continue
            c = AES.new(k, AES.MODE_ECB)
            head = c.decrypt(first)
            if (head[0] == 0x78 or head[:8] == b"tsLniBNB") and _toc(raw, c) is not None:
                _cipher = c
                return
    raise RuntimeError("this game version's data could not be opened")


def _decrypt(data: bytes) -> bytes:
    return _cipher.decrypt(_pad(data))


def _inflate(data: bytes) -> bytes:
    return zlib.decompressobj().decompress(data)


class Tkdata:
    def __init__(self, path=None):
        path = path or TKDATA
        self.f = open(path, "rb")
        self.f.seek(0, 2)
        size = self.f.tell()
        self.f.seek(0)
        assert self.f.read(16) == b"_TEKKEN7CSFILES_"
        self.f.seek(size - 0x68)
        off, stored = struct.unpack("<QQ", self.f.read(16))
        self.f.seek(off)
        raw = self.f.read(stored)
        _open_cipher(raw, path)
        toc = _toc(raw, _cipher)
        count, info, names = struct.unpack_from("<QQQ", toc, 8)
        self.entries = {}
        for i in range(count):
            end = toc.index(b"\0", names)
            name = toc[names:end].decode("ascii", "replace")
            names = end + 1
            flags, = struct.unpack_from("<Q", toc, info + i * 64 + 8)
            offset, st, sz = struct.unpack_from("<QQQ", toc, info + i * 64 + 24)
            self.entries[name.lower()] = (flags, offset, st, sz)

    def read(self, name: str) -> bytes:
        flags, offset, stored, size = self.entries[name.lower()]
        self.f.seek(offset + 0x10)
        data = _decrypt(self.f.read(stored))
        return _inflate(data[:stored]) if flags & 1 else data[:size]

    def mot(self, code: str) -> bytes:
        return self.read("mothead" + chr(92) + "bin" + chr(92) + code + "mot.bin")

    def character_codes(self):
        out = []
        for n in self.entries:
            if n.startswith("mothead\\bin\\") and n.endswith("mot.bin"):
                code = n[len("mothead\\bin\\"):-len("mot.bin")]
                if code != "test":
                    out.append(code)
        return sorted(out)


class Moveset:
    """A mot.bin in file form: references are row indexes, -1 = none."""

    def __init__(self, image: bytes):
        assert image[4:7] == b"HID", "not a T7 moveset"
        self.b = image
        self.tables = {}
        for i, t in enumerate(TABLES):
            rel, cnt = struct.unpack_from("<QQ", image, 0x150 + 16 * i)
            self.tables[t] = (DATA_ROOT + rel, cnt, STRIDES[i])
        self.name = self.cstr(struct.unpack_from("<Q", image, 0x8)[0])

    def cstr(self, rel):
        at = DATA_ROOT + rel
        return self.b[at:self.b.index(b"\0", at)].decode("ascii", "replace")

    def row(self, table, idx):
        base, cnt, stride = self.tables[table]
        assert idx < cnt, (table, idx, cnt)
        return base + idx * stride

    def count(self, table):
        return self.tables[table][1]

    def u16(self, at): return struct.unpack_from("<H", self.b, at)[0]
    def u32(self, at): return struct.unpack_from("<I", self.b, at)[0]
    def u64(self, at): return struct.unpack_from("<Q", self.b, at)[0]

    def aliases(self):
        return [self.u16(0x28 + 2 * i) for i in range(56)]

    def resolve(self, move_id):
        if move_id >= 0x8000:
            a = move_id - 0x8000
            al = self.aliases()
            return al[a] if a < len(al) else -1
        return move_id

    # --- moves ---
    def move_name(self, i):
        return self.cstr(self.u64(self.row("move", i)))

    def extra_props(self, i):
        idx = self.u64(self.row("move", i) + 0x80)
        out = []
        if idx == NONE:
            return out
        while idx < self.count("extraProp"):
            r = self.row("extraProp", idx)
            pid, val = self.u32(r + 4), self.u32(r + 8)
            if pid == 0:
                break
            out.append((self.u32(r), pid, val))
            idx += 1
        return out

    def other_props(self, i, which):  # which: 0x88 start, 0x90 end
        idx = self.u64(self.row("move", i) + which)
        table = "startProp" if which == 0x88 else "endProp"
        out = []
        if idx == NONE:
            return out
        while idx < self.count(table):
            r = self.row(table, idx)
            pid, val = self.u32(r + 8), self.u32(r + 12)
            if pid == 881:
                break
            out.append((pid, val))
            idx += 1
        return out

    def all_prop_ids(self, i):
        ids = {p for _, p, v in self.extra_props(i) if v}
        ids |= {p for p, v in self.other_props(i, 0x88) + self.other_props(i, 0x90) if v}
        return ids

    # --- cancels ---
    def requirements(self, idx):
        out = []
        if idx == NONE:
            return out
        while idx < self.count("requirement"):
            r = self.row("requirement", idx)
            c, p = self.u32(r), self.u32(r + 4)
            if c == 881:
                break
            out.append((c, p))
            idx += 1
        return out

    def cancel(self, table, idx):
        r = self.row(table, idx)
        return dict(row=idx, command=self.u64(r), req=self.u64(r + 8), extradata=self.u64(r + 0x10),
                    det_start=self.u32(r + 0x18), det_end=self.u32(r + 0x1C), start=self.u32(r + 0x20),
                    move_id=self.u16(r + 0x24), option=self.u16(r + 0x26))

    def move_cancels(self, i):
        """Main cancel list of move i, with grouped cancels expanded."""
        idx = self.u64(self.row("move", i) + 0x20)
        out = []
        if idx == NONE:
            return out
        while idx < self.count("cancel"):
            c = self.cancel("cancel", idx)
            if c["command"] == 0x800B:
                g = c["move_id"]
                while g < self.count("groupCancel"):
                    gc = self.cancel("groupCancel", g)
                    if gc["command"] == 0x800C:
                        break
                    out.append(("group", gc))
                    g += 1
            else:
                out.append(("main", c))
            if c["command"] == 0x8000:
                break
            idx += 1
        return out
