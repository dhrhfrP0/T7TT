// Model swap (plan B, 11-21): the partner's look on the player's actor.
// TEKKEN 7 characters are modular actors (BP_TekkenPlayer_Modular_C): one SkeletalMeshComponent per item slot
// (ItemSlotPosition: HEAD 0, HAIR 1, UPPER 2, LOWER 3, FULL_BODY 4, GLASSES 5, FACE 6, HAIR_ACC 7, UPPER_ACC 8,
// LOWER_ACC 9, FACE_HAIR 10 ...). All characters share the skeleton (movesets already play across characters).
// At match setup, on the game thread:
//   cm     = the BP_CharacterManager_C actor
//   actor  = cm.GetCharacterLoadHelper().GetCharacterActor(side)
//   comp[p]= actor.GetMeshComponent(p)            the main's meshes are kept (rooted so GC keeps them)
//   set    = cm.LoadItemSetFromXML(0, partnerId, side)   the partner's default costume (rooted)
//   part[p]= set.GetMeshAtPosition(p)
// A tag then only calls comp[p].SetSkeletalMesh(main or partner mesh). Everything is logged ("model:").
#include "common.h"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <cmath>
#include <mmsystem.h>
#include <functional>
#include <vector>
#include <map>
#include <string>

namespace ue {
bool HookReady();
void RunOnGameThread(std::function<void()> f);
uintptr_t Function(const char* cls, const char* fn);
uintptr_t Instance(const char* cls);
uintptr_t Class(uintptr_t obj);
std::string Name(uintptr_t obj);
std::string ClassName(uintptr_t obj);
int PropOffset(uintptr_t st, const char* prop);
int ParmsSize(uintptr_t fn);
bool Call(uintptr_t obj, uintptr_t fn, void* parms);
void SetRoot(uintptr_t obj, bool on);
uintptr_t Find(const char* cls, const char* outer, const char* name);
uintptr_t ObjectAtIndex(int i);
uintptr_t Outer(uintptr_t obj);
int NumObjects();
}

namespace {
constexpr int kSlots = 14;
const char* kSlotName[kSlots] = {"HEAD", "HAIR", "UPPER", "LOWER", "FULL_BODY", "GLASSES", "FACE", "HAIR_ACC",
                                 "UPPER_ACC", "LOWER_ACC", "FACE_HAIR", "MAKE_UP", "FULL_HEAD", "EXTRA"};

struct Fns {
    bool ok = false, tried = false;
    uintptr_t getHelper, getActor, getComp, loadXml, meshAt, setMesh, setMaster, setAnimClass;
    int animClassOff = -1;      // SkeletalMeshComponent.AnimClass
    int overrideOff = -1;       // MeshComponent.OverrideMaterials (TArray<UMaterialInterface*>)
    uintptr_t setMaterial = 0;  // PrimitiveComponent.SetMaterial(int, material)
    int skeletonOff = -1;       // SkeletalMesh.Skeleton
    int meshOff = -1;           // SkinnedMeshComponent.SkeletalMesh
    int masterOff = -1;         // SkinnedMeshComponent.MasterPoseComponent (weak pointer: object index, serial)
    int playerNumOff = -1;      // TekkenPlayer.PlayerNumber
} F;

struct SideModel {
    bool ready = false;
    int ids[2] = {-1, -1};
    uintptr_t actor = 0;
    uintptr_t comp[kSlots] = {};
    uintptr_t mesh[2][kSlots] = {};
    uintptr_t set = 0;
    int shown = 0;
    uintptr_t imc = 0, mainSet = 0;            // item manager + the main's own costume set (equip mode)
    int tries = 0;
    int alias[kSlots] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
    uintptr_t anim[2][kSlots] = {};          // animation blueprint class per part: each skeleton needs its own
    std::vector<uintptr_t> mats[kSlots];       // the main's material overrides (customization) per part
    uintptr_t snd[2] = {}, eff[2] = {};        // character sound manager (voice) / effect manager per member
    std::vector<uintptr_t> extras;             // the main's other mesh components (devil wings...) hidden for the partner
    std::vector<uintptr_t> extraMaster;        // their pose leaders (re-linked when shown)
    std::vector<uintptr_t> partnerExtraMaster;
    std::map<uintptr_t, std::pair<uintptr_t, uintptr_t>> parked;   // hidden part -> (its mesh, its anim class)
    std::vector<bool> extraVis;
    std::vector<std::pair<uintptr_t, bool>> statics;  // 11-23: the main's rigid accessories (glasses, weapons) and whether they were visible
    std::vector<uintptr_t> partnerExtras;      // the partner's extra items (devil wings...) added on its first showing
    bool partnerExtrasTried = false;
    uintptr_t doll = 0, dollImc = 0;            // 11-23: look-only second actor (doll.cpp) and its item manager
    uintptr_t dollComp[kSlots] = {};
    int dollDressed = -1, dollIdx = -1;
    std::vector<uintptr_t> dollExtras[2];      // per member: extra items (devil wings ...) added to the doll
    bool dollExtrasTried[2] = {};
};
SideModel g_m[2];
// the doll actor still is that object (not collected with an unloaded level): same slot in the object table
bool DollAlive(const SideModel& m) { return m.doll && m.dollIdx >= 0 && ue::ObjectAtIndex(m.dollIdx) == m.doll; }                        // 11-23: the partner's custom costume set taken at the select screen

bool Resolve() {
    if (F.tried) return F.ok;
    F.tried = true;
    F.getHelper = ue::Function("CharacterManager", "GetCharacterLoadHelper");
    F.getActor = ue::Function("TekkenCharacterLoadHelper", "GetCharacterActor");
    F.getComp = ue::Function("ModularTekkenPlayer", "GetMeshComponent");
    F.loadXml = ue::Function("CharacterManager", "LoadItemSetFromXML");
    F.meshAt = ue::Function("CharacterItemSet", "GetMeshAtPosition");
    F.setMesh = ue::Function("SkinnedMeshComponent", "SetSkeletalMesh");
    F.setMaster = ue::Function("SkinnedMeshComponent", "SetMasterPoseComponent");
    F.setAnimClass = ue::Function("SkeletalMeshComponent", "SetAnimInstanceClass");
    F.setMaterial = ue::Function("PrimitiveComponent", "SetMaterial");
    uintptr_t mc = ue::Find("Class", "/Script/Engine", "MeshComponent");
    F.overrideOff = mc ? ue::PropOffset(mc, "OverrideMaterials") : -1;
    uintptr_t skc = ue::Find("Class", "/Script/Engine", "SkeletalMeshComponent");
    uintptr_t skm = ue::Find("Class", "/Script/Engine", "SkeletalMesh");
    F.animClassOff = skc ? ue::PropOffset(skc, "AnimClass") : -1;
    F.skeletonOff = skm ? ue::PropOffset(skm, "Skeleton") : -1;
    uintptr_t skinned = ue::Find("Class", "/Script/Engine", "SkinnedMeshComponent");
    uintptr_t tp = ue::Find("Class", "/Script/TekkenGame", "TekkenPlayer");
    F.meshOff = skinned ? ue::PropOffset(skinned, "SkeletalMesh") : -1;
    F.masterOff = skinned ? ue::PropOffset(skinned, "MasterPoseComponent") : -1;
    F.playerNumOff = tp ? ue::PropOffset(tp, "PlayerNumber") : -1;
    F.ok = F.getHelper && F.getActor && F.getComp && F.loadXml && F.meshAt && F.setMesh && F.meshOff > 0;
    Log("model: engine functions %s (helper %d actor %d comp %d xml %d meshAt %d setMesh %d, mesh +0x%X, playerNumber +0x%X)",
        F.ok ? "found" : "MISSING", !!F.getHelper, !!F.getActor, !!F.getComp, !!F.loadXml, !!F.meshAt, !!F.setMesh,
        F.meshOff, F.playerNumOff);
    return F.ok;
}

template <class T> T At(const uint8_t* p, int off) { T v; memcpy(&v, p + off, sizeof(T)); return v; }

// FighterId order (CharacterItemEditor enum) -> folder code of /Game/Character/Item/Sets/<CODE>/
const char* kCode[] = {"PAU", "LAW", "KIN", "YOS", "HWO", "XIA", "JIN", "BRY", "HEI", "KAZ", "STE", "JAC", "ASK", "DVJ",
    "FEN", "LIL", "DRA", "LEO", "LAR", "ASA", "ITA", "LTN", "DNC", "ARB", "MUT", "CRZ", "KZM", "BS7", "NIN", "FRV", "LEE",
    "BOB", "MRX", "KUM", "PAN", "EDD", "ELZ", "MIG", "ZAK", "YKZ", "JA4", "YHE", "DEK", "MRY", "MRZ", "ANN", "LEI", "MAR",
    "AKI", "JUL", "NSA", "ZAF", "GAN", "NSB", "NSC", "KNM", "NSD"};

// FName(const wchar_t*, EFindName) at 0x140867350 (same code T7Hook uses)
struct FNameRaw { int index, number; };
bool MakeName(const wchar_t* s, FNameRaw& out) {
    using Fn = void*(__fastcall*)(FNameRaw*, const wchar_t*, int);
    static Fn ctor = nullptr;
    if (!ctor) {
        auto at = (uint8_t*)(GameBase() + 0x867350);
        static const uint8_t sig[] = {0x40, 0x53, 0x48, 0x83, 0xEC, 0x30, 0x48, 0x8B, 0xD9, 0x48, 0x85, 0xD2};
        if (memcmp(at, sig, sizeof(sig)) != 0) { Log("model: FName constructor not where expected"); return false; }
        ctor = (Fn)at;
    }
    out = FNameRaw{0, 0};
    ctor(&out, s, 1 /*FNAME_Add*/);
    return out.index > 0;
}

// the character's default costume asset, loaded by path (TekkenBlueprintLibrary.LoadObjectFromAssetPath)
int g_oppModel[2] = {-1, -1};          // the other side's character: same character -> the other costume colour

uintptr_t LoadDefaultSet(int side, int id, int forceColor = 0) {
    static uintptr_t fn = 0, cdo = 0, setCls = 0;
    if (!fn) {
        fn = ue::Function("TekkenBlueprintLibrary", "LoadObjectFromAssetPath");
        cdo = ue::Find("TekkenBlueprintLibrary", "/Script/TekkenGame", "Default__TekkenBlueprintLibrary");
        setCls = ue::Find("Class", "/Script/TekkenGame", "CharacterItemSet");
    }
    if (!fn || !cdo || !setCls || id < 0 || id >= (int)(sizeof(kCode) / sizeof(kCode[0]))) {
        Log("model: asset loader missing (fn %d cdo %d class %d) or id %d", !!fn, !!cdo, !!setCls, id);
        return 0;
    }
    for (int k = 0; k < 2; ++k) {
        // 1P costume on the left, 2P on the right; a partner who is also the opponent's character takes the other one
        int pref = side == 0 && g_oppModel[side] == id ? 2 : side + 1;
        if (forceColor) pref = forceColor;
        int color = k == 0 ? pref : 3 - pref;
        wchar_t path[200];
        swprintf_s(path, L"/Game/Character/Item/Sets/%S/CS_%S_DefaultCos%dP.CS_%S_DefaultCos%dP", kCode[id], kCode[id],
                   color, kCode[id], color);
        FNameRaw nm;
        if (!MakeName(path, nm)) continue;
        uint8_t a[24]{};
        memcpy(a, &setCls, 8); memcpy(a + 8, &nm, 8);
        if (!ue::Call(cdo, fn, a)) continue;
        uintptr_t set = At<uintptr_t>(a, 0x10);
        char p8[200]; WideCharToMultiByte(CP_UTF8, 0, path, -1, p8, sizeof(p8), nullptr, nullptr);
        Log("model: P%d load %s -> %s", side + 1, p8, set ? ue::ClassName(set).c_str() : "nothing");
        if (set) return set;
    }
    return 0;
}

// a costume set asset of a character by suffix ("DefaultCos2P_old" ...) - the select list's Default 1-4 (11-23: Default 2 of
// Devil Jin loaded CS_DVJ_DefaultCos1P_old)
uintptr_t LoadSetAsset(int id, const char* suffix) {
    static uintptr_t fn = 0, cdo = 0, setCls = 0;
    if (!fn) {
        fn = ue::Function("TekkenBlueprintLibrary", "LoadObjectFromAssetPath");
        cdo = ue::Find("TekkenBlueprintLibrary", "/Script/TekkenGame", "Default__TekkenBlueprintLibrary");
        setCls = ue::Find("Class", "/Script/TekkenGame", "CharacterItemSet");
    }
    if (!fn || !cdo || !setCls || id < 0 || id >= (int)(sizeof(kCode) / sizeof(kCode[0]))) return 0;
    wchar_t path[200];
    swprintf_s(path, L"/Game/Character/Item/Sets/%S/CS_%S_%S.CS_%S_%S", kCode[id], kCode[id], suffix, kCode[id], suffix);
    FNameRaw nm;
    if (!MakeName(path, nm)) return 0;
    uint8_t a[24]{};
    memcpy(a, &setCls, 8); memcpy(a + 8, &nm, 8);
    if (!ue::Call(cdo, fn, a)) return 0;
    return At<uintptr_t>(a, 0x10);
}

}  // namespace
// 11-26 boss versions: the story's own costume set (CS_HEI_final ...), loaded by path; 0 = none / not loadable
uintptr_t BossSet(int id) {
    static uintptr_t fn = 0, cdo = 0, setCls = 0;
    if (!fn) {
        fn = ue::Function("TekkenBlueprintLibrary", "LoadObjectFromAssetPath");
        cdo = ue::Find("TekkenBlueprintLibrary", "/Script/TekkenGame", "Default__TekkenBlueprintLibrary");
        setCls = ue::Find("Class", "/Script/TekkenGame", "CharacterItemSet");
    }
    const wchar_t* name = id == 8 ? L"CS_HEI_final" : id == 9 ? L"CS_KAZ_final" : id == 32 ? L"CS_MRX_final" : nullptr;
    if (!fn || !cdo || !setCls || !name) return 0;
    wchar_t path[200];
    swprintf_s(path, L"/Game/Demo/StoryMode/Character/Sets/%s.%s", name, name);
    FNameRaw nm;
    if (!MakeName(path, nm)) return 0;
    uint8_t a[24]{};
    memcpy(a, &setCls, 8); memcpy(a + 8, &nm, 8);
    uintptr_t set = ue::Call(cdo, fn, a) ? At<uintptr_t>(a, 0x10) : 0;
    Log("model: boss costume %S -> %s", name, set ? ue::Name(set).c_str() : "not loaded");
    return set;
}
namespace {

// the animation blueprint for a mesh: SKT_CH_<x> skeleton -> /Game/Character/Common/Skeleton/AB_CH_<x>.AB_CH_<x>_C
uintptr_t AnimClassFor(uintptr_t mesh) {
    static uintptr_t fn = 0, cdo = 0, abgc = 0;
    if (!mesh || F.skeletonOff <= 0) return 0;
    uintptr_t skel = 0; Rd(mesh + F.skeletonOff, skel);
    std::string sk = ue::Name(skel);
    if (sk.rfind("SKT_CH_", 0) != 0) { Log("model: mesh %s has skeleton '%s'", ue::Name(mesh).c_str(), sk.c_str()); return 0; }
    std::string ab = "AB_CH_" + sk.substr(7);
    uintptr_t have = ue::Find("AnimBlueprintGeneratedClass", ("/Game/Character/Common/Skeleton/" + ab).c_str(), (ab + "_C").c_str());
    if (have) return have;
    if (!fn) {
        fn = ue::Function("TekkenBlueprintLibrary", "LoadObjectFromAssetPath");
        cdo = ue::Find("TekkenBlueprintLibrary", "/Script/TekkenGame", "Default__TekkenBlueprintLibrary");
        abgc = ue::Find("Class", "/Script/Engine", "AnimBlueprintGeneratedClass");
    }
    std::string path = "/Game/Character/Common/Skeleton/" + ab + "." + ab + "_C";
    wchar_t w[200]; MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, w, 200);
    FNameRaw nm;
    if (!fn || !cdo || !abgc || !MakeName(w, nm)) return 0;
    uint8_t a[24]{};
    memcpy(a, &abgc, 8); memcpy(a + 8, &nm, 8);
    if (!ue::Call(cdo, fn, a)) return 0;
    uintptr_t cls = At<uintptr_t>(a, 0x10);
    Log("model: anim blueprint %s -> %s", path.c_str(), cls ? "loaded" : "NOT found");
    if (cls) ue::SetRoot(cls, true);
    return cls;
}

