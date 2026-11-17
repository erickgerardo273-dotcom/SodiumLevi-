// SodiumLevi v0.2 - mod de optimización para LeviLauncher (Minecraft Bedrock, Android arm64).
// Autor: erickzgcs
//
// Todo lo de esta versión se engancha a libEGL.so (eglSwapBuffers / eglCreateWindowSurface) y a
// APIs públicas de Android. NO usa firmas ni offsets de Minecraft, así que no depende de la versión
// del juego. Las optimizaciones que tocan el render interno (culling, partículas, etc.) van en v0.3.

#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#include <EGL/egl.h>
#include <android/log.h>
#include <android/native_window.h>
#include <dlfcn.h>
#include <errno.h>
#include <pl/Mod.hpp>
#include <pl/ModMenu.hpp>
#include <pl/memory/Hook.hpp>
#include <sched.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/system_properties.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr const char* kTag = "SodiumLevi";
constexpr const char* kModuleId = "sodiumlevi.main";

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, kTag, __VA_ARGS__)

// ------------------------------------------------------------------------------------------
// Modos (presets). Cada uno solo cambia los ajustes de abajo; el modo "Personalizado" no toca nada.
// ------------------------------------------------------------------------------------------
enum Mode { Custom = 0, Ultra, Alto, Medio, Equilibrado, Bajo, Vibrant, ModeCount };

struct Preset {
    int fpsCap;     // 0 = sin límite
    int boost;      // prioridad del hilo de render (0 = no tocar, 15 = máxima)
    bool hint;      // Performance Hint
    int cores;      // 0 = no tocar, 1 = núcleos rápidos, 2 = solo el/los más rápidos
    bool thermal;   // límite automático por temperatura
    bool battery;   // límite automático por batería baja
};

constexpr Preset kPresets[ModeCount] = {
    /* Custom      */ {0, 0, false, 0, false, false},
    /* Ultra       */ {0, 15, true, 2, true, false},
    /* Alto        */ {0, 10, true, 1, true, false},
    /* Medio       */ {0, 6, true, 0, true, false},
    /* Equilibrado */ {0, 4, true, 0, true, true},
    /* Bajo        */ {40, 0, true, 0, true, true},
    /* Vibrant     */ {0, 8, true, 1, true, false},
};

const char* kModeValues[ModeCount] = {"custom", "ultra", "alto", "medio", "equilibrado", "bajo", "vibrant"};

// ------------------------------------------------------------------------------------------
// Ajustes (los lee el hilo de render, los escribe el menú -> atómicos)
// ------------------------------------------------------------------------------------------
struct Settings {
    std::atomic<bool> active{true};
    std::atomic<int> mode{Equilibrado};
    std::atomic<int> fpsCap{0};
    std::atomic<int> boost{4};
    std::atomic<bool> hint{true};
    std::atomic<int> hintFps{0};
    std::atomic<int> cores{0};
    std::atomic<bool> thermal{true};
    std::atomic<bool> battery{false};
    std::atomic<int> batteryPct{20};
    std::atomic<int> resolution{0};  // 0 = nativa, si no, lado corto en px (360..1080)
};

Settings S;
std::atomic<uint32_t> gVersion{1};
std::filesystem::path gConfigFile;
std::mutex gSaveMutex;
std::atomic<bool> gPushing{false};

int modeFromValue(std::string_view v) {
    for (int i = 0; i < ModeCount; ++i)
        if (v == kModeValues[i]) return i;
    return Custom;
}

bool parseBool(std::string_view v) { return v == "true" || v == "1" || v == "on"; }
int parseInt(std::string_view v, int fallback) {
    std::string s(v);
    char* end = nullptr;
    long x = std::strtol(s.c_str(), &end, 10);
    return (end && end != s.c_str()) ? static_cast<int>(x) : fallback;
}

std::string resolutionValue(int r) { return r <= 0 ? "native" : std::to_string(r); }
int resolutionFromValue(std::string_view v) { return v == "native" ? 0 : std::clamp(parseInt(v, 0), 0, 4320); }

void applyPreset(int m) {
    if (m <= Custom || m >= ModeCount) return;
    const Preset& p = kPresets[m];
    S.fpsCap = p.fpsCap;
    S.boost = p.boost;
    S.hint = p.hint;
    S.cores = p.cores;
    S.thermal = p.thermal;
    S.battery = p.battery;
}

