#include "fh6/sources/local_file_source.hpp"
#include "fh6/log.hpp"

// miniaudio used only as a format-agnostic decoder into S16LE/48k/stereo.
#define MINIAUDIO_IMPLEMENTATION
#define MA_NO_DEVICE_IO
#define MA_NO_GENERATION
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4244) // narrowing inside miniaudio's header
#endif
#include <miniaudio.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include <algorithm>
#include <cctype>
#include <fstream>
#include <random>
#include <string>

namespace fh6::sources {

struct LocalFileSource::Decoder {
    ma_decoder ma{};
    bool open = false;
    TrackInfo info{};
};

namespace {
bool extension_matches(const std::filesystem::path& p, const std::vector<std::string>& exts) {
    if (!p.has_extension()) return false;
    auto e = p.extension().string();
    if (!e.empty() && e.front() == '.') e.erase(0, 1);
    std::ranges::transform(e, e.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return std::ranges::find(exts, e) != exts.end();
}

bool ieq_str(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    }
    return true;
}

// Parse an .m3u / .m3u8 playlist: one path or URL per line. Comments and
// extended metadata lines are ignored; relative file paths resolve beside
// the playlist file.
std::vector<std::filesystem::path> parse_m3u_playlist(const std::filesystem::path& file) {
    std::vector<std::filesystem::path> out;
    std::ifstream in{file};
    if (!in) return out;

    const auto dir = file.parent_path();
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        auto first = line.find_first_not_of(" \t");
        if (first == std::string::npos) continue;
        auto last = line.find_last_not_of(" \t");
        line = line.substr(first, last - first + 1);
        if (line.empty() || line.front() == '#') continue;
        if (line.starts_with("http://") || line.starts_with("https://")) continue;

        std::filesystem::path p{line};
        if (p.is_relative()) p = dir / p;
        out.push_back(std::move(p));
    }
    return out;
}
} // namespace

LocalFileSource::LocalFileSource(LocalFilesConfig cfg)
    : cfg_{std::move(cfg)}, dec_{std::make_unique<Decoder>()} {}

LocalFileSource::~LocalFileSource() { close_current(); }

void LocalFileSource::shutdown() noexcept { close_current(); }

bool LocalFileSource::initialize() {
    if (!cfg_.enabled) return false;
    // No directory yet is fine. auth_state stays needs_auth until the user
    // sets one from the dashboard, which calls set_directory() to rescan.
    if (cfg_.music_dir.empty() || !std::filesystem::exists(cfg_.music_dir)) {
        log::warn("[local] registered with no music_dir yet -- set one from the dashboard");
        return true;
    }
    rebuild_playlist();
    log::info("[local] discovered {} tracks in {}", playlist_.size(), cfg_.music_dir.string());
    return true;
}

void LocalFileSource::rebuild_playlist() {
    std::scoped_lock lk{mu_};
    playlist_.clear();
    std::error_code ec;
    auto add = [&](const std::filesystem::path& p) {
        const auto ext = p.extension().string();
        if (ieq_str(ext, ".m3u") || ieq_str(ext, ".m3u8")) {
            for (auto& entry : parse_m3u_playlist(p)) {
                if (extension_matches(entry, cfg_.supported_formats))
                    playlist_.push_back(std::move(entry));
            }
        } else if (extension_matches(p, cfg_.supported_formats)) {
            playlist_.push_back(p);
        }
    };
    if (cfg_.recursive) {
        for (const auto& e : std::filesystem::recursive_directory_iterator(
                 cfg_.music_dir, std::filesystem::directory_options::skip_permission_denied, ec))
            if (e.is_regular_file(ec)) add(e.path());
    } else {
        for (const auto& e : std::filesystem::directory_iterator(cfg_.music_dir, ec))
            if (e.is_regular_file(ec)) add(e.path());
    }
    if (cfg_.shuffle) {
        static thread_local std::mt19937 rng{std::random_device{}()};
        std::shuffle(playlist_.begin(), playlist_.end(), rng);
    } else {
        std::ranges::sort(playlist_);
    }
    cursor_ = 0;
}

void LocalFileSource::close_current() {
    std::scoped_lock lk{mu_};
    if (dec_->open) {
        ma_decoder_uninit(&dec_->ma);
        dec_->open = false;
    }
    dec_->info = {};
}

bool LocalFileSource::open_track(std::size_t index) {
    if (playlist_.empty()) return false;
    cursor_          = index % playlist_.size();
    const auto& path = playlist_[cursor_];

    if (dec_->open) {
        ma_decoder_uninit(&dec_->ma);
        dec_->open = false;
    }

    ma_decoder_config cfg = ma_decoder_config_init(ma_format_s16, 2, 48000);
    if (ma_decoder_init_file(path.string().c_str(), &cfg, &dec_->ma) != MA_SUCCESS) {
        log::warn("[local] failed to open {}", path.string());
        return false;
    }
    dec_->open = true;

    dec_->info       = TrackInfo{};
    dec_->info.title = path.stem().string();
    dec_->info.album = path.parent_path().filename().string();
    ma_uint64 frames = 0;
    if (ma_decoder_get_length_in_pcm_frames(&dec_->ma, &frames) == MA_SUCCESS)
        dec_->info.duration_ms = (frames * 1000ull) / 48000ull;
    position_ms_.store(0, std::memory_order_release);
    log::info("[local] now playing: {}", path.string());
    return true;
}