// 11-22 (Jack-7 drawn broken as a partner): some characters run their own anim blueprint kept with their meshes,
// /Game/Character/Item/Meshes/<code>/AB_CH_<code> (Jack: AB_CH_jac), not the skeleton's. Loaded by path; 0 if none.
uintptr_t CharacterAnimClass(int id) {
    if (id < 0 || id >= (int)(sizeof(kCode) / sizeof(kCode[0]))) return 0;
    std::string c = kCode[id];
    for (char& ch : c) ch = (char)tolower((unsigned char)ch);
    std::string ab = "AB_CH_" + c, path = "/Game/Character/Item/Meshes/" + c + "/" + ab + "." + ab + "_C";
    uintptr_t have = ue::Find("AnimBlueprintGeneratedClass", ("/Game/Character/Item/Meshes/" + c + "/" + ab).c_str(), (ab + "_C").c_str());
    if (have) return have;
    static uintptr_t fn = ue::Function("TekkenBlueprintLibrary", "LoadObjectFromAssetPath");
    static uintptr_t cdo = ue::Find("TekkenBlueprintLibrary", "/Script/TekkenGame", "Default__TekkenBlueprintLibrary");
    static uintptr_t abgc = ue::Find("Class", "/Script/Engine", "AnimBlueprintGeneratedClass");
    wchar_t w[200]; MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, w, 200);
    FNameRaw nm;
    if (!fn || !cdo || !abgc || !MakeName(w, nm)) return 0;
    uint8_t a[24]{};
    memcpy(a, &abgc, 8); memcpy(a + 8, &nm, 8);
    if (!ue::Call(cdo, fn, a)) return 0;
    uintptr_t cls = At<uintptr_t>(a, 0x10);
    if (cls) { ue::SetRoot(cls, true); Log("model: character anim blueprint %s loaded", path.c_str()); }
    return cls;
}

