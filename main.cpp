// SodiumLevi - mod de optimización para LeviLauncher (Minecraft Bedrock, Android arm64).
//
// v0.1: optimizaciones "seguras" que NO tocan el render del juego (cero cambios visuales):
//   1. Prioridad del hilo de render.
//   2. APerformanceHint: le dice a Android cuánto tarda cada frame para que el
//      gobernador de CPU/GPU suba la frecuencia justo cuando hace falta.
//   3. (Opcional) Limitador de FPS para ahorrar batería / bajar temperatura.
//
// El único punto de enganche es eglSwapBuffers (símbolo exportado de libEGL.so),
// por eso NO depende de firmas/offsets de una versión concreta de Minecraft.

#include <EGL/egl.h>
#include <android/log.h>
#include <dlfcn.h>
#include <errno.h>
#include <pl/Mod.hpp>
#include <pl/memory/Hook.hpp>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

namespace {

constexpr const char* kTag = "SodiumLevi";

struct Config {
    bool renderThreadPriority = true;  // subir prioridad del hilo de render
    bool performanceHint = true;       // APerformanceHint (Android 13+, si no existe se ignora)
    int fpsCap = 0;                    // 0 = sin límite (no toca nada)
};

Config gConfig;

// ---- APerformanceHint (se resuelve con dlsym: en Android < 13 simplemente no existe) ----
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
void* gSession = nullptr;
std::atomic<pid_t> gRenderTid{0};

// Estado del frame (solo lo toca el hilo de render)
int64_t gLastReturnNs = 0;
int64_t gTargetNs = 16'666'666;
int gHintFrames = 0;

using EglSwapFn = EGLBoolean (*)(EGLDisplay, EGLSurface);
EglSwapFn gOriginalSwap = nullptr;
void* gSwapTarget = nullptr;
bool gHookInstalled = false;

int64_t nowNs() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return int64_t(ts.tv_sec) * 1'000'000'000LL + ts.tv_nsec;
}

pid_t currentTid() { return static_cast<pid_t>(syscall(SYS_gettid)); }

void loadHintApi() {
    if (gHint.ok()) return;
    void* lib = dlopen("libandroid.so", RTLD_NOW);
    if (!lib) return;
    gHint.getManager = reinterpret_cast<HintApi::GetManager>(dlsym(lib, "APerformanceHint_getManager"));
    gHint.createSession = reinterpret_cast<HintApi::CreateSession>(dlsym(lib, "APerformanceHint_createSession"));
    gHint.updateTarget = reinterpret_cast<HintApi::UpdateTarget>(dlsym(lib, "APerformanceHint_updateTargetWorkDuration"));
    gHint.reportActual = reinterpret_cast<HintApi::ReportActual>(dlsym(lib, "APerformanceHint_reportActualWorkDuration"));
    gHint.closeSession = reinterpret_cast<HintApi::CloseSession>(dlsym(lib, "APerformanceHint_closeSession"));
}

// Se ejecuta una sola vez, desde el hilo que llama a eglSwapBuffers (= hilo de render).
void setupRenderThread(pid_t tid) {
    if (gConfig.renderThreadPriority) {
        // Mejor esfuerzo: si el sistema no lo permite, no pasa nada.
        if (setpriority(PRIO_PROCESS, static_cast<id_t>(tid), -8) != 0) {
            __android_log_print(ANDROID_LOG_INFO, kTag, "setpriority no permitido (errno=%d), se ignora", errno);
        }
    }
    if (gConfig.performanceHint) {
        loadHintApi();
        if (gHint.ok()) {
            if (void* manager = gHint.getManager()) {
                const int32_t tids[1] = {static_cast<int32_t>(tid)};
                gSession = gHint.createSession(manager, tids, 1, gTargetNs);
                __android_log_print(ANDROID_LOG_INFO, kTag, "PerformanceHint %s", gSession ? "activo" : "no disponible");
            }
        } else {
            __android_log_print(ANDROID_LOG_INFO, kTag, "APerformanceHint no existe en este Android, se ignora");
        }
    }
}

void sleepUntil(int64_t deadlineNs) {
    timespec ts{};
    ts.tv_sec = deadlineNs / 1'000'000'000LL;
    ts.tv_nsec = deadlineNs % 1'000'000'000LL;
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr) == EINTR) {
    }
}

