"""Build tag movesets (.t7p v2) from tkdata.bin, in the form the game holds a moveset in memory.

Every pointer is stored as an offset from the image start; the reloc table lists where those pointers are, so the
DLL only adds its allocation address. common.arc is identical for every character, so it is written once
(common.bin, already little-endian) and appended by the DLL at commonOffset.

Image layout: [mot.bin | rotsm.arc | tail | common.arc], each 16-aligned.
tail = moves table copy with one extra move "T7Tag_Assault" (standing copy that plays the tag assault effects),
       its name, and an effect property block the DLL may rewrite from T7Tag.ini.

File (little-endian):
  0x00 'T7TP'  0x04 u32 version 3  0x08 u32 characterId  0x0C u32 relocCount  0x10 u64 imageSize (incl. common)
  0x18 char[8] code  0x20 char[32] name  0x40 u32 assaultMoveId  0x44 u32 effectOffset  0x48 u32 effectRows
  0x4C u32 standingMoveId  0x50 u64 commonOffset  0x58 u64 commonSize
  0x60 u32 returnMoveId  0x64 u32 returnEffectOffset  (v3)
  0x68 u32 assaultBaseMoveId (forward dash, else standing)  0x6C u32 wallScrewReqOffset (0 = none)  (v4)
v4 wall screw: the game decides a screw with requirement 615 ("screw allowed"), which is false near a wall. Every
hit-condition list that starts with [615] gets a copy (the hitCondition table is copied to the tail and extended)
with one extra row right after it: same damage and screw reaction, requirement list = [614 opponent juggled]. The DLL
flips that one requirement id between 614 and 901 (never) so a screw still happens only once per combo.
  0x70 u32 relocs[relocCount], then 16-aligned image[0:commonOffset]
Rules for the relocation follow T7Disk::BuildFile (TKMovesets docs).
"""
import os
import re
import struct
import sys
import zlib
from collections import Counter

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from t7data import Tkdata, Moveset, DATA_ROOT, NONE, TABLES, STRIDES  # noqa: E402
from anim import mota_to_little, mota_anims, layout  # noqa: E402

VERSION = 6
HEADER = 0x70
T = {name: i for i, name in enumerate(TABLES)}
NO_ROUTE = {0x22E, 0x22F, 0x27A, 0x32B, 0x27C}
EXCLUDED = {"zak"}  # no screw / tag-in moves
EFFECT_ROWS = 24     # capacity of the effect block (rows of 0xC), the standing move's own props follow ours
INSTANT = 0x8001
# Default tag assault presentation (all instant, all self-expiring). The DLL can rewrite these from T7Tag.ini.
# Tag assault (TTT2: the partner drops in outlined white and chases the opponent hanging in the bound). The move is a
# copy of the partner's forward dash, so the partner closes the distance the screw carried the opponent.
DEFAULT_EFFECTS = [
    (INSTANT, 0x828D, 1),           # this player ignores the slow-down below (Rage Art pattern)
    (INSTANT, 0x828C, 0x004B001E),  # everything else at 75 % for 30 frames: a light, even float (no long hang)
    (INSTANT, 0x8229, 0),           # EX effect: white flash around the character
    (INSTANT, 0x84C4, 0x0100002B),  # impact sound (candidate)
    (INSTANT, 0x8293, 0x00041400),  # opponent 2D stop motion: 4 frames, shake 0x14 ("tang")
    (INSTANT, 0x8003, 0),           # camera shake medium
]
# Control returns to the point character (TTT2: the point flashes white)
RETURN_EFFECTS = [
    (INSTANT, 0x8229, 0),           # white flash
    (INSTANT, 0x828D, 0),           # slow-down immunity off again
]
SPECIAL_MOVES = [("T7Tag_Assault", DEFAULT_EFFECTS), ("T7Tag_Return", RETURN_EFFECTS), ("T7Tag_Entry", []),
                 ("T7Tag_EntryRaw", [])]   # v6: raw (standing) tag entry = standing copy, long lock
