#include <stdafx.h>

#include "xr/vr.h"
#include "kernel/memory.h"
#include "gpu/frame_plan.h"
#include "gpu/vr_clip.h"
#include <os/logger.h>
#include <bit>
#include <set>
#include <array>
#include <map>
#include <debug/battle_menu.h>
#include <chrono>

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
// FSceneView arguments from the player: +0x78 is the post-process chain (a
// UObject), +0x7C the FSceneViewState, +0x80 the actor visibility history, +0x88
// the post-process settings. The right eye gets a view state of its own from the
// game's AllocateViewState (sub_825B0CB8: new FSceneViewState, constructor sub_825AF048)
// and shares the player's post-process chain. Its post-processing passes set the scissor
// from the view as (X, Y, SizeX, SizeY), so renderer.cpp reads an empty scissor of a view
// off the origin as origin + size (VR stereo only).

extern "C" PPC_FUNC(__imp__sub_82300158);
extern "C" PPC_FUNC(__imp__sub_82486C88);
// sub_825B0CB8 (AllocateViewState) is declared by ppc_recomp_shared.h.

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

std::atomic<uint32_t> g_leftView{0}, g_rightView{0};  // last views built per eye (post probe)

// Movies (FLO_MOVIE post-process proxy) and comic-panel cutscenes are 2D pictures, not scenes to
// split per eye. Comic-panel cutscenes: the game builds extra views (sub_82307378, caller 0x82307BEC) for panels
// it composites over the player's view in screen space, which can't be split per eye. While it does,
// the player's view is built whole (as without VR) and the frame is shown flat (vr::FlatFrameBit).
uint32_t g_drawCounter = 0, g_panelSeenAt = 0;
bool g_panelSeen = false, g_flatFrame = false;
uint32_t g_flatSerial = 0;
namespace
{
void NoteFlatCause(const char* cause)
{
    if (!vr::StereoViewsActive()) return;
    // Log each start of a flat stretch (not every frame of it).
    if (!g_panelSeen || g_drawCounter - g_panelSeenAt > 3) LOG_NOTICE("vr: {}: showing the frame flat", cause);
    g_panelSeen = true;
    g_panelSeenAt = g_drawCounter;
}
}
std::atomic<int64_t> g_battleRequestedMs{0};
int64_t NowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
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
    // "nochain" drops the post-process chain on the right eye (no post-processing there); "sharedstate"
    // shares the player's view state; "shared" shares both; "nostate" gives the right eye no state.
    static const std::string test = [] { const char* v = getenv("LO_VR_STEREO_TEST"); return std::string(v ? v : ""); }();
    {
        // One-time facts for giving the right eye a view state of its own.
        static bool logged = false;
        const uint32_t state = LoadWord(player + 0x7C);
        if (!logged && state > 0x10000) {
            logged = true;
            LOG_NOTICE("vr: player {:08x} view state {:08x} words {:08x} {:08x} {:08x} {:08x} chain {:08x}", player, state,
                LoadWord(state), LoadWord(state + 4), LoadWord(state + 8), LoadWord(state + 12), LoadWord(player + 0x78));
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
    // Comic panels in the last few frames: one whole view, shown flat (LO_VR_PANEL_FLAT=0 disables).
    static const bool panelFlat = [] { const char* v = getenv("LO_VR_PANEL_FLAT"); return !v || v[0] != '0'; }();
    ++g_drawCounter;
    // Battle transition: the warp (the field picture, grabbed whole and drawn into each eye) is shown flat
    // from the request for LO_VR_BATTLE_FLAT_MS (default 3100; 0: until the battle core runs);
    // LO_VR_BATTLE_FLAT=0 keeps it in VR. Times are logged (F10 logs a mark relative to the last request).
    static const char* battleFlatEnv = getenv("LO_VR_BATTLE_FLAT");
    // The warp is a fixed animation (timed to its sound): marked with F10 at 3082 and 3275 ms after the
    // request (reaction time included); the battle core only runs ~8250 ms after it, past the opening shot.
    static const int64_t battleFlatMs = [] { const char* v = getenv("LO_VR_BATTLE_FLAT_MS"); return v ? int64_t(atoll(v)) : int64_t(3100); }();
    const bool battleFlat = !battleFlatEnv || battleFlatEnv[0] != '0';
    if (const int64_t requested = g_battleRequestedMs.load(std::memory_order_relaxed); requested) {
        const int64_t elapsed = NowMs() - requested;
        if (debug_menu::CurrentBattle() != 0 || elapsed > 10000) {
            g_battleRequestedMs.store(0, std::memory_order_relaxed);
            LOG_NOTICE("vr: battle running {} ms after the request", elapsed);
        } else if (battleFlat && (battleFlatMs <= 0 || elapsed < battleFlatMs)) {
            NoteFlatCause("battle transition");
        }
    }
    g_flatFrame = panelFlat && g_panelSeen && g_drawCounter - g_panelSeenAt <= 3;
    if (g_flatFrame) {
        __imp__sub_82300158(ctx, base);
        return;
    }
    // A cinematic camera constrains the view to its aspect ratio (PlayerCamera+0x214 bit 0x40000000,
    // tested at 0x82300660): for an eye's half it letterboxes the view to 640x360 at Y 180, which the
    // post-processing doesn't handle (black). The headset frustum replaces the projection anyway, so
    // the eyes' views are built without it (LO_VR_KEEP_CONSTRAINED_ASPECT=1 keeps it).
    static const bool keepConstraint = [] { const char* v = getenv("LO_VR_KEEP_CONSTRAINED_ASPECT"); return v && v[0] == '1'; }();
    uint32_t cameraFlags = 0, savedCameraFlags = 0;
    if (!keepConstraint) {
        const uint32_t controller = LoadWord(player + 0x40);
        const uint32_t camera = controller > 0x10000 ? LoadWord(controller + 0x308) : 0;
        if (camera > 0x10000) {
            cameraFlags = camera + 0x214;
            savedCameraFlags = LoadWord(cameraFlags);
            if (savedCameraFlags & 0x40000000u) StoreWord(cameraFlags, savedCameraFlags & ~0x40000000u);
            else cameraFlags = 0;
        }
    }
    const float originX = LoadFloat(player + 0x68);
    const float sizeX = LoadFloat(player + 0x70);
    const PPCContext arguments = ctx;
    // LO_VR_VIEW_LOG=1 (developer): log each player's split rectangle and the views built, on change.
    static const bool viewLog = getenv("LO_VR_VIEW_LOG") != nullptr;
    auto logViews = [&](uint32_t left, uint32_t right) {
        if (!viewLog) return;
        static std::map<uint32_t, std::array<float, 12>> last;
        std::array<float, 12> now{originX, LoadFloat(player + 0x6C), sizeX, LoadFloat(player + 0x74)};
        for (int i = 0; i < 4; ++i) now[4 + i] = left > 0x10000 ? LoadFloat(left + 0x1C + 4 * i) : -1.0f;
        for (int i = 0; i < 4; ++i) now[8 + i] = right > 0x10000 ? LoadFloat(right + 0x1C + 4 * i) : -1.0f;
        auto& was = last[player];
        if (was == now) return;
        was = now;
        LOG_NOTICE("vr: views player {:08x} split ({:.3f},{:.3f} {:.3f}x{:.3f}) left ({},{} {}x{}) right ({},{} {}x{})", player,
            now[0], now[1], now[2], now[3], now[4], now[5], now[6], now[7], now[8], now[9], now[10], now[11]);
    };

    // "rightonly" (developer): only the right-eye view, with the player's own state and chain,
    // to tell an offset-viewport problem from a two-view one.
    if (test == "rightonly") {
        vr::SetCurrentEye(1);
        StoreFloat(player + 0x70, sizeX * 0.5f);
        StoreFloat(player + 0x68, originX + sizeX * 0.5f);
        __imp__sub_82300158(ctx, base);
        vr::SetCurrentEye(-1);
        StoreFloat(player + 0x68, originX);
        StoreFloat(player + 0x70, sizeX);
        if (cameraFlags) StoreWord(cameraFlags, savedCameraFlags);
        return;
    }
    vr::SetCurrentEye(0);
    StoreFloat(player + 0x70, sizeX * 0.5f);
    __imp__sub_82300158(ctx, base);
    const auto leftResult = ctx.r3;
    g_leftView.store(leftResult.u32, std::memory_order_relaxed);

    if (test != "left")
    {
        ctx = arguments;
        vr::SetCurrentEye(1);
        StoreFloat(player + 0x68, originX + sizeX * 0.5f);
        // The right eye keeps the post-process chain (+0x78) and gets a view state of its own
        // (+0x7C). Its post-processing needs the renderer's scissor fix (renderer.cpp: the game sets
        // the post scissor as X, Y, SizeX, SizeY, which is empty for a view at X=640).
        const uint32_t savedChain = LoadWord(player + 0x78), savedState = LoadWord(player + 0x7C);
        uint32_t rightState = savedState;
        if (test != "sharedstate" && test != "shared" && savedState) {
            // One state per player, allocated on first use (game thread, like the player's own).
            static uint32_t statePlayer = 0, ownState = 0;
            if (statePlayer != player || !ownState) {
                PPCContext call = arguments;
                sub_825B0CB8(call, base);
                ownState = call.r3.u32;
                statePlayer = player;
                LOG_NOTICE("vr: right eye view state {:08x} for player {:08x} (left {:08x})", ownState, player, savedState);
            }
            rightState = ownState;
        }
        if (test == "nostate") rightState = 0;
        StoreWord(player + 0x7C, rightState);
        if (test == "nochain") StoreWord(player + 0x78, 0);
        __imp__sub_82300158(ctx, base);
        g_rightView.store(ctx.r3.u32, std::memory_order_relaxed);
        StoreWord(player + 0x78, savedChain);
        StoreWord(player + 0x7C, savedState);
    }

    vr::SetCurrentEye(-1);
    StoreFloat(player + 0x68, originX);
    StoreFloat(player + 0x70, sizeX);
    logViews(leftResult.u32, test != "left" ? ctx.r3.u32 : 0);
    if (cameraFlags) StoreWord(cameraFlags, savedCameraFlags);
    ctx.r3 = leftResult;
}


std::atomic<int64_t> g_lastBattleRequestMs{0};

void vr::NoteBattleRequested()
{
    if (!vr::StereoViewsActive()) return;
    const int64_t now = NowMs();
    g_battleRequestedMs.store(now, std::memory_order_relaxed);
    g_lastBattleRequestMs.store(now, std::memory_order_relaxed);
    LOG_NOTICE("vr: battle requested");
}

void vr::MarkBattleTransition()
{
    const int64_t requested = g_lastBattleRequestMs.load(std::memory_order_relaxed);
    if (requested) LOG_NOTICE("vr: F10 mark {} ms after the battle request", NowMs() - requested);
    else LOG_NOTICE("vr: F10 mark (no battle requested yet)");
}

void vr::NotePanelView() { NoteFlatCause("comic-panel views"); }
void vr::NoteMovie() { NoteFlatCause("movie"); }

// Developer probe (LO_VR_POST_PROBE=1): the depth of field / bloom / tone-map post-process step
// (sub_823E2DE8, a scene-proxy Render method). Logs its first calls: which view it gets and what it
// returns, to see whether the right eye's view reaches it.
extern "C" PPC_FUNC(__imp__sub_823E2DE8);
PPC_FUNC(sub_823E2DE8)
{
    static const bool probe = [] { const char* v = getenv("LO_VR_POST_PROBE"); return v && v[0] == '1'; }();
    if (!probe) { __imp__sub_823E2DE8(ctx, base); return; }
    static std::atomic<uint32_t> calls{0};
    const uint32_t n = calls.fetch_add(1, std::memory_order_relaxed);
    const uint32_t self = ctx.r3.u32, a4 = ctx.r4.u32, a5 = ctx.r5.u32, view = ctx.r6.u32, a7 = ctx.r7.u32;
    const uint32_t lr = uint32_t(ctx.lr);
    const uint32_t flag = view > 0x10000 ? LoadWord(view + 0x1D4) : 0xFFFFFFFFu;
    __imp__sub_823E2DE8(ctx, base);
    const char* which = view == g_leftView.load(std::memory_order_relaxed) ? "left" :
                        view == g_rightView.load(std::memory_order_relaxed) ? "right" : "other";
    if (n < 16 || (n >= 4000 && n < 4040))
        LOG_NOTICE("vr: post step #{} {} lr={:08x} this={:08x} r4={:08x} r5={:08x} view={:08x} r7={:08x} view+1d4={:#x} -> {:#x}",
            n, which, lr, self, a4, a5, view, a7, flag, ctx.r3.u32);
}

// Post-process scene proxies, per view (sub_823E2A08: FSceneRenderer renders each view's
// post-process proxies of one depth priority group). Rebuilt here so that, in VR stereo,
// each view's proxies run inside a VR clip packet (gpu/vr_clip.h) with that view's
// rectangle: the game's post-processing draws, restores and clears the whole surface for
// every view, which overwrote the other eye. Same calls and order as the original.
extern "C" PPC_FUNC(__imp__sub_823E2A08);
// sub_823E2B28 is declared by ppc_recomp_shared.h.
namespace
{
void EmitVrClip(float left, float top, float right, float bottom)
{
    const uint32_t device = LoadWord(0x83302A38);
    const auto edge = [](float v) { return uint32_t(std::clamp<long>(std::lround(v), 0, 8192)); };
    const uint32_t words[gpu::vr_clip::WordCount] = {gpu::vr_clip::Magic, edge(left), edge(top), edge(right), edge(bottom),
                                                    gpu::vr_clip::Magic};
    gpu::frame_plan::EmitPrivatePacket(device, gpu::vr_clip::RegisterBase, words);
}
void CallVirtual(PPCContext& ctx, uint8_t* base, uint32_t target)
{
    ctx.ctr.u64 = target;
    PPC_CALL_INDIRECT_FUNC(target);
}
}

extern "C" PPC_FUNC(__imp__sub_823C10F8);

namespace
{
// The view origin of the post-processing pass in progress (stereo, game render thread).
int32_t g_postOffsetX = 0, g_postOffsetY = 0;
// The viewport the game set last (after the fix below), for AdjustPostQuad.
int32_t g_viewportMinX = 0, g_viewportMaxX = 0;
}

// RHISetViewport (r3 device, r4 MinX, r5 MinY, r7 MaxX, r8 MaxY, f1/f2 Z range). The
// post-processing passes the view size as MaxX/MaxY, right only for a view at the origin;
// inside a view's post-processing off the origin, a max that is not past the min is a
// size, so add the origin.
PPC_FUNC(sub_823C10F8)
{
    if (g_postOffsetX > 0 && int32_t(ctx.r4.u32) > 0 && int32_t(ctx.r7.u32) <= int32_t(ctx.r4.u32))
        ctx.r7.u64 = uint32_t(int32_t(ctx.r7.u32) + int32_t(ctx.r4.u32));
    if (g_postOffsetY > 0 && int32_t(ctx.r5.u32) > 0 && int32_t(ctx.r8.u32) <= int32_t(ctx.r5.u32))
        ctx.r8.u64 = uint32_t(int32_t(ctx.r8.u32) + int32_t(ctx.r5.u32));
    g_viewportMinX = int32_t(ctx.r4.u32);
    g_viewportMaxX = int32_t(ctx.r7.u32);
    __imp__sub_823C10F8(ctx, base);
}

uint32_t vr::AdjustPostQuad(double& x, double& y, double width, double height,
                            double& u, double& v, double sizeU, double sizeV,
                            uint32_t stack, const uint32_t* ints)
{
    if ((g_postOffsetX <= 0 && g_postOffsetY <= 0) || width <= 0.0 || height <= 0.0 || !stack) return 0;
    const uint32_t targetX = LoadWord(stack + 92);
    if (getenv("LO_VR_POST_QUAD_LOG")) {
        static std::set<std::array<float, 10>> seen;  // game render thread only
        if (seen.size() < 64 && seen.insert({float(x), float(y), float(width), float(u), float(sizeU), float(targetX),
                float(LoadWord(stack + 108)), float(g_viewportMinX), float(g_viewportMaxX), float(g_postOffsetX)}).second)
            LOG_NOTICE("vr: post quad origin {} xy {},{} size {}x{} uv {},{} size {}x{} target {}x{} texture {}x{} viewport {}..{}",
                g_postOffsetX, x, y, width, height, u, v, sizeU, sizeV, targetX, LoadWord(stack + 100),
                LoadWord(stack + 108), LoadWord(stack + 116), g_viewportMinX, g_viewportMaxX);
    }
    // Only a full-view quad at the view's origin placed in a target the game takes to be the view's
    // size (it ends past that target: NDC 1..3 for the right eye). The viewport is the view's (see
    // sub_823C10F8), so it belongs at 0, 0. Quads placed in the whole target (the menu backdrop,
    // drawn with the whole target as viewport) are already right.
    if (x <= 0.0 || int32_t(x) != g_postOffsetX) return 0;
    if (int32_t(targetX) <= 0 || x + width <= double(int32_t(targetX)) + 0.5) return 0;
    x = 0.0;
    return 0;
}

PPC_FUNC(sub_823E2A08)
{
    // Hand the head pose of this frame's views to the GPU thread in command order.
    if (g_memory.base && LoadWord(ctx.r3.u32 + 0x34) != 0) {
        static uint32_t lastSerial = 0, lastFlatDraw = 0;
        uint32_t serial = vr::PoseSerialForView(LoadWord(ctx.r3.u32 + 0x30));
        if (g_flatFrame) {
            if (lastFlatDraw != g_drawCounter) { lastFlatDraw = g_drawCounter; ++g_flatSerial; }
            serial = (g_flatSerial & 0x7FFFFFFFu) | vr::FlatFrameBit;
        }
        if (serial && serial != lastSerial) {
            lastSerial = serial;
            const uint32_t words[gpu::vr_clip::PoseWordCount] = {gpu::vr_clip::Magic, serial};
            gpu::frame_plan::EmitPrivatePacket(LoadWord(0x83302A38), gpu::vr_clip::PoseBegin, words);
        }
    }
    static const bool clipEnabled = [] { const char* v = getenv("LO_VR_POST_CLIP"); return !v || v[0] != '0'; }();
    if (!clipEnabled || !vr::StereoViewsActive() || !g_memory.base) { __imp__sub_823E2A08(ctx, base); return; }
    const uint32_t sceneRenderer = ctx.r3.u32, group = ctx.r4.u32;
    const int32_t viewCount = int32_t(LoadWord(sceneRenderer + 0x34));
    if (viewCount < 2) { __imp__sub_823E2A08(ctx, base); return; }
    const PPCContext entry = ctx;
    bool started = false;
    // LO_VR_POST_REVERSE=1 (developer): run the right view's post-processing first.
    static const bool reverse = [] { const char* v = getenv("LO_VR_POST_REVERSE"); return v && v[0] == '1'; }();
    for (int32_t n = 0; n < viewCount; ++n) {
        const int32_t v = reverse ? viewCount - 1 - n : n;
        const uint32_t view = LoadWord(sceneRenderer + 0x30) + uint32_t(v) * 0x6F0;
        bool clipped = false;
        for (int32_t i = 0; i < int32_t(LoadWord(view + 0x6B8)); ++i) {
            const uint32_t proxy = LoadWord(LoadWord(view + 0x6B4) + uint32_t(i) * 4);
            if (!proxy || (LoadWord(proxy + 4) >> 29) != group) continue;
            if (!started) {
                PPCContext call = entry;
                call.r3.u32 = LoadWord(0x83302A38); call.r4.u64 = 0; call.r5.u64 = 0x20; call.r6.u64 = 0x60;
                sub_823E2B28(call, base);
                started = true;
            }
            if (group == 4 && int32_t(LoadWord(view + 0x1D4)) > 0) StoreWord(proxy + 4, LoadWord(proxy + 4) & ~0x10000000u);
            if (!clipped) {
                // FViewInfo's integer render-target rectangle: +0x2C X, +0x30 Y, +0x34 SizeX, +0x38 SizeY.
                static const bool originFix = [] { const char* e = getenv("LO_VR_POST_ORIGIN_FIX"); return !e || e[0] != '0'; }();
                if (originFix) { g_postOffsetX = int32_t(LoadWord(view + 0x2C)); g_postOffsetY = int32_t(LoadWord(view + 0x30)); }
                const float x = LoadFloat(view + 0x1C), y = LoadFloat(view + 0x20);
                EmitVrClip(x, y, x + LoadFloat(view + 0x24), y + LoadFloat(view + 0x28));
                clipped = true;
            }
            PPCContext call = entry;
            call.r3.u32 = proxy; call.r4.u32 = LoadWord(sceneRenderer + 0x9C); call.r5.u32 = group;
            call.r6.u32 = view; call.r7.u32 = sceneRenderer + 0x50;
            CallVirtual(call, base, LoadWord(LoadWord(proxy) + 4));
            ctx.r3 = call.r3;
        }
        g_postOffsetX = g_postOffsetY = 0;
        if (clipped) EmitVrClip(0, 0, 0, 0);
    }
    if (started) {
        PPCContext call = entry;
        call.r3.u32 = LoadWord(0x83302A38); call.r4.u64 = 0; call.r5.u64 = 0x40; call.r6.u64 = 0x40;
        sub_823E2B28(call, base);
    }
}
