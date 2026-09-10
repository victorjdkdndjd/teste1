#include <pl/Mod.hpp>
#include <pl/memory/Hook.hpp>
#include <pl/memory/Signature.hpp>

#include <android/log.h>

#include <atomic>
#include <cstdint>
#include <string_view>

namespace {

constexpr const char* LOG_TAG = "FlyingPet";
constexpr std::string_view MC_MODULE = "libminecraftpe.so";

#define FP_LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define FP_LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// Diagnostic A deliberately resolves and hooks ONLY NormalTick.
// There is no RenderLevel/Tessellator hook and no Actor memory dereference.
constexpr std::string_view SIG_NORMAL_TICK =
    "? ? ? FC ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? 91 ? ? ? D1 54 D0 3B D5 F3 03 00 AA ? ? ? F9 ? ? ? F8 ? ? ? 39";

using NormalTick = void (*)(void*);

NormalTick g_normalTickOriginal = nullptr;
void* g_normalTickTarget = nullptr;
bool g_tickHooked = false;
std::atomic<bool> g_firstTickSeen{false};
std::atomic<std::uint64_t> g_tickCount{0};

void normalTickHook(void* actor) {
    // Preserve Minecraft behavior first. Do not inspect or dereference actor.
    if (g_normalTickOriginal) {
        g_normalTickOriginal(actor);
    }

    const auto count = g_tickCount.fetch_add(1, std::memory_order_relaxed) + 1;
    if (!g_firstTickSeen.exchange(true, std::memory_order_relaxed)) {
        FP_LOGI("Diagnostic A: first NormalTick reached safely. actor=%p", actor);
    }

    // Very low-rate heartbeat to prove the hook remains alive without flooding Logcat.
    if ((count % 1200) == 0) {
        FP_LOGI("Diagnostic A: NormalTick heartbeat count=%llu",
                static_cast<unsigned long long>(count));
    }
}

bool resolveRuntime() {
    const auto address = pl::memory::resolveSignature(SIG_NORMAL_TICK, MC_MODULE);
    if (!address) {
        FP_LOGE("Diagnostic A: NormalTick signature not found.");
        return false;
    }

    g_normalTickTarget = reinterpret_cast<void*>(address);
    FP_LOGI("Diagnostic A: NormalTick resolved at %p", g_normalTickTarget);
    return true;
}

bool installHook() {
    if (!g_normalTickTarget) return false;

    const int result = pl::memory::hook(
        g_normalTickTarget,
        reinterpret_cast<void*>(&normalTickHook),
        reinterpret_cast<void**>(&g_normalTickOriginal));

    if (result != 0) {
        FP_LOGE("Diagnostic A: failed to hook NormalTick (code=%d).", result);
        return false;
    }

    g_tickHooked = true;
    FP_LOGI("Diagnostic A enabled: NO rendering, NO Player memory reads.");
    return true;
}

void removeHook() {
    if (g_tickHooked && g_normalTickTarget) {
        pl::memory::unhook(g_normalTickTarget, reinterpret_cast<void*>(&normalTickHook));
    }

    g_tickHooked = false;
    g_normalTickOriginal = nullptr;
    g_normalTickTarget = nullptr;
    g_firstTickSeen.store(false, std::memory_order_relaxed);
    g_tickCount.store(0, std::memory_order_relaxed);
}

class FlyingPetMod {
public:
    static FlyingPetMod& instance() {
        static FlyingPetMod mod;
        return mod;
    }

    bool load(pl::mod::ModContext&) {
        FP_LOGI("Flying Pet v0.2.2 Diagnostic A loaded.");
        return true;
    }

    bool enable(pl::mod::ModContext&) {
        if (!resolveRuntime()) return false;
        return installHook();
    }

    bool disable(pl::mod::ModContext&) {
        removeHook();
        return true;
    }

    bool unload(pl::mod::ModContext&) {
        removeHook();
        return true;
    }
};

} // namespace

PL_REGISTER_MOD(FlyingPetMod, FlyingPetMod::instance())