void saveConfig() {
    std::lock_guard<std::mutex> lock(gSaveMutex);
    if (gConfigFile.empty()) return;
    nlohmann::json j = {
        {"mode", kModeValues[std::clamp(S.mode.load(), 0, ModeCount - 1)]},
        {"fpsCap", S.fpsCap.load()},
        {"boost", S.boost.load()},
        {"performanceHint", S.hint.load()},
        {"hintFps", S.hintFps.load()},
        {"cores", S.cores.load()},
        {"thermalGuard", S.thermal.load()},
        {"batteryGuard", S.battery.load()},
        {"batteryPct", S.batteryPct.load()},
        {"resolution", resolutionValue(S.resolution.load())},
    };
    if (std::ofstream o{gConfigFile}) o << j.dump(2) << "\n";
}

void loadConfig(const std::filesystem::path& dir) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    gConfigFile = dir / "sodiumlevi.json";
    nlohmann::json j = nlohmann::json::object();
    if (std::ifstream in{gConfigFile}) {
        j = nlohmann::json::parse(in, nullptr, false);
        if (!j.is_object()) j = nlohmann::json::object();
    }
    S.mode = modeFromValue(j.value("mode", std::string("equilibrado")));
    if (S.mode.load() != Custom) applyPreset(S.mode.load());
    if (S.mode.load() == Custom || j.contains("fpsCap")) {
        S.fpsCap = std::clamp(j.value("fpsCap", S.fpsCap.load()), 0, 1000);
        S.boost = std::clamp(j.value("boost", S.boost.load()), 0, 15);
        S.hint = j.value("performanceHint", S.hint.load());
        S.cores = std::clamp(j.value("cores", S.cores.load()), 0, 2);
        S.thermal = j.value("thermalGuard", S.thermal.load());
        S.battery = j.value("batteryGuard", S.battery.load());
    }
    S.hintFps = std::clamp(j.value("hintFps", 0), 0, 240);
    S.batteryPct = std::clamp(j.value("batteryPct", 20), 5, 50);
    S.resolution = resolutionFromValue(j.value("resolution", std::string("native")));
    saveConfig();
}

// ------------------------------------------------------------------------------------------
// Utilidades de sistema
// ------------------------------------------------------------------------------------------
int64_t nowNs() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return int64_t(ts.tv_sec) * 1'000'000'000LL + ts.tv_nsec;
}

pid_t currentTid() { return static_cast<pid_t>(syscall(SYS_gettid)); }

std::string readLine(const std::string& path) {
    std::ifstream f(path);
    std::string s;
    if (f) std::getline(f, s);
    return s;
}

std::string prop(const char* name) {
    char buf[PROP_VALUE_MAX] = {0};
    __system_property_get(name, buf);
    return buf;
}

// Topología de CPU: se lee la frecuencia máxima de cada núcleo (sirve igual en Snapdragon,
// MediaTek/Dimensity, Exynos, Tensor...). No se adivina nada por la marca del chip.
struct CpuInfo {
    int count = 0;
    std::vector<long> maxKhz;
    long top = 0;
};
CpuInfo gCpu;
std::string gSocName;

void detectCpu() {
    long n = sysconf(_SC_NPROCESSORS_CONF);
    gCpu.count = static_cast<int>(std::clamp<long>(n, 1, 64));
    gCpu.maxKhz.assign(gCpu.count, 0);
    for (int i = 0; i < gCpu.count; ++i) {
        std::string v = readLine("/sys/devices/system/cpu/cpu" + std::to_string(i) + "/cpufreq/cpuinfo_max_freq");
        gCpu.maxKhz[i] = v.empty() ? 0 : std::strtol(v.c_str(), nullptr, 10);
        gCpu.top = std::max(gCpu.top, gCpu.maxKhz[i]);
    }
    std::string maker = prop("ro.soc.manufacturer");
    std::string model = prop("ro.soc.model");
    std::string platform = prop("ro.board.platform");
    gSocName = maker;
    if (!model.empty()) gSocName += (gSocName.empty() ? "" : " ") + model;
    if (gSocName.empty()) gSocName = platform.empty() ? prop("ro.hardware") : platform;
    if (gSocName.empty()) gSocName = "desconocido";
    LOGI("Chip: %s | núcleos: %d | frecuencia máx: %ld kHz", gSocName.c_str(), gCpu.count, gCpu.top);
}

