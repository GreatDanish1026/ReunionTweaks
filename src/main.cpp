// ReunionTweaks: a ReShade add-on for Life is Strange: Reunion (UE 5.6, DirectX 12) that raises
// the game's lighting, shadow and detail settings beyond its menu, live, from an in-game overlay.
//
// Everything goes through the game's console variables (cvars.hpp: console manager found by
// code scan, vtable layout read from the game's own call sites). Settings are kept in
// ReShade.ini [ReunionTweaks] and applied once the console manager is found. The game's own
// SetByCode writes (its menu) win over our set-only writes, so the add-on checks its values
// every second and puts back any the game changed. F8 switches all of it off and on.
//
// The exe is protected against tampering, so the add-on never patches game code: it only reads
// it (to find the console manager) and calls the engine's own console variable functions.
//
// No Engine.ini is written; nothing is left behind when the add-on is removed.
// Writes ReunionTweaks.log (and ReunionTweaks-probe.txt) next to the game executable.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#define ImTextureID ImU64
#include <imgui.h>
#include <reshade.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "cvars.hpp"
#include "version.h"

extern "C" __declspec(dllexport) const char* NAME = DT_PRODUCT;
extern "C" __declspec(dllexport) const char* DESCRIPTION =
    "Lighting, shadow and detail settings beyond " DT_GAME "'s menu, live from the overlay.";

