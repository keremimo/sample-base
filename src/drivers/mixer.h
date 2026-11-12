// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include "drivers/codec_power.h"
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
    // While the speaker is held off for the codec's power-up (SpeakerHold),
    // the change waits for the hold to end, and true means it will be made.
    bool speaker(bool enabled) { return named_switch(speaker_switch, enabled); }
    // The hold that keeps the speaker off while the codec powers up (it
    // outlives the mixer); poll() ends it on time, every service pass.
    void attach(SpeakerHold* hold) { speaker_hold = hold; }
    void poll() { if (speaker_hold) speaker_hold->poll(); }
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
    SpeakerHold* speaker_hold = nullptr;
};

// The speaker switch of a card ("hw:N") for SpeakerHold, through ALSA.
SwitchIo speaker_switch_io();