# v5 (11-21): entries cannot be cancelled (the dash copy used to carry the dash's ~500 cancels from frame 1, so a tag
# right after a move wiped its recovery). Each entry gets one cancel row: the dash's own end, moved to LOCK frames.
#   assault: 18 frames then standing (TTT2: ~17 frames from the jump-in to the first hit)
#   entry (tag combo): 20 frames of the dash, then standing. TTT2 measured (official tutorial #4, 13:53, 60 fps):
#     the partner runs in from behind ~0.1 s after the launcher hits and starts its attack ~0.45 s after it.
#   raw entry (standing tag): 45 frames standing still, then standing. TTT2 measured (Avoiding The Puddle "Raw Tag
#     Safety", 6 tags): partner in place 0.6-0.8 s, first button move 0.75 s, crouch 1.0-1.1 s after the tag.
#     A standing copy: no travel (the 30-frame dash entry carried the partner forward under a juggled opponent).
ENTRY_LOCKS = {0: (18, INSTANT), 2: (20, INSTANT), 3: (45, INSTANT)}      # special index -> (lock frame or None = dash end, move or None)
# v5 wall screw (11-21, measured with the F9 wall test): the screw reaction IS chosen at the wall, but the spin move's
# first cancels ("touching the wall" = requirement 182 -> wDm_*_WB frames 1-4, wDm_*_00 frames 1-18) turn it into a wall
# hit on frame 1. In the tag copies the spin moves get their own cancel list with those wall cancels moved to frame
# SPIN_WALL_DELAY: the screw is visible, and a spin that still touches the wall then hits it (wall combos stay).
SPIN_WALL_DELAY = 0    # 11-22: wall screw dropped (user: "a bit odd") -> the spin moves keep their own wall cancels


def align(n, a=16):
    return (n + a - 1) // a * a


# ---- boss variants (11-26, user: "add 3 characters": story bosses = the normal character + their story moves, damage
# as in versus). A story battle opens its moves with cancels whose requirements say "story battle N" (559 = N, 558/225
# = story mode flags). The variant's copy of such a cancel gets its own requirement list without those rows, so the
# branch is open in any mode. Scripted parts of the story fight (intros 564/569, AI resets to standing, phase changes,
# finishers, cut-scenes) stay closed. Story moves hit as hard as the move they replace.
STORY_FLAGS = {558, 559, 225}
SCRIPTED = re.compile(r"sKAM|PHASE|sMOVE|fatal|ChgBreak|_FIN_|Chapter|AteN|WaitingDead|nige|Blocking|Treasure|END|"
                      r"Png|^sDm_|^Dw_|Co_Chg|Story_|_y$|REMATCH")
VARIANTS = {  # file code -> base character, story battles, which story moves (target names), extra rows to drop
    # Final Battle Heihachi (story ch.14 battle 4 = 559 25): his own motions for 1,2 / 2,1 / 3,4 / 4 / b+4 / throw
    # follow-up and the final-battle Rage Art (that one still asks for a Kazuya opponent, like the story)
    "hei_fb": dict(base="hei", battles={25}, name="Final Battle Heihachi", targets=r"_7CS$|^He_m_p02$|_CH14BT4"),
    # True Devil Kazuya: always in devil form (344 = 1: laser, wings, devil upper ... as the game's devil Kazuya), plus
    # the story devil fights' moves (ch.14 battles 2-4 = 559 23-25: third-eye beam, devil 10-hit, EX moves)
    "kaz_td": dict(base="kaz", battles={23, 24, 25}, name="True Devil Kazuya", unlock={(344, 1)},
                   targets=r"CH14BT02$|^Kz_CH14_2_10com|^Kz_vipLP_EX_7CS$|^SKz_dslp_EX_7CS$|^Kz_ThirdEyeBeam_7CS$|"
                           r"^Kz_m_k00Gup$|^Kz_narakudslp$"),
    # Akuma (story ch.16 = 559 27): the story Rage Art finish; 347 (round) is dropped, the 1000 damage finish
    # becomes the normal finish's damage
    "mrx_16": dict(base="mrx", battles={27}, name="Akuma (Story)", targets=r"CH16", drop={347}),
}
STORY_NAME = re.compile(r"_?CH1\dBT\d+(_7CS)?|_?CH1\d|_7CS")


def hit_rows(ms, i):
    """hitCondition rows of move i (file form), the terminating row included"""
    h = ms.u64(ms.row("move", i) + 0x60)
    rows = []
    while h != NONE and h < ms.count("hitCondition"):
        rows.append(h)
        rs = ms.requirements(ms.u64(ms.row("hitCondition", h)))
        if not rs or rs[0][0] == 881:
            break
        h += 1
    return rows


