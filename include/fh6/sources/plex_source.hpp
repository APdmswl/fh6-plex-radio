#pragma once

#include "fh6/audio_source.hpp"
#include "fh6/config.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace fh6::sources {

struct PlexLibrary {
    std::string key;
    std::string title;
    std::string type;
    int leaf_count = 0;
};

struct PlexPlaylist {
    std::string key;
    std::string title;
    int leaf_count = 0;
};

struct PlexArtist {
    std::string key;
    std::string title;
    int leaf_count = 0;
};

struct PlexAlbum {
    std::string key;
    std::string title;
    std::string artist;
    std::string artwork_url;
    int leaf_count = 0;
};

struct PlexTrack {
    std::string key;
    std::string title;
    std::string artist;
    std::string album;
    std::string part_key;
    std::string artwork_url;
    std::uint64_t duration_ms = 0;
};

class PlexSource final : public IAudioSource {
public:
    explicit PlexSource(PlexConfig cfg);
    ~PlexSource() override;

    std::string_view name() const noexcept override { return "plex"; }
    std::string_view display_name() const noexcept override { return "Plex"; }

    bool initialize() override;
    void shutdown() noexcept override;

    void play() override;
    void pause() override;
    void stop() override;
    void next() override;
    void previous() override;
    bool skip_next() override;
    bool restart_current() override;
    void pump(RingBuffer& ring) override;

    void update_config(PlexConfig cfg);
    bool refresh_catalog();
    bool refresh_libraries();
    bool refresh_playlists();
    bool refresh_artists();
    bool refresh_albums();

    TrackInfo current_track() const override;
    PlaybackState playback_state() const noexcept override {
        return state_.load(std::memory_order_acquire);
    }
    AuthState auth_state() const noexcept override {
        return auth_.load(std::memory_order_acquire);
    }
    std::string auth_instructions() const override;
    SourceCapabilities capabilities() const noexcept override { return {false, true, true}; }

    std::size_t track_count() const noexcept;
    std::vector<PlexLibrary> libraries_snapshot() const;
    std::vector<PlexPlaylist> playlists_snapshot() const;
    std::vector<PlexArtist> artists_snapshot() const;
    std::vector<PlexAlbum> albums_snapshot() const;
    std::vector<PlexTrack> queue_snapshot() const;

private:
    struct Pipe;

    bool refresh_catalog_locked();
    bool refresh_libraries_locked();
    bool refresh_playlists_locked();
    bool refresh_artists_locked();
    bool refresh_albums_locked();
    bool append_tracks_from_path_locked(const std::string& path, std::vector<PlexTrack>& out,
                                        std::string& error);
    void start_pipe_locked();
    void stop_pipe_locked();
    void set_error_locked(std::string msg);

    PlexConfig cfg_;
    std::unique_ptr<Pipe> pipe_;

    mutable std::mutex mu_;
    std::vector<PlexLibrary> libraries_;
    std::vector<PlexPlaylist> playlists_;
    std::vector<PlexArtist> artists_;
    std::vector<PlexAlbum> albums_;
    std::vector<PlexTrack> queue_;
    std::size_t queue_idx_ = 0;
    TrackInfo info_{};
    std::string last_error_;
    std::atomic<std::uint64_t> position_ms_{0};
    int consecutive_failed_ = 0;
    std::atomic<AuthState> auth_{AuthState::needs_auth};
    std::atomic<PlaybackState> state_{PlaybackState::stopped};
};

} // namespace fh6::sources