bool gAffinityTouched = false;

void applyAffinity(pid_t tid, int mode) {
    if (gCpu.count <= 0) return;
    if (mode == 0 && !gAffinityTouched) return;
    cpu_set_t set;
    CPU_ZERO(&set);
    if (mode == 0 || gCpu.top <= 0) {
        for (int i = 0; i < gCpu.count; ++i) CPU_SET(i, &set);
    } else {
        const long threshold = (mode == 2) ? gCpu.top : gCpu.top * 75 / 100;
        for (int i = 0; i < gCpu.count; ++i)
            if (gCpu.maxKhz[i] >= threshold) CPU_SET(i, &set);
    }
    if (sched_setaffinity(tid, sizeof(set), &set) == 0) {
        gAffinityTouched = (mode != 0);
    } else {
        LOGI("sched_setaffinity no permitido (errno=%d)", errno);
    }
}

// ---- APerformanceHint (Android 13+) y AThermal (Android 11+), resueltos con dlsym ----
struct HintApi {
    using GetManager = void* (*)();
    using CreateSession = void* (*)(void*, const int32_t*, size_t, int64_t);
    using UpdateTarget = int (*)(void*, int64_t);
    using ReportActual = int (*)(void*, int64_t);
    using CloseSession = void (*)(void*);
    GetManager getManager = nullptr;
    CreateSession createSession = nullptr;
    UpdateTarget updateTarget = nullptr;
    ReportActual reportActual = nullptr;
    CloseSession closeSession = nullptr;
    bool ok() const { return getManager && createSession && updateTarget && reportActual && closeSession; }
};
HintApi gHint;

struct ThermalApi {
    using Acquire = void* (*)();
    using GetStatus = int (*)(void*);
    Acquire acquire = nullptr;
    GetStatus getStatus = nullptr;
    void* manager = nullptr;
};
ThermalApi gThermal;

void loadSystemApis() {
    void* lib = dlopen("libandroid.so", RTLD_NOW);
    if (!lib) return;
    gHint.getManager = reinterpret_cast<HintApi::GetManager>(dlsym(lib, "APerformanceHint_getManager"));
    gHint.createSession = reinterpret_cast<HintApi::CreateSession>(dlsym(lib, "APerformanceHint_createSession"));
    gHint.updateTarget = reinterpret_cast<HintApi::UpdateTarget>(dlsym(lib, "APerformanceHint_updateTargetWorkDuration"));
    gHint.reportActual = reinterpret_cast<HintApi::ReportActual>(dlsym(lib, "APerformanceHint_reportActualWorkDuration"));
    gHint.closeSession = reinterpret_cast<HintApi::CloseSession>(dlsym(lib, "APerformanceHint_closeSession"));
    gThermal.acquire = reinterpret_cast<ThermalApi::Acquire>(dlsym(lib, "AThermal_acquireManager"));
    gThermal.getStatus = reinterpret_cast<ThermalApi::GetStatus>(dlsym(lib, "AThermal_getCurrentThermalStatus"));
}

int thermalStatus() {
    if (!gThermal.acquire || !gThermal.getStatus) return 0;
    if (!gThermal.manager) gThermal.manager = gThermal.acquire();
    if (!gThermal.manager) return 0;
    int s = gThermal.getStatus(gThermal.manager);
    return s < 0 ? 0 : s;
}

// ------------------------------------------------------------------------------------------
// Estado del hilo de render (solo lo toca el hilo que llama a eglSwapBuffers)
// ------------------------------------------------------------------------------------------
std::atomic<pid_t> gRenderTid{0};
uint32_t gAppliedVersion = 0;
void* gSession = nullptr;
int64_t gLastReturnNs = 0;
int64_t gTargetNs = 16'666'666;
int gHintFrames = 0;
int64_t gLastGuardNs = 0;
int gGuardCap = 0;

