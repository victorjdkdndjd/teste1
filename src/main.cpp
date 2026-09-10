#include <pl/Mod.hpp>
#include <pl/memory/Hook.hpp>
#include <pl/memory/Signature.hpp>

#include <android/log.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace {

constexpr const char* LOG_TAG = "FlyingPet";
constexpr std::string_view MC_MODULE = "libminecraftpe.so";

#define FP_LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define FP_LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// Diagnostic C: NormalTick + RenderLevel are already known-safe.
// This build probes ONLY two render object offsets and never draws anything.
constexpr std::size_t OFF_SCREEN_CONTEXT_TESSELLATOR = 0xB8;
constexpr std::size_t OFF_LEVEL_RENDERER_PLAYER = 0x420;

constexpr std::string_view SIG_NORMAL_TICK =
    "? ? ? FC ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? 91 ? ? ? D1 54 D0 3B D5 F3 03 00 AA ? ? ? F9 ? ? ? F8 ? ? ? 39";
constexpr std::string_view SIG_RENDER_LEVEL =
    "? ? ? FC ? ? ? 6D ? ? ? 6D ? ? ? 6D ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? 91 ? ? ? D1 57 D0 3B D5";

using NormalTick = void (*)(void*);
using RenderLevel = void (*)(void*, void*, void*);

NormalTick g_normalTickOriginal = nullptr;
RenderLevel g_renderLevelOriginal = nullptr;
void* g_normalTickTarget = nullptr;
void* g_renderLevelTarget = nullptr;
bool g_tickHooked = false;
bool g_renderHooked = false;

std::atomic<bool> g_firstTickSeen{false};
std::atomic<bool> g_firstRenderSeen{false};
std::atomic<bool> g_offsetsProbed{false};

void normalTickHook(void* actor) {
    if (g_normalTickOriginal) {
        g_normalTickOriginal(actor);
    }

    if (!g_firstTickSeen.exchange(true, std::memory_order_relaxed)) {
        FP_LOGI("Diagnostic C: first NormalTick reached safely. actor=%p", actor);
    }
}

void renderLevelHook(void* self, void* screenContext, void* a3) {
    if (g_renderLevelOriginal) {
        g_renderLevelOriginal(self, screenContext, a3);
    }

    if (!g_firstRenderSeen.exchange(true, std::memory_order_relaxed)) {
        FP_LOGI("Diagnostic C: first RenderLevel reached safely. self=%p screenContext=%p",
                self, screenContext);
    }

    // Probe only once. No nested dereference, no camera/material/Tessellator calls.
    if (!g_offsetsProbed.exchange(true, std::memory_order_relaxed)) {
        const auto selfAddr = reinterpret_cast<std::uintptr_t>(self);
        const auto screenAddr = reinterpret_cast<std::uintptr_t>(screenContext);

        if (selfAddr < 0x10000 || screenAddr < 0x10000) {
            FP_LOGE("Diagnostic C: invalid base pointer(s): self=%p screenContext=%p",
                    self, screenContext);
            return;
        }

        const auto tessellator = *reinterpret_cast<const std::uintptr_t*>(
            screenAddr + OFF_SCREEN_CONTEXT_TESSELLATOR);
        FP_LOGI("Diagnostic C: ScreenContext+0xB8 read safely. tessellator=%p plausible=%s",
                reinterpret_cast<void*>(tessellator),
                tessellator >= 0x10000 ? "yes" : "no");

        const auto levelRendererPlayer = *reinterpret_cast<const std::uintptr_t*>(
            selfAddr + OFF_LEVEL_RENDERER_PLAYER);
        FP_LOGI("Diagnostic C: LevelRenderer+0x420 read safely. levelRendererPlayer=%p plausible=%s",
                reinterpret_cast<void*>(levelRendererPlayer),
                levelRendererPlayer >= 0x10000 ? "yes" : "no");

        FP_LOGI("Diagnostic C: offset probe complete. NO nested memory reads and NO drawing.");
    }
}

bool resolveRuntime() {
    const auto tick = pl::memory::resolveSignature(SIG_NORMAL_TICK, MC_MODULE);
    const auto render = pl::memory::resolveSignature(SIG_RENDER_LEVEL, MC_MODULE);

    if (!tick) {
        FP_LOGE("Diagnostic C: NormalTick signature not found.");
        return false;
    }
    if (!render) {
        FP_LOGE("Diagnostic C: RenderLevel signature not found.");
        return false;
    }

    g_normalTickTarget = reinterpret_cast<void*>(tick);
    g_renderLevelTarget = reinterpret_cast<void*>(render);
    FP_LOGI("Diagnostic C: NormalTick resolved at %p", g_normalTickTarget);
    FP_LOGI("Diagnostic C: RenderLevel resolved at %p", g_renderLevelTarget);
    return true;
}

bool installHooks() {
    int result = pl::memory::hook(
        g_normalTickTarget,
        reinterpret_cast<void*>(&normalTickHook),
        reinterpret_cast<void**>(&g_normalTickOriginal));
    if (result != 0) {
        FP_LOGE("Diagnostic C: failed to hook NormalTick (code=%d).", result);
        return false;
    }
    g_tickHooked = true;

    result = pl::memory::hook(
        g_renderLevelTarget,
        reinterpret_cast<void*>(&renderLevelHook),
        reinterpret_cast<void**>(&g_renderLevelOriginal));
    if (result != 0) {
        FP_LOGE("Diagnostic C: failed to hook RenderLevel (code=%d).", result);
        pl::memory::unhook(g_normalTickTarget, reinterpret_cast<void*>(&normalTickHook));
        g_tickHooked = false;
        g_normalTickOriginal = nullptr;
        return false;
    }
    g_renderHooked = true;

    FP_LOGI("Diagnostic C enabled: probing offsets 0xB8 and 0x420 only; NO drawing.");
    return true;
}

void removeHooks() {
    if (g_renderHooked && g_renderLevelTarget) {
        pl::memory::unhook(g_renderLevelTarget, reinterpret_cast<void*>(&renderLevelHook));
    }
    if (g_tickHooked && g_normalTickTarget) {
        pl::memory::unhook(g_normalTickTarget, reinterpret_cast<void*>(&normalTickHook));
    }

    g_renderHooked = false;
    g_tickHooked = false;
    g_renderLevelOriginal = nullptr;
    g_normalTickOriginal = nullptr;
    g_renderLevelTarget = nullptr;
    g_normalTickTarget = nullptr;
    g_firstTickSeen.store(false, std::memory_order_relaxed);
    g_firstRenderSeen.store(false, std::memory_order_relaxed);
    g_offsetsProbed.store(false, std::memory_order_relaxed);
}

class FlyingPetMod {
public:
    static FlyingPetMod& instance() {
        static FlyingPetMod mod;
        return mod;
    }

    bool load(pl::mod::ModContext&) {
        FP_LOGI("Flying Pet v0.2.4 Diagnostic C loaded.");
        return true;
    }

    bool enable(pl::mod::ModContext&) {
        if (!resolveRuntime()) return false;
        return installHooks();
    }

    bool disable(pl::mod::ModContext&) {
        removeHooks();
        return true;
    }

    bool unload(pl::mod::ModContext&) {
        removeHooks();
        return true;
    }
};

} // namespace

PL_REGISTER_MOD(FlyingPetMod, FlyingPetMod::instance())
