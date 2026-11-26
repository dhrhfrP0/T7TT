"""Tekken 7 animation / MOTA endian conversion (disk = big-endian, game memory = little-endian).

Layout (from the format as documented by TKMovesets, reimplemented):
  0xC8: u16 type, u16 bones, u32 frames, bones x u32, frames x bones x 12 bytes. Every 4-byte unit after +4 swaps.
  0x64: u16 type, u16 bones, bones x u16 descriptor, then at P = 4 + 2*bones:
        u16 frames, u8 shortShift, u8 floatShift, u16 floatBones, floatBones x f32,
        base pose (per bone 3 x (s16 if descriptor 4..7 else f32)), keyframe table ((frames+14)//16 x u32),
        then a byte-oriented bitstream (endian free).
"""
import struct


def _rev(b: bytearray, off: int, width: int):
    b[off:off + width] = b[off:off + width][::-1]


def layout(b, off, size_limit=None):
    """Return dict describing the animation at b[off:]. Detects endian from the type tag."""
    tag = struct.unpack_from("<H", b, off)[0]
    big = tag in (0x6400, 0xC800)
    e = ">" if big else "<"
    typ, bones = struct.unpack_from(e + "HH", b, off)
    if typ not in (0x64, 0xC8) or not 0 < bones <= 255:
        raise ValueError("bad animation header %#x" % tag)
    L = dict(big=big, type=typ, bones=bones)
    if typ == 0xC8:
        frames = struct.unpack_from(e + "I", b, off + 4)[0]
        L.update(frames=frames, size=8 + bones * 4 + bones * 12 * frames)
        return L
    post = 4 + bones * 2
    frames, = struct.unpack_from(e + "H", b, off + post)
    fbones, = struct.unpack_from(e + "H", b, off + post + 4)
    descs = struct.unpack_from(e + "%dH" % bones, b, off + 4)
    nfloat = sum(1 for d in descs if not 4 <= d <= 7)
    if not frames or nfloat != fbones:
        raise ValueError("bad 0x64 animation")
    base = post + 6 + fbones * 4
    keyf = base + bones * 6 + fbones * 6
    chunks = (frames + 14) // 16
    size = keyf + chunks * 4
    for c in range(chunks):
        o = struct.unpack_from(e + "I", b, off + keyf + c * 4)[0]
        cur = keyf + o
        for _ in range(bones * 3):
            ln = b[off + cur] >> 2
            if ln == 0:
                raise ValueError("zero channel")
            cur += ln
        size = max(size, cur)
    L.update(frames=frames, post=post, floatBones=fbones, descs=descs, base=base, keyframes=keyf, chunks=chunks, size=size)
    return L


def to_little(b: bytearray, off: int):
    """Convert one animation in place to little-endian. Returns its frame count."""
    L = layout(b, off)
    if not L["big"]:
        return L["frames"]
    if L["type"] == 0xC8:
        for o in range(4, L["size"], 4):
            _rev(b, off + o, 4)
    else:
        pose = L["base"]
        for d in L["descs"]:
            w = 2 if 4 <= d <= 7 else 4
            for _ in range(3):
                _rev(b, off + pose, w)
                pose += w
        for i in range(L["floatBones"]):
            _rev(b, off + L["post"] + 6 + i * 4, 4)
        for i in range(L["chunks"]):
            _rev(b, off + L["keyframes"] + i * 4, 4)
        for i in range(L["bones"]):
            _rev(b, off + 4 + i * 2, 2)
        _rev(b, off + L["post"], 2)
        _rev(b, off + L["post"] + 4, 2)
    _rev(b, off, 2)
    _rev(b, off + 2, 2)
    L2 = layout(b, off)
    assert not L2["big"] and L2["frames"] == L["frames"] and L2["size"] == L["size"]
    return L["frames"]


def mota_anims(b, off):
    """Absolute offsets of every animation in the MOTA at b[off:] (either endian)."""
    assert b[off:off + 4] == b"MOTA", "not a MOTA"
    e = ">" if b[off + 5] == 1 else "<"
    count = struct.unpack_from(e + "I", b, off + 12)[0]
    return [off + struct.unpack_from(e + "I", b, off + 20 + 4 * i)[0] for i in range(count)]


def mota_to_little(b: bytearray, off: int):
    """Convert a MOTA (header + all member animations) in place to little-endian."""
    if b[off:off + 4] != b"MOTA" or b[off + 5] != 1:
        return
    anims = mota_anims(b, off)
    for a in sorted(set(anims)):
        to_little(b, a)
    count = len(anims)
    for i in range(count):
        _rev(b, off + 20 + 4 * i, 4)
    _rev(b, off + 4, 2)    # endian flag pair (little, big)
    _rev(b, off + 6, 2)
    _rev(b, off + 8, 4)
    _rev(b, off + 12, 4)
    b[off + 0x11] = 0 if b[off + 0x11] else 1
    assert b[off + 5] == 0 and mota_anims(b, off) == anims
