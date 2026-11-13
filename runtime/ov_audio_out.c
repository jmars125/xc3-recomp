// nn::audio AudioOut on SDL3: XC3 streams PCM through AudioOut next to the audio renderer (XC3 used only the
// renderer). Each open AudioOut is an SDL audio stream of 16-bit samples at 48 kHz with the channel count the game
// asked for. AppendAudioOutBuffer queues a buffer's data on the stream; a buffer is released once the device has
// consumed every byte up to its end, which GetReleasedAudioOutBuffer reports in append order. Without an audio device
// the data is consumed at the real-time rate, so the game's pacing stays the same.
//
// Guest layouts (SDK 19.3.2): AudioOutBuffer {next, data, capacity, size} at +0, +8, +0x10, +0x18, filled by the SDK's
// own SetAudioOutBufferInfo; AudioOutParameter {int32 sample rate, uint16 channel count, uint16 0xCAFE}, 0 meaning the
// default. The AudioOut object itself is only a key here.
//
// A watch thread (watch_main) checks every open AudioOut four times a second: it signals the release event of an
// AudioOut without a device (nothing else would), reports each AudioOut 5 s and 60 s after it starts (how much the device
// took, the loudest sample, the gain), and recovers a device that stops taking data, first by opening the stream
// again, then by running the AudioOut without a device so the game's audio thread keeps going.
#include <SDL3/SDL.h>
#include <stdint.h>
#include <string.h>

#include "host.h"
#include "platform/platform.h"

#define OUT_MAX 8
#define QUEUE_MAX 64
#define RATE 48000
#define FMT_PCM16 2
enum { STATE_STARTED = 0, STATE_STOPPED = 1 };

typedef struct { uint64_t buf, end; } Queued;   // end: total bytes appended once this buffer's data is in
typedef struct {
    uint64_t guest, event;   // event: the SystemEvent of the overloads that take one, signalled on release; 0 if none
    uint32_t channels, state;
    float volume, gain;      // gain: what the stream has, the game's volume times the player's
    SDL_AudioStream *stream;
    uint64_t appended, consumed_at_stop, start_ns;   // byte counts; start_ns: the device-less clock
    uint64_t started_ns, seen, moved_ns;   // seen: consumed() at the last watch; moved_ns: when it last changed
    uint64_t appends, nonzero;             // buffers appended, buffers with a nonzero sample
    int peak, reported, reopened;          // peak: the loudest sample appended (0..32768); reported: reports made
    Queued q[QUEUE_MAX];
    int nq;
} Out;

static Out outs[OUT_MAX];
static PlatMutex lock = PLAT_MUTEX_INIT;
static int sdl_ready = -1, watching;

#define STALL_NS 2000000000ull   // a started AudioOut with data queued whose device took nothing for this long
static const uint64_t REPORT_NS[] = {5000000000ull, 60000000000ull};   // the reports, after the first start

static uint64_t now_ns(void) { return plat_now_ns(); }

// The SDL audio subsystem for both outputs (this file and audio_mixer.c output_open). On Linux SDL 3 tries PipeWire
// before PulseAudio. On a ROG Ally running SteamOS (issue #35) the PipeWire driver opened its default device and took
// every byte, yet nothing was heard, while SDL_AUDIO_DRIVER=pulseaudio played through the speakers. PulseAudio comes
// first here: PipeWire systems serve it through pipewire-pulse, PulseAudio systems natively, and SDL moves on to the
// next name when a driver cannot start. SDL_AUDIO_DRIVER in the environment still decides (SDL lets it override a
// hint of normal priority).
int audio_sdl_init(void) {
#ifdef __linux__
    SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "pulseaudio,pipewire,alsa,jack,sndio,oss");
#endif
    return SDL_InitSubSystem(SDL_INIT_AUDIO) ? 1 : 0;
}

static Out *find(uint64_t guest) {
    for (int i = 0; i < OUT_MAX; i++)
        if (outs[i].guest == guest) return &outs[i];
    return NULL;
}

static uint32_t frame_bytes(const Out *o) { return 2 * o->channels; }

