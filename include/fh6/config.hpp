#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace fh6 {

struct PlaybackConfig {
    // "next" | "restart" | "ignore". Plex defaults this to ignore so races
    // never advance the queue unless the user opts in.
    std::string race_start_playback = "ignore";
    bool quick_station_skip         = false;
    bool volume_normalization       = false;
    bool equalizer_enabled          = false;
    std::array<float, 5> equalizer_bands{}; // 60 / 250 / 1000 / 4000 / 12000 Hz
    bool force_stereo_audio         = true;
};

struct GeneralConfig {
    uint16_t port                = 8420;
    uint32_t ring_buffer_mb      = 16;
    bool open_dashboard_on_start = true;
    std::string default_source   = "local_files";
    std::string fallback_source  = "local_files";
    std::filesystem::path ffmpeg_path; // empty = look up on PATH; shared fallback
};

struct LocalFilesConfig {
    bool enabled = true;
    std::filesystem::path music_dir;
    bool recursive = true;
    bool shuffle   = true;
    std::vector<std::string> supported_formats{"mp3", "flac", "wav",  "ogg",  "m4a",
                                               "opus", "aac",  "wma", "aiff", "aif"};
};

struct YouTubeMusicConfig {
    bool enabled = false;
    std::filesystem::path cookies_path;
    std::filesystem::path yt_dlp_path; // empty = look up on PATH
    std::filesystem::path ffmpeg_path; // empty = look up on PATH
    std::string default_playlist;
};

struct PlexConfig {
    bool enabled = false;
    std::string server_url = "https://your-plex.example.com";
    std::string token;
    std::string library_key;
    std::string playlist_key;
    std::string artist_key;
    std::string album_key;
    std::filesystem::path ffmpeg_path; // empty = look up on PATH
    bool shuffle = true;
};

struct AudioConfig {
    float output_gain = 1.0f;
};

struct Config {
    GeneralConfig general;
    LocalFilesConfig local_files;
    YouTubeMusicConfig youtube_music;
    PlexConfig plex;
    AudioConfig audio;
    PlaybackConfig playback;
};

// Missing file is fine, defaults are returned.
Config load_config(const std::filesystem::path& path);

// Atomic write (temp + rename). Throws std::system_error on failure.
void save_config(const std::filesystem::path& path, const Config& cfg);

} // namespace fh6
