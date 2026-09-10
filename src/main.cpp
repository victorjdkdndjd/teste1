#include <pl/Mod.hpp>
#include <pl/memory/Hook.hpp>
#include <pl/memory/Signature.hpp>

#include <android/log.h>

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace {

constexpr const char* LOG_TAG = "FlyingPet";
constexpr std::string_view MC_MODULE = "libminecraftpe.so";

#define FP_LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define FP_LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// Diagnostic E: render-memory reads from D are already known-safe.
// This build isolates the remaining Player-side read used by the crashy build:
// Actor + 0x208 -> StateVectorComponent -> Vec3 position.
// No Tessellator/RenderMesh calls are made.
constexpr std::size_t OFF_ACTOR_STATE_VECTOR = 0x208;
constexpr std::size_t OFF_SCREEN_CONTEXT_COLOR_HOLDER = 0x30;
constexpr std::size_t OFF_SCREEN_CONTEXT_TESSELLATOR = 0xB8;
constexpr std::size_t OFF_LEVEL_RENDERER_PLAYER = 0x420;
constexpr std::size_t OFF_CAMERA_POS = 0x61C;
constexpr std::size_t OFF_SELECTION_OVERLAY_MATERIAL = 0x1030;

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
std::atomic<bool> g_playerProbeDone{false};
std::atomic<bool> g_renderProbeDone{false};

void normalTickHook(void* actor) {
    if (g_normalTickOriginal) g_normalTickOriginal(actor);

    if (!g_firstTickSeen.exchange(true, std::memory_order_relaxed)) {
        FP_LOGI("Diagnostic E: first NormalTick reached safely. actor=%p", actor);
    }

    if (g_playerProbeDone.exchange(true, std::memory_order_relaxed)) return;

    const auto actorAddr = reinterpret_cast<std::uintptr_t>(actor);
    if (actorAddr < 0x10000) {
        FP_LOGE("Diagnostic E: Actor pointer is not plausible: %p", actor);
        return;
    }

    FP_LOGI("Diagnostic E: about to read Actor+0x208 StateVectorComponent pointer.");
    const auto stateVector = *reinterpret_cast<const std::uintptr_t*>(actorAddr + OFF_ACTOR_STATE_VECTOR);
    FP_LOGI("Diagnostic E: StateVectorComponent pointer read safely: %p plausible=%s",
            reinterpret_cast<void*>(stateVector), stateVector >= 0x10000 ? "yes" : "no");

    if (stateVector < 0x10000) {
        FP_LOGE("Diagnostic E: StateVectorComponent pointer is not plausible; skipping Vec3 read.");
        return;
    }

    FP_LOGI("Diagnostic E: about to read Vec3 position from StateVectorComponent.");
    float pos[3]{};
    std::memcpy(pos, reinterpret_cast<const void*>(stateVector), sizeof(pos));
    FP_LOGI("Diagnostic E: Player position read safely: x=%f y=%f z=%f finite=%s",
            pos[0], pos[1], pos[2],
            (std::isfinite(pos[0]) && std::isfinite(pos[1]) && std::isfinite(pos[2])) ? "yes" : "no");
    FP_LOGI("Diagnostic E: Player StateVector probe complete. NO drawing calls were made.");
}

void renderLevelHook(void* self, void* screenContext, void* a3) {
    if (g_renderLevelOriginal) g_renderLevelOriginal(self, screenContext, a3);

    if (!g_firstRenderSeen.exchange(true, std::memory_order_relaxed)) {
        FP_LOGI("Diagnostic E: first RenderLevel reached safely. self=%p screenContext=%p", self, screenContext);
    }

    // Keep D's already-safe render reads as a control sample.
    if (g_renderProbeDone.exchange(true, std::memory_order_relaxed)) return;

    const auto selfAddr = reinterpret_cast<std::uintptr_t>(self);
    const auto screenAddr = reinterpret_cast<std::uintptr_t>(screenContext);
    if (selfAddr < 0x10000 || screenAddr < 0x10000) {
        FP_LOGE("Diagnostic E: invalid render base pointer(s).");
        return;
    }

    const auto tessellator = *reinterpret_cast<const std::uintptr_t*>(screenAddr + OFF_SCREEN_CONTEXT_TESSELLATOR);
    const auto lrp = *reinterpret_cast<const std::uintptr_t*>(selfAddr + OFF_LEVEL_RENDERER_PLAYER);
    FP_LOGI("Diagnostic E: render base offsets safe. tessellator=%p lrp=%p",
            reinterpret_cast<void*>(tessellator), reinterpret_cast<void*>(lrp));

    if (lrp < 0x10000) return;

    float cam[3]{};
    std::memcpy(cam, reinterpret_cast<const void*>(lrp + OFF_CAMERA_POS), sizeof(cam));
    FP_LOGI("Diagnostic E: camera control read safe: x=%f y=%f z=%f", cam[0], cam[1], cam[2]);

    const auto colorHolder = *reinterpret_cast<const std::uintptr_t*>(screenAddr + OFF_SCREEN_CONTEXT_COLOR_HOLDER);
    FP_LOGI("Diagnostic E: ColorHolder control pointer=%p plausible=%s",
            reinterpret_cast<void*>(colorHolder), colorHolder >= 0x10000 ? "yes" : "no");

    if (colorHolder >= 0x10000) {
        float rgba[4]{};
        std::memcpy(rgba, reinterpret_cast<const void*>(colorHolder), sizeof(rgba));
        FP_LOGI("Diagnostic E: ColorHolder control RGBA=%f %f %f %f",
                rgba[0], rgba[1], rgba[2], rgba[3]);
    }

    const auto materialHolder = lrp + OFF_SELECTION_OVERLAY_MATERIAL;
    FP_LOGI("Diagnostic E: material-holder control address=%p (not dereferenced)",
            reinterpret_cast<void*>(materialHolder));
}

bool resolveRuntime() {
    const auto tick = pl::memory::resolveSignature(SIG_NORMAL_TICK, MC_MODULE);
    const auto render = pl::memory::resolveSignature(SIG_RENDER_LEVEL, MC_MODULE);
    if (!tick || !render) {
        FP_LOGE("Diagnostic E: required signature missing. tick=%p render=%p",
                reinterpret_cast<void*>(tick), reinterpret_cast<void*>(render));
        return false;
    }
    g_normalTickTarget = reinterpret_cast<void*>(tick);
    g_renderLevelTarget = reinterpret_cast<void*>(render);
    FP_LOGI("Diagnostic E: NormalTick resolved at %p", g_normalTickTarget);
    FP_LOGI("Diagnostic E: RenderLevel resolved at %p", g_renderLevelTarget);
    return true;
}

bool installHooks() {
    int result = pl::memory::hook(g_normalTickTarget, reinterpret_cast<void*>(&normalTickHook), reinterpret_cast<void**>(&g_normalTickOriginal));
    if (result != 0) {
        FP_LOGE("Diagnostic E: failed to hook NormalTick (code=%d).", result);
        return false;
    }
    g_tickHooked = true;

    result = pl::memory::hook(g_renderLevelTarget, reinterpret_cast<void*>(&renderLevelHook), reinterpret_cast<void**>(&g_renderLevelOriginal));
    if (result != 0) {
        FP_LOGE("Diagnostic E: failed to hook RenderLevel (code=%d).", result);
        pl::memory::unhook(g_normalTickTarget, reinterpret_cast<void*>(&normalTickHook));
        g_tickHooked = false;
        g_normalTickOriginal = nullptr;
        return false;
    }
    g_renderHooked = true;
    FP_LOGI("Diagnostic E enabled: probing Actor+0x208 StateVector; NO drawing.");
    return true;
}

void removeHooks() {
    if (g_renderHooked && g_renderLevelTarget) pl::memory::unhook(g_renderLevelTarget, reinterpret_cast<void*>(&renderLevelHook));
    if (g_tickHooked && g_normalTickTarget) pl::memory::unhook(g_normalTickTarget, reinterpret_cast<void*>(&normalTickHook));
    g_renderHooked = false;
    g_tickHooked = false;
    g_renderLevelOriginal = nullptr;
    g_normalTickOriginal = nullptr;
    g_renderLevelTarget = nullptr;
    g_normalTickTarget = nullptr;
    g_firstTickSeen.store(false, std::memory_order_relaxed);
    g_firstRenderSeen.store(false, std::memory_order_relaxed);
    g_playerProbeDone.store(false, std::memory_order_relaxed);
    g_renderProbeDone.store(false, std::memory_order_relaxed);
}

class FlyingPetMod {
public:
    static FlyingPetMod& instance() { static FlyingPetMod mod; return mod; }
    bool load(pl::mod::ModContext&) { FP_LOGI("Flying Pet v0.2.6 Diagnostic E loaded."); return true; }
    bool enable(pl::mod::ModContext&) { return resolveRuntime() && installHooks(); }
    bool disable(pl::mod::ModContext&) { removeHooks(); return true; }
    bool unload(pl::mod::ModContext&) { removeHooks(); return true; }
};

} // namespace

PL_REGISTER_MOD(FlyingPetMod, FlyingPetMod::instance())