// per character: voice (Wwise sound manager) and effect manager blueprints (from the pak index, 11-21)
struct MgrPaths { const char* code; const char* sound; const char* effect; };
const MgrPaths kMgr[] = {
    {"PAU", "/Game/Sound/Character/pau/BP_pauCharacterSoundManager.BP_pauCharacterSoundManager_C", "/Game/Effect/TK75/Character/pau/BP_PauEffectManager.BP_PauEffectManager_C"},
    {"LAW", "/Game/Sound/Character/law/BP_lawCharacterSoundManager.BP_lawCharacterSoundManager_C", "/Game/Effect/TK75/Character/law/BP_LawEffectManager.BP_LawEffectManager_C"},
    {"KIN", "/Game/Sound/Character/kin/BP_kinCharacterSoundManager.BP_kinCharacterSoundManager_C", "/Game/Effect/TK75/Character/kin/BP_kinEffectManager.BP_kinEffectManager_C"},
    {"YOS", "/Game/Sound/Character/yos/BP_yosCharacterSoundManager.BP_yosCharacterSoundManager_C", "/Game/Effect/TK75/Character/yos/BP_yosEffectManager.BP_yosEffectManager_C"},
    {"HWO", "/Game/Sound/Character/hwo/BP_hwoCharacterSoundManager.BP_hwoCharacterSoundManager_C", "/Game/Effect/TK75/Character/hwo/BP_HwoEffectManager.BP_HwoEffectManager_C"},
    {"XIA", "/Game/Sound/Character/xia/BP_xiaCharacterSoundManager.BP_xiaCharacterSoundManager_C", "/Game/Effect/TK75/Character/xia/BP_XiaEffectManager.BP_XiaEffectManager_C"},
    {"JIN", "/Game/Sound/Character/jin/BP_jinCharacterSoundManager.BP_jinCharacterSoundManager_C", "/Game/Effect/TK75/Character/jin/BP_jinEffectManager.BP_jinEffectManager_C"},
    {"BRY", "/Game/Sound/Character/bry/BP_bryCharacterSoundManager.BP_bryCharacterSoundManager_C", "/Game/Effect/TK75/Character/bry/BP_bryEffectManager.BP_bryEffectManager_C"},
    {"HEI", "/Game/Sound/Character/hei/BP_heiCharacterSoundManager.BP_heiCharacterSoundManager_C", "/Game/Effect/TK75/Character/hei/BP_HeiEffectManager.BP_HeiEffectManager_C"},
    {"KAZ", "/Game/Sound/Character/kaz/BP_kazCharacterSoundManager.BP_kazCharacterSoundManager_C", "/Game/Effect/TK75/Character/kaz/BP_KazEffectManager.BP_KazEffectManager_C"},
    {"STE", "/Game/Sound/Character/ste/BP_steCharacterSoundManager.BP_steCharacterSoundManager_C", "/Game/Effect/TK75/Character/ste/BP_SteEffectManager.BP_SteEffectManager_C"},
    {"JAC", "/Game/Sound/Character/jac/BP_jacCharacterSoundManager.BP_jacCharacterSoundManager_C", "/Game/Effect/TK75/Character/jac/BP_jacEffectManager.BP_jacEffectManager_C"},
    {"ASK", "/Game/Sound/Character/ask/BP_askCharacterSoundManager.BP_askCharacterSoundManager_C", "/Game/Effect/TK75/Character/ask/BP_AskEffectManager.BP_AskEffectManager_C"},
    {"DVJ", "/Game/Sound/Character/dvj/BP_dvjCharacterSoundManager.BP_dvjCharacterSoundManager_C", "/Game/Effect/TK75/Character/dvj/BP_dvjEffectManager.BP_dvjEffectManager_C"},
    {"FEN", "/Game/Sound/Character/fen/BP_fenCharacterSoundManager.BP_fenCharacterSoundManager_C", "/Game/Effect/TK75/Character/fen/BP_FenEffectManager.BP_FenEffectManager_C"},
    {"LIL", "/Game/Sound/Character/lil/BP_lilCharacterSoundManager.BP_lilCharacterSoundManager_C", "/Game/Effect/TK75/Character/lil/BP_LilEffectManager.BP_LilEffectManager_C"},
    {"DRA", "/Game/Sound/Character/dra/BP_draCharacterSoundManager.BP_draCharacterSoundManager_C", "/Game/Effect/TK75/Character/dra/BP_DraEffectManager.BP_DraEffectManager_C"},
    {"LEO", "/Game/Sound/Character/leo/BP_leoCharacterSoundManager.BP_leoCharacterSoundManager_C", "/Game/Effect/TK75/Character/leo/BP_LeoEffectManager.BP_LeoEffectManager_C"},
    {"LAR", "/Game/Sound/Character/lar/BP_larCharacterSoundManager.BP_larCharacterSoundManager_C", "/Game/Effect/TK75/Character/lar/BP_LarEffectManager.BP_LarEffectManager_C"},
    {"ASA", "/Game/Sound/Character/asa/BP_asaCharacterSoundManager.BP_asaCharacterSoundManager_C", "/Game/Effect/TK75/Character/asa/BP_AsaEffectManager.BP_AsaEffectManager_C"},
    {"ITA", "/Game/Sound/Character/ita/BP_itaCharacterSoundManager.BP_itaCharacterSoundManager_C", "/Game/Effect/TK75/Character/ita/BP_itaEffectManager.BP_itaEffectManager_C"},
    {"LTN", "/Game/Sound/Character/ltn/BP_ltnCharacterSoundManager.BP_ltnCharacterSoundManager_C", "/Game/Effect/TK75/Character/ltn/BP_LtnEffectManager.BP_LtnEffectManager_C"},
    {"DNC", "/Game/Sound/Character/dnc/BP_dncCharacterSoundManager.BP_dncCharacterSoundManager_C", "/Game/Effect/TK75/Character/dnc/BP_dncEffectManager.BP_dncEffectManager_C"},
    {"ARB", "/Game/Sound/Character/arb/BP_arbCharacterSoundManager.BP_arbCharacterSoundManager_C", "/Game/Effect/TK75/Character/arb/BP_arbEffectManager.BP_arbEffectManager_C"},
    {"MUT", "/Game/Sound/Character/mut/BP_mutCharacterSoundManager.BP_mutCharacterSoundManager_C", "/Game/Effect/TK75/Character/mut/BP_mutEffectManager.BP_mutEffectManager_C"},
    {"CRZ", "/Game/Sound/Character/crz/BP_crzCharacterSoundManager.BP_crzCharacterSoundManager_C", "/Game/Effect/TK75/Character/crz/BP_crzEffectManager.BP_crzEffectManager_C"},
    {"KZM", "/Game/Sound/Character/kzm/BP_kzmCharacterSoundManager.BP_kzmCharacterSoundManager_C", "/Game/Effect/TK75/Character/kzm/BP_kzmEffectManager.BP_kzmEffectManager_C"},
    {"BS7", "/Game/Sound/Character/bs7/BP_bs7CharacterSoundManager.BP_bs7CharacterSoundManager_C", "/Game/Effect/TK75/Character/bs7/BP_bs7EffectManager.BP_bs7EffectManager_C"},
    {"NIN", "/Game/Sound/Character/nin/BP_ninCharacterSoundManager.BP_ninCharacterSoundManager_C", "/Game/Effect/TK75/Character/nin/BP_ninEffectManager.BP_ninEffectManager_C"},
    {"FRV", "/Game/Sound/Character/frv/BP_frvCharacterSoundManager.BP_frvCharacterSoundManager_C", "/Game/Effect/TK75/Character/frv/BP_frvEffectManager.BP_frvEffectManager_C"},
    {"LEE", "/Game/Sound/Character/lee/BP_leeCharacterSoundManager.BP_leeCharacterSoundManager_C", "/Game/Effect/TK75/Character/lee/BP_leeEffectManager.BP_leeEffectManager_C"},
    {"BOB", "/Game/Sound/Character/bob/BP_bobCharacterSoundManager.BP_bobCharacterSoundManager_C", "/Game/Effect/TK75/Character/bob/BP_bobEffectManager.BP_bobEffectManager_C"},
    {"MRX", "/Game/Sound/Character/mrx/BP_mrxCharacterSoundManager.BP_mrxCharacterSoundManager_C", "/Game/Effect/TK75/Character/mrx/BP_mrxEffectManager.BP_mrxEffectManager_C"},
    {"KUM", "/Game/Sound/Character/kum/BP_kumCharacterSoundManager.BP_kumCharacterSoundManager_C", "/Game/Effect/TK75/Character/kum/BP_kumEffectManager.BP_kumEffectManager_C"},
    {"PAN", "/Game/Sound/Character/pan/BP_panCharacterSoundManager.BP_panCharacterSoundManager_C", "/Game/Effect/TK75/Character/pan/BP_panEffectManager.BP_panEffectManager_C"},
    {"EDD", "/Game/Sound/Character/edd/BP_eddCharacterSoundManager.BP_eddCharacterSoundManager_C", "/Game/Effect/TK75/Character/edd/BP_eddEffectManager.BP_eddEffectManager_C"},
    {"ELZ", "/Game/Sound/Character/elz/BP_elzCharacterSoundManager.BP_elzCharacterSoundManager_C", "/Game/Effect/TK75/Character/elz/BP_elzEffectManager.BP_elzEffectManager_C"},
    {"MIG", "/Game/Sound/Character/mig/BP_migCharacterSoundManager.BP_migCharacterSoundManager_C", "/Game/Effect/TK75/Character/mig/BP_migEffectManager.BP_migEffectManager_C"},
    {"ZAK", "/Game/Sound/Character/zak/BP_zakCharacterSoundManager.BP_zakCharacterSoundManager_C", "/Game/Effect/TK75/Character/zak/BP_zakEffectManager.BP_zakEffectManager_C"},
    {"YKZ", "/Game/Sound/Character/ykz/BP_ykzCharacterSoundManager.BP_ykzCharacterSoundManager_C", "/Game/Effect/TK75/Character/ykz/BP_ykzEffectManager.BP_ykzEffectManager_C"},
    {"JA4", "/Game/Sound/Character/ja4/BP_ja4CharacterSoundManager.BP_ja4CharacterSoundManager_C", "/Game/Effect/TK75/Character/ja4/BP_ja4EffectManager.BP_ja4EffectManager_C"},
    {"YHE", "/Game/Sound/Character/yhe/BP_yheCharacterSoundManager.BP_yheCharacterSoundManager_C", "/Game/Effect/TK75/Character/yhe/BP_yheEffectManager.BP_yheEffectManager_C"},
    {"DEK", "/Game/Sound/Character/dek/BP_dekCharacterSoundManager.BP_dekCharacterSoundManager_C", "/Game/Effect/TK75/Character/dek/BP_dekEffectManager.BP_dekEffectManager_C"},
    {"MRY", "/Game/Sound/Character/mry/BP_mryCharacterSoundManager.BP_mryCharacterSoundManager_C", "/Game/Effect/TK75/Character/mry/BP_mryEffectManager.BP_mryEffectManager_C"},
    {"MRZ", "/Game/Sound/Character/mrz/BP_mrzCharacterSoundManager.BP_mrzCharacterSoundManager_C", "/Game/Effect/TK75/Character/mrz/BP_mrzEffectManager.BP_mrzEffectManager_C"},
    {"ANN", "/Game/Sound/Character/ann/BP_annCharacterSoundManager.BP_annCharacterSoundManager_C", "/Game/Effect/TK75/Character/ann/BP_annEffectManager.BP_annEffectManager_C"},
    {"LEI", "/Game/Sound/Character/lei/BP_leiCharacterSoundManager.BP_leiCharacterSoundManager_C", "/Game/Effect/TK75/Character/lei/BP_leiEffectManager.BP_leiEffectManager_C"},
    {"MAR", "/Game/Sound/Character/mar/BP_marCharacterSoundManager.BP_marCharacterSoundManager_C", "/Game/Effect/TK75/Character/mar/BP_marEffectManager.BP_marEffectManager_C"},
    {"AKI", "/Game/Sound/Character/aki/BP_akiCharacterSoundManager.BP_akiCharacterSoundManager_C", "/Game/Effect/TK75/Character/aki/BP_akiEffectManager.BP_akiEffectManager_C"},
    {"JUL", "/Game/Sound/Character/jul/BP_julCharacterSoundManager.BP_julCharacterSoundManager_C", "/Game/Effect/TK75/Character/jul/BP_julEffectManager.BP_julEffectManager_C"},
    {"NSA", "/Game/Sound/Character/nsa/BP_nsaCharacterSoundManager.BP_nsaCharacterSoundManager_C", "/Game/Effect/TK75/Character/nsa/BP_nsaEffectManager.BP_nsaEffectManager_C"},
    {"ZAF", "/Game/Sound/Character/zaf/BP_zafCharacterSoundManager.BP_zafCharacterSoundManager_C", "/Game/Effect/TK75/Character/zaf/BP_zafEffectManager.BP_zafEffectManager_C"},
    {"GAN", "/Game/Sound/Character/gan/BP_ganCharacterSoundManager.BP_ganCharacterSoundManager_C", "/Game/Effect/TK75/Character/gan/BP_ganEffectManager.BP_ganEffectManager_C"},
    {"NSB", "/Game/Sound/Character/nsb/BP_nsbCharacterSoundManager.BP_nsbCharacterSoundManager_C", "/Game/Effect/TK75/Character/nsb/BP_nsbEffectManager.BP_nsbEffectManager_C"},
    {"NSC", "/Game/Sound/Character/nsc/BP_nscCharacterSoundManager.BP_nscCharacterSoundManager_C", "/Game/Effect/TK75/Character/nsc/BP_nscEffectManager.BP_nscEffectManager_C"},
    {"KNM", "/Game/Sound/Character/knm/BP_knmCharacterSoundManager.BP_knmCharacterSoundManager_C", "/Game/Effect/TK75/Character/knm/BP_knmEffectManager.BP_knmEffectManager_C"},
    {"NSD", "/Game/Sound/Character/nsd/BP_nsdCharacterSoundManager.BP_nsdCharacterSoundManager_C", "/Game/Effect/TK75/Character/nsd/BP_nsdEffectManager.BP_nsdEffectManager_C"},
};

