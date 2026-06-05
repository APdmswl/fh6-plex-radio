#include "fh6/fmod/dsp_control_loop.hpp"
#include "fh6/fmod/radio_discovery.hpp"
#include "fh6/audio_source.hpp"
#include "fh6/audio_source_manager.hpp"
#include "fh6/log.hpp"

#include <chrono>
#include <cmath>
#include <utility>

namespace fh6::fmod_bridge {

namespace {
using namespace std::chrono_literals;
constexpr auto kTick           = 20ms;
constexpr auto kDiscoveryRetry = 5s;
constexpr int kDiscoveryTries  = 120; // 10-minute budget; the radio system
                                      // isn't wired up until well into launch.

// Ticks of no read_callback progress (while the source is producing PCM)
// before we conclude the game tore the radio channel down. 1s @ 20ms.
constexpr int kStaleTickThreshold = 50;

// Frozen-read ticks before we treat the station as genuinely silenced (pause
// menu, radio off). Must clear the stall-retune rebuild window, so a rewind or
// race transition that briefly tears the channel down doesn't read as a pause.
constexpr int kInaudibleTicks = 175;

constexpr auto kRetuneCooldown = 6s;
constexpr auto kMetadataRefreshWindow = 5s;
constexpr auto kMetadataRefreshRetry  = 250ms;
constexpr auto kTargetRecheckInterval = 3s;

// SoundName of the placeholder sample our DSP overwrites. Matches the carrier
// shipped by the radio-mod media overlay; if absent, we fall back to the
// first chain-valid instance so a stale overlay doesn't silently break audio.
constexpr const char* kTargetSoundName = "HZ6_R9_PeterBroderick_EyesClosedandTraveling";
} // namespace

ControlLoop::ControlLoop(DSPBridge& bridge, const PEImage& img, PlaybackConfig initial_playback,
                         float configured_gain)
    : bridge_{bridge}, img_{img}, configured_gain_{configured_gain}, game_state_{img},
      playback_opts_{std::make_shared<const PlaybackConfig>(std::move(initial_playback))},
      thread_{[this](const std::stop_token& tok) { run(tok); }} {}

void ControlLoop::push_playback_options(PlaybackConfig opts) {
    auto next = std::make_shared<const PlaybackConfig>(std::move(opts));
    std::lock_guard lock{playback_opts_mtx_};
    playback_opts_ = std::move(next);
}

ControlLoop::~ControlLoop() {
    thread_.request_stop();
    if (thread_.joinable()) thread_.join();
}

void ControlLoop::run(const std::stop_token& tok) {
    log::info("[ctrl] control loop started");

    bool acquired = false;
    for (int attempt = 0; attempt < kDiscoveryTries && !tok.stop_requested(); ++attempt) {
        if (acquire_target()) {
            acquired = true;
            break;
        }
        for (auto t = std::chrono::steady_clock::now() + kDiscoveryRetry;
             std::chrono::steady_clock::now() < t && !tok.stop_requested();)
            std::this_thread::sleep_for(kTick);
    }

    if (!acquired) {
        log::warn("[ctrl] discovery timed out; control loop exiting");
        return;
    }
    next_target_recheck_ = std::chrono::steady_clock::now() + kTargetRecheckInterval;

    // The radio HUD reads from the SampleProperties slots at a much lower
    // rate than the audio mixer. 4 Hz is more than enough and keeps the
    // memory writes off the hot path.
    constexpr int kMetaEveryNTicks = 12; // ~240 ms at the 20 ms tick rate.
    int meta_tick                  = 0;

    auto next = std::chrono::steady_clock::now();
    while (!tok.stop_requested()) {
        const auto now = std::chrono::steady_clock::now();
        if (next < now) next = now;
        next += kTick;
        bridge_.retarget_if_needed();
        bridge_.manager().pump_once();

        if (++meta_tick >= kMetaEveryNTicks) {
            meta_tick = 0;
            if (radio_audible_) push_metadata();
        }
        pump_metadata_refresh(now);
        if (now >= next_target_recheck_) {
            next_target_recheck_ = now + kTargetRecheckInterval;
            if (game_state_.read().on_target_station) acquire_target(false);
        }

        // Staleness watchdog: if the game tears down the radio channel, cycle
        // the in-game station off/on so FH6 allocates a fresh FMOD channel.
        auto* active          = bridge_.manager().active();
        const bool busy       = active && (active->playback_state() == PlaybackState::playing ||
                                           active->playback_state() == PlaybackState::buffering);
        const std::uint64_t c = bridge_.call_count();
        if (busy && c == prev_calls_) {
            if (++stale_ticks_ >= kStaleTickThreshold) {
                stale_ticks_ = 0;
                if (now - last_retune_ >= kRetuneCooldown &&
                    game_state_.read().on_target_station &&
                    game_state_.retune_streamer_station()) {
                    last_retune_ = now;
                    acquire_target();
                }
            }
        } else {
            stale_ticks_ = 0;
        }

        // Audibility edge: while a source is producing, a frozen read_callback
        // means the game silenced our station. Skip metadata refreshes during
        // sustained silence so the HUD does not get re-populated from a stale
        // RadioStreamFmod slot after pause/rewind-style transitions.
        if (active && busy) {
            if (active != audible_source_) {
                audible_source_ = active;
                idle_ticks_     = 0;
                audible_primed_ = false;
                radio_audible_  = true;
            }
            if (c != prev_calls_) {
                idle_ticks_     = 0;
                audible_primed_ = true;
            } else if (audible_primed_) {
                ++idle_ticks_;
            }
            const bool audible = idle_ticks_ < kInaudibleTicks;
            if (audible != radio_audible_) {
                radio_audible_ = audible;
                active->on_radio_audible(audible);
                if (audible) schedule_metadata_refresh(now);
            }
        } else {
            idle_ticks_     = 0;
            audible_primed_ = false;
            radio_audible_  = true;
            audible_source_ = nullptr;
        }

        run_playback_state_machines(now);
        prev_calls_ = c;

        const float target = [this, active] {
            if (!active) return 0.0f;
            switch (active->playback_state()) {
                case PlaybackState::playing:
                case PlaybackState::buffering:
                    return configured_gain_.load(std::memory_order_acquire);
                default: return 0.0f;
            }
        }();
        // 1-pole low-pass at ~100 ms so play/pause fades smoothly.
        const float cur = bridge_.gain();
        float next_g    = cur + (target - cur) * 0.1f;
        if (std::abs(next_g - cur) < 1e-4f) next_g = target;
        bridge_.set_gain(next_g);

        std::this_thread::sleep_until(next);
    }
    log::info("[ctrl] control loop exiting");
}

bool ControlLoop::acquire_target(bool allow_fallback) noexcept {
    auto disc = discover_radio_instances(img_);
    const RadioInstance* chosen = select_instance(disc, allow_fallback && !exact_target_seen_);
    if (!chosen) return false;
    const bool exact_target = chosen->sound_name == kTargetSoundName;
    if (!exact_target) {
        log::warn(R"([ctrl] no instance matches target "{}"; falling back to "{}")",
                  kTargetSoundName, chosen->sound_name);
    } else {
        exact_target_seen_ = true;
    }

    void* fmod_system = resolve_fmod_system(img_, chosen->radio_stream);
    if (!fmod_system) {
        log::warn("[ctrl] FMOD SystemI resolution failed");
        return false;
    }
    const bool changed = chosen->radio_stream != target_radio_stream_ ||
                         chosen->sample_props_body != target_sample_props_;
    if (changed) {
        bridge_.set_target(*chosen, fmod_system);
        meta_.set_target(chosen->sample_props_body);
        target_radio_stream_ = chosen->radio_stream;
        target_sample_props_ = chosen->sample_props_body;
        log::info("[ctrl] targeting RadioStreamFmod @0x{:X} SampleProperties=0x{:X} "
                  "SoundName=\"{}\" SystemI*=0x{:X}",
                  reinterpret_cast<uintptr_t>(chosen->radio_stream),
                  reinterpret_cast<uintptr_t>(chosen->sample_props_body), chosen->sound_name,
                  reinterpret_cast<uintptr_t>(fmod_system));
    }
    return true;
}

const RadioInstance* ControlLoop::select_instance(const DiscoveryResult& disc,
                                                  bool allow_fallback) const noexcept {
    const RadioInstance* target = nullptr;
    const RadioInstance* fallback = nullptr;
    for (auto& i : disc.instances) {
        const bool is_target = i.sound_name == kTargetSoundName;
        if (is_target && bridge_.channel_handle_alive(i.radio_stream)) return &i;
        if (is_target && !target) target = &i;
        if (!fallback) fallback = &i;
    }
    return target ? target : (allow_fallback ? fallback : nullptr);
}

void ControlLoop::schedule_metadata_refresh(time_point now) noexcept {
    metadata_refresh_until_ = now + kMetadataRefreshWindow;
    next_metadata_refresh_  = now;
}

void ControlLoop::pump_metadata_refresh(time_point now) noexcept {
    if (metadata_refresh_until_ == time_point{} || now > metadata_refresh_until_) {
        metadata_refresh_until_ = {};
        next_metadata_refresh_  = {};
        return;
    }
    if (now < next_metadata_refresh_) return;

    meta_.reset_cache();
    if (radio_audible_) push_metadata();
    next_metadata_refresh_ = now + kMetadataRefreshRetry;
}

void ControlLoop::run_playback_state_machines(time_point now) noexcept {
    using namespace std::chrono_literals;
    constexpr auto kQuickSkipWindow     = 1000ms;
    constexpr auto kCommandCooldown     = 1500ms;
    constexpr auto kRaceStartDebounce   = 45s;
    constexpr auto kRaceRestartDebounce = 5s;

    std::shared_ptr<const PlaybackConfig> opts;
    {
        std::lock_guard lock{playback_opts_mtx_};
        opts = playback_opts_;
    }
    if (!opts) return;

    auto* active = bridge_.manager().active();
    if (!active) {
        prev_r10_ = prev_race_ = prev_race_restart_ = false;
        quick_skip_armed_ = false;
        return;
    }

    const auto game = game_state_.read();
    const bool r10  = game.on_target_station;
    auto& ring      = bridge_.manager().ring();

    const bool race_edge_in    = game.race_active && !prev_race_;
    const bool restart_edge_in = game.race_restart && !prev_race_restart_;
    const bool race_event      = (race_edge_in || restart_edge_in) && r10;
    const auto race_debounce   = restart_edge_in ? kRaceRestartDebounce : kRaceStartDebounce;
    if (race_event) {
        acquire_target();
        schedule_metadata_refresh(now);
    }
    if (race_event && now - last_race_event_ >= race_debounce &&
        now - last_skip_cmd_ >= kCommandCooldown) {
        const auto& mode    = opts->race_start_playback;
        const char* outcome = "keeping current position";
        bool fired          = false;
        if (mode == "next") {
            fired   = active->skip_next();
            outcome = fired ? "advanced to next track" : "could not advance queue";
        } else if (mode == "restart") {
            fired   = active->restart_current();
            outcome = fired ? "restarted current track" : "could not restart current track";
        }
        if (fired) {
            ring.drain();
            last_skip_cmd_ = now;
        }
        schedule_metadata_refresh(now);
        last_race_event_ = now;
        log::info("[ctrl] race {} -- {}", restart_edge_in ? "restarted" : "started", outcome);
    }
    prev_race_         = game.race_active;
    prev_race_restart_ = game.race_restart;

    if (prev_r10_ && !r10) {
        last_r10_off_ = now;
        if (opts->quick_station_skip) quick_skip_armed_ = true;
    } else if (!prev_r10_ && r10) {
        acquire_target();
        schedule_metadata_refresh(now);
        if (quick_skip_armed_ && now - last_r10_off_ <= kQuickSkipWindow &&
            now - last_skip_cmd_ >= kCommandCooldown) {
            if (active->skip_next()) {
                ring.drain();
                last_skip_cmd_ = now;
                log::info("[ctrl] quick station return -- advanced to next track");
            }
        }
        quick_skip_armed_ = false;
    }
    prev_r10_ = r10;
}

void ControlLoop::push_metadata() noexcept {
    auto* a = bridge_.manager().active();
    if (!a) {
        meta_.update("FH6 Universal Radio", "Idle");
        return;
    }
    TrackInfo info;
    try {
        info = a->current_track();
    } catch (...) {
        return;
    }
    std::string title  = !info.title.empty() ? info.title : std::string{a->display_name()};
    std::string artist = info.artist;
    if (artist.empty()) {
        switch (a->playback_state()) {
            case PlaybackState::playing:   artist = "Playing"; break;
            case PlaybackState::buffering: artist = "Buffering"; break;
            case PlaybackState::paused:    artist = "Paused"; break;
            case PlaybackState::stopped:   artist = "Stopped"; break;
        }
    }
    meta_.update(title, artist);
}

} // namespace fh6::fmod_bridge
