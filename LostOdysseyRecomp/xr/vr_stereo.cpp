#include <stdafx.h>

#include "xr/vr.h"
#include "kernel/memory.h"
#include <os/logger.h>

// Native stereo through UE3's own split-screen path.
//
// ULocalPlayer::CalcSceneView (sub_82300158; r3 player, r4 view family, r5/r6
// out location/rotation, r7 viewport) builds the player's FSceneView from the
// player's split-screen rectangle: Origin (player+0x68/+0x6C) and Size
// (+0x70/+0x74) as fractions of the viewport, and adds it to the family's Views
// array. Calling it once per eye with the left and right halves gives the
// family two views; UE3's scene renderer then draws the scene for both eyes in
// the same frame (shadows and scene traversal shared, culling per view), the
// way it renders split-screen. The FSceneView hook (vr::AdjustMainView) gives
// each view its eye's pose and frustum.
//
// The right eye is built without the player's FSceneViewState (+0x78). Sharing
// one state between both views corrupts the post-processing history (the scene
// colours came out false-coloured in both eyes); with no state the right eye
// only loses its occlusion history. TODO: give the right eye a state of its own.

extern "C" PPC_FUNC(__imp__sub_82300158);
extern "C" PPC_FUNC(__imp__sub_82486C88);

namespace
{
float LoadFloat(uint32_t address)
{
    return std::bit_cast<float>(__builtin_bswap32(*reinterpret_cast<const uint32_t*>(g_memory.base + address)));
}
void StoreFloat(uint32_t address, float value)
{
    *reinterpret_cast<uint32_t*>(g_memory.base + address) = __builtin_bswap32(std::bit_cast<uint32_t>(value));
}
uint32_t LoadWord(uint32_t address)
{
    return __builtin_bswap32(*reinterpret_cast<const uint32_t*>(g_memory.base + address));
}
void StoreWord(uint32_t address, uint32_t value)
{
    *reinterpret_cast<uint32_t*>(g_memory.base + address) = __builtin_bswap32(value);
}
}

namespace
{
// Developer probe (LO_VR_ALLOC_PROBE=1): remember recent guest allocations so the
// player's FSceneViewState can be traced back to the code that allocated it.
struct Allocation { uint32_t address, size, caller; };
constexpr uint32_t AllocationRing = 1u << 20;
std::array<Allocation, AllocationRing> g_allocations{};
std::atomic<uint32_t> g_allocationCount{0};
bool AllocationProbe()
{
    static const bool enabled = [] { const char* v = getenv("LO_VR_ALLOC_PROBE"); return v && v[0] == '1'; }();
    return enabled;
}
}

// Guest operator new / appMalloc (r3 size; returns r3). Only records when the probe is on.
PPC_FUNC(sub_82486C88)
{
    const uint32_t size = ctx.r3.u32, caller = uint32_t(ctx.lr);
    __imp__sub_82486C88(ctx, base);
    if (AllocationProbe()) {
        const uint32_t i = g_allocationCount.fetch_add(1, std::memory_order_relaxed);
        g_allocations[i % AllocationRing] = {ctx.r3.u32, size, caller};
    }
}

PPC_FUNC(sub_82300158)
{
    const uint32_t player = ctx.r3.u32;
    if (!vr::StereoViewsActive() || !g_memory.base || player < 0x10000)
    {
        __imp__sub_82300158(ctx, base);
        return;
    }
    // LO_VR_STEREO_TEST (developer): "left" builds only the left-eye view (half rectangle);
    // "shared" lets the right eye share the player's FSceneViewState (shows the colour corruption).
    static const std::string test = [] { const char* v = getenv("LO_VR_STEREO_TEST"); return std::string(v ? v : ""); }();
    {
        // One-time facts for giving the right eye a view state of its own.
        static bool logged = false;
        const uint32_t state = LoadWord(player + 0x78);
        if (!logged && state > 0x10000) {
            logged = true;
            LOG_NOTICE("vr: player {:08x} view state {:08x} words {:08x} {:08x} {:08x} {:08x}", player, state,
                LoadWord(state), LoadWord(state + 4), LoadWord(state + 8), LoadWord(state + 12));
            if (AllocationProbe()) {
                const uint32_t count = std::min(g_allocationCount.load(), AllocationRing);
                bool found = false;
                for (uint32_t i = 0; i < count; ++i) {
                    const auto& a = g_allocations[i];
                    if (a.address && state >= a.address && state < a.address + a.size) {
                        LOG_NOTICE("vr: view state allocation {:08x} size {:#x} caller {:08x} (state offset {:#x})",
                            a.address, a.size, a.caller, state - a.address);
                        found = true;
                    }
                }
                if (!found) LOG_NOTICE("vr: view state not in the last {} allocations", count);
            }
        }
    }
    const float originX = LoadFloat(player + 0x68);
    const float sizeX = LoadFloat(player + 0x70);
    const PPCContext arguments = ctx;

    vr::SetCurrentEye(0);
    StoreFloat(player + 0x70, sizeX * 0.5f);
    __imp__sub_82300158(ctx, base);
    const auto leftResult = ctx.r3;

    if (test != "left")
    {
        ctx = arguments;
        vr::SetCurrentEye(1);
        StoreFloat(player + 0x68, originX + sizeX * 0.5f);
        const uint32_t viewState = LoadWord(player + 0x78);
        if (test != "shared") StoreWord(player + 0x78, 0);
        __imp__sub_82300158(ctx, base);
        StoreWord(player + 0x78, viewState);
    }

    vr::SetCurrentEye(-1);
    StoreFloat(player + 0x68, originX);
    StoreFloat(player + 0x70, sizeX);
    ctx.r3 = leftResult;
}