uintptr_t LoadAsset(uintptr_t cls, const std::string& path) {
    static uintptr_t fn = 0, cdo = 0;
    if (!fn) {
        fn = ue::Function("TekkenBlueprintLibrary", "LoadObjectFromAssetPath");
        cdo = ue::Find("TekkenBlueprintLibrary", "/Script/TekkenGame", "Default__TekkenBlueprintLibrary");
    }
    wchar_t w[260]; MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, w, 260);
    FNameRaw nm;
    if (!fn || !cdo || !cls || !MakeName(w, nm)) return 0;
    uint8_t a[24]{};
    memcpy(a, &cls, 8); memcpy(a + 8, &nm, 8);
    if (!ue::Call(cdo, fn, a)) return 0;
    return At<uintptr_t>(a, 0x10);
}

// spawn an actor of a blueprint class (GameplayStatics.BeginDeferredActorSpawnFromClass + FinishSpawningActor)
uintptr_t SpawnActor(uintptr_t cls, uintptr_t owner) {
    static uintptr_t begin = 0, finish = 0, gs = 0;
    if (!begin) {
        begin = ue::Function("GameplayStatics", "BeginDeferredActorSpawnFromClass");
        finish = ue::Find("Function", "GameplayStatics", "FinishSpawningActor");
        gs = ue::Find("GameplayStatics", "/Script/Engine", "Default__GameplayStatics");
    }
    if (!begin || !finish || !gs || !cls) return 0;
    float xf[12] = {0, 0, 0, 1,  0, 0, 0, 0,  1, 1, 1, 0};   // FTransform: rotation quat, translation, scale
    uint8_t a[0x60]{};
    memcpy(a + 0x00, &owner, 8); memcpy(a + 0x08, &cls, 8); memcpy(a + 0x10, xf, 48);
    a[0x40] = 1;                                              // AlwaysSpawn
    memcpy(a + 0x48, &owner, 8);
    if (!ue::Call(gs, begin, a)) return 0;
    uintptr_t actor = At<uintptr_t>(a, 0x50);
    if (!actor) return 0;
    uint8_t b[0x50]{};
    memcpy(b, &actor, 8); memcpy(b + 0x10, xf, 48);
    ue::Call(gs, finish, b);
    return actor;
}

uintptr_t CallGetter(uintptr_t obj, const char* cls, const char* fn) {
    uintptr_t f = ue::Function(cls, fn);
    uint8_t a[16]{};
    if (!f || !ue::Call(obj, f, a)) return 0;
    return At<uintptr_t>(a, 0);
}
void CallSetter(uintptr_t obj, const char* cls, const char* fn, uintptr_t v) {
    static std::map<std::string, uintptr_t> cache;
    std::string key = std::string(cls) + "." + fn;
    uintptr_t& f = cache[key];
    if (!f) f = ue::Function(cls, fn);
    uint8_t a[16]{};
    memcpy(a, &v, 8);
    if (f) ue::Call(obj, f, a);
}

// the partner's voice and effects: its own manager actors, spawned once per match
void PrepareManagers(SideModel& m, int side, uintptr_t actor, int partnerId) {
    if (!m.snd[0]) m.snd[0] = CallGetter(actor, "TekkenPlayer", "GetSoundManager");
    if (!m.eff[0]) m.eff[0] = CallGetter(actor, "TekkenPlayer", "GetEffectManager");
    if (partnerId < 0 || partnerId >= (int)(sizeof(kMgr) / sizeof(kMgr[0]))) return;
    uintptr_t bgc = ue::Find("Class", "/Script/Engine", "BlueprintGeneratedClass");
    const MgrPaths& mp = kMgr[partnerId];
    for (int k = 0; k < 2; ++k) {
        const char* path = k == 0 ? mp.sound : mp.effect;
        if (!path || (k == 0 ? m.snd[1] : m.eff[1])) continue;      // already there (retry only what failed)
        uintptr_t cls = LoadAsset(bgc, path);
        uintptr_t a = cls ? SpawnActor(cls, actor) : 0;
        (k == 0 ? m.snd[1] : m.eff[1]) = a;
        Log("model: P%d partner %s manager %s -> %s", side + 1, k == 0 ? "sound" : "effect", path,
            a ? ue::Name(a).c_str() : (cls ? "spawn failed" : "class not loaded"));
    }
    Log("model: P%d main managers: sound %s, effect %s", side + 1, ue::Name(m.snd[0]).c_str(), ue::Name(m.eff[0]).c_str());
}

uintptr_t ActorFor(uintptr_t cm, int side) {
    uint8_t p[32]{};
    if (!ue::Call(cm, F.getHelper, p)) return 0;
    uintptr_t helper = At<uintptr_t>(p, 0);
    if (!helper) return 0;
    for (int pn : {side, 1 - side}) {
        uint8_t a[32]{};
        memcpy(a, &pn, 4);
        if (!ue::Call(helper, F.getActor, a)) continue;
        uintptr_t actor = At<uintptr_t>(a, 8);
        if (!actor) continue;
        int num = -1;
        if (F.playerNumOff > 0) Rd(actor + F.playerNumOff, num);
        if (F.playerNumOff <= 0 || num == side) {
            if (pn != side) Log("model: P%d actor found under player number %d", side + 1, pn);
            return actor;
        }
    }
    return 0;
}

uintptr_t MeshOf(uintptr_t comp) { uintptr_t m = 0; if (comp) Rd(comp + F.meshOff, m); return m; }

// the component this part copies its pose from (weak pointer -> object array)
uintptr_t MasterOf(uintptr_t comp) {
    int idx = -1;
    if (!comp || F.masterOff <= 0 || !Rd(comp + F.masterOff, idx) || idx < 0) return 0;
    return ue::ObjectAtIndex(idx);
}

