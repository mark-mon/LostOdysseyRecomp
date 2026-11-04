#include <stdafx.h>

#include "xr/vr.h"
#include "kernel/memory.h"

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
// Both eyes share the player's FSceneViewState (+0x78; occlusion history), so
// the right eye reuses the left eye's occlusion results from the same frame.

extern "C" PPC_FUNC(__imp__sub_82300158);

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
}

PPC_FUNC(sub_82300158)
{
    const uint32_t player = ctx.r3.u32;
    if (!vr::StereoViewsActive() || !g_memory.base || player < 0x10000)
    {
        __imp__sub_82300158(ctx, base);
        return;
    }
    const float originX = LoadFloat(player + 0x68);
    const float sizeX = LoadFloat(player + 0x70);
    const PPCContext arguments = ctx;

    vr::SetCurrentEye(0);
    StoreFloat(player + 0x70, sizeX * 0.5f);
    __imp__sub_82300158(ctx, base);
    const auto leftResult = ctx.r3;

    ctx = arguments;
    vr::SetCurrentEye(1);
    StoreFloat(player + 0x68, originX + sizeX * 0.5f);
    __imp__sub_82300158(ctx, base);

    vr::SetCurrentEye(-1);
    StoreFloat(player + 0x68, originX);
    StoreFloat(player + 0x70, sizeX);
    ctx.r3 = leftResult;
}
