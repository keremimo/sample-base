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
    assert(codec.found() && codec.dir == link && codec.clock == clock);
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
    return 0;
}
