# CoachNowPlaying for Linux (ETS2 under Proton)

A Linux build of [CoachNowPlaying](https://github.com/videtess/CoachNowPlaying) by videtess, the plugin for the
**"CarPlay-Style Infotainment for Coaches"** mod. Everything works like on Windows. The difference is the music page:
it shows **whatever your system is playing**, including songs on **[ETS2/ATS Local Radio](https://github.com/Koenvh1/ets2-local-radio)**.

## Why a Linux build

On Windows, the music page reads the Windows media controls. Wine/Proton doesn't have those, so under Linux the page
stayed empty. This build swaps that one part (`src/media_mpris.cpp` instead of `src/media.cpp`) for a small native
helper, **`coach-mpris-bridge`**, which reads the "now playing" info of your Linux desktop (MPRIS):

- **Any player:** Spotify, VLC, a YouTube tab, … The title, artist, cover, progress bar and the play/pause/next/previous
  buttons all work.
- **ETS2/ATS Local Radio:** works with the [Linux version of Local Radio](https://github.com/KoJesko/ets2-local-radio)
  (a port of [Koenvh's Local Radio](https://github.com/Koenvh1/ets2-local-radio)). Local Radio only knows the station,
  not the song, so the bridge finds the song itself:
  1. If the station sends the song in its stream ("Artist - Title"), that's used. It's exact and instant.
  2. If it doesn't, the bridge records about 12 s of **the browser's audio only** (not the game) and identifies the song
     with [SongRec](https://github.com/marin-m/SongRec) (Shazam). This repeats every 30 s while needed.

  The screen **alternates every 15 s** between the song (with its album cover) and the **station name** (with the
  station logo). **Next/Previous** on the screen switch the Local Radio station, and **Play/Pause** pauses it.

### What the screen shows for Local Radio

The second line while the **station name** shows (first rule that matches):

| Station | Line |
|---|---|
| North Korea (평양FM방송) | 존경하는 동지께서 깊은 존경을 담아 전합니다. |
| World of Trucks Christmas / Halloween channels | The Official North Pole Station / The Official Station of Witches Brew |
| Radio Vinci Autoroutes | *Vous n'êtes jamais à 3 chansons de Traffic.* · Vinci Autoroutes Radio. |
| American public radio (NPR, "Public Radio", WNYC, KQED, OPB) | Your listener supported radio |
| ESPN | The Worldwide Leader in Sports |
| Other sports stations | Your home of sports |
| Talk / news (detected) | Your trusted news and talk station |
| EDM (Shazam genre or name) | The Station with Real DJs |
| Everything else | *Iceland's* best *Pop* station (region from Local Radio, genre from Shazam) |

The **song** half: the identified song (stream title or Shazam). Nothing for talk shows or when nothing could be
identified, except on EDM stations: *No song identified · Likely DJ mix*.

- **Talk detection:** Shazam misses at least twice in a row and [YAMNet](https://huggingface.co/qualcomm/YamNet) hears
  mostly speech in the recent clips (or the name says news/talk/info, until songs get identified). Traffic stations
  (Vinci, Sanef 107.7, Autoroute Info, Rai Isoradio, Traffic FM, ...) are never treated as talk; their songs are shown.
- **Genres:** Shazam's "Christian & Gospel" is only used for Christian stations, so Quran stations (e.g. Ramallah,
  ERTU, Diyanet) and Jewish religious stations (Kan Moreshet) never read as Christian.
- **Regions:** each station is labelled with its own Local Radio region, e.g. Israel and West Bank stations each
  under their own name; Gaza / Palestine are named properly if such regions are added.

The rest of the plugin (arrival clock, speed limit, passenger TV, announcements, number plate, start-up animation) is
unchanged upstream code.

## Install

Needs: ETS2 running under Proton, Python 3 with PyGObject (preinstalled on KDE/GNOME), and optionally `songrec` and
PipeWire's `pw-record` for song recognition. For talk detection the installer creates a small Python environment with ONNX
Runtime and downloads the YAMNet model (14 MB; skip with `NO_YAMNET=1`).

```bash
sudo pacman -S songrec          # Debian/Ubuntu/Fedora: see SongRec's README, or the Flatpak re.fossplant.songrec
git clone -b linux https://github.com/KoJesko/CoachNowPlaying.git
cd CoachNowPlaying
./linux/install.sh              # --game-dir "<path>/Euro Truck Simulator 2" if it's not in ~/.local/share/Steam
```

The installer:
1. **Builds the DLL** if [llvm-mingw](https://github.com/mstorsjo/llvm-mingw) is in `~/.local/opt/`. Otherwise, put
   `CoachNowPlaying.dll` from the Linux release into `linux/` first.
2. **Copies it** into `bin/win_x64/plugins/`, backing up an existing Windows build.
3. **Starts the bridge** as a systemd user service at login.

Copy the `CoachNowPlaying_piper` folder from the upstream release ZIP too if you want the voice announcements.

### Announcements on coach jobs

On a Coaches DLC job this build reads the game's `bus_job` data and announces **every stop by name** a few minutes
before it ("our next stop is Egilsstaðir…"), and the end of the trip at the last stop. The passenger TV shows the next
stop. Upstream only knew the freight job, so it said "our destination" before whatever the GPS pointed at.

### Without the mod (TruckersMP): CarPlay on the game's own cab screen

TruckersMP does not load Workshop mods, so the mod's placeholder textures never appear there. With
`[screen] gps_takeover=1` the plugin instead shows its picture on the game's own cab screen. The game draws that
screen into a 1024x1024 RGBA8 render target, found with upstream's texture finder (`[debug] texture_finder=1`).
A coach has two textures that look like it, so **Ctrl+Shift+F10** moves the picture to the next one, and after the
last one back to the stock GPS. The same key switches between CarPlay and the map while driving.
`gps_takeover_pick` (default 0) is the one it starts with. Leave this off when the mod is loaded, or use
`gps_takeover=auto`: on like `1`, but it switches itself off as soon as the mod's own CarPlay screen loads (single
player with the mod), so one setting works for both. Don't let PrismTextureStreamer take over the GPS screen at the same
time either.

On the coaches tested (Volvo 9700, MAN Lion's Coach, Scania Touring) use `gps_takeover_pick=1`: the game keeps the cab
screen texture in pairs, and the second one is what the displays show, so CarPlay can be limited to one display there.
Where two displays show it (Volvo 9700: the centre screen and the middle of the instrument cluster),
**Ctrl+Shift+F11** picks which one shows CarPlay; the other keeps the game's picture (saved as `gps_takeover_hide`).
The picture is fitted to the display by itself.

Pages, like the mod's: the game's **Infotainment** button blanks the screen, and every time it comes back the next
page shows: **Map** (the game's map with the now-playing bar), **Music**, **Trip** (arrival time, distance and time
left, next coach stop, speed limit). On Music and Trip the game's own buttons and signs on that screen are hidden. The
blank page in between comes from the game and cannot be skipped without the mod.

Credit: [PrismTextureStreamer](https://github.com/Baldywaldy09/PrismTextureStreamer) by Baldywaldy09 showed that a
plain SDK plugin can take over a cab screen on TruckersMP. Its code was not used (it has no licence). This build
reuses CoachNowPlaying's own view swap. For Prism itself on Linux/Wayland, see
[gabors0's fork](https://github.com/gabors0/PrismTextureStreamer) (issue #9 upstream).

### English announcements

Since 0.8.0 the plugin picks the Piper voice for each announcement language from `CoachNowPlaying_piper/voice/`
(see upstream's README for the language list). For English, put an English voice there, both the `.onnx` and the
`.onnx.json` file. One example is "Ryan" (high quality, US English, CC BY-NC-SA 4.0, so it isn't included here):

- [en_US-ryan-high.onnx](https://huggingface.co/rhasspy/piper-voices/resolve/v1.0.0/en/en_US/ryan/high/en_US-ryan-high.onnx)
- [en_US-ryan-high.onnx.json](https://huggingface.co/rhasspy/piper-voices/resolve/v1.0.0/en/en_US/ryan/high/en_US-ryan-high.onnx.json)

Other voices: [rhasspy/piper-voices](https://huggingface.co/rhasspy/piper-voices/tree/v1.0.0/en). Then set
`[announce] language=en` in the INI (or keep `auto` with the game in English). Coach stops along the line get
"our next stop is …" in English and German. Other languages use the normal arrival text, which also names the stop.
Without a Piper voice for the language, the Windows speech voice is used. Under Proton that needs the Steam tool "Proton Voice Files" (`steam://install/3086180`).

For Local Radio, use the Linux port linked above (it tells the bridge which station plays). With **Plasma browser
integration** (KDE) the bridge also sees the stream address and can use the station's own song titles; without it,
songs are identified with SongRec only.

## Settings

The INI is the same as on Windows (`Documents/Euro Truck Simulator 2/CoachNowPlaying/CoachNowPlaying.ini`):

- `[media] app_filter=` is empty by default in this build (= any player). Put e.g. `spotify` there to show only Spotify.
  Local Radio is always shown.
- Bridge port: `CNP_BRIDGE_PORT` (default 8332), set for both the game and the service.
- Local Radio server: `LOCAL_RADIO_URL` (default `http://127.0.0.1:8330`).

## Troubleshooting

- `diag.log` (next to the INI) shows `MEDIA bridge not reachable` → `systemctl --user status coach-mpris-bridge`.
- To see what the bridge sees: `curl http://127.0.0.1:8332/nowplaying` and `journalctl --user -u coach-mpris-bridge -f`.

## Building by hand

```bash
cmake -S . -B build-linux -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=linux/toolchain-llvm-mingw.cmake
cmake --build build-linux
```

The Windows (MSVC) build is unchanged. `build.bat` still produces the original SMTC version.

## Roadmap

- **Truck version:** the same now-playing source (system audio + Local Radio) for trucks, not just coaches.

## Credits

[CoachNowPlaying](https://github.com/videtess/CoachNowPlaying) by videtess, based on
[EtsNowPlaying](https://github.com/jxhnch/EtsNowPlaying) by jxhnch (MIT). Local Radio:
[Koenvh1/ets2-local-radio](https://github.com/Koenvh1/ets2-local-radio) (MPL-2.0); Linux port
[KoJesko/ets2-local-radio](https://github.com/KoJesko/ets2-local-radio). Song recognition:
[SongRec](https://github.com/marin-m/SongRec) (GPL-3.0, used as a separate program).
