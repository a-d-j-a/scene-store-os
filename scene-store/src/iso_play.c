/*
 * iso_play.c — the first-party audio player demo guest app.
 *
 * Started by the host with SCENE_STORE_PORT set; connects back over TCP
 * on 127.0.0.1, builds a window through the full scene_app stack
 * (titlebar + close + content + a status LABEL) and plays audio:
 *
 *   argv[1] = a WAV file path (RIFF/WAVE, PCM 16-bit LE, 1-2 channels,
 *             sample rates 8k-48k) or "SYN" — the built-in synthesized
 *             melody demo (a triangle-ish 440Hz/554Hz tune with per-note
 *             envelopes) — or absent (same as SYN). argv[2] = event log
 *             path (tests) or nothing (stderr).
 *
 * Playback on the ISO goes through the kernel's raw PCM UAPI via
 * src/alsa_uapi.h (self-contained, mirroring the DRM strategy in
 * iso_drm.c — the musl sysroot ships no ALSA headers): open
 * /dev/snd/pcmC0D0p, negotiate HW_PARAMS (RW_INTERLEAVED S16_LE, the
 * file's rate/channels, 4 periods of period_size frames), SW_PARAMS
 * (start_threshold=1, stop_threshold=boundary=INT_MAX — the tinyalsa
 * recipe), PREPARE, then blocking write() bursts of one period per main
 * loop pass (ALSA's own kernel buffer absorbs the pacing; the app loop
 * keeps pumping the scene transport around each burst). FAILING opens
 * degrade gracefully: status shows "no audio device", the UI stays
 * alive, and the app exits 0 when the window is closed.
 *
 * On Windows the ALSA parts are compiled OUT (#ifndef _WIN32): a
 * wall-clock null sink still runs the whole WAV parse and drains the
 * generated samples at the real sample rate, so the UI flow (p -> done,
 * close button) is fully exercised without sound hardware.
 *
 * `iso_play --selftest` (before connecting) proves the audio logic on
 * Windows: generate the SYN melody, write it to build/test_tone.wav
 * with its own RIFF writer, parse it back with its own WAV parser and
 * assert sample count / rate / channel / PCM16 plus exact sample values
 * (taken from the same generator — self-consistent, no guessed colors).
 */
#define _POSIX_C_SOURCE 200809L
#include "scene_app.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <math.h>

