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

// Game thread, once per frame: the scene size VR wants (mono: one eye; stereo: both
// eyes side by side) at `percent` (per axis) of the headset's recommended per-eye
// size. False when VR is off, theater, or the headset is unknown: Render
// resolution applies then.
bool RenderSize(uint32_t percent, uint32_t& width, uint32_t& height);
// The runtime's recommended per-eye image size (false before OpenXR starts).
bool RecommendedEyeSize(uint32_t& width, uint32_t& height);

// Stereo: CalcSceneView is being run once per eye this frame.
bool StereoViewsActive();
// Head pose per rendered frame (mono/stereo). AdjustMainView numbers each frame's pose;
// the game render thread looks the serial up from its FViewInfo (same view matrix) and puts
// it in the command ring; the GPU thread hands it over with the frame, and the frame is
// submitted to OpenXR with the pose it was rendered with (no swim when turning the head).
uint32_t PoseSerialForView(uint32_t view);
void SetFramePoseSerial(uint32_t serial);
// Stereo, game render thread, inside a view's post-processing: the game places its full-view
// quads (DrawDenormalizedQuad) at the view's origin X/Y, in a target it takes to be the view's
// size or the whole surface; with the viewport the view's own, they belong at 0, 0 of a target
// the view's size. No change outside VR post-processing or for a view at the origin.
// `stack` is the caller's stack pointer: its +92/+100 words are the target size the quad is
// placed in. Returns the original TargetSizeX word to put back at stack + 92 after the
// call when it was changed, else 0. `ints` (r4..r10) are for the diagnostic log only.
uint32_t AdjustPostQuad(double& x, double& y, double width, double height,
                        double& u, double& v, double sizeU, double sizeV,
                        uint32_t stack, const uint32_t* ints = nullptr);
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
// Stereo with VR → HUD position "Fixed in space": the 2D canvas (HudCanvas: same test as
// HudEyeRects) is drawn into a layer of its own instead of the eyes, and the headset
// compositor shows that layer as a flat panel fixed in space (true perspective, no roll).
bool HudLayerActive();
bool HudCanvas(const plume::RenderViewport& viewport, int32_t targetWidth, int32_t targetHeight, uint32_t guestWidth,
               uint64_t vertexShader, uint64_t pixelShader);
// Renderer, at the present: this frame's HUD layer (null when nothing was drawn into it).
void SetHudLayerSource(plume::RenderTexture* texture, uint32_t width, uint32_t height);
// Renderer: the frame the headset gets already holds the game's display gamma ramp and the
// player's Brightness / Gamma, so desktop presentation must not apply them again.
void SetDisplayCurveInFrame(bool inFrame);
// Stereo frames the game builds with its comic-panel views (cutscenes) are rendered flat (one
// full view, panels composited on top) and shown on a screen in front of you. Their pose serial
// (gpu/vr_clip.h PoseBegin) carries FlatFrameBit. GPU thread: the frame being processed is one.
constexpr uint32_t FlatFrameBit = 0x80000000u;
bool FlatFrame();
// Game thread (hor_plus.cpp): the game built a comic-panel view (caller 0x82307BEC) this frame.
void NotePanelView();
// Game thread (hor_plus.cpp): a movie (FLO_MOVIE) was drawn into the main output this frame.
// Movies are shown flat the same way, whatever the movie.
void NoteMovie();
// Game thread (patches/encounter_defer.cpp): a battle was requested. Its transition (the field
// picture warped full screen) is shown flat until the battle runs (at most 10 s).
void NoteBattleRequested();
// F10 (developer): log the time since the last battle request (measuring the transition).
void MarkBattleTransition();
bool DisplayCurveInFrame();

// Present thread: the game's final frame for this present (internal
// resolution, 8-bit RGBA, before desktop scaling); the VR copy prefers it over
// the desktop swap chain image. Not used in theater mode.
void SetFrameSource(plume::RenderTexture* texture, uint32_t width, uint32_t height);

// Present thread: a host menu (Settings, F1 debug menu) is drawn this frame.
// The headset then shows the finished desktop image (with the menu) on a
// screen in front of the player instead of the per-eye views.
void SetHostMenuVisible(bool visible);

// Recenter (F9 or both sticks clicked): the current head yaw and position
// become forward / origin. Applied on the next VR frame; any thread.
void RequestRecenter();

// Session is running and frames are being submitted.
bool Active();

void Shutdown();
}
