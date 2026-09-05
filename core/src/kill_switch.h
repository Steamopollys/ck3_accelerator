#pragma once
#include <functional>
#include <string_view>

namespace ck3accel {

struct Hotkey {
    bool ctrl  = false;
    bool shift = false;
    bool alt   = false;
    int  vk    = 0;       // Windows virtual-key code
    bool valid = false;
};

// parse "Ctrl+Shift+F12" combos: Ctrl/Shift/Alt + F1..F24, A..Z, 0..9.
// valid=false on error.
Hotkey parse_hotkey(std::string_view combo);

// edge detector: true once per rising edge. was_down is caller state (init false).
bool detect_edge(bool combo_down_now, bool& was_down);

// start the ~40ms poll thread. on_trigger fires once per press.
void kill_switch_start(Hotkey hk, std::function<void()> on_trigger);
void kill_switch_stop();

// Latched engaged state; backs CoreApi.is_kill_switch_active (returns 0/1).
int is_kill_switch_active();

} // namespace ck3accel