#if defined(_WIN32)
#include <windows.h>
static void msleep(unsigned m) { Sleep(m); }
#else
#include <time.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include "alsa_uapi.h"
static void msleep(unsigned m)
{
    struct timespec ts = { (time_t)(m / 1000), (long)(m % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}
#endif

/* ---- layout ------------------------------------------------------------- */
/* window (100,50,240,160): titlebar 32px, content (100,82,240,128); the
 * status LABEL (id = app base+5) sits at (104,90,232,16) in the content. */
#define PLY_X 100
#define PLY_Y 50
#define PLY_W 240
#define PLY_H 160
#define STATUS_NODE 40005u

/* Playback states; each maps to one status text. */
#define ST_PLAY       1   /* "p"               */
#define ST_DONE       2   /* "done"            */
#define ST_NO_DEVICE  3   /* "no audio device" */
#define ST_BAD_FILE   4   /* "bad file"        */

static scene_app *g_app;
static FILE      *g_log;

/* ---- track buffer (owned by the app, played from) ------------------------ */
static int16_t  *g_samples;      /* samples, interleaved */
static uint32_t  g_frames;       /* frame count */
static uint16_t  g_channels;     /* 1 or 2 */
static uint32_t  g_rate;         /* 8000..48000 */
static uint32_t  g_pos;          /* consumed frames */
static int        g_state;       /* ST_* */
static int        g_status_dirty;

static void dlog(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    if (g_log) vfprintf(g_log, fmt, ap);
    else vfprintf(stderr, fmt, ap);
    va_end(ap);
    if (g_log) fflush(g_log);
    else fflush(stderr);
}

/* ---- tiny endian helpers -------------------------------------------------- */

static void w_u16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void w_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static uint16_t r_u16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}
static uint32_t r_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* ---- WAV ---------------------------------------------------------------- */

struct wav_file {
    uint16_t     channels;
    uint32_t     rate;
    uint32_t     frames;
    const int16_t *data;     /* into the caller's buffer */
};

/* Validate + decode a RIFF/WAVE in memory. Returns 0 and fills *out, or
 * -1 with *err set. Accepts exactly: fmt chunk, PCM (format 1) 16-bit,
 * 1-2 channels, 8000-48000 Hz. Anything else is rejected. */
static int wav_parse(const uint8_t *buf, size_t len,
                     struct wav_file *out, const char **err)
{
    uint32_t off;

    if (len < 12 || memcmp(buf, "RIFF", 4) != 0 ||
        memcmp(buf + 8, "WAVE", 4) != 0) {
        *err = "not a RIFF/WAVE file";
        return -1;
    }
    out->channels = 0;
    out->rate = 0;
    off = 12;
    while (off + 8 <= len) {
        const uint8_t *id = buf + off;
        uint32_t csz = r_u32(buf + off + 4);
        off += 8;
        if (csz > len - off) csz = (uint32_t)(len - off);
        if (memcmp(id, "fmt ", 4) == 0 && out->channels == 0) {
            uint16_t afmt, ch, block, bits;
            uint32_t rate;
            if (csz < 16) { *err = "fmt chunk too small"; return -1; }
            afmt  = r_u16(buf + off);
            ch    = r_u16(buf + off + 2);
            rate  = r_u32(buf + off + 4);
            block = r_u16(buf + off + 12);
            bits  = r_u16(buf + off + 14);
            if (afmt != 1)      { *err = "not PCM"; return -1; }
            if (ch < 1 || ch > 2) { *err = "channels not 1-2"; return -1; }
            if (rate < 8000 || rate > 48000) {
                *err = "sample rate not 8k-48k"; return -1;
            }
            if (bits != 16)     { *err = "not 16-bit"; return -1; }
            if (block != ch * 2u) { *err = "bad block align"; return -1; }
            out->channels = ch;
            out->rate = rate;
        } else if (memcmp(id, "data", 4) == 0) {
            /* first data chunk wins; allow it to be at the end of a
             * truncated file (csz was clamped above) */
            size_t frames = csz / (size_t)(out->channels ? out->channels * 2u : 2u);
            if (out->channels == 0 || frames == 0) {
                *err = "no fmt before data"; return -1;
            }
            out->frames = (uint32_t)frames;
            out->data = (const int16_t *)(buf + off);
            return 0;
        }
        off += csz + (csz & 1u);       /* chunks are word-aligned */
    }
    *err = "no data chunk";
    return -1;
}

/* Write a minimal RIFF/WAVE (PCM 16-bit) from memory. Own writer — the
 * parser above is the reader; used by the selftest round-trip. */
static int wav_write(const char *path, uint16_t ch, uint32_t rate,
                     const int16_t *samples, uint32_t frames)
{
    uint32_t dsz = frames * ch * 2u;
    uint8_t *img = (uint8_t *)malloc((size_t)dsz + 44);
    FILE *f;
    int rc = -1;

    if (!img) return -1;
    memcpy(img, "RIFF", 4);
    w_u32(img + 4, 36u + dsz);
    memcpy(img + 8, "WAVE", 4);
    memcpy(img + 12, "fmt ", 4);
    w_u32(img + 16, 16u);
    w_u16(img + 20, 1u);                     /* PCM */
    w_u16(img + 22, ch);
    w_u32(img + 24, rate);
    w_u32(img + 28, rate * ch * 2u);         /* byte rate */
    w_u16(img + 32, (uint16_t)(ch * 2u));    /* block align */
    w_u16(img + 34, 16u);                    /* bits */
    memcpy(img + 36, "data", 4);
    w_u32(img + 40, dsz);
    memcpy(img + 44, samples, dsz);
    f = fopen(path, "wb");
    if (f) {
        rc = fwrite(img, 1, (size_t)dsz + 44, f) == (size_t)dsz + 44 ? 0 : -1;
        fclose(f);
    }
    free(img);
    return rc;
}

/* Load a WAV file into the track buffer. */
static int track_load_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    uint8_t *buf;
    long sz;
    struct wav_file wf;
    const char *err = NULL;
    size_t bytes;

    if (!f) { dlog("iso-play: can't open %s\n", path); return -1; }
    if (fseek(f, 0, SEEK_END) != 0 || (sz = ftell(f)) < 44 ||
        fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        dlog("iso-play: bad file size\n");
        return -1;
    }
    buf = (uint8_t *)malloc((size_t)sz);
    if (!buf) { fclose(f); return -1; }
    bytes = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (bytes != (size_t)sz || wav_parse(buf, bytes, &wf, &err) != 0) {
        dlog("iso-play: %s: %s\n", path, err ? err : "read error");
        free(buf);
        return -1;
    }
    g_samples = (int16_t *)malloc((size_t)wf.frames * wf.channels * 2u);
    if (!g_samples) { free(buf); return -1; }
    memcpy(g_samples, wf.data, (size_t)wf.frames * wf.channels * 2u);
    g_frames = wf.frames;
    g_rate = wf.rate;
    g_channels = wf.channels;
    free(buf);
    dlog("iso-play: wav %s: %u frames x%u @%u\n",
         path, (unsigned)g_frames, (unsigned)g_channels, (unsigned)g_rate);
    return 0;
}

