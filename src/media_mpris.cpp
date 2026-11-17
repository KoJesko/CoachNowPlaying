// Linux (Wine/Proton) media backend for CoachNowPlaying.
//
// The Windows build reads the Windows media controls (SMTC, see media.cpp). Wine does not implement SMTC, so under
// Proton the music page never had anything to show. This backend asks the small native helper
// `coach-mpris-bridge` (linux/coach-mpris-bridge.py) instead. The helper reads MPRIS, the standard "now playing"
// interface of Linux desktops, so whatever plays on the system shows up: Spotify, a browser tab (e.g. ETS2 Local
// Radio), a video player...
//
// Protocol (plain HTTP on 127.0.0.1, port from CNP_BRIDGE_PORT or 8332):
//   GET /nowplaying?filter=<app_filter>   -> "key=value" lines: has, app, title, artist, album, playing,
//                                            duration, position, cover (cover id, changes with the image)
//   GET /cover                            -> the current cover image bytes (PNG/JPEG), 404 if none
//   GET /cmd/previous | /cmd/playpause | /cmd/next
//   GET /seek?s=<seconds>
#include "media.h"

#include "http.h"
#include "log.h"
#include "settings.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <map>
#include <mutex>
#include <thread>

namespace ep {

double MediaSnapshot::PositionNow() const {
    double pos = position;
    if (playing) pos += std::chrono::duration<double>(std::chrono::steady_clock::now() - sampledAt).count();
    if (duration > 0) pos = std::min(pos, duration);
    return std::max(pos, 0.0);
}

namespace {

std::mutex g_mutex;
MediaSnapshot g_snap;
std::thread g_thread;
std::atomic<bool> g_stop{false};
std::atomic<int> g_pendingCommand{static_cast<int>(MediaCommand::None)};
std::atomic<double> g_pendingSeek{-1.0};  // seconds, < 0 = none

std::wstring BaseUrl() {
    int port = 8332;
    wchar_t buf[16] = {};
    if (GetEnvironmentVariableW(L"CNP_BRIDGE_PORT", buf, 16) > 0) {
        int p = _wtoi(buf);
        if (p > 0 && p < 65536) port = p;
    }
    return L"http://127.0.0.1:" + std::to_wstring(port);
}

std::wstring UrlEncode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::wstring out;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += static_cast<wchar_t>(c);
        } else {
            out += L'%';
            out += static_cast<wchar_t>(hex[c >> 4]);
            out += static_cast<wchar_t>(hex[c & 15]);
        }
    }
    return out;
}

std::map<std::string, std::string> ParseLines(const std::vector<uint8_t>& body) {
    std::map<std::string, std::string> out;
    std::string text(body.begin(), body.end());
    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(start, end - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t eq = line.find('=');
        if (eq != std::string::npos) out[line.substr(0, eq)] = line.substr(eq + 1);
        start = end + 1;
    }
    return out;
}

double ToDouble(const std::map<std::string, std::string>& m, const char* key) {
    auto it = m.find(key);
    if (it == m.end() || it->second.empty()) return 0.0;
    double v = std::atof(it->second.c_str());
    return std::isfinite(v) ? v : 0.0;
}

std::string Get(const std::map<std::string, std::string>& m, const char* key) {
    auto it = m.find(key);
    return it == m.end() ? std::string() : it->second;
}

