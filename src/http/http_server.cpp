#include "fh6/http/http_server.hpp"
#include "fh6/audio_source_manager.hpp"
#include "fh6/config_store.hpp"
#include "fh6/fmod/dsp_bridge.hpp"
#include "fh6/log.hpp"
#include "fh6/sources/local_file_source.hpp"
#include "fh6/sources/plex_source.hpp"
#include "fh6/sources/youtube_music_source.hpp"

#include <nlohmann/json.hpp>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace fh6::http {

using json = nlohmann::json;

namespace {

constexpr const char* state_string(PlaybackState s) noexcept {
    switch (s) {
        case PlaybackState::stopped:   return "stopped";
        case PlaybackState::playing:   return "playing";
        case PlaybackState::paused:    return "paused";
        case PlaybackState::buffering: return "buffering";
    }
    return "unknown";
}

constexpr const char* auth_string(AuthState s) noexcept {
    switch (s) {
        case AuthState::none_required: return "none_required";
        case AuthState::authenticated: return "authenticated";
        case AuthState::needs_auth:    return "needs_auth";
        case AuthState::error:         return "error";
    }
    return "unknown";
}

constexpr const char* mode_string(fmod_bridge::DSPMode m) noexcept {
    switch (m) {
        case fmod_bridge::DSPMode::off:         return "off";
        case fmod_bridge::DSPMode::passthrough: return "passthrough";
        case fmod_bridge::DSPMode::silence:     return "silence";
        case fmod_bridge::DSPMode::pcm:         return "pcm";
    }
    return "unknown";
}

// Force UTF-8 so non-ASCII paths survive the JSON round-trip on Windows.
std::string path_s(const std::filesystem::path& p) {
    if (p.empty()) return {};
    auto u8 = p.u8string();
    return std::string{u8.begin(), u8.end()};
}

json track_to_json(const TrackInfo& t) {
    return json{
        {"title", t.title},
        {"artist", t.artist},
        {"album", t.album},
        {"artwork_url", t.artwork_url},
        {"duration_ms", t.duration_ms},
        {"position_ms", t.position_ms},
    };
}

json plex_library_to_json(const sources::PlexLibrary& lib) {
    return json{
        {"key", lib.key},
        {"title", lib.title},
        {"type", lib.type},
        {"leaf_count", lib.leaf_count},
    };
}

json plex_playlist_to_json(const sources::PlexPlaylist& pl) {
    return json{
        {"key", pl.key},
        {"title", pl.title},
        {"leaf_count", pl.leaf_count},
    };
}

json plex_artist_to_json(const sources::PlexArtist& artist) {
    return json{
        {"key", artist.key},
        {"title", artist.title},
        {"leaf_count", artist.leaf_count},
    };
}

json plex_album_to_json(const sources::PlexAlbum& album) {
    return json{
        {"key", album.key},
        {"title", album.title},
        {"artist", album.artist},
        {"artwork_url", album.artwork_url},
        {"leaf_count", album.leaf_count},
    };
}

json plex_track_to_json(const sources::PlexTrack& t) {
    return json{
        {"key", t.key},
        {"title", t.title},
        {"artist", t.artist},
        {"album", t.album},
        {"artwork_url", t.artwork_url},
        {"duration_ms", t.duration_ms},
    };
}

json source_to_json(IAudioSource* s) {
    auto c = s->capabilities();
    json j{
        {"name", std::string{s->name()}},
        {"display_name", std::string{s->display_name()}},
        {"playback_state", state_string(s->playback_state())},
        {"auth_state", auth_string(s->auth_state())},
        {"auth_instructions", s->auth_instructions()},
        {"capabilities",
         json{
             {"seek", c.seek},
             {"previous", c.previous},
             {"queue", c.queue},
         }},
        {"details", json::object()},
    };
    if (auto* lf = dynamic_cast<sources::LocalFileSource*>(s))
        j["details"]["track_count"] = lf->track_count();
    if (auto* plex = dynamic_cast<sources::PlexSource*>(s)) {
        j["details"]["track_count"]    = plex->track_count();
        j["details"]["library_count"]  = plex->libraries_snapshot().size();
        j["details"]["playlist_count"] = plex->playlists_snapshot().size();
        j["details"]["artist_count"]   = plex->artists_snapshot().size();
        j["details"]["album_count"]    = plex->albums_snapshot().size();
    }
    return j;
}

json config_to_json(const Config& c) {
    return json{
        {"general",
         json{
             {"port", c.general.port},
             {"ring_buffer_mb", c.general.ring_buffer_mb},
             {"open_dashboard_on_start", c.general.open_dashboard_on_start},
             {"default_source", c.general.default_source},
             {"fallback_source", c.general.fallback_source},
         }},
        {"local_files",
         json{
             {"enabled", c.local_files.enabled},
             {"music_dir", path_s(c.local_files.music_dir)},
             {"recursive", c.local_files.recursive},
             {"shuffle", c.local_files.shuffle},
             {"supported_formats", c.local_files.supported_formats},
         }},
        {"youtube_music",
         json{
             {"enabled", c.youtube_music.enabled},
             {"cookies_path", path_s(c.youtube_music.cookies_path)},
             {"yt_dlp_path", path_s(c.youtube_music.yt_dlp_path)},
             {"ffmpeg_path", path_s(c.youtube_music.ffmpeg_path)},
             {"default_playlist", c.youtube_music.default_playlist},
         }},
        {"plex",
         json{
             {"enabled", c.plex.enabled},
             {"server_url", c.plex.server_url},
             {"token", c.plex.token},
             {"library_key", c.plex.library_key},
             {"playlist_key", c.plex.playlist_key},
             {"artist_key", c.plex.artist_key},
             {"album_key", c.plex.album_key},
             {"ffmpeg_path", path_s(c.plex.ffmpeg_path)},
             {"shuffle", c.plex.shuffle},
         }},
        {"audio",
         json{
             {"output_gain", c.audio.output_gain},
         }},
        {"playback",
         json{
             {"race_start_playback", c.playback.race_start_playback},
             {"quick_station_skip", c.playback.quick_station_skip},
         }},
    };
}

template <class T> T pull(const json& tbl, const char* k, T fallback) {
    if (auto it = tbl.find(k); it != tbl.end() && !it->is_null()) {
        try {
            return it->get<T>();
        } catch (...) {}
    }
    return fallback;
}

std::filesystem::path pull_path(const json& tbl, const char* k,
                                const std::filesystem::path& fallback) {
    auto s = pull<std::string>(tbl, k, path_s(fallback));
    return s.empty() ? std::filesystem::path{} : std::filesystem::path{s};
}

void apply_patch(Config& c, const json& j) {
    if (auto it = j.find("general"); it != j.end()) {
        c.general.port            = pull(*it, "port", c.general.port);
        c.general.ring_buffer_mb  = pull(*it, "ring_buffer_mb", c.general.ring_buffer_mb);
        c.general.open_dashboard_on_start =
            pull(*it, "open_dashboard_on_start", c.general.open_dashboard_on_start);
        c.general.default_source  = pull(*it, "default_source", c.general.default_source);
        c.general.fallback_source = pull(*it, "fallback_source", c.general.fallback_source);
    }
    if (auto it = j.find("local_files"); it != j.end()) {
        c.local_files.enabled   = pull(*it, "enabled", c.local_files.enabled);
        c.local_files.music_dir = pull_path(*it, "music_dir", c.local_files.music_dir);
        c.local_files.recursive = pull(*it, "recursive", c.local_files.recursive);
        c.local_files.shuffle   = pull(*it, "shuffle", c.local_files.shuffle);
        if (auto fmts = it->find("supported_formats"); fmts != it->end() && fmts->is_array())
            c.local_files.supported_formats = fmts->get<std::vector<std::string>>();
    }
    if (auto it = j.find("youtube_music"); it != j.end()) {
        c.youtube_music.enabled      = pull(*it, "enabled", c.youtube_music.enabled);
        c.youtube_music.cookies_path = pull_path(*it, "cookies_path", c.youtube_music.cookies_path);
        c.youtube_music.yt_dlp_path  = pull_path(*it, "yt_dlp_path", c.youtube_music.yt_dlp_path);
        c.youtube_music.ffmpeg_path  = pull_path(*it, "ffmpeg_path", c.youtube_music.ffmpeg_path);
        c.youtube_music.default_playlist =
            pull(*it, "default_playlist", c.youtube_music.default_playlist);
    }
    if (auto it = j.find("plex"); it != j.end()) {
        c.plex.enabled      = pull(*it, "enabled", c.plex.enabled);
        c.plex.server_url   = pull(*it, "server_url", c.plex.server_url);
        c.plex.token        = pull(*it, "token", c.plex.token);
        c.plex.library_key  = pull(*it, "library_key", c.plex.library_key);
        c.plex.playlist_key = pull(*it, "playlist_key", c.plex.playlist_key);
        c.plex.artist_key   = pull(*it, "artist_key", c.plex.artist_key);
        c.plex.album_key    = pull(*it, "album_key", c.plex.album_key);
        c.plex.ffmpeg_path  = pull_path(*it, "ffmpeg_path", c.plex.ffmpeg_path);
        c.plex.shuffle      = pull(*it, "shuffle", c.plex.shuffle);
    }
    if (auto it = j.find("audio"); it != j.end()) {
        c.audio.output_gain = pull(*it, "output_gain", c.audio.output_gain);
    }
    if (auto it = j.find("playback"); it != j.end()) {
        auto rs = pull<std::string>(*it, "race_start_playback", c.playback.race_start_playback);
        if (rs == "next" || rs == "restart" || rs == "ignore")
            c.playback.race_start_playback = std::move(rs);
        c.playback.quick_station_skip =
            pull(*it, "quick_station_skip", c.playback.quick_station_skip);
    }
}

struct Request {
    std::string method;
    std::string path;
    std::string body;
};

constexpr std::string_view status_text(int code) noexcept {
    switch (code) {
        case 200: return "OK";
        case 400: return "Bad Request";
        case 404: return "Not Found";
        case 502: return "Bad Gateway";
        default: return "Internal Server Error";
    }
}

constexpr std::string_view mime_for(std::string_view path) noexcept {
    auto ends = [&](std::string_view ext) {
        return path.size() >= ext.size() &&
               path.compare(path.size() - ext.size(), ext.size(), ext) == 0;
    };
    if (ends(".html")) return "text/html";
    if (ends(".css")) return "text/css";
    if (ends(".js")) return "application/javascript";
    if (ends(".svg")) return "image/svg+xml";
    if (ends(".png")) return "image/png";
    if (ends(".json")) return "application/json";
    return "text/plain";
}

std::string strip_query(std::string path) {
    if (auto pos = path.find('?'); pos != std::string::npos) path.resize(pos);
    return path;
}

size_t header_size_t(std::string_view headers, std::string_view name_lower) {
    for (size_t i = 0; i + name_lower.size() < headers.size(); ++i) {
        bool match = true;
        for (size_t k = 0; k < name_lower.size(); ++k) {
            if (static_cast<char>(std::tolower(static_cast<unsigned char>(headers[i + k]))) !=
                name_lower[k]) {
                match = false;
                break;
            }
        }
        if (!match) continue;
        size_t p = i + name_lower.size();
        while (p < headers.size() && (headers[p] == ':' || headers[p] == ' ' || headers[p] == '\t'))
            ++p;
        size_t v = 0;
        while (p < headers.size() && std::isdigit(static_cast<unsigned char>(headers[p])))
            v = v * 10 + static_cast<size_t>(headers[p++] - '0');
        return v;
    }
    return 0;
}

bool read_request(SOCKET client, Request& req) {
    std::string raw;
    raw.reserve(1024);
    std::array<char, 4096> buf{};

    size_t header_end = std::string::npos;
    while (header_end == std::string::npos) {
        int r = recv(client, buf.data(), static_cast<int>(buf.size()), 0);
        if (r <= 0) return false;
        raw.append(buf.data(), static_cast<size_t>(r));
        header_end = raw.find("\r\n\r\n");
        if (raw.size() > 64 * 1024) return false;
    }

    std::istringstream first{raw.substr(0, raw.find("\r\n"))};
    if (!(first >> req.method >> req.path)) return false;
    req.path = strip_query(std::move(req.path));

    const std::string_view headers{raw.data(), header_end};
    const size_t content_length = header_size_t(headers, "content-length");

    req.body.assign(raw, header_end + 4, std::string::npos);
    while (req.body.size() < content_length) {
        const size_t need = std::min<size_t>(buf.size(), content_length - req.body.size());
        int r = recv(client, buf.data(), static_cast<int>(need), 0);
        if (r <= 0) break;
        req.body.append(buf.data(), static_cast<size_t>(r));
    }
    return true;
}

void send_all(SOCKET client, std::string_view data) {
    while (!data.empty()) {
        int n = send(client, data.data(), static_cast<int>(data.size()), 0);
        if (n <= 0) return;
        data.remove_prefix(static_cast<size_t>(n));
    }
}

void send_response(SOCKET client, int code, std::string_view body,
                   std::string_view content_type = "application/json") {
    std::ostringstream resp;
    resp << "HTTP/1.1 " << code << ' ' << status_text(code) << "\r\n"
         << "Content-Type: " << content_type << "\r\n"
         << "Content-Length: " << body.size() << "\r\n"
         << "Access-Control-Allow-Origin: *\r\n"
         << "Access-Control-Allow-Methods: GET, POST, PUT, OPTIONS\r\n"
         << "Access-Control-Allow-Headers: Content-Type\r\n"
         << "Connection: close\r\n\r\n";
    auto headers = resp.str();
    send_all(client, headers);
    send_all(client, body);
}

bool serve_file(SOCKET client, const std::filesystem::path& file) {
    std::ifstream f{file, std::ios::binary};
    if (!f) return false;
    std::ostringstream buf;
    buf << f.rdbuf();
    send_response(client, 200, buf.str(), mime_for(file.string()));
    return true;
}

} // namespace

