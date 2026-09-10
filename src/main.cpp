#include <pl/Mod.hpp>
#include <pl/memory/Hook.hpp>

#include <android/log.h>
#include <dlfcn.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace {

constexpr const char* LOG_TAG = "FlyingPet";

#define FP_LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define FP_LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

struct Vec3 {
    float x;
    float y;
    float z;
};

struct BedrockToolsApiV1 {
    std::uint32_t abiVersion;
    std::uint32_t structSize;
    std::uintptr_t (*resolveSignature)(std::uint16_t id);
    void* (*clientInstance)();
    void* subscribe;
    void* unsubscribe;
};

using GetBedrockToolsApi = const BedrockToolsApiV1* (*)(std::uint32_t version);

// Signature IDs from BedrockTools ABI v1.
constexpr std::uint16_t SIG_CLIENT_INSTANCE_GET_LOCAL_PLAYER = 27;
constexpr std::uint16_t SIG_RENDER_LEVEL = 34;
constexpr std::uint16_t SIG_TESSELLATOR_BEGIN = 35;
constexpr std::uint16_t SIG_TESSELLATOR_COLOR = 36;
constexpr std::uint16_t SIG_TESSELLATOR_VERTEX = 37;
constexpr std::uint16_t SIG_RENDER_MESH_IMMEDIATELY = 38;

// Offsets used by the current BedrockTools SDK.
constexpr std::size_t OFF_ACTOR_STATE_VECTOR_COMPONENT = 0x208;
constexpr std::size_t OFF_LEVEL_RENDERER_PLAYER = 0x420;
constexpr std::size_t OFF_CAMERA_POS = 0x61C;
constexpr std::size_t OFF_SELECTION_OVERLAY_MATERIAL = 0x1030;
constexpr std::size_t OFF_SCREEN_CONTEXT_COLOR_HOLDER = 0x30;
constexpr std::size_t OFF_SCREEN_CONTEXT_TESSELLATOR = 0xB8;

using TessellatorBegin = void (*)(void* tessellator, void* debugCallback, int primitiveMode, int vertexCount, int noIndices);
using TessellatorColor = void (*)(void* tessellator, float r, float g, float b, float a);
using TessellatorVertex = void (*)(void* tessellator, float x, float y, float z);
using RenderMeshImmediately = void (*)(void* screenContext, void* tessellator, void* material, char* pad);
using RenderLevel = void (*)(void* self, void* screenContext, void* a3);
using GetLocalPlayer = void* (*)(void* clientInstance);

void* g_bedrockToolsHandle = nullptr;
const BedrockToolsApiV1* g_api = nullptr;

TessellatorBegin g_tessBegin = nullptr;
TessellatorColor g_tessColor = nullptr;
TessellatorVertex g_tessVertex = nullptr;
RenderMeshImmediately g_renderMesh = nullptr;
RenderLevel g_renderLevelOriginal = nullptr;
void* g_renderLevelTarget = nullptr;

bool g_hooked = false;
bool g_petInitialized = false;
Vec3 g_petPos{0.0f, 0.0f, 0.0f};

float elapsedSeconds() {
    static const auto start = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    return std::chrono::duration<float>(now - start).count();
}

float distanceSquared(const Vec3& a, const Vec3& b) {
    const float dx = a.x - b.x;
    const float dy = a.y - b.y;
    const float dz = a.z - b.z;
    return dx * dx + dy * dy + dz * dz;
}

Vec3 lerp(const Vec3& a, const Vec3& b, float t) {
    return {
        a.x + (b.x - a.x) * t,
        a.y + (b.y - a.y) * t,
        a.z + (b.z - a.z) * t,
    };
}

void* getLocalPlayer() {
    if (!g_api || !g_api->clientInstance || !g_api->resolveSignature) return nullptr;

    void* client = g_api->clientInstance();
    if (!client) return nullptr;

    const auto address = g_api->resolveSignature(SIG_CLIENT_INSTANCE_GET_LOCAL_PLAYER);
    if (!address) return nullptr;

    auto getPlayer = reinterpret_cast<GetLocalPlayer>(address);
    return getPlayer(client);
}