void LocalFileSource::play() {
    std::scoped_lock lk{mu_};
    if (!dec_->open && !open_track(cursor_)) {
        state_.store(PlaybackState::stopped, std::memory_order_release);
        return;
    }
    state_.store(PlaybackState::playing, std::memory_order_release);
}

void LocalFileSource::pause() { state_.store(PlaybackState::paused, std::memory_order_release); }

void LocalFileSource::stop() {
    close_current();
    state_.store(PlaybackState::stopped, std::memory_order_release);
}

void LocalFileSource::next() {
    std::scoped_lock lk{mu_};
    open_track(cursor_ + 1);
    state_.store(PlaybackState::playing, std::memory_order_release);
}

void LocalFileSource::previous() {
    std::scoped_lock lk{mu_};
    open_track(cursor_ == 0 ? playlist_.size() - 1 : cursor_ - 1);
    state_.store(PlaybackState::playing, std::memory_order_release);
}

void LocalFileSource::pump(RingBuffer& ring) {
    if (state_.load(std::memory_order_acquire) != PlaybackState::playing) return;

    std::scoped_lock lk{mu_};
    if (!dec_->open) return;

    constexpr std::size_t kChunkFrames = 4096;
    constexpr std::size_t kFrameBytes  = 4;
    while (ring.writable() >= kChunkFrames * kFrameBytes) {
        int16_t scratch[kChunkFrames * 2];
        ma_uint64 read = 0;
        if (ma_decoder_read_pcm_frames(&dec_->ma, scratch, kChunkFrames, &read) != MA_SUCCESS) {
            read = 0;
        }
        if (read == 0) {
            open_track(cursor_ + 1);
            return;
        }
        ring.write(scratch, read * kFrameBytes);
        if (read < kChunkFrames) break;
    }

    // Show the audible head, not the decoder head: subtract what's still
    // queued in the ring (up to ~20 s on a 4 MB buffer).
    ma_uint64 cursor = 0;
    if (ma_decoder_get_cursor_in_pcm_frames(&dec_->ma, &cursor) == MA_SUCCESS) {
        const uint64_t queued = ring.readable() / kFrameBytes;
        const uint64_t played = cursor > queued ? cursor - queued : 0;
        position_ms_.store((played * 1000ull) / 48000ull, std::memory_order_release);
    }
}

TrackInfo LocalFileSource::current_track() const {
    std::scoped_lock lk{mu_};
    TrackInfo info   = dec_->info;
    info.position_ms = position_ms_.load(std::memory_order_acquire);
    return info;
}

void LocalFileSource::set_directory(std::filesystem::path dir, bool recursive) {
    {
        std::scoped_lock lk{mu_};
        if (cfg_.music_dir == dir && cfg_.recursive == recursive) return;
        cfg_.music_dir = std::move(dir);
        cfg_.recursive = recursive;
    }
    rebuild_playlist();
    log::info("[local] rescanned: {} tracks under {}", playlist_.size(), cfg_.music_dir.string());
}

AuthState LocalFileSource::auth_state() const noexcept {
    std::scoped_lock lk{mu_};
    if (cfg_.music_dir.empty()) return AuthState::needs_auth;
    if (!std::filesystem::exists(cfg_.music_dir)) return AuthState::error;
    return playlist_.empty() ? AuthState::needs_auth : AuthState::none_required;
}

std::string LocalFileSource::auth_instructions() const {
    std::scoped_lock lk{mu_};
    if (cfg_.music_dir.empty()) {
        return "Pick a folder containing your music in the Settings drawer "
               "(Local files -> Music directory), then click Save.";
    }
    if (!std::filesystem::exists(cfg_.music_dir))
        return "Music folder doesn't exist: " + cfg_.music_dir.string();
    if (playlist_.empty()) {
        return "No audio files matching the configured extensions were found in " +
               cfg_.music_dir.string();
    }
    return {};
}

std::size_t LocalFileSource::track_count() const noexcept {
    std::scoped_lock lk{mu_};
    return playlist_.size();
}

void LocalFileSource::set_shuffle(bool shuffle) {
    {
        std::scoped_lock lk{mu_};
        cfg_.shuffle = shuffle;
    }
    rebuild_playlist();
}

std::vector<std::string> LocalFileSource::playlist_snapshot() const {
    std::scoped_lock lk{mu_};
    std::vector<std::string> out;
    out.reserve(playlist_.size());
    for (const auto& p : playlist_) out.push_back(p.string());
    return out;
}

} // namespace fh6::sources