void Prepare(int side, int mainId, int partnerId);
}  // namespace
void AddBossExtras(int side, int mainId, uintptr_t actor);
namespace {

void Show(int side, int member) {
    SideModel& m = g_m[side];
    if (!m.ready && member == 1 && m.ids[1] >= 0 && m.tries < 3) Prepare(side, m.ids[0], m.ids[1]);   // actor was not ready yet
    if (!m.ready) return;
    int changed = 0;
    // 11-22 crash fix (dumps: skeletal bounds / anim workers out of range on an assault DVJ -> Heihachi): a hidden part
    // stayed a pose slave of the body; once the body's mesh changed its bone map was stale. Parts that are about to be
    // hidden leave their leader BEFORE the body meshes change; shown parts are re-linked after.
    auto detach = [&](const std::vector<uintptr_t>& parts) {
        for (uintptr_t o : parts) {
            if (!o) continue;
            if (F.setMaster) { uint8_t b[16]{}; ue::Call(o, F.setMaster, b); }
            // no mesh while hidden: nothing of it is evaluated against the other character's body
            uintptr_t mesh = MeshOf(o), cls = 0;
            if (F.animClassOff > 0) Rd(o + F.animClassOff, cls);
            if (mesh) { m.parked[o] = {mesh, cls}; uint8_t a[16]{}; a[8] = 1; ue::Call(o, F.setMesh, a); }
        }
    };
    if (member == 1) detach(m.extras); else detach(m.partnerExtras);
    for (int p = 0; p < kSlots; ++p) {
        if (!m.comp[p]) continue;
        uintptr_t want = m.mesh[member][p];
        if (MeshOf(m.comp[p]) == want) continue;
        uintptr_t master = MasterOf(m.comp[p]);
        uint8_t a[16]{};
        memcpy(a, &want, 8);
        a[8] = 1;                                       // bReinitPose: the new mesh's bones start from its own pose
        if (ue::Call(m.comp[p], F.setMesh, a)) ++changed;
        uintptr_t cls = m.anim[member][p];
        if (cls && F.setAnimClass) {                    // Kazuya etc. have their own skeleton + anim BP
            uint8_t c[8]; memcpy(c, &cls, 8);
            ue::Call(m.comp[p], F.setAnimClass, c);
        }
        if (F.setMaterial) {                            // partner: the mesh's own materials; main: its overrides back
            int n = (int)m.mats[p].size();
            if (member == 1 && F.overrideOff > 0) { int cur = 0; Rd(m.comp[p] + F.overrideOff + 8, cur); n = (std::max)(n, cur); }
            for (int i = 0; i < n && i < 32; ++i) {
                uintptr_t mt = member == 0 && i < (int)m.mats[p].size() ? m.mats[p][i] : 0;
                uint8_t d[16]{};
                memcpy(d, &i, 4); memcpy(d + 8, &mt, 8);
                ue::Call(m.comp[p], F.setMaterial, d);
            }
        }
        if (master && F.setMaster) {                    // re-link to the leader so the bone map is rebuilt
            uint8_t b[16]{};
            memcpy(b, &master, 8);
            ue::Call(m.comp[p], F.setMaster, b);
        }
    }
    // the main's extra parts: off for the partner, back as they were for the main
    static uintptr_t isVis = ue::Function("SceneComponent", "IsVisible"), setVis = ue::Function("SceneComponent", "SetVisibility");
    if (isVis && setVis) {
        // 11-22 (wings lost): record only when the main is really leaving (a second partner Show used to record the
        // already hidden state, and the main came back without wings)
        bool record = member == 1 && m.shown != 1;
        if (record) m.extraVis.assign(m.extras.size(), false);
        for (size_t i = 0; i < m.extras.size(); ++i) {
            uint8_t v[8]{};
            if (record) { ue::Call(m.extras[i], isVis, v); m.extraVis[i] = v[0] != 0; }
            if (member == 1 && !record) continue;
            uint8_t a[8]{};
            a[0] = member == 1 ? 0 : (i < m.extraVis.size() && m.extraVis[i]); a[1] = 1;
            ue::Call(m.extras[i], setVis, a);
        }
    }
    // 11-23: the main's rigid accessories (static mesh components: glasses, guns of a custom outfit) go with the main
    if (isVis && setVis) {
        if (member == 1 && m.shown != 1) {
            m.statics.clear();
            int n = ue::NumObjects();
            for (int i = 0; i < n; ++i) {
                uintptr_t o = ue::ObjectAtIndex(i);
                if (!o || ue::Outer(o) != m.actor || ue::ClassName(o) != "StaticMeshComponent") continue;
                uint8_t v[8]{}; ue::Call(o, isVis, v);
                m.statics.push_back({o, v[0] != 0});
            }
            if (!m.statics.empty()) Log("model: P%d %d rigid accessories of the main hidden", side + 1, (int)m.statics.size());
        }
        for (auto& st : m.statics) { uint8_t a[8]{}; a[0] = member == 0 && st.second; a[1] = 1; if (member == 1 || st.second) ue::Call(st.first, setVis, a); }
    }
    // 11-22 colours: mask materials ("M_CH_Base_mask") get their colours from the item set's customize data; without it
    // the partner was drawn white. The game's ItemManagerComponent_C.SetAllCustomizeInfo(ItemSet) paints the meshes now
    // on the actor from that set (no mesh changes, no aura). The main gets its own saved materials back above.
    if (member == 1 && m.imc && m.set && g_cfg.modelEquip) {
        static uintptr_t paint = ue::Function("ItemManagerComponent_C", "SetAllCustomizeInfo");
        if (paint) {
            std::vector<uint8_t> f(0x80, 0);
            memcpy(f.data(), &m.set, 8);
            bool ok = ue::Call(m.imc, paint, f.data());
            Log("model: P%d partner colours from %s: %s", side + 1, ue::Name(m.set).c_str(), ok ? "painted" : "FAILED");
        }
    }
    // the partner's extra items (devil wings ...): the game's "Add Extra Item" on the actor, once; then shown / hidden
    if (isVis && setVis) {
        if (member == 1 && !m.partnerExtrasTried && m.set) {
            m.partnerExtrasTried = true;
            static uintptr_t addExtra = ue::Function("BP_TekkenPlayer_Modular_C", "Add Extra Item");
            uintptr_t extraSet = 0; Rd(m.set + 0x40, extraSet);              // CharacterItemSet.ExtraItemSet
            uintptr_t arr = 0; int num = 0;
            if (extraSet) { Rd(extraSet + 0x30, arr); Rd(extraSet + 0x38, num); }   // CharacterExtraItems.ExtraItems
            std::vector<uintptr_t> before;
            int n = ue::NumObjects();
            for (int i = 0; i < n; ++i) { uintptr_t o = ue::ObjectAtIndex(i); if (o && ue::Outer(o) == m.actor) before.push_back(o); }
            for (int k = 0; addExtra && arr && k < num && k < 8; ++k) {
                uintptr_t cci = 0; Rd(arr + 8 * k, cci);
                if (!cci) continue;
                uintptr_t ci = 0, ab = 0, cls = 0; Rd(cci + 0x30, ci); if (ci) Rd(ci + 0x168, ab);   // CharacterItem.ItemAB
                static int genOff = -1;
                if (genOff < 0) { uintptr_t bpc = ue::Find("Class", "/Script/Engine", "Blueprint"); genOff = bpc ? ue::PropOffset(bpc, "GeneratedClass") : -2; }
                if (ab && genOff > 0) Rd(ab + genOff, cls);
                std::vector<uint8_t> f(0x200, 0);
                memcpy(f.data(), &cls, 8); memcpy(f.data() + 8, &cci, 8);
                bool ok = ue::Call(m.actor, addExtra, f.data());
                uintptr_t added = 0; memcpy(&added, f.data() + 0x118, 8);
                Log("model: P%d partner extra item %s (anim %s): %s, component %s", side + 1, ue::Name(cci).c_str(),
                    ue::Name(cls).c_str(), ok ? "added" : "FAILED", ue::Name(added).c_str());
            }
            n = ue::NumObjects();
            for (int i = 0; i < n; ++i) {
                uintptr_t o = ue::ObjectAtIndex(i);
                if (!o || ue::Outer(o) != m.actor || ue::ClassName(o) != "SkeletalMeshComponent") continue;
                if (std::find(before.begin(), before.end(), o) != before.end()) continue;
                m.partnerExtras.push_back(o); ue::SetRoot(o, true);
                m.partnerExtraMaster.push_back(MasterOf(o) ? MasterOf(o) : (m.comp[2] ? m.comp[2] : m.comp[3]));
                // the real character's wing component runs the body's anim blueprint (AB_CH_base_Skeleton_C); the
                // added one had none and stayed in its folded reference pose
                uintptr_t body = m.comp[2] ? m.comp[2] : m.comp[3];   // UPPER (else LOWER)
                uintptr_t abc = 0; if (body && F.animClassOff > 0) Rd(body + F.animClassOff, abc);
                if (abc && F.setAnimClass) { uint8_t c[8]; memcpy(c, &abc, 8); ue::Call(o, F.setAnimClass, c); }
                Log("model: P%d partner extra part %s mesh %s anim %s", side + 1, ue::Name(o).c_str(), ue::Name(MeshOf(o)).c_str(),
                    ue::Name(abc).c_str());
            }
        }
        for (uintptr_t o : m.partnerExtras) { uint8_t a[8]{}; a[0] = member == 1; a[1] = 1; ue::Call(o, setVis, a); }
        // shown parts back onto their leaders (the body now carries the matching mesh)
        const std::vector<uintptr_t>& shownParts = member == 1 ? m.partnerExtras : m.extras;
        const std::vector<uintptr_t>& leaders = member == 1 ? m.partnerExtraMaster : m.extraMaster;
        for (uintptr_t o : shownParts) {                // parked meshes back first
            auto it = m.parked.find(o);
            if (it == m.parked.end()) continue;
            uint8_t a[16]{}; memcpy(a, &it->second.first, 8); a[8] = 1; ue::Call(o, F.setMesh, a);
            if (it->second.second && F.setAnimClass) { uint8_t c[8]; memcpy(c, &it->second.second, 8); ue::Call(o, F.setAnimClass, c); }
            m.parked.erase(it);
        }
        for (size_t i = 0; i < shownParts.size() && F.setMaster; ++i)
            if (i < leaders.size() && leaders[i]) { uint8_t b[16]{}; memcpy(b, &leaders[i], 8); ue::Call(shownParts[i], F.setMaster, b); }
    }
    // the game's battle materials (hit flash, rim light...) for the meshes now on: BP_BattleEvent builds them
    if (changed) {                                      // both ways: the effect managers work on the new instances
        static uintptr_t initMat = 0;
        if (!initMat) initMat = ue::Find("Function", "BP_BattleEvent_C", "InitCharacterMaterial");
        uintptr_t be = ue::Instance("BP_BattleEvent_C");
        if (initMat && be) {
            std::vector<uint8_t> frame(0x200, 0);
            bool ok = ue::Call(be, initMat, frame.data());
            int n = 0; uintptr_t data = 0;
            if (F.overrideOff > 0 && m.comp[2]) { Rd(m.comp[2] + F.overrideOff, data); Rd(m.comp[2] + F.overrideOff + 8, n); }
            uintptr_t first = 0; if (data && n) Rd(data, first);
            Log("model: P%d battle materials rebuilt %s (upper body: %d overrides, first %s)", side + 1, ok ? "ok" : "FAILED", n,
                ue::Name(first).c_str());
        } else Log("model: BP_BattleEvent not found (function %d, instance %d)", !!initMat, !!be);
    }
    // 11-22: spawning the partner's managers at match setup sometimes fails -> retry when the partner first shows
    if (member == 1 && (!m.snd[1] || !m.eff[1]) && m.actor) PrepareManagers(m, side, m.actor, m.ids[1]);
    // voice and effects follow the character in control
    if (m.snd[member]) CallSetter(m.actor, "TekkenPlayer", "SetSoundManager", m.snd[member]);
    if (m.eff[member]) CallSetter(m.actor, "TekkenPlayer", "SetEffectManager", m.eff[member]);
    m.shown = member;
    Log("model: P%d shows %s (%d parts changed, sound %s, effect %s)", side + 1, member ? "partner" : "main", changed,
        ue::Name(m.snd[member]).c_str(), ue::Name(m.eff[member]).c_str());
}

void Prepare(int side, int mainId, int partnerId) {
    SideModel& m = g_m[side];
    // release the previous match's objects
    for (int k = 0; k < 2; ++k) for (int p = 0; p < kSlots; ++p) ue::SetRoot(m.mesh[k][p], false);
    ue::SetRoot(m.set, false); ue::SetRoot(m.mainSet, false);
    for (uintptr_t o : m.partnerExtras) ue::SetRoot(o, false);
    int tries = m.ids[1] == partnerId && m.ids[0] == mainId ? m.tries + 1 : 1;
    if (m.doll && DollAlive(m)) {                       // the last match's doll actor goes away (if its level kept it)
        static uintptr_t destroy = ue::Function("Actor", "K2_DestroyActor");
        uint8_t z[8]{}; if (destroy) ue::Call(m.doll, destroy, z);
    }
    m = SideModel{};
    m.ids[0] = mainId; m.ids[1] = partnerId; m.tries = tries;
    if (!Resolve()) return;
    uintptr_t cm = ue::Instance("BP_CharacterManager_C");
    if (!cm) { Log("model: no BP_CharacterManager_C instance"); return; }
    uintptr_t actor = ActorFor(cm, side);
    if (!actor) { Log("model: P%d actor not found", side + 1); return; }
    m.actor = actor;
    m.imc = CallGetter(actor, "BP_TekkenPlayer_Modular_C", "GetItemManagerComponent");
    if (!m.imc) {                                       // the native getter is a blueprint event: find the component
        int n = ue::NumObjects();
        for (int i = 0; i < n && !m.imc; ++i) {
            uintptr_t o = ue::ObjectAtIndex(i);
            if (o && ue::Outer(o) == actor && ue::ClassName(o) == "ItemManagerComponent_C") m.imc = o;
        }
    }
    if (m.imc) Rd(m.imc + 0x150, m.mainSet);           // ItemManagerComponent_C.PlayerDefaultCostume (empty in fights)
    ue::SetRoot(m.mainSet, true);
    Log("model: P%d item manager %s, main costume set %s", side + 1, ue::Name(m.imc).c_str(), ue::Name(m.mainSet).c_str());
    int parts = 0;
    for (int p = 0; p < kSlots; ++p) {
        uint8_t a[16]{};
        a[0] = (uint8_t)p;
        if (!ue::Call(actor, F.getComp, a)) continue;
        uintptr_t c = At<uintptr_t>(a, 8);
        int owner = -1;
        for (int q = 0; q < p; ++q) if (m.comp[q] == c) owner = q;
        if (c && owner >= 0) { m.alias[p] = owner; continue; }   // same component as an earlier slot (HEAD / HAIR)
        if (!c) continue;
        m.comp[p] = c;
        m.mesh[0][p] = MeshOf(c);
        ue::SetRoot(m.mesh[0][p], true);
        if (F.animClassOff > 0) Rd(c + F.animClassOff, m.anim[0][p]);
        if (F.overrideOff > 0) {                        // keep the main's material overrides to put them back
            uintptr_t data = 0; int num = 0;
            Rd(c + F.overrideOff, data); Rd(c + F.overrideOff + 8, num);
            for (int i = 0; data && i < num && i < 32; ++i) {
                uintptr_t mt = 0; Rd(data + 8 * i, mt);
                m.mats[p].push_back(mt); ue::SetRoot(mt, true);
            }
        }
        ++parts;
    }
    // 11-23: the partner's own costume choice, built by the game's loader (costume.cpp); else the default costume
    m.set = g_sel.partnerBoss[side] ? BossSet(partnerId) : 0;          // 11-26 boss version: its story costume
    if (!m.set && g_cfg.partnerCustom) m.set = CostumeSet(side, partnerId);
    Log("model: P%d partner costume: %s", side + 1, m.set ? ue::Name(m.set).c_str() : "default");
    if (!m.set) m.set = LoadDefaultSet(side, partnerId);
    if (!m.set) { Log("model: P%d no costume set for character %d", side + 1, partnerId); return; }
    ue::SetRoot(m.set, true);
    int found = 0;
    for (int p = 0; p < kSlots; ++p) {
        if (!m.comp[p] && m.alias[p] < 0) continue;
        uint8_t a[16]{};
        a[0] = (uint8_t)p;
        uintptr_t mesh = 0;
        if (ue::Call(m.set, F.meshAt, a)) mesh = At<uintptr_t>(a, 8);
        if (m.alias[p] >= 0) {                          // e.g. the partner's hair sits in HAIR, the main's in HEAD
            int q = m.alias[p];
            if (mesh && !m.mesh[1][q]) { m.mesh[1][q] = mesh; m.anim[1][q] = AnimClassFor(mesh); ue::SetRoot(mesh, true); ++found;
                Log("model: P%d %-9s (same component as %s) partner %s", side + 1, kSlotName[p], kSlotName[q], ue::Name(mesh).c_str()); }
            continue;
        }
        m.mesh[1][p] = mesh;
        m.anim[1][p] = AnimClassFor(mesh);
        if (m.mesh[1][p]) { ue::SetRoot(m.mesh[1][p], true); ++found; }
        Log("model: P%d %-9s comp %-13s main %-22s %-22s partner %-22s %s", side + 1, kSlotName[p], ue::Name(m.comp[p]).c_str(),
            ue::Name(m.mesh[0][p]).c_str(), ue::Name(m.anim[0][p]).c_str(), m.mesh[1][p] ? ue::Name(m.mesh[1][p]).c_str() : "-",
            ue::Name(m.anim[1][p]).c_str());
    }
    if (uintptr_t own = CharacterAnimClass(partnerId)) {   // the partner's own anim blueprint wins (Jack-7 ...)
        for (int p = 0; p < kSlots; ++p) if (m.mesh[1][p]) m.anim[1][p] = own;
        Log("model: P%d partner parts use %s", side + 1, ue::Name(own).c_str());
    }
    m.ready = found > 0;
    if (m.ready) PrepareManagers(m, side, actor, partnerId);
    if (m.ready) AddBossExtras(side, mainId, actor);   // 11-26 boss main: its set's wings / hair (the game leaves them out)
    if (m.ready) {                                      // the main's extra meshes (devil wings ...): hidden for the partner
        int n = ue::NumObjects();
        for (int i = 0; i < n; ++i) {
            uintptr_t o = ue::ObjectAtIndex(i);
            if (!o || ue::Outer(o) != actor || ue::ClassName(o) != "SkeletalMeshComponent") continue;
            bool known = false;
            for (int p = 0; p < kSlots; ++p) if (m.comp[p] == o) known = true;
            if (known) continue;
            m.extras.push_back(o);
            m.extraMaster.push_back(MasterOf(o));
            Log("model: P%d extra part %s mesh %s", side + 1, ue::Name(o).c_str(), ue::Name(MeshOf(o)).c_str());
        }
    }
    Log("model: P%d %s: actor %s, %d parts, partner (id %d) meshes %d", side + 1, m.ready ? "ready" : "NOT ready",
        ue::Name(actor).c_str(), parts, partnerId, found);
}
}  // namespace

