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
| Piper voices (`voice\*.onnx`), from https://huggingface.co/rhasspy/piper-voices - license of the training data per voice: | | |
| `de_DE-thorsten-high`, `bg_BG-dimitar`, `cs_CZ-jirka`, `da_DK-talesyntese`, `el_GR-rapunzelina`, `es_ES-davefx`, `fi_FI-harri`, `hu_HU-imre`, `it_IT-paola`, `lv_LV-aivars`, `nl_BE-rdh`, `nl_NL-pim`, `no_NO-talesyntese`, `pl_PL-darkman`, `pt_BR-faber`, `pt_PT-tugao`, `ro_RO-mihai`, `ru_RU-dmitri`, `sk_SK-lili`, `sv_SE-nst`, `uk_UA-ukrainian_tts` (all medium except thorsten) | CC0 | model cards on Hugging Face |
| `es_MX-ald-medium` | Unlicense | https://huggingface.co/datasets/rmcpantoja/Ald_Mexican_Spanish_speech_dataset |
| `eu_ES-antton-medium`, `sl_SI-artur-medium`, `vi_VN-vais1000-medium`, `et_EE-news-medium` | CC BY 4.0 / CC-BY | model cards on Hugging Face |
| `ca_ES-upc_ona-medium` | CC BY-SA 3.0 ES | model card on Hugging Face |
| `en_US-ryan-medium`, `ko_KR-kss-medium`, `sr_RS-serbski_institut-medium`, `tr_TR-dfki-medium` | CC BY-NC-SA 4.0 (non-commercial) | model cards on Hugging Face |
| `fr_FR-tom-medium` | AGPL-3.0 | model card on Hugging Face |
| `en_GB-alan-medium`, `ka_GE-natia-medium`, `zh_CN-huayan-medium` | see the model card (dataset terms) | model cards on Hugging Face |

The `.onnx.json` files of the voices made with Piper 1.5 have five unused diphthong entries removed from
`phoneme_id_map` so that Piper 1.2 can load them; the models themselves are unchanged.

eSpeak NG is licensed under the GNU GPL version 3; its complete source code is available at the link above.