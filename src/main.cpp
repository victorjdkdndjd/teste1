#include <pl/Mod.hpp>
#include <pl/memory/Hook.hpp>
#include <pl/memory/Signature.hpp>

#include <android/log.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace {

constexpr const char* LOG_TAG = "FlyingPet";
constexpr std::string_view MC_MODULE = "libminecraftpe.so";

#define FP_LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define FP_LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

struct Vec3 { float x, y, z; };

constexpr std::size_t OFF_ACTOR_STATE_VECTOR_COMPONENT = 0x208;
constexpr std::size_t OFF_LEVEL_RENDERER_PLAYER = 0x420;
constexpr std::size_t OFF_CAMERA_POS = 0x61C;
constexpr std::size_t OFF_SELECTION_OVERLAY_MATERIAL = 0x1030;
constexpr std::size_t OFF_SCREEN_CONTEXT_COLOR_HOLDER = 0x30;
constexpr std::size_t OFF_SCREEN_CONTEXT_TESSELLATOR = 0xB8;

constexpr std::string_view SIG_CLIENT_INSTANCE_UPDATE =
    "? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 FD 03 00 91 ? ? ? D1 59 D0 3B D5 F3 03 00 AA F4 03 01 2A ? ? ? F9 ? ? ? F8 ? ? ? F9 ? ? ? F9";
constexpr std::string_view SIG_GET_LOCAL_PLAYER =
    "? ? ? D1 ? ? ? A9 ? ? ? F9 ? ? ? 91 53 D0 3B D5 E8 03 00 AA ? ? ? 91 ? ? ? F9 ? ? ? 91 ? ? ? F8 ? ? ? 95 ? ? ? 91 ? ? ? 95 ? ? ? 36 ? ? ? 91 ? ? ? 52 ? ? ? 94";
constexpr std::string_view SIG_RENDER_LEVEL =
    "? ? ? FC ? ? ? 6D ? ? ? 6D ? ? ? 6D ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? 91 ? ? ? D1 57 D0 3B D5";
constexpr std::string_view SIG_TESSELLATOR_BEGIN =
    "? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 FD 03 00 91 ? ? ? 39 ? ? ? 39 08 01 09 2A";
constexpr std::string_view SIG_TESSELLATOR_COLOR =
    "? ? ? 52 ? ? ? 39 04 01 27 1E";
constexpr std::string_view SIG_TESSELLATOR_VERTEX =
    "? ? ? D1 ? ? ? FD ? ? ? 6D ? ? ? A9 ? ? ? F9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? 91 58 D0 3B D5 ? ? ? F9";
constexpr std::string_view SIG_RENDER_MESH =
    "? ? ? A9 ? ? ? F9 ? ? ? A9 ? ? ? A9 ? ? ? A9 FD 03 00 91 ? ? ? D1 58 D0 3B D5 F7 03 00 AA E0 03 01 AA ? ? ? F9 F4 03 04 AA";

using ClientInstanceUpdate = void* (*)(void*, bool);
using GetLocalPlayer = void* (*)(void*);
using RenderLevel = void (*)(void*, void*, void*);
using TessellatorBegin = void (*)(void*, void*, int, int, int);
using TessellatorColor = void (*)(void*, float, float, float, float);
using TessellatorVertex = void (*)(void*, float, float, float);
using RenderMeshImmediately = void (*)(void*, void*, void*, char*);

std::atomic<void*> g_clientInstance{nullptr};
ClientInstanceUpdate g_clientUpdateOriginal = nullptr;
GetLocalPlayer g_getLocalPlayer = nullptr;
RenderLevel g_renderLevelOriginal = nullptr;
TessellatorBegin g_tessBegin = nullptr;
TessellatorColor g_tessColor = nullptr;
TessellatorVertex g_tessVertex = nullptr;
RenderMeshImmediately g_renderMesh = nullptr;

void* g_clientUpdateTarget = nullptr;
void* g_renderLevelTarget = nullptr;
bool g_clientHooked = false;
bool g_renderHooked = false;
bool g_petInitialized = false;
Vec3 g_petPos{0.0f, 0.0f, 0.0f};