bool ModelAvailable() { return g_cfg.modelSwap && ue::HookReady(); }

// 11-22 research (slot lab "spawn:side"): a second character actor of the same class, next to this side's fighter.
// Goal: a look-only double (no battle logic) so both characters can be seen during an assault, as in TTT2.
uintptr_t g_doubleActor = 0;
void ModelSpawnDouble(int side, int playerNumber) {
    if (!ue::HookReady()) return;
    ue::RunOnGameThread([side, playerNumber] {
        uintptr_t actor = g_m[side].actor;
        if (!actor) { Log("double: P%d has no actor (not a tag side?)", side + 1); return; }
        uintptr_t cls = 0; Rd(actor + 0x10, cls);
        Log("double: P%d spawning %s next to %s", side + 1, ue::Name(cls).c_str(), ue::Name(actor).c_str());
        uintptr_t d = SpawnActor(cls, actor);
        if (!d) { Log("double: spawn failed"); return; }
        ue::SetRoot(d, true);
        static uintptr_t getLoc = ue::Function("Actor", "K2_GetActorLocation"), setLoc = ue::Function("Actor", "K2_SetActorLocation");
        float loc[3]{};
        uint8_t g[16]{};
        if (getLoc && ue::Call(actor, getLoc, g)) memcpy(loc, g, 12);
        // the visible body: the main's upper-body mesh component in the world (the actor root may sit elsewhere)
        static uintptr_t compLoc = ue::Function("SceneComponent", "K2_GetComponentLocation");
        float body[3]{};
        uintptr_t up = g_m[side].comp[2];
        if (up && compLoc) { uint8_t c[16]{}; if (ue::Call(up, compLoc, c)) memcpy(body, c, 12); }
        Log("double: actor root %.0f %.0f %.0f, upper body %.0f %.0f %.0f", loc[0], loc[1], loc[2], body[0], body[1], body[2]);
        if (up) memcpy(loc, body, 12);
        loc[0] -= 120.0f;                                     // a step beside the fighter
        std::vector<uint8_t> f(0x200, 0);
        memcpy(f.data(), loc, 12);
        static int tele = setLoc ? ue::PropOffset(setLoc, "bTeleport") : -1;
        if (tele > 0) f[tele] = 1;
        bool ok = setLoc && ue::Call(d, setLoc, f.data());
        Log("double: %s spawned at %.0f %.0f %.0f (moved %d)", ue::Name(d).c_str(), loc[0], loc[1], loc[2], ok);
        // dress it: the main's default costume through the double's own item manager (EquipCharacterItemSet)
        uintptr_t imc = 0;
        int n = ue::NumObjects();
        std::vector<uintptr_t> comps;
        for (int i = 0; i < n; ++i) {
            uintptr_t o = ue::ObjectAtIndex(i);
            if (!o || ue::Outer(o) != d) continue;
            if (ue::ClassName(o) == "ItemManagerComponent_C") imc = o;
            comps.push_back(o);
        }
        int mainId = g_m[side].ids[0];
        uintptr_t set = g_sel.mainBoss[side] == mainId ? BossSet(mainId) : 0;
        if (!set) set = LoadDefaultSet(side, mainId, side + 1);
        static uintptr_t equip = ue::Function("ItemManagerComponent_C", "EquipCharacterItemSet");
        bool eq = false;
        if (imc && set && equip) { uint8_t a[16]{}; memcpy(a, &set, 8); memcpy(a + 8, &mainId, 4); eq = ue::Call(imc, equip, a); }
        Log("double: %d components, item manager %s, set %s, equip %s", (int)comps.size(), ue::Name(imc).c_str(),
            ue::Name(set).c_str(), eq ? "called" : "not called");
        static uintptr_t setVisBp = ue::Function("BP_TekkenPlayer_Modular_C", "SetVisible");
        if (setVisBp) { std::vector<uint8_t> v(0x40, 0); v[0] = 1; ue::Call(d, setVisBp, v.data()); }
        static uintptr_t setVis = ue::Function("SceneComponent", "SetVisibility");
        static uintptr_t hiddenFn = ue::Function("Actor", "SetActorHiddenInGame");
        if (hiddenFn) { uint8_t h[8]{}; ue::Call(d, hiddenFn, h); }
        for (uintptr_t o : comps)
            if (ue::ClassName(o) == "SkeletalMeshComponent") {
                uint8_t a[8]{}; a[0] = 1; a[1] = 1; if (setVis) ue::Call(o, setVis, a);
                uint8_t c[16]{}; float w[3]{}; if (compLoc && ue::Call(o, compLoc, c)) memcpy(w, c, 12);
                Log("double:   %s mesh %s at %.0f %.0f %.0f", ue::Name(o).c_str(), ue::Name(MeshOf(o)).c_str(), w[0], w[1], w[2]);
            }
        // 11-23 doll: the pose comes from AnimNode_MotHead, which reads GetPlayer(PlayerNumber) -> that player's
        // motion block. PlayerNumber lives on the actor (TekkenPlayer +0x3E0) and on every TekkenAnimInstance (+0x478)
        if (playerNumber >= 0) {
            Wr(d + 0x3E0, playerNumber);
            // the spawned actor has no anim blueprint (the character manager sets it at load): the main body's one
            uintptr_t abc = 0; if (up && F.animClassOff > 0) Rd(up + F.animClassOff, abc);
            static uintptr_t skc = ue::Find("Class", "/Script/Engine", "SkeletalMeshComponent");
            static int instOff = skc ? ue::PropOffset(skc, "AnimScriptInstance") : -1;
            int set = 0;
            for (uintptr_t o : comps) {
                if (ue::ClassName(o) != "SkeletalMeshComponent") continue;
                if (abc && F.setAnimClass) { uint8_t c[8]; memcpy(c, &abc, 8); ue::Call(o, F.setAnimClass, c); }
                uintptr_t inst = 0; if (instOff > 0) Rd(o + instOff, inst);
                if (inst) { Wr(inst + 0x478, playerNumber); ++set; }
            }
            Log("double: anim class %s, player number %d on the actor and %d anim instances", ue::Name(abc).c_str(), playerNumber, set);
        }
        g_doubleActor = d;
    });
}

