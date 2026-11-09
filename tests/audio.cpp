// SPDX-License-Identifier: AGPL-3.0-only
#include "audio.h"
#include <cassert>
#include <cerrno>

static unsigned reads = 0, writes = 0, waits = 0, prepares = 0;
extern "C" int __real_snd_pcm_prepare(snd_pcm_t* pcm);
extern "C" int __wrap_snd_pcm_prepare(snd_pcm_t* pcm) { ++prepares; return __real_snd_pcm_prepare(pcm); }
extern "C" int __wrap_snd_pcm_start(snd_pcm_t*) { return 0; }
extern "C" int __wrap_snd_pcm_wait(snd_pcm_t*, int timeout) { assert(timeout == 100); ++waits; return 1; }
extern "C" snd_pcm_sframes_t __wrap_snd_pcm_readi(snd_pcm_t*, void* output, snd_pcm_uframes_t frames) {
    ++reads;
    if (reads == 2) return -EAGAIN;
    if (reads == 4) return -EPIPE;
    unsigned count = reads <= 3 ? 64 : 128;
    assert(count <= frames);
    auto* samples = static_cast<int32_t*>(output);
    for (unsigned i = 0; i < count * 2; ++i) samples[i] = reads == 1 ? 100 : reads == 3 ? 200 : 300;
    return count;
}
extern "C" snd_pcm_sframes_t __wrap_snd_pcm_writei(snd_pcm_t*, const void*, snd_pcm_uframes_t frames) {
    ++writes;
    if (writes == 1) return -EAGAIN;
    if (writes == 4) return -EPIPE;
    return writes <= 3 ? 64 : frames;
}
struct Context { unsigned callbacks = 0; std::atomic<bool> running {true}; };
static void render(void* pointer, const AudioBlock& input, AudioBlock& output) {
    auto& context = *static_cast<Context*>(pointer);
    ++context.callbacks;
    if (context.callbacks == 1 || context.callbacks == 3) for (auto sample : input) assert(sample == 0);
    if (context.callbacks == 2) {
        for (unsigned i = 0; i < 128; ++i) assert(input[i] == 100);
        for (unsigned i = 128; i < 256; ++i) assert(input[i] == 200);
    }
    if (context.callbacks == 4) {
        for (auto sample : input) assert(sample == 300);
        context.running.store(false);
    }
    output.fill(107374182);
}
int main() {
    AudioDevice device("null", "null");
    prepares = 0;
    Context context; AudioStats stats; std::atomic<bool> failed {false};
    device.run(context.running, failed, stats, render, &context);
    assert(!failed && context.callbacks == 4);
    assert(stats.frames == 384 && stats.capture_frames == 256);
    assert(stats.xruns == 1 && stats.capture_xruns == 1 && waits == 1 && prepares == 2);
    assert(stats.peak > 0.049f && stats.peak < 0.051f);
}