float elapsedSeconds() {
    static const auto start = std::chrono::steady_clock::now();
    return std::chrono::duration<float>(std::chrono::steady_clock::now() - start).count();
}

float distanceSquared(const Vec3& a, const Vec3& b) {
    const float dx = a.x - b.x;
    const float dy = a.y - b.y;
    const float dz = a.z - b.z;
    return dx * dx + dy * dy + dz * dz;
}

Vec3 lerp(const Vec3& a, const Vec3& b, float t) {
    return {a.x + (b.x - a.x) * t,
            a.y + (b.y - a.y) * t,
            a.z + (b.z - a.z) * t};
}

std::uintptr_t resolve(std::string_view name, std::string_view signature) {
    const auto address = pl::memory::resolveSignature(signature, MC_MODULE);
    if (!address) {
        FP_LOGE("Signature not found: %.*s", static_cast<int>(name.size()), name.data());
    } else {
        FP_LOGI("Resolved %.*s at %p", static_cast<int>(name.size()), name.data(), reinterpret_cast<void*>(address));
    }
    return address;
}

bool resolveMinecraftRuntime() {
    const auto clientUpdate = resolve("ClientInstanceUpdate", SIG_CLIENT_INSTANCE_UPDATE);
    const auto getLocalPlayer = resolve("ClientInstanceGetLocalPlayer", SIG_GET_LOCAL_PLAYER);
    const auto renderLevel = resolve("RenderLevel", SIG_RENDER_LEVEL);
    const auto tessBegin = resolve("TessellatorBegin", SIG_TESSELLATOR_BEGIN);
    const auto tessColor = resolve("TessellatorColor", SIG_TESSELLATOR_COLOR);
    const auto tessVertex = resolve("TessellatorVertex", SIG_TESSELLATOR_VERTEX);
    const auto renderMesh = resolve("MeshHelpersRenderMeshImmediately", SIG_RENDER_MESH);

    if (!clientUpdate || !getLocalPlayer || !renderLevel || !tessBegin || !tessColor || !tessVertex || !renderMesh) {
        FP_LOGE("Flying Pet cannot start: one or more Minecraft signatures are unavailable.");
        return false;
    }

    g_clientUpdateTarget = reinterpret_cast<void*>(clientUpdate);
    g_renderLevelTarget = reinterpret_cast<void*>(renderLevel);
    g_getLocalPlayer = reinterpret_cast<GetLocalPlayer>(getLocalPlayer);
    g_tessBegin = reinterpret_cast<TessellatorBegin>(tessBegin);
    g_tessColor = reinterpret_cast<TessellatorColor>(tessColor);
    g_tessVertex = reinterpret_cast<TessellatorVertex>(tessVertex);
    g_renderMesh = reinterpret_cast<RenderMeshImmediately>(renderMesh);
    return true;
}

void* getLocalPlayer() {
    void* client = g_clientInstance.load(std::memory_order_acquire);
    if (!client || !g_getLocalPlayer) return nullptr;
    return g_getLocalPlayer(client);
}

bool getPlayerPosition(void* player, Vec3& out) {
    if (!player || reinterpret_cast<std::uintptr_t>(player) < 0x1000) return false;
    const auto actor = reinterpret_cast<std::uintptr_t>(player);
    const auto stateVector = *reinterpret_cast<std::uintptr_t*>(actor + OFF_ACTOR_STATE_VECTOR_COMPONENT);
    if (stateVector < 0x1000) return false;
    out = *reinterpret_cast<Vec3*>(stateVector);
    return std::isfinite(out.x) && std::isfinite(out.y) && std::isfinite(out.z);
}

void updatePet(const Vec3& playerPos) {
    const float time = elapsedSeconds();
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

    const float d2 = distanceSquared(g_petPos, target);
    const float follow = d2 > 36.0f ? 0.28f : (d2 > 9.0f ? 0.18f : 0.095f);
    g_petPos = lerp(g_petPos, target, follow);
}