// extra items (devil wings ...) of an item set added to an actor with the game's "Add Extra Item"; the new parts
// follow `leader` (the body). Returns the new skeletal mesh components.
std::vector<uintptr_t> AddSetExtras(uintptr_t actor, uintptr_t set, uintptr_t leader) {
    std::vector<uintptr_t> out;
    static uintptr_t addExtra = ue::Function("BP_TekkenPlayer_Modular_C", "Add Extra Item");
    uintptr_t extraSet = 0; if (set) Rd(set + 0x40, extraSet);
    uintptr_t arr = 0; int num = 0;
    if (extraSet) { Rd(extraSet + 0x30, arr); Rd(extraSet + 0x38, num); }
    if (!addExtra || !arr || num <= 0) return out;
    std::vector<uintptr_t> before;
    int n = ue::NumObjects();
    for (int i = 0; i < n; ++i) { uintptr_t o = ue::ObjectAtIndex(i); if (o && ue::Outer(o) == actor) before.push_back(o); }
    static int genOff = -1;
    if (genOff < 0) { uintptr_t bpc = ue::Find("Class", "/Script/Engine", "Blueprint"); genOff = bpc ? ue::PropOffset(bpc, "GeneratedClass") : -2; }
    for (int k = 0; k < num && k < 8; ++k) {
        uintptr_t cci = 0; Rd(arr + 8 * k, cci);
        if (!cci) continue;
        uintptr_t ci = 0, ab = 0, cls = 0; Rd(cci + 0x30, ci); if (ci) Rd(ci + 0x168, ab);
        if (ab && genOff > 0) Rd(ab + genOff, cls);
        std::vector<uint8_t> f(0x200, 0);
        memcpy(f.data(), &cls, 8); memcpy(f.data() + 8, &cci, 8);
        ue::Call(actor, addExtra, f.data());
    }
    n = ue::NumObjects();
    uintptr_t abc = 0; if (leader && F.animClassOff > 0) Rd(leader + F.animClassOff, abc);
    for (int i = 0; i < n; ++i) {
        uintptr_t o = ue::ObjectAtIndex(i);
        if (!o || ue::Outer(o) != actor || ue::ClassName(o) != "SkeletalMeshComponent") continue;
        if (std::find(before.begin(), before.end(), o) != before.end()) continue;
        if (abc && F.setAnimClass) { uint8_t c[8]; memcpy(c, &abc, 8); ue::Call(o, F.setAnimClass, c); }
        if (leader && F.setMaster) { uint8_t b[16]{}; memcpy(b, &leader, 8); ue::Call(o, F.setMaster, b); }
        out.push_back(o);                              // not rooted: the doll's parts leave with it
    }
    return out;
}


// 11-26 a boss main's extra items (True Devil Kazuya's wings, story Akuma's hair): the game equips the story set without
// them in a fight, so they are added like the partner's, once per fighter actor
void AddBossExtras(int side, int mainId, uintptr_t actor) {
    static uintptr_t done[2] = {};
    if (side < 0 || side > 1 || g_sel.mainBoss[side] != mainId || done[side] == actor) return;
    uintptr_t set = BossSet(mainId);
    if (!set) return;
    ue::SetRoot(set, true);
    uintptr_t body = 0;
    for (int p : {2, 3}) { uint8_t a[16]{}; a[0] = (uint8_t)p; if (!body && ue::Call(actor, F.getComp, a)) body = At<uintptr_t>(a, 8); }
    std::vector<uintptr_t> parts = AddSetExtras(actor, set, body);
    done[side] = actor;
    Log("model: P%d boss main extra parts: %d", side + 1, (int)parts.size());
}

// 11-23 doll (doll.cpp): a second actor of the main's class, dressed as the leaving member (meshes, anim blueprints,
// the main's material overrides / the partner's colours), PlayerNumber 2 = its pose comes from the spare motion block.
// Spawned on first use, then only shown / hidden. Game thread.
bool ModelDoll(int side, int member, bool show) {
    if (!ModelAvailable() || side < 0 || side > 1 || member < 0 || member > 1) return false;
    SideModel& m = g_m[side];
    if (!m.actor || !Resolve()) return false;
    static uintptr_t hiddenFn = ue::Function("Actor", "SetActorHiddenInGame");
    static uintptr_t getLoc = ue::Function("Actor", "K2_GetActorLocation"), setLoc = ue::Function("Actor", "K2_SetActorLocation");
    static uintptr_t setVis = ue::Function("SceneComponent", "SetVisibility");
    if (!show) {
        if (m.doll && DollAlive(m) && hiddenFn) { uint8_t h[8]{}; h[0] = 1; ue::Call(m.doll, hiddenFn, h); }
        return true;
    }
    if (m.doll && !DollAlive(m)) {                      // gone with its level: a new one
        Log("doll: P%d actor gone, spawning a new one", side + 1);
        m.doll = 0; m.dollImc = 0; m.dollDressed = -1;
        for (int p = 0; p < kSlots; ++p) m.dollComp[p] = 0;
        for (int k = 0; k < 2; ++k) { m.dollExtras[k].clear(); m.dollExtrasTried[k] = false; }
    }
    if (!m.doll) {
        uintptr_t d = SpawnActor(ue::Class(m.actor), m.actor);
        if (!d) { Log("doll: spawn failed"); return false; }
        m.doll = d;                                     // not rooted: it leaves with its level (11-23 reload crash)
        Rd(d + 0xC, m.dollIdx);                         // UObject internal index
        for (int p = 0; p < kSlots; ++p) {
            if (!m.comp[p]) continue;
            uint8_t a[16]{}; a[0] = (uint8_t)p;
            if (ue::Call(d, F.getComp, a)) m.dollComp[p] = At<uintptr_t>(a, 8);
        }
        int n = ue::NumObjects();
        for (int i = 0; i < n && !m.dollImc; ++i) {
            uintptr_t o = ue::ObjectAtIndex(i);
            if (o && ue::Outer(o) == d && ue::ClassName(o) == "ItemManagerComponent_C") m.dollImc = o;
        }
        Log("doll: P%d actor %s spawned (item manager %s)", side + 1, ue::Name(d).c_str(), ue::Name(m.dollImc).c_str());
    }
    static uintptr_t skc = ue::Find("Class", "/Script/Engine", "SkeletalMeshComponent");
    static int instOff = skc ? ue::PropOffset(skc, "AnimScriptInstance") : -1;
    if (m.dollDressed != member) {
        for (int p = 0; p < kSlots; ++p) {
            uintptr_t c = m.dollComp[p];
            if (!c || !m.comp[p]) continue;
            uintptr_t mesh = m.mesh[member][p] ? m.mesh[member][p] : m.mesh[0][p];
            uint8_t a[16]{}; memcpy(a, &mesh, 8); a[8] = 1;
            ue::Call(c, F.setMesh, a);
            uintptr_t cls = m.anim[member][p] ? m.anim[member][p] : m.anim[0][p];
            if (cls && F.setAnimClass) { uint8_t k[8]; memcpy(k, &cls, 8); ue::Call(c, F.setAnimClass, k); }
            if (F.setMaterial) {
                int cnt = (int)m.mats[p].size();
                if (F.overrideOff > 0) { int cur = 0; Rd(c + F.overrideOff + 8, cur); cnt = (std::max)(cnt, cur); }
                for (int i = 0; i < cnt && i < 32; ++i) {
                    uintptr_t mt = member == 0 && i < (int)m.mats[p].size() ? m.mats[p][i] : 0;
                    uint8_t d[16]{}; memcpy(d, &i, 4); memcpy(d + 8, &mt, 8);
                    ue::Call(c, F.setMaterial, d);
                }
            }
        }
        if (member == 1 && m.dollImc && m.set && g_cfg.modelEquip) {   // the partner's colours, as on the fighter
            static uintptr_t setAll = ue::Function("ItemManagerComponent_C", "SetAllCustomizeInfo");
            if (setAll) { std::vector<uint8_t> f(0x80, 0); memcpy(f.data(), &m.set, 8); ue::Call(m.dollImc, setAll, f.data()); }
        }
        m.dollDressed = member;
        // extra items (wings): the main's default costume set / the partner's set, added once per member
        uintptr_t body = m.dollComp[2] ? m.dollComp[2] : m.dollComp[3];
        if (!m.dollExtrasTried[member]) {
            m.dollExtrasTried[member] = true;
            uintptr_t set = member == 1 ? m.set : g_sel.mainBoss[side] == m.ids[0] ? BossSet(m.ids[0]) : 0;
            if (!set) set = LoadDefaultSet(side, m.ids[0]);
            m.dollExtras[member] = AddSetExtras(m.doll, set, body);
            Log("doll: P%d member %d extra parts: %d (set %s)", side + 1, member, (int)m.dollExtras[member].size(), ue::Name(set).c_str());
        }
        for (int k = 0; k < 2; ++k)
            for (uintptr_t o : m.dollExtras[k]) {
                if (k == member && body && F.setMaster) { uint8_t b[16]{}; memcpy(b, &body, 8); ue::Call(o, F.setMaster, b); }
                if (setVis) { uint8_t v[8]{}; v[0] = k == member; v[1] = 1; ue::Call(o, setVis, v); }
            }
    }
    Wr(m.doll + 0x3E0, 2);                                // TekkenPlayer.PlayerNumber
    for (int p = 0; p < kSlots; ++p) {
        uintptr_t c = m.dollComp[p], inst = 0;
        if (!c) continue;
        if (instOff > 0) Rd(c + instOff, inst);
        if (inst) Wr(inst + 0x478, 2);                     // TekkenAnimInstance.PlayerNumber
        if (setVis) { uint8_t v[8]{}; v[0] = 1; v[1] = 1; ue::Call(c, setVis, v); }
    }
    if (getLoc && setLoc) {                               // the pose is in world space; the actor near it keeps it in view
        uint8_t g[16]{}; float loc[3]{};
        if (ue::Call(m.actor, getLoc, g)) memcpy(loc, g, 12);
        std::vector<uint8_t> f(0x200, 0); memcpy(f.data(), loc, 12);
        static int tele = ue::PropOffset(setLoc, "bTeleport");
        if (tele > 0) f[tele] = 1;
        ue::Call(m.doll, setLoc, f.data());
    }
    if (hiddenFn) { uint8_t h[8]{}; ue::Call(m.doll, hiddenFn, h); }
    return true;
}

void ModelPrepare(int side, int mainId, int partnerId, int oppId) {
    if (side >= 0 && side < 2) { g_oppModel[side] = oppId; }
    if (!ModelAvailable()) return;
    ue::RunOnGameThread([=] { Prepare(side, mainId, partnerId); });
}

void ModelBossSolo(int side, int mainId) {
    if (!ModelAvailable() || side < 0 || side > 1) return;
    ue::RunOnGameThread([=] {
        if (!Resolve()) return;
        uintptr_t cm = ue::Instance("BP_CharacterManager_C");
        uintptr_t actor = cm ? ActorFor(cm, side) : 0;
        if (actor) AddBossExtras(side, mainId, actor);
    });
}

void ModelShow(int side, int member) {
    if (!ModelAvailable()) return;
    ue::RunOnGameThread([=] { Show(side, member); });
}

