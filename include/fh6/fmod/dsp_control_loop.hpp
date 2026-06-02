#pragma once

#include "fh6/config.hpp"
#include "fh6/fmod/dsp_bridge.hpp"
#include "fh6/fmod/game_state_probe.hpp"
#include "fh6/fmod/metadata_injector.hpp"
#include "fh6/fmod/pe_image.hpp"
#include "fh6/fmod/radio_discovery.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <stop_token>
#include <thread>

namespace fh6 { class IAudioSource; }

namespace fh6::fmod_bridge {

// 50 Hz tick: keeps the DSP installed on the current radio channel and
// drives the AudioSourceManager pump.
class ControlLoop {
public:
    ControlLoop(DSPBridge& bridge, const PEImage& img, PlaybackConfig initial_playback,
                float configured_gain);
    ~ControlLoop();

    ControlLoop(const ControlLoop&)            = delete;
    ControlLoop& operator=(const ControlLoop&) = delete;

    void set_configured_gain(float g) noexcept {
        configured_gain_.store(g, std::memory_order_release);
    }

    void push_playback_options(PlaybackConfig opts);

private:
    using clock      = std::chrono::steady_clock;
    using time_point = clock::time_point;

    void run(const std::stop_token& tok);
    void push_metadata() noexcept;
    void schedule_metadata_refresh(time_point now) noexcept;
    void pump_metadata_refresh(time_point now) noexcept;
    void run_playback_state_machines(time_point now) noexcept;

    bool acquire_target(bool allow_fallback = true) noexcept;
    const RadioInstance* select_instance(const DiscoveryResult& disc,
                                         bool allow_fallback) const noexcept;

    DSPBridge& bridge_;
    const PEImage& img_;
    std::atomic<float> configured_gain_;
    MetadataInjector meta_;
    GameStateProbe game_state_;
    std::uint64_t prev_calls_ = 0;
    int stale_ticks_          = 0;
    int idle_ticks_           = 0;
    bool radio_audible_       = true;
    bool audible_primed_      = false;
    IAudioSource* audible_source_ = nullptr;
    time_point last_retune_{};
    time_point next_target_recheck_{};
    time_point metadata_refresh_until_{};
    time_point next_metadata_refresh_{};
    std::byte* target_radio_stream_ = nullptr;
    std::byte* target_sample_props_ = nullptr;
    bool exact_target_seen_ = false;

    mutable std::mutex playback_opts_mtx_;
    std::shared_ptr<const PlaybackConfig> playback_opts_;
    bool prev_r10_          = false;
    bool prev_race_         = false;
    bool prev_race_restart_ = false;
    bool quick_skip_armed_  = false;
    time_point last_r10_off_{};
    time_point last_race_event_{};
    time_point last_skip_cmd_{};

    std::jthread thread_;
};

} // namespace fh6::fmod_bridge
