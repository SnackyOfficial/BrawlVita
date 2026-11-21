/* FMOD's Android "Audio Track" output (FMOD_OUTPUTTYPE_AUDIOTRACK = 11 in this libfmod) on the Vita's audio out.
 *
 * FMOD drives a Java object org/fmod/AudioDevice through JNI (seen on the console 2026-11-11, fmod_output=11 run):
 *     <init>()V, init(IIII)Z, write([BI)V, close()V
 * FMOD's mixer thread calls write() with the next block of mixed PCM; on Android AudioTrack.write blocks until there is room, which paces the mixer.
 * Here write() collects the bytes into grains of BV_AT_GRAIN frames and hands each full grain to sceAudioOutOutput, which also blocks until the previous grain
 * is playing: the same pacing. Two grain buffers alternate (the hardware reads one while the next is filled).
 *
 * Assumed format: 16-bit signed little-endian, interleaved (AudioTrack's ENCODING_PCM_16BIT). init's four ints are logged; the channel count and the sample rate
 * are taken as the first two (or swapped when the first one looks like a rate). Channels beyond 2 are dropped (front left / right kept), mono plays on a mono port.
 * NOT VERIFIED ON HARDWARE at the time of writing.
 */
#include "audiotrack.h"

#include <stdint.h>
#include <string.h>

#include "../loader/log.h"

#ifdef BV_HOST_TEST
/* the host test provides these */
int bv_test_audio_open(int grain, int freq, int mono);
int bv_test_audio_output(int port, const void *buf);
void bv_test_audio_release(int port);
#define AO_OPEN(g, f, m) bv_test_audio_open(g, f, m)
#define AO_OUTPUT(p, b) bv_test_audio_output(p, b)
#define AO_RELEASE(p) bv_test_audio_release(p)
#define AO_VOLUME(p) ((void)0)
#else
#include <psp2/audioout.h>
static int ao_open(int grain, int freq, int mono) {
    return sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM, grain, freq, mono ? SCE_AUDIO_OUT_MODE_MONO : SCE_AUDIO_OUT_MODE_STEREO);
}
static void ao_volume(int port) {
    int vol[2] = {SCE_AUDIO_VOLUME_0DB, SCE_AUDIO_VOLUME_0DB};
    sceAudioOutSetVolume(port, (SceAudioOutChannelFlag)(SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH), vol);
}
#define AO_OPEN(g, f, m) ao_open(g, f, m)
#define AO_OUTPUT(p, b) sceAudioOutOutput(p, b)
#define AO_RELEASE(p) sceAudioOutReleasePort(p)
#define AO_VOLUME(p) ao_volume(p)
#include <psp2/kernel/threadmgr.h>
#endif

/* update 184 (user: audio very choppy at low fps): FMOD's mixer thread fed sceAudioOutOutput directly with two 10.7 ms grains, so any delay of the
 * mixer thread (the game threads take the CPU when the frame rate drops) was a gap. audio_ring=1 (default): a ring of BV_AT_RING grains (85 ms)
 * played by our own high-priority output thread (silence on an underrun, then 3 grains of cushion before it plays again), and the mixer thread's
 * priority is raised on its first write. audio_ring=0 = the old direct output. */
int bv_audio_ring = 1;
#define BV_AT_RING 8
static int g_port = -1;
#ifndef BV_HOST_TEST
static int16_t g_ring[BV_AT_RING][BV_AT_GRAIN * 2];
static int16_t g_silence[BV_AT_GRAIN * 2];
static volatile uint32_t g_wr, g_rd;   /* grains queued / played (one writer, one reader) */
static volatile int g_thr_run;
static SceUID g_thr = -1;
static volatile uint32_t g_underruns;
static int ring_thread(SceSize args, void *argp) {
    (void)args; (void)argp;
    int primed = 0;
    while (g_thr_run) {
        uint32_t avail = g_wr - g_rd;
        if (!primed && avail >= 3) primed = 1;
        if (primed && avail > 0) {
            sceAudioOutOutput(g_port, g_ring[g_rd % BV_AT_RING]);
            __sync_synchronize();
            g_rd++;
        } else {
            if (primed) {
                primed = 0;
                uint32_t u = ++g_underruns;
                if (u <= 5 || !(u & (u - 1))) bv_log("AudioDevice: underrun #%u (the mixer was late)", (unsigned)u);
            }
            sceAudioOutOutput(g_port, g_silence);   /* keeps the hardware clock running */
        }
    }
    return 0;
}
static void ring_start(void) {
    g_wr = g_rd = 0;
    g_underruns = 0;
    g_thr_run = 1;
    g_thr = sceKernelCreateThread("bv_audio_out", ring_thread, 64, 0x4000, 0, 0, NULL);
    if (g_thr >= 0 && sceKernelStartThread(g_thr, 0, NULL) >= 0) {
        bv_log("AudioDevice: output thread started (ring of %d grains, priority 64)", BV_AT_RING);
        return;
    }
    bv_log("AudioDevice: output thread could not start (0x%08x): direct output", (unsigned)g_thr);
    if (g_thr >= 0) sceKernelDeleteThread(g_thr);
    g_thr = -1;
    g_thr_run = 0;
}
static void ring_stop(void) {
    if (g_thr < 0) return;
    g_thr_run = 0;
    SceUInt to = 500000;
    sceKernelWaitThreadEnd(g_thr, NULL, &to);
    sceKernelDeleteThread(g_thr);
    g_thr = -1;
    bv_log("AudioDevice: output thread stopped (%u underruns)", (unsigned)g_underruns);
}
#endif