void applyRenderThreadSettings(pid_t tid) {
    const int boost = std::clamp(S.boost.load(), 0, 15);
    if (boost > 0) {
        if (setpriority(PRIO_PROCESS, static_cast<id_t>(tid), -boost) != 0)
            LOGI("setpriority(-%d) no permitido (errno=%d)", boost, errno);
    } else {
        setpriority(PRIO_PROCESS, static_cast<id_t>(tid), 0);
    }

    applyAffinity(tid, S.cores.load());

    if (S.hint.load()) {
        if (!gSession && gHint.ok()) {
            if (void* manager = gHint.getManager()) {
                const int32_t tids[1] = {static_cast<int32_t>(tid)};
                gSession = gHint.createSession(manager, tids, 1, gTargetNs);
                LOGI("PerformanceHint %s", gSession ? "activo" : "no disponible");
            }
        }
        if (gSession && S.hintFps.load() > 0) {
            gTargetNs = 1'000'000'000LL / S.hintFps.load();
            gHint.updateTarget(gSession, gTargetNs);
        }
    } else if (gSession) {
        gHint.closeSession(gSession);
        gSession = nullptr;
    }
}

void updateGuards(int64_t now) {
    if (now - gLastGuardNs < 2'000'000'000LL) return;
    gLastGuardNs = now;
    int cap = 0;
    if (S.thermal.load()) {
        const int st = thermalStatus();  // 2 = moderado, 3+ = severo
        if (st >= 3) cap = 30;
        else if (st == 2) cap = 45;
    }
    if (S.battery.load()) {
        const std::string pct = readLine("/sys/class/power_supply/battery/capacity");
        const std::string st = readLine("/sys/class/power_supply/battery/status");
        if (!pct.empty() && st != "Charging" && st != "Full") {
            if (std::atoi(pct.c_str()) <= S.batteryPct.load()) cap = cap ? std::min(cap, 30) : 30;
        }
    }
    gGuardCap = cap;
}

void sleepUntil(int64_t deadlineNs) {
    timespec ts{};
    ts.tv_sec = deadlineNs / 1'000'000'000LL;
    ts.tv_nsec = deadlineNs % 1'000'000'000LL;
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr) == EINTR) {
    }
}

// ------------------------------------------------------------------------------------------
// Hooks de EGL
// ------------------------------------------------------------------------------------------
using EglSwapFn = EGLBoolean (*)(EGLDisplay, EGLSurface);
using EglCreateWindowSurfaceFn = EGLSurface (*)(EGLDisplay, EGLConfig, EGLNativeWindowType, const EGLint*);

EglSwapFn gOriginalSwap = nullptr;
EglCreateWindowSurfaceFn gOriginalCreate = nullptr;
void* gSwapTarget = nullptr;
void* gCreateTarget = nullptr;
bool gSwapHooked = false;
bool gCreateHooked = false;

EGLBoolean swapDetour(EGLDisplay display, EGLSurface surface) {
    if (!S.active.load()) return gOriginalSwap ? gOriginalSwap(display, surface) : EGL_FALSE;

    const pid_t tid = currentTid();
    pid_t expected = 0;
    gRenderTid.compare_exchange_strong(expected, tid);
    const bool isRender = (gRenderTid.load() == tid);

    const int64_t enterNs = nowNs();
    int64_t workNs = 0;
    if (isRender) {
        const uint32_t v = gVersion.load();
        if (v != gAppliedVersion) {
            gAppliedVersion = v;
            applyRenderThreadSettings(tid);
        }
        updateGuards(enterNs);
        workNs = gLastReturnNs ? (enterNs - gLastReturnNs) : 0;
    }

    EGLBoolean result = gOriginalSwap ? gOriginalSwap(display, surface) : EGL_FALSE;
    if (!isRender) return result;

    int64_t afterNs = nowNs();

    const int userCap = S.fpsCap.load();
    int cap = userCap;
    if (gGuardCap > 0) cap = cap > 0 ? std::min(cap, gGuardCap) : gGuardCap;
    if (cap > 0) {
        const int64_t deadline = enterNs + 1'000'000'000LL / cap;
        if (afterNs < deadline) {
            sleepUntil(deadline);
            afterNs = nowNs();
        }
    }

    if (gSession && workNs > 0) {
        if (S.hintFps.load() == 0 && gLastReturnNs && ++gHintFrames % 60 == 0) {
            const int64_t period = std::clamp<int64_t>(afterNs - gLastReturnNs, 4'000'000, 33'333'333);
            if (period != gTargetNs) {
                gTargetNs = period;
                gHint.updateTarget(gSession, gTargetNs);
            }
        }
        gHint.reportActual(gSession, workNs);
    }

    gLastReturnNs = afterNs;
    return result;
}

