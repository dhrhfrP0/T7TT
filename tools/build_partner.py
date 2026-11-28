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

VERSION = 6        # file format (the DLL checks it: src/tag.cpp kVersion)
DATA_REV = 2       # move data revision: bump when the moves change so installs build again (2 = 0.2 Heaven's Door)
STAMP = f"{VERSION}.{DATA_REV}"
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
# 11-27: the same moves are also opened by the treasure-battle boss rows (563 = 7, 634) and a test mode (359 = 0x60001)
TREASURE, TESTMODE = (563, 7), (359, 0x60001)
MODE_IDS = STORY_FLAGS | {563, 634}
SCRIPTED = re.compile(r"sKAM|PHASE|sMOVE|fatal|ChgBreak|_FIN_|Chapter|AteN|WaitingDead|nige|Blocking|Treasure|END|"
                      r"Png|^sDm_|^Dw_|Co_Chg|Story_|_y$|REMATCH")
# 11-27 (user: "ignore balance, put every epilogue / boss move in"): every treasure-battle (563 = 7) and test-mode
# branch is opened too - the treasure boss's own strings (random follow-ups included). Not the ones that only walk,
# dash or step (they would take the control away)
MOVEMENT = re.compile(r"sWALK|cWALK|sDASH|sSTEP|sRUN|sJUMP|_sv$")
# 11-28 (opt-in per variant, spec no_auto=True; none uses it since the rollback): the treasure boss's AUTO follow-ups (no input) into an attack stay
# closed when they fire later than frame 1 or at random (361 / 544-547): Heihachi b+2 after uppercuts, 3 then 4, 3 u
# then twin palms, the 6 hell sweeps; Kazuya's df+2 strings. Frame-1 swaps to the story version of the same move stay
# (He_rk00 -> He_rk00_7CS ...). Strings the user wants keep their hits on buttons instead (spec "edit")
RANDOM_REQ = {361, 544, 545, 546, 547}
# 11-28 (user: "the health bars disappear"): the story Rage Art finishes hide the HUD for good with 0x829d (= 0, at the
# frame where the versus-like end would branch off: the story goes on to its cut-scene). Measured 11-28: Heihachi's final
# battle Rage Art without it keeps the bars, the treasure one with it loses them. Never in a versus move. 0x840c (the
# story's health-bar mode, story Rage Art starts and reactions only) goes too. Taken out of every boss-version move.
STRIP_PROPS = {0x829D, 0x840C}
# 11-27 (user): the computer's story moves get buttons. Free inputs on every boss: the 3-button presses.
P1, P2, K1, K2 = 1, 2, 4, 8          # u i j k (lp rp lk rk)


def press(buttons, dirs=0):
    return (0x40000000 | buttons) << 32 | dirs


LASER, WING = 20, 20                 # devil moves the versus game does not have (user OK 11-27)
TEN_HIT = dict(zip(["Kz_CH14_2_10com%d" % k for k in range(1, 7)] + ["Kz_CH14_2_10com7_CS", "Kz_CH14_2_10com8_CS",
                   "Kz_CH14_2_10com9_CS", "Kz_CH14_2_10com10_CS", "Kz_CH14_2_10com10E_CS"],
                   [4] * 6 + [3, 5, 6, 7, 14]))          # 256 in the story -> 59
