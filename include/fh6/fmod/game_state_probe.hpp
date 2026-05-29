#pragma once

#include "fh6/fmod/pe_image.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace fh6::fmod_bridge {

class GameStateProbe {
public:
    explicit GameStateProbe(const PEImage& img) noexcept;

    bool resolved() const noexcept { return singleton_slot_ != nullptr; }

    struct Snapshot {
        bool on_target_station = false;
        bool race_active       = false;
        bool race_restart      = false;
    };
    Snapshot read() const noexcept;

    bool retune_streamer_station() noexcept;

private:
    using SetStationFn = void (*)(void* radio_state, const void* name);
    void set_station(const std::byte* radio_state, std::string_view name) const noexcept;

    // Address of FH6's radio_state global pointer. FH6 reallocates the
    // pointed object during world loads, so read() dereferences it each tick.
    const void* const* singleton_slot_ = nullptr;
    SetStationFn set_station_fn_ = nullptr;
};

} // namespace fh6::fmod_bridge