bool getPlayerPosition(void* player, Vec3& out) {
    if (!player || reinterpret_cast<std::uintptr_t>(player) < 0x1000) return false;

    const auto actor = reinterpret_cast<std::uintptr_t>(player);
    const auto stateVector = *reinterpret_cast<std::uintptr_t*>(actor + OFF_ACTOR_STATE_VECTOR_COMPONENT);
    if (!stateVector || stateVector < 0x1000) return false;

    out = *reinterpret_cast<Vec3*>(stateVector);
    return std::isfinite(out.x) && std::isfinite(out.y) && std::isfinite(out.z);
}

void updatePet(const Vec3& playerPos) {
    const float time = elapsedSeconds();

    // The pet slowly circles the player, bobs vertically, and follows with smoothing.
    const float orbit = time * 0.72f;
    const Vec3 target{
        playerPos.x + std::cos(orbit) * 1.55f,
        playerPos.y + 1.75f + std::sin(time * 2.4f) * 0.18f,
        playerPos.z + std::sin(orbit) * 1.55f,
    };

    if (!g_petInitialized || distanceSquared(g_petPos, target) > 400.0f) {
        g_petPos = target;
        g_petInitialized = true;
        return;
    }

    const float dist2 = distanceSquared(g_petPos, target);
    const float follow = dist2 > 36.0f ? 0.28f : (dist2 > 9.0f ? 0.18f : 0.095f);
    g_petPos = lerp(g_petPos, target, follow);
}

