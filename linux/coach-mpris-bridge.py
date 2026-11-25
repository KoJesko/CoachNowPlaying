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
import ipaddress
import json
import os
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time
import unicodedata
import urllib.error
import urllib.parse
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

from gi.repository import Gio, GLib

sys.path.insert(0, os.path.dirname(os.path.realpath(__file__)))  # yamnet_cls.py next to this file

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
TALK_INTERVAL = 120        # once a station is known to be talk, check less often (music may come back)
COVER_FIRST_DELAY = 10     # stations with their own song titles: Shazam this long after a new title, for the cover...
COVER_RETRY = 20           # ...again after this long if it heard something else (ad, DJ)...
COVER_TRIES = 3            # ...at most this many times per song
MODEL_DIR = os.environ.get("CNP_MODEL_DIR") or os.path.expanduser("~/.local/share/coach-mpris-bridge/model/yamnet-onnx-float")

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
    """Station lists of Local Radio: stream url / (name, logo) / name -> ALL matching entries (name, logo, country key,
    europe|america), the English names of the country keys, and city coordinates per country key.
    Many stations share a name, logo or stream across regions (KISS FM: 7 regions), so callers pick by position."""
    global _index
    if _index is None:
        by_url, by_name_logo, by_name, countries, cities = {}, {}, {}, {}, {}
        for f in ("stations-europe.js", "stations-america.js"):
            region = "america" if "america" in f else "europe"
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
                entry = (m.group(2), m.group(3), key, region)
                by_url.setdefault(m.group(4).rstrip("/"), []).append(entry)
                by_name_logo.setdefault((m.group(2), m.group(3)), []).append(entry)
                by_name.setdefault(m.group(2), []).append(entry)
        for f in ("cities-ets2.js", "cities-ats.js"):
            try:
                src = open(os.path.join(WEB_DIR or "", "cities", f), encoding="utf-8").read()
            except OSError:
                continue
            props = src[src.find("country_properties"):]
            for k, name in re.findall(r'"([^"]+)":\s*\{[^{}]*?name_english:\s*"([^"]*)"', props):
                countries.setdefault(k, re.sub(r"\s*\[.*?\]\s*$", "", name).strip())
            for k, x, z in re.findall(r'"country":\s*"([^"]+)",\s*"x":\s*"?(-?[\d.]+)"?,\s*"y":\s*"?[-\d.]*"?,\s*'
                                      r'"z":\s*"?(-?[\d.]+)"?', src):
                cities.setdefault(k.lower(), []).append((float(x), float(z)))
        _index = (by_url, by_name_logo, by_name, countries, cities)
    return _index


_pos_cache = {"at": 0.0, "pos": None, "game": ""}


def truck_position():
    """(x, z) of the truck from Local Radio's /api/ (cached 2 s), or None while not driving."""
    if time.monotonic() - _pos_cache["at"] > 2:
        _pos_cache["at"] = time.monotonic()
        try:
            with urllib.request.urlopen(LOCAL_RADIO + "/api/", timeout=1) as r:
                d = json.load(r)
            p = d["TruckValues"]["Positioning"]["HeadPositionInWorldSpace"]
            _pos_cache["pos"] = (p["X"], p["Z"]) if d.get("SdkActive") and (p["X"] or p["Z"]) else None
            _pos_cache["game"] = {"Ets2": "europe", "Ats": "america"}.get(d.get("Game"), _pos_cache["game"])
        except (OSError, ValueError, KeyError, TypeError):
            _pos_cache["pos"] = None
    return _pos_cache["pos"], _pos_cache["game"]


def pick_entry(entries):
    """The one of several same-named stations that is in the region where the truck is: the candidate whose
    nearest city is closest. Without a position: the current game's map (ETS2 = europe, ATS = america), real
    regions before the World of Trucks event channels."""
    entries = list(dict.fromkeys(entries or []))
    if len(entries) <= 1:
        return entries[0] if entries else None
    cities = station_index()[4]
    pos, game = truck_position()

    def score(e):
        event = e[2].lower() in EVENT_LINES
        dist = float("inf")
        if pos:
            for x, z in cities.get(e[2].lower(), ()):
                dist = min(dist, (x - pos[0]) ** 2 + (z - pos[1]) ** 2)
        return (dist, e[3] != game if game else False, event)
    return min(entries, key=score)


def normalize_logo(logo):
    """The page reports logos with its URL prefix (https://localradio.koenvh.nl/stations/...): keep stations/..."""
    i = (logo or "").find("stations/")
    return logo[i:] if i > 0 else (logo or "")


