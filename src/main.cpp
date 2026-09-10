#include <pl/Mod.hpp>
#include <pl/memory/Hook.hpp>
#include <pl/memory/Signature.hpp>

#include <android/log.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace {

constexpr const char* LOG_TAG = "FlyingPet";
constexpr std::string_view MC_MODULE = "libminecraftpe.so";

#define FP_LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define FP_LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// Diagnostic H: keep NormalTick as a control signal and retry the render pipeline
// quickly with the BedrockTools-preferred RenderMeshImmediately2 variant.
constexpr std::size_t OFF_SCREEN_CONTEXT_COLOR_HOLDER = 0x30;
constexpr std::size_t OFF_SCREEN_CONTEXT_TESSELLATOR = 0xB8;
constexpr std::size_t OFF_LEVEL_RENDERER_PLAYER = 0x420;
constexpr std::size_t OFF_SELECTION_OVERLAY_MATERIAL = 0x1030;
constexpr std::uint64_t RENDER_WARMUP_FRAMES = 30;

constexpr std::string_view SIG_NORMAL_TICK =
    "? ? ? FC ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? 91 ? ? ? D1 54 D0 3B D5 F3 03 00 AA ? ? ? F9 ? ? ? F8 ? ? ? 39";
constexpr std::string_view SIG_RENDER_LEVEL =
    "? ? ? FC ? ? ? 6D ? ? ? 6D ? ? ? 6D ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? 91 ? ? ? D1 57 D0 3B D5";
constexpr std::string_view SIG_TESSELLATOR_BEGIN =
    "? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 FD 03 00 91 ? ? ? 39 ? ? ? 39 08 01 09 2A";
constexpr std::string_view SIG_TESSELLATOR_COLOR =
    "? ? ? 52 ? ? ? 39 04 01 27 1E";
constexpr std::string_view SIG_TESSELLATOR_VERTEX =
    "? ? ? D1 ? ? ? FD ? ? ? 6D ? ? ? A9 ? ? ? F9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? 91 58 D0 3B D5 ? ? ? F9";
constexpr std::string_view SIG_RENDER_MESH_2 =
    "? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 FD 03 00 91 ? ? ? D1 57 D0 3B D5 F6 03 00 AA E0 03 01 AA ? ? ? F9 F4 03 03 AA";

using NormalTick = void (*)(void*);
using RenderLevel = void (*)(void*, void*, void*);
using TessellatorBegin = void (*)(void*, void*, int, int, int);
using TessellatorColor = void (*)(void*, float, float, float, float);
using TessellatorVertex = void (*)(void*, float, float, float);
using RenderMeshImmediately = void (*)(void*, void*, void*, char*);

NormalTick g_normalTickOriginal = nullptr;
RenderLevel g_renderLevelOriginal = nullptr;
TessellatorBegin g_tessBegin = nullptr;
TessellatorColor g_tessColor = nullptr;
TessellatorVertex g_tessVertex = nullptr;
RenderMeshImmediately g_renderMesh2 = nullptr;

void* g_normalTickTarget = nullptr;
void* g_renderLevelTarget = nullptr;
bool g_tickHooked = false;
bool g_renderHooked = false;

std::atomic<bool> g_firstTickSeen{false};
std::atomic<bool> g_firstRenderSeen{false};
std::atomic<std::uint64_t> g_renderFrames{0};
std::atomic<bool> g_pipelineAttempted{false};
std::atomic<bool> g_loggedMaterialWait{false};

bool plausible(std::uintptr_t p) {
    return p >= 0x10000;
}

std::uintptr_t resolve(std::string_view name, std::string_view sig) {
    const auto addr = pl::memory::resolveSignature(sig, MC_MODULE);
    if (addr) {
        FP_LOGI("Diagnostic H: %.*s resolved at %p",
                static_cast<int>(name.size()), name.data(), reinterpret_cast<void*>(addr));
    } else {
        FP_LOGE("Diagnostic H: %.*s signature not found.",
                static_cast<int>(name.size()), name.data());
    }
    return addr;
}

void normalTickHook(void* actor) {
    if (g_normalTickOriginal) g_normalTickOriginal(actor);
    if (!g_firstTickSeen.exchange(true, std::memory_order_relaxed)) {
        FP_LOGI("Diagnostic H: first NormalTick reached safely. actor=%p", actor);
    }
}