// Bytes the device has taken so far. Under lock.
static uint64_t consumed(const Out *o) {
    if (o->state != STATE_STARTED) return o->consumed_at_stop;
    if (o->stream) {
        int queued = SDL_GetAudioStreamQueued(o->stream);
        return queued < 0 || (uint64_t)queued > o->appended ? o->consumed_at_stop : o->appended - (uint64_t)queued;
    }
    uint64_t played = o->consumed_at_stop + (now_ns() - o->start_ns) * RATE / 1000000000 * frame_bytes(o);
    return played < o->appended ? played : o->appended;
}

// The SDL device pulls on its own thread: a release there wakes a game waiting on the buffer event. SDL holds the
// stream's lock around this callback, and the functions below hold `lock` while they call into the stream
// (consumed), so waiting for `lock` here deadlocks with them: the game's audio thread then never returns from
// GetReleasedAudioOutBuffer, keeps its virtual core, and the threads bound to that core stop (XC3 after New Game).
// A busy `lock` skips this pull; the next one, a few milliseconds later, signals the release.
static void SDLCALL device_pull(void *user, SDL_AudioStream *s, int additional, int total) {
    (void)s, (void)additional, (void)total;
    Out *o = user;
    uint64_t ev = 0;
    if (!plat_mutex_trylock_shared(&lock)) return;
    if (o->guest && o->event && o->nq && consumed(o) >= o->q[0].end) ev = o->event;
    plat_mutex_unlock_shared(&lock);
    if (ev) kern_signal_system_event(ev);
}

static SDL_AudioStream *open_stream(Out *o) {
    SDL_AudioSpec spec = {SDL_AUDIO_S16LE, (int)o->channels, RATE};
    SDL_AudioStream *s = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
    if (!s) {
        rt_log("audout: opening the default playback device failed (%s)", SDL_GetError());
        return NULL;
    }
    SDL_SetAudioStreamGetCallback(s, device_pull, o);
    SDL_AudioDeviceID dev = SDL_GetAudioStreamDevice(s);
    SDL_AudioSpec ds = {0};
    int frames = 0;
    const char *name = SDL_GetAudioDeviceName(dev);
    if (!SDL_GetAudioDeviceFormat(dev, &ds, &frames)) ds = (SDL_AudioSpec){0};
    const char *drv = SDL_GetCurrentAudioDriver();
    rt_log("audout: SDL driver %s, device '%s', %d Hz %d channels format %#x, %d frames per pull", drv ? drv : "?",
           name ? name : "?", ds.freq, ds.channels, (unsigned)ds.format, frames);
    return s;
}

// The playback devices SDL sees, once: a missing or wrong default shows here.
static void list_devices(void) {
    int n = 0, v = SDL_GetVersion();
    SDL_AudioDeviceID *ids = SDL_GetAudioPlaybackDevices(&n);
    const char *drv = SDL_GetCurrentAudioDriver();
    rt_log("audout: %d playback device%s with the %s driver (SDL %d.%d.%d)", n, n == 1 ? "" : "s", drv ? drv : "?",
           SDL_VERSIONNUM_MAJOR(v), SDL_VERSIONNUM_MINOR(v), SDL_VERSIONNUM_MICRO(v));
    for (int i = 0; ids && i < n; i++) {
        const char *name = SDL_GetAudioDeviceName(ids[i]);
        rt_log("audout:   %u '%s'", (unsigned)ids[i], name ? name : "?");
    }
    SDL_free(ids);
}