def max_damage(ms, i):
    return max([ms.u32(ms.row("hitCondition", h) + 8) for h in hit_rows(ms, i)] or [0])


def plan_variant(ms, spec, log=print):
    """-> ({(table, row): new requirement list}, {move: versus damage})"""
    n = ms.count("move")
    names = {ms.move_name(i): i for i in range(n)}
    over, damage = {}, {}
    for i in range(n):
        src = ms.move_name(i)
        cancels = ms.move_cancels(i)
        for kind, c in cancels:
            rs = ms.requirements(c["req"])
            if c["command"] in (0x800B, 0x800C) or any(a in (564, 569) for a, _ in rs):
                continue
            t = ms.resolve(c["move_id"])
            if not 0 <= t < n:
                continue
            tn = ms.move_name(t)
            if SCRIPTED.search(tn) or any(a == 73 for a, _ in rs):   # 73: the computer player's own branches
                continue
            unlock = spec.get("unlock", set())
            story = any(a == 559 and b in spec["battles"] for a, b in rs) and re.search(spec["targets"], tn)
            if story:          # a story battle's move: the story rows (and the variant's state rows) go
                drop = STORY_FLAGS | spec.get("drop", set())
                own = [r for r in rs if r[0] not in drop and r not in unlock]
            elif unlock & set(rs) and not any(a == 559 for a, _ in rs):
                own = [r for r in rs if r not in unlock]        # a state the variant is always in (devil form)
            else:
                continue
            over[("cancel" if kind == "main" else "groupCancel", c["row"])] = own
            # versus damage = the move this branch stands in for: what the same input at the same frame does without
            # conditions (Akuma's story finish replaces 2L_n), else the move it is named after, else the source move
            same = [ms.resolve(d["move_id"]) for k, d in cancels if d["command"] == c["command"] and max_damage(ms, i)
                    and d["start"] == c["start"] and ms.requirements(d["req"]) in ([], [(0, 0)])]
            same = [x for x in same if 0 <= x < n and x != t]
            twin = same[0] if same else names.get(STORY_NAME.sub("", tn))
            if twin is None or twin == t:
                twin = i if c["command"] == 0 and c["start"] <= 1 else None
            dt = max_damage(ms, t)
            want = max_damage(ms, twin) if twin is not None else dt
            if not 0 < want <= 100:                         # no attack to compare with (or an encoded value)
                want = dt
            if dt and want != dt and t not in damage:
                damage[t] = want
            note = f" -> {want} ({ms.move_name(twin)})" if dt and want != dt else ""
            log(f"  {src} -> {tn} cmd {c['command']:#x} f{c['start']} "
                f"req {over[('cancel' if kind == 'main' else 'groupCancel', c['row'])]} dmg {dt}{note}")
    return over, damage


