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
}