// Under lock (exclusive). *signal: the release event to signal once the lock is gone, for an AudioOut without a device.
static void watch_one(Out *o, uint64_t now, uint64_t *signal) {
    if (o->state != STATE_STARTED) return;
    uint64_t took = consumed(o);
    if (took != o->seen || o->appended == took) o->seen = took, o->moved_ns = now;
    if (o->reported < 2 && now - o->started_ns >= REPORT_NS[o->reported]) {
        o->reported++;
        float player = audio_player_volume();
        rt_log("audout: %#llx %.1f s after start: %llu buffers (%llu with sound, loudest sample %d of 32768), %llu bytes "
               "appended, the %s took %llu; gain %.2f (the game's %.2f, the player's %.2f)",
               (unsigned long long)o->guest, (double)(now - o->started_ns) / 1e9, (unsigned long long)o->appends,
               (unsigned long long)o->nonzero, o->peak, (unsigned long long)o->appended,
               o->stream ? "device" : "clock", (unsigned long long)took, o->volume * player, o->volume, player);
    }
    if (o->stream && now - o->moved_ns >= STALL_NS) {
        // SDL calls device_pull with the stream's lock held and device_pull only tries `lock`, so destroying the
        // stream here, under `lock`, cannot deadlock with it.
        SDL_DestroyAudioStream(o->stream);
        o->stream = o->reopened ? NULL : open_stream(o);
        rt_log("audout: %#llx: the device took nothing for %.1f s with %llu bytes queued; %s",
               (unsigned long long)o->guest, (double)(now - o->moved_ns) / 1e9,
               (unsigned long long)(o->appended - took),
               o->stream ? "opened the device again" : "running without a device (timed)");
        o->reopened = 1;
        // The data queued on the old stream counts as played: consumed() of a new stream is appended minus its own
        // queue, and the clock without a device starts from everything appended.
        o->consumed_at_stop = o->appended;
        o->start_ns = now;
        o->seen = o->appended, o->moved_ns = now;
        o->gain = 1.0f;
        if (o->stream) SDL_ResumeAudioStreamDevice(o->stream);
    }
    if (!o->stream && o->event && o->nq && consumed(o) >= o->q[0].end) *signal = o->event;
}

static void watch_main(void *arg) {
    (void)arg;
    for (;;) {
        plat_sleep_ms(250);
        uint64_t ev[OUT_MAX] = {0}, now = now_ns();
        plat_mutex_lock(&lock);
        for (int i = 0; i < OUT_MAX; i++)
            if (outs[i].guest) watch_one(&outs[i], now, &ev[i]);
        plat_mutex_unlock(&lock);
        for (int i = 0; i < OUT_MAX; i++)
            if (ev[i]) kern_signal_system_event(ev[i]);
    }
}

static uint32_t open_out(Cpu *c, uint64_t guest, uint64_t event, const uint8_t *param) {
    int32_t rate = 0;
    uint16_t channels = 0;
    if (param) memcpy(&rate, param, 4), memcpy(&channels, param + 4, 2);
    if (rate && rate != RATE) rt_log("audout: the game asked for %d Hz; the output runs at %d Hz", rate, RATE);
    if (!channels) channels = 2;
    if (channels != 1 && channels != 2 && channels != 6) rt_fatal("audout: %u channels", channels);
    if (event) kern_bind_system_event(c, event, 1);
    plat_mutex_lock(&lock);
    Out *o = find(guest);
    if (!o) o = find(0);
    if (!o) rt_fatal("audout: more than %d AudioOuts open", OUT_MAX);
    if (sdl_ready < 0) {
        sdl_ready = audio_sdl_init();
        if (sdl_ready) list_devices();
        else rt_log("audout: SDL audio init failed (%s)", SDL_GetError());
    }
    if (o->guest && o->stream) {   // opened again without a close: the old stream would keep its device
        rt_log("audout: %#llx opened again without CloseAudioOut", (unsigned long long)guest);
        SDL_DestroyAudioStream(o->stream);
    }
    *o = (Out){.guest = guest, .event = event, .channels = channels, .state = STATE_STOPPED, .volume = 1.0f, .gain = 1.0f};
    if (sdl_ready) o->stream = open_stream(o);
    if (!watching) watching = plat_thread_spawn(watch_main, NULL, "audout watch", 0);
    plat_mutex_unlock(&lock);
    rt_log("audout: opened %#llx, %u channels at %d Hz, %s", (unsigned long long)guest, channels, RATE,
           o->stream ? "on the default device" : "without a device (timed)");
    return 0;
}