/* ---- SYN: the built-in 440/554Hz melody ---------------------------------- */
/* 20 eighth notes of 0.2 s at 8 kHz mono (4.0 s total): A A A A C# C# C#
 * C# A A A A C# C# C# C# A C# A C#. Triangle-ish wave (0.75 triangle +
 * 0.25 second harmonic) with a linear attack and release envelope per
 * note. Pure function of the sample index — deterministic everywhere.  */
#define SYN_RATE    8000u
#define SYN_EIGHTH  (SYN_RATE / 5u)          /* 0.2 s = 1600 frames */
#define SYN_ATTACK  40u
#define SYN_RELEASE 320u
#define SYN_PEAK    12000

static const uint16_t syn_notes[20] = {
    440, 440, 440, 440, 554, 554, 554, 554,
    440, 440, 440, 440, 554, 554, 554, 554,
    440, 554, 440, 554
};

static uint32_t syn_frames(void) { return SYN_EIGHTH * 20u; }

static void gen_syn(int16_t *out)
{
    uint32_t total = syn_frames();
    uint32_t f;

    for (f = 0; f < total; f++) {
        uint32_t note_i = f / SYN_EIGHTH;
        uint32_t t = f - note_i * SYN_EIGHTH;
        uint32_t freq = syn_notes[note_i];
        double ph = (double)(freq * t) / (double)SYN_RATE;
        double frac = ph - (double)(int64_t)ph;    /* in [0,1) */
        double tri = 1.0 - 4.0 * fabs(frac - 0.5);
        double harm = sin(4.0 * 3.14159265358979323846 * frac);
        double env;
        double v;

        if (t < SYN_ATTACK) env = (double)t / (double)SYN_ATTACK;
        else if (t > SYN_EIGHTH - SYN_RELEASE)
            env = (double)(SYN_EIGHTH - t) / (double)SYN_RELEASE;
        else env = 1.0;
        v = (0.75 * tri + 0.25 * harm) * env * (double)SYN_PEAK;
        if (v > 32767.0) v = 32767.0;
        if (v < -32767.0) v = -32767.0;
        out[f] = (int16_t)v;
    }
}

static int track_load_syn(void)
{
    uint32_t total = syn_frames();

    g_samples = (int16_t *)malloc((size_t)total * 2u);
    if (!g_samples) return -1;
    gen_syn(g_samples);
    g_frames = total;
    g_rate = SYN_RATE;
    g_channels = 1;
    dlog("iso-play: syn: %u frames mono @%u\n",
         (unsigned)g_frames, (unsigned)g_rate);
    return 0;
}

/* ---- ALSA raw PCM (POSIX only) — see alsa_uapi.h for the UAPI notes ---- */

#if !defined(_WIN32)
static int    g_afd = -1;
static uint32_t g_chunk;        /* period size in frames (from HW_PARAMS) */
static uint32_t g_periods;