void drawPet(void* levelRenderer, void* screenContext) {
    if (!levelRenderer || !screenContext || !g_tessBegin || !g_tessColor || !g_tessVertex || !g_renderMesh) return;

    void* player = getLocalPlayer();
    Vec3 playerPos{};
    if (!getPlayerPosition(player, playerPos)) {
        g_petInitialized = false;
        return;
    }

    updatePet(playerPos);

    const auto screen = reinterpret_cast<std::uintptr_t>(screenContext);
    const auto renderer = reinterpret_cast<std::uintptr_t>(levelRenderer);

    const auto tessellatorAddress = *reinterpret_cast<std::uintptr_t*>(screen + OFF_SCREEN_CONTEXT_TESSELLATOR);
    const auto colorHolderAddress = *reinterpret_cast<std::uintptr_t*>(screen + OFF_SCREEN_CONTEXT_COLOR_HOLDER);
    const auto levelRendererPlayer = *reinterpret_cast<std::uintptr_t*>(renderer + OFF_LEVEL_RENDERER_PLAYER);

    if (tessellatorAddress < 0x1000 || colorHolderAddress < 0x1000 || levelRendererPlayer < 0x1000) return;

    void* tessellator = reinterpret_cast<void*>(tessellatorAddress);
    float* colorHolder = reinterpret_cast<float*>(colorHolderAddress);

    const float camX = *reinterpret_cast<float*>(levelRendererPlayer + OFF_CAMERA_POS);
    const float camY = *reinterpret_cast<float*>(levelRendererPlayer + OFF_CAMERA_POS + 4);
    const float camZ = *reinterpret_cast<float*>(levelRendererPlayer + OFF_CAMERA_POS + 8);

    if (!std::isfinite(camX) || !std::isfinite(camY) || !std::isfinite(camZ)) return;

    float savedColor[4] = {colorHolder[0], colorHolder[1], colorHolder[2], colorHolder[3]};
    colorHolder[0] = 1.0f;
    colorHolder[1] = 1.0f;
    colorHolder[2] = 1.0f;
    colorHolder[3] = 1.0f;

    // BedrockTools uses the selection-box material as a safe world-space line material.
    void* material = reinterpret_cast<void*>(levelRendererPlayer + OFF_SELECTION_OVERLAY_MATERIAL);

    const float time = elapsedSeconds();
    const float flap = std::sin(time * 8.0f) * 0.16f;
    const float s = 0.27f;

    auto v = [&](float x, float y, float z) -> Vec3 {
        return {g_petPos.x + x, g_petPos.y + y, g_petPos.z + z};
    };

    // 12 body edges + 6 wing edges + 2 antenna + 8 eye edges + 3 mouth = 31 lines.
    constexpr int LINE_COUNT = 31;
    g_tessBegin(tessellator, nullptr, 4, LINE_COUNT * 2, 0);

    auto emit = [&](const Vec3& a, const Vec3& b, float r, float g, float bl, float alpha = 1.0f) {
        g_tessColor(tessellator, r, g, bl, alpha);
        g_tessVertex(tessellator, a.x - camX, a.y - camY, a.z - camZ);
        g_tessVertex(tessellator, b.x - camX, b.y - camY, b.z - camZ);
    };

    const Vec3 p000 = v(-s, -s, -s);
    const Vec3 p100 = v( s, -s, -s);
    const Vec3 p110 = v( s,  s, -s);
    const Vec3 p010 = v(-s,  s, -s);
    const Vec3 p001 = v(-s, -s,  s);
    const Vec3 p101 = v( s, -s,  s);
    const Vec3 p111 = v( s,  s,  s);
    const Vec3 p011 = v(-s,  s,  s);

    constexpr float br = 1.00f, bg = 0.56f, bb = 0.12f;
    emit(p000, p100, br, bg, bb); emit(p100, p110, br, bg, bb);
    emit(p110, p010, br, bg, bb); emit(p010, p000, br, bg, bb);
    emit(p001, p101, br, bg, bb); emit(p101, p111, br, bg, bb);
    emit(p111, p011, br, bg, bb); emit(p011, p001, br, bg, bb);
    emit(p000, p001, br, bg, bb); emit(p100, p101, br, bg, bb);
    emit(p110, p111, br, bg, bb); emit(p010, p011, br, bg, bb);

    // Wings.
    constexpr float wr = 0.35f, wg = 0.85f, wb = 1.00f;
    const Vec3 leftRoot = v(-s, 0.05f, 0.0f);
    const Vec3 leftTop = v(-0.78f, 0.28f + flap, 0.0f);
    const Vec3 leftBottom = v(-0.68f, -0.18f - flap * 0.35f, 0.0f);
    emit(leftRoot, leftTop, wr, wg, wb); emit(leftTop, leftBottom, wr, wg, wb); emit(leftBottom, leftRoot, wr, wg, wb);

    const Vec3 rightRoot = v(s, 0.05f, 0.0f);
    const Vec3 rightTop = v(0.78f, 0.28f + flap, 0.0f);
    const Vec3 rightBottom = v(0.68f, -0.18f - flap * 0.35f, 0.0f);
    emit(rightRoot, rightTop, wr, wg, wb); emit(rightTop, rightBottom, wr, wg, wb); emit(rightBottom, rightRoot, wr, wg, wb);

    // Antennas.
    emit(v(-0.10f, s, -0.10f), v(-0.18f, 0.52f, -0.14f), br, bg, bb);
    emit(v( 0.10f, s, -0.10f), v( 0.18f, 0.52f, -0.14f), br, bg, bb);

    // Eyes on the -Z face.
    constexpr float er = 1.0f, eg = 1.0f, eb = 1.0f;
    const float zFace = -s - 0.006f;
    auto eye = [&](float cx) {
        const float ex = 0.055f;
        const float ey = 0.070f;
        emit(v(cx - ex, 0.08f - ey, zFace), v(cx + ex, 0.08f - ey, zFace), er, eg, eb);
        emit(v(cx + ex, 0.08f - ey, zFace), v(cx + ex, 0.08f + ey, zFace), er, eg, eb);
        emit(v(cx + ex, 0.08f + ey, zFace), v(cx - ex, 0.08f + ey, zFace), er, eg, eb);
        emit(v(cx - ex, 0.08f + ey, zFace), v(cx - ex, 0.08f - ey, zFace), er, eg, eb);
    };
    eye(-0.11f);
    eye(0.11f);

    // Small smile.
    emit(v(-0.10f, -0.10f, zFace), v(-0.04f, -0.15f, zFace), er, eg, eb);
    emit(v(-0.04f, -0.15f, zFace), v( 0.04f, -0.15f, zFace), er, eg, eb);
    emit(v( 0.04f, -0.15f, zFace), v( 0.10f, -0.10f, zFace), er, eg, eb);

    char pad[0x58]{};
    g_renderMesh(screenContext, tessellator, material, pad);

    colorHolder[0] = savedColor[0];
    colorHolder[1] = savedColor[1];
    colorHolder[2] = savedColor[2];
    colorHolder[3] = savedColor[3];
}