// Result OpenDefaultAudioOut(AudioOut *, const AudioOutParameter &)
static void ov_open_default(Cpu *c) { ret_x(c, open_out(c, arg_x(c, 0), 0, arg_p(c, 1))); }
// Result OpenDefaultAudioOut(AudioOut *, os::SystemEvent *, const AudioOutParameter &)
static void ov_open_default_event(Cpu *c) { ret_x(c, open_out(c, arg_x(c, 0), arg_x(c, 1), arg_p(c, 2))); }
// Result OpenAudioOut(AudioOut *, const char *name, const AudioOutParameter &): every name is the default device
static void ov_open_named(Cpu *c) { ret_x(c, open_out(c, arg_x(c, 0), 0, arg_p(c, 2))); }
// Result OpenAudioOut(AudioOut *, os::SystemEvent *, const char *name, const AudioOutParameter &)
static void ov_open_named_event(Cpu *c) { ret_x(c, open_out(c, arg_x(c, 0), arg_x(c, 1), arg_p(c, 3))); }

static Out *need(Cpu *c, const char *fn) {
    Out *o = find(arg_x(c, 0));
    if (!o || !arg_x(c, 0)) rt_fatal("nn::audio::%s(%#llx): not an open AudioOut", fn, (unsigned long long)arg_x(c, 0));
    return o;
}

// void CloseAudioOut(AudioOut *)
static void ov_close(Cpu *c) {
    plat_mutex_lock(&lock);
    Out *o = need(c, "CloseAudioOut");
    SDL_AudioStream *s = o->stream;
    rt_log("audout: closed %#llx after %llu bytes (%llu of %llu buffers with sound)", (unsigned long long)o->guest,
           (unsigned long long)o->appended, (unsigned long long)o->nonzero, (unsigned long long)o->appends);
    memset(o, 0, sizeof *o);
    plat_mutex_unlock(&lock);
    if (s) SDL_DestroyAudioStream(s);
}

// Result StartAudioOut(AudioOut *)
static void ov_start(Cpu *c) {
    plat_mutex_lock(&lock);
    Out *o = need(c, "StartAudioOut");
    if (o->state != STATE_STARTED) {
        o->state = STATE_STARTED;
        o->start_ns = now_ns();
        if (!o->started_ns) o->started_ns = o->start_ns;
        o->seen = consumed(o), o->moved_ns = o->start_ns;
        if (o->stream) SDL_ResumeAudioStreamDevice(o->stream);
    }
    plat_mutex_unlock(&lock);
    ret_x(c, 0);
}

// void StopAudioOut(AudioOut *)
static void ov_stop(Cpu *c) {
    plat_mutex_lock(&lock);
    Out *o = need(c, "StopAudioOut");
    if (o->state == STATE_STARTED) {
        o->consumed_at_stop = consumed(o);
        o->state = STATE_STOPPED;
        if (o->stream) SDL_PauseAudioStreamDevice(o->stream);
    }
    plat_mutex_unlock(&lock);
}

// bool AppendAudioOutBuffer(AudioOut *, AudioOutBuffer *)
static void ov_append(Cpu *c) {
    uint64_t buf = arg_x(c, 1);
    if (!buf) rt_fatal("nn::audio::AppendAudioOutBuffer: null buffer");
    const void *data = (const void *)(uintptr_t)LD64(buf + 8);
    uint64_t size = LD64(buf + 0x18);
    plat_mutex_lock(&lock);
    Out *o = need(c, "AppendAudioOutBuffer");
    int ok = o->nq < QUEUE_MAX;
    if (ok) {
        size -= size % frame_bytes(o);
        int peak = 0;   // for the report in watch_one
        for (const int16_t *p = data, *e = p + size / 2; p < e; p++) {
            int v = *p < 0 ? -*p : *p;
            if (v > peak) peak = v;
        }
        o->appends++;
        if (peak && !o->nonzero++)
            rt_log("audout: %#llx: the first buffer with sound, %.1f s after start (loudest sample %d)",
                   (unsigned long long)o->guest, o->started_ns ? (double)(now_ns() - o->started_ns) / 1e9 : 0.0, peak);
        if (peak > o->peak) o->peak = peak;
        if (o->stream && size) {
            float gain = o->volume * audio_player_volume();
            if (gain != o->gain && SDL_SetAudioStreamGain(o->stream, gain)) o->gain = gain;
            SDL_PutAudioStreamData(o->stream, data, (int)size);
        }
        o->appended += size;
        o->q[o->nq++] = (Queued){buf, o->appended};
    }
    plat_mutex_unlock(&lock);
    ret_x(c, (uint64_t)ok);
}