struct HttpServer::Impl {
    AudioSourceManager& mgr;
    fmod_bridge::DSPBridge& bridge;
    ConfigStore& store;
    std::filesystem::path ui_dist;
    std::atomic<bool> stopping{false};
    SOCKET srv_sock = INVALID_SOCKET;
    std::thread thr;

    Impl(AudioSourceManager& m, fmod_bridge::DSPBridge& b, ConfigStore& s, uint16_t port,
         std::filesystem::path dist)
        : mgr{m}, bridge{b}, store{s}, ui_dist{std::move(dist)} {
        thr = std::thread{[this, port] { run(port); }};
    }

    ~Impl() {
        stopping.store(true, std::memory_order_release);
        if (srv_sock != INVALID_SOCKET) closesocket(srv_sock);
        if (thr.joinable()) thr.join();
    }

    json build_state() const {
        auto* a      = mgr.active();
        json sources = json::array();
        for (auto* s : mgr.sources_snapshot()) sources.push_back(source_to_json(s));
        return json{
            {"game", json{{"attached", true}, {"injector_ready", true}}},
            {"audio",
             json{
                 {"active", bridge.mode() == fmod_bridge::DSPMode::pcm},
                 {"native_dsp_mode", mode_string(bridge.mode())},
                 {"output_gain", bridge.gain()},
                 {"underruns", bridge.underruns()},
                 {"calls", bridge.call_count()},
                 {"buffer_len", bridge.last_buffer_len()},
                 {"out_channels", bridge.last_out_channels()},
                 {"ring_avail", mgr.ring().readable()},
                 {"ring_capacity", mgr.ring().capacity()},
             }},
            {"sources",
             {
                 {"active", a ? std::string{a->name()} : ""},
                 {"available", std::move(sources)},
             }},
            {"track", a ? track_to_json(a->current_track()) : json::object()},
            {"errors", json::array()},
        };
    }

