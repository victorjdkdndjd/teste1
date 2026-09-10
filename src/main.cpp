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

struct Vec3 {
    float x;
    float y;
    float z;
};

struct Color {
    float r;
    float g;
    float b;
    float a;
};

constexpr std::size_t OFF_ACTOR_STATE_VECTOR = 0x208;
constexpr std::size_t OFF_SCREEN_CONTEXT_COLOR_HOLDER = 0x30;
constexpr std::size_t OFF_SCREEN_CONTEXT_TESSELLATOR = 0xB8;
constexpr std::size_t OFF_LEVEL_RENDERER_PLAYER = 0x420;
constexpr std::size_t OFF_CAMERA_POS = 0x61C;
constexpr std::size_t OFF_SELECTION_OVERLAY_MATERIAL = 0x1030;
constexpr std::uint64_t RENDER_WARMUP_FRAMES = 30;
constexpr int PET_LINE_COUNT = 28;
constexpr int PET_VERTEX_COUNT = PET_LINE_COUNT * 2;

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

std::atomic<bool> g_playerValid{false};
std::atomic<bool> g_petInitialized{false};
std::atomic<float> g_playerX{0.0f};
std::atomic<float> g_playerY{0.0f};
std::atomic<float> g_playerZ{0.0f};
std::atomic<float> g_petX{0.0f};
std::atomic<float> g_petY{0.0f};
std::atomic<float> g_petZ{0.0f};
std::atomic<std::uint64_t> g_tickCounter{0};
std::atomic<std::uint64_t> g_renderFrames{0};
std::atomic<bool> g_renderStarted{false};
std::atomic<bool> g_loggedMaterialWait{false};

bool plausible(std::uintptr_t p) {
    return p >= 0x10000;
}

std::uintptr_t resolve(std::string_view name, std::string_view sig) {
    const auto addr = pl::memory::resolveSignature(sig, MC_MODULE);
    if (addr) {
        FP_LOGI("%.*s resolved at %p",
                static_cast<int>(name.size()), name.data(), reinterpret_cast<void*>(addr));
    } else {
        FP_LOGE("%.*s signature not found.",
                static_cast<int>(name.size()), name.data());
    }
    return addr;
}

void normalTickHook(void* actor) {
    if (g_normalTickOriginal) g_normalTickOriginal(actor);

    const auto actorAddr = reinterpret_cast<std::uintptr_t>(actor);
    if (!plausible(actorAddr)) return;

    const auto stateVector = *reinterpret_cast<const std::uintptr_t*>(actorAddr + OFF_ACTOR_STATE_VECTOR);
    if (!plausible(stateVector)) return;

    Vec3 player{};
    std::memcpy(&player, reinterpret_cast<const void*>(stateVector), sizeof(player));
    if (!std::isfinite(player.x) || !std::isfinite(player.y) || !std::isfinite(player.z)) return;

    g_playerX.store(player.x, std::memory_order_relaxed);
    g_playerY.store(player.y, std::memory_order_relaxed);
    g_playerZ.store(player.z, std::memory_order_relaxed);
    g_playerValid.store(true, std::memory_order_release);

    const auto tick = g_tickCounter.fetch_add(1, std::memory_order_relaxed) + 1;
    const float t = static_cast<float>(tick);

    // The pet gently orbits the player while hovering in the air.
    const float orbit = t * 0.045f;
    const float targetX = player.x + std::cos(orbit) * 1.45f;
    const float targetY = player.y + 1.60f + std::sin(t * 0.12f) * 0.14f;
    const float targetZ = player.z + std::sin(orbit) * 1.45f;

    if (!g_petInitialized.load(std::memory_order_acquire)) {
        g_petX.store(targetX, std::memory_order_relaxed);
        g_petY.store(targetY, std::memory_order_relaxed);
        g_petZ.store(targetZ, std::memory_order_relaxed);
        g_petInitialized.store(true, std::memory_order_release);
        FP_LOGI("Flying pet initialized near the player.");
        return;
    }

    float px = g_petX.load(std::memory_order_relaxed);
    float py = g_petY.load(std::memory_order_relaxed);
    float pz = g_petZ.load(std::memory_order_relaxed);

    const float dx = targetX - px;
    const float dy = targetY - py;
    const float dz = targetZ - pz;
    const float distSq = dx * dx + dy * dy + dz * dz;

    if (distSq > 100.0f) {
        px = targetX;
        py = targetY;
        pz = targetZ;
    } else {
        constexpr float follow = 0.20f;
        px += dx * follow;
        py += dy * follow;
        pz += dz * follow;
    }

    g_petX.store(px, std::memory_order_relaxed);
    g_petY.store(py, std::memory_order_relaxed);
    g_petZ.store(pz, std::memory_order_release);
}