// AudioOutBuffer *GetReleasedAudioOutBuffer(AudioOut *): the oldest buffer the device has finished, else NULL
static void ov_released(Cpu *c) {
    uint64_t buf = 0;
    plat_mutex_lock(&lock);
    Out *o = need(c, "GetReleasedAudioOutBuffer");
    if (o->nq && consumed(o) >= o->q[0].end) {
        buf = o->q[0].buf;
        memmove(o->q, o->q + 1, (size_t)--o->nq * sizeof *o->q);
    }
    plat_mutex_unlock(&lock);
    ret_x(c, buf);
}

// bool ContainsAudioOutBuffer(const AudioOut *, const AudioOutBuffer *)
static void ov_contains(Cpu *c) {
    int found = 0;
    plat_mutex_lock_shared(&lock);
    Out *o = need(c, "ContainsAudioOutBuffer");
    for (int i = 0; i < o->nq && !found; i++) found = o->q[i].buf == arg_x(c, 1);
    plat_mutex_unlock_shared(&lock);
    ret_x(c, (uint64_t)found);
}

// bool FlushAudioOutBuffers(AudioOut *): drops the data not played yet; the buffers are released at once
static void ov_flush(Cpu *c) {
    plat_mutex_lock(&lock);
    Out *o = need(c, "FlushAudioOutBuffers");
    if (o->stream) SDL_ClearAudioStream(o->stream);
    o->consumed_at_stop = o->appended;
    o->start_ns = now_ns();
    for (int i = 0; i < o->nq; i++) o->q[i].end = 0;
    plat_mutex_unlock(&lock);
    ret_x(c, 1);
}

static void ov_state(Cpu *c) {
    plat_mutex_lock_shared(&lock);
    uint32_t s = need(c, "GetAudioOutState")->state;
    plat_mutex_unlock_shared(&lock);
    ret_x(c, s);
}
static void ov_channels(Cpu *c) {
    plat_mutex_lock_shared(&lock);
    uint32_t n = need(c, "GetAudioOutChannelCount")->channels;
    plat_mutex_unlock_shared(&lock);
    ret_x(c, n);
}
static void ov_rate(Cpu *c) { need(c, "GetAudioOutSampleRate"), ret_x(c, RATE); }
static void ov_format(Cpu *c) { need(c, "GetAudioOutSampleFormat"), ret_x(c, FMT_PCM16); }
static void ov_buffer_count(Cpu *c) {
    plat_mutex_lock_shared(&lock);
    int n = need(c, "GetAudioOutBufferCount")->nq;
    plat_mutex_unlock_shared(&lock);
    ret_x(c, (uint64_t)n);
}
static void ov_played(Cpu *c) {
    plat_mutex_lock_shared(&lock);
    Out *o = need(c, "GetAudioOutPlayedSampleCount");
    uint64_t n = consumed(o) / frame_bytes(o);
    plat_mutex_unlock_shared(&lock);
    ret_x(c, n);
}
static void ov_get_volume(Cpu *c) {
    plat_mutex_lock_shared(&lock);
    float v = need(c, "GetAudioOutVolume")->volume;
    plat_mutex_unlock_shared(&lock);
    ret_s(c, v);
}
static void ov_set_volume(Cpu *c) {
    plat_mutex_lock(&lock);
    need(c, "SetAudioOutVolume")->volume = arg_s(c, 0);
    plat_mutex_unlock(&lock);
}