void renderLevelHook(void* self, void* screenContext, void* a3) {
    if (g_renderLevelOriginal) g_renderLevelOriginal(self, screenContext, a3);

    if (!g_firstRenderSeen.exchange(true, std::memory_order_relaxed)) {
        FP_LOGI("Diagnostic H: first RenderLevel reached safely. self=%p screenContext=%p", self, screenContext);
    }

    const auto frame = g_renderFrames.fetch_add(1, std::memory_order_relaxed) + 1;
    if (frame < RENDER_WARMUP_FRAMES) return;
    if (g_pipelineAttempted.load(std::memory_order_acquire)) return;

    const auto selfAddr = reinterpret_cast<std::uintptr_t>(self);
    const auto screenAddr = reinterpret_cast<std::uintptr_t>(screenContext);
    if (!plausible(selfAddr) || !plausible(screenAddr)) return;

    const auto tessAddr = *reinterpret_cast<const std::uintptr_t*>(screenAddr + OFF_SCREEN_CONTEXT_TESSELLATOR);
    const auto lrp = *reinterpret_cast<const std::uintptr_t*>(selfAddr + OFF_LEVEL_RENDERER_PLAYER);
    const auto colorHolder = *reinterpret_cast<const std::uintptr_t*>(screenAddr + OFF_SCREEN_CONTEXT_COLOR_HOLDER);
    if (!plausible(tessAddr) || !plausible(lrp) || !plausible(colorHolder)) return;

    const auto materialHolder = lrp + OFF_SELECTION_OVERLAY_MATERIAL;
    std::uintptr_t materialWords[2]{};
    std::memcpy(materialWords, reinterpret_cast<const void*>(materialHolder), sizeof(materialWords));
    if (!plausible(materialWords[0])) {
        if (!g_loggedMaterialWait.exchange(true, std::memory_order_relaxed)) {
            FP_LOGI("Diagnostic H: waiting for selection MaterialPtr. holder=%p word0=%p word1=%p",
                    reinterpret_cast<void*>(materialHolder),
                    reinterpret_cast<void*>(materialWords[0]),
                    reinterpret_cast<void*>(materialWords[1]));
        }
        return;
    }

    if (g_pipelineAttempted.exchange(true, std::memory_order_acq_rel)) return;

    void* tessellator = reinterpret_cast<void*>(tessAddr);
    void* material = reinterpret_cast<void*>(materialHolder);

    FP_LOGI("Diagnostic H: pipeline ready at frame=%llu. tess=%p materialHolder=%p material0=%p",
            static_cast<unsigned long long>(frame), tessellator, material,
            reinterpret_cast<void*>(materialWords[0]));

    FP_LOGI("Diagnostic H: about to call TessellatorBegin(mode=4, vertices=2).");
    g_tessBegin(tessellator, nullptr, 4, 2, 0);
    FP_LOGI("Diagnostic H: TessellatorBegin returned safely.");

    FP_LOGI("Diagnostic H: about to call TessellatorColor.");
    g_tessColor(tessellator, 0.25f, 1.0f, 0.35f, 1.0f);
    FP_LOGI("Diagnostic H: TessellatorColor returned safely.");

    FP_LOGI("Diagnostic H: about to submit vertex 1.");
    g_tessVertex(tessellator, 0.0f, 1.9f, 1.0f);
    FP_LOGI("Diagnostic H: vertex 1 returned safely.");

    FP_LOGI("Diagnostic H: about to submit vertex 2.");
    g_tessVertex(tessellator, 0.65f, 1.9f, 1.0f);
    FP_LOGI("Diagnostic H: vertex 2 returned safely.");

    char pad[0x58]{};
    FP_LOGI("Diagnostic H: about to call RenderMeshImmediately2.");
    g_renderMesh2(screenContext, tessellator, material, pad);
    FP_LOGI("Diagnostic H: RenderMeshImmediately2 returned safely.");
    FP_LOGI("Diagnostic H: FULL variant-2 drawing pipeline completed safely.");
}

bool resolveRuntime() {
    const auto tick = resolve("NormalTick", SIG_NORMAL_TICK);
    const auto render = resolve("RenderLevel", SIG_RENDER_LEVEL);
    const auto begin = resolve("TessellatorBegin", SIG_TESSELLATOR_BEGIN);
    const auto color = resolve("TessellatorColor", SIG_TESSELLATOR_COLOR);
    const auto vertex = resolve("TessellatorVertex", SIG_TESSELLATOR_VERTEX);
    const auto mesh2 = resolve("RenderMeshImmediately2", SIG_RENDER_MESH_2);

    if (!tick || !render || !begin || !color || !vertex || !mesh2) return false;

    g_normalTickTarget = reinterpret_cast<void*>(tick);
    g_renderLevelTarget = reinterpret_cast<void*>(render);
    g_tessBegin = reinterpret_cast<TessellatorBegin>(begin);
    g_tessColor = reinterpret_cast<TessellatorColor>(color);
    g_tessVertex = reinterpret_cast<TessellatorVertex>(vertex);
    g_renderMesh2 = reinterpret_cast<RenderMeshImmediately>(mesh2);
    return true;
}

bool installHooks() {
    int result = pl::memory::hook(
        g_normalTickTarget,
        reinterpret_cast<void*>(&normalTickHook),
        reinterpret_cast<void**>(&g_normalTickOriginal));
    if (result != 0) {
        FP_LOGE("Diagnostic H: failed to hook NormalTick (code=%d).", result);
        return false;
    }
    g_tickHooked = true;

    result = pl::memory::hook(
        g_renderLevelTarget,
        reinterpret_cast<void*>(&renderLevelHook),
        reinterpret_cast<void**>(&g_renderLevelOriginal));
    if (result != 0) {
        FP_LOGE("Diagnostic H: failed to hook RenderLevel (code=%d).", result);
        pl::memory::unhook(g_normalTickTarget, reinterpret_cast<void*>(&normalTickHook));
        g_tickHooked = false;
        g_normalTickOriginal = nullptr;
        return false;
    }
    g_renderHooked = true;

    FP_LOGI("Diagnostic H enabled: NormalTick control + 30-frame RenderMeshImmediately2 test.");
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
    g_tessBegin = nullptr;
    g_tessColor = nullptr;
    g_tessVertex = nullptr;
    g_renderMesh2 = nullptr;
    g_firstTickSeen.store(false, std::memory_order_relaxed);
    g_firstRenderSeen.store(false, std::memory_order_relaxed);
    g_renderFrames.store(0, std::memory_order_relaxed);
    g_pipelineAttempted.store(false, std::memory_order_relaxed);
    g_loggedMaterialWait.store(false, std::memory_order_relaxed);
}

class FlyingPetMod {
public:
    static FlyingPetMod& instance() {
        static FlyingPetMod mod;
        return mod;
    }

    bool load(pl::mod::ModContext&) {
        FP_LOGI("Flying Pet v0.2.9 Diagnostic H loaded.");
        return true;
    }

    bool enable(pl::mod::ModContext&) {
        return resolveRuntime() && installHooks();
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