# 11-27 the final battle's big hit (user: "that's it, apply it"; youtube 5Vq15E_MpOc 4:26.8): Kazuya's charge break
# there = effect 155 (0x8032, the spark burst) + the Rage Art shock wave 103 (0x8036 = bone << 8 | effect id: red
# flash, rocks, the bubble). Measured in game 11-27 (104 = white-out, too blinding). On the first active frame of
# Final Battle Heihachi's own attacks (a whiff shows it too: move properties have no "on hit" condition)
FINAL_HITS = r"^He_(rk00B2|rk00|lkrk00|rp1|rprp01|hadoh03H)_7CS$"
HIT_FX = [(FINAL_HITS, 0x8032, 0x9B), (FINAL_HITS, 0x8036, 0x67)]
VARIANTS = {  # file code -> base character, story battles, which story moves (target names), extra rows to drop
    # spec keys: battles/targets = story-battle branches opened for these target names; drop = more rows to remove;
    # unlock = a state the variant is always in; allow = story names opened although they look scripted;
    # open = (source, target) name pairs opened in any story / treasure / test-mode branch;
    # adds = (source, input, target): new input branches, written over cancel rows of other story battles;
    # damage = fixed damage by move name (else: the damage of the move the branch stands in for, never higher)
    # Final Battle Heihachi (story ch.14 battle 4 = 559 25): his own motions for 1,2 / 2,1 / 3,4 / 4 / b+4 / throw
    # follow-up and the final-battle Rage Art (that one still asks for a Kazuya opponent, like the story)
    # 11-27: entrance attacks out (user); the final battle's and the treasure boss's Rage Arts on 3-button presses (no
    # rage needed); the final battle's hit effect (see HIT_FX) on his final-battle attacks. 11-28: the EX moves out
    # (user: not needed)
    "hei_fb": dict(base="hei", battles={25}, name="Final Battle Heihachi", targets=r"_7CS$|^He_m_p02$|_CH14BT4",
                   open=[(r"^He_hadoh03$", r"^He_hadoh03H_7CS$")], allow=r"^He_RageArts0[12]_Treasure_7CS$",
                   never=r"^He_dslp_SH$|^He_rp00BEX_y$|^He_zutukiEX",
                   adds=[("std", press(P2 | K1 | K2), "He_RageArts01_CH14BT4_7CS"),  # final battle Rage Art
                         ("std", press(P1 | K1 | K2), "He_RageArts01_Treasure_7CS")],  # treasure boss Rage Art
                   hit_fx=HIT_FX,
                   # 11-28: the lightning sword of the Rage Art finishes stayed on screen for the rest of the round
                   # (the story ends the fight there). Out: the effects started before it shows (108 at frame 350,
                   # 104 at 185) and the 0x84c6 cue at 350 / 380; measured 11-28 solo: no sword left, bars kept
                   props_drop={k: {(0x8038, 0x6C), (0x8038, 0x468), (0x84C6, 0x100001E)}
                               for k in ("He_RageArts_n2_CH14BT4_7CS", "He_RageArts_n2_Treasure_7CS")}),
    # True Devil Kazuya: always in devil form (344 = 1: laser, wings, devil upper ... as the game's devil Kazuya), plus
    # the story devil fights' moves (ch.14 battles 2-4 = 559 23-25: third-eye beam, devil 10-hit, EX moves, flight,
    # chest beam, the true devil Rage Art)
    "kaz_td": dict(base="kaz", battles={18, 22, 23, 24, 25}, name="True Devil Kazuya", unlock={(344, 1)}, drop={143},
                   targets=r"CH14BT02$|^Kz_CrossCounter_st_n$|^Kz_CH14_2_10com|^Kz_vipLP_EX_7CS$|^SKz_dslp_EX_7CS$|^Kz_ThirdEyeBeam_7CS$|"
                           r"^Kz_m_k00Gup$|^Kz_narakudslp$|^SKz_sMOVE_[FB]$|"
                           r"^SKz_RageArts01_CH14BT3_7CS$|^SKz_RageArts_sp|^Kz_RageArts_n2ch14_7CS$",
                   allow=r"^SKz_sMOVE_[FB]$|^SKz_RageArts01(_CH14BT3|Treasure)_7CS$|^SKz_RageArtsSPTreasure_n$",
                   cpu=r"^Kz_dslp_EX$",
                   # 11-28 (user): the chest beam out (a big flash, no beam: its projectile type is story-only)
                   never=r"^SKz_BreastBeam",
                   # 11-27: entrance attacks out (user); the true devil Rage Art without rage. 11-28 (user): flight
                   # stays on the story's own inputs (j then 4 / 6 = 4 j+k, 6 j+k), the 3-button flight and the
                   # landing input out
                   adds=[("std", press(P1 | P2 | K1, 0x10), "Kz_CrossCounter_st"),    # ch.11 cross counter (4)
                         ("std", press(P1 | P2 | K1), "Kz_CH14_2_10com"),            # devil 10-hit (after the 4 one)
                         ("std", press(P1 | K1 | K2), "SKz_RageArts01_CH14BT3_7CS")],  # true devil Rage Art
                   # the story Rage Art goes back to standing on frame 1 without rage (33442 = 0): that row closed
                   close=[(r"^SKz_RageArts01_CH14BT3_7CS$", r"^Kz_sKAM00_$", 33442)],
                   # 11-28: the hover lands by itself when its loop ends (the story's computer lands it)
                   retarget=[("SKz_sKAM00_", 0x8000, "Kz_sKAM00_")],
                   # the flight stance makes itself the standing move (0x80bd = 1); standing again clears it
                   # 0x83c5 = the standing move (0x8001 goes there); standing again resets it to itself
                   props={"Kz_sKAM00_": [(0x8001, 0x83C5, "Kz_sKAM00_"), (0x8001, 0x80BD, 0)]},
                   damage=dict(TEN_HIT, Kz_Beam00=LASER, Kz_Beam00CH14BT02=LASER, Kz_Beam01=LASER, Kz_Beam01_7CS=LASER,
                               Kz_Beam02=LASER, Kz_Beam02_7CS=LASER, Kz_Wing00=WING, Kz_Wing00_7CS=WING,
                               Kz_Wing01E=WING, Kz_Wing01E_7CS=WING, SKz_RageArts_sp_n=50)),
    # Akuma (story ch.16 = 559 27): the story Rage Art finish; 347 (round) is dropped, the 1000 damage finish
    # becomes the normal finish's damage. Plus the computer's story moves: parry (high / low, chains, counters),
    # the story Ashura (teleport, then a fireball or focus attack by distance) and the EX recovery
    "mrx_16": dict(base="mrx", battles={27}, name="Akuma (Story)", targets=r"CH16", drop={347},
                   allow=r"^Mx_Blocking(low)?_7CS$|^Mx_RageArts2Treasure[LR]_n$",
                   open=[(r"^Mx_Blocking(low)?_7CS$|^Mx_asyura_7CS$|^Mx_asyurab_7CS$|^Mx_EXrecover_7CS$", r".")],
                   adds=[("std", press(P1 | P2 | K1), "Mx_Blocking_7CS"),
                         ("std", press(P2 | K1 | K2), "Mx_Blockinglow_7CS"),
                         ("std", press(P1 | P2 | K2), "Mx_asyura_7CS"),
                         ("std", press(P1 | K1 | K2), "Mx_EXrecover_7CS")]),
    # (11-28: the treasure boss Jin taken out, user)
    # Kazumi: the story boss's own moveset file (bs7, ch.13: tiger, beams, teleports), shown as Kazumi (id 26)
    "kzm_13": dict(base="bs7", battles=set(), name="Kazumi (Boss)", targets=r"$^", char_id=26, cpu=r"^Ka_66rp_EX$",
                   adds=[("std", press(P1 | P2 | K1), "Ka_66rp_EX"),                  # EX (computer only)
                         ("std", press(P1 | P2 | K2), "Ka_nyoidob2_bs7_7CS")]),       # ch.13 boss teleport
}
# Final Battle Heihachi also gets the treasure boss's 6-kick string (3 k follow-up -> 15 10 8 8 8 8 + 20 / 10)
VARIANTS["hei_fb"]["open"] += [(r"^He_m_k00$", r"^He_m_k00_CS$"), (r"^He_m_k0\dM?_CS$", r".")]
# (11-28 22nd round - manual 6 hell sweeps, Kazuya df+2,1,2 on buttons, versus Rage Drive, Heaven's Door start motion -
# rolled back the same day at the user's request; the spec keys edit / anim / close-any stay available)
# 11-28 (user: Heaven's Door should fly and slam like Tag 2): the body motion is the same in both games (Ndv_air_f,
# shared with Devil Jin: rise holding the opponent, flip, dive, slam); the difference is the camera. Tekken 7 plays a
# cinematic throw camera (0x843c = throw camera 0x15: low angle up into the sky, then top-down on the slam); Tag 2 keeps
# the side view, following up a little (youtube cKWTC4A8S6g, 3CY0ae4_aaM). Without 0x843c the fight camera stays on
# the side (measured in game 11-28; the common throw camera 6 went inside the bodies)
VARIANTS["kaz_td"]["props_drop"] = {m: {0x843C} for m in ("Kz_dsrp00n", "Kz_dsrp00n_EX", "SKz_dsrp00n_EX_7CS")}
# 11-28 (user: "Tag 2's brisk speed"): measured hit -> slam: Tekken 7 ~1.5 s (a long low hover holding the opponent),
# Tag 2 ~1.0-1.1 s (3CY0ae4_aaM 5.00 -> 6.05). 0x81d6 = animation speed (0x1000 = 1x, the game's own Heaven's Door
# Rage Drive uses 0x1333 at frame 80). Thrower and the thrown (Kz_dsrp00y, played from this moveset) get the same
# speed so they stay together: x1.6 up to the slam (frame 55), x1.2 for the recovery. HD_SPEED = (to the slam, after)
HD_SPEED = (0x1999, 0x1334)
for _m in ("Kz_dsrp00n", "Kz_dsrp00n_EX", "SKz_dsrp00n_EX_7CS", "Kz_dsrp00y"):
    VARIANTS["kaz_td"]["props"][_m] = [(0x8001, 0x81D6, HD_SPEED[0]), (56, 0x81D6, HD_SPEED[1])]
    VARIANTS["kaz_td"]["props_drop"].setdefault(_m, set()).update({(0x81D6, 0x1333), (0x81D6, 0x1000)})