// Resolución "estirada": se fija el tamaño del búfer de dibujo ANTES de crear la superficie y
// Android lo escala a la pantalla. EXPERIMENTAL: aplica al abrir el juego (reiniciar Minecraft).
EGLSurface createSurfaceDetour(EGLDisplay display, EGLConfig config, EGLNativeWindowType window, const EGLint* attribs) {
    const int shortSide = S.resolution.load();
    if (S.active.load() && shortSide > 0 && window) {
        const int w0 = ANativeWindow_getWidth(window);
        const int h0 = ANativeWindow_getHeight(window);
        const int short0 = std::min(w0, h0);
        if (w0 > 0 && h0 > 0 && shortSide < short0) {
            const double scale = double(shortSide) / double(short0);
            int w = static_cast<int>(std::lround(w0 * scale)) & ~1;
            int h = static_cast<int>(std::lround(h0 * scale)) & ~1;
            const int rc = ANativeWindow_setBuffersGeometry(window, w, h, 0);
            LOGI("Resolución %dx%d -> %dx%d (rc=%d)", w0, h0, w, h, rc);
        }
    }
    return gOriginalCreate ? gOriginalCreate(display, config, window, attribs) : EGL_NO_SURFACE;
}

bool installHooks() {
    void* egl = dlopen("libEGL.so", RTLD_NOW | RTLD_NOLOAD);
    if (!egl) egl = dlopen("libEGL.so", RTLD_NOW);
    if (!egl) return false;

    if (!gSwapHooked) {
        gSwapTarget = dlsym(egl, "eglSwapBuffers");
        if (!gSwapTarget) return false;
        if (pl::memory::hook(gSwapTarget, reinterpret_cast<void*>(swapDetour),
                             reinterpret_cast<void**>(&gOriginalSwap)) != 0)
            return false;
        gSwapHooked = true;
    }
    if (!gCreateHooked) {
        gCreateTarget = dlsym(egl, "eglCreateWindowSurface");
        if (gCreateTarget &&
            pl::memory::hook(gCreateTarget, reinterpret_cast<void*>(createSurfaceDetour),
                             reinterpret_cast<void**>(&gOriginalCreate)) == 0) {
            gCreateHooked = true;
        } else {
            LOGI("No se pudo enganchar eglCreateWindowSurface: la resolución no estará disponible");
        }
    }
    return true;
}

void removeHooks() {
    if (gSwapHooked && gSwapTarget) pl::memory::unhook(gSwapTarget, reinterpret_cast<void*>(swapDetour));
    if (gCreateHooked && gCreateTarget) pl::memory::unhook(gCreateTarget, reinterpret_cast<void*>(createSurfaceDetour));
    gSwapHooked = gCreateHooked = false;
    if (gSession && gHint.closeSession) gHint.closeSession(gSession);
    gSession = nullptr;
    gRenderTid = 0;
    gLastReturnNs = 0;
    gHintFrames = 0;
    gAppliedVersion = 0;
}

// ------------------------------------------------------------------------------------------
// Menú dentro del juego
// ------------------------------------------------------------------------------------------
using namespace pl::modmenu;

ConfigNodeV2 baseNode(const std::string& key, const std::string& title, const std::string& desc, ConfigControlTypeV2 type,
                      const std::string& category) {
    ConfigNodeV2 n;
    n.id = key;
    n.key = key;
    n.title = title;
    n.description = desc;
    n.type = type;
    n.category = category;
    return n;
}

ConfigNodeV2 toggleNode(const std::string& key, const std::string& title, const std::string& desc, bool cur,
                        const std::string& cat) {
    auto n = baseNode(key, title, desc, ConfigControlTypeV2::Toggle, cat);
    n.defaultValue = n.currentValue = cur ? "true" : "false";
    n.hasCurrentValue = true;
    return n;
}