void Run() {
    const std::wstring base = BaseUrl();
    Logf("MEDIA Linux/MPRIS backend: asking coach-mpris-bridge at %ls", base.c_str());

    std::string trackKey;
    std::string coverId;
    double seekTarget = 0;
    auto seekTime = std::chrono::steady_clock::now() - std::chrono::hours(1);
    bool holdPlaying = false, heldValue = false;
    auto holdUntil = std::chrono::steady_clock::now();
    bool lastPlaying = false;
    bool bridgeOk = true;
    auto fastPollUntil = std::chrono::steady_clock::now();
    int tick = 0;

    while (!g_stop) {
        std::vector<uint8_t> body;

        auto command = static_cast<MediaCommand>(g_pendingCommand.exchange(static_cast<int>(MediaCommand::None)));
        if (command != MediaCommand::None) {
            const wchar_t* path = command == MediaCommand::PlayPause ? L"/cmd/playpause"
                                  : command == MediaCommand::Next    ? L"/cmd/next"
                                                                     : L"/cmd/previous";
            bool ok = HttpGet(base + path, body, 2000, 4096);
            Logf("MEDIA command %d -> %s", static_cast<int>(command), ok ? "ok" : "refused or bridge not running");
            fastPollUntil = std::chrono::steady_clock::now() + std::chrono::seconds(3);
            if (ok && command == MediaCommand::PlayPause) {
                holdPlaying = true;
                heldValue = !lastPlaying;
                holdUntil = std::chrono::steady_clock::now() + std::chrono::milliseconds(3000);
            }
        }
        const double seek = g_pendingSeek.exchange(-1.0);
        if (seek >= 0) {
            bool ok = HttpGet(base + L"/seek?s=" + std::to_wstring(seek), body, 2000, 4096);
            seekTarget = seek;
            seekTime = std::chrono::steady_clock::now();
            Logf("MEDIA seek to %.1fs -> %s", seek, ok ? "ok" : "refused or bridge not running");
        }

        MediaSnapshot next;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            next = g_snap;  // keeps serials and the cover between polls
        }

        body.clear();
        const bool ok = HttpGet(base + L"/nowplaying?filter=" + UrlEncode(GetSettings().appFilter), body, 1500, 64 * 1024);
        if (ok != bridgeOk) {
            bridgeOk = ok;
            Logf(ok ? "MEDIA bridge reachable" : "MEDIA bridge not reachable (is coach-mpris-bridge running?)");
        }
        auto m = ok ? ParseLines(body) : std::map<std::string, std::string>{};

        if (!ok || Get(m, "has") != "1") {
            if (next.hasSession) Logf("MEDIA no active player");
            next.hasSession = false;
            next.playing = false;
        } else {
            next.hasSession = true;
            next.app = Get(m, "app");
            next.title = Get(m, "title");
            next.artist = Get(m, "artist");
            next.album = Get(m, "album");
            next.playing = Get(m, "playing") == "1";
            if (holdPlaying) {
                if (next.playing == heldValue || std::chrono::steady_clock::now() > holdUntil) holdPlaying = false;
                else next.playing = heldValue;
            }
            if (next.playing != lastPlaying) {
                lastPlaying = next.playing;
                Logf("MEDIA playback state: %s", next.playing ? "playing" : "paused");
            }
            next.duration = ToDouble(m, "duration");
            next.position = ToDouble(m, "position");
            next.sampledAt = std::chrono::steady_clock::now();
            {
                const double since = std::chrono::duration<double>(next.sampledAt - seekTime).count();
                const double expected = seekTarget + (next.playing ? since : 0.0);
                if (since < 1.5 && std::fabs(next.position - expected) > 2.5) next.position = expected;
            }

            std::string key = next.app + "|" + next.title + "|" + next.artist + "|" + next.album;
            if (key != trackKey) {
                trackKey = key;
                ++next.trackSerial;
                Logf("MEDIA [%s] %s: '%s' - '%s' (%s) %.0fs", next.app.c_str(), next.playing ? "playing" : "paused",
                     next.artist.c_str(), next.title.c_str(), next.album.c_str(), next.duration);
            }
            const std::string cid = Get(m, "cover");
            if (cid != coverId) {
                coverId = cid;
                next.cover.reset();
                if (!cid.empty()) {
                    std::vector<uint8_t> img;
                    if (HttpGet(base + L"/cover", img, 4000, 16u * 1024 * 1024) && !img.empty()) {
                        next.cover = std::make_shared<const std::vector<uint8_t>>(std::move(img));
                        Logf("MEDIA cover loaded: %zu bytes", next.cover->size());
                    } else {
                        Logf("MEDIA cover not available");
                    }
                }
                ++next.coverSerial;
            }
            if (tick % 60 == 0) Logf("MEDIA position %.1fs of %.1fs", next.position, next.duration);
        }
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_snap = std::move(next);
        }
        ++tick;
        // Twice a second (five times right after a click), but react to a button press within ~50 ms.
        // When the bridge is down, only check every few seconds.
        const int steps = !bridgeOk ? 100 : std::chrono::steady_clock::now() < fastPollUntil ? 3 : 10;
        for (int i = 0; i < steps && !g_stop && g_pendingCommand.load() == static_cast<int>(MediaCommand::None) &&
                        g_pendingSeek.load() < 0;
             ++i) {
            Sleep(50);
        }
    }
}

}  // namespace

void MediaStart() {
    g_stop = false;
    g_thread = std::thread(Run);
}

void MediaStop() {
    g_stop = true;
    if (g_thread.joinable()) g_thread.join();
}

void MediaSendCommand(MediaCommand cmd) { g_pendingCommand = static_cast<int>(cmd); }

void MediaSeek(double seconds) {
    g_pendingSeek = std::max(0.0, seconds);
    std::lock_guard<std::mutex> lock(g_mutex);
    g_snap.position = std::max(0.0, seconds);
    g_snap.sampledAt = std::chrono::steady_clock::now();
}

MediaSnapshot MediaGetSnapshot() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_snap;
}

}  // namespace ep