void renderLevelHook(void* self, void* screenContext, void* a3) {
    if (g_renderLevelOriginal) g_renderLevelOriginal(self, screenContext, a3);

    const auto frame = g_renderFrames.fetch_add(1, std::memory_order_relaxed) + 1;
    if (frame < RENDER_WARMUP_FRAMES) return;
    if (!g_playerValid.load(std::memory_order_acquire) || !g_petInitialized.load(std::memory_order_acquire)) return;

    const auto selfAddr = reinterpret_cast<std::uintptr_t>(self);
    const auto screenAddr = reinterpret_cast<std::uintptr_t>(screenContext);
    if (!plausible(selfAddr) || !plausible(screenAddr)) return;

    const auto tessAddr = *reinterpret_cast<const std::uintptr_t*>(screenAddr + OFF_SCREEN_CONTEXT_TESSELLATOR);
    const auto lrp = *reinterpret_cast<const std::uintptr_t*>(selfAddr + OFF_LEVEL_RENDERER_PLAYER);
    const auto colorHolder = *reinterpret_cast<const std::uintptr_t*>(screenAddr + OFF_SCREEN_CONTEXT_COLOR_HOLDER);
    if (!plausible(tessAddr) || !plausible(lrp) || !plausible(colorHolder)) return;

    float cam[3]{};
    std::memcpy(cam, reinterpret_cast<const void*>(lrp + OFF_CAMERA_POS), sizeof(cam));
    if (!std::isfinite(cam[0]) || !std::isfinite(cam[1]) || !std::isfinite(cam[2])) return;

    const auto materialHolder = lrp + OFF_SELECTION_OVERLAY_MATERIAL;
    std::uintptr_t materialWords[2]{};
    std::memcpy(materialWords, reinterpret_cast<const void*>(materialHolder), sizeof(materialWords));
    if (!plausible(materialWords[0])) {
        if (!g_loggedMaterialWait.exchange(true, std::memory_order_relaxed)) {
            FP_LOGI("Waiting for selection material before drawing pet.");
        }
        return;
    }

    void* tessellator = reinterpret_cast<void*>(tessAddr);
    void* material = reinterpret_cast<void*>(materialHolder);

    const Vec3 center{
        g_petX.load(std::memory_order_relaxed),
        g_petY.load(std::memory_order_relaxed),
        g_petZ.load(std::memory_order_acquire)
    };

    if (!std::isfinite(center.x) || !std::isfinite(center.y) || !std::isfinite(center.z)) return;

    const float wingWave = std::sin(static_cast<float>(frame) * 0.30f) * 0.18f;
    constexpr float h = 0.28f;
    const float frontZ = center.z + h + 0.012f;

    const Color body{1.00f, 0.58f, 0.12f, 1.00f};
    const Color wing{0.20f, 0.85f, 1.00f, 0.95f};
    const Color face{1.00f, 1.00f, 1.00f, 1.00f};
    const Color accent{0.25f, 1.00f, 0.35f, 1.00f};

    auto cameraRelative = [&](const Vec3& p) -> Vec3 {
        return Vec3{p.x - cam[0], p.y - cam[1], p.z - cam[2]};
    };

    auto emitLine = [&](const Vec3& aWorld, const Vec3& bWorld, const Color& c) {
        const Vec3 a = cameraRelative(aWorld);
        const Vec3 b = cameraRelative(bWorld);
        g_tessColor(tessellator, c.r, c.g, c.b, c.a);
        g_tessVertex(tessellator, a.x, a.y, a.z);
        g_tessVertex(tessellator, b.x, b.y, b.z);
    };

    g_tessBegin(tessellator, nullptr, 4, PET_VERTEX_COUNT, 0);

    // Body cube: 12 edges.
    const Vec3 p000{center.x - h, center.y - h, center.z - h};
    const Vec3 p100{center.x + h, center.y - h, center.z - h};
    const Vec3 p110{center.x + h, center.y + h, center.z - h};
    const Vec3 p010{center.x - h, center.y + h, center.z - h};
    const Vec3 p001{center.x - h, center.y - h, center.z + h};
    const Vec3 p101{center.x + h, center.y - h, center.z + h};
    const Vec3 p111{center.x + h, center.y + h, center.z + h};
    const Vec3 p011{center.x - h, center.y + h, center.z + h};

    emitLine(p000, p100, body); emitLine(p100, p110, body);
    emitLine(p110, p010, body); emitLine(p010, p000, body);
    emitLine(p001, p101, body); emitLine(p101, p111, body);
    emitLine(p111, p011, body); emitLine(p011, p001, body);
    emitLine(p000, p001, body); emitLine(p100, p101, body);
    emitLine(p110, p111, body); emitLine(p010, p011, body);

    // Two animated triangular wings: 6 edges.
    const Vec3 l0{center.x - h, center.y + 0.10f, center.z};
    const Vec3 l1{center.x - 0.82f, center.y + 0.34f + wingWave, center.z - 0.05f};
    const Vec3 l2{center.x - 0.78f, center.y - 0.12f - wingWave * 0.35f, center.z + 0.08f};
    emitLine(l0, l1, wing); emitLine(l1, l2, wing); emitLine(l2, l0, wing);

    const Vec3 r0{center.x + h, center.y + 0.10f, center.z};
    const Vec3 r1{center.x + 0.82f, center.y + 0.34f - wingWave, center.z - 0.05f};
    const Vec3 r2{center.x + 0.78f, center.y - 0.12f + wingWave * 0.35f, center.z + 0.08f};
    emitLine(r0, r1, wing); emitLine(r1, r2, wing); emitLine(r2, r0, wing);

    // Antennas: 4 edges.
    const Vec3 aL0{center.x - 0.11f, center.y + h, center.z + 0.05f};
    const Vec3 aL1{center.x - 0.18f, center.y + 0.48f, center.z + 0.08f};
    const Vec3 aL2{center.x - 0.28f, center.y + 0.58f, center.z + 0.12f};
    emitLine(aL0, aL1, accent); emitLine(aL1, aL2, accent);

    const Vec3 aR0{center.x + 0.11f, center.y + h, center.z + 0.05f};
    const Vec3 aR1{center.x + 0.18f, center.y + 0.48f, center.z + 0.08f};
    const Vec3 aR2{center.x + 0.28f, center.y + 0.58f, center.z + 0.12f};
    emitLine(aR0, aR1, accent); emitLine(aR1, aR2, accent);

    // Eyes: 2 edges.
    emitLine(Vec3{center.x - 0.10f, center.y + 0.10f, frontZ},
             Vec3{center.x - 0.10f, center.y - 0.01f, frontZ}, face);
    emitLine(Vec3{center.x + 0.10f, center.y + 0.10f, frontZ},
             Vec3{center.x + 0.10f, center.y - 0.01f, frontZ}, face);

    // Smile: 2 edges.
    emitLine(Vec3{center.x - 0.12f, center.y - 0.10f, frontZ},
             Vec3{center.x, center.y - 0.16f, frontZ}, face);
    emitLine(Vec3{center.x, center.y - 0.16f, frontZ},
             Vec3{center.x + 0.12f, center.y - 0.10f, frontZ}, face);

    // Short glowing tail: 2 edges.
    emitLine(Vec3{center.x, center.y, center.z - h},
             Vec3{center.x, center.y + 0.04f, center.z - 0.48f}, accent);
    emitLine(Vec3{center.x, center.y + 0.04f, center.z - 0.48f},
             Vec3{center.x, center.y + 0.10f, center.z - 0.62f}, accent);

    char pad[0x58]{};
    g_renderMesh2(screenContext, tessellator, material, pad);

    if (!g_renderStarted.exchange(true, std::memory_order_relaxed)) {
        FP_LOGI("Flying pet rendering started safely with RenderMeshImmediately2.");
    }
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
        FP_LOGE("Failed to hook NormalTick (code=%d).", result);
        return false;
    }
    g_tickHooked = true;

    result = pl::memory::hook(
        g_renderLevelTarget,
        reinterpret_cast<void*>(&renderLevelHook),
        reinterpret_cast<void**>(&g_renderLevelOriginal));
    if (result != 0) {
        FP_LOGE("Failed to hook RenderLevel (code=%d).", result);
        pl::memory::unhook(g_normalTickTarget, reinterpret_cast<void*>(&normalTickHook));
        g_tickHooked = false;
        g_normalTickOriginal = nullptr;
        return false;
    }
    g_renderHooked = true;

    FP_LOGI("Flying Pet enabled. Safe RenderMeshImmediately2 pipeline active.");
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

    g_playerValid.store(false, std::memory_order_relaxed);
    g_petInitialized.store(false, std::memory_order_relaxed);
    g_tickCounter.store(0, std::memory_order_relaxed);
    g_renderFrames.store(0, std::memory_order_relaxed);
    g_renderStarted.store(false, std::memory_order_relaxed);
    g_loggedMaterialWait.store(false, std::memory_order_relaxed);
}

class FlyingPetMod {
public:
    static FlyingPetMod& instance() {
        static FlyingPetMod mod;
        return mod;
    }

    bool load(pl::mod::ModContext&) {
        FP_LOGI("Flying Pet v0.3.0 loaded.");
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