static int alsa_setup(void)
{
    static const uint32_t psz_lad[] = {512, 1024, 2048, 4096, 256, 128, 8192};
    static const uint32_t pns_lad[] = {4, 2, 8};
    snd_pcm_sw_params sw;
    uint32_t i, j;
    int fd;

    fd = open("/dev/snd/pcmC0D0p", O_WRONLY);
    if (fd < 0) {
        dlog("iso-play: open /dev/snd/pcmC0D0p: %s\n", strerror(errno));
        return -1;
    }

    /* HW_REFINE with the open ("any") constraint set pulls the card's
     * ranges into the struct (the snd_pcm_hw_params_any() contract). */
    {
        snd_pcm_hw_params hp;
        scene_al_hw_any(&hp);
        if (ioctl(fd, SNDRV_PCM_IOCTL_HW_REFINE, &hp) < 0) {
            dlog("iso-play: hw_refine: %s\n", strerror(errno));
            close(fd);
            return -1;
        }
    }

    /* Ladder: exact ACCESS/FORMAT/CHANNELS/RATE plus a period size and
     * period count that the card accepts (EINVAL = constraint conflict,
     * try the next step; every PCM device accepts one of these). */
    for (i = 0; i < sizeof(psz_lad) / sizeof(psz_lad[0]); i++) {
        for (j = 0; j < sizeof(pns_lad) / sizeof(pns_lad[0]); j++) {
            snd_pcm_hw_params hp;

            scene_al_hw_any(&hp);
            scene_al_mask_clear_all(&hp.masks[SCENE_AL_MASK_IDX(SNDRV_PCM_HW_PARAM_ACCESS)]);
            scene_al_mask_set(&hp.masks[SCENE_AL_MASK_IDX(SNDRV_PCM_HW_PARAM_ACCESS)],
                              SNDRV_PCM_ACCESS_RW_INTERLEAVED);
            scene_al_mask_clear_all(&hp.masks[SCENE_AL_MASK_IDX(SNDRV_PCM_HW_PARAM_FORMAT)]);
            scene_al_mask_set(&hp.masks[SCENE_AL_MASK_IDX(SNDRV_PCM_HW_PARAM_FORMAT)],
                              SNDRV_PCM_FORMAT_S16_LE);
            scene_al_iv_exact(&hp.intervals[SCENE_AL_INTERVAL_IDX(SNDRV_PCM_HW_PARAM_CHANNELS)],
                              g_channels);
            scene_al_iv_exact(&hp.intervals[SCENE_AL_INTERVAL_IDX(SNDRV_PCM_HW_PARAM_RATE)],
                              g_rate);
            scene_al_iv_exact(&hp.intervals[SCENE_AL_INTERVAL_IDX(SNDRV_PCM_HW_PARAM_PERIOD_SIZE)],
                              psz_lad[i]);
            scene_al_iv_exact(&hp.intervals[SCENE_AL_INTERVAL_IDX(SNDRV_PCM_HW_PARAM_PERIODS)],
                              pns_lad[j]);
            if (ioctl(fd, SNDRV_PCM_IOCTL_HW_PARAMS, &hp) == 0) {
                /* read back the negotiated period size (the kernel
                 * resolves exact intervals to min==max) */
                snd_interval *iv =
                    &hp.intervals[SCENE_AL_INTERVAL_IDX(SNDRV_PCM_HW_PARAM_PERIOD_SIZE)];
                g_chunk = iv->min;
                g_periods = pns_lad[j];
                dlog("iso-play: hw ok: chunk=%u periods=%u\n",
                     (unsigned)g_chunk, (unsigned)g_periods);
                break;
            }
        }
        if (g_chunk) break;
    }
    if (g_chunk == 0) {
        dlog("iso-play: hw_params: no acceptible period geometry\n");
        close(fd);
        return -1;
    }

    /* SW_PARAMS: the tinyalsa recipe (heavily exercised in production):
     * start as soon as any data lands, never auto-stop, wrap at 2^31-1.
     * boundary must be all-ones (2^n - 1) — 0x7fffffff qualifies. */
    memset(&sw, 0, sizeof(sw));
    sw.tstamp_mode = 0;
    sw.period_step = 1;
    sw.sleep_min = 0;
    sw.avail_min = g_chunk;
    sw.xfer_align = g_chunk;                 /* obsolete, ignored by 6.6 */
    sw.start_threshold = 1;
    sw.stop_threshold = 0x7FFFFFFFu;
    sw.silence_threshold = 0;
    sw.silence_size = 0;
    sw.boundary = 0x7FFFFFFFu;
    sw.proto = SNDRV_PCM_VERSION;
    sw.tstamp_type = 0;
    if (ioctl(fd, SNDRV_PCM_IOCTL_SW_PARAMS, &sw) < 0) {
        dlog("iso-play: sw_params: %s\n", strerror(errno));
        close(fd);
        return -1;
    }
    if (ioctl(fd, SNDRV_PCM_IOCTL_PREPARE) < 0) {
        dlog("iso-play: prepare: %s\n", strerror(errno));
        close(fd);
        return -1;
    }
    g_afd = fd;
    return 0;
}

