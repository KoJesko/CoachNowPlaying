# CoachNowPlaying

Optional ETS2 plugin for the Steam Workshop mod **"CarPlay-Style Infotainment for Coaches"** (Coaches DLC).
*English below · Deutsch weiter unten*

## What it adds
- **MUSIK page** and **"now playing" bar** on the KARTE page: Spotify (Windows media controls) and the in-game radio
  (song, artist, cover, station logo).
- **Arrival time as a real clock time** – the game's own field shows the remaining travel time ("25:08");
  the plugin shows e.g. `22:52 +1` (tomorrow).
- **Speed limit digits** on the sign, read from the telemetry.

The mod works without the plugin; these extras need it.

## Download & install
1. Download **`CoachNowPlaying_v*.zip`** from [Releases](../../releases).
2. Close the game.
3. Copy `CoachNowPlaying.dll` to `...\Steam\steamapps\common\Euro Truck Simulator 2\bin\win_x64\plugins\`
   (create the `plugins` folder if it does not exist).
4. Start the game and confirm the message about plugins / advanced SDK features.

Do not use it together with EtsNowPlaying.

## Controls
- Pages: the **"Infotainment Display Mode"** key.
- Radio: the game's own radio keys. Spotify: your keyboard's media keys.
- Cabin screens cannot be clicked.

## Settings & privacy
`Documents\Euro Truck Simulator 2\CoachNowPlaying\CoachNowPlaying.ini` is created on the first start; the log is
`diag.log` in the same folder. With `[radio] cover_lookup=1` (default) the current radio song title and station name are
sent to iTunes Search, Deezer and radio-browser.info to find covers / logos. Set `cover_lookup=0` to switch this off.
No accounts, no tracking.

## How it works
Unlike EtsNowPlaying it does **not patch game code**. The mod ships transparent placeholder textures with unusual sizes
(`/material/ui/cpc_np/*.dds`); a Direct3D 11 hook recognises them and shows the plugin's own textures in their place.
Telemetry comes from the official SCS SDK. A game update therefore rarely breaks it.

## Build
`build.bat` – needs Visual Studio or the Build Tools with the "Desktop development with C++" workload.

## Credits & license
Based on [EtsNowPlaying](https://github.com/jxhnch/EtsNowPlaying) by jxhnch, MIT License (see `LICENSE`).
Includes MinHook and the SCS SDK headers, see `THIRD_PARTY_NOTICES.md`.
Not affiliated with SCS Software, Spotify, Apple, Deezer or TruckersMP. Use at your own risk.

---

## Deutsch

Optionales Plugin für die Workshop-Mod **„CarPlay-Style Infotainment for Coaches“**.

**Installation:** Spiel schließen → `CoachNowPlaying.dll` aus der ZIP unter [Releases](../../releases) nach
`...\Euro Truck Simulator 2\bin\win_x64\plugins\` kopieren → beim nächsten Start die Plugin-Meldung bestätigen.

**Bringt:** MUSIK-Seite und „Läuft gerade“-Leiste (Spotify und Ingame-Radio), echte Ankunftsuhrzeit statt Restzeit,
Tempolimit-Zahl im Schild.

**Steuerung:** Seiten mit „Infotainment Display Mode“, Radio mit den Radio-Tasten des Spiels, Spotify mit den
Medientasten der Tastatur.

**Datenschutz:** Für Cover und Senderlogos werden Songtitel und Sendername an iTunes, Deezer und radio-browser.info
geschickt. Abschalten: `cover_lookup=0` in `Dokumente\Euro Truck Simulator 2\CoachNowPlaying\CoachNowPlaying.ini`.