# (tried 11-28: the cinematic camera with the faster motion - the camera animation keeps its own speed, so it looked
# at an empty sky while the two were already diving; the side view stays)
# 11-28 (user: "both"): the same Tag 2 Heaven's Door (side view, faster) for the normal characters' own files too -
# Kazuya (devil form f,n,d,df+1 / Rage Drive) and Devil Jin (same animations). Used where the mod plays its own copy
# (tag mode, both members; solo only with the wall-screw option)
def _hd_tweak(movers, receiver):
    t = dict(props={}, props_drop={})
    for m in movers + (receiver,):
        t["props"][m] = [(0x8001, 0x81D6, HD_SPEED[0]), (56, 0x81D6, HD_SPEED[1])]
        t["props_drop"][m] = {(0x81D6, 0x1333), (0x81D6, 0x1000)} | ({0x843C} if m != receiver else set())
    return t
BASE_TWEAKS = {"kaz": _hd_tweak(("Kz_dsrp00n", "Kz_dsrp00n_EX"), "Kz_dsrp00y"),
               "dvj": _hd_tweak(("Dj_dsrp00n",), "Dj_dsrp00y")}
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
    """-> ({(table, row): new requirement list}, {move: versus damage}, {(table, row): (command, move, input data)})"""
    n = ms.count("move")
    names = {ms.move_name(i): i for i in range(n)}
    over, damage, rewrite = {}, {}, {}
    unlock = spec.get("unlock", set())
    drop = MODE_IDS | spec.get("drop", set())
    allow = spec.get("allow")
    pairs = spec.get("open", [])
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
            if spec.get("never") and re.search(spec["never"], tn):
                continue                                    # moves the variant leaves closed (user's call)
            cpu = any(a == 73 for a, _ in rs)               # 73: the computer player's own branches
            freed = cpu and spec.get("cpu") and re.search(spec["cpu"], tn) and c["command"] >> 32
            if freed:
                rs, cpu = [r for r in rs if r[0] != 73], False   # ... unless the variant gives that button input
            if SCRIPTED.search(tn) and not (allow and re.search(allow, tn)) or cpu:
                continue
            foreign = any(a == 559 and b not in spec["battles"] or a == 563 and b != 7 or a == 359 and b != TESTMODE[1]
                          for a, b in rs)
            mode = not foreign and (any(a == 559 for a, _ in rs) or (558, 0) in rs or TREASURE in rs or TESTMODE in rs)
            story = any(a == 559 and b in spec["battles"] for a, b in rs) and re.search(spec["targets"], tn)
            paired = mode and any(re.search(s, src) and re.search(d, tn) for s, d in pairs)
            # 11-28: not from walking either (the treasure boss walks in and uppercuts at random: the player's walk
            # would turn into an uppercut by itself)
            boss = not foreign and (TREASURE in rs or TESTMODE in rs) and not MOVEMENT.search(tn) and not MOVEMENT.search(src)
            if spec.get("no_auto") and boss and c["command"] == 0 and max_damage(ms, t) and "RageArts" not in tn and                     (c["start"] > 1 or any(a in RANDOM_REQ for a, _ in rs)):
                boss = False                                # see RANDOM_REQ: no attack by itself
            if story or paired or boss or freed:  # a story / treasure battle's move: the mode rows (and the variant's state rows) go
                own = [r for r in rs if r[0] not in drop and r != TESTMODE and r not in unlock]
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
                want = 50
            # 11-27 (user: ignore balance): story damage stays, only the scripted finishes (1000 / 200) come down
            if dt > 100 and t not in damage:
                damage[t] = want
            note = f" -> {want} ({ms.move_name(twin) if twin is not None else '-'})" if dt > 100 else ""
            log(f"  {src} -> {tn} cmd {c['command']:#x} f{c['start']} "
                f"req {over[('cancel' if kind == 'main' else 'groupCancel', c['row'])]} dmg {dt}{note}")
    for name, d in spec.get("damage", {}).items():
        if name in names and max_damage(ms, names[name]):
            damage[names[name]] = d
    # 11-28: a story / treasure finish reached by an unconditional row (never "opened", so not capped above): Heihachi's
    # treasure Rage Art finish hit 200 in game. Every story move over 100 comes down to 50 (the versus Rage Art finish)
    for i in range(n):
        if i not in damage and max_damage(ms, i) > 100 and re.search(r"7CS|CH1\d|Treasure|^S[A-Z][a-z]_", ms.move_name(i)):
            damage[i] = 50
            log(f"  damage {ms.move_name(i)} {max_damage(ms, i)} -> 50")
    # new inputs: a cancel row of a story battle (559, never true outside the story) in the source's list becomes
    # "input -> target" without requirements; its input-buffer data is copied from a plain button cancel of the list
    for src, command, tgt in spec.get("adds", []):
        i = ms.resolve(0x8001) if src == "std" else names[src]
        cancels = [("cancel" if k == "main" else "groupCancel", c) for k, c in ms.move_cancels(i)]
        btn = command >> 32 != 0                    # a button press, or a direction alone (0 < command < 0x400)
        # a new button input must be unused; a direction may be taken over (the row sits first, in the move's own list)
        assert not btn or all(c["command"] != command for _, c in cancels), (src, hex(command), "input already used")
        tmpl = next(c for _, c in cancels if ms.requirements(c["req"]) in ([], [(0, 0)]) and
                    (c["command"] >> 32 and c["command"] & 0xFFFFFFFF if btn else 0 < c["command"] < 0x400))
        free = [(t, c) for t, c in cancels if (t, c["row"]) not in over and (t, c["row"]) not in rewrite
                and c["command"] not in (0x800B, 0x800C)]
        # story / treasure-battle rows first; else a computer-only row of the move's own list
        if not btn:                                 # a direction: only the move's own rows (groups are shared)
            free = [(t, c) for t, c in free if t == "cancel"]
        key, c = next(iter([(t, c) for t, c in free if any(a in (559, 563) for a, _ in ms.requirements(c["req"]))] +
                           [(t, c) for t, c in free if t == "cancel" and any(a == 73 for a, _ in ms.requirements(c["req"]))]))
        rewrite[(key, c["row"])] = (command, names[tgt], tmpl["extradata"], 1, 32767, 1)
        over[(key, c["row"])] = []
        log(f"  new input {command:#x} -> {tgt} (row {key} {c['row']}, was -> {ms.move_name(ms.resolve(c['move_id']))})")
    # rows that are closed for good (a requirement nothing meets: story battle 999)
    for s_re, t_re, rid, *only in spec.get("close", []):     # rid None = any row; optional command filter
        for i in range(n):
            if re.search(s_re, ms.move_name(i)):
                for k, c in ms.move_cancels(i):
                    if c["command"] in (0x800B, 0x800C, 0x8000) or only and c["command"] != only[0]:
                        continue
                    if re.search(t_re, ms.move_name(ms.resolve(c["move_id"]))) and                             (rid is None or any(a == rid for a, _ in ms.requirements(c["req"]))):
                        over[("cancel" if k == "main" else "groupCancel", c["row"])] = [(559, 999)]
                        log(f"  closed {ms.move_name(i)} -> {ms.move_name(ms.resolve(c['move_id']))} row {c['row']}")
    # rows of a move's own list sent to another move (same command, timing and requirements)
    for src, command, tgt in spec.get("retarget", []):
        for k, c in ms.move_cancels(names[src]):
            if k == "main" and c["command"] == command:
                key = ("cancel", c["row"])
                rewrite[key] = (command, names[tgt], c["extradata"], c["det_start"], c["det_end"], c["start"])
                log(f"  retarget {src} {command:#x} row {c['row']} -> {tgt}")
    # 11-28 rows edited in place: dict(src, tgt = name patterns; new = other target; cmd = a button input instead (its
    # input-buffer data from a plain row of the moveset with that input; early = taken from frame 1 on, like the
    # game's buffered rows); req = new requirements; first_only = the
    # other matching rows of the move close)
    tmpl_rows = {}
    for e in spec.get("edit", []):
        for i in range(n):
            if not re.search(e["src"], ms.move_name(i)):
                continue
            done = False
            for k, c in ms.move_cancels(i):
                if k != "main" or c["command"] in (0x8000, 0x800B, 0x800C) or                         not re.search(e["tgt"], ms.move_name(ms.resolve(c["move_id"]))):
                    continue
                key = ("cancel", c["row"])
                if done and e.get("first_only"):
                    over[key] = [(559, 999)]; rewrite.pop(key, None)
                    continue
                command, extra, d0, d1 = c["command"], c["extradata"], c["det_start"], c["det_end"]
                if e.get("cmd") is not None:
                    command = e["cmd"]
                    if command not in tmpl_rows:
                        tmpl_rows[command] = [d for j in range(n) for kk, d in ms.move_cancels(j) if kk == "main" and
                                              d["command"] == command and ms.requirements(d["req"]) in ([], [(0, 0)])]
                    same = [d for d in tmpl_rows[command] if d["start"] == c["start"]]
                    t_ = (same or tmpl_rows[command])[0]
                    extra = t_["extradata"]
                    d0, d1 = (t_["det_start"], t_["det_end"]) if same and not e.get("early") else (1, c["start"])
                if e.get("xd") is not None:                 # a cancel-extra row index (moveset specific)
                    extra = e["xd"]
                tgt = names[e["new"]] if e.get("new") else ms.resolve(c["move_id"])
                rewrite[key] = (command, tgt, extra, d0, d1, c["start"])
                if "req" in e:
                    over[key] = list(e["req"])
                done = True
                log(f"  edit {ms.move_name(i)} row {c['row']} -> {ms.move_name(tgt)} [{command:#x}] f{c['start']} "
                    f"w{d0}-{d1} req {over.get(key, 'kept')}")
    # longer input windows for opened branches (the story's flight needs back/forward within 2 frames of j)
    for s_re, t_re, end, *more in spec.get("window", []):    # optional: a plain input instead of the story's,
        cmd, first = (list(more) + [None, None])[:2]         # an earlier first frame
        for i in range(n):
            if not re.search(s_re, ms.move_name(i)):
                continue
            for k, c in ms.move_cancels(i):
                key = ("cancel" if k == "main" else "groupCancel", c["row"])
                if key in over and key not in rewrite and re.search(t_re, ms.move_name(ms.resolve(c["move_id"]))):
                    rewrite[key] = (cmd or c["command"], ms.resolve(c["move_id"]), c["extradata"],
                                    first or c["det_start"], end, c["start"])
    return over, damage, rewrite