// ---- the output device as the game sees it: one stereo TV output that never changes
#define DEVICE_NAME "AudioTvOutput"
// Result GetActiveAudioDeviceName(AudioDeviceName *out): 256 bytes
static void ov_device_name(Cpu *c) {
    char *out = arg_p(c, 0);
    if (out) memset(out, 0, 256), memcpy(out, DEVICE_NAME, sizeof DEVICE_NAME);
    ret_x(c, 0);
}
static void ov_device_channels(Cpu *c) { ret_x(c, 2); }
// Result AcquireAudioDeviceNotificationForOutput(os::SystemEvent *): an event that never fires
static void ov_device_notification(Cpu *c) {
    kern_bind_system_event(c, arg_x(c, 0), 1);
    ret_x(c, 0);
}

HOST_OVERRIDE("_ZN2nn5audio19OpenDefaultAudioOutEPNS0_8AudioOutERKNS0_17AudioOutParameterE", ov_open_default);
HOST_OVERRIDE("_ZN2nn5audio19OpenDefaultAudioOutEPNS0_8AudioOutEPNS_2os11SystemEventERKNS0_17AudioOutParameterE",
              ov_open_default_event);
HOST_OVERRIDE("_ZN2nn5audio12OpenAudioOutEPNS0_8AudioOutEPKcRKNS0_17AudioOutParameterE", ov_open_named);
HOST_OVERRIDE("_ZN2nn5audio12OpenAudioOutEPNS0_8AudioOutEPNS_2os11SystemEventEPKcRKNS0_17AudioOutParameterE",
              ov_open_named_event);
HOST_OVERRIDE("_ZN2nn5audio13CloseAudioOutEPNS0_8AudioOutE", ov_close);
HOST_OVERRIDE("_ZN2nn5audio13StartAudioOutEPNS0_8AudioOutE", ov_start);
HOST_OVERRIDE("_ZN2nn5audio12StopAudioOutEPNS0_8AudioOutE", ov_stop);
HOST_OVERRIDE("_ZN2nn5audio20AppendAudioOutBufferEPNS0_8AudioOutEPNS0_14AudioOutBufferE", ov_append);
HOST_OVERRIDE("_ZN2nn5audio25GetReleasedAudioOutBufferEPNS0_8AudioOutE", ov_released);
HOST_OVERRIDE("_ZN2nn5audio22ContainsAudioOutBufferEPKNS0_8AudioOutEPKNS0_14AudioOutBufferE", ov_contains);
HOST_OVERRIDE("_ZN2nn5audio20FlushAudioOutBuffersEPNS0_8AudioOutE", ov_flush);
HOST_OVERRIDE("_ZN2nn5audio16GetAudioOutStateEPKNS0_8AudioOutE", ov_state);
HOST_OVERRIDE("_ZN2nn5audio23GetAudioOutChannelCountEPKNS0_8AudioOutE", ov_channels);
HOST_OVERRIDE("_ZN2nn5audio21GetAudioOutSampleRateEPKNS0_8AudioOutE", ov_rate);
HOST_OVERRIDE("_ZN2nn5audio23GetAudioOutSampleFormatEPKNS0_8AudioOutE", ov_format);
HOST_OVERRIDE("_ZN2nn5audio22GetAudioOutBufferCountEPKNS0_8AudioOutE", ov_buffer_count);
HOST_OVERRIDE("_ZN2nn5audio28GetAudioOutPlayedSampleCountEPKNS0_8AudioOutE", ov_played);
HOST_OVERRIDE("_ZN2nn5audio17GetAudioOutVolumeEPKNS0_8AudioOutE", ov_get_volume);
HOST_OVERRIDE("_ZN2nn5audio17SetAudioOutVolumeEPNS0_8AudioOutEf", ov_set_volume);
HOST_OVERRIDE("_ZN2nn5audio24GetActiveAudioDeviceNameEPNS0_15AudioDeviceNameE", ov_device_name);
HOST_OVERRIDE("_ZN2nn5audio30GetActiveAudioOutputDeviceNameEPNS0_15AudioDeviceNameE", ov_device_name);
HOST_OVERRIDE("_ZN2nn5audio41GetActiveAudioDeviceChannelCountForOutputEv", ov_device_channels);
HOST_OVERRIDE("_ZN2nn5audio39AcquireAudioDeviceNotificationForOutputEPNS_2os11SystemEventE", ov_device_notification);
