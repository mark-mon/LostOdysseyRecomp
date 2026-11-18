#pragma once

#include <cstdint>

// VR stereo: an ordered host-private register packet (like movie_clear) that confines the
// draws and resolve clears of one view's post-processing to that view's rectangle. The game's
// post-processing assumes its view starts at the origin and draws or clears the whole surface;
// with both eyes on one surface that overwrote the other eye. Guest units (1280x720 space);
// an all-zero rectangle turns the clip off.
namespace gpu::vr_clip
{
constexpr uint32_t RegisterBase = 0x7F60;
constexpr uint32_t Magic = 0x4C4F5643; // "LOVC"
enum Register : uint32_t
{
    Begin = RegisterBase,
    Left,
    Top,
    Right,
    Bottom,
    Commit,
};
constexpr uint32_t WordCount = Commit - RegisterBase + 1;

// VR: the head pose serial (vr::PoseSerialForView) the frame in the ring was rendered with,
// so the frame is shown with exactly that pose. Two words: Magic, then the serial.
constexpr uint32_t PoseBegin = RegisterBase + 8;
constexpr uint32_t PoseSerial = PoseBegin + 1;
constexpr uint32_t PoseWordCount = 2;

// VR stereo: inside a view's post-processing, the game's post-process quad helper
// (DrawDenormalizedQuad, sub_823E4B50) is drawing: Magic, then 1 (begin) or 0 (end). Its quads
// sample the both-eyes surface at this eye's part already; other draws there are not trusted to
// (renderer VrEyeCrop).
constexpr uint32_t PostQuadBegin = RegisterBase + 10;
constexpr uint32_t PostQuadState = PostQuadBegin + 1;
constexpr uint32_t PostQuadWordCount = 2;

// VR stereo, sent with a view's clip: Magic, that eye's view-projection (FViewInfo +0x100, 16 floats)
// and the game's own view-projection for the same frame, before the headset's (16 floats). Effects
// the renderer gives this eye's part of the picture (VrEyeCrop) are screen materials on a mesh the
// game placed to fill its flat screen; drawn with the eye's matrix that mesh sits off the eye's
// screen (double image), so they are drawn with the game's.
// VR stereo: the post-process proxy (its vtable) whose Render the game is about to run inside a view's
// clip: Magic, vtable. The renderer reports the proxies whose draws needed the eye crop (vr::NoteEyeCropProxy).
constexpr uint32_t PostProxyBegin = RegisterBase + 14;
constexpr uint32_t PostProxyVtable = PostProxyBegin + 1;
constexpr uint32_t PostProxyWordCount = 2;

constexpr uint32_t ViewMatricesBegin = RegisterBase + 0x10;
constexpr uint32_t ViewMatricesWordCount = 33;
}