ConfigNodeV2 sliderNode(const std::string& key, const std::string& title, const std::string& desc, int cur, int mn,
                        int mx, int step, const std::string& unit, const std::string& cat) {
    auto n = baseNode(key, title, desc, ConfigControlTypeV2::SliderInt, cat);
    n.defaultValue = n.currentValue = std::to_string(cur);
    n.hasCurrentValue = true;
    n.minValue = std::to_string(mn);
    n.maxValue = std::to_string(mx);
    n.step = std::to_string(step);
    n.unit = unit;
    return n;
}

ConfigNodeV2 choiceNode(const std::string& key, const std::string& title, const std::string& desc, const std::string& cur,
                        std::vector<ConfigOptionV2> options, const std::string& cat) {
    auto n = baseNode(key, title, desc, ConfigControlTypeV2::Choice, cat);
    n.choiceStyle = ConfigChoiceStyleV2::Dropdown;
    n.defaultValue = n.currentValue = cur;
    n.hasCurrentValue = true;
    n.options = std::move(options);
    return n;
}

ConfigOptionV2 opt(const std::string& value, const std::string& label, const std::string& desc = {}) {
    ConfigOptionV2 o;
    o.value = value;
    o.label = label;
    o.description = desc;
    return o;
}

std::string buildSchema() {
    ConfigSchemaBuilder b;
    b.category("modo", "Modo", "Presets de optimización")
        .category("rendimiento", "Rendimiento", "Ajustes del hilo de render y del sistema")
        .category("pantalla", "Pantalla", "Resolución")
        .category("info", "Info", "Tu dispositivo")
        .defaultCategory("modo");

    b.node(choiceNode(
        "mode", "Modo de optimización",
        "Cada modo ajusta las opciones de abajo. Si cambias una opción a mano, pasa a Personalizado.",
        kModeValues[std::clamp(S.mode.load(), 0, ModeCount - 1)],
        {opt("custom", "Personalizado"),
         opt("ultra", "Ultra optimización", "Máxima prioridad, solo los núcleos más rápidos"),
         opt("alto", "Rendimiento alto", "Alta prioridad y núcleos rápidos"),
         opt("medio", "Rendimiento medio", "Prioridad media, sin fijar núcleos"),
         opt("equilibrado", "Equilibrado", "Prioridad baja, cuida calor y batería"),
         opt("bajo", "Rendimiento bajo (ahorro)", "Límite de 40 FPS, menos calor y batería"),
         opt("vibrant", "Priorizar Vibrant Visuals", "Pensado para packs pesados: prioridad alta y control de temperatura")},
        "modo"));

    b.node(sliderNode("fpsCap", "Límite de FPS", "0 = sin límite", S.fpsCap.load(), 0, 240, 5, "FPS", "rendimiento"));
    b.node(sliderNode("boost", "Prioridad del hilo de render",
                      "0 = no tocar. Más alto = el sistema le da más tiempo de CPU (si Android lo permite)", S.boost.load(),
                      0, 15, 1, "", "rendimiento"));
    b.node(toggleNode("performanceHint", "Performance Hint (Android 13+)",
                      "Le avisa a Android cuánto tarda cada frame para ajustar la frecuencia de CPU/GPU", S.hint.load(),
                      "rendimiento"));
    b.node(sliderNode("hintFps", "Objetivo de FPS del hint", "0 = automático", S.hintFps.load(), 0, 144, 5, "FPS",
                      "rendimiento"));
    b.node(choiceNode("cores", "Núcleos del hilo de render",
                      "Detectados por frecuencia, funciona igual en Snapdragon, MediaTek, Exynos y Tensor",
                      std::to_string(S.cores.load()),
                      {opt("0", "No tocar"), opt("1", "Núcleos rápidos", "Los de frecuencia alta"),
                       opt("2", "Solo el más rápido", "Agresivo: puede calentar más")},
                      "rendimiento"));
    b.node(toggleNode("thermalGuard", "Protección térmica",
                      "Si el celular se calienta, limita los FPS (45 si está tibio, 30 si está muy caliente)",
                      S.thermal.load(), "rendimiento"));
    b.node(toggleNode("batteryGuard", "Ahorro con batería baja",
                      "Limita a 30 FPS cuando la batería está baja y no estás cargando", S.battery.load(),
                      "rendimiento"));
    b.node(sliderNode("batteryPct", "Batería baja a partir de", "", S.batteryPct.load(), 5, 50, 5, "%", "rendimiento"));

    b.node(choiceNode(
        "resolution", "Resolución de render (experimental)",
        "Dibuja a menor resolución y estira a tu pantalla. Reinicia Minecraft para aplicar. Si algo falla, vuelve a Nativa.",
        resolutionValue(S.resolution.load()),
        {opt("native", "Nativa"), opt("1080", "1080p"), opt("900", "900p (1600x900 en 16:9)"), opt("720", "720p"),
         opt("540", "540p"), opt("360", "360p")},
        "pantalla"));

    auto info = baseNode("soc", "Chip detectado: " + gSocName,
                         "Núcleos: " + std::to_string(gCpu.count) + " | Frecuencia máx: " +
                             std::to_string(gCpu.top / 1000) + " MHz",
                         ConfigControlTypeV2::Info, "info");
    b.node(std::move(info));
    return b.toJson();
}

