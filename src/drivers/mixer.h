// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <alsa/asoundlib.h>
#include <map>
#include <string>
#include <vector>

// ALSA controls, including DAPM switches and read-only jack detection. Only
// explicit commands change routes; every changed value is restored on exit.
class HardwareMixer {
public:
    explicit HardwareMixer(const std::string& card = "hw:I2S");
    ~HardwareMixer();
    HardwareMixer(const HardwareMixer&) = delete;
    HardwareMixer& operator=(const HardwareMixer&) = delete;
    bool set(unsigned id, const std::vector<long>& values);
    bool input(const std::string& source);
    // The built-in speaker on or off; restored with the other changes on exit.
    bool speaker(bool enabled) { return named_switch("Builtin Speaker Switch", enabled); }
    std::string json() const;
    // The first value of a readable boolean control such as "Headphones
    // Jack" (jack controls are card controls, switches mixer controls):
    // 1 or 0, or -1 without it.
    int switch_value(const char* name) const;
    bool available() const { return ctl != nullptr; }
private:
    bool named_switch(const char* name, bool enabled);
    void silence_inputs();
    snd_ctl_t* ctl = nullptr;
    bool routes_changed = false;
    std::map<unsigned, snd_ctl_elem_value_t*> original;
};
