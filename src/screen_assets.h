#pragma once
#include "common.h"

namespace ep {

// Size of the placeholder texture shipped by the "CarPlay-Style Infotainment for Coaches" mod
// (/material/ui/cpc_np/np.dds). The D3D11 hook recognises the MUSIK page by this unusual size.
constexpr UINT kPlaceholderWidth = 60;
constexpr UINT kPlaceholderHeight = 1996;

// Not used by CoachNowPlaying (the mod ships the placeholder), kept for the original EtsNowPlaying code path.
bool EnsureScreenAssets(const std::wstring& dir);

}  // namespace ep
