# Third-party notices

EtsNowPlaying includes the following third-party code. Their licenses apply to those parts.

| Component | Location | License |
|---|---|---|
| [MinHook](https://github.com/TsudaKageyu/minhook) – x86/x64 API hooking library | `third_party/minhook` | BSD 2-Clause, see `third_party/minhook/LICENSE.txt` |
| SCS Software SDK headers (telemetry plugin interface) | `third_party/scssdk` | MIT-style, see `third_party/scssdk/LICENSE.txt` |

The plugin additionally uses these Windows components at run time: Direct3D 11, DXGI, Direct2D, DirectWrite, WIC, WinHTTP,
and the Windows Runtime media control APIs. No other libraries are linked.

## Bundled in the release ZIP (folder `CoachNowPlaying_piper`, used for the arrival announcements)

These programs are shipped unchanged as separate executables/libraries next to the plugin; the plugin only starts
`piper.exe`. Their licenses apply to them.

| Component | License | Source |
|---|---|---|
| Piper text-to-speech (`piper.exe`, `piper_phonemize.dll`), release 2023.11.14-2 | MIT | https://github.com/rhasspy/piper |
| ONNX Runtime (`onnxruntime*.dll`) | MIT | https://github.com/microsoft/onnxruntime |
| eSpeak NG (`espeak-ng.dll`, `espeak-ng-data`) | GPL-3.0-or-later | https://github.com/rhasspy/espeak-ng (fork used by Piper) |
| Piper voice `de_DE-thorsten-high` | CC0 (Thorsten-Voice dataset) | https://huggingface.co/rhasspy/piper-voices |

eSpeak NG is licensed under the GNU GPL version 3; its complete source code is available at the link above.