def build(td: Tkdata, code: str, variant=None, log=print):
    base = VARIANTS[code]["base"] if variant else code
    mot = bytearray(td.mot(base))
    arc = bytearray(td.read("mothead" + chr(92) + "mask" + chr(92) + base + "rotsm.arc"))
    common = bytearray(td.read("mothead" + chr(92) + "mask" + chr(92) + "common.arc"))
    ms = Moveset(bytes(mot))
    req_over, dmg_over = plan_variant(ms, VARIANTS[code], log) if variant else ({}, {})
    for mv, d in dmg_over.items():
        for h in hit_rows(ms, mv):
            r = ms.row("hitCondition", h)
            if ms.u32(r + 8):
                struct.pack_into("<I", mot, r + 8, d)
    req_rows = sum(len(v) + 1 for v in req_over.values())
    n_moves = ms.count("move")
    std_idx = ms.resolve(0x8001)
    std_extra = ms.extra_props(std_idx)
    dash = [i for i in range(n_moves) if ms.move_name(i).endswith("_sDASHF")]
    base_idx = dash[0] if dash else std_idx
    bases = [base_idx, std_idx, base_idx, std_idx]          # assault = dash, return = standing, entry = dash, raw = standing
    base_extra = [ms.extra_props(b) for b in bases]
    # wall screw: hit-condition lists starting with [615]
    n_hc = ms.count("hitCondition")
    starts = set()
    for i in range(n_moves):
        h = ms.u64(ms.row("move", i) + 0x60)
        if h != NONE:
            starts.add(h)
    for j in range(ms.count("projectile")):
        h = ms.u64(ms.row("projectile", j) + 0x60)
        if h != NONE:
            starts.add(h)
    screw_lists = {}
    for L in sorted(starts):
        if ms.requirements(ms.u64(ms.row("hitCondition", L))) != [(615, 0)]:
            continue
        e = L
        while e + 1 < n_hc and ms.requirements(ms.u64(ms.row("hitCondition", e))):
            e += 1
        screw_lists[L] = e                                  # rows L..e (e = terminating row)
    extra_hc = sum(e - L + 2 for L, e in screw_lists.items())
    # spin (screw) reactions: their raw main cancel rows, copied to the tail with the wall cancels delayed
    def raw_rows(i):
        idx = ms.u64(ms.row("move", i) + 0x20)
        rows = []
        while idx != NONE and idx < ms.count("cancel"):
            rows.append(idx)
            if ms.u64(ms.row("cancel", idx)) == 0x8000:
                break
            idx += 1
        return rows
    spin_moves = {i: raw_rows(i) for i in range(n_moves) if ms.move_name(i).startswith("sDw_AIR_SPIN")}
    spin_total = sum(len(r) for r in spin_moves.values())

    arc_off = align(len(mot))
    tail_off = align(arc_off + len(arc))
    moves_new = tail_off
    n_special = len(SPECIAL_MOVES)
    name_off = align(moves_new + (n_moves + n_special) * 0xB0)
    eff_rows = EFFECT_ROWS + max(len(x) for x in base_extra) + 1
    eff_offs = [align(name_off + 16 * n_special, 8) + k * align(eff_rows * 0xC, 8) for k in range(n_special)]
    wall_req = align(eff_offs[-1] + eff_rows * 0xC)
    hc_new = align(wall_req + 16)
    can_new = align(hc_new + (n_hc + extra_hc) * 0x18)      # v5: one locked cancel row per entry move
    spin_new = align(can_new + len(ENTRY_LOCKS) * 0x28)      # v5: spin moves' own cancel lists
    req_new = align(spin_new + spin_total * 0x28)            # variants: their own requirement lists
    com_off = align(req_new + req_rows * 8)
    img = bytearray(align(com_off + len(common)))
    img[0:len(mot)] = mot
    img[arc_off:arc_off + len(arc)] = arc
    img[com_off:com_off + len(common)] = common
    relocs = []

    def q(o): return struct.unpack_from("<Q", img, o)[0]
    def put(o, v): struct.pack_into("<Q", img, o, v)
    def ptr(o, v):  # pointer field = image offset v (relocated by the DLL)
        put(o, v); relocs.append(o)

    at = lambda rel: DATA_ROOT + rel
    for f in (0x8, 0x10, 0x18, 0x20):
        ptr(f, at(q(f)))
    tables = {}
    for i, t in enumerate(TABLES):
        rel, cnt = struct.unpack_from("<QQ", img, 0x150 + 16 * i)
        tables[t] = (at(rel), cnt)
        ptr(0x150 + 16 * i, at(rel))
    arc_count = struct.unpack_from("<I", arc, 0)[0]
    assert arc_count <= 6
    slots = [0] * 12
    for s in range(12):
        f = 0x280 + 8 * s
        if s % 2 == 0:
            v = q(f)
            slots[s] = at(v) if v else 0
        elif s // 2 < arc_count:
            slots[s] = arc_off + struct.unpack_from("<I", arc, 4 + 8 * (s // 2))[0]
        if slots[s]:
            ptr(f, slots[s])
        else:
            put(f, 0)
    put(0x280 + 12 * 8, 0)
    img[2] = 1  # isInitialized

    shared = mota_anims(img, com_off)
    slot1 = mota_anims(img, slots[1]) if slots[1] else []
    slot0 = mota_anims(img, slots[0]) if slots[0] else []
    done = set()
    for s in slots:
        if s and s not in done and img[s:s + 4] == b"MOTA":
            mota_to_little(img, s); done.add(s)
    mota_to_little(img, com_off)

    def row(t, i): return tables[t][0] + i * STRIDES[T[t]]

    def ref(o, target):
        v = q(o)
        if v == NONE:
            put(o, 0); return
        assert v < tables[target][1], (target, v)
        ptr(o, row(target, v))

    for i in range(n_moves):
        m = row("move", i)
        ptr(m + 0x00, at(q(m + 0x00)))
        ptr(m + 0x08, at(q(m + 0x08)))
        key = q(m + 0x10)
        lst = shared if key & 0x8000 else slot1 if key & 0x4000 else slot0
        idx = key & 0x3FFF if key & 0xC000 else key
        anim = lst[idx]
        ptr(m + 0x10, anim)
        struct.pack_into("<i", img, m + 0x68, layout(img, anim)["frames"])
        ref(m + 0x20, "cancel")
        for p, g in ((0x28, 0x30), (0x38, 0x40), (0x48, 0x50)):
            if struct.unpack_from("<i", img, m + g)[0] < 1:
                put(m + p, 0)
            else:
                ref(m + p, "cancel")
        ref(m + 0x60, "hitCondition")
        ref(m + 0x78, "voiceclip")
        ref(m + 0x80, "extraProp")
        ref(m + 0x88, "startProp")
        ref(m + 0x90, "endProp")
    for i in range(tables["projectile"][1]):
        r = row("projectile", i)
        for f, t in ((0x68, "cancel"), (0x60, "hitCondition")):
            if q(r + f):
                ref(r + f, t)
        if not struct.unpack_from("<i", img, r + 0x78)[0]:
            struct.pack_into("<i", img, r + 0x78, struct.unpack_from("<i", img, r + 0x30)[0] * 2)
        if not struct.unpack_from("<i", img, r + 0x7C)[0]:
            struct.pack_into("<i", img, r + 0x7C, struct.unpack_from("<i", img, r + 0x34)[0])
    req_at = req_new
    for t in ("cancel", "groupCancel"):
        for i in range(tables[t][1]):
            c = row(t, i)
            ids = []
            ri = q(c + 8)
            while ri != NONE and ri < tables["requirement"][1]:
                cid = struct.unpack_from("<I", img, row("requirement", ri))[0]
                if cid == 881:
                    break
                ids.append(cid); ri += 1
            own = req_over.get((t, i))
            if own is not None:
                ids = [a for a, _ in own]
            cmd0 = q(c) == 0
            opt = 1 if cmd0 else 0
            if any(x >= 0x32C and not x & 0x8000 for x in ids): opt |= 2
            if any(x >= 0x349 and not x & 0x8000 for x in ids): opt |= 4
            if 0x84CE in ids: opt |= 8
            if (not cmd0 or {0x83, 0x84, 0x85} & set(ids)) and not NO_ROUTE & set(ids): opt |= 0x10
            ei = q(c + 0x10)
            assert ei < tables["cancelExtra"][1]
            extra = struct.unpack_from("<I", img, row("cancelExtra", ei))[0]
            if extra & 0x3C00 != 0x2800: opt |= 0x40
            if extra & 0x3F == 0x16: opt |= 0x80
            mv = struct.unpack_from("<H", img, c + 0x24)[0]
            if t == "cancel" and mv & 0x8000 and (mv == 0x8024 or mv <= 0x801D or mv >= 0x8028): opt |= 0x100
            struct.pack_into("<H", img, c + 0x26, opt)
            ref(c + 0x08, "requirement")
            ref(c + 0x10, "cancelExtra")
            if own is not None:
                for k, (a, v) in enumerate(own + [(881, 0)]):
                    struct.pack_into("<II", img, req_at + 8 * k, a, v)
                ptr(c + 0x08, req_at)
                req_at += 8 * (len(own) + 1)
    for i in range(tables["reactions"][1]):
        for k in range(7):
            ref(row("reactions", i) + 8 * k, "pushback")
    for i in range(tables["inputSequence"][1]):
        r = row("inputSequence", i)
        if struct.unpack_from("<H", img, r + 2)[0]:
            ref(r + 8, "input")
        else:
            put(r + 8, 0)
    for i in range(tables["throwCamera"][1]):
        ref(row("throwCamera", i) + 8, "cameraData")
    for i in range(tables["hitCondition"][1]):
        ref(row("hitCondition", i), "requirement")
        ref(row("hitCondition", i) + 0x10, "reactions")
    for i in range(tables["pushback"][1]):
        ref(row("pushback", i) + 8, "pushbackExtra")
    for t in ("startProp", "endProp"):
        for i in range(tables[t][1]):
            ref(row(t, i), "requirement")

    # ---- tail: moves table + T7Tag_Assault ----
    old_moves = tables["move"][0]
    old_end = old_moves + n_moves * 0xB0
    img[moves_new:moves_new + n_moves * 0xB0] = img[old_moves:old_end]
    for r in [r for r in relocs if old_moves <= r < old_end]:
        relocs.append(r - old_moves + moves_new)
    special_ids = []
    for k, (mname, effects) in enumerate(SPECIAL_MOVES):
        mid = n_moves + k
        a = moves_new + mid * 0xB0
        src = moves_new + bases[k] * 0xB0
        img[a:a + 0xB0] = img[src:src + 0xB0]
        for r in [r for r in relocs if src <= r < src + 0xB0]:
            relocs.append(r - src + a)
        nb = mname.encode() + b"\0"
        img[name_off + 16 * k:name_off + 16 * k + len(nb)] = nb
        ptr(a + 0x00, name_off + 16 * k)
        if bases[k] == std_idx:
            struct.pack_into("<H", img, a + 0x54, 0x8001)  # back to standing when it ends (a dash keeps its own)
        # [effects][the base move's own properties][end]; the DLL rebuilds it the same way from the ini
        block = list(effects) + list(base_extra[k]) + [(0, 0, 0)]
        block += [(0, 0, 0)] * (eff_rows - len(block))
        for j, (f, pid, v) in enumerate(block):
            struct.pack_into("<III", img, eff_offs[k] + 0xC * j, f, pid, v)
        ptr(a + 0x80, eff_offs[k])
        special_ids.append(mid)
    # ---- tail: entry locks (v5) ----
    term = [c for k, c in ms.move_cancels(base_idx) if k == "main" and c["command"] == 0x8000]
    assert term, "base move without an end cancel"
    src = tables["cancel"][0] + term[0]["row"] * 0x28
    for j, (k, (lock, to)) in enumerate(sorted(ENTRY_LOCKS.items())):
        d = can_new + j * 0x28
        img[d:d + 0x28] = img[src:src + 0x28]
        ptr(d + 8, q(src + 8))
        ptr(d + 0x10, q(src + 0x10))
        if lock is not None:
            struct.pack_into("<IIIH", img, d + 0x18, lock, 32767, lock, to)
        ptr(moves_new + special_ids[k] * 0xB0 + 0x20, d)     # the entry's only cancel: its end
    # ---- tail: spin moves with delayed wall cancels (v5 wall screw) ----
    at_spin, spin_delayed = spin_new, 0
    for i, rows in sorted(spin_moves.items()):
        start = at_spin
        for idx in rows:
            src = tables["cancel"][0] + idx * 0x28
            d = at_spin
            img[d:d + 0x28] = img[src:src + 0x28]
            ptr(d + 8, q(src + 8))
            ptr(d + 0x10, q(src + 0x10))
            c = ms.cancel("cancel", idx)
            if c["command"] == 0 and any(r == 182 for r, _ in ms.requirements(c["req"])):
                ds, de, stt = c["det_start"], c["det_end"], c["start"]
                shift = max(0, SPIN_WALL_DELAY - ds)
                struct.pack_into("<III", img, d + 0x18, ds + shift, max(de + shift, ds + shift), stt + shift)
                spin_delayed += 1
            at_spin += 0x28
        ptr(moves_new + i * 0xB0 + 0x20, start)
    put(0x150 + T["move"] * 16, moves_new)                   # reloc entry already exists for this field
    struct.pack_into("<Q", img, 0x150 + T["move"] * 16 + 8, n_moves + n_special)

    # ---- tail: wall screw (switchable requirement + extended hitCondition table) ----
    struct.pack_into("<IIII", img, wall_req, 614, 0, 881, 0)
    hc_old = tables["hitCondition"][0]
    img[hc_new:hc_new + n_hc * 0x18] = img[hc_old:hc_old + n_hc * 0x18]
    for r in [r for r in relocs if hc_old <= r < hc_old + n_hc * 0x18]:
        relocs.append(r - hc_old + hc_new)
    remap, at_row = {}, n_hc
    for L, e in sorted(screw_lists.items()):
        remap[L] = at_row
        for src_row in [L, None] + list(range(L + 1, e + 1)):
            d = hc_new + at_row * 0x18
            sr = hc_new + (L if src_row is None else src_row) * 0x18
            img[d:d + 0x18] = img[sr:sr + 0x18]
            ptr(d, q(sr))
            ptr(d + 0x10, q(sr + 0x10))
            if src_row is None:
                put(d, wall_req)                             # same screw reaction, requirement = [614] (switchable)
            at_row += 1
    assert at_row == n_hc + extra_hc

    def retarget(field):
        v = q(field)
        if v and hc_old <= v < hc_old + n_hc * 0x18:
            L = (v - hc_old) // 0x18
            put(field, hc_new + remap.get(L, L) * 0x18)
    for i in range(n_moves + n_special):
        retarget(moves_new + i * 0xB0 + 0x60)
    for j in range(tables["projectile"][1]):
        retarget(row("projectile", j) + 0x60)
    put(0x150 + T["hitCondition"] * 16, hc_new)
    struct.pack_into("<Q", img, 0x150 + T["hitCondition"] * 16 + 8, n_hc + extra_hc)

    assert req_at == req_new + req_rows * 8
    ids = Counter(struct.unpack_from("<H", img, moves_new + i * 0xB0 + 0x5A)[0] for i in range(n_moves))
    meta = dict(char_id=ids.most_common(1)[0][0], name=VARIANTS[code]["name"] if variant else ms.name.strip("[]"), assault=special_ids[0], standing=std_idx,
                eff_off=eff_offs[0], eff_rows=eff_rows, std_props=len(std_extra), com_off=com_off,
                com_size=len(img) - com_off, ret=special_ids[1], ret_eff_off=eff_offs[1], assault_base=base_idx,
                wall_req=wall_req if screw_lists else 0, wall_lists=len(screw_lists), spin_delayed=spin_delayed, variant=len(req_over))
    return img, sorted(set(relocs)), meta


def write_blob(path, code, img, relocs, meta):
    head = struct.pack("<4sIII Q 8s 32s IIII QQ IIII", b"T7TP", VERSION, meta["char_id"], len(relocs), len(img),
                       code.encode()[:8], meta["name"].encode()[:32], meta["assault"], meta["eff_off"],
                       meta["eff_rows"], meta["standing"], meta["com_off"], meta["com_size"],
                       meta["ret"], meta["ret_eff_off"], meta["assault_base"], meta["wall_req"])
    assert len(head) == HEADER
    body = struct.pack("<%dI" % len(relocs), *relocs)
    pad = b"\0" * (image_offset(len(relocs)) - HEADER - len(body))
    with open(path, "wb") as f:
        f.write(head + body + pad + bytes(img[:meta["com_off"]]))


def write_common(path, img, meta):
    data = bytes(img[meta["com_off"]:])
    with open(path, "wb") as f:
        f.write(struct.pack("<4sII", b"T7TC", VERSION, zlib.crc32(data)) + b"\0" * 4 + data)


def image_offset(n_relocs):
    return align(HEADER + 4 * n_relocs)


def build_all(out_dir, codes, log=print):
    os.makedirs(out_dir, exist_ok=True)
    td = Tkdata()
    common_done = False
    for code in codes:
        if code in EXCLUDED:
            continue
        img, relocs, meta = build(td, code)
        write_blob(os.path.join(out_dir, code + ".t7p"), code, img, relocs, meta)
        if not common_done:
            write_common(os.path.join(out_dir, "common.bin"), img, meta)
            common_done = True
        log(f"{code} {meta['name']} id={meta['char_id']} size={meta['com_off'] // 1024}KB relocs={len(relocs)} "
            f"assault={meta['assault']} wallScrewLists={meta['wall_lists']} spinWallDelayed={meta['spin_delayed']}")
    for code in VARIANTS:
        if codes and VARIANTS[code]["base"] not in codes:
            continue
        detail = log if log is print else (lambda *a: None)     # the launcher counts one line per file
        detail(f"{code}: story moves of {VARIANTS[code]['base']} battles {sorted(VARIANTS[code]['battles'])}")
        img, relocs, meta = build(td, code, variant=True, log=detail)
        write_blob(os.path.join(out_dir, code + ".t7p"), code, img, relocs, meta)
        log(f"{code} {meta['name']} id={meta['char_id']} size={meta['com_off'] // 1024}KB branches={meta['variant']}")
    with open(os.path.join(out_dir, "version.txt"), "w") as f:
        f.write(str(VERSION))


def main():
    out_dir = sys.argv[1]
    codes = sys.argv[2:] or [c for c in Tkdata().character_codes() if c not in EXCLUDED]
    build_all(out_dir, codes)


if __name__ == "__main__":
    main()
