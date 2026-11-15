# CoachNowPlaying

Optional ETS2 plugin for the Steam Workshop mod **"CarPlay-Style Infotainment for Coaches"** (Coaches DLC).
*English below · Deutsch weiter unten*

## What it adds
- **Music page** and **"now playing" bar** on the map page for the in-game radio (song, artist, cover, station logo).
  The bar only shows while the radio plays.
- **Arrival time as a real clock time** – the game's own field shows the remaining travel time ("25:08");
  the plugin shows e.g. `22:52 +1` (tomorrow).
- **Speed limit digits** on the sign, read from the telemetry.
- **Passenger TV** (MAN Lion's Coach): destination, arrival time, remaining distance, clock, progress bar and
  "Next stop" shortly before arrival, on the TV at the front ceiling.
- **Arrival announcement** in coaches: about 5 minutes before the destination a soft chime and a natural German voice
  announce the destination ("Sehr geehrte Fahrgäste, wir erreichen in wenigen Minuten …"), sounding like the driver at
  the microphone. Uses [Piper](https://github.com/rhasspy/piper) with the voice "Thorsten" (offline, included in the
  release ZIP); without it the Windows voice is used.
- **Start-up animation** on the CarPlay screen (mod 0.7.0 or newer): when the bus is switched on, and when the screen
  comes back from the black OFF page, four dots circle and merge into the ROADWORKS logo before the page shows.
- **Number plate on the rear luggage box** (mod "Rear Luggage Box for MAN Lion's Coach"): the game only shows the plate
  on the bus itself (hidden behind the box), so the plugin draws the bus's plate onto the box – text and country from
  the telemetry (EU band with stars and country code, yellow for UK).

The mods work without the plugin; these extras need it.

## Download & install
1. Download **`CoachNowPlaying_v*.zip`** from [Releases](../../releases).
2. Close the game.
3. Copy `CoachNowPlaying.dll` **and the folder `CoachNowPlaying_piper`** to
   `...\Steam\steamapps\common\Euro Truck Simulator 2\bin\win_x64\plugins\` (create the `plugins` folder if it does not
   exist). The folder is only needed for the voice announcements.
4. Start the game and confirm the message about plugins / advanced SDK features.

Do not use it together with EtsNowPlaying.

## Controls
- Pages: the **"Infotainment Display Mode"** key.
- Radio: the game's own radio keys.
- Cabin screens cannot be clicked.

## Settings & privacy
`Documents\Euro Truck Simulator 2\CoachNowPlaying\CoachNowPlaying.ini` is created on the first start; the log is
`diag.log` in the same folder. With `[radio] cover_lookup=1` (default) the current radio song title and station name are
sent to iTunes Search, Deezer and radio-browser.info to find covers / logos. Set `cover_lookup=0` to switch this off.
No accounts, no tracking.

Announcements: section `[announce]` – `enabled` (1/0), `minutes` (real minutes before arrival, default 5), `volume`
(0..100), `language` (`auto` = German, `en` = English with the Windows voice). The sound plays through Windows, not
through the game's volume settings.

**Convoy:** if joining a Convoy session fails with "DLCs do not match", set `[radio] stream_titles=0` in the INI.
The radio info (music page, now playing bar) is then off; arrival time and speed limit keep working.

## How it works
Unlike EtsNowPlaying it does **not patch game code**. The mod ships transparent placeholder textures with unusual sizes
(`/material/ui/cpc_np/*.dds`); a Direct3D 11 hook recognises them and shows the plugin's own textures in their place.
Telemetry comes from the official SCS SDK. A game update therefore rarely breaks it.

## Build
`build.bat` – needs Visual Studio or the Build Tools with the "Desktop development with C++" workload.

## Credits & license
Based on [EtsNowPlaying](https://github.com/jxhnch/EtsNowPlaying) by jxhnch, MIT License (see `LICENSE`).
Includes MinHook and the SCS SDK headers, see `THIRD_PARTY_NOTICES.md`. The release ZIP also contains Piper (MIT),
ONNX Runtime (MIT), eSpeak NG (GPL-3.0) and the Piper voice "Thorsten" (CC0), see `THIRD_PARTY_NOTICES.md`.
Not affiliated with SCS Software, Apple, Deezer or TruckersMP. Use at your own risk.

---

## Deutsch

Optionales Plugin für die Workshop-Mod **„CarPlay-Style Infotainment for Coaches“**.

**Installation:** Spiel schließen → `CoachNowPlaying.dll` aus der ZIP unter [Releases](../../releases) nach
`...\Euro Truck Simulator 2\bin\win_x64\plugins\` kopieren, dazu den Ordner `CoachNowPlaying_piper` (Stimme für die
Ansagen) → beim nächsten Start die Plugin-Meldung bestätigen.

**Bringt:** Musik-Seite und „Läuft gerade“-Leiste fürs Ingame-Radio (nur sichtbar, wenn das Radio läuft), echte Ankunftsuhrzeit statt Restzeit,
Tempolimit-Zahl im Schild, Live-Fahrgastanzeige auf dem Fernseher im MAN Lion's Coach (Ziel, Ankunft, Fortschritt,
„Nächster Halt“) und eine Ansage etwa 5 Minuten vor dem Ziel mit Gong und natürlicher Stimme („Thorsten“, Piper, offline).
Einstellungen unter `[announce]` in der INI (an/aus, Minuten, Lautstärke). Dazu eine Startanimation auf dem
CarPlay-Bildschirm (ab Mod 0.7.0), wenn der Bus eingeschaltet wird und wenn das Display von der schwarzen Seite wieder
angeht. Mit der Mod „Rear Luggage Box for MAN Lion's Coach“ zeichnet das Plugin außerdem das Kennzeichen des Busses auf
die Heckbox (das Spiel zeigt es nur am Bus selbst).

**Steuerung:** Seiten mit „Infotainment Display Mode“, Radio mit den Radio-Tasten des Spiels.

**Convoy:** Kommt beim Beitreten „DLCs stimmen nicht überein“, in der INI `[radio] stream_titles=0` setzen. Dann fehlen
nur die Radio-Infos.

**Datenschutz:** Für Cover und Senderlogos werden Songtitel und Sendername an iTunes, Deezer und radio-browser.info
geschickt. Abschalten: `cover_lookup=0` in `Dokumente\Euro Truck Simulator 2\CoachNowPlaying\CoachNowPlaying.ini`.