static int g_in_ch = 2;  /* channels in FMOD's data */
static int g_out_ch = 2; /* channels of the port (1 or 2) */
static int g_rate;
static int16_t g_buf[2][BV_AT_GRAIN * 2];
static int g_cur;        /* buffer being filled */
static int g_fill;       /* frames in it */
static int16_t g_part[8]; /* a frame cut between two write() calls (bytes kept in g_partb) */
static int g_partb;
static uint32_t g_grains, g_writes, g_bytes;
static int g_peak;

static int rate_ok(int r) {
    static const int ok[] = {8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000};
    for (unsigned i = 0; i < sizeof ok / sizeof ok[0]; i++)
        if (ok[i] == r) return 1;
    return 0;
}

int bv_audiotrack_init(int a0, int a1, int a2, int a3) {
    int ch = a0, rate = a1;
    if (a0 >= 4000 && a1 > 0 && a1 <= 8) { ch = a1; rate = a0; }
    bv_log("AudioDevice.init(%d, %d, %d, %d): %d channel(s), %d Hz, 16-bit PCM assumed", a0, a1, a2, a3, ch, rate);
    if (ch < 1 || ch > 8 || !rate_ok(rate)) {
        bv_log("AudioDevice.init: channel count or rate not supported by the Vita audio out -> false");
        return 0;
    }
    if (g_port >= 0) bv_audiotrack_close();
    g_in_ch = ch;
    g_out_ch = ch == 1 ? 1 : 2;
    g_rate = rate;
    g_cur = g_fill = g_partb = 0;
    g_grains = g_writes = g_bytes = 0;
    g_peak = 0;
    int port = AO_OPEN(BV_AT_GRAIN, rate, g_out_ch == 1);
    if (port < 0) {
        bv_log("AudioDevice.init: sceAudioOutOpenPort(BGM, %d, %d, %s) failed 0x%08x -> false", BV_AT_GRAIN, rate, g_out_ch == 1 ? "mono" : "stereo", (unsigned)port);
        return 0;
    }
    AO_VOLUME(port);
    g_port = port;
    bv_log("AudioDevice.init: audio out port %d open (BGM, %d frames per grain, %d Hz, %s) -> true", port, BV_AT_GRAIN, rate, g_out_ch == 1 ? "mono" : "stereo");
#ifndef BV_HOST_TEST
    if (bv_audio_ring) ring_start();
#endif
    return 1;
}

static int16_t *grain_buf(void) {
#ifndef BV_HOST_TEST
    if (g_thr >= 0) return g_ring[g_wr % BV_AT_RING];
#endif
    return g_buf[g_cur];
}
static void grain_done(void) {
#ifndef BV_HOST_TEST
    if (g_thr >= 0) {
        __sync_synchronize();
        g_wr++;
        while (g_thr_run && g_wr - g_rd >= BV_AT_RING) sceKernelDelayThread(2000);   /* full: wait like a blocking AudioTrack.write (paces FMOD) */
        return;
    }
#endif
    AO_OUTPUT(g_port, g_buf[g_cur]);
    g_cur ^= 1;
}

static void put_frame(const int16_t *f) {
    int16_t *d = &grain_buf()[g_fill * g_out_ch];
    d[0] = f[0];
    if (g_out_ch == 2) d[1] = g_in_ch >= 2 ? f[1] : f[0];
    for (int c = 0; c < (g_in_ch < 2 ? g_in_ch : 2); c++) {
        int v = f[c] < 0 ? -f[c] : f[c];
        if (v > g_peak) g_peak = v;
    }
    if (++g_fill == BV_AT_GRAIN) {
        static int heard;
        if (!heard && g_peak > 0) {
            heard = 1;
            bv_log("AudioDevice: first non-silent grain (#%u, peak %d)", (unsigned)g_grains + 1, g_peak);
        }
        grain_done();   /* update 184: into the ring (or straight out with audio_ring=0) */
        g_fill = 0;
        g_grains++;
        /* a few lines early on, then one per ~5 minutes at 48 kHz / 512: enough to see that sound flows, not enough to flood the log */
        if (g_grains == 1 || g_grains == 100 || g_grains % 28125 == 0)
            bv_log("AudioDevice: %u grains played (%u writes, %u bytes so far, peak %d of 32767)", (unsigned)g_grains, (unsigned)g_writes, (unsigned)g_bytes, g_peak);
    }
}

void bv_audiotrack_write(const uint8_t *data, int len) {
    if (g_port < 0 || !data || len <= 0) return;
#ifndef BV_HOST_TEST
    if (bv_audio_ring && !g_writes) {   /* update 184: FMOD's mixer thread mixes ahead of the game threads */
        int r = sceKernelChangeThreadPriority(0, 70);
        bv_log("AudioDevice: mixer thread priority -> 70 (0x%08x)", (unsigned)r);
    }
#endif
    g_writes++;
    g_bytes += (uint32_t)len;
    const int fb = g_in_ch * 2; /* bytes per frame */
    int i = 0;
    if (g_partb) { /* finish the frame cut off by the last call */
        while (g_partb < fb && i < len) ((uint8_t *)g_part)[g_partb++] = data[i++];
        if (g_partb < fb) return;
        put_frame(g_part);
        g_partb = 0;
    }
    for (; i + fb <= len; i += fb) {
        int16_t f[8];
        memcpy(f, data + i, (size_t)fb);
        put_frame(f);
    }
    while (i < len) ((uint8_t *)g_part)[g_partb++] = data[i++];
}

void bv_audiotrack_close(void) {
    if (g_port < 0) return;
    bv_log("AudioDevice.close: port %d released after %u grains (%u writes, %u bytes, peak %d)", g_port, (unsigned)g_grains, (unsigned)g_writes, (unsigned)g_bytes, g_peak);
#ifndef BV_HOST_TEST
    ring_stop();
#endif
    AO_RELEASE(g_port);
    g_port = -1;
}

int bv_audiotrack_port(void) { return g_port; }
