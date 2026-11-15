#pragma once
#include "common.h"

#include <d3d11.h>
#include <string>

namespace ep {

// Small text fields drawn by the plugin into placeholder textures of the coach mod (/material/ui/cpc_np/):
//  - ANKUNFT  eta.dds   BC3 52x1996: arrival as clock time = game.time + truck.navigation.time, wrapped to 24 h,
//                       "+1" etc. when it is on a later day. Opaque (covers the game's own value underneath).
//  - LIMIT    limit.dds BC3 44x1996: speed limit from truck.navigation.speed_limit, dark digits, transparent
//                       background (sits on the white sign of the mod).
// The game's dashboard ID 1530 is the remaining travel time (e.g. "25:08" = 25 h 8 min), not a clock time.

// Telemetry input (called from the SDK channel callbacks).
void EtaSetGameTime(uint32_t minutes);
void EtaSetNavigationTime(float seconds);
void EtaSetNavigationDistance(float meters);
void EtaSetSpeedLimit(float metersPerSecond);

// D3D11 hook plumbing (all called from d3d11_hooks.cpp).
using CreateTexFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const D3D11_TEXTURE2D_DESC*, const D3D11_SUBRESOURCE_DATA*,
                                                ID3D11Texture2D**);
using CreateSrvFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, ID3D11Resource*, const D3D11_SHADER_RESOURCE_VIEW_DESC*,
                                                ID3D11ShaderResourceView**);
bool EtaIsPlaceholder(const D3D11_TEXTURE2D_DESC* d);
void EtaOnPlaceholderCreated(ID3D11Device* dev, const D3D11_TEXTURE2D_DESC& d, ID3D11Texture2D* placeholder,
                             CreateTexFn createTex);
bool EtaIsPlaceholderResource(ID3D11Resource* res);
void EtaOnPlaceholderViewCreated(ID3D11Device* dev, ID3D11Resource* res, ID3D11ShaderResourceView* gameView,
                                 const D3D11_SHADER_RESOURCE_VIEW_DESC* desc, CreateSrvFn createSrv);
void EtaForgetView(ID3D11ShaderResourceView* s);
// Returns our view if `s` is one of the game's views of a text placeholder, else nullptr.
ID3D11ShaderResourceView* EtaSwapFor(ID3D11ShaderResourceView* s);
// Redraws texts that changed and uploads them (Present thread).
void EtaUpload();
// Telemetry truck.electric.enabled: switching on plays the start-up animation of the CarPlay screen.
void EtaSetElectric(bool on);
// Telemetry config truck license.plate / license.plate.country.id: drawn on the rear luggage box placeholder.
void EtaSetPlate(const std::string& plate, const std::string& countryId);

}  // namespace ep