// early on the game thread: find the engine functions once (no hitch at the first match), proves the call path
void ModelWarmup() {
    if (!ModelAvailable()) return;
    ue::RunOnGameThread([] {
        ULONGLONG t = GetTickCount64();
        Resolve();
        Log("model: warmup on the game thread took %llu ms", GetTickCount64() - t);
    });
}

void ModelForget(int side) {
    if (!ModelAvailable()) return;
    ue::RunOnGameThread([=] { g_m[side].ready = false; });
}

// ---- HUD portrait (11-21): the game's own art, not a screen grab. The fight HUD shows
// /Game/UI_common/Texture2D/ReplaceImages/HUD_CH_ICON_{L|R}/HUD_CH_ICON_{L|R}_<CODE>; the partner's texture is loaded
// and handed to the HUD movies with GFxMoviePlayer.SetExternalTexture (true = that movie has the resource). The
// resource name is probed once from a short list and remembered; everything is logged ("hudicon:").
namespace {
int g_iconShown[2] = {-1, -1};     // character whose texture we put into the HUD (-1 = the game's own)
int g_iconOk[2] = {-1, -1};        // character id the HUD portrait shows after our last successful switch
int g_gameIcon[2] = {-1, -1};      // character the game loaded on this side (its portrait is the HUD's own)

// one HUD picture kind ("ICON" portrait / "NAME" plate) of a side -> charId; returns how many movies took it
int HudSwap(int side, int charId, const char* kind, std::string& took) {
    static uintptr_t setExt = 0, texCls = 0;
    if (!setExt) setExt = ue::Function("GFxMoviePlayer", "SetExternalTexture");
    if (!texCls) texCls = ue::Find("Class", "/Script/Engine", "Texture2D");
    if (!setExt || !texCls) return -1;
    char lr = side == 0 ? 'L' : 'R';
    char path[200];
    sprintf_s(path, "/Game/UI_common/Texture2D/ReplaceImages/HUD_CH_%s_%c/HUD_CH_%s_%c_%s.HUD_CH_%s_%c_%s", kind, lr, kind, lr,
              kCode[charId], kind, lr, kCode[charId]);
    uintptr_t tex = LoadAsset(texCls, path);
    if (!tex) { Log("hudicon: P%d texture not loaded: %s", side + 1, path); return -1; }
    ue::SetRoot(tex, true);
    char rep[64]; sprintf_s(rep, "Rep_HUD_CH_%s_%c", kind, lr);
    std::wstring w(rep, rep + strlen(rep));
    int hits = 0;
    int n = ue::NumObjects();
    for (int i = 0; i < n; ++i) {
        uintptr_t o = ue::ObjectAtIndex(i);
        if (!o || ue::ClassName(o) != "GFxMoviePlayer" || ue::Name(o).rfind("Default__", 0) == 0) continue;
        uint8_t a[0x20]{};
        const wchar_t* data = w.c_str();
        int32_t num = (int32_t)w.size() + 1;
        memcpy(a, &data, 8); memcpy(a + 8, &num, 4); memcpy(a + 12, &num, 4);
        memcpy(a + 0x10, &tex, 8);
        if (ue::Call(o, setExt, a) && a[0x18]) {
            if (!hits) took = std::string(rep) + " on " + ue::Name(o);
            ++hits;
        }
    }
    Log("hudicon: P%d %s -> %s: %d movies took it%s%s", side + 1, kind, ue::Name(tex).c_str(), hits,
        hits ? ", first: " : "", took.c_str());
    return hits;
}

void HudIconNow(int side, int charId) {
    if (charId < 0 || charId >= (int)(sizeof(kCode) / sizeof(kCode[0]))) return;
    std::string t1, t2;
    int icon = HudSwap(side, charId, "ICON", t1);
    HudSwap(side, charId, "NAME", t2);                  // the name plate (shown for CPU / VS sides)
    g_iconShown[side] = charId;
    g_iconOk[side] = icon > 0 ? charId : -1;
}
}  // namespace

void ModelHudIcon(int side, int charId) {
    if (!ue::HookReady() || side < 0 || side > 1) return;
    ue::RunOnGameThread([=] { HudIconNow(side, charId); });
}
bool ModelHudIconShows(int side, int charId) { return side >= 0 && side < 2 && charId >= 0 && g_iconOk[side] == charId; }
void ModelHudIconRestore(int side, int charId) {      // new match: put the game's own portrait back if we changed it
    if (side >= 0 && side < 2) g_gameIcon[side] = charId;
    if (side < 0 || side > 1 || g_iconShown[side] < 0 || g_iconShown[side] == charId) { if (side >= 0 && side < 2) g_iconShown[side] = -1; return; }
    ModelHudIcon(side, charId);
}

// a short bright "confirm" tone (two quick sine blips), played from memory
void PlayConfirmTone() {
    static std::vector<uint8_t> wav;
    if (wav.empty()) {
        const int rate = 44100, n = rate * 16 / 100;
        std::vector<int16_t> pcm(n);
        for (int i = 0; i < n; ++i) {
            double t = (double)i / rate, f = t < 0.06 ? 1320.0 : 1760.0;
            double env = t < 0.06 ? 1 - t / 0.07 : (1 - (t - 0.06) / 0.10);
            pcm[i] = (int16_t)(9000 * (env > 0 ? env : 0) * sin(2 * 3.14159265 * f * t));
        }
        auto put32 = [&](uint32_t v) { for (int k = 0; k < 4; ++k) wav.push_back((uint8_t)(v >> (8 * k))); };
        auto put16 = [&](uint16_t v) { wav.push_back((uint8_t)v); wav.push_back((uint8_t)(v >> 8)); };
        wav.insert(wav.end(), {'R', 'I', 'F', 'F'}); put32(36 + n * 2);
        wav.insert(wav.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '}); put32(16); put16(1); put16(1); put32(rate); put32(rate * 2);
        put16(2); put16(16);
        wav.insert(wav.end(), {'d', 'a', 't', 'a'}); put32(n * 2);
        const uint8_t* b = (const uint8_t*)pcm.data();
        wav.insert(wav.end(), b, b + n * 2);
    }
    PlaySoundW((LPCWSTR)wav.data(), nullptr, SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
}

// ---------------------------------------------------------------- battle sound: Wwise events on the player's actor
// names separated by '+' all play (e.g. a metallic accent + a bass hit). PostEventByName has no return value.
void BattleSound(int side, const std::string& names) {
    if (!ue::HookReady() || names.empty()) return;
    ue::RunOnGameThread([side, names] {
        static uintptr_t fn = 0, cdo = 0;
        static int oName = -1, oActor = -1, size = 0;
        if (!fn) {
            fn = ue::Function("AkGameplayStatics", "PostEventByName");
            cdo = ue::Find("AkGameplayStatics", "/Script/AkAudio", "Default__AkGameplayStatics");
            if (fn) { oName = ue::PropOffset(fn, "EventName"); oActor = ue::PropOffset(fn, "Actor"); size = ue::ParmsSize(fn); }
        }
        if (!fn || !cdo || oName < 0 || size <= 0 || size > 256) return;
        uintptr_t actor = side >= 0 && side < 2 ? g_m[side].actor : 0;
        if (!actor) actor = ue::Instance("TekkenPlayerController");
        size_t start = 0;
        while (start < names.size()) {
            size_t plus = names.find('+', start);
            std::string one = names.substr(start, plus == std::string::npos ? std::string::npos : plus - start);
            start = plus == std::string::npos ? names.size() : plus + 1;
            if (one.empty()) continue;
            int times = 1;                                  // "name*3": posted 3 times at once = louder (Wwise has no volume parm here)
            size_t star = one.find('*');
            if (star != std::string::npos) { times = (std::max)(1, (std::min)(6, atoi(one.c_str() + star + 1))); one = one.substr(0, star); }
            for (int rep = 0; rep < times; ++rep) {
            static wchar_t buf[96];
            MultiByteToWideChar(CP_UTF8, 0, one.c_str(), -1, buf, 95);
            struct FStr { wchar_t* data; int num, max; } str{buf, (int)wcslen(buf) + 1, (int)wcslen(buf) + 1};
            uint8_t p[256]{};
            memcpy(p + oName, &str, sizeof(str));
            if (oActor >= 0) memcpy(p + oActor, &actor, 8);
            bool ok = ue::Call(cdo, fn, p);
            if (rep == 0) Log("sound: P%d %s x%d %s", side + 1, one.c_str(), times, ok ? "posted" : "failed");
            }
        }
    });
}

// ---------------------------------------------------------------- UI sound (Wwise event by name, e.g. "Decide_Red")
void UiSound(const char* ev) {
    if (!ue::HookReady()) return;
    std::string name = ev;
    ue::RunOnGameThread([name] {
        static uintptr_t fn = 0, cdo = 0;
        static int oName = -1, oActor = -1, size = 0;
        if (!fn) {
            fn = ue::Function("AkGameplayStatics", "PostEventByName");
            cdo = ue::Find("AkGameplayStatics", "/Script/AkAudio", "Default__AkGameplayStatics");
            if (fn) { oName = ue::PropOffset(fn, "EventName"); oActor = ue::PropOffset(fn, "Actor"); size = ue::ParmsSize(fn); }
            Log("model: UI sound function %s (EventName +%d, Actor +%d, %d bytes)", fn && cdo ? "found" : "MISSING", oName, oActor, size);
        }
        if (!fn || !cdo || oName < 0 || size <= 0 || size > 256) return;
        uintptr_t actor = ue::Instance("TekkenPlayerController");
        if (!actor) actor = ue::Instance("BP_CharacterManager_C");
        // several candidates separated by '|': the first one Wwise knows plays
        static int oRet = ue::PropOffset(fn, "ReturnValue");
        size_t start = 0;
        while (start <= name.size()) {
            size_t bar = name.find('|', start);
            std::string one = name.substr(start, bar == std::string::npos ? std::string::npos : bar - start);
            static wchar_t buf[64];
            MultiByteToWideChar(CP_UTF8, 0, one.c_str(), -1, buf, 63);
            struct FStr { wchar_t* data; int num, max; } str{buf, (int)wcslen(buf) + 1, (int)wcslen(buf) + 1};
            uint8_t p[256]{};
            memcpy(p + oName, &str, sizeof(str));
            if (oActor >= 0) memcpy(p + oActor, &actor, 8);
            bool ok = ue::Call(cdo, fn, p);
            int id = 0; if (oRet >= 0) memcpy(&id, p + oRet, 4);
            Log("model: UI sound %s %s (playing id %d%s)", one.c_str(), ok ? "posted" : "failed", id, id ? "" : " = not known");
            if (id) return;
            if (bar == std::string::npos) break;
            start = bar + 1;
        }
        PlayConfirmTone();                              // no Wwise event answered: our own short confirm tone
    });
}