void pushSchema() {
    gPushing = true;
    setConfigSchemaJson(kModuleId, buildSchema());
    gPushing = false;
}

void onConfigChanged(std::string_view, std::string_view key, std::string_view value) {
    if (gPushing.load()) return;

    if (key == "mode") {
        const int m = modeFromValue(value);
        S.mode = m;
        applyPreset(m);
        gVersion++;
        saveConfig();
        pushSchema();
        return;
    }

    bool changed = true;
    if (key == "fpsCap") S.fpsCap = std::clamp(parseInt(value, 0), 0, 1000);
    else if (key == "boost") S.boost = std::clamp(parseInt(value, 0), 0, 15);
    else if (key == "performanceHint") S.hint = parseBool(value);
    else if (key == "hintFps") S.hintFps = std::clamp(parseInt(value, 0), 0, 240);
    else if (key == "cores") S.cores = std::clamp(parseInt(value, 0), 0, 2);
    else if (key == "thermalGuard") S.thermal = parseBool(value);
    else if (key == "batteryGuard") S.battery = parseBool(value);
    else if (key == "batteryPct") S.batteryPct = std::clamp(parseInt(value, 20), 5, 50);
    else if (key == "resolution") S.resolution = resolutionFromValue(value);
    else changed = false;
    if (!changed) return;

    const bool wasPreset = S.mode.load() != Custom && key != "resolution" && key != "hintFps" && key != "batteryPct";
    if (wasPreset) S.mode = Custom;
    gVersion++;
    saveConfig();
    if (wasPreset) pushSchema();
}

bool registerMenu(const std::string& modId) {
    ModuleBuilder builder(kModuleId, "SodiumLevi");
    builder.description("Optimización para Minecraft Bedrock - por erickzgcs")
        .modId(modId)
        .defaultEnabled(true)
        .onToggle([](std::string_view, bool enabled) {
            S.active = enabled;
            gVersion++;
        })
        .onConfigChanged(onConfigChanged);
    if (!builder.registerModule()) return false;
    pushSchema();
    return true;
}

}  // namespace

class SodiumLeviMod {
public:
    static SodiumLeviMod& instance() {
        static SodiumLeviMod mod;
        return mod;
    }

    bool load(pl::mod::ModContext& context) {
        loadConfig(context.configDir());
        detectCpu();
        loadSystemApis();
        context.logger().info("SodiumLevi 0.2 cargado (chip: {})", gSocName);
        return true;
    }

    bool enable(pl::mod::ModContext& context) {
        if (!installHooks()) {
            context.logger().info("SodiumLevi: no se pudo enganchar eglSwapBuffers");
            return false;
        }
        if (!registerMenu(context.id())) {
            context.logger().info("SodiumLevi: el menú no se pudo registrar (el mod sigue funcionando con sodiumlevi.json)");
        }
        return true;
    }

    bool disable(pl::mod::ModContext&) {
        unregisterModule(kModuleId);
        removeHooks();
        return true;
    }

    bool unload(pl::mod::ModContext&) { return true; }
};

PL_REGISTER_MOD(SodiumLeviMod, SodiumLeviMod::instance())
