"""T7Tag 런처: 모드 선택(솔로/태그), 파트너 선택, 설치·제거, 게임 실행."""
import configparser
import glob
import os
import re
import shutil
import sys
import time
import zipfile
import tkinter as tk
from tkinter import messagebox, ttk

VERSION = "0.2 테스트"
FROZEN = getattr(sys, "frozen", False)
HERE = os.path.dirname(os.path.abspath(sys.executable if FROZEN else __file__))
BUNDLE = getattr(sys, "_MEIPASS", HERE)
TOOLS = BUNDLE if FROZEN else os.path.abspath(os.path.join(HERE, "..", "tools"))
DLL_SRC = os.path.join(BUNDLE, "dinput8.dll") if FROZEN else os.path.join(HERE, "..", "bin", "dinput8.dll")


def find_game():
    """TEKKEN 7 (Steam app 389730): registry -> steam library folders"""
    roots = []
    try:
        import winreg
        for hive, key, val in ((winreg.HKEY_CURRENT_USER, r"Software\Valve\Steam", "SteamPath"),
                               (winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\WOW6432Node\Valve\Steam", "InstallPath")):
            try:
                with winreg.OpenKey(hive, key) as k:
                    roots.append(os.path.normpath(winreg.QueryValueEx(k, val)[0]))
            except OSError:
                pass
    except ImportError:
        pass
    libs = []
    for r in roots:
        libs.append(r)
        try:
            t = open(os.path.join(r, "steamapps", "libraryfolders.vdf"), encoding="utf-8", errors="replace").read()
            libs += [os.path.normpath(p) for p in re.findall(r'"path"\s+"([^"]+)"', t)]
        except OSError:
            pass
    for lib in libs:
        g = os.path.join(lib, "steamapps", "common", "TEKKEN 7")
        if os.path.exists(os.path.join(g, r"TekkenGame\Binaries\Win64\TekkenGame-Win64-Shipping.exe")):
            return g
    return ""


GAME = find_game()
WIN64 = os.path.join(GAME, r"TekkenGame\Binaries\Win64") if GAME else ""
DLL_DST = os.path.join(WIN64, "dinput8.dll")
INI = os.path.join(WIN64, "T7Tag.ini")
PARTNER_DIR = os.path.join(WIN64, "T7Tag_partners")
CFG_DIR = os.path.join(os.environ["LOCALAPPDATA"], r"TekkenGame\Saved\Config\WindowsNoEditor")
ENGINE_INI = os.path.join(CFG_DIR, "Engine.ini")
USER_INI = os.path.join(CFG_DIR, "GameUserSettings.ini")
BAK = ".t7tag.bak"
MARK = b"T7Tag start"  # string inside our dll

# tkdata code -> display name (zak = Tekken Force grunt: no screw / tag-in moves, excluded)
CHARACTERS = {
    "pau": "폴", "law": "로우", "kin": "킹", "yos": "요시미츠", "hwo": "화랑", "xia": "샤오유", "jin": "진",
    "bry": "브라이언", "hei": "헤이하치", "kaz": "카즈야", "ste": "스티브", "jac": "잭-7", "ask": "아스카",
    "dvj": "데빌 진", "fen": "펭", "lil": "릴리", "dra": "드라구노프", "leo": "레오", "lar": "라스",
    "asa": "알리사", "ita": "클라우디오", "ltn": "카타리나", "dnc": "럭키 클로이", "arb": "샤힌", "mut": "조시",
    "crz": "기가스", "kzm": "카즈미", "nin": "니나", "frv": "마스터 레이븐", "lee": "리", "bob": "밥",
    "mrx": "고우키", "kum": "쿠마", "pan": "판다", "edd": "에디", "elz": "엘리자", "mig": "미겔",
    "mry": "기스", "mrz": "녹티스", "ann": "안나", "lei": "레이", "aki": "아머킹", "mar": "마두크",
    "jul": "줄리아", "nsa": "니건", "zaf": "자피나", "gan": "간류", "nsb": "르로이", "nsc": "파쿰람",
    "knm": "쿠니미츠", "nsd": "리디아", "ja4": "잭-4", "bs7": "데빌 카즈미(보스)",
    "ykz": "영 카즈야(스토리)", "yhe": "영 헤이하치(스토리)",
}
NAME_TO_CODE = {v: k for k, v in CHARACTERS.items()}
P2_MODES = {"Auto": "자동 (CPU가 알아서 태그)", "Key": "P키로 직접", "Off": "끔"}

ENGINE_BLOCK = """
; --- T7Tag: keep running when alt-tabbed ---
[/Script/Engine.Engine]
bPauseOnLossOfFocus=False

[ConsoleVariables]
t.IdleWhenNotForeground=0

[Audio]
UnfocusedVolumeMultiplier=1.0
; --- T7Tag end ---
"""


def is_our_dll(path):
    try:
        return MARK in open(path, "rb").read()
    except OSError:
        return False


def backup(path):
    if os.path.exists(path) and not os.path.exists(path + BAK):
        shutil.copy2(path, path + BAK)


def restore(path):
    if os.path.exists(path + BAK):
        shutil.copy2(path + BAK, path)
        os.remove(path + BAK)


def set_ini_keys(path, section, values):
    """Edit keys of one section in a UE4 ini, keeping everything else as is."""
    lines = open(path, encoding="utf-8-sig").read().splitlines() if os.path.exists(path) else []
    out, in_sec, done = [], False, set()
    for ln in lines:
        s = ln.strip()
        if s.startswith("[") and s.endswith("]"):
            if in_sec:
                out += [f"{k}={v}" for k, v in values.items() if k not in done]
            in_sec = s == f"[{section}]"
        elif in_sec and "=" in s and s.split("=", 1)[0] in values:
            k = s.split("=", 1)[0]
            out.append(f"{k}={values[k]}")
            done.add(k)
            continue
        out.append(ln)
    if in_sec:
        out += [f"{k}={v}" for k, v in values.items() if k not in done]
    elif not any(l.strip() == f"[{section}]" for l in lines):
        out += ["", f"[{section}]"] + [f"{k}={v}" for k, v in values.items()]
    open(path, "w", encoding="utf-8").write("\n".join(out) + "\n")


def tag_data_ready(out_dir):
    if TOOLS not in sys.path:
        sys.path.insert(0, TOOLS)
    import build_partner
    try:
        ok = open(os.path.join(out_dir, "version.txt")).read().strip() == build_partner.STAMP
    except OSError:
        return False
    # 0.2: boss versions (hei_fb ...) are new files next to the characters: an older install builds again
    return ok and os.path.exists(os.path.join(out_dir, "common.bin")) and         all(os.path.exists(os.path.join(out_dir, v + ".t7p")) for v in build_partner.VARIANTS)


def build_tag_data(out_dir, progress=None):
    """Convert every character's moveset from tkdata.bin into tag movesets (about 40 s, ~540 MB)."""
    if TOOLS not in sys.path:
        sys.path.insert(0, TOOLS)
    import build_partner
    import t7data
    from t7data import Tkdata
    t7data.TKDATA = os.path.join(GAME, r"TekkenGame\Content\Binary\tkdata.bin")
    codes = [c for c in Tkdata().character_codes() if c not in build_partner.EXCLUDED]
    done = [0]

    def log(_line):
        done[0] += 1
        if progress:
            progress(done[0], len(codes) + len(build_partner.VARIANTS))
    build_partner.build_all(out_dir, codes, log=log)


DEFAULT_ASSAULT = "0x828D:1, 0x828C:0x00370018, 0x8229:0, 0x8293:0x000A1400, 0x8003:0"   # 11-22: no in-move sound (user: "too weird")
# 11-22: TTT2 assault measured: 0.15-0.3 s hit freeze + white flash; sounds: metallic activation + heavy hit
DEFAULT_ASSAULT_SOUND = "FX_HIT_L03*2"   # picked by ear from the game's own hit sounds
OLD_ASSAULTS = {"0x828D:1, 0x828C:0x0028002D, 0x8229:0, 0x84C4:0x0100002B, 0x8293:0x00041400, 0x8003:0",
                "0x828D:1, 0x828C:0x004B001E, 0x8229:0, 0x84C4:0x0100002B, 0x8293:0x00041400, 0x8003:0",
                "0x828D:1, 0x828C:0x00370018, 0x8229:0, 0x84C4:0x0100002B, 0x8293:0x000A1400, 0x8003:0"}
DEFAULT_RETURN = "0x8229:0, 0x828D:0"
PADS = {"LB": "LB", "RB": "RB", "LS": "왼쪽 스틱 누르기", "RS": "오른쪽 스틱 누르기", "None": "패드 태그 끔"}
KEYS = {"Space": "스페이스바", "O": "O", "Shift": "Shift", "F": "F"}


# keys the launcher window decides; every other key already in T7Tag.ini (sound picked in game with F11, tuned frames,
# [Debug] ...) is kept when the launcher writes the file (11-22: an install wiped the user's chosen assault sound)
LAUNCHER_KEYS = {("General", "Mode"), ("Fixes", "FocusFix"), ("Fixes", "NoRageArts"), ("Tag", "TagKey"), ("Tag", "PadTag"), ("Tag", "P2Tag"),
                 ("Tag", "TagIn"), ("Tag", "WallScrew"), ("Tag", "ModelSwap"), ("Effects", "Assault"), ("Effects", "Return"),
                 ("Launcher", "Borderless")}


def merge_ini(text, path):
    """new file text from the template, with the values the user already had for every key the launcher does not own;
    keys only in the old file are added to the end of their section"""
    old = {}
    if os.path.exists(path):
        sec = ""
        for line in open(path, encoding="utf-8-sig", errors="replace").read().splitlines():
            t = line.strip()
            if t.startswith("[") and t.endswith("]"):
                sec = t[1:-1]
            elif "=" in t and not t.startswith(";"):
                k, v = t.split("=", 1)
                old[(sec, k.strip())] = v
    out, sec, seen = [], "", set()
    lines = text.split("\n")

    def flush(section):
        for (s, k), v in old.items():
            if s == section and (s, k) not in seen and (s, k) not in LAUNCHER_KEYS:
                out.append(f"{k}={v}")
                seen.add((s, k))
    for line in lines:
        t = line.strip()
        if t.startswith("[") and t.endswith("]"):
            if sec:
                while out and out[-1] == "":
                    out.pop()
                flush(sec)
                out.append("")
            sec = t[1:-1]
            out.append(line)
            continue
        if "=" in t and not t.startswith(";"):
            k = t.split("=", 1)[0].strip()
            seen.add((sec, k))
            if (sec, k) in old and (sec, k) not in LAUNCHER_KEYS:
                line = f"{k}={old[(sec, k)]}"
        out.append(line)
    flush(sec)
    for s in sorted({s for s, _ in old} - {l.strip()[1:-1] for l in lines if l.strip().startswith("[")}):
        out.append(f"\n[{s}]")
        flush(s)
    return "\n".join(out).rstrip("\n") + "\n"


def ini_text(mode, focus, norage, p2mode, jump, borderless, assault=DEFAULT_ASSAULT, key="Space", ret=DEFAULT_RETURN,
             wall=0, model=1, pad="LB"):
    return (f"[General]\nMode={mode}\n\n[Fixes]\nFocusFix={focus}\nNoRageArts={norage}\n\n"
            f"[Tag]\nTagKey={key}\nPadTag={pad}\nP2Tag={p2mode}\nP2TagKey=P\n"
            f"; tag key during your own attack = assault the moment it screws (ms); assault only this long after the screw\n"
            f"; (ms); after the screwed opponent lands, partner standing idle this long -> main back (ms)\n"
            f"AssaultBufferMs=700\nAssaultWindowMs=500\nReturnIdleMs=700\n"
            f"; tag entry lock frames (60 = 1 s): tag combo / raw tag (TTT2 measured ~0.45 s / 0.75-1.0 s). F7 reapplies.\n"
            f"ComboTagFrames=20\nRawTagFrames=60\nRawTagButtonFrames=45\n"
            f"; tag crash: while down, tag + right punch when the partner has rage or the active one is at or below this %\n"
            f"CrashHealthPct=40\n"
            f"; assault: Landing = the partner keeps control until the opponent touches the floor (+ReturnIdleMs),\n"
            f"; Move = TTT2 as measured: the main is back as soon as the partner's one move is over\n"
            f"AssaultReturn=Landing\nWallScrew={wall}\n"
            f"; the partner's look too (experimental engine calls; 0 = only moves and health change)\nModelSwap={model}\n"
            f"TagIn={'Jump' if jump else 'None'}\nMaxHealth=175\nRedPercent=40\nRecoverPerSec10=40\nOverlay=1\n\n"
            f"[Effects]\n; tag assault presentation (T7Tag_Assault). id:value, or frame@id:value\n"
            f"; 0x828D slow-down immunity, 0x828C slow (speed%<<16|frames), 0x8229 white EX flash,\n"
            f"; 0x84C4 sound (bank<<24|n), 0x8293 opponent stop-motion, 0x8003 camera shake. F7 in game reloads.\n"
            f"Assault={assault}\n"
            f"; Wwise sounds at the assault / when the main comes back ('+' plays several together)\n"
            f"AssaultSound={DEFAULT_ASSAULT_SOUND}\nReturnSound=\n"
            f"; control back to the main after the partner's attack: white flash, slow-down immunity off\n"
            f"Return={ret}\n\n"
            f"[HUD]\n; the game's P1 health bar (fractions of the window): top edge X0..X1 at Y, height H, bottom edge\n"
            f"; shifted right by Slant. TTT2 layout: the partner's bar = a copy of it, BackH x height, right above,\n"
            f"; shifted outward by BackShift. Portrait at the outer top corner: PortraitW x PortraitH.\n"
            f"Y=0.0840\nH=0.0319\nX0=0.1273\nX1=0.4281\nSlant=0.0148\nBackH=0.6\nBackShift=0.006\n"
            f"PortraitX=0.064\nPortraitY=0.031\nPortraitW=0.053\nPortraitH=0.132\n\n"
            f"[Launcher]\nBorderless={borderless}\n")


class App:
    def __init__(self, root):
        self.root = root
        root.title(f"T7TT - 철권 7 태그 모드 ({VERSION})")
        root.resizable(False, False)
        self.mode = tk.StringVar(value="Tag")
        self.focus = tk.IntVar(value=1)
        self.norage = tk.IntVar(value=1)
        self.borderless = tk.IntVar(value=1)
        self.p2mode = tk.StringVar(value=P2_MODES["Auto"])
        self.jump = tk.IntVar(value=0)
        self.wall = tk.IntVar(value=0)
        self.model = tk.IntVar(value=1)
        self.assault = DEFAULT_ASSAULT
        self.ret = DEFAULT_RETURN
        self.key = tk.StringVar(value=KEYS["Space"])
        self.pad = tk.StringVar(value=PADS["LB"])
        self.load()

        pad = dict(padx=12, pady=4, sticky="w")
        tk.Label(root, text="모드", font=("Malgun Gothic", 11, "bold")).grid(row=0, column=0, **pad)
        tk.Radiobutton(root, text="태그 기능 끄기 (일반 철권 7)", variable=self.mode, value="Solo").grid(row=1, column=0, **pad)
        tk.Radiobutton(root, text="태그 기능 켜기 (선택 화면에서 솔로/태그 선택, 태그 키로 교대, 스크류 중 태그 키 = 어썰트)",
                       variable=self.mode, value="Tag").grid(row=2, column=0, **pad)

        tag = tk.LabelFrame(root, text="태그 설정 (솔로/태그·파트너는 게임 캐릭터 선택 화면에서 고릅니다)")
        tag.grid(row=3, column=0, padx=12, pady=4, sticky="we")
        tk.Label(tag, text="선택 화면: 왼쪽 = TAG / 오른쪽 = SOLO → 결정 → MAIN 결정 → PARTNER 결정\n"
                           "의상: 캐릭터에 커서를 두고 U(패드 X) → 목록에서 결정",
                 fg="#2050a0", justify="left").grid(row=0, column=0, columnspan=2, padx=6, pady=3, sticky="w")
        tk.Label(tag, text="태그 키 (키보드)").grid(row=1, column=0, padx=6, pady=3, sticky="w")
        ttk.Combobox(tag, textvariable=self.key, values=list(KEYS.values()), state="readonly",
                     width=24).grid(row=1, column=1, padx=6, sticky="w")
        tk.Label(tag, text="태그 버튼 (패드·스틱)").grid(row=7, column=0, padx=6, pady=3, sticky="w")
        ttk.Combobox(tag, textvariable=self.pad, values=list(PADS.values()), state="readonly",
                     width=24).grid(row=7, column=1, padx=6, sticky="w")
        tk.Label(tag, text="P2 태그").grid(row=2, column=0, padx=6, pady=3, sticky="w")
        ttk.Combobox(tag, textvariable=self.p2mode, values=list(P2_MODES.values()), state="readonly",
                     width=24).grid(row=2, column=1, padx=6, sticky="w")
        tk.Checkbutton(tag, text="모습도 파트너로 바꾸기 (문제 있으면 끄세요)", variable=self.model).grid(
            row=5, column=0, columnspan=2, padx=6, pady=3, sticky="w")
        tk.Label(tag, text="※ 끄면 모습은 MAIN 그대로이고 기술·체력만 파트너로 바뀝니다.",
                 fg="gray").grid(row=6, column=0, columnspan=2, padx=6, pady=(0, 4), sticky="w")

        tk.Label(root, text="옵션", font=("Malgun Gothic", 11, "bold")).grid(row=4, column=0, **pad)
        tk.Checkbutton(root, text="알트탭 해도 게임 안 멈춤", variable=self.focus).grid(row=5, column=0, **pad)
        tk.Checkbutton(root, text="레이지 아츠 삭제 (레이지 드라이브는 유지)", variable=self.norage).grid(row=6, column=0, **pad)
        tk.Checkbutton(root, text="테두리 없는 창 모드로 바꾸기 (알트탭·태그 표시용 권장)",
                       variable=self.borderless).grid(row=7, column=0, **pad)
        tk.Label(root, text="※ 온라인 대전에서는 레이지 아츠 삭제·태그가 자동으로 꺼집니다.", fg="gray").grid(row=8, column=0, **pad)

        bar = tk.Frame(root)
        bar.grid(row=9, column=0, padx=12, pady=10, sticky="we")
        tk.Button(bar, text="설치 / 적용", width=12, command=self.install).pack(side="left", padx=2)
        tk.Button(bar, text="게임 실행", width=12, command=self.launch).pack(side="left", padx=2)
        tk.Button(bar, text="제거", width=8, command=self.uninstall).pack(side="left", padx=2)
        tk.Button(bar, text="피드백 파일 만들기", width=16, command=self.feedback).pack(side="left", padx=2)
        self.status = tk.Label(root, text="", fg="blue")
        self.status.grid(row=10, column=0, padx=12, pady=(0, 10), sticky="w")
        self.refresh_status()

    def load(self):
        if not os.path.exists(INI):
            return
        c = configparser.ConfigParser()
        c.read(INI, encoding="utf-8")
        self.mode.set(c.get("General", "Mode", fallback="Solo"))
        self.focus.set(c.getint("Fixes", "FocusFix", fallback=1))
        self.norage.set(c.getint("Fixes", "NoRageArts", fallback=1))
        self.borderless.set(c.getint("Launcher", "Borderless", fallback=1))
        self.assault = c.get("Effects", "Assault", fallback=DEFAULT_ASSAULT)
        if "0x828D" not in self.assault or self.assault in OLD_ASSAULTS:   # older defaults: upgrade
            self.assault = DEFAULT_ASSAULT
        self.ret = c.get("Effects", "Return", fallback=DEFAULT_RETURN)
        old = c.get("Tag", "TagKey", fallback="Space")
        self.key.set(KEYS["Space"] if old == "O" else KEYS.get(old, KEYS["Space"]))   # O was the old default
        self.pad.set(PADS.get(c.get("Tag", "PadTag", fallback="LB"), PADS["LB"]))
        self.p2mode.set(P2_MODES.get(c.get("Tag", "P2Tag", fallback="Auto"), P2_MODES["Auto"]))
        self.jump.set(0 if c.get("Tag", "TagIn", fallback="Jump") == "None" else 1)
        self.wall.set(c.getint("Tag", "WallScrew", fallback=0))
        self.model.set(c.getint("Tag", "ModelSwap", fallback=1))

    def refresh_status(self):
        ok = is_our_dll(DLL_DST)
        self.status.config(text="상태: 설치됨" if ok else "상태: 설치 안 됨", fg="green" if ok else "red")

    def install(self):
        if not WIN64:
            return messagebox.showerror("T7TT", "스팀에서 설치한 철권 7을 찾지 못했습니다.")
        if os.path.exists(DLL_DST) and not is_our_dll(DLL_DST):
            if not messagebox.askyesno("T7TT", "게임 폴더에 다른 모드의 dinput8.dll이 있습니다.\n"
                                                 "백업(dinput8.dll.t7tag.bak)하고 교체할까요?"):
                return
            backup(DLL_DST)
        p2mode = next(k for k, v in P2_MODES.items() if v == self.p2mode.get())
        if self.mode.get() == "Tag" and not tag_data_ready(PARTNER_DIR):
            def progress(i, n):
                self.status.config(text=f"태그용 캐릭터 데이터 만드는 중... {i}/{n} (처음 한 번, 약 40초)", fg="blue")
                self.root.update()
            progress(0, 55)
            try:
                build_tag_data(PARTNER_DIR, progress)
            except Exception as e:  # noqa: BLE001
                self.refresh_status()
                return messagebox.showerror("T7TT", f"태그 데이터 생성 실패:\n{e}")
        try:
            shutil.copy2(DLL_SRC, DLL_DST)
        except PermissionError:
            self.refresh_status()
            return messagebox.showerror("T7TT", "복사 실패: 게임이 실행 중이면 끄고 다시 시도하세요.")
        key = next(k for k, v in KEYS.items() if v == self.key.get())
        text = ini_text(self.mode.get(), self.focus.get(), self.norage.get(), p2mode,
                        self.jump.get(), self.borderless.get(), self.assault, key, self.ret, self.wall.get(),
                        self.model.get(), next(k for k, v in PADS.items() if v == self.pad.get()))
        text = merge_ini(text, INI)                      # 11-22: keep what the user set in game (F11 sound, ...)
        with open(INI, "w", encoding="utf-8") as f:
            f.write(text)
        # Engine.ini: UE4 focus-loss settings
        os.makedirs(CFG_DIR, exist_ok=True)
        backup(ENGINE_INI)
        text = open(ENGINE_INI, encoding="utf-8-sig").read() if os.path.exists(ENGINE_INI) else ""
        if "T7Tag" in text:
            text = text[:text.index("; --- T7Tag:")].rstrip("\n") + "\n"
        if self.focus.get():
            text += ENGINE_BLOCK
        open(ENGINE_INI, "w", encoding="utf-8").write(text)
        if self.borderless.get():
            backup(USER_INI)
            set_ini_keys(USER_INI, "/Script/Engine.GameUserSettings",
                         {"FullscreenMode": 1, "LastConfirmedFullscreenMode": 1, "PreferredFullscreenMode": 1})
        self.refresh_status()
        messagebox.showinfo("T7TT", "적용했습니다. '게임 실행'을 누르세요.")

    def uninstall(self):
        if is_our_dll(DLL_DST):
            os.remove(DLL_DST)
        restore(DLL_DST)
        shutil.rmtree(os.path.join(WIN64, "T7Tag_portraits"), ignore_errors=True)
        for p in [INI] + glob.glob(os.path.join(WIN64, "T7Tag*.log")) + glob.glob(os.path.join(WIN64, "T7Tag_mark_*.txt")):
            if os.path.exists(p):
                os.remove(p)
        shutil.rmtree(PARTNER_DIR, ignore_errors=True)
        restore(ENGINE_INI)
        restore(USER_INI)
        self.refresh_status()
        messagebox.showinfo("T7TT", "제거했습니다. 게임 설정 파일도 원래대로 돌렸습니다.")

    def launch(self):
        os.startfile("steam://rungameid/389730")

    def feedback(self):
        """one zip on the desktop: the F8 recordings, the mod's log and settings - nothing else"""
        files = sorted(glob.glob(os.path.join(WIN64, "T7Tag_mark_*.txt"))) if WIN64 else []
        files += [p for p in glob.glob(os.path.join(WIN64, "T7Tag*.log")) + [INI] if os.path.exists(p)] if WIN64 else []
        if not files:
            return messagebox.showinfo("T7TT", "아직 기록이 없습니다. 게임에서 이상한 장면이 나오면 F8을 눌러 주세요.")
        out = make_feedback(files)
        os.startfile(os.path.dirname(out))
        messagebox.showinfo("T7TT", f"바탕화면에 만들었습니다:\n{os.path.basename(out)}\n\n"
                                     "이 파일과 함께 '무엇을 했고 무엇이 나왔는지' 한 줄 적어 주세요.")


def desktop():
    try:
        import winreg
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER,
                            r"Software\Microsoft\Windows\CurrentVersion\Explorer\User Shell Folders") as k:
            return os.path.expandvars(winreg.QueryValueEx(k, "Desktop")[0])
    except OSError:
        return os.path.join(os.path.expanduser("~"), "Desktop")


def make_feedback(files, folder=None):
    out = os.path.join(folder or desktop(), "T7TT_피드백_" + time.strftime("%m%d_%H%M%S") + ".zip")
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        for p in files:
            z.write(p, os.path.basename(p))
    return out


def selftest(out):
    """T7TT.exe --selftest <folder>: builds the data into the folder and writes result.txt (no install, no window)"""
    os.makedirs(out, exist_ok=True)
    lines = [f"game {GAME}", f"dll {os.path.getsize(DLL_SRC)}"]
    try:
        build_tag_data(os.path.join(out, "partners"))
        n = len(glob.glob(os.path.join(out, "partners", "*.t7p")))
        lines.append(f"data ok {n} ready {tag_data_ready(os.path.join(out, 'partners'))}")
    except Exception as e:  # noqa: BLE001
        lines.append(f"data FAILED {type(e).__name__}: {e}")
    open(os.path.join(out, "result.txt"), "w", encoding="utf-8").write("\n".join(lines))


if __name__ == "__main__":
    if len(sys.argv) == 3 and sys.argv[1] == "--selftest":
        selftest(sys.argv[2])
        sys.exit(0)
    root = tk.Tk()
    App(root)
    root.mainloop()
