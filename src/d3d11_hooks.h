#pragma once
#include "common.h"

namespace ep {

// Hooks Present / PSSetShaderResources / CreateShaderResourceView of the game's D3D11 device.
// replaceTexture: swap the 64x2048 placeholder for our screen texture. uploadFrames: fill it every frame.
// touch (beta): hook the draw calls so the mouse position on the cabin screen can be found.
bool D3D11HooksInstall(bool replaceTexture, bool uploadFrames, bool touch);
void D3D11HooksDisable();

// GPS takeover: true while our picture is on the second texture of the game's screen pair, which the game fills
// upside down (it is a flipped copy of the first), so the picture has to be drawn flipped as well.
bool TakeoverFlip();

}  // namespace ep
