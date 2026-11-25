#pragma once
#include "common.h"

namespace ep {

// The layout is designed on a canvas of 800x480 units. It is drawn into the screen texture, whose size is
// kScreenWidth x kScreenHeight, through a transform that fits it onto the real shape of the truck's display.
constexpr uint32_t kCanvasWidth = 800;
constexpr uint32_t kCanvasHeight = 480;
constexpr uint32_t kScreenWidth = 1024;
// 640 with the mod. GPS takeover: 1024, like the game's own square cab screen texture, because the display only shows a
// band of it (on the Volvo 9700 a third of the height) and the text needs those rows. Set once before the renderer starts.
inline uint32_t kScreenHeight = 640;

struct FrameView {
    const uint8_t* data = nullptr;  // BGRA, premultiplied, opaque
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t pitch = 0;
    uint64_t version = 0;
};

// Starts the thread that draws the screen (Direct2D / DirectWrite into a CPU bitmap, ~15 fps).
void RendererStart();
void RendererStop();

// If a frame newer than `afterVersion` exists, fills `view` and returns true with the frame locked.
// The caller must then call RendererUnlockFrame().
bool RendererLockFrame(uint64_t afterVersion, FrameView* view);
void RendererUnlockFrame();

}  // namespace ep