EGLBoolean swapDetour(EGLDisplay display, EGLSurface surface) {
    const pid_t tid = currentTid();
    pid_t expected = 0;
    if (gRenderTid.compare_exchange_strong(expected, tid)) {
        setupRenderThread(tid);
    }
    const bool isRenderThread = (gRenderTid.load() == tid);

    const int64_t enterNs = nowNs();
    const int64_t workNs = gLastReturnNs ? (enterNs - gLastReturnNs) : 0;

    EGLBoolean result = gOriginalSwap ? gOriginalSwap(display, surface) : EGL_FALSE;

    if (!isRenderThread) return result;

    int64_t afterNs = nowNs();

    // Límite de FPS opcional (pacing absoluto, sin acumular deriva).
    if (gConfig.fpsCap > 0) {
        const int64_t frameNs = 1'000'000'000LL / gConfig.fpsCap;
        const int64_t deadline = enterNs + frameNs;
        if (afterNs < deadline) {
            sleepUntil(deadline);
            afterNs = nowNs();
        }
    }

    // Informar a Android del tiempo real de trabajo del frame.
    if (gSession && workNs > 0) {
        // El objetivo sigue el periodo real entre frames, acotado a 4–33 ms (240–30 FPS).
        const int64_t period = std::clamp<int64_t>(afterNs - gLastReturnNs, 4'000'000, 33'333'333);
        if (gLastReturnNs && ++gHintFrames % 60 == 0 && period != gTargetNs) {
            gTargetNs = period;
            gHint.updateTarget(gSession, gTargetNs);
        }
        gHint.reportActual(gSession, workNs);
    }

    gLastReturnNs = afterNs;
    return result;
}

void loadConfig(const std::filesystem::path& dir) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const auto file = dir / "sodiumlevi.json";

    nlohmann::json j = nlohmann::json::object();
    if (std::ifstream in{file}) {
        j = nlohmann::json::parse(in, nullptr, /*allow_exceptions=*/false);
        if (!j.is_object()) j = nlohmann::json::object();
    }
    gConfig.renderThreadPriority = j.value("renderThreadPriority", gConfig.renderThreadPriority);
    gConfig.performanceHint = j.value("performanceHint", gConfig.performanceHint);
    gConfig.fpsCap = std::clamp(j.value("fpsCap", gConfig.fpsCap), 0, 1000);

    // Escribe el archivo con los valores vigentes para que el usuario pueda editarlo.
    nlohmann::json out = {
        {"_info", "fpsCap: 0 = sin limite. Reinicia Minecraft tras editar."},
        {"renderThreadPriority", gConfig.renderThreadPriority},
        {"performanceHint", gConfig.performanceHint},
        {"fpsCap", gConfig.fpsCap},
    };
    if (std::ofstream o{file}) o << out.dump(2) << "\n";
}

bool installHook() {
    if (gHookInstalled) return true;
    void* egl = dlopen("libEGL.so", RTLD_NOW | RTLD_NOLOAD);
    if (!egl) egl = dlopen("libEGL.so", RTLD_NOW);
    if (!egl) return false;
    gSwapTarget = dlsym(egl, "eglSwapBuffers");
    if (!gSwapTarget) return false;
    if (pl::memory::hook(gSwapTarget, reinterpret_cast<void*>(swapDetour),
                         reinterpret_cast<void**>(&gOriginalSwap)) != 0) {
        return false;
    }
    gHookInstalled = true;
    return true;
}

void removeHook() {
    if (gHookInstalled && gSwapTarget) {
        pl::memory::unhook(gSwapTarget, reinterpret_cast<void*>(swapDetour));
    }
    gHookInstalled = false;
    if (gSession && gHint.closeSession) {
        gHint.closeSession(gSession);
    }
    gSession = nullptr;
    gRenderTid = 0;
    gLastReturnNs = 0;
    gHintFrames = 0;
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
        context.logger().info("SodiumLevi cargado (fpsCap={}, hint={}, prioridad={})", gConfig.fpsCap,
                              gConfig.performanceHint, gConfig.renderThreadPriority);
        return true;
    }

    bool enable(pl::mod::ModContext& context) {
        if (!installHook()) {
            context.logger().info("SodiumLevi: no se pudo enganchar eglSwapBuffers");
            return false;
        }
        return true;
    }

    bool disable(pl::mod::ModContext&) {
        removeHook();
        return true;
    }

    bool unload(pl::mod::ModContext&) { return true; }
};

PL_REGISTER_MOD(SodiumLeviMod, SodiumLeviMod::instance())
