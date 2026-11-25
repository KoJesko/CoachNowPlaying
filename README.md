# CoachNowPlaying

> **🐧 Linux (Proton):** this fork's `linux` branch adds a Linux build. The music page shows whatever your system plays, including song + station on [ETS2/ATS Local Radio](https://github.com/Koenvh1/ets2-local-radio) ([Linux port](https://github.com/KoJesko/ets2-local-radio)), with songs identified by Shazam when the station doesn't send them. See **[linux/README.md](linux/README.md)**.

Optional ETS2 plugin for the Steam Workshop mod **"CarPlay-Style Infotainment for Coaches"** (Coaches DLC).
*English below · Deutsch weiter unten*

## What it adds
- **Music page** and **"now playing" bar** on the map page for the in-game radio (song, artist, cover, station logo).
  The bar only shows while the radio plays.
- **Arrival time as a real clock time** – the game's own field shows the remaining travel time ("25:08");
  the plugin shows e.g. `22:52 +1` (tomorrow).
- **Speed limit digits** on the sign, read from the telemetry.
- **Passenger TV** (MAN Lion's Coach): destination, arrival time, remaining distance, clock, progress bar and
  "Next stop" shortly before arrival, on the TV at the front ceiling - in the language of the game with English
  below (40 languages).
- **Arrival announcement** in coaches: about 5 minutes before the destination a soft chime and a natural voice
  announce the destination in the **language of the game** ("Ladies and gentlemen, we will arrive in …"), sounding like
  the driver at the microphone. Uses [Piper](https://github.com/rhasspy/piper) (offline) with one voice per language:
  bg, ca, cs, da, de, el, en (UK/US), es (ES/Latin America), et, eu, fi, fr, hu, it, ka, ko, lv, nl (NL/BE), no, pl,
  pt (PT/BR), ro, ru, sk, sl, sr, sv, tr, uk, vi, zh. For Galician, Croatian, Japanese, Lithuanian and Macedonian the
  Windows voice of that language is used if installed, else English.
- **Start-up animation** on the CarPlay screen (mod 0.7.0 or newer): when the bus is switched on (not when the
  screen comes back from the black OFF page), four dots circle and merge into the ROADWORKS logo before the page shows.
- **Number plate on the rear luggage box** (mod "Rear Luggage Box for MAN Lion's Coach"): the game only shows the plate
  on the bus itself (hidden behind the box), so the plugin draws the bus's plate onto the box – text and country from
  the telemetry (EU band with stars and country code, yellow for UK).

The mods work without the plugin; these extras need it.

## Download & install
1. Download **`CoachNowPlaying_v*.zip`** from [Releases](../../releases).
2. Close the game.
3. Copy `CoachNowPlaying.dll` **and the folder `CoachNowPlaying_piper`** to
   `...\Steam\steamapps\common\Euro Truck Simulator 2\bin\win_x64\plugins\` (create the `plugins` folder if it does not
   exist). The folder is only needed for the voice announcements; it contains the German and British English voice.
4. Other languages: download **`CoachNowPlaying_voice_<language>.zip`** (e.g. `..._voice_fr_FR.zip`, about 56 MB each)
   for the language you play in and extract it into the same `plugins` folder (it adds a file to
   `CoachNowPlaying_piper\voice`). Without it the announcement uses the Windows voice of that language, else English.
5. Start the game and confirm the message about plugins / advanced SDK features.

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
(0..100), `language` (`auto` = language of the game; or codes such as `fr`, `pt_br`; several with commas,
e.g. `auto,en` = first in the game's language, then in English). Only the voices in `CoachNowPlaying_piper\voice` are
needed for the languages you use - the others can be deleted (about 60 MB each). The sound plays through Windows, not
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
ONNX Runtime (MIT), eSpeak NG (GPL-3.0) and the Piper voices (licenses per voice), see `THIRD_PARTY_NOTICES.md`.
Not affiliated with SCS Software, Apple, Deezer or TruckersMP. Use at your own risk.

---

## Deutsch

Optionales Plugin für die Workshop-Mod **„CarPlay-Style Infotainment for Coaches“**.

**Installation:** Spiel schließen → `CoachNowPlaying.dll` aus der ZIP unter [Releases](../../releases) nach
`...\Euro Truck Simulator 2\bin\win_x64\plugins\` kopieren, dazu den Ordner `CoachNowPlaying_piper` (Stimmen für die
Ansagen, Deutsch und Englisch). Für andere Sprachen zusätzlich `CoachNowPlaying_voice_<Sprache>.zip` in denselben
`plugins`-Ordner entpacken → beim nächsten Start die Plugin-Meldung bestätigen.

**Bringt:** Musik-Seite und „Läuft gerade“-Leiste fürs Ingame-Radio (nur sichtbar, wenn das Radio läuft), echte Ankunftsuhrzeit statt Restzeit,
Tempolimit-Zahl im Schild, Live-Fahrgastanzeige auf dem Fernseher im MAN Lion's Coach (Ziel, Ankunft, Fortschritt,
„Nächster Halt“) und eine Ansage etwa 5 Minuten vor dem Ziel mit Gong und natürlicher Stimme in der Sprache des Spiels
(Piper, offline, 34 Sprachen). Einstellungen unter `[announce]` in der INI (an/aus, Minuten, Lautstärke, `language` –
z. B. `auto,en` für erst Spielsprache, dann Englisch). Dazu eine Startanimation auf dem
CarPlay-Bildschirm (ab Mod 0.7.0), wenn der Bus eingeschaltet wird (nicht mehr, wenn das Display von der schwarzen Seite wieder
angeht). Mit der Mod „Rear Luggage Box for MAN Lion's Coach“ zeichnet das Plugin außerdem das Kennzeichen des Busses auf
die Heckbox (das Spiel zeigt es nur am Bus selbst).

**Steuerung:** Seiten mit „Infotainment Display Mode“, Radio mit den Radio-Tasten des Spiels.

**Convoy:** Kommt beim Beitreten „DLCs stimmen nicht überein“, in der INI `[radio] stream_titles=0` setzen. Dann fehlen
nur die Radio-Infos.

**Datenschutz:** Für Cover und Senderlogos werden Songtitel und Sendername an iTunes, Deezer und radio-browser.info
geschickt. Abschalten: `cover_lookup=0` in `Dokumente\Euro Truck Simulator 2\CoachNowPlaying\CoachNowPlaying.ini`.