/* Write one period burst (or the remainder). Returns 1 when the whole
 * track is queued, 0 to keep playing, -1 on an unrecoverable error.
 * Blocking by design: ALSA's kernel buffer (4 periods) paces the app.
 *
 * Start semantics (verified in sound/core/pcm_native.c v6.6): with
 * start_threshold=1 the first write auto-starts the stream, so the
 * explicit START after each burst normally hits a RUNNING stream and
 * returns -EBADFD (snd_pcm_pre_start only accepts PREPARED); we ignore
 * that. After an xrun re-PREPARE the state is PREPARED again and the
 * next burst both auto-starts and (if the card ever refuses to) gets
 * the explicit START it needs. */
static int alsa_write_pass(void)
{
    uint32_t left, n;
    size_t bytes, done;
    const uint8_t *p;

    if (g_afd < 0) return -1;
    if (g_pos >= g_frames) return 1;
    left = g_frames - g_pos;
    n = left < g_chunk ? left : g_chunk;
    bytes = (size_t)n * g_channels * 2u;
    p = (const uint8_t *)(g_samples + (size_t)g_pos * g_channels);
    done = 0;
    while (done < bytes) {
        ssize_t w = write(g_afd, p + done, bytes - done);
        if (w < 0) {
            if (errno == EINTR) continue;
            if (errno == EPIPE) {
                /* underrun: the kernel dropped us; re-prepare and
                 * resync — the next burst restarts the stream */
                dlog("iso-play: xrun, re-prepare\n");
                ioctl(g_afd, SNDRV_PCM_IOCTL_PREPARE);
                continue;
            }
            dlog("iso-play: write: %s\n", strerror(errno));
            return -1;
        }
        done += (size_t)w;
    }
    g_pos += n;
    if (ioctl(g_afd, SNDRV_PCM_IOCTL_START) < 0 && errno != EBADFD)
        dlog("iso-play: start: %s\n", strerror(errno));
    if (g_pos >= g_frames) {
        /* play the queued tail out, then stop (DROP would cut it) */
        if (ioctl(g_afd, SNDRV_PCM_IOCTL_DRAIN) < 0)
            dlog("iso-play: drain: %s\n", strerror(errno));
        close(g_afd);
        g_afd = -1;
        return 1;
    }
    return 0;
}
#endif /* !_WIN32 */

#if defined(_WIN32)
/* Wall-clock null sink: consume at the real sample rate, so the UI
 * flow (p -> done) matches the ISO cadence without sound hardware. */
static uint64_t g_t0;

static void win_start(void)
{
    g_t0 = GetTickCount64();
}

static void win_drain_step(void)
{
    uint64_t now = GetTickCount64();
    uint64_t consumed = (now - g_t0) * g_rate / 1000u;
    g_pos = consumed >= g_frames ? g_frames : (uint32_t)consumed;
}
#endif

/* ---- status label -------------------------------------------------------- */

static const char *state_text(int st)
{
    switch (st) {
    case ST_PLAY:       return "p";
    case ST_DONE:       return "done";
    case ST_NO_DEVICE:  return "no audio device";
    case ST_BAD_FILE:   return "bad file";
    default:            return "";
    }
}

static void set_state(int st)
{
    if (g_state == st) return;
    g_state = st;
    g_status_dirty = 1;
    dlog("iso-play: status=%s\n", state_text(st));
}

static void push_status(void)
{
    if (!g_status_dirty) return;
    g_status_dirty = 0;
    scene_app_set_text(g_app, STATUS_NODE, 0, state_text(g_state));
    scene_app_present(g_app);
    scene_app_flush(g_app);
}

/* ---- input --------------------------------------------------------------- */

static void on_pointer(void *ud, uint64_t seq, int32_t x, int32_t y,
                       uint8_t buttons)
{
    (void)ud;
    dlog("iso-play: pointer %d,%d buttons=%u\n", (int)x, (int)y, buttons);
    scene_app_ack(g_app, seq);
}