namespace {

using namespace reshade::api;

constexpr int kToggleHotkey = VK_F8;

// ---------------------------------------------------------------- logging

std::mutex g_logMutex;
FILE* g_log = nullptr;
ULONGLONG g_loadTick = 0;
std::wstring g_exeDir;

void Log(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    std::lock_guard<std::mutex> lock(g_logMutex);
    if (!g_log) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(g_log, "%02u:%02u:%02u.%03u [+%5.1f s] %s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds,
            (GetTickCount64() - g_loadTick) / 1000.0, buf);
    fflush(g_log);
}

void OpenLog() {
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (!n || n >= MAX_PATH) return;
    g_exeDir.assign(path, n);
    g_exeDir.resize(g_exeDir.find_last_of(L"\\/") + 1);
    g_log = _wfopen((g_exeDir + L"ReunionTweaks.log").c_str(), L"w");
    Log(DT_PRODUCT " " DT_VERSION " loaded");
}

// Cvar names and values are ASCII.
std::string Narrow(const std::wstring& w) {
    std::string s;
    for (wchar_t c : w) s += static_cast<char>(c);
    return s;
}

// ---------------------------------------------------------------- options
//
// Each option is a combo; choice id 0 leaves the game's value. The chosen id is kept in
// ReShade.ini. Game values quoted come from the probe (docs/FINDINGS.md).

using CvarList = std::vector<std::pair<const wchar_t*, const wchar_t*>>;

struct Choice {
    const char* label;
    int id;
    CvarList cvars;
};

struct Option {
    const char* tab;
    const char* section;
    const char* key;       // ReShade.ini
    const char* label;
    const char* tip;
    const wchar_t* shown;  // cvar whose game value the "Game" choice shows (or null)
    std::vector<Choice> choices;
    int value = 0;
};

// The game ships without ray tracing shaders (r.RayTracing=1 in Engine.ini stops it at start:
// "Missing global shader FNiagaraCollisionRayTraceMiss"), so Lumen always runs in software and
// nothing here uses hardware ray tracing.
std::vector<Option> g_options = {
    // ------------------------------------------------ lighting (software Lumen)
    {"Lighting", "Global illumination", "GiTracing", "Bounce light tracing",
     "What Lumen traces bounce light against. The game (High and Epic alike) traces only a\n"
     "coarse whole-scene distance field: soft, blobby bounce light and leaks through thin\n"
     "walls. Detailed traces each object's own distance field near the camera.\n"
     "Expensive: about 40% longer frames on an RTX 5080 at 4K.",
     L"r.Lumen.TraceMeshSDFs.Allow",
     {{"Game", 0, {}}, {"Detailed", 1, {{L"r.Lumen.TraceMeshSDFs.Allow", L"1"}}}}},
    {"Lighting", "Global illumination", "GiProbes", "Bounce light detail",
     "Spacing of Lumen's lighting probes in pixels and how many rays each traces. The menu\n"
     "uses 32 px on High and 16 px on Epic, with a budget of 300 far-field probes. Epic+ is\n"
     "16 px with more probes; Cinematic is 8 px with twice the rays per probe: steadier,\n"
     "more detailed indirect light on faces and walls. Epic+ is nearly free; Cinematic is\n"
     "expensive (about 65% longer frames on an RTX 5080 at 4K).",
     L"r.Lumen.ScreenProbeGather.DownsampleFactor",
     {{"Game", 0, {}},
      {"Epic+", 16,
       {{L"r.Lumen.ScreenProbeGather.DownsampleFactor", L"16"},
        {L"r.Lumen.ScreenProbeGather.RadianceCache.NumProbesToTraceBudget", L"500"}}},
      {"Cinematic", 8,
       {{L"r.Lumen.ScreenProbeGather.DownsampleFactor", L"8"},
        {L"r.Lumen.ScreenProbeGather.TracingOctahedronResolution", L"16"},
        {L"r.Lumen.ScreenProbeGather.RadianceCache.NumProbesToTraceBudget", L"1000"}}}}},
    {"Lighting", "Reflections", "ReflRoughness", "Reflections on rougher surfaces",
     "Lumen traces reflections up to this roughness; rougher surfaces (wood, painted\n"
     "walls, skin) fall back to blurry probe lighting. The game leaves it to each scene.\n"
     "About 5% (0.6) and 10% (0.8) longer frames.",
     L"r.Lumen.Reflections.MaxRoughnessToTrace",
     {{"Game", 0, {}},
      {"0.6", 6,
       {{L"r.Lumen.Reflections.MaxRoughnessToTrace", L"0.6"}, {L"r.Lumen.Reflections.MaxRoughnessToTraceClamp", L"1"}}},
      {"0.8", 8,
       {{L"r.Lumen.Reflections.MaxRoughnessToTrace", L"0.8"}, {L"r.Lumen.Reflections.MaxRoughnessToTraceClamp", L"1"}}}}},
    {"Lighting", "Reflections", "ReflGlass", "Glass and water reflections",
     "Lumen reflections on transparent surfaces (windows, glasses, water). The game\n"
     "allows them but leaves them off.",
     L"r.Lumen.TranslucencyReflections.FrontLayer.Enable",
     {{"Game", 0, {}}, {"On", 1, {{L"r.Lumen.TranslucencyReflections.FrontLayer.Enable", L"1"}}}}},

    // ------------------------------------------------ shadows
    {"Shadows", "Sun, moon and lamps", "ShadowRes", "Shadow sharpness",
     "Resolution of the virtual shadow maps (lower bias = sharper). The menu's Epic uses\n"
     "-0.5 for the sun but 0 for lamps and 0.5 for lamp shadows of anything moving\n"
     "(characters); High is coarser everywhere. Sharp: the sun's Epic sharpness for lamps\n"
     "and moving objects too, with twice the shadow memory. Sharpest: twice that again.",
     L"r.Shadow.Virtual.ResolutionLodBiasLocal",
     {{"Game", 0, {}},
      {"Sharp", 1,
       {{L"r.Shadow.Virtual.MaxPhysicalPages", L"8192"},
        {L"r.Shadow.Virtual.ResolutionLodBiasDirectional", L"-0.5"},
        {L"r.Shadow.Virtual.ResolutionLodBiasDirectionalMoving", L"-0.5"},
        {L"r.Shadow.Virtual.ResolutionLodBiasLocal", L"-0.5"},
        {L"r.Shadow.Virtual.ResolutionLodBiasLocalMoving", L"-0.5"}}},
      {"Sharpest", 2,
       {{L"r.Shadow.Virtual.MaxPhysicalPages", L"16384"},
        {L"r.Shadow.Virtual.ResolutionLodBiasDirectional", L"-1"},
        {L"r.Shadow.Virtual.ResolutionLodBiasDirectionalMoving", L"-1"},
        {L"r.Shadow.Virtual.ResolutionLodBiasLocal", L"-1"},
        {L"r.Shadow.Virtual.ResolutionLodBiasLocalMoving", L"-1"}}}}},
    {"Shadows", "Sun, moon and lamps", "ShadowSoft", "Soft shadow quality",
     "Rays per pixel for the soft edges of shadows. The menu uses 4 rays of 2 samples on\n"
     "High and 8 of 4 on Epic. More rays give smoother, steadier soft shadows.",
     L"r.Shadow.Virtual.SMRT.RayCountLocal",
     {{"Game", 0, {}},
      {"Epic (8 rays)", 8,
       {{L"r.Shadow.Virtual.SMRT.RayCountDirectional", L"8"},
        {L"r.Shadow.Virtual.SMRT.SamplesPerRayDirectional", L"4"},
        {L"r.Shadow.Virtual.SMRT.RayCountLocal", L"8"},
        {L"r.Shadow.Virtual.SMRT.SamplesPerRayLocal", L"4"}}},
      {"Cinematic (16 rays)", 16,
       {{L"r.Shadow.Virtual.SMRT.RayCountDirectional", L"16"},
        {L"r.Shadow.Virtual.SMRT.SamplesPerRayDirectional", L"8"},
        {L"r.Shadow.Virtual.SMRT.RayCountLocal", L"16"},
        {L"r.Shadow.Virtual.SMRT.SamplesPerRayLocal", L"8"}}}}},
    {"Shadows", "Sun, moon and lamps", "ShadowDistance", "Shadow distance",
     "How far away lamps still cast shadows. The menu uses 85% on High, 100% on Epic.",
     L"r.Shadow.DistanceScale",
     {{"Game", 0, {}}, {"Full (100%)", 100, {{L"r.Shadow.DistanceScale", L"1"}}},
      {"Far (150%)", 150, {{L"r.Shadow.DistanceScale", L"1.5"}}}}},
    {"Shadows", "Characters", "CapsuleShadows", "Character soft shadows",
     "The soft indirect shadows characters cast on the ground and walls. The game renders\n"
     "them at half resolution.",
     L"r.CapsuleShadowsFullResolution",
     {{"Game", 0, {}}, {"Full resolution", 1, {{L"r.CapsuleShadowsFullResolution", L"1"}}}}},

    // ------------------------------------------------ characters
    {"Characters", "Detail", "CharLod", "Character detail in gameplay",
     "Deck Nine's detail manager uses full-detail character models only in cutscenes; in\n"
     "free roam even the nearest characters drop one level (less detailed faces, hair\n"
     "and clothes). Full keeps cutscene detail everywhere. Background people too: also\n"
     "the crowd.",
     L"D9.SigMan.MinLOD.Default.Highest",
     {{"Game", 0, {}},
      {"Full", 1,
       {{L"D9.SigMan.MinLOD.Default.Highest", L"0"},
        {L"D9.SigMan.MinLOD.Default.High", L"0"},
        {L"D9.SigMan.MinLOD.Default.Mid", L"0"}}},
      {"Full, background people too", 2,
       {{L"D9.SigMan.MinLOD.Default.Highest", L"0"},
        {L"D9.SigMan.MinLOD.Default.High", L"0"},
        {L"D9.SigMan.MinLOD.Default.Mid", L"0"},
        {L"D9.SigMan.MinLOD.Chumps.Highest", L"0"},
        {L"D9.SigMan.MinLOD.Chumps.High", L"0"}}}}},
    {"Characters", "Skin and hair", "Skin", "Skin quality",
     "Subsurface scattering (light through skin) is computed on a checkerboard and filled\n"
     "in; Full computes every pixel: smoother skin, no fine dithering on faces.",
     L"r.SSS.Checkerboard",
     {{"Game", 0, {}}, {"Full", 1, {{L"r.SSS.Checkerboard", L"0"}}}}},
    {"Characters", "Skin and hair", "Hair", "Hair quality",
     "Strand hair samples per pixel and its shadow map resolution. The game uses 4 samples\n"
     "and 2048 px: fine strands break up and shimmer, most visible at reduced resolution\n"
     "scale.",
     L"r.HairStrands.Visibility.MSAA.SamplePerPixel",
     {{"Game", 0, {}},
      {"High", 1,
       {{L"r.HairStrands.Visibility.MSAA.SamplePerPixel", L"8"},
        {L"r.HairStrands.Visibility.MSAA.MeanSamplePerPixel", L"1"},
        {L"r.HairStrands.DeepShadow.Resolution", L"4096"}}}}},

    // ------------------------------------------------ detail
    {"Detail", "Textures", "TexturePool", "Texture memory",
     "Video memory for streamed textures. The game keeps 1.4 GB whatever the card, so\n"
     "textures go blurry and reload. The RTX 5080 has 16 GB.",
     L"r.Streaming.PoolSize",
     {{"Game", 0, {}},
      {"4 GB", 4096, {{L"r.Streaming.PoolSize", L"4096"}}},
      {"6 GB", 6144, {{L"r.Streaming.PoolSize", L"6144"}}},
      {"8 GB", 8192, {{L"r.Streaming.PoolSize", L"8192"}}}}},
    {"Detail", "Textures", "Anisotropy", "Anisotropic filtering",
     "Sharper floors and roads at shallow angles. The game uses 8x. Textures already loaded\n"
     "keep their filtering until they reload (or the game restarts).",
     L"r.MaxAnisotropy",
     {{"Game", 0, {}}, {"16x", 16, {{L"r.MaxAnisotropy", L"16"}, {L"r.VT.MaxAnisotropy", L"16"}}}}},
    {"Detail", "Distance", "ViewDistance", "Draw distance",
     "Distance at which objects, foliage and details switch to simpler versions or vanish.\n"
     "The menu's High uses 80% and thins foliage to 80%; Epic uses 100%.",
     L"r.ViewDistanceScale",
     {{"Game", 0, {}},
      {"Epic (100%)", 100, {{L"r.ViewDistanceScale", L"1"}, {L"foliage.DensityScale", L"1"}}},
      {"Far (150%)", 150,
       {{L"r.ViewDistanceScale", L"1.5"}, {L"foliage.DensityScale", L"1"}, {L"foliage.LODDistanceScale", L"1.5"}}}}},
    {"Detail", "Distance", "NaniteDetail", "Geometry detail",
     "Triangle size of the detailed (Nanite) models, in pixels. Smaller keeps more detail\n"
     "on distant and mid-range surfaces. Matters most at reduced resolution scale.",
     L"r.Nanite.MaxPixelsPerEdge",
     {{"Game", 0, {}}, {"Finer (0.5 px)", 1, {{L"r.Nanite.MaxPixelsPerEdge", L"0.5"}}}}},
    {"Detail", "Fog", "FogCells", "Volumetric fog detail",
     "Size of the fog's light grid in screen pixels. The menu uses 16 px and 64 depth\n"
     "slices on High (blocky light shafts, flicker around lamps) and 8 px / 128 on Epic.",
     L"r.VolumetricFog.GridPixelSize",
     {{"Game", 0, {}},
      {"Epic (8 px)", 8,
       {{L"r.VolumetricFog.GridPixelSize", L"8"}, {L"r.VolumetricFog.GridSizeZ", L"128"}}},
      {"Fine (4 px)", 4,
       {{L"r.VolumetricFog.GridPixelSize", L"4"}, {L"r.VolumetricFog.GridSizeZ", L"128"},
        {L"r.VolumetricFog.HistoryMissSupersampleCount", L"8"}}}}},
    {"Detail", "Image", "TsrHistory", "Upscaler precision",
     "TSR keeps its history in a compact colour format that can band in dark HDR scenes\n"
     "and gradients. High stores it at full precision.",
     L"r.TSR.History.R11G11B10",
     {{"Game", 0, {}}, {"High", 1, {{L"r.TSR.History.R11G11B10", L"0"}}}}},

    // ------------------------------------------------ post-processing (taste; in no preset)
    {"Post-processing", "Camera", "FilmGrain", "Film grain", "The game's film grain noise.",
     L"r.FilmGrain", {{"Game", 0, {}}, {"Off", 1, {{L"r.FilmGrain", L"0"}}}}},
    {"Post-processing", "Camera", "LensFlares", "Lens flares", "Ghost reflections of bright lights across the screen.",
     L"r.LensFlareQuality", {{"Game", 0, {}}, {"Off", 1, {{L"r.LensFlareQuality", L"0"}}}}},
    {"Post-processing", "Camera", "DepthOfField", "Depth of field",
     "Background blur in close-ups and cutscenes. (Your Engine.ini already turns it off.)",
     L"r.DepthOfFieldQuality", {{"Game", 0, {}}, {"Off", 1, {{L"r.DepthOfFieldQuality", L"0"}}}}},
    {"Post-processing", "Camera", "Fringe", "Chromatic aberration",
     "Colour fringes at the screen edges. (Your Engine.ini already turns it off.)",
     L"r.SceneColorFringeQuality", {{"Game", 0, {}}, {"Off", 1, {{L"r.SceneColorFringeQuality", L"0"}}}}},
};

bool g_enabled = true;  // F8

const Choice* Chosen(const Option& o) {
    for (const Choice& c : o.choices)
        if (c.id == o.value) return &c;
    return nullptr;
}

void LoadSettings() {
    for (Option& o : g_options) {
        reshade::get_config_value(nullptr, DT_PRODUCT, o.key, o.value);
        if (!Chosen(o)) o.value = 0;
    }
}

void SaveSettings() {
    for (const Option& o : g_options) reshade::set_config_value(nullptr, DT_PRODUCT, o.key, o.value);
}

// What we want in force now.
std::map<std::wstring, std::wstring> Wanted() {
    std::map<std::wstring, std::wstring> m;
    if (!g_enabled) return m;
    for (const Option& o : g_options)
        if (const Choice* c = Chosen(o))
            for (auto& [key, value] : c->cvars) m[key] = value;
    return m;
}

// ---------------------------------------------------------------- live settings

enum class Live { Idle, Searching, Ready, Failed };
std::atomic<Live> g_live{Live::Idle};
void* g_manager = nullptr;
int g_sinkFlag = -1;
std::string g_liveError;
ULONGLONG g_readyTick = 0;

std::map<std::wstring, std::wstring> g_originals;  // the game's value of each cvar we hold
std::map<std::wstring, std::wstring> g_applied;    // what we keep in force
std::set<std::wstring> g_rejected;                 // values the game didn't take (logged once)
bool g_dirty = true;                               // options changed: apply at the next present
int g_reapplied = 0;                               // values put back after the game changed them

struct VarInfo {
    void* var = nullptr;
    cvars::Type type = cvars::Type::Unknown;
};
std::map<std::wstring, VarInfo> g_vars;  // lookup cache (cvars live for the whole session)

VarInfo Lookup(const std::wstring& name) {
    auto it = g_vars.find(name);
    if (it != g_vars.end()) return it->second;
    VarInfo info;
    info.var = cvars::FindVariable(g_manager, name.c_str());
    if (info.var) info.type = cvars::GetType(info.var);
    if (info.var) g_vars[name] = info;  // misses aren't cached: plugin cvars can register late
    return info;
}

bool Read(const std::wstring& name, std::wstring& out) {
    VarInfo v = Lookup(name);
    wchar_t buf[64];
    if (!v.var || !cvars::GetText(v.var, v.type, buf, 64)) return false;
    out = buf;
    return true;
}

bool SameValue(cvars::Type t, const std::wstring& a, const std::wstring& b) {
    if (t == cvars::Type::Float) return fabs(_wtof(a.c_str()) - _wtof(b.c_str())) < 1e-4;
    return _wtoi(a.c_str()) == _wtoi(b.c_str());
}

void FindManager() {
    const uint8_t* base = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
    int votes = 0, matches = 0;
    void** singleton = cvars::FindConsoleManagerSingleton(base, &votes, &matches);
    void* manager = nullptr;
    if (singleton) cvars::SafeCopy(&manager, singleton, 8);
    Log("console manager: singleton %p (%d of %d matches), manager %p", singleton, votes, matches, manager);
    if (!manager || !cvars::HasModuleVtable(base, manager)) {
        g_liveError = "console manager not found";
        g_live = Live::Failed;
        return;
    }
    // Known variables check the layout: r.MaxAnisotropy is an int (1-16), r.ScreenPercentage a float.
    void* aniso = cvars::FindVariable(manager, L"r.MaxAnisotropy");
    void* screen = cvars::FindVariable(manager, L"r.ScreenPercentage");
    bool vt = aniso && screen && cvars::HasModuleVtable(base, aniso) && cvars::HasModuleVtable(base, screen);
    bool slots = vt && cvars::FindTypeSlots(aniso, screen);
    int32_t a = -1;
    float s = -1;
    bool reads = slots && cvars::GetInt(aniso, a) && cvars::GetFloat(screen, s);
    Log("layout check: r.MaxAnisotropy %p, r.ScreenPercentage %p, vtables %d, IsVariableBool at +0x%x, values %d / %g",
        aniso, screen, vt, cvars::g_isVariableBool, a, s);
    if (!reads || a < 0 || a > 16 || !(s >= 10 && s <= 400)) {
        g_liveError = "the game's settings layout didn't match (see the log)";
        g_live = Live::Failed;
        return;
    }
    g_sinkFlag = cvars::FindSinkFlagOffset(manager);
    Log("console variable sinks flag at +0x%x (now %d)", g_sinkFlag, cvars::ReadSinkFlag(manager, g_sinkFlag));
    g_manager = manager;
    g_readyTick = GetTickCount64();
    g_live = Live::Ready;
}

bool SetVar(const std::wstring& key, const std::wstring& value) {
    VarInfo v = Lookup(key);
    if (!v.var) {
        if (g_rejected.insert(key).second) Log("%ls not found", key.c_str());
        return false;
    }
    if (!cvars::Set(v.var, value.c_str())) return false;
    std::wstring now;
    if (Read(key, now) && !SameValue(v.type, now, value)) {
        if (g_rejected.insert(key).second) Log("%ls = %ls not taken (reads %ls)", key.c_str(), value.c_str(), now.c_str());
        return false;
    }
    g_rejected.erase(key);
    return true;
}

// Brings the game in line with Wanted(): our values in force, ones no longer wanted back to the
// game's.
void Apply() {
    std::map<std::wstring, std::wstring> want = Wanted();
    int set = 0, restored = 0, failed = 0;
    for (auto& [key, value] : want) {
        if (!g_applied.count(key)) {  // newly held: the game's value is what's there now
            std::wstring old;
            if (Read(key, old)) g_originals[key] = old;
        }
        if (SetVar(key, value)) ++set;
        else ++failed;
    }
    for (auto& [key, value] : g_applied) {
        if (want.count(key)) continue;
        auto it = g_originals.find(key);
        if (it != g_originals.end() && SetVar(key, it->second)) ++restored;
    }
    g_applied = std::move(want);
    if (g_sinkFlag >= 0) cvars::RequestSinks(g_manager, g_sinkFlag);
    Log("applied %d, restored %d, failed %d (tweaks %s)", set, restored, failed, g_enabled ? "on" : "off");
}

// Every second: any value the game changed (a menu change re-applying its settings) becomes the
// new game value, and ours goes back in.
void Hold() {
    static ULONGLONG last = 0;
    ULONGLONG now = GetTickCount64();
    if (now - last < 1000) return;
    last = now;
    static std::map<std::wstring, int> counts;  // per setting, to keep the log short
    int fixed = 0;
    for (auto& [key, value] : g_applied) {
        if (g_rejected.count(key)) continue;
        VarInfo v = Lookup(key);
        std::wstring cur;
        if (!Read(key, cur) || SameValue(v.type, cur, value)) continue;
        g_originals[key] = cur;
        if (SetVar(key, value)) ++fixed;
        int n = ++counts[key];
        if (n <= 3 || n == 10 || n % 100 == 0)
            Log("put back %ls = %ls (the game set %ls; %d times so far)", key.c_str(), value.c_str(), cur.c_str(), n);
    }
    if (fixed) {
        g_reapplied += fixed;
        if (g_sinkFlag >= 0) cvars::RequestSinks(g_manager, g_sinkFlag);
    }
}

// ---------------------------------------------------------------- probe
//
// Writes the game's value of every console variable under the prefixes below to
// ReunionTweaks-probe.txt: automatically 5 s and 60 s after connecting (title screen, then after
// the game has applied its menu), and from the Advanced tab at any time (in a scene). Names come
// from the game's read-only data (UTF-16 strings); nothing is enumerated.

const char* const kProbePrefixes[] = {
    "r.Lumen.", "r.RayTracing", "r.Raytracing", "r.Shadow.", "r.TSR.", "r.TemporalAA", "r.AntiAliasing",
    "r.ScreenPercentage", "r.SecondaryScreenPercentage", "r.DynamicRes.", "r.DynamicGlobalIlluminationMethod",
    "r.ReflectionMethod", "r.Streaming.", "r.MaxAnisotropy", "r.VT.", "r.HDR.", "r.Tonemapper", "r.Nanite.MaxPixelsPerEdge",
    "r.Nanite.ViewMeshLODBias", "r.HairStrands.", "r.SSS.", "r.SubsurfaceScattering", "r.SkinCache.", "r.MegaLights.",
    "r.VolumetricFog", "r.VolumetricCloud", "r.Bloom", "r.LensFlare", "r.DepthOfField", "r.DOF.", "r.MotionBlur",
    "r.SceneColorFringe", "r.FilmGrain", "r.Substrate", "r.PostProcessAAQuality", "r.SSR.", "r.AmbientOcclusion",
    "r.DistanceField", "r.ContactShadows", "r.CapsuleShadows", "r.ViewDistanceScale", "r.StaticMeshLODDistanceScale",
    "r.SkeletalMeshLODBias", "r.LandscapeLODBias", "r.Translucency", "r.SSGI", "r.LightMaxDrawDistanceScale",
    "r.MipMapLODBias", "r.Sharpen", "r.LUT.", "r.Color.", "r.EyeAdaptation", "r.DefaultFeature.",
    "foliage.", "grass.", "sg.", "D9.",
};

std::set<std::wstring> g_probeNames;  // collected once

void CollectProbeNames() {
    const uint8_t* base = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
    std::vector<std::wstring> prefixes;
    for (const char* p : kProbePrefixes) prefixes.emplace_back(p, p + strlen(p));
    for (const cvars::Section& s : cvars::Sections(base, cvars::SectionKind::ReadOnly)) {
        for (size_t i = 2; i + 8 < s.size; i += 2) {
            if (s.begin[i - 2] || s.begin[i - 1]) continue;  // must start right after a terminator
            const wchar_t* w = reinterpret_cast<const wchar_t*>(s.begin + i);
            wchar_t c0 = w[0];
            if (c0 != L'r' && c0 != L'f' && c0 != L'g' && c0 != L's' && c0 != L'D') continue;
            size_t len = 0;
            while (len < 120 && i + 2 * len + 2 < s.size && w[len] > 32 && w[len] < 127) ++len;
            if (len < 3 || w[len] != 0) continue;
            for (const std::wstring& p : prefixes)
                if (len >= p.size() && !wcsncmp(w, p.c_str(), p.size())) {
                    g_probeNames.emplace(w, len);
                    break;
                }
        }
    }
    Log("probe: %zu candidate names", g_probeNames.size());
}

std::string g_probeResult;

void WriteProbe(const char* label) {
    if (g_probeNames.empty()) CollectProbeNames();
    FILE* f = _wfopen((g_exeDir + L"ReunionTweaks-probe.txt").c_str(), L"a");
    if (!f) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(f, "\n==== %s (%04u-%02u-%02u %02u:%02u:%02u, %.0f s after start, tweaks %s, holding %zu)\n", label, t.wYear,
            t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, (GetTickCount64() - g_loadTick) / 1000.0,
            g_enabled ? "on" : "off", g_applied.size());
    int found = 0, unreadable = 0;
    for (const std::wstring& name : g_probeNames) {
        VarInfo v = Lookup(name);
        if (!v.var) continue;
        std::wstring value;
        if (Read(name, value)) {
            ++found;
            fprintf(f, "%ls (%s) = %ls\n", name.c_str(), cvars::TypeName(v.type), value.c_str());
        } else {
            ++unreadable;
            fprintf(f, "%ls (%s) = ?\n", name.c_str(), cvars::TypeName(v.type));
        }
    }
    fprintf(f, "---- %d values, %d unreadable (strings/commands skipped)\n", found, unreadable);
    fclose(f);
    Log("probe '%s': %d values written", label, found);
    g_probeResult = std::to_string(found) + " values written to ReunionTweaks-probe.txt";
}

void ProbeOnPresent() {
    static int done = 0;
    ULONGLONG since = GetTickCount64() - g_readyTick;
    if (done == 0 && since > 5000) WriteProbe("auto: 5 s after connecting"), done = 1;
    else if (done == 1 && since > 60000) WriteProbe("auto: 60 s after connecting"), done = 2;
}

// ---------------------------------------------------------------- presets
//
// A preset sets the quality options (option key -> choice id); every other quality option goes
// to Game. Options in no preset are left alone. The preset shown is derived from the options,
// "Custom" when none matches.

struct QualityPreset {
    const char* label;
    const char* cost;  // shown next to the label in the picker
    const char* tip;
    std::vector<std::pair<const char*, int>> values;
};

const QualityPreset kPresets[] = {
    {"Game", "", "The game's own settings: everything below at Game.", {}},
    // Costs measured 2026-10-25 with "Measure preset costs": RTX 5080, 4K at 65% resolution scale
    // (TSR), menu mostly Epic, one free-roam scene: Game 84.5 / Ultra+ 73.6 / Cinematic 35.8 fps.
    // Ultra+ is every option that measured nearly free (within noise) at its best choice, plus
    // the cheap reflection upgrades. The two expensive lighting choices are left out
    // (docs/FINDINGS.md, per-option costs).
    {"Ultra+", "about 15% slower",
     "Everything beyond the menu's Epic that costs little: more lighting probes, reflections\n"
     "on rougher surfaces and on glass, the sharpest and smoothest shadows, full-detail\n"
     "characters (background people too), full skin shading, high-quality hair, 8 GB\n"
     "textures, 16x filtering, far draw distance, finer geometry, Epic fog and the\n"
     "higher-precision upscaler. Leaves out detailed bounce light tracing and Cinematic\n"
     "probes (about +40% and +65% on their own).",
     {{"GiProbes", 16}, {"ReflRoughness", 6}, {"ReflGlass", 1}, {"ShadowRes", 2}, {"ShadowSoft", 16},
      {"ShadowDistance", 150}, {"CapsuleShadows", 1}, {"CharLod", 2}, {"Skin", 1}, {"Hair", 1},
      {"TexturePool", 8192}, {"Anisotropy", 16}, {"ViewDistance", 150}, {"NaniteDetail", 1}, {"FogCells", 8},
      {"TsrHistory", 1}}},
    {"Cinematic", "about 2.4x frame time",
     "Everything at its highest: Cinematic bounce light and soft shadows, sharpest shadows,\n"
     "full detail for background people too, high-quality hair, finer geometry, far draw\n"
     "distance and finest fog. For photo mode or a lot of GPU headroom.",
     {{"GiTracing", 1}, {"GiProbes", 8}, {"ReflRoughness", 8}, {"ReflGlass", 1}, {"ShadowRes", 2},
      {"ShadowSoft", 16}, {"ShadowDistance", 150}, {"CapsuleShadows", 1}, {"CharLod", 2}, {"Skin", 1},
      {"Hair", 1}, {"TexturePool", 8192}, {"Anisotropy", 16}, {"ViewDistance", 150}, {"NaniteDetail", 1},
      {"FogCells", 4}, {"TsrHistory", 1}}},
};
constexpr int kPresetCount = static_cast<int>(sizeof(kPresets) / sizeof(kPresets[0]));

Option* FindOption(const char* key) {
    for (Option& o : g_options)
        if (!strcmp(o.key, key)) return &o;
    return nullptr;
}

std::set<std::string> PresetKeys() {
    std::set<std::string> keys;
    for (const QualityPreset& p : kPresets)
        for (auto& [key, id] : p.values) keys.insert(key);
    return keys;
}

int PresetValue(const QualityPreset& p, const std::string& key) {
    for (auto& [k, id] : p.values)
        if (key == k) return id;
    return 0;
}

int CurrentPreset() {
    std::set<std::string> keys = PresetKeys();
    for (int i = 0; i < kPresetCount; ++i) {
        bool match = true;
        for (const std::string& key : keys) {
            const Option* o = FindOption(key.c_str());
            if (o && o->value != PresetValue(kPresets[i], key)) match = false;
        }
        if (match) return i;
    }
    return -1;
}

void ApplyPreset(int index) {
    for (const std::string& key : PresetKeys())
        if (Option* o = FindOption(key.c_str())) o->value = PresetValue(kPresets[index], key);
    g_dirty = true;
}

// ---------------------------------------------------------------- frame time

int64_t g_qpcFreq = 1;
std::vector<float> g_frameMs;  // recent frames, a ring of kFrameRing
size_t g_framePos = 0;
constexpr size_t kFrameRing = 240;

// The average, not the median: with frame generation the present intervals alternate.
float Mean(const std::vector<float>& v) {
    if (v.empty()) return 0;
    double sum = 0;
    for (float x : v) sum += x;
    return static_cast<float>(sum / v.size());
}

// A measurement runs stages one after another: each sets the quality options (key -> choice id,
// everything else in a preset at Game), settles, then averages the frame time. "Presets" runs
// Game and each preset; "Each option" runs Game, then every option a preset uses at each choice
// a preset picks, one at a time, then Game again (to show drift).
struct Stage {
    std::string label;
    std::vector<std::pair<std::string, int>> values;
};

struct Measure {
    bool running = false;
    bool perOption = false;
    ULONGLONG settleMs = 5000, sampleMs = 8000;
    std::vector<Stage> stages;
    size_t stage = 0;
    ULONGLONG stageStart = 0;
    bool sampling = false;
    std::vector<float> samples;
    std::vector<std::pair<std::string, int>> saved;
    std::vector<float> resultMs;
    bool done = false;
    std::string note;
    std::string summary;
} g_measure;

void ApplyStage(const Stage& st) {
    for (const std::string& key : PresetKeys())
        if (Option* o = FindOption(key.c_str())) o->value = 0;
    for (auto& [key, id] : st.values)
        if (Option* o = FindOption(key.c_str())) o->value = id;
    g_dirty = true;
}

const char* ChoiceName(const Option& o, int id) {
    for (const Choice& c : o.choices)
        if (c.id == id) return c.label;
    return "?";
}

void StartMeasure(bool perOption) {
    g_measure = {};
    g_measure.running = true;
    g_measure.perOption = perOption;
    if (perOption) g_measure.settleMs = 4000, g_measure.sampleMs = 6000;
    for (const Option& o : g_options) g_measure.saved.push_back({o.key, o.value});
    g_measure.stages.push_back({"Game", {}});
    if (perOption) {
        for (const Option& o : g_options) {
            std::set<int> ids;
            for (const QualityPreset& p : kPresets)
                if (int id = PresetValue(p, o.key)) ids.insert(id);
            for (int id : ids) g_measure.stages.push_back({std::string(o.key) + " " + ChoiceName(o, id), {{o.key, id}}});
        }
        g_measure.stages.push_back({"Game (again)", {}});
    } else {
        for (int i = 1; i < kPresetCount; ++i) {
            Stage st{kPresets[i].label, {}};
            for (auto& [key, id] : kPresets[i].values) st.values.push_back({key, id});
            g_measure.stages.push_back(st);
        }
    }
    g_measure.resultMs.assign(g_measure.stages.size(), 0);
    g_measure.stageStart = GetTickCount64();
    ApplyStage(g_measure.stages[0]);
    Log("measure: started (%s, %zu stages)", perOption ? "each option" : "presets", g_measure.stages.size());
}

void FinishMeasure(const char* why) {
    for (auto& [key, value] : g_measure.saved)
        if (Option* o = FindOption(key.c_str())) o->value = value;
    g_dirty = true;
    g_measure.running = false;
    g_measure.note = why;
    Log("measure: %s", why);
}

void SummarizeMeasure() {
    const std::vector<float>& r = g_measure.resultMs;
    const float base = g_measure.perOption ? (r.front() + r.back()) / 2 : r.front();
    char line[160];
    if (!g_measure.perOption) {
        std::string sum = "Game " + std::to_string(static_cast<int>(1000.0f / r[0] + 0.5f)) + " fps";
        std::string log = "measure: result";
        for (size_t i = 1; i < r.size(); ++i) {
            snprintf(line, sizeof(line), ", %s %.0f fps (%+.0f%% frame time)", g_measure.stages[i].label.c_str(),
                     1000.0f / r[i], (r[i] / base - 1) * 100);
            sum += line;
            snprintf(line, sizeof(line), ", %s %+.0f%% frame time", g_measure.stages[i].label.c_str(), (r[i] / base - 1) * 100);
            log += line;
        }
        g_measure.summary = sum;
        Log("%s", log.c_str());
        return;
    }
    // Each option, costliest first.
    std::vector<size_t> order;
    for (size_t i = 1; i + 1 < r.size(); ++i) order.push_back(i);
    std::sort(order.begin(), order.end(), [&](size_t x, size_t y) { return r[x] > r[y]; });
    Log("measure: each option vs Game %.2f ms (start %.2f, end %.2f), costliest first:", base, r.front(), r.back());
    std::string sum;
    for (size_t k = 0; k < order.size(); ++k) {
        size_t i = order[k];
        Log("measure:   %+6.1f%%  %+6.2f ms  %s", (r[i] / base - 1) * 100, r[i] - base, g_measure.stages[i].label.c_str());
        if (k < 4) {
            snprintf(line, sizeof(line), "%s%s %+.0f%%", k ? ", " : "Costliest: ", g_measure.stages[i].label.c_str(),
                     (r[i] / base - 1) * 100);
            sum += line;
        }
    }
    g_measure.summary = sum + " (all in ReunionTweaks.log)";
}

void MeasureOnFrame(float ms, bool focused) {
    if (!g_measure.running) return;
    if (!focused) return FinishMeasure("stopped: the game lost focus");
    if (!g_enabled) return FinishMeasure("stopped: tweaks switched off (F8)");
    ULONGLONG now = GetTickCount64();
    if (!g_measure.sampling) {
        if (now - g_measure.stageStart >= g_measure.settleMs) g_measure.sampling = true, g_measure.stageStart = now;
        return;
    }
    g_measure.samples.push_back(ms);
    if (now - g_measure.stageStart < g_measure.sampleMs) return;
    float avg = Mean(g_measure.samples);
    g_measure.resultMs[g_measure.stage] = avg;
    Log("measure: %s %.2f ms average (%.1f fps) over %zu frames", g_measure.stages[g_measure.stage].label.c_str(), avg,
        1000.0f / avg, g_measure.samples.size());
    g_measure.samples.clear();
    g_measure.sampling = false;
    g_measure.stageStart = now;
    if (++g_measure.stage < g_measure.stages.size()) {
        ApplyStage(g_measure.stages[g_measure.stage]);
        return;
    }
    g_measure.done = true;
    SummarizeMeasure();
    FinishMeasure("done");
}

void FrameOnPresent(bool focused) {
    static int64_t last = 0;
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    float ms = last ? static_cast<float>((t.QuadPart - last) * 1000.0 / g_qpcFreq) : 0;
    last = t.QuadPart;
    if (!ms || !focused || ms > 500) {
        if (g_measure.running && !focused) MeasureOnFrame(ms, false);
        return;
    }
    if (g_frameMs.size() < kFrameRing) g_frameMs.push_back(ms);
    else g_frameMs[g_framePos] = ms;
    g_framePos = (g_framePos + 1) % kFrameRing;
    MeasureOnFrame(ms, true);
}

// ---------------------------------------------------------------- ray tracing activity
//
// Counts acceleration structure builds and DispatchRays calls: the only sure sign that the GPU
// is doing hardware ray tracing (the Lumen cvars read 1 even when r.RayTracing is off). Logged
// every 10 s while the count changes, for the first few minutes.

std::atomic<uint64_t> g_asBuilds{0}, g_rayDispatches{0};
uint64_t g_asPerSec = 0, g_raysPerSec = 0;

bool OnBuildAs(command_list*, acceleration_structure_type, acceleration_structure_build_flags, uint32_t,
               const acceleration_structure_build_input*, resource, uint64_t, resource_view, resource_view,
               acceleration_structure_build_mode) {
    ++g_asBuilds;
    return false;
}

bool OnDispatchRays(command_list*, resource, uint64_t, uint64_t, resource, uint64_t, uint64_t, uint64_t, resource,
                    uint64_t, uint64_t, uint64_t, resource, uint64_t, uint64_t, uint64_t, uint32_t, uint32_t, uint32_t) {
    ++g_rayDispatches;
    return false;
}

void RayTracingOnPresent() {
    static ULONGLONG last = 0, lastLog = 0;
    static uint64_t prevAs = 0, prevRays = 0, loggedAs = ~0ull;
    ULONGLONG now = GetTickCount64();
    if (now - last < 1000) return;
    uint64_t as = g_asBuilds.load(), rays = g_rayDispatches.load();
    g_asPerSec = as - prevAs, g_raysPerSec = rays - prevRays;
    prevAs = as, prevRays = rays, last = now;
    if (now - lastLog >= 10000 && now - g_loadTick < 600000 && as != loggedAs) {
        lastLog = now, loggedAs = as;
        Log("ray tracing: %llu acceleration structure builds, %llu DispatchRays so far (%llu / %llu per second)", as,
            rays, g_asPerSec, g_raysPerSec);
    }
}

// ---------------------------------------------------------------- present

color_space g_colorSpace = color_space::unknown;
bool g_keyWasDown = false;

void LiveOnPresent() {
    Live state = g_live.load();
    if (state == Live::Idle) {
        g_live = Live::Searching;
        std::thread(FindManager).detach();
        return;
    }
    if (state != Live::Ready) return;
    static bool first = true;
    if (first) {  // the game's values of everything the options touch, for reference
        first = false;
        std::set<std::wstring> keys;
        for (const Option& o : g_options)
            for (const Choice& c : o.choices)
                for (auto& [key, value] : c.cvars) keys.insert(key);
        for (const std::wstring& key : keys) {
            std::wstring v;
            VarInfo info = Lookup(key);
            Log("game value: %ls (%s) = %ls", key.c_str(), cvars::TypeName(info.type), Read(key, v) ? v.c_str() : L"(not found)");
        }
    }
    if (g_dirty) {
        g_dirty = false;
        Apply();
    }
    Hold();
    ProbeOnPresent();
}

void OnPresent(command_queue*, swapchain* sc, const rect*, const rect*, uint32_t, const rect*) {
    if (sc->get_color_space() != g_colorSpace) {
        g_colorSpace = sc->get_color_space();
        Log("swap chain colour space %s", g_colorSpace == color_space::hdr10_pq ? "HDR10 PQ"
                                          : g_colorSpace == color_space::srgb  ? "sRGB"
                                          : g_colorSpace == color_space::extended_srgb_linear ? "scRGB"
                                                                               : "other");
    }
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    bool down = pid == GetCurrentProcessId() && (GetAsyncKeyState(kToggleHotkey) & 0x8000);
    if (down && !g_keyWasDown) {
        g_enabled = !g_enabled;
        g_dirty = true;
        Log("tweaks %s (F8)", g_enabled ? "on" : "off");
    }
    g_keyWasDown = down;
    FrameOnPresent(pid == GetCurrentProcessId());
    RayTracingOnPresent();
    LiveOnPresent();
}

// ---------------------------------------------------------------- overlay

constexpr ImVec4 kGood(0.45f, 0.85f, 0.45f, 1.0f);
constexpr ImVec4 kWarn(1.0f, 0.8f, 0.3f, 1.0f);
constexpr ImVec4 kBad(1.0f, 0.45f, 0.35f, 1.0f);
constexpr ImVec4 kAccent(0.4f, 0.75f, 1.0f, 1.0f);

float ControlWidth() { return ImGui::GetFontSize() * 14.0f; }

void Help(const char* text) {
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", text);
}

void ChangedMark(bool changed) {
    if (changed)
        ImGui::TextColored(kAccent, "*");
    else
        ImGui::TextUnformatted(" ");
    ImGui::SameLine();
}

std::string GameValue(const wchar_t* name) {
    if (!name || g_live.load() != Live::Ready) return {};
    auto it = g_originals.find(name);
    if (it != g_originals.end() && g_applied.count(name)) return Narrow(it->second);
    std::wstring cur;
    return Read(name, cur) ? Narrow(cur) : std::string();
}

std::string ChoiceLabel(const Option& o, const Choice& c) {
    if (c.id != 0) return c.label;
    std::string v = GameValue(o.shown);
    return v.empty() ? "Game" : "Game (" + v + ")";
}

bool DrawOption(Option& o) {
    bool changed = false;
    const Choice* cur = Chosen(o);
    ChangedMark(o.value != 0);
    ImGui::SetNextItemWidth(ControlWidth());
    std::string preview = cur ? ChoiceLabel(o, *cur) : "?";
    if (ImGui::BeginCombo(o.label, preview.c_str())) {
        for (const Choice& c : o.choices) {
            std::string label = ChoiceLabel(o, c);
            if (ImGui::Selectable(label.c_str(), c.id == o.value) && c.id != o.value) {
                o.value = c.id;
                changed = true;
                Log("option %s: %s", o.key, c.label);
            }
        }
        ImGui::EndCombo();
    }
    Help(o.tip);
    return changed;
}

bool DrawTab(const char* tab) {
    bool changed = false;
    const char* section = nullptr;
    for (Option& o : g_options) {
        if (strcmp(o.tab, tab)) continue;
        if (!section || strcmp(section, o.section)) {
            section = o.section;
            ImGui::SeparatorText(section);
        }
        changed |= DrawOption(o);
    }
    ImGui::Spacing();
    bool any = false;
    for (const Option& o : g_options) any |= !strcmp(o.tab, tab) && o.value != 0;
    ImGui::BeginDisabled(!any);
    if (ImGui::Button("Set this tab to Game")) {
        for (Option& o : g_options)
            if (!strcmp(o.tab, tab)) o.value = 0;
        changed = true;
    }
    ImGui::EndDisabled();
    return changed;
}

char g_cvarName[128] = "r.Lumen.HardwareRayTracing";
char g_cvarValue[64] = "";
std::string g_cvarResult;

void DrawAdvanced() {
    const Live live = g_live.load();
    ImGui::SeparatorText("Status");
    ImGui::TextDisabled("Game settings: %s", live == Live::Ready    ? "connected"
                                             : live == Live::Failed ? g_liveError.c_str()
                                                                    : "connecting...");
    ImGui::TextDisabled("Holding %zu settings; put back %d times after the game changed them", g_applied.size(),
                        g_reapplied);
    if (!g_rejected.empty()) {
        std::string list;
        for (const std::wstring& k : g_rejected) list += (list.empty() ? "" : ", ") + Narrow(k);
        ImGui::TextColored(kWarn, "Not taken: %s", list.c_str());
    }
    ImGui::TextDisabled("Hardware ray tracing: %s (%llu structure builds, %llu ray dispatches per second)",
                        g_asPerSec || g_raysPerSec ? "active" : "not in use", g_asPerSec, g_raysPerSec);
    ImGui::TextDisabled("Log: ReunionTweaks.log next to the game's exe");
    if (live != Live::Ready) return;

    ImGui::SeparatorText("Probe");
    if (ImGui::Button("Write all settings to file")) WriteProbe("manual");
    Help("Writes the game's value of every lighting, shadow, upscaling, detail and\n"
         "post-processing setting to ReunionTweaks-probe.txt next to the game's exe.\n"
         "Do it in a scene (not a menu) for the values the game really plays with.");
    if (!g_probeResult.empty()) ImGui::TextDisabled("%s", g_probeResult.c_str());

    ImGui::SeparatorText("Console variable");
    ImGui::SetNextItemWidth(ControlWidth() * 1.6f);
    ImGui::InputText("Name", g_cvarName, sizeof(g_cvarName));
    std::wstring name(g_cvarName, g_cvarName + strlen(g_cvarName));
    std::wstring value;
    VarInfo v = Lookup(name);
    if (!v.var)
        ImGui::TextDisabled("Not found");
    else
        ImGui::TextDisabled("%s, now %s%s", cvars::TypeName(v.type), Read(name, value) ? Narrow(value).c_str() : "?",
                            g_applied.count(name) ? " (held by an option)" : "");
    ImGui::SetNextItemWidth(ControlWidth() * 0.6f);
    ImGui::InputText("##value", g_cvarValue, sizeof(g_cvarValue));
    ImGui::SameLine();
    ImGui::BeginDisabled(!v.var || !g_cvarValue[0]);
    if (ImGui::Button("Set once")) {
        std::wstring val(g_cvarValue, g_cvarValue + strlen(g_cvarValue));
        bool ok = SetVar(name, val);
        Log("console: %ls = %ls %s", name.c_str(), val.c_str(), ok ? "set" : "NOT taken");
        g_cvarResult = ok ? "Set (until the game or an option changes it)" : "Not taken";
    }
    ImGui::EndDisabled();
    Help("For experiments: sets the value now. It isn't saved, and the game's menu or an\n"
         "option above can change it again.");
    if (!g_cvarResult.empty()) ImGui::TextDisabled("%s", g_cvarResult.c_str());
}

void DrawOverlay(effect_runtime*) {
    ImGui::TextDisabled(DT_PRODUCT " " DT_VERSION);
    ImGui::SameLine();
    const Live live = g_live.load();
    if (live == Live::Failed)
        ImGui::TextColored(kBad, "  Can't reach the game's settings (Advanced tab)");
    else if (live != Live::Ready)
        ImGui::TextDisabled("  Connecting to the game's settings...");
    else if (!g_enabled)
        ImGui::TextColored(kWarn, "  Tweaks off (F8)");
    else
        ImGui::TextColored(kGood, "  Tweaks on (F8 compares)");
    ImGui::TextDisabled("* = changed from the game's value. Hover (?) for details. Changes apply at once.");

    bool changed = false;
    const bool measuring = g_measure.running;

    ImGui::Spacing();
    if (kPresetCount > 1) {
        ImGui::BeginDisabled(measuring || live != Live::Ready);
        const int current = CurrentPreset();
        ChangedMark(current != 0);
        ImGui::SetNextItemWidth(ControlWidth());
        if (ImGui::BeginCombo("Quality preset", current >= 0 ? kPresets[current].label : "Custom")) {
            for (int i = 0; i < kPresetCount; ++i) {
                std::string item = kPresets[i].label;
                if (*kPresets[i].cost) item += std::string(" (") + kPresets[i].cost + ")";
                if (ImGui::Selectable(item.c_str(), i == current) && i != current) {
                    ApplyPreset(i);
                    changed = true;
                    Log("preset: %s", kPresets[i].label);
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s%s", kPresets[i].tip,
                                      *kPresets[i].cost ? "\n\nMeasured on an RTX 5080 at 4K (65% resolution scale, menu\n"
                                                          "mostly Epic) in one free-roam scene. Yours may differ."
                                                        : "");
            }
            ImGui::EndCombo();
        }
        ImGui::EndDisabled();
        Help("Sets the quality options together. Measure preset costs checks them on your PC.\n"
             "Changing an option afterwards shows Custom.");
        ImGui::SameLine();
    }
    float ms = Mean(g_frameMs);
    if (ms > 0) ImGui::TextDisabled("   %.1f ms (%.0f fps)", ms, 1000.0f / ms);

    if (kPresetCount > 1) {
        if (measuring) {
            ImGui::TextColored(kAccent, "Measuring %s (%zu of %zu)... keep the camera still.",
                               g_measure.stages[g_measure.stage].label.c_str(), g_measure.stage + 1,
                               g_measure.stages.size());
            ImGui::SameLine();
            if (ImGui::SmallButton("Stop")) FinishMeasure("stopped");
        } else {
            ImGui::BeginDisabled(live != Live::Ready || !g_enabled);
            if (ImGui::SmallButton("Measure preset costs")) StartMeasure(false);
            ImGui::EndDisabled();
            Help("Runs each preset for 13 s (5 s to settle, 8 s measured) and shows how much longer\n"
                 "each frame takes than with Game. Stand still in a typical scene and leave the mouse\n"
                 "alone. Your options come back afterwards.");
            ImGui::SameLine();
            ImGui::BeginDisabled(live != Live::Ready || !g_enabled);
            if (ImGui::SmallButton("Measure each option")) StartMeasure(true);
            ImGui::EndDisabled();
            Help("Runs every option the presets use on its own (10 s each, about 5 minutes in all)\n"
                 "and writes how much each one costs to ReunionTweaks.log, costliest first.");
            if (g_measure.done) {
                ImGui::TextDisabled("%s", g_measure.summary.c_str());
            } else if (!g_measure.note.empty()) {
                ImGui::TextColored(kWarn, "Measurement %s.", g_measure.note.c_str());
            }
        }
    }
    ImGui::Spacing();

    ImGui::BeginDisabled(measuring);
    if (ImGui::BeginTabBar("tabs")) {
        std::vector<const char*> tabs;  // in the order the options list them
        for (const Option& o : g_options)
            if (std::none_of(tabs.begin(), tabs.end(), [&](const char* t) { return !strcmp(t, o.tab); }))
                tabs.push_back(o.tab);
        for (const char* tab : tabs)
            if (ImGui::BeginTabItem(tab)) {
                changed |= DrawTab(tab);
                ImGui::EndTabItem();
            }
        if (ImGui::BeginTabItem("Advanced")) {
            DrawAdvanced();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::EndDisabled();
    if (changed) {
        SaveSettings();
        g_dirty = true;
    }
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    switch (reason) {
        case DLL_PROCESS_ATTACH:
            if (!reshade::register_addon(module)) return FALSE;
            g_loadTick = GetTickCount64();
            {
                LARGE_INTEGER f;
                QueryPerformanceFrequency(&f);
                g_qpcFreq = f.QuadPart;
            }
            OpenLog();
            LoadSettings();
            reshade::register_event<reshade::addon_event::present>(&OnPresent);
            reshade::register_event<reshade::addon_event::build_acceleration_structure>(&OnBuildAs);
            reshade::register_event<reshade::addon_event::dispatch_rays>(&OnDispatchRays);
            reshade::register_overlay(DT_PRODUCT, &DrawOverlay);
            break;
        case DLL_PROCESS_DETACH:
            reshade::unregister_addon(module);
            if (g_log) fclose(g_log);
            break;
    }
    return TRUE;
}
