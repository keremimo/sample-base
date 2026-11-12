// SPDX-License-Identifier: AGPL-3.0-only
#include "drivers/codec_power.h"
#include <cassert>
#include <fstream>
#include <string>
#include <sys/stat.h>

static void write_file(const std::string& path, const std::string& text) { std::ofstream(path) << text; }
static std::string first_line(const std::string& path) {
    std::ifstream file(path);
    std::string text;
    std::getline(file, text);
    return text;
}

int main(int argc, char** argv) {
    assert(argc == 2);
    const std::string root = argv[1], sys = root + "/sys", clock = root + "/clk_rate";
    const std::string card = sys + "/class/sound/card2/device", link = card + "/fe410000.i2s-ak4558-hifi";
    for (const auto& dir : {sys, sys + "/class", sys + "/class/sound", sys + "/class/sound/card2", card, link})
        if (mkdir(dir.c_str(), 0755) != 0) return 1;
    // The id snd_pcm_info() gives the MPC's I2S playback PCM: the link, then the codec DAI.
    const std::string id = "fe410000.i2s-ak4558-hifi ak4558-hifi-0";

    // A link counts only with a pmdown_time to write.
    assert(!codec_link(2, id, sys, clock).found());
    write_file(link + "/pmdown_time", "5000\n");
    CodecLink codec = codec_link(2, id, sys, clock);
    assert(codec.found() && codec.dir == link && codec.clock == clock && codec.card == 2);
    assert(!codec_link(1, id, sys, clock).found());          // another card
    assert(!codec_link(-1, "null", sys, clock).found());     // a plug-in PCM has no card
    assert(!codec_link(2, "", sys, clock).found());
    assert(!codec_link(2, "../card2 x", sys, clock).found()); // never outside the card's folder

    // Powered: the codec's bias, the last line of dapm_widget.
    assert(!codec_powered(codec));
    write_file(link + "/dapm_widget", "DACL: On\nDACR: On\nADCL: Off\nADCR: Off\nPM State: On\n");
    assert(codec_powered(codec));
    write_file(link + "/dapm_widget", "DACL: Off\nDACR: Off\nADCL: Off\nADCR: Off\nPM State: Off\n");
    assert(!codec_powered(codec));
    write_file(link + "/dapm_widget", "PM State: Standby\n");
    assert(codec_powered(codec));
    assert(!codec_powered(CodecLink {}));

    // The rate the master clock is set for, at 512 fs.
    assert(codec_clock_rate(codec) == 0);
    write_file(clock, "24576000\n");
    assert(codec_clock_rate(codec) == 48000);
    write_file(clock, "22579200\n");
    assert(codec_clock_rate(codec) == 44100);
    write_file(clock, "22579195\n"); // a fractional divider lands a few Hz off
    assert(codec_clock_rate(codec) == 44100);
    write_file(clock, "49152000\n");
    assert(codec_clock_rate(codec) == 96000);
    write_file(clock, "12000000\n"); // 23437.5 Hz: no rate
    assert(codec_clock_rate(codec) == 0);
    write_file(clock, "fast\n");
    assert(codec_clock_rate(codec) == 0);
    assert(codec_clock_rate(CodecLink {}) == 0);

    // pmdown_time is 0 while one lives, then what it was.
    {
        ImmediatePowerDown now(codec);
        assert(first_line(link + "/pmdown_time") == "0");
    }
    assert(first_line(link + "/pmdown_time") == "5000");
    { ImmediatePowerDown nothing {CodecLink {}}; }
    assert(first_line(link + "/pmdown_time") == "5000");
    // Already 0 (another one alive): left as it is, and not set to 0 afterwards by mistake.
    write_file(link + "/pmdown_time", "0\n");
    { ImmediatePowerDown again(codec); }
    assert(first_line(link + "/pmdown_time") == "0");

    // The speaker held off while the codec powers up, on a switch that counts its writes (card 2 has one).
    int speaker = 1, writes = 0;
    SwitchIo io {[&](int on_card) { return on_card == 2 ? speaker : -1; },
                 [&](int on_card, bool on) { if (on_card != 2) return false; speaker = on; ++writes; return true; }};
    const auto t = SpeakerHold::Clock::time_point {} + std::chrono::hours(1);
    {
        SpeakerHold hold(io);
        assert(hold.original() == -1 && !hold.holding() && !hold.defer(false));
        hold.hold(1); // no speaker on that card: nothing to hold
        assert(!hold.holding() && writes == 0);
        hold.hold(2);
        assert(hold.holding() && speaker == 0 && writes == 1 && hold.original() == 1);
        hold.poll(t + std::chrono::hours(9)); // the codec not up yet
        assert(hold.holding() && speaker == 0);
        hold.powered_up(t);
        hold.poll(t + codec_settle - std::chrono::milliseconds(1));
        assert(hold.holding() && speaker == 0);
        hold.poll(t + codec_settle);
        assert(!hold.holding() && speaker == 1 && writes == 2);
        // The app's changes wait for the end, the last one counting; a second power-up restarts the wait.
        hold.hold(2);
        assert(speaker == 0 && writes == 3);
        assert(hold.defer(true) && hold.defer(false) && speaker == 0);
        hold.powered_up(t);
        hold.hold(2);
        assert(hold.holding() && writes == 3);
        hold.powered_up(t + std::chrono::seconds(1));
        hold.poll(t + std::chrono::milliseconds(1500));
        assert(hold.holding());
        hold.poll(t + std::chrono::seconds(2));
        assert(!hold.holding() && speaker == 0 && writes == 3); // it waited for off: nothing to write
        // Held while already off: turning it on waits too.
        hold.hold(2);
        assert(hold.holding() && writes == 3 && hold.defer(true));
        hold.powered_up(t);
        hold.poll(t + codec_settle);
        assert(speaker == 1 && writes == 4 && hold.original() == 1);
        hold.hold(2);
        assert(speaker == 0 && writes == 5);
    }
    // Leaving while held: the switch as it was before the first hold.
    assert(speaker == 1 && writes == 6);
    speaker = 0; writes = 0;
    {
        SpeakerHold hold(io);
        hold.hold(2);
        assert(hold.holding() && writes == 0 && hold.original() == 0);
    }
    assert(speaker == 0 && writes == 0);
    return 0;
}