void drawPet(void* levelRenderer, void* screenContext) {
    if (!levelRenderer || !screenContext || !g_tessBegin || !g_tessColor || !g_tessVertex || !g_renderMesh) return;

    Vec3 playerPos{};
    if (!getPlayerPosition(getLocalPlayer(), playerPos)) {
        g_petInitialized = false;
        return;
    }
    updatePet(playerPos);

    const auto screen = reinterpret_cast<std::uintptr_t>(screenContext);
    const auto renderer = reinterpret_cast<std::uintptr_t>(levelRenderer);
    const auto tessAddress = *reinterpret_cast<std::uintptr_t*>(screen + OFF_SCREEN_CONTEXT_TESSELLATOR);
    const auto colorAddress = *reinterpret_cast<std::uintptr_t*>(screen + OFF_SCREEN_CONTEXT_COLOR_HOLDER);
    const auto lrp = *reinterpret_cast<std::uintptr_t*>(renderer + OFF_LEVEL_RENDERER_PLAYER);
    if (tessAddress < 0x1000 || colorAddress < 0x1000 || lrp < 0x1000) return;

    void* tessellator = reinterpret_cast<void*>(tessAddress);
    float* colorHolder = reinterpret_cast<float*>(colorAddress);
    const float camX = *reinterpret_cast<float*>(lrp + OFF_CAMERA_POS);
    const float camY = *reinterpret_cast<float*>(lrp + OFF_CAMERA_POS + 4);
    const float camZ = *reinterpret_cast<float*>(lrp + OFF_CAMERA_POS + 8);
    if (!std::isfinite(camX) || !std::isfinite(camY) || !std::isfinite(camZ)) return;

    const float savedColor[4] = {colorHolder[0], colorHolder[1], colorHolder[2], colorHolder[3]};
    colorHolder[0] = colorHolder[1] = colorHolder[2] = colorHolder[3] = 1.0f;
    void* material = reinterpret_cast<void*>(lrp + OFF_SELECTION_OVERLAY_MATERIAL);

    const float time = elapsedSeconds();
    const float flap = std::sin(time * 8.0f) * 0.16f;
    const float s = 0.27f;
    auto v = [&](float x, float y, float z) -> Vec3 {
        return {g_petPos.x + x, g_petPos.y + y, g_petPos.z + z};
    };

    constexpr int LINE_COUNT = 31;
    g_tessBegin(tessellator, nullptr, 4, LINE_COUNT * 2, 0);
    auto emit = [&](const Vec3& a, const Vec3& b, float r, float g, float bl, float alpha = 1.0f) {
        g_tessColor(tessellator, r, g, bl, alpha);
        g_tessVertex(tessellator, a.x - camX, a.y - camY, a.z - camZ);
        g_tessVertex(tessellator, b.x - camX, b.y - camY, b.z - camZ);
    };

    const Vec3 p000=v(-s,-s,-s), p100=v(s,-s,-s), p110=v(s,s,-s), p010=v(-s,s,-s);
    const Vec3 p001=v(-s,-s,s),  p101=v(s,-s,s),  p111=v(s,s,s),  p011=v(-s,s,s);
    constexpr float br=1.00f,bg=0.56f,bb=0.12f;
    emit(p000,p100,br,bg,bb); emit(p100,p110,br,bg,bb); emit(p110,p010,br,bg,bb); emit(p010,p000,br,bg,bb);
    emit(p001,p101,br,bg,bb); emit(p101,p111,br,bg,bb); emit(p111,p011,br,bg,bb); emit(p011,p001,br,bg,bb);
    emit(p000,p001,br,bg,bb); emit(p100,p101,br,bg,bb); emit(p110,p111,br,bg,bb); emit(p010,p011,br,bg,bb);

    constexpr float wr=0.35f,wg=0.85f,wb=1.00f;
    const Vec3 leftRoot=v(-s,0.05f,0), leftTop=v(-0.78f,0.28f+flap,0), leftBottom=v(-0.68f,-0.18f-flap*0.35f,0);
    emit(leftRoot,leftTop,wr,wg,wb); emit(leftTop,leftBottom,wr,wg,wb); emit(leftBottom,leftRoot,wr,wg,wb);
    const Vec3 rightRoot=v(s,0.05f,0), rightTop=v(0.78f,0.28f+flap,0), rightBottom=v(0.68f,-0.18f-flap*0.35f,0);
    emit(rightRoot,rightTop,wr,wg,wb); emit(rightTop,rightBottom,wr,wg,wb); emit(rightBottom,rightRoot,wr,wg,wb);

    emit(v(-0.10f,s,-0.10f),v(-0.18f,0.52f,-0.14f),br,bg,bb);
    emit(v(0.10f,s,-0.10f),v(0.18f,0.52f,-0.14f),br,bg,bb);

    constexpr float er=1.0f,eg=1.0f,eb=1.0f;
    const float zFace=-s-0.006f;
    auto eye=[&](float cx){
        const float ex=0.055f, ey=0.070f;
        emit(v(cx-ex,0.08f-ey,zFace),v(cx+ex,0.08f-ey,zFace),er,eg,eb);
        emit(v(cx+ex,0.08f-ey,zFace),v(cx+ex,0.08f+ey,zFace),er,eg,eb);
        emit(v(cx+ex,0.08f+ey,zFace),v(cx-ex,0.08f+ey,zFace),er,eg,eb);
        emit(v(cx-ex,0.08f+ey,zFace),v(cx-ex,0.08f-ey,zFace),er,eg,eb);
    };
    eye(-0.11f); eye(0.11f);
    emit(v(-0.10f,-0.10f,zFace),v(-0.04f,-0.15f,zFace),er,eg,eb);
    emit(v(-0.04f,-0.15f,zFace),v(0.04f,-0.15f,zFace),er,eg,eb);
    emit(v(0.04f,-0.15f,zFace),v(0.10f,-0.10f,zFace),er,eg,eb);

    char pad[0x58]{};
    g_renderMesh(screenContext, tessellator, material, pad);
    colorHolder[0]=savedColor[0]; colorHolder[1]=savedColor[1]; colorHolder[2]=savedColor[2]; colorHolder[3]=savedColor[3];
}