def country_name(key, logo):
    countries = station_index()[3]
    if key in countries:
        return countries[key]
    if key in REGION_NAMES:
        return REGION_NAMES[key]
    folder = (logo or "").split("/")[2] if (logo or "").count("/") >= 3 else ""
    if folder in countries:
        return countries[folder]
    base = (key or folder).split("-")[0].replace("_", " ")
    return base.title() if base and not base[0].isdigit() else ""


def local_radio_station(stream_url):
    """The station Local Radio plays: name, logo path, country key + English name, region (europe/america)."""
    by_url, by_name_logo, by_name, _, _ = station_index()
    name = logo = ""
    try:
        with urllib.request.urlopen(LOCAL_RADIO + "/nowplaying/", timeout=1) as r:
            d = json.load(r)
        name, logo = d.get("station") or "", normalize_logo(d.get("logo") or "")
    except (OSError, ValueError):
        pass
    stream = (stream_url or "").rstrip("/")
    # Most specific first; when several regions still fit, the truck's position decides (pick_entry).
    entry = (pick_entry(by_name_logo.get((name, logo))) or
             pick_entry([e for e in by_url.get(stream, []) if not name or e[0] == name]) or
             pick_entry(by_name.get(name)) or pick_entry(by_url.get(stream)))
    if entry and not name:
        name, logo = entry[0], entry[1]
    key = entry[2] if entry else ""
    return {"name": name, "logo": logo or (entry[1] if entry else ""), "key": key,
            "country": country_name(key, logo), "region": entry[3] if entry else ""}


def local_radio_control(action):
    try:
        urllib.request.urlopen(f"{LOCAL_RADIO}/control/{action}", timeout=2).read()
        return True
    except OSError as e:
        log(f"local radio {action}: {e}")
        return False


# Station rules (station line = the second line while the station name shows).
# Traffic stations (Vinci, Sanef 107.7, Autoroute Info, Rai Isoradio, Traffic Radio NL, France Bleu Haute Normandie,
# tbn, Traffic FM...): songs are shown when there are any, and they are never treated as talk.
TRAFFIC_RE = re.compile(r"vinci|autoroute|sanef|isoradio|\btraffic\b|\btrafic\b|verkehr|bleu haute.?normandie", re.I)
TALK_NAME_RE = re.compile(r"\b(news|talk|info|nachrichten|nieuws|noticias|actualit\w*|inforadio|journal)\b", re.I)
NOT_TALK_RE = re.compile(r"groot nieuws", re.I)   # "good news": a Christian music station
# No "house"/"club": The House FM / House of Praise are Christian stations, Music Club / Kids Club aren't EDM either.
EDM_NAME_RE = re.compile(r"\b(dance|edm|techno|trance|hardstyle|electro\w*)\b", re.I)
PUBLIC_RADIO_RE = re.compile(r"npr|public radio|\bwnyc\b|\bkqed\b|\bopb\b", re.I)   # American listener-supported
ESPN_RE = re.compile(r"\bespn\b", re.I)
# Shazam files much religious audio (incl. Quran recitation and Jewish religious music) under "Christian & Gospel".
# That genre is only used for stations that are Christian themselves; e.g. Quran stations (also spelled without
# hamza, قران) and Kan Moreshet never read as Christian.
CHRISTIAN_GENRE_RE = re.compile(r"christian|gospel", re.I)
CHRISTIAN_STATION_RE = re.compile(r"christ|gospel|praise|church|catholic|faith|bible|worship|jesus|vatican|ewtn|"
                                  r"groot nieuws|k-?love|\bair1\b", re.I)
REGION_NAMES = {"gaza": "Gaza", "palestine": "Palestine", "westbank": "West Bank"}  # if not in Local Radio's data
SPORTS_RE = re.compile(r"sport|\b5 live\b", re.I)
EVENT_LINES = {"christmas": "The Official North Pole Station",            # World of Trucks event channels
               "halloween": "The Official Station of Witches Brew"}
EDM_GENRE_RE = re.compile(r"dance|electronic|house|techno|trance|edm|drum|dubstep|hardstyle|electro", re.I)
NORTH_KOREA_LINE = "존경하는 동지께서 깊은 존경을 담아 전합니다."
VINCI_TITLE, VINCI_LINE = "Vous n'êtes jamais à 3 chansons de Traffic.", "Vinci Autoroutes Radio."


def is_north_korea(st):
    return st["key"] == "koreadpr" or bool(re.search(r"평양|pyongyang", st["name"], re.I))


