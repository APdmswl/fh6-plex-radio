#pragma once

#include "fh6/fmod/pe_image.hpp"

#include <cstdint>

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

private:
    // Address of FH6's radio_state global pointer. FH6 reallocates the
    // pointed object during world loads, so read() dereferences it each tick.
    const void* const* singleton_slot_ = nullptr;
};

} // namespace fh6::fmod_bridge