void* clientUpdateHook(void* clientInstance, bool value) {
    if (clientInstance) g_clientInstance.store(clientInstance, std::memory_order_release);
    return g_clientUpdateOriginal ? g_clientUpdateOriginal(clientInstance, value) : nullptr;
}

void renderLevelHook(void* self, void* screenContext, void* a3) {
    if (g_renderLevelOriginal) g_renderLevelOriginal(self, screenContext, a3);
    drawPet(self, screenContext);
}

bool installHooks() {
    if (pl::memory::hook(g_clientUpdateTarget,
                         reinterpret_cast<void*>(&clientUpdateHook),
                         reinterpret_cast<void**>(&g_clientUpdateOriginal)) != 0) {
        FP_LOGE("Failed to hook ClientInstanceUpdate.");
        return false;
    }
    g_clientHooked = true;

    if (pl::memory::hook(g_renderLevelTarget,
                         reinterpret_cast<void*>(&renderLevelHook),
                         reinterpret_cast<void**>(&g_renderLevelOriginal)) != 0) {
        FP_LOGE("Failed to hook RenderLevel.");
        pl::memory::unhook(g_clientUpdateTarget, reinterpret_cast<void*>(&clientUpdateHook));
        g_clientHooked = false;
        g_clientUpdateOriginal = nullptr;
        return false;
    }
    g_renderHooked = true;
    FP_LOGI("Flying Pet standalone enabled. BedrockTools is not required.");
    return true;
}

void removeHooks() {
    if (g_renderHooked && g_renderLevelTarget) {
        pl::memory::unhook(g_renderLevelTarget, reinterpret_cast<void*>(&renderLevelHook));
    }
    if (g_clientHooked && g_clientUpdateTarget) {
        pl::memory::unhook(g_clientUpdateTarget, reinterpret_cast<void*>(&clientUpdateHook));
    }
    g_renderHooked = false;
    g_clientHooked = false;
    g_renderLevelOriginal = nullptr;
    g_clientUpdateOriginal = nullptr;
    g_clientInstance.store(nullptr, std::memory_order_release);
    g_petInitialized = false;
}

class FlyingPetMod {
public:
    static FlyingPetMod& instance() {
        static FlyingPetMod mod;
        return mod;
    }

    bool load(pl::mod::ModContext&) {
        FP_LOGI("Flying Pet standalone loaded.");
        return true;
    }

    bool enable(pl::mod::ModContext&) {
        if (!resolveMinecraftRuntime()) return false;
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
