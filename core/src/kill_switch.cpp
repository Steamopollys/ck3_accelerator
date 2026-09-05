#include "kill_switch.h"
#include "logger.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace ck3accel {

namespace {

    // --- module state for the poll thread -----------------------------------
    std::thread        g_poll_thread;
    HANDLE             g_stop_event = nullptr;   // manual-reset; signaled to stop
    std::atomic<int>   g_active{0};              // latched engaged state (0/1)

    std::string to_lower(std::string_view sv) {
        std::string s(sv);
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s;
    }

    std::string trim(std::string_view sv) {
        std::size_t b = 0;
        std::size_t e = sv.size();
        while (b < e && std::isspace(static_cast<unsigned char>(sv[b]))) ++b;
        while (e > b && std::isspace(static_cast<unsigned char>(sv[e - 1]))) --e;
        return std::string(sv.substr(b, e - b));
    }

    // lower-cased main-key token to a VK code; 0 if unrecognized.
    int main_key_vk(const std::string& tok) {
        if (tok.empty()) return 0;

        // F1..F24
        if ((tok[0] == 'f') && tok.size() >= 2) {
            bool all_digits = true;
            for (std::size_t i = 1; i < tok.size(); ++i) {
                if (tok[i] < '0' || tok[i] > '9') { all_digits = false; break; }
            }
            if (all_digits) {
                int n = 0;
                for (std::size_t i = 1; i < tok.size(); ++i) {
                    n = n * 10 + (tok[i] - '0');
                }
                if (n >= 1 && n <= 24) {
                    return VK_F1 + (n - 1);   // VK_F1..VK_F24 are contiguous
                }
                return 0;
            }
        }

        // Single A..Z or 0..9
        if (tok.size() == 1) {
            char c = tok[0];
            if (c >= 'a' && c <= 'z') {
                return static_cast<int>('A' + (c - 'a'));   // uppercase VK for letters
            }
            if (c >= '0' && c <= '9') {
                return static_cast<int>(c);                 // '0'..'9' VK == ASCII
            }
        }

        return 0;
    }

    // true iff every required key is down per GetAsyncKeyState.
    bool combo_down(const Hotkey& hk) {
        auto down = [](int vk) {
            return (GetAsyncKeyState(vk) & 0x8000) != 0;
        };
        if (hk.ctrl  && !down(VK_CONTROL)) return false;
        if (hk.shift && !down(VK_SHIFT))   return false;
        if (hk.alt   && !down(VK_MENU))    return false;
        return down(hk.vk);
    }

} // namespace

Hotkey parse_hotkey(std::string_view combo) {
    Hotkey hk;

    std::vector<std::string> tokens;
    {
        std::string trimmed = trim(combo);
        std::size_t start = 0;
        for (std::size_t i = 0; i <= trimmed.size(); ++i) {
            if (i == trimmed.size() || trimmed[i] == '+') {
                std::string tok = trim(std::string_view(trimmed).substr(start, i - start));
                if (!tok.empty()) tokens.push_back(to_lower(tok));
                start = i + 1;
            }
        }
    }

    if (tokens.empty()) return hk;   // valid stays false

    bool have_main = false;
    for (const auto& tok : tokens) {
        if (tok == "ctrl" || tok == "control") {
            hk.ctrl = true;
        } else if (tok == "shift") {
            hk.shift = true;
        } else if (tok == "alt") {
            hk.alt = true;
        } else {
            int vk = main_key_vk(tok);
            if (vk == 0)  return Hotkey{};   // unknown token -> invalid
            if (have_main) return Hotkey{};  // more than one main key -> invalid
            hk.vk = vk;
            have_main = true;
        }
    }

    if (!have_main) return Hotkey{};   // modifiers only -> invalid
    hk.valid = true;
    return hk;
}

bool detect_edge(bool combo_down_now, bool& was_down) {
    bool fire = combo_down_now && !was_down;
    was_down = combo_down_now;
    return fire;
}

void kill_switch_start(Hotkey hk, std::function<void()> on_trigger) {
    if (!hk.valid) {
        LOG_WARN("kill_switch: hotkey is invalid; panic hotkey disabled");
        return;
    }
    if (g_poll_thread.joinable()) {
        LOG_WARN("kill_switch: already started; ignoring duplicate start");
        return;
    }

    g_stop_event = CreateEventW(nullptr, /*manualReset=*/TRUE, /*initial=*/FALSE, nullptr);
    if (!g_stop_event) {
        LOG_ERROR("kill_switch: CreateEventW failed; panic hotkey disabled");
        return;
    }

    HANDLE stop = g_stop_event;
    g_poll_thread = std::thread([hk, on_trigger = std::move(on_trigger), stop]() {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
        bool was_down = false;
        for (;;) {
            if (WaitForSingleObject(stop, /*ms=*/40) == WAIT_OBJECT_0) {
                break;
            }
            if (detect_edge(combo_down(hk), was_down)) {
                g_active.store(1, std::memory_order_release);
                if (on_trigger) on_trigger();
            }
        }
    });

    std::ostringstream os;
    os << "kill_switch: polling for panic hotkey (ctrl=" << hk.ctrl
       << " shift=" << hk.shift << " alt=" << hk.alt << " vk=0x"
       << std::hex << hk.vk << ")";
    LOG_INFO(os.str());
}

void kill_switch_stop() {
    if (g_stop_event) {
        SetEvent(g_stop_event);
    }
    if (g_poll_thread.joinable()) {
        g_poll_thread.join();
    }
    if (g_stop_event) {
        CloseHandle(g_stop_event);
        g_stop_event = nullptr;
    }
}

int is_kill_switch_active() {
    return g_active.load(std::memory_order_acquire);
}

} // namespace ck3accel
