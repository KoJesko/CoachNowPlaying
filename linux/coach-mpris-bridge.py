#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""
coach-mpris-bridge - "now playing" for CoachNowPlaying under Wine/Proton.

CoachNowPlaying's Windows build reads the Windows media controls, which Wine doesn't have. The Linux build of the
plugin asks this helper instead:

* Any player: reads MPRIS (the "now playing" interface of Linux desktops, the same thing the KDE/GNOME media widgets
  show): Spotify, VLC, a browser tab, ...
* ETS2/ATS Local Radio (https://github.com/Koenvh1/ets2-local-radio, Linux port:
  https://github.com/KoJesko/ets2-local-radio): Local Radio only knows the station, not the song. The bridge asks the
  Local Radio server which station plays, then gets the song
    1. from the stream itself (ICY "StreamTitle"), when the station sends a real "Artist - Title", otherwise
    2. by recognising ~12 s of the browser's audio with SongRec (Shazam), every 30 s while needed.
  The screen alternates every 15 s between the song and the station name.

Serves plain HTTP on 127.0.0.1 only (port: env CNP_BRIDGE_PORT or 8332):
  GET /nowplaying?filter=<text>  key=value lines (has, app, title, artist, album, playing, duration, position, cover)
  GET /cover                     image bytes of the current cover
  GET /cmd/previous|playpause|next   (for Local Radio, previous/next switch the station)
  GET /seek?s=<seconds>
Needs PyGObject (python-gobject / python3-gi). Optional: songrec + pw-record (PipeWire) for song recognition,
rsvg-convert for SVG station logos.
"""

import base64
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import urllib.parse
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

from gi.repository import Gio, GLib

MPRIS_PREFIX = "org.mpris.MediaPlayer2."
PLAYER_IFACE = "org.mpris.MediaPlayer2.Player"
ROOT_IFACE = "org.mpris.MediaPlayer2"
SKIP_NAMES = ("playerctld",)  # playerctl's aggregator mirrors other players
MAX_COVER = 8 * 1024 * 1024

ROTATE_SECONDS = 15        # song <-> station name
ICY_INTERVAL = 20          # seconds between stream-title checks
RECOGNIZE_INTERVAL = 30    # seconds between SongRec attempts while no stream title
RECORD_SECONDS = 12
SONG_MAX_AGE = 240         # a recognised song is shown for at most this long without a new match
GENRE_INTERVAL_NEW = 60    # stations with their own song titles: Shazam check for the genre until it's known...
GENRE_INTERVAL = 300       # ...then every 5 minutes

LOCAL_RADIO = (os.environ.get("LOCAL_RADIO_URL") or "http://127.0.0.1:8330").rstrip("/")
LOCAL_RADIO_WEB = os.environ.get("LOCAL_RADIO_WEB") or ""  # web/ folder for station lists + logos (auto-detected)

bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)
lock = threading.Lock()
state = {"chosen": None, "identity": {}, "cover_key": None, "cover_bytes": None, "cover_src": None}


def log(*a):
    print(time.strftime("%H:%M:%S"), *a, file=sys.stderr, flush=True)


# --------------------------------------------------------------------------- MPRIS

def call(name, path, iface, method, args=None, reply=None, timeout=800):
    return bus.call_sync(name, path, iface, method, args, reply and GLib.VariantType(reply),
                         Gio.DBusCallFlags.NONE, timeout, None)


def get_all(name, iface):
    try:
        v = call(name, "/org/mpris/MediaPlayer2", "org.freedesktop.DBus.Properties", "GetAll",
                 GLib.Variant("(s)", (iface,)), "(a{sv})")
        return v.unpack()[0]
    except GLib.Error:
        return None


def identity(name):
    if name not in state["identity"]:
        props = get_all(name, ROOT_IFACE) or {}
        state["identity"][name] = props.get("Identity") or name[len(MPRIS_PREFIX):].split(".")[0]
    return state["identity"][name]


def players():
    """(name, Player properties) of all players. A browser tab that Plasma's browser integration also publishes
    appears twice; the integration's copy (which knows the page URL and stream) is kept."""
    names = call("org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "ListNames",
                 None, "(as)").unpack()[0]
    out, pbi_pids = [], set()
    for n in sorted(names):
        if not n.startswith(MPRIS_PREFIX) or n[len(MPRIS_PREFIX):].split(".")[0] in SKIP_NAMES:
            continue
        p = get_all(n, PLAYER_IFACE)
        if p is None:
            continue
        pid = (p.get("Metadata") or {}).get("kde:pid")
        if pid:
            pbi_pids.add(str(pid))
        out.append((n, p))
    return [(n, p) for n, p in out if not any(n.endswith(f".instance{pid}") for pid in pbi_pids)]


def is_local_radio(p):
    md = p.get("Metadata") or {}
    url = str(md.get("xesam:url") or "")
    host = urllib.parse.urlsplit(LOCAL_RADIO).netloc
    return (host and host in url) or str(md.get("xesam:title") or "").startswith("ETS2/ATS Local Radio")


def pick(filter_text):
    """A playing player wins (the current one while it plays), then the last chosen, then a paused one."""
    f = (filter_text or "").strip().lower()
    cands = [(n, p) for n, p in players()
             if not f or f in n.lower() or f in identity(n).lower() or is_local_radio(p)]
    if not cands:
        return None, None
    chosen = state["chosen"]
    playing = [c for c in cands if c[1].get("PlaybackStatus") == "Playing"]
    if playing:
        return next((c for c in playing if c[0] == chosen), playing[0])
    for c in cands:
        if c[0] == chosen:
            return c
    paused = [c for c in cands if c[1].get("PlaybackStatus") == "Paused"]
    return (paused or cands)[0]


def clean(s):
    if isinstance(s, (list, tuple)):
        s = ", ".join(str(x) for x in s if x)
    return str(s or "").replace("\r", " ").replace("\n", " ").strip()


# --------------------------------------------------------------------------- Local Radio

def find_web_dir():
    if LOCAL_RADIO_WEB and os.path.isdir(LOCAL_RADIO_WEB):
        return LOCAL_RADIO_WEB
    link = shutil.which("ets2-local-radio")  # the Linux port's launcher symlink -> <repo>/linux/server/...
    if link:
        web = os.path.realpath(os.path.join(os.path.dirname(os.path.realpath(link)), "..", "..", "web"))
        if os.path.isdir(web):
            return web
    for guess in ("~/ets2-local-radio/web", "~/GitHub/ets2-local-radio/web"):
        if os.path.isdir(os.path.expanduser(guess)):
            return os.path.expanduser(guess)
    return None


WEB_DIR = find_web_dir()
_index = None


def station_index():
    """Station lists of Local Radio: stream url / (name, logo) / name -> (name, logo, country key), plus the
    English names of the country keys (cities files' country_properties)."""
    global _index
    if _index is None:
        by_url, by_name_logo, by_name, countries = {}, {}, {}, {}
        for f in ("stations-europe.js", "stations-america.js"):
            try:
                src = open(os.path.join(WEB_DIR or "", "stations", f), encoding="utf-8").read()
            except OSError:
                continue
            key = ""
            for m in re.finditer(r'^\s{4}"([^"]+)":\s*\[|name:\s*"([^"]*)",\s*logo:\s*"([^"]*)",\s*url:\s*"([^"]*)"',
                                 src, re.M):
                if m.group(1):
                    key = m.group(1)
                    continue
                entry = (m.group(2), m.group(3), key)
                by_url.setdefault(m.group(4).rstrip("/"), entry)
                by_name_logo.setdefault((m.group(2), m.group(3)), entry)
                by_name.setdefault(m.group(2), entry)
        for f in ("cities-ets2.js", "cities-ats.js"):
            try:
                src = open(os.path.join(WEB_DIR or "", "cities", f), encoding="utf-8").read()
            except OSError:
                continue
            props = src[src.find("country_properties"):]
            for k, name in re.findall(r'"([^"]+)":\s*\{[^{}]*?name_english:\s*"([^"]*)"', props):
                countries.setdefault(k, re.sub(r"\s*\[.*?\]\s*$", "", name).strip())
        _index = (by_url, by_name_logo, by_name, countries)
    return _index


def country_name(key, logo):
    countries = station_index()[3]
    if key in countries:
        return countries[key]
    folder = (logo or "").split("/")[2] if (logo or "").count("/") >= 3 else ""
    if folder in countries:
        return countries[folder]
    base = (key or folder).split("-")[0].replace("_", " ")
    return base.title() if base and not base[0].isdigit() else ""


def local_radio_station(stream_url):
    """(name, logo path, country name) of the station Local Radio plays."""
    by_url, by_name_logo, by_name, _ = station_index()
    name = logo = ""
    try:
        with urllib.request.urlopen(LOCAL_RADIO + "/nowplaying/", timeout=1) as r:
            d = json.load(r)
        name, logo = d.get("station") or "", d.get("logo") or ""
    except (OSError, ValueError):
        pass
    entry = by_name_logo.get((name, logo)) or (name and by_name.get(name)) or by_url.get((stream_url or "").rstrip("/"))
    if entry and not name:
        name, logo = entry[0], entry[1]
    return name, logo, country_name(entry[2] if entry else "", logo)


def local_radio_control(action):
    try:
        urllib.request.urlopen(f"{LOCAL_RADIO}/control/{action}", timeout=2).read()
        return True
    except OSError as e:
        log(f"local radio {action}: {e}")
        return False


SONG_RE = re.compile(r"^\s*(?P<artist>[^-–]+?)\s+[-–]\s+(?P<title>.+?)\s*$")


def split_song(text, station):
    """('artist', 'title') if an ICY title looks like a song, else None (station/show names, slogans, URLs)."""
    t = (text or "").strip()
    if not t or len(t) < 5 or t.lower() in (station or "").lower():
        return None
    if re.search(r"https?://|www\.|\.(com|de|fm|nl|uk|fr|it|pl|ru|net|org)\b", t, re.I):
        return None
    m = SONG_RE.match(t)
    if not m:
        return None
    artist, title = m.group("artist").strip(), m.group("title").strip()
    st = (station or "").lower()
    if not artist or not title or (st and (st in artist.lower() or artist.lower() in st)):
        return None
    return artist, title


class LocalRadioSong:
    """Background worker: knows the song on the current Local Radio stream (ICY title or SongRec)."""

    def __init__(self):
        self.cv = threading.Condition()
        self.url = None
        self.station = ""
        self.pid = None
        self.active_until = 0.0
        self.icy = None          # (artist, title) or None
        self.rec = None          # (artist, title, cover_url, when)
        self.genres = {}         # station -> {genre: count} from Shazam
        self.last_icy = 0.0
        self.last_rec = 0.0
        self.warned = set()
        threading.Thread(target=self.run, daemon=True).start()

    def want(self, url, station, pid):
        with self.cv:
            if url != self.url:
                self.url, self.icy, self.rec = url, None, None
                self.last_icy = self.last_rec = 0.0
                log(f"local radio: {station or '?'} ({url})")
            self.station, self.pid = station, pid
            self.active_until = time.monotonic() + 5
            self.cv.notify_all()

    def genre(self, station):
        """Most common Shazam genre of the songs heard on this station, or ''."""
        with self.cv:
            counts = self.genres.get(station) or {}
        return max(counts, key=counts.get) if counts else ""

    def song(self):
        """(artist, title, cover_url or None, source) or None."""
        with self.cv:
            if self.icy:
                return self.icy[0], self.icy[1], None, "stream"
            if self.rec and time.time() - self.rec[3] < SONG_MAX_AGE:
                return self.rec[0], self.rec[1], self.rec[2], "songrec"
        return None

    def run(self):
        while True:
            with self.cv:
                self.cv.wait(timeout=1)
                if time.monotonic() > self.active_until or not self.url:
                    continue
                url, station, pid = self.url, self.station, self.pid
                due_icy = time.monotonic() - self.last_icy > ICY_INTERVAL
            if due_icy:
                icy = self.read_icy(url) if url.startswith(("http://", "https://")) else ""
                with self.cv:
                    if url == self.url:
                        self.last_icy = time.monotonic()
                        song = split_song(icy, station)
                        if song != self.icy:
                            log(f"stream title: {icy!r} -> {'song' if song else 'not a song'}")
                        self.icy = song
            with self.cv:
                since = time.monotonic() - self.last_rec
                if self.icy:  # the stream names the song; Shazam only to learn the station's genre
                    need_rec = since > (GENRE_INTERVAL if self.genres.get(station) else GENRE_INTERVAL_NEW)
                else:
                    need_rec = since > RECOGNIZE_INTERVAL
                need_rec = need_rec and url == self.url
            if need_rec:
                with self.cv:
                    self.last_rec = time.monotonic()
                found = self.recognize(pid)
                with self.cv:
                    if found and url == self.url:
                        artist, title, cover, genre = found
                        if not self.rec or self.rec[:2] != (artist, title):
                            log(f"songrec: {artist} - {title} [{genre or 'no genre'}]")
                            if genre:
                                counts = self.genres.setdefault(station, {})
                                counts[genre] = counts.get(genre, 0) + 1
                        self.rec = (artist, title, cover, time.time())

    @staticmethod
    def read_icy(url):
        try:
            req = urllib.request.Request(url, headers={"Icy-MetaData": "1", "User-Agent": "VLC/3.0.20 LibVLC/3.0.20"})
            with urllib.request.urlopen(req, timeout=8) as r:
                metaint = int(r.headers.get("icy-metaint") or 0)
                if not metaint or metaint > 256 * 1024:
                    return ""
                r.read(metaint)
                n = r.read(1)
                raw = r.read(n[0] * 16) if n else b""
        except (OSError, ValueError):
            return ""
        for enc in ("utf-8", "latin-1"):
            try:
                text = raw.decode(enc)
                break
            except UnicodeDecodeError:
                continue
        m = re.search(r"StreamTitle='(.*?)';", text, re.S)
        return m.group(1).strip() if m else ""

    def recognize(self, pid):
        if not (shutil.which("songrec") and shutil.which("pw-record")):
            if "tools" not in self.warned:
                self.warned.add("tools")
                log("songrec/pw-record not installed - only stream titles are available")
            return None
        target = browser_stream(pid)
        if not target:
            return None
        with tempfile.TemporaryDirectory(prefix="cnp-rec-") as d:
            wav = os.path.join(d, "a.wav")
            try:
                subprocess.run(["timeout", str(RECORD_SECONDS), "pw-record", "--target", str(target),
                                "--rate", "44100", "--channels", "2", wav],
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=RECORD_SECONDS + 5)
                out = subprocess.run(["songrec", "recognize", "-j", wav], capture_output=True, text=True, timeout=30)
                track = (json.loads(out.stdout or "{}").get("track") or {})
            except (subprocess.SubprocessError, ValueError, OSError) as e:
                log(f"songrec failed: {e}")
                return None
        if not track.get("title"):
            return None
        cover = (track.get("images") or {}).get("coverarthq") or (track.get("images") or {}).get("coverart")
        genre = ((track.get("genres") or {}).get("primary") or "").strip()
        return track.get("subtitle") or "", track["title"], cover, genre


def browser_stream(pid):
    """PipeWire playback stream (sink-input) of the browser process, so only the radio is recorded, not the game."""
    try:
        data = json.loads(subprocess.run(["pactl", "-f", "json", "list", "sink-inputs"], capture_output=True,
                                         text=True, timeout=5).stdout or "[]")
    except (subprocess.SubprocessError, ValueError, OSError):
        return None
    best = None
    for s in data:
        p = s.get("properties") or {}
        if pid and str(p.get("application.process.id")) == str(pid):
            return s.get("index")
        binary = str(p.get("application.process.binary") or "").lower()
        if best is None and any(b in binary for b in ("brave", "chrom", "firefox", "librewolf", "vivaldi", "opera")):
            best = s.get("index")
    return best


lr_song = LocalRadioSong()


# --------------------------------------------------------------------------- covers

def load_image(src):
    """Bytes of an image given as file:// / http(s):// / data: URL or a path inside the Local Radio web folder."""
    if not src:
        return None
    try:
        if src.startswith("file://"):
            path = urllib.parse.unquote(urllib.parse.urlparse(src).path)
        elif src.startswith("data:"):
            header, _, data = src.partition(",")
            return base64.b64decode(data) if ";base64" in header else urllib.parse.unquote_to_bytes(data)
        elif src.startswith(("http://", "https://")):
            req = urllib.request.Request(src, headers={"User-Agent": "coach-mpris-bridge"})
            with urllib.request.urlopen(req, timeout=5) as r:
                return r.read(MAX_COVER + 1)[:MAX_COVER]
        else:
            path = os.path.join(WEB_DIR or "", src)
        if path.lower().endswith(".svg"):
            if not shutil.which("rsvg-convert"):
                return None
            return subprocess.run(["rsvg-convert", "-w", "256", "-f", "png", path], capture_output=True,
                                  timeout=10).stdout or None
        if os.path.getsize(path) > MAX_COVER:
            return None
        with open(path, "rb") as f:
            return f.read()
    except (OSError, ValueError, subprocess.SubprocessError) as e:
        log(f"cover: {str(src)[:80]}: {e}")
    return None


# --------------------------------------------------------------------------- snapshot

def snapshot(filter_text):
    name, p = pick(filter_text)
    with lock:
        if name != state["chosen"]:
            log(f"player: {identity(name) if name else 'none'} ({name or '-'})")
            state["chosen"] = name
    if not name:
        return "has=0\n"
    md = p.get("Metadata") or {}
    playing = p.get("PlaybackStatus") == "Playing"

    if is_local_radio(p):
        stream = str(md.get("kde:mediaSrc") or "")
        station, logo, country = local_radio_station(stream)
        if playing and (stream or station):
            # Without Plasma's browser integration there's no stream address: key on the station, SongRec only.
            lr_song.want(stream or f"station:{station}", station, md.get("kde:pid"))
        song = lr_song.song() if playing else None
        show_song = song and int(time.time() // ROTATE_SECONDS) % 2 == 0
        if show_song:
            artist, title, cover_url, _ = song
            cover = cover_url or logo
        else:
            genre = lr_song.genre(station)
            best = f"best {genre} station" if genre else "best station"
            possessive = country + ("'" if country.endswith("s") else "'s")  # Netherlands' / Iceland's
            artist = f"{possessive} {best}" if country else best[0].upper() + best[1:]
            title, cover = station or "Local Radio", logo
        lines = {"app": "ETS2 Local Radio", "title": title, "artist": artist, "album": station,
                 "duration": "0", "position": "0"}
    else:
        length = (md.get("mpris:length") or 0) / 1e6
        try:  # Position is not part of PropertiesChanged; ask for it directly
            pos = call(name, "/org/mpris/MediaPlayer2", "org.freedesktop.DBus.Properties", "Get",
                       GLib.Variant("(ss)", (PLAYER_IFACE, "Position")), "(v)").unpack()[0] / 1e6
        except (GLib.Error, TypeError):
            pos = 0.0
        cover = md.get("mpris:artUrl") or ""
        lines = {"app": identity(name), "title": md.get("xesam:title"), "artist": md.get("xesam:artist"),
                 "album": md.get("xesam:album"), "duration": f"{max(length, 0):.3f}", "position": f"{max(pos, 0):.3f}"}

    with lock:
        if cover != state["cover_src"]:
            state["cover_src"] = cover
            state["cover_bytes"] = None
    out = {"has": "1", **{k: clean(v) for k, v in lines.items()}, "playing": "1" if playing else "0",
           "cover": hashlib.sha1(cover.encode()).hexdigest()[:12] if cover else ""}
    return "".join(f"{k}={v}\n" for k, v in out.items())


# --------------------------------------------------------------------------- commands

def chosen_props():
    name = state["chosen"]
    return name, (get_all(name, PLAYER_IFACE) or {}) if name else {}


def command(action):
    name, p = chosen_props()
    if not name:
        return False
    if is_local_radio(p) and action in ("next", "previous"):
        return local_radio_control(action)  # next/previous station in Local Radio
    method = {"previous": "Previous", "playpause": "PlayPause", "next": "Next"}[action]
    try:
        call(name, "/org/mpris/MediaPlayer2", PLAYER_IFACE, method)
        return True
    except GLib.Error as e:
        log(f"{method}: {e.message}")
        return False


def seek(seconds):
    name, p = chosen_props()
    if not name or is_local_radio(p):
        return False
    track = (p.get("Metadata") or {}).get("mpris:trackid")
    us = int(max(seconds, 0) * 1e6)
    try:
        if track:
            call(name, "/org/mpris/MediaPlayer2", PLAYER_IFACE, "SetPosition", GLib.Variant("(ox)", (track, us)))
        else:
            call(name, "/org/mpris/MediaPlayer2", PLAYER_IFACE, "Seek",
                 GLib.Variant("(x)", (us - (p.get("Position") or 0),)))
        return True
    except GLib.Error as e:
        log(f"seek: {e.message}")
        return False


# --------------------------------------------------------------------------- HTTP

class Handler(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def send(self, code, body=b"", ctype="text/plain; charset=utf-8"):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        u = urllib.parse.urlsplit(self.path)
        q = urllib.parse.parse_qs(u.query)
        try:
            if u.path == "/nowplaying":
                return self.send(200, snapshot(q.get("filter", [""])[0]).encode())
            if u.path == "/cover":
                with lock:
                    src, data = state["cover_src"], state["cover_bytes"]
                if data is None and src:
                    data = load_image(src)
                    with lock:
                        if state["cover_src"] == src:
                            state["cover_bytes"] = data
                if not data:
                    return self.send(404)
                return self.send(200, data, "image/png" if data[:4] == b"\x89PNG" else "image/jpeg")
            if u.path in ("/cmd/previous", "/cmd/playpause", "/cmd/next"):
                return self.send(200 if command(u.path[5:]) else 409)
            if u.path == "/seek":
                return self.send(200 if seek(float(q.get("s", ["0"])[0])) else 409)
            return self.send(404)
        except (BrokenPipeError, ConnectionResetError):
            pass
        except Exception as e:  # keep serving whatever a player does
            log(f"{u.path}: {e!r}")
            try:
                self.send(500)
            except OSError:
                pass


def main():
    port = int(os.environ.get("CNP_BRIDGE_PORT") or 8332)
    try:
        srv = ThreadingHTTPServer(("127.0.0.1", port), Handler)
    except OSError as e:
        sys.exit(f"cannot listen on 127.0.0.1:{port}: {e}")
    srv.daemon_threads = True
    log(f"coach-mpris-bridge on http://127.0.0.1:{port}/nowplaying - players: "
        + (", ".join(identity(n) for n, _ in players()) or "none")
        + f" - Local Radio: {LOCAL_RADIO} (web: {WEB_DIR or 'not found'})"
        + (" - songrec ready" if shutil.which("songrec") and shutil.which("pw-record") else " - songrec missing"))
    srv.serve_forever()


if __name__ == "__main__":
    main()