def build(td: Tkdata, code: str, variant=None, log=print):
    base = VARIANTS[code]["base"] if variant else code
    mot = bytearray(td.mot(base))
    arc = bytearray(td.read("mothead" + chr(92) + "mask" + chr(92) + base + "rotsm.arc"))
    common = bytearray(td.read("mothead" + chr(92) + "mask" + chr(92) + "common.arc"))
    ms = Moveset(bytes(mot))
    req_over, dmg_over, rewrite = plan_variant(ms, VARIANTS[code], log) if variant else ({}, {}, {})
    for mv, d in dmg_over.items():
        for h in hit_rows(ms, mv):
            r = ms.row("hitCondition", h)
            if ms.u32(r + 8):
                struct.pack_into("<I", mot, r + 8, d)
    for (t, i), (command, mv, extra, d0, d1, st) in rewrite.items():
        r = ms.row(t, i)
        struct.pack_into("<QQQIIIH", mot, r, command, ms.u64(r + 8), extra, d0, d1, st, mv)
    req_rows = sum(len(v) + 1 for v in req_over.values())
    # variants: extra move properties (frame, id, value) added to a move's own list (copied to the tail)
    names_ = {ms.move_name(i): i for i in range(ms.count("move"))}
    spec_ = VARIANTS[code] if variant else BASE_TWEAKS.get(code, {})
    prop_over = {}
    for k, v in spec_.get("props", {}).items():       # value may be a move name (-> its index)
        prop_over[names_[k]] = ms.extra_props(names_[k]) + [(f, pid, names_[x] if isinstance(x, str) else x)
                                                              for f, pid, x in v]
    # 11-27 hit effects: (move name pattern, property, value) set on the move's first active frame
    for pat, pid, val in spec_.get("hit_fx", []):
        for nm, i in names_.items():
            if re.search(pat, nm):
                first = ms.u32(ms.row("move", i) + 0xA0)
                prop_over[i] = prop_over.get(i, ms.extra_props(i)) + [(max(first, 1), pid, val)]
    # 11-28 another move's animation (with its active frames and air timings): the move's own frame events and
    # cancel rows from the old first active frame on move with the hit (user: Heaven's Door in its Tag 2 motion)
    for dst, src_m in spec_.get("anim", {}).items():
        i, j = names_[dst], names_[src_m]
        rd, rs_ = ms.row("move", i), ms.row("move", j)
        old, shift = ms.u32(rd + 0xA0), ms.u32(rs_ + 0xA0) - ms.u32(rd + 0xA0)
        for off, size in ((0x8, 8), (0x10, 8), (0x6C, 4), (0x70, 4), (0x74, 4), (0xA0, 4), (0xA4, 4), (0xA8, 4)):
            mot[rd + off:rd + off + size] = mot[rs_ + off:rs_ + off + size]
        prop_over[i] = [(f + shift if f < 0x8000 and f >= old - 2 else f, p, v)
                        for f, p, v in prop_over.get(i, ms.extra_props(i))]
        for k, c in ms.move_cancels(i):
            if k == "main" and c["start"] >= old - 2:
                r = ms.row("cancel", c["row"])
                d0, d1, st = struct.unpack_from("<III", mot, r + 0x18)
                struct.pack_into("<III", mot, r + 0x18, d0 + shift if d0 >= old - 2 else d0,
                                 min(d1 + shift, 32767) if d1 >= old - 2 else d1, st + shift)
        log(f"  anim {dst} <- {src_m} (frames {shift:+d})")
    for k, pids in spec_.get("props_drop", {}).items():      # a property id, or (id, value) for one row
        prop_over[names_[k]] = [r for r in prop_over.get(names_[k], ms.extra_props(names_[k]))
                                if r[1] not in pids and (r[1], r[2]) not in pids]
    if variant:                                         # 11-28 story-only properties (STRIP_PROPS) out of every move
        for i in names_.values():
            rows = prop_over.get(i, ms.extra_props(i))
            if any(r[1] in STRIP_PROPS for r in rows):
                prop_over[i] = rows = [r for r in rows if r[1] not in STRIP_PROPS]
                log(f"  stripped {[hex(p) for p in STRIP_PROPS]} from {ms.move_name(i)}")
            # 11-28: damage dealt by properties (measured: Heihachi's treasure finish 120, the true devil laser 400 on
            # screen). 0x823d (low 16 bits) and 0x821c (hit damage) over 100 -> the versus finish values, 0x821b (a damage
            # scale in %, 200 in the story finishes, measured: the laser 92 -> 492) -> 100. Kz_RageArts_n2: 0x823d 60, 0x821c <= 20
            cap = {0x823D: 60, 0x821B: 100, 0x821C: 20}
            def capped(p, v):
                return (v & 0xFFFF0000) | cap[p] if p in cap and isinstance(v, int) and 100 < (v & 0xFFFF) < 0xFFFF else v
            if any(capped(p, v) != v for _, p, v in rows):
                prop_over[i] = [(f, p, capped(p, v)) for f, p, v in rows]
                log(f"  property damage of {ms.move_name(i)} capped")
    prop_rows = sum(len(v) + 1 for v in prop_over.values())
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
    props_new = align(req_new + req_rows * 8)
    com_off = align(props_new + prop_rows * 0xC)
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
        if i in prop_over:
            for k, row_ in enumerate(prop_over[i] + [(0, 0, 0)]):
                struct.pack_into("<III", img, props_new + 0xC * k, *row_)
            ptr(m + 0x80, props_new)
            props_new += 0xC * (len(prop_over[i]) + 1)
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
    meta = dict(char_id=(variant and VARIANTS[code].get("char_id")) or ids.most_common(1)[0][0], name=VARIANTS[code]["name"] if variant else ms.name.strip("[]"), assault=special_ids[0], standing=std_idx,
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
        f.write(STAMP)


def main():
    out_dir = sys.argv[1]
    codes = sys.argv[2:] or [c for c in Tkdata().character_codes() if c not in EXCLUDED]
    build_all(out_dir, codes)


if __name__ == "__main__":
    main()