void renderLevelHook(void* self, void* screenContext, void* a3) {
    if (g_renderLevelOriginal) g_renderLevelOriginal(self, screenContext, a3);
    drawPet(self, screenContext);
}

bool connectBedrockTools() {
    g_bedrockToolsHandle = dlopen("libBedrockTools.so", RTLD_NOW | RTLD_NOLOAD);
    if (!g_bedrockToolsHandle) {
        FP_LOGE("BedrockTools is not loaded. Enable BedrockTools before Flying Pet.");
        return false;
    }

    auto getApi = reinterpret_cast<GetBedrockToolsApi>(dlsym(g_bedrockToolsHandle, "BedrockTools_GetApi"));
    if (!getApi) {
        FP_LOGE("BedrockTools_GetApi was not found.");
        return false;
    }

    g_api = getApi(1);
    if (!g_api || g_api->abiVersion != 1 || !g_api->resolveSignature || !g_api->clientInstance) {
        FP_LOGE("Incompatible BedrockTools API.");
        return false;
    }

    const auto renderLevel = g_api->resolveSignature(SIG_RENDER_LEVEL);
    const auto tessBegin = g_api->resolveSignature(SIG_TESSELLATOR_BEGIN);
    const auto tessColor = g_api->resolveSignature(SIG_TESSELLATOR_COLOR);
    const auto tessVertex = g_api->resolveSignature(SIG_TESSELLATOR_VERTEX);
    const auto renderMesh = g_api->resolveSignature(SIG_RENDER_MESH_IMMEDIATELY);

    if (!renderLevel || !tessBegin || !tessColor || !tessVertex || !renderMesh) {
        FP_LOGE("One or more BedrockTools signatures required by Flying Pet are unavailable.");
        return false;
    }

    g_renderLevelTarget = reinterpret_cast<void*>(renderLevel);
    g_tessBegin = reinterpret_cast<TessellatorBegin>(tessBegin);
    g_tessColor = reinterpret_cast<TessellatorColor>(tessColor);
    g_tessVertex = reinterpret_cast<TessellatorVertex>(tessVertex);
    g_renderMesh = reinterpret_cast<RenderMeshImmediately>(renderMesh);
    return true;
}

bool installHook() {
    if (g_hooked) return true;
    if (!g_renderLevelTarget) return false;

    if (pl::memory::hook(g_renderLevelTarget, reinterpret_cast<void*>(&renderLevelHook), reinterpret_cast<void**>(&g_renderLevelOriginal)) != 0) {
        FP_LOGE("Failed to hook RenderLevel.");
        return false;
    }

    g_hooked = true;
    FP_LOGI("Flying Pet enabled.");
    return true;
}

void removeHook() {
    if (!g_hooked || !g_renderLevelTarget) return;
    pl::memory::unhook(g_renderLevelTarget, reinterpret_cast<void*>(&renderLevelHook));
    g_hooked = false;
    g_renderLevelOriginal = nullptr;
    g_petInitialized = false;
}

class FlyingPetMod {
public:
    static FlyingPetMod& instance() {
        static FlyingPetMod mod;
        return mod;
    }

    bool load(pl::mod::ModContext&) {
        FP_LOGI("Flying Pet loaded.");
        return true;
    }

    bool enable(pl::mod::ModContext&) {
        if (!connectBedrockTools()) return false;
        return installHook();
    }

    bool disable(pl::mod::ModContext&) {
        removeHook();
        return true;
    }

    bool unload(pl::mod::ModContext&) {
        removeHook();
        g_api = nullptr;
        if (g_bedrockToolsHandle) {
            dlclose(g_bedrockToolsHandle);
            g_bedrockToolsHandle = nullptr;
        }
        return true;
    }
};

} // namespace

PL_REGISTER_MOD(FlyingPetMod, FlyingPetMod::instance())