static void on_activate(void *ud, uint64_t seq, scene_node_id id)
{
    (void)ud;
    dlog("iso-play: activate id=%u\n", (unsigned)id);
    if (id == STATUS_NODE - 2) {            /* close button = base+3 */
        dlog("iso-play: close clicked, exiting\n");
        /* flush delivers the DESTROY op; then exit(0) closes the socket
         * and the host reaps the session. NOTE: no scene_app_pump here —
         * pump inside an input callback re-enters scene_client_pump while
         * the INPUT_ACTIVATE record is still in flight (in_off advances
         * only after dispatch returns), re-dispatching the same record
         * forever (stack overflow; seen 0xC00000FD under the test suite
         * and proven under gdb, iso_play on_activate -> pump -> dispatch
         * -> on_activate -> ...). */
        scene_app_destroy_window(g_app, STATUS_NODE - 1);
        scene_app_flush(g_app);
        exit(0);
    }
    scene_app_ack(g_app, seq);
}

static void on_key(void *ud, uint64_t seq, uint32_t key_code,
                   uint8_t state, uint8_t modifiers)
{
    (void)ud;
    dlog("iso-play: key %u state=%u mods=%u\n", key_code, state, modifiers);
    scene_app_ack(g_app, seq);
}

/* ---- selftest (Windows-verifiable audio logic) --------------------------- */

#define ST_ASSERT(cond) do {                                                \
    if (!(cond)) {                                                          \
        printf("SELFTEST FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
        fflush(stdout);                                                     \
        return 1;                                                           \
    }                                                                       \
} while (0)

static int run_selftest(void)
{
    static const char *path = "build/test_tone.wav";
    uint32_t total = syn_frames();
    int16_t *buf = (int16_t *)malloc((size_t)total * 2u);
    FILE *f;
    uint8_t *raw;
    long sz;
    struct wav_file wf;
    const char *err = NULL;
    static const uint32_t probes[] = {0, 50, 999, 1000, 1601, 20000, 31999};
    size_t pi;

    printf("iso_play --selftest\n");
    fflush(stdout);
    ST_ASSERT(buf != NULL);
    gen_syn(buf);
    ST_ASSERT(wav_write(path, 1, SYN_RATE, buf, total) == 0);

    f = fopen(path, "rb");
    ST_ASSERT(f != NULL);
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    ST_ASSERT(sz == (long)total * 2L + 44L);
    raw = (uint8_t *)malloc((size_t)sz);
    ST_ASSERT(raw != NULL);
    ST_ASSERT(fread(raw, 1, (size_t)sz, f) == (size_t)sz);
    fclose(f);

    ST_ASSERT(wav_parse(raw, (size_t)sz, &wf, &err) == 0);
    ST_ASSERT(wf.channels == 1);
    ST_ASSERT(wf.rate == SYN_RATE);
    ST_ASSERT(wf.frames == total);
    for (pi = 0; pi < sizeof(probes) / sizeof(probes[0]); pi++) {
        uint32_t k = probes[pi];
        ST_ASSERT(k < wf.frames);
        ST_ASSERT(wf.data[k] == buf[k]);
    }
    /* full scale inside the first note: sample 200 is the triangle's
     * exact negative peak (440*200/8000 = 11.0 whole cycles -> frac 0,
     * harm sin(0) = 0) = -0.75*SYN_PEAK = -9000, exactly, in every libm.
     * The original probe pair (200, 2000) collided on one phase — 1800
     * samples is exactly 99 cycles of 440 Hz — and compared -9000 with
     * -9000; (2000 -> 2100) is the next note (554 Hz, t=500, frac 0.625)
     * where the triangle sits at +0.5 full scale (~+7500), always > 0. */
    ST_ASSERT(buf[200] == -9000);
    ST_ASSERT(buf[200] < buf[2100]);
    /* attack ramp not silent: t=80 past the 40-frame attack, frac 0.4,
     * v = (0.75*0.6 + 0.25*sin(1.6*pi))*SYN_PEAK >= 0.2*SYN_PEAK > 0 */
    ST_ASSERT(buf[SYN_ATTACK * 2] != 0);

    free(raw);
    free(buf);
    printf("SELFTEST OK\n");
    fflush(stdout);
    return 0;
}

/* ---- main ---------------------------------------------------------------- */

static void usage(void)
{
    printf("usage: iso_play [--selftest | WAVFILE | SYN]\n");
    printf("       SYN (default): built-in 440/554Hz melody demo\n");
    fflush(stdout);
}