SONG_RE = re.compile(r"^\s*(?P<artist>[^-–]+?)\s+[-–]\s+(?P<title>.+?)\s*$")


_FOLD = str.maketrans({"ø": "o", "Ø": "o", "æ": "ae", "Æ": "ae", "ß": "ss", "þ": "th", "Þ": "th", "ð": "d", "Ð": "d",
                       "đ": "d", "Đ": "d", "ł": "l", "Ł": "l", "œ": "oe", "Œ": "oe"})


def _song_key(text):
    """Lower case, accents and brackets ("(feat. ...)", "[Radio Edit]") removed, only letters and digits.
    Letters that don't decompose (ø æ ß þ ð ł) are spelled out, so "Røyksopp" matches Shazam's "Royksopp"."""
    text = unicodedata.normalize("NFKD", (text or "").translate(_FOLD))
    text = "".join(c for c in text if not unicodedata.combining(c)).lower()
    text = re.sub(r"[(\[].*?[)\]]|\b(feat|ft|featuring)\b.*", " ", text)
    return re.sub(r"[^0-9a-zЀ-ӿͰ-Ͽ]+", "", text)


_ARTIST_SPLIT = re.compile(r"\s*(?:,|&|\bx\b|\band\b|\bog\b|\bund\b|/|\+)\s*", re.I)


def same_song(stream_song, shazam_song):
    """True if Shazam heard the song the station's stream title names: same title, and the artists overlap
    (stations write "A & B" / "A x B" where Shazam writes "A, B")."""
    (s_artist, s_title), (z_artist, z_title) = stream_song, shazam_song
    t1, t2 = _song_key(s_title), _song_key(z_title)
    if not t1 or not t2 or not (t1 == t2 or (min(len(t1), len(t2)) >= 5 and (t1 in t2 or t2 in t1))):
        return False
    a1, a2 = _song_key(s_artist), _song_key(z_artist)
    first1 = _song_key(_ARTIST_SPLIT.split(s_artist or "")[0])
    first2 = _song_key(_ARTIST_SPLIT.split(z_artist or "")[0])
    return bool(a1 and a2 and (a1 == a2 or (first1 and first1 in a2) or (first2 and first2 in a1)))


ART_KEYS = ("imageUrl", "image", "imageURL", "artworkUrl", "artwork", "coverUrl", "cover", "albumArt")


# Image addresses taken from radio stream metadata: whoever runs a station chooses them, so they are only fetched from
# public internet hosts (never this computer or the home network), redirects included.
_untrusted_urls = set()


def public_host(url):
    """True if every address the URL's host resolves to is a public internet address."""
    host = urllib.parse.urlsplit(url).hostname
    if not host:
        return False
    try:
        infos = socket.getaddrinfo(host, None)
    except OSError:
        return False
    for info in infos:
        try:
            if not ipaddress.ip_address(info[4][0].split("%")[0]).is_global:
                return False
        except ValueError:
            return False
    return bool(infos)