    IAudioSource* find(std::string_view name) const {
        for (auto* s : mgr.sources_snapshot()) {
            if (s->name() == name) return s;
        }
        return nullptr;
    }

    template <class T> T* find_typed(std::string_view name) const {
        return dynamic_cast<T*>(find(name));
    }

    void run(uint16_t port) {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
            log::error("[http] WSAStartup failed");
            return;
        }

        srv_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (srv_sock == INVALID_SOCKET) {
            log::error("[http] socket() failed");
            WSACleanup();
            return;
        }
        BOOL yes = TRUE;
        setsockopt(srv_sock, SOL_SOCKET, SO_REUSEADDR,
                   reinterpret_cast<const char*>(&yes), sizeof(yes));

        sockaddr_in addr{};
        addr.sin_family      = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);

        uint16_t bound = port;
        addr.sin_port  = htons(bound);
        if (bind(srv_sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            bound         = static_cast<uint16_t>(port + 1);
            addr.sin_port = htons(bound);
            if (bind(srv_sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
                log::error("[http] could not bind port {} or {}", port, bound);
                closesocket(srv_sock);
                srv_sock = INVALID_SOCKET;
                WSACleanup();
                return;
            }
        }

        if (listen(srv_sock, 16) != 0) {
            log::error("[http] listen() failed");
            closesocket(srv_sock);
            srv_sock = INVALID_SOCKET;
            WSACleanup();
            return;
        }
        log::info("[http] listening on http://0.0.0.0:{} (LAN-reachable)", bound);

        while (!stopping.load(std::memory_order_acquire)) {
            fd_set rfds;
            FD_ZERO(&rfds);
            FD_SET(srv_sock, &rfds);
            timeval tv{0, 200'000};
            if (select(0, &rfds, nullptr, nullptr, &tv) <= 0) continue;

            SOCKET client = accept(srv_sock, nullptr, nullptr);
            if (client == INVALID_SOCKET) continue;
            handle(client);
            closesocket(client);
        }

        WSACleanup();
    }

    void handle(SOCKET client) {
        Request req;
        if (!read_request(client, req)) return;

        auto ok = [&](const json& j = json::object()) {
            std::string body = j.empty()
                                   ? std::string{R"({"ok":true})"}
                                   : j.dump(-1, ' ', false, json::error_handler_t::replace);
            send_response(client, 200, body);
        };
        auto fail = [&](int code, std::string_view msg) {
            send_response(client, code, json{{"error", std::string{msg}}}.dump());
        };

        try {
            dispatch(client, req, ok, fail);
        } catch (...) {
            fail(400, "bad request body");
        }
    }

    template <class Ok, class Fail>
    void dispatch(SOCKET client, const Request& req, Ok&& ok, Fail&& fail) {
        const auto& m = req.method;
        const auto& p = req.path;

        if (m == "OPTIONS") return send_response(client, 200, "", "text/plain");

        if (m == "GET" && p == "/api/state") return ok(build_state());
        if (m == "GET" && p == "/api/events") return send_event_snapshot(client);
        if (m == "GET" && p == "/api/sources") return ok(build_state()["sources"]);
        if (m == "GET" && p == "/api/config") return ok(config_to_json(store.snapshot()));
        if (m == "GET" && p == "/api/source/local_files/playlist") {
            auto* lf = find_typed<sources::LocalFileSource>("local_files");
            return lf ? ok(json{{"tracks", lf->playlist_snapshot()}})
                      : fail(404, "local_files not registered");
        }

        if (m == "GET" && p == "/api/source/plex/libraries") {
            auto* plex = find_typed<sources::PlexSource>("plex");
            if (!plex) return fail(404, "plex not registered; enable Plex first");
            if (!plex->refresh_libraries()) return fail(502, plex->auth_instructions());
            json libs = json::array();
            for (const auto& lib : plex->libraries_snapshot())
                libs.push_back(plex_library_to_json(lib));
            return ok(json{{"libraries", libs}});
        }
        if (m == "GET" && p == "/api/source/plex/playlists") {
            auto* plex = find_typed<sources::PlexSource>("plex");
            if (!plex) return fail(404, "plex not registered; enable Plex first");
            if (!plex->refresh_playlists()) return fail(502, plex->auth_instructions());
            json playlists = json::array();
            for (const auto& pl : plex->playlists_snapshot())
                playlists.push_back(plex_playlist_to_json(pl));
            return ok(json{{"playlists", playlists}});
        }
        if (m == "GET" && p == "/api/source/plex/artists") {
            auto* plex = find_typed<sources::PlexSource>("plex");
            if (!plex) return fail(404, "plex not registered; enable Plex first");
            if (!plex->refresh_artists()) return fail(502, plex->auth_instructions());
            json artists = json::array();
            for (const auto& artist : plex->artists_snapshot())
                artists.push_back(plex_artist_to_json(artist));
            return ok(json{{"artists", artists}});
        }
        if (m == "GET" && p == "/api/source/plex/albums") {
            auto* plex = find_typed<sources::PlexSource>("plex");
            if (!plex) return fail(404, "plex not registered; enable Plex first");
            if (!plex->refresh_albums()) return fail(502, plex->auth_instructions());
            json albums = json::array();
            for (const auto& album : plex->albums_snapshot())
                albums.push_back(plex_album_to_json(album));
            return ok(json{{"albums", albums}});
        }
        if (m == "GET" && p == "/api/source/plex/queue") {
            auto* plex = find_typed<sources::PlexSource>("plex");
            if (!plex) return fail(404, "plex not registered; enable Plex first");
            json tracks = json::array();
            for (const auto& t : plex->queue_snapshot()) tracks.push_back(plex_track_to_json(t));
            return ok(json{{"tracks", tracks}});
        }

        if (m == "PUT" && p == "/api/config") {
            auto patch = json::parse(req.body);
            store.patch([&](Config& c) { apply_patch(c, patch); });
            return ok(config_to_json(store.snapshot()));
        }

        if (m == "POST" && p == "/api/config/reload") {
            store.reload();
            return ok(config_to_json(store.snapshot()));
        }
        if (m == "POST" && p == "/api/source/switch") {
            auto src = json::parse(req.body).at("source").get<std::string>();
            return mgr.switch_to(src) ? ok() : fail(404, "unknown source");
        }
        if (m == "POST" && p == "/api/source/youtube_music/cast") {
            auto* yt = find_typed<sources::YouTubeMusicSource>("youtube_music");
            if (!yt) return fail(404, "youtube_music not registered");
            auto url              = json::parse(req.body).at("url").get<std::string>();
            const bool was_active = (mgr.active() == yt);
            yt->set_target(std::move(url));
            yt->stop();
            if (was_active) mgr.ring().drain();
            yt->play();
            mgr.switch_to("youtube_music");
            return ok();
        }
        if (m == "POST" && p == "/api/source/local_files/rescan") {
            auto* lf = find_typed<sources::LocalFileSource>("local_files");
            if (!lf) return fail(404, "local_files not registered");
            auto j = req.body.empty() ? json::object() : json::parse(req.body);
            if (auto it = j.find("music_dir"); it != j.end()) {
                std::filesystem::path dir = it->get<std::string>();
                bool recursive            = j.value("recursive", true);
                lf->set_directory(dir, recursive);
                store.patch([&](Config& c) {
                    c.local_files.music_dir = dir;
                    c.local_files.recursive = recursive;
                });
            } else {
                auto snap = store.snapshot();
                lf->set_directory(snap.local_files.music_dir, snap.local_files.recursive);
            }
            return ok(json{{"track_count", lf->playlist_snapshot().size()}});
        }
        if (m == "POST" && p == "/api/source/plex/refresh") {
            auto* plex = find_typed<sources::PlexSource>("plex");
            if (!plex) return fail(404, "plex not registered; enable Plex first");
            const bool was_active = (mgr.active() == plex);
            if (was_active) {
                plex->stop();
                mgr.ring().drain();
            }
            if (!plex->refresh_catalog()) return fail(502, plex->auth_instructions());
            if (was_active) plex->play();
            return ok(json{{"track_count", plex->track_count()}});
        }
        if (m == "POST" && p == "/api/options") {
            auto j = json::parse(req.body);
            if (auto it = j.find("output_gain"); it != j.end()) {
                float g = std::clamp(it->get<float>(), 0.0f, 1.0f);
                bridge.set_gain(g);
                store.patch([&](Config& c) { c.audio.output_gain = g; });
            }
            return ok();
        }

        constexpr std::string_view prefix = "/api/source/";
        if (m == "POST" && p.starts_with(prefix)) {
            const std::string_view rest{p.data() + prefix.size(), p.size() - prefix.size()};
            const auto slash = rest.find('/');
            if (slash == std::string_view::npos) return fail(400, "invalid route");
            const std::string_view name{rest.data(), slash};
            const std::string_view act{rest.data() + slash + 1, rest.size() - slash - 1};
            auto* s = find(name);
            if (!s) return fail(404, "unknown source");
            const bool is_active = (s == mgr.active());
            if (act == "play") {
                s->play();
            } else if (act == "pause") {
                s->pause();
            } else if (act == "stop") {
                s->stop();
                if (is_active) mgr.ring().drain();
            } else if (act == "next") {
                s->next();
                if (is_active) mgr.ring().drain();
            } else if (act == "previous") {
                s->previous();
                if (is_active) mgr.ring().drain();
            } else {
                return fail(404, "unknown action");
            }
            return ok();
        }

        if (m == "GET" && !ui_dist.empty()) {
            const std::string rel = (p == "/") ? "index.html" : p.substr(1);
            if (serve_file(client, ui_dist / rel)) return;
            if (p.find('.') == std::string::npos && serve_file(client, ui_dist / "index.html"))
                return;
        }
        fail(404, "not found");
    }

    void send_event_snapshot(SOCKET client) {
        auto body = build_state().dump(-1, ' ', false, json::error_handler_t::replace);
        std::string evt;
        evt.reserve(body.size() + 256);
        evt.append("HTTP/1.1 200 OK\r\n"
                   "Content-Type: text/event-stream\r\n"
                   "Cache-Control: no-store\r\n"
                   "Access-Control-Allow-Origin: *\r\n"
                   "Connection: close\r\n\r\n"
                   "retry: 1000\ndata: ");
        evt.append(body);
        evt.append("\n\n");
        send_all(client, evt);
    }
};

HttpServer::HttpServer(AudioSourceManager& mgr, fmod_bridge::DSPBridge& bridge, ConfigStore& cfg,
                       uint16_t port, std::filesystem::path ui_dist)
    : impl_{std::make_unique<Impl>(mgr, bridge, cfg, port, std::move(ui_dist))} {}

HttpServer::~HttpServer() = default;

} // namespace fh6::http