int main(int argc, char **argv)
{
    const char *port;
    const char *src = "SYN";
    char title[32];
    scene_node_id content;
    scene_client *cl;
    int i;

    if (argc > 1) {
        if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
            usage();
            return 0;
        }
        if (strcmp(argv[1], "--selftest") == 0)
            return run_selftest();
        src = argv[1];
    }
    if (argc > 2) g_log = fopen(argv[2], "w");

    port = getenv("SCENE_STORE_PORT");
    if (!port) return 2;
    dlog("iso-play: start port=%s src=%s\n", port, src);

    if (strcmp(src, "SYN") == 0) {
        snprintf(title, sizeof(title), "iso-play");
        if (track_load_syn() != 0) { dlog("iso-play: syn failed\n"); return 6; }
    } else {
        const char *base = src, *p;
        for (p = src; *p; p++)
            if (*p == '/' || *p == '\\') base = p + 1;
        snprintf(title, sizeof(title), "%.25s", base);
        if (track_load_file(src) != 0) {
            g_frames = 0;                    /* stay alive, show status */
            set_state(ST_BAD_FILE);
        }
    }

    char target[64];
    snprintf(target, sizeof(target), "127.0.0.1:%s", port);
    scene_transport *t = scene_tcp_client(target);
    if (!t) { dlog("iso-play: tcp client failed\n"); return 3; }

    scene_app_cbs cbs;
    memset(&cbs, 0, sizeof(cbs));
    cbs.pointer = on_pointer;
    cbs.activate = on_activate;
    cbs.key = on_key;
    g_app = scene_app_new_on(t, target, &cbs, NULL);
    if (!g_app) { dlog("iso-play: app_new failed\n"); return 4; }
    scene_tcp_set_nonblock(t, 1);            /* pass-17 lesson */
    dlog("iso-play: connected, waiting for welcome\n");

    for (i = 0; i < 500; i++) {
        scene_app_pump(g_app);
        scene_app_flush(g_app);
        if (scene_client_welcomed(scene_app_client(g_app))) break;
        msleep(5);
    }
    if (i >= 500) { dlog("iso-play: no welcome\n"); return 5; }
    dlog("iso-play: welcomed\n");

    content = scene_app_create_window_role(g_app, PLY_X, PLY_Y, PLY_W, PLY_H,
                                           title, SCENE_ROLE_GENERIC);
    if (content == SCENE_NO_PARENT) {
        dlog("iso-play: window create failed\n");
        return 6;
    }
    dlog("iso-play: window built content=%u\n", (unsigned)content);

    /* status label: child of the CONTENT node, in the content area */
    cl = scene_app_client(g_app);
    {
        static const scene_rect sr = {PLY_X + 4, PLY_Y + 40, PLY_W - 8, 16};
        scene_client_create_node(cl, content, STATUS_NODE, SCENE_ROLE_LABEL,
                                 &sr, SCENE_FLAG_VISIBLE);
    }
    scene_app_present(g_app);
    scene_app_flush(g_app);

    /* ---- playback begin -------------------------------------------------- */
    if (g_state != ST_BAD_FILE) {
        int rc;
#if defined(_WIN32)
        win_start();
        rc = 0;
#else
        rc = alsa_setup();
#endif
        if (rc != 0) {
            set_state(ST_NO_DEVICE);
            dlog("iso-play: no audio device, UI alive\n");
        } else {
            set_state(ST_PLAY);
            dlog("iso-play: playing %u frames\n", (unsigned)g_frames);
        }
    }
    push_status();

    for (;;) {
        scene_app_pump(g_app);
        scene_app_flush(g_app);
        if (g_state == ST_PLAY) {
            int r;
#if defined(_WIN32)
            win_drain_step();
            r = g_pos >= g_frames ? 1 : 0;
#else
            r = alsa_write_pass();
#endif
            if (r > 0) {                     /* track fully queued/drained */
                set_state(ST_DONE);
                dlog("iso-play: done at frame %u\n", (unsigned)g_pos);
                push_status();
            } else if (r < 0) {
                set_state(ST_DONE);          /* stop on errors, stay alive */
                dlog("iso-play: playback ended with error\n");
                push_status();
            }
        } else if (g_status_dirty) {
            push_status();
        }
        msleep(5);
    }
}