class _PublicRedirects(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        if not newurl.startswith(("http://", "https://")) or not public_host(newurl):
            raise urllib.error.URLError(f"redirect to a non-public address refused: {newurl[:80]}")
        return super().redirect_request(req, fp, code, msg, headers, newurl)


_public_opener = urllib.request.build_opener(_PublicRedirects)


def icy_art(meta):
    """Cover image address from an ICY metadata block's StreamUrl, or None."""
    m = re.search(r"StreamUrl='(\{.*\})';", meta, re.S) or re.search(r"StreamUrl='(.*?)';", meta, re.S)
    value = m.group(1).strip() if m else ""
    if value.startswith("{"):
        try:
            data = json.loads(value)
        except ValueError:
            return None
        value = next((data[k] for k in ART_KEYS if isinstance(data.get(k), str) and data[k]), "")
    elif not re.search(r"\.(jpe?g|png|webp)(\?|$)", value, re.I):
        return None  # a plain link to the station's website, not a picture
    if not value.startswith(("http://", "https://")):
        return None
    if len(_untrusted_urls) > 500:
        _untrusted_urls.clear()
    _untrusted_urls.add(value)
    return value


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
    """Background worker for the current Local Radio station: the song (ICY title or SongRec) and what kind of
    station it is (talk / EDM / music), learned from Shazam results and a YAMNet speech/music check."""

    def __init__(self):
        self.cv = threading.Condition()
        self.url = None
        self.station = ""
        self.pid = None
        self.active_until = 0.0
        self.icy = None          # (artist, title) or None
        self.icy_since = 0.0     # when the stream title last changed to a new song
        self.icy_cover = None    # (artist, title) of the stream song -> cover_url, from a Shazam match of the same song
        self.icy_art = None      # (artist, title) -> cover_url the station itself sends with the title (StreamUrl)
        self.cover_tries = 0     # Shazam attempts for the current stream song's cover
        self.rec = None          # (artist, title, cover_url, when)
        self.prof = {}           # station -> {"genres": {}, "clips": [speech bools], "misses": n, "hits": n}
        self.last_icy = 0.0
        self.last_rec = 0.0
        self.warned = set()
        self.model = self.load_model()
        threading.Thread(target=self.run, daemon=True).start()

    @staticmethod
    def load_model():
        try:
            import yamnet_cls
            m = yamnet_cls.Yamnet(MODEL_DIR)
            log(f"talk detection: YAMNet loaded from {MODEL_DIR}")
            return m
        except Exception as e:  # numpy / onnxruntime / model missing: talk only by name and Shazam misses
            log(f"talk detection without YAMNet ({e.__class__.__name__}: {e})")
            return None

    def profile(self, station):
        return self.prof.setdefault(station, {"genres": {}, "clips": [], "misses": 0, "hits": 0})

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
            counts = dict(self.profile(station)["genres"])
        if not CHRISTIAN_STATION_RE.search(station):
            counts = {g: c for g, c in counts.items() if not CHRISTIAN_GENRE_RE.search(g)}
        return max(counts, key=counts.get) if counts else ""

    def kind(self, station):
        """'talk', 'edm' or 'music'."""
        with self.cv:
            p = self.profile(station)
            clips, misses, hits = list(p["clips"][-4:]), p["misses"], p["hits"]
        if not TRAFFIC_RE.search(station):
            if misses >= 2 and clips and sum(clips) * 2 >= len(clips):
                return "talk"   # Shazam keeps missing and it sounds like speech
            if TALK_NAME_RE.search(station) and not NOT_TALK_RE.search(station) and not (hits and misses == 0):
                return "talk"   # news/talk/info in the name, unless songs are being identified right now
        if EDM_GENRE_RE.search(self.genre(station)) or EDM_NAME_RE.search(station):
            return "edm"
        return "music"

    def song(self):
        """(artist, title, cover_url or None, source) or None."""
        with self.cv:
            if self.icy:  # the station's own song info; cover from the station, else Shazam (when it heard the same song)
                return self.icy[0], self.icy[1], self.stream_cover(), "stream"
            if self.rec and time.time() - self.rec[3] < SONG_MAX_AGE:
                return self.rec[0], self.rec[1], self.rec[2], "songrec"
        return None

    def stream_cover(self):
        """Cover for the current stream song: the station's own artwork first, then a Shazam match. Call with cv held."""
        for pair in (self.icy_art, self.icy_cover):
            if pair and pair[0] == self.icy:
                return pair[1]
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
                icy, art = self.read_icy(url) if url.startswith(("http://", "https://")) else ("", None)
                with self.cv:
                    if url == self.url:
                        self.last_icy = time.monotonic()
                        song = split_song(icy, station)
                        if song != self.icy:
                            log(f"stream title: {icy!r} -> {'song' if song else 'not a song'}")
                            self.icy_since = time.monotonic()
                            self.cover_tries = 0
                        self.icy = song
                        if song and art and self.icy_art != (song, art):
                            log(f"cover for stream song {song[0]} - {song[1]} from the station")
                            self.icy_art = (song, art)
            talk = self.kind(station) == "talk"
            with self.cv:
                since = time.monotonic() - self.last_rec
                if self.icy and not talk:  # the stream names the song; Shazam only for its cover and the genre
                    need_rec = since > (GENRE_INTERVAL if self.profile(station)["genres"] else GENRE_INTERVAL_NEW)
                    has_cover = self.stream_cover() is not None
                    if not has_cover and self.cover_tries < COVER_TRIES:
                        wait = COVER_FIRST_DELAY if self.cover_tries == 0 else COVER_RETRY
                        need_rec = need_rec or (time.monotonic() - self.icy_since > wait and since > wait)
                    if need_rec:
                        self.cover_tries += 1
                else:
                    need_rec = since > (TALK_INTERVAL if talk else RECOGNIZE_INTERVAL)
                need_rec = need_rec and url == self.url
            if need_rec:
                with self.cv:
                    self.last_rec = time.monotonic()
                found, speech = self.recognize(pid)
                with self.cv:
                    if url != self.url:
                        continue
                    p = self.profile(station)
                    if found:
                        artist, title, cover, genre = found
                        p["hits"] += 1
                        p["misses"] = 0
                        if not self.rec or self.rec[:2] != (artist, title):
                            log(f"songrec: {artist} - {title} [{genre or 'no genre'}]")
                            if genre:
                                p["genres"][genre] = p["genres"].get(genre, 0) + 1
                        self.rec = (artist, title, cover, time.time())
                        if self.icy and cover and same_song(self.icy, (artist, title)):
                            if not self.stream_cover():
                                log(f"cover for stream song {self.icy[0]} - {self.icy[1]} from Shazam")
                            self.icy_cover = (self.icy, cover)
                    elif found is None and speech is not ...:
                        p["misses"] += 1
                        if speech is not None:
                            p["clips"] = (p["clips"] + [speech])[-6:]
                        log(f"songrec: no match ({p['misses']} in a row), "
                            f"audio sounds like {'speech' if speech else 'music' if speech is False else '?'}")
                kind = self.kind(station)
                if kind != ("talk" if talk else kind):
                    log(f"{station}: now treated as {kind}")

    @staticmethod
    def read_icy(url):
        """(stream title, cover_url or None). Some stations (e.g. P11 Dance) put the song's artwork in StreamUrl,
        either as JSON ({"imageUrl": ...}) or as a bare image address."""
        try:
            req = urllib.request.Request(url, headers={"Icy-MetaData": "1", "User-Agent": "VLC/3.0.20 LibVLC/3.0.20"})
            with urllib.request.urlopen(req, timeout=8) as r:
                metaint = int(r.headers.get("icy-metaint") or 0)
                if not metaint or metaint > 256 * 1024:
                    return "", None
                r.read(metaint)
                n = r.read(1)
                raw = r.read(n[0] * 16) if n else b""
        except (OSError, ValueError):
            return "", None
        for enc in ("utf-8", "latin-1"):
            try:
                text = raw.decode(enc)
                break
            except UnicodeDecodeError:
                continue
        m = re.search(r"StreamTitle='(.*?)';", text, re.S)
        return (m.group(1).strip() if m else ""), icy_art(text)

    def recognize(self, pid):
        """(song or None, speech True/False/None). speech is ... (Ellipsis) when nothing could be recorded."""
        if not (shutil.which("songrec") and shutil.which("pw-record")):
            if "tools" not in self.warned:
                self.warned.add("tools")
                log("songrec/pw-record not installed - only stream titles are available")
            return None, ...
        target = browser_stream(pid)
        if not target:
            return None, ...
        speech = None
        with tempfile.TemporaryDirectory(prefix="cnp-rec-") as d:
            wav = os.path.join(d, "a.wav")
            try:
                # 16 kHz mono: what Shazam fingerprints use internally and what YAMNet expects.
                subprocess.run(["timeout", str(RECORD_SECONDS), "pw-record", "--target", str(target),
                                "--rate", "16000", "--channels", "1", "--format", "s16", wav],
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=RECORD_SECONDS + 5)
                if not os.path.exists(wav) or os.path.getsize(wav) < 16000:
                    return None, ...
                out = subprocess.run(["songrec", "recognize", "-j", wav], capture_output=True, text=True, timeout=30)
                track = (json.loads(out.stdout or "{}").get("track") or {})
                if not track.get("title") and self.model is not None:
                    speech = self.speech(wav)
            except (subprocess.SubprocessError, ValueError, OSError) as e:
                log(f"songrec failed: {e}")
                return None, ...
        if not track.get("title"):
            return None, speech
        cover = (track.get("images") or {}).get("coverarthq") or (track.get("images") or {}).get("coverart")
        genre = ((track.get("genres") or {}).get("primary") or "").strip()
        return (track.get("subtitle") or "", track["title"], cover, genre), False

    def speech(self, wav):
        try:
            import wave
            import numpy as np
            import yamnet_cls
            with wave.open(wav, "rb") as w:
                x = np.frombuffer(w.readframes(w.getnframes()), dtype=np.int16).astype(np.float32) / 32768.0
            return yamnet_cls.is_speech(self.model, x)
        except Exception as e:
            log(f"yamnet: {e!r}")
            return None


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

IMAGE_MAGIC = (b"\x89PNG\r\n\x1a\n", b"\xff\xd8\xff", b"GIF8", b"RIFF")  # PNG, JPEG, GIF, WebP (RIFF....WEBP)


def is_image(data):
    return bool(data) and (data.startswith(IMAGE_MAGIC[:3]) or (data[:4] == b"RIFF" and data[8:12] == b"WEBP"))


def allowed_file(path):
    """Covers are only read from places where players and browsers put artwork, never from anywhere else."""
    real = os.path.realpath(path)
    roots = [tempfile.gettempdir(), "/tmp", "/var/tmp", os.environ.get("XDG_RUNTIME_DIR") or "",
             os.path.expanduser("~/.cache"), os.path.expanduser("~/.var/app"), WEB_DIR or ""]
    return any(r and (real == os.path.realpath(r) or real.startswith(os.path.realpath(r) + os.sep)) for r in roots)



def load_image(src):
    """Image bytes for a file:// / http(s):// / data: URL or a path inside the Local Radio web folder.
    Anything that isn't an image, or a file outside the artwork folders, is refused (MPRIS data comes from any
    program on the desktop)."""
    data = _load_image(src)
    return data if is_image(data) else None


def _load_image(src):
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
            if src in _untrusted_urls:  # from a station's stream metadata: public hosts only
                if not public_host(src):
                    log(f"cover: refusing station artwork on a non-public address: {src[:80]}")
                    return None
                opener = _public_opener.open
            else:
                opener = urllib.request.urlopen
            with opener(req, timeout=5) as r:
                return r.read(MAX_COVER + 1)[:MAX_COVER]
        else:
            path = os.path.join(WEB_DIR or "", src)
        if not allowed_file(path):
            log(f"cover: refusing file outside the artwork folders: {path[:80]}")
            return None
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

def station_phase(st, kind, genre):
    """(title, second line, cover) shown while the station name is on screen."""
    name, logo = st["name"] or "Local Radio", st["logo"]
    if is_north_korea(st):
        return name, NORTH_KOREA_LINE, logo
    for event, line in EVENT_LINES.items():
        if event in (st["key"] or "").lower():
            return name, line, logo
    if re.search(r"vinci", name, re.I):
        return VINCI_TITLE, VINCI_LINE, logo
    if st["region"] == "america" and PUBLIC_RADIO_RE.search(name):
        return name, "Your listener supported radio", logo
    if ESPN_RE.search(name):
        return name, "The Worldwide Leader in Sports", logo   # ESPN's official slogan
    if SPORTS_RE.search(name):
        return name, "Your home of sports", logo
    if kind == "talk":
        return name, "Your trusted news and talk station", logo
    if kind == "edm":
        return name, "The Station with Real DJs", logo
    best = f"best {genre} station" if genre else "best station"
    country = st["country"]
    possessive = country + ("'" if country.endswith("s") else "'s")  # Netherlands' / Iceland's
    return name, f"{possessive} {best}" if country else best[0].upper() + best[1:], logo


def song_phase(kind, song):
    """(artist, title, cover_url) for the song half of the rotation, or None to show only the station."""
    if kind == "talk":
        return None                                       # talk shows: no song info
    if song:
        return song[0], song[1], song[2]
    if kind == "edm":
        return "Likely DJ mix", "No song identified", None
    return None                                           # nothing identified: no song info


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
        st = local_radio_station(stream)
        station, logo = st["name"], st["logo"]
        if playing and (stream or station):
            # Without Plasma's browser integration there's no stream address: key on the station, SongRec only.
            lr_song.want(stream or f"station:{station}", station, md.get("kde:pid"))
        kind = lr_song.kind(station)
        title, artist, cover = station_phase(st, kind, lr_song.genre(station))
        song = song_phase(kind, lr_song.song() if playing else None)
        if song and int(time.time() // ROTATE_SECONDS) % 2 == 0:
            artist, title, cover_url = song
            cover = cover_url or logo
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

    def request_ok(self):
        """Only the plugin may talk to us: right Host (no DNS rebinding) and no browser headers (no cross-site
        requests from web pages). WinHTTP in the game sends neither Origin nor Sec-Fetch-*."""
        host = (self.headers.get("Host") or "").lower()
        port = str(self.server.server_address[1])
        if host not in (f"127.0.0.1:{port}", f"localhost:{port}"):
            return False
        return not (self.headers.get("Origin") or self.headers.get("Sec-Fetch-Site") or self.headers.get("Sec-Fetch-Mode"))

    def do_GET(self):
        u = urllib.parse.urlsplit(self.path)
        q = urllib.parse.parse_qs(u.query)
        if not self.request_ok():
            return self.send(403)
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
