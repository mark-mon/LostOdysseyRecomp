#pragma once

// VR output through OpenXR (opt-in). The OpenXR loader is opened at runtime only
// when VR is requested, so builds and players without a headset are unaffected.
//
// Phase 1 ("theater"): the finished desktop frame is shown on a quad in the
// headset. Later phases add head tracking and per-eye scene views.

#include <cstdint>
#include <string>
#include <vector>

namespace plume
{
struct RenderCommandQueue;
struct RenderCommandSemaphore;
struct RenderDevice;
struct RenderTexture;
struct VulkanExtensionHooks;
struct RenderViewport;
struct RenderRect;
}

namespace vr
{
enum class Mode : uint32_t
{
    Off = 0,
    Theater = 1,  // finished frame on a quad in front of the player
    Mono = 2,     // one head-tracked view, the same image to both eyes (stepping stone to stereo)
    Stereo = 3,   // two FSceneViews per frame (left/right halves), each with its eye's pose and frustum
};

// Requested at startup (LO_VR=theater|mono|stereo); fixed for the run.
Mode RequestedMode();

// Opens the loader, creates the OpenXR instance and finds the headset. Safe to
// call more than once; returns false (and logs why) when VR cannot start.
bool Initialize();

// Vulkan: adds the instance/device extensions the runtime needs to the hooks
// plume already uses for DLSS/Streamline. Returns `inner` unchanged when VR is off.
plume::VulkanExtensionHooks WrapVulkanHooks(const plume::VulkanExtensionHooks& inner);

// Name of the GPU the headset is attached to, or empty to keep the normal choice.
std::string PreferredDeviceName();

// Creates the OpenXR session on the game's device and graphics queue.
void OnDeviceCreated(plume::RenderDevice* device, plume::RenderCommandQueue* queue);

// Called on the present thread just before the desktop swap chain is presented.
// `image` is the finished swap chain image (in present layout). Paces the
// caller to the headset (xrWaitFrame) and may add a semaphore that the desktop
// present must wait on.
void SubmitDesktopFrame(plume::RenderTexture* image, uint32_t width, uint32_t height,
                        std::vector<plume::RenderCommandSemaphore*>& presentWaits);

// Game thread, from the FSceneView constructor hook (view+0x40 view matrix and
// view+0x80 projection copied, nothing derived yet). Applies the head pose and
// the headset field of view to the main camera. Returns true when it changed
// the view (the caller then skips its own projection edits).
bool AdjustMainView(uint32_t view);

// Stereo: CalcSceneView is being run once per eye this frame.
bool StereoViewsActive();
// Eye whose FSceneView is being built on this thread (0 left, 1 right, -1 none).
void SetCurrentEye(int eye);

// Stereo, renderer thread: a draw with `viewport` into a target of the given
// size is the game's 2D canvas (HUD, menus, fades) when it covers the whole
// full-size 16:9 output (the eye views use half-width viewports) and is not a
// full-screen post-processing quad (identified by its vertex shader). Then fills one
// viewport and scissor per eye (a centred, aspect-correct panel in each half)
// and returns true.
bool HudEyeRects(const plume::RenderViewport& viewport, int32_t targetWidth, int32_t targetHeight, uint32_t guestWidth,
                 uint64_t vertexShader, uint64_t pixelShader, plume::RenderViewport* eyes, plume::RenderRect* scissors);

// Present thread: the game's final frame for this present (internal
// resolution, 8-bit RGBA, before desktop scaling); the VR copy prefers it over
// the desktop swap chain image. Not used in theater mode.
void SetFrameSource(plume::RenderTexture* texture, uint32_t width, uint32_t height);

// Recenter (F9 or both sticks clicked): the current head yaw and position
// become forward / origin. Applied on the next VR frame; any thread.
void RequestRecenter();

// Session is running and frames are being submitted.
bool Active();

void Shutdown();
}
