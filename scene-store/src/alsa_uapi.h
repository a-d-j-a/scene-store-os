/*
 * alsa_uapi.h — self-contained ALSA PCM UAPI for the scene engine.
 *
 * The musl ISO sysroot ships no ALSA headers, so this header mirrors the
 * kernel boundary the way iso_drm.c mirrors <drm/drm.h>: minimal structs
 * and ioctl numbers taken from Linux 6.6
 * include/uapi/sound/asound.h (verified against the actual v6.6 file —
 * every value below carries the header line it came from). No alsa-lib,
 * no libasound; the app talks raw ioctls to /dev/snd/pcmC0D0p.
 *
 * 64-bit note: snd_pcm_uframes_t is `unsigned long` (8 bytes on x86_64);
 * sizeof(struct snd_pcm_hw_params) = 608 and sizeof(struct
 * snd_pcm_sw_params) = 136 on LP64 — static-asserted at the bottom of
 * this header (x86_64 only; other ABIs differ).
 */
#ifndef SCENE_ALSA_UAPI_H
#define SCENE_ALSA_UAPI_H

#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- ioctl encoding (linux/asm-generic/ioctl.h, the x86_64 layout) ------ */

#define SCENE_AL_IOC_NRBITS   8
#define SCENE_AL_IOC_TYPEBITS 8
#define SCENE_AL_IOC_SIZEBITS 14
#define SCENE_AL_IOC_DIRBITS  2
#define SCENE_AL_IOC_NRSHIFT   0
#define SCENE_AL_IOC_TYPESHIFT (SCENE_AL_IOC_NRSHIFT + SCENE_AL_IOC_NRBITS)
#define SCENE_AL_IOC_SIZESHIFT (SCENE_AL_IOC_TYPESHIFT + SCENE_AL_IOC_TYPEBITS)
#define SCENE_AL_IOC_DIRSHIFT  (SCENE_AL_IOC_SIZESHIFT + SCENE_AL_IOC_SIZEBITS)
#define SCENE_AL_IOC_NONE      0U
#define SCENE_AL_IOC_WRITE     1U
#define SCENE_AL_IOC_READ      2U
#define SCENE_AL_IOC(dir, type, nr, size) \
    (((dir) << SCENE_AL_IOC_DIRSHIFT) | \
     ((type) << SCENE_AL_IOC_TYPESHIFT) | \
     ((nr) << SCENE_AL_IOC_NRSHIFT) | \
     ((size) << SCENE_AL_IOC_SIZESHIFT))
#define SCENE_AL_IO(type, nr)      SCENE_AL_IOC(SCENE_AL_IOC_NONE, (type), (nr), 0)
#define SCENE_AL_IOR(type, nr, sz) SCENE_AL_IOC(SCENE_AL_IOC_READ, (type), (nr), (sz))
#define SCENE_AL_IOW(type, nr, sz) SCENE_AL_IOC(SCENE_AL_IOC_WRITE, (type), (nr), (sz))
#define SCENE_AL_IOWR(type, nr, sz) \
    SCENE_AL_IOC(SCENE_AL_IOC_READ | SCENE_AL_IOC_WRITE, (type), (nr), (sz))

/* ---- asound.h: type · snd_pcm_uframes_t --------------------------------- */

typedef unsigned long snd_pcm_uframes_t;
typedef signed long   snd_pcm_sframes_t;

/* ---- asound.h: access modes (enum, "Digital audio interface") ------------ */
/* #define	SNDRV_PCM_ACCESS_RW_INTERLEAVED	((__force snd_pcm_access_t) 3)  */
#define SNDRV_PCM_ACCESS_RW_INTERLEAVED 3

/* ---- asound.h: formats --------------------------------------------------- */
/* #define	SNDRV_PCM_FORMAT_S16_LE	((__force snd_pcm_format_t) 2)         */
#define SNDRV_PCM_FORMAT_S16_LE 2

/* ---- asound.h: hw parameter indices ------------------------------------- */
/* asound.h lines (inline comments removed: -Wcomment):                    */
/*   #define SNDRV_PCM_HW_PARAM_ACCESS       0   // Access type             */
/*   #define SNDRV_PCM_HW_PARAM_FORMAT       1   // Format                  */
/*   #define SNDRV_PCM_HW_PARAM_SUBFORMAT    2   // Subformat               */
/*   #define SNDRV_PCM_HW_PARAM_FIRST_MASK   SNDRV_PCM_HW_PARAM_ACCESS      */
/*   #define SNDRV_PCM_HW_PARAM_LAST_MASK    SNDRV_PCM_HW_PARAM_SUBFORMAT   */
#define SNDRV_PCM_HW_PARAM_ACCESS        0
#define SNDRV_PCM_HW_PARAM_FORMAT        1
#define SNDRV_PCM_HW_PARAM_SUBFORMAT     2
#define SNDRV_PCM_HW_PARAM_FIRST_MASK    SNDRV_PCM_HW_PARAM_ACCESS
#define SNDRV_PCM_HW_PARAM_LAST_MASK     SNDRV_PCM_HW_PARAM_SUBFORMAT
/*   #define SNDRV_PCM_HW_PARAM_SAMPLE_BITS  8   // Bits per sample         */
/*   #define SNDRV_PCM_HW_PARAM_FRAME_BITS   9   // Bits per frame          */
/*   #define SNDRV_PCM_HW_PARAM_CHANNELS     10  // Channels                */
/*   #define SNDRV_PCM_HW_PARAM_RATE         11  // Approx rate             */
/*   #define SNDRV_PCM_HW_PARAM_PERIOD_TIME  12  // Distance between irqs   */
/*   #define SNDRV_PCM_HW_PARAM_PERIOD_SIZE  13  // Frames between irqs     */
/*   #define SNDRV_PCM_HW_PARAM_PERIOD_BYTES 14  // Bytes between irqs      */
/*   #define SNDRV_PCM_HW_PARAM_PERIODS      15  // Irqs per buffer         */
/*   #define SNDRV_PCM_HW_PARAM_BUFFER_TIME  16  // Buffer duration in us   */
/*   #define SNDRV_PCM_HW_PARAM_BUFFER_SIZE  17  // Buffer size in frames   */
/*   #define SNDRV_PCM_HW_PARAM_BUFFER_BYTES 18  // Buffer size in bytes    */
/*   #define SNDRV_PCM_HW_PARAM_TICK_TIME    19  // Tick duration in us     */
/*   #define SNDRV_PCM_HW_PARAM_FIRST_INTERVAL  SNDRV_PCM_HW_PARAM_SAMPLE_BITS */
/*   #define SNDRV_PCM_HW_PARAM_LAST_INTERVAL   SNDRV_PCM_HW_PARAM_TICK_TIME   */
#define SNDRV_PCM_HW_PARAM_SAMPLE_BITS  8
#define SNDRV_PCM_HW_PARAM_FRAME_BITS   9
#define SNDRV_PCM_HW_PARAM_CHANNELS     10
#define SNDRV_PCM_HW_PARAM_RATE         11
#define SNDRV_PCM_HW_PARAM_PERIOD_TIME  12
#define SNDRV_PCM_HW_PARAM_PERIOD_SIZE  13
#define SNDRV_PCM_HW_PARAM_PERIOD_BYTES 14
#define SNDRV_PCM_HW_PARAM_PERIODS      15
#define SNDRV_PCM_HW_PARAM_BUFFER_TIME  16
#define SNDRV_PCM_HW_PARAM_BUFFER_SIZE  17
#define SNDRV_PCM_HW_PARAM_BUFFER_BYTES 18
#define SNDRV_PCM_HW_PARAM_TICK_TIME    19
#define SNDRV_PCM_HW_PARAM_FIRST_INTERVAL SNDRV_PCM_HW_PARAM_SAMPLE_BITS
#define SNDRV_PCM_HW_PARAM_LAST_INTERVAL  SNDRV_PCM_HW_PARAM_TICK_TIME

/* ---- asound.h: struct snd_interval / struct snd_mask --------------------- */
/* #define SNDRV_MASK_MAX	256 */
/* struct snd_interval { unsigned int min, max; unsigned int openmin:1,
 *                       openmax:1, integer:1, empty:1; };                  */
/* struct snd_mask { __u32 bits[(SNDRV_MASK_MAX+31)/32]; };                  */
#define SNDRV_MASK_MAX 256
#define SCENE_AL_MASK_WORDS ((SNDRV_MASK_MAX + 31) / 32)   /* 8 */

typedef struct snd_interval {
    unsigned int min, max;
    unsigned int openmin : 1;
    unsigned int openmax : 1;
    unsigned int integer : 1;
    unsigned int empty   : 1;
} snd_interval;

typedef struct snd_mask {
    uint32_t bits[SCENE_AL_MASK_WORDS];
} snd_mask;

/* ---- asound.h: struct snd_pcm_hw_params ---------------------------------- */
/* flags, masks[3], mres[5], intervals[12], ires[9], rmask, cmask, info,
 * msbits, rate_num, rate_den, fifo_size (snd_pcm_uframes_t), reserved[64].
 * sizeof == 608 on LP64: 4 + 3*32 + 5*32 + 12*12 + 9*12 + 6*4 + 8 + 64.   */
typedef struct snd_pcm_hw_params {
    unsigned int      flags;
    snd_mask          masks[(SNDRV_PCM_HW_PARAM_LAST_MASK -
                             SNDRV_PCM_HW_PARAM_FIRST_MASK) + 1];
    snd_mask          mres[5];        /* reserved masks  */
    snd_interval      intervals[(SNDRV_PCM_HW_PARAM_LAST_INTERVAL -
                                 SNDRV_PCM_HW_PARAM_FIRST_INTERVAL) + 1];
    snd_interval      ires[9];        /* reserved intervals */
    unsigned int      rmask;          /* W: requested masks */
    unsigned int      cmask;          /* R: changed masks */
    unsigned int      info;           /* R: Info flags for returned setup */
    unsigned int      msbits;         /* R: used most significant bits */
    unsigned int      rate_num;       /* R: rate numerator */
    unsigned int      rate_den;       /* R: rate denominator */
    snd_pcm_uframes_t fifo_size;      /* R: chip FIFO size in frames */
    unsigned char     reserved[64];   /* reserved for future */
} snd_pcm_hw_params;

/* ---- asound.h: struct snd_pcm_sw_params ---------------------------------- */
/* tstamp_mode, period_step, sleep_min, avail_min, xfer_align (obsolete),
 * start_threshold, stop_threshold, silence_threshold, silence_size,
 * boundary, proto, tstamp_type, reserved[56]. sizeof == 136 on LP64.      */
typedef struct snd_pcm_sw_params {
    int               tstamp_mode;
    unsigned int      period_step;
    unsigned int      sleep_min;
    snd_pcm_uframes_t avail_min;
    snd_pcm_uframes_t xfer_align;          /* obsolete */
    snd_pcm_uframes_t start_threshold;
    snd_pcm_uframes_t stop_threshold;
    snd_pcm_uframes_t silence_threshold;
    snd_pcm_uframes_t silence_size;
    snd_pcm_uframes_t boundary;
    unsigned int      proto;
    unsigned int      tstamp_type;
    unsigned char     reserved[56];
} snd_pcm_sw_params;

/* ---- asound.h: protocol version ------------------------------------------ */
/* #define SNDRV_PROTOCOL_VERSION(major, minor, subminor) \
 *   (((major)<<16)|((minor)<<8)|(subminor))                                 */
#define SNDRV_PROTOCOL_VERSION(major, minor, subminor) \
    (((major) << 16) | ((minor) << 8) | (subminor))
#define SNDRV_PCM_VERSION SNDRV_PROTOCOL_VERSION(2, 0, 15)

/* ---- asound.h: PCM ioctls (type byte 'A') -------------------------------- */
/* #define SNDRV_PCM_IOCTL_HW_REFINE	_IOWR('A', 0x10, struct snd_pcm_hw_params)  */
/* #define SNDRV_PCM_IOCTL_HW_PARAMS	_IOWR('A', 0x11, struct snd_pcm_hw_params)  */
/* #define SNDRV_PCM_IOCTL_SW_PARAMS	_IOWR('A', 0x13, struct snd_pcm_sw_params)  */
/* #define SNDRV_PCM_IOCTL_PREPARE	_IO('A', 0x40)                            */
/* #define SNDRV_PCM_IOCTL_RESET		_IO('A', 0x41)                            */
/* #define SNDRV_PCM_IOCTL_START		_IO('A', 0x42)                            */
/* #define SNDRV_PCM_IOCTL_DROP		_IO('A', 0x43)                            */
/* #define SNDRV_PCM_IOCTL_DRAIN		_IO('A', 0x44)                            */
/* NOTE: SNDRV_PCM_IOCTL_STOP does NOT exist in the modern kernel UAPI — it
 * was removed. For playback the stop verbs are DROP (0x43: discard the
 * queued tail, back to SETUP immediately) and DRAIN (0x44: play the tail
 * out first — verified in sound/core/pcm_native.c v6.6: drain waits for
 * the DRAINING streams to empty, bounded by buffer_size*1100/rate ms, and
 * returns immediately when nothing is queued). START (0x42) only succeeds
 * from the PREPARED state — snd_pcm_pre_start() returns -EBADFD otherwise
 * (pcm_native.c:1410-1415).                                                 */
#define SNDRV_PCM_IOCTL_HW_REFINE \
    SCENE_AL_IOWR('A', 0x10, sizeof(snd_pcm_hw_params))
#define SNDRV_PCM_IOCTL_HW_PARAMS \
    SCENE_AL_IOWR('A', 0x11, sizeof(snd_pcm_hw_params))
#define SNDRV_PCM_IOCTL_SW_PARAMS \
    SCENE_AL_IOWR('A', 0x13, sizeof(snd_pcm_sw_params))
#define SNDRV_PCM_IOCTL_PREPARE SCENE_AL_IO('A', 0x40)
#define SNDRV_PCM_IOCTL_START   SCENE_AL_IO('A', 0x42)
#define SNDRV_PCM_IOCTL_DROP    SCENE_AL_IO('A', 0x43)
#define SNDRV_PCM_IOCTL_DRAIN   SCENE_AL_IO('A', 0x44)

/* ---- small helpers (static inline, used by iso_play.c) ------------------- */

/* Open constraint set: every mask all bits, every interval full range.
 * This is the "any" parameter set snd_pcm_hw_params_any() builds; the
 * kernel refines it against the hardware in HW_REFINE/HW_PARAMS.        */
static inline void scene_al_hw_any(snd_pcm_hw_params *p)
{
    uint32_t i;
    memset(p, 0, sizeof(*p));
    for (i = 0; i < SCENE_AL_MASK_WORDS; i++) {
        uint32_t k;
        for (k = 0; k < sizeof(p->masks) / sizeof(p->masks[0]); k++)
            p->masks[k].bits[i] = 0xFFFFFFFFu;
    }
    {
        uint32_t k;
        for (k = 0;
             k < sizeof(p->intervals) / sizeof(p->intervals[0]); k++) {
            p->intervals[k].min = 0;
            p->intervals[k].max = 0xFFFFFFFFu;
        }
    }
}

static inline void scene_al_mask_set(snd_mask *m, unsigned int bit)
{
    m->bits[bit >> 5] |= (uint32_t)1u << (bit & 31u);
}

static inline void scene_al_mask_clear_all(snd_mask *m)
{
    uint32_t i;
    for (i = 0; i < SCENE_AL_MASK_WORDS; i++)
        m->bits[i] = 0;
}

/* Pin an interval to exactly `v` with integer=1 (min==max after refine). */
static inline void scene_al_iv_exact(snd_interval *iv, unsigned int v)
{
    iv->min = v;
    iv->max = v;
    iv->openmin = 0;
    iv->openmax = 0;
    iv->integer = 1;
    iv->empty = 0;
}

/* Index of a mask parameter inside hw_params.masks[] / interval parameter
 * inside hw_params.intervals[] (asound.h FIRST_* definitions).            */
#define SCENE_AL_MASK_IDX(k)    ((k) - SNDRV_PCM_HW_PARAM_FIRST_MASK)
#define SCENE_AL_INTERVAL_IDX(k) ((k) - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL)

#ifdef __cplusplus
}
#endif

/* ---- ABI guards: struct sizes, proven against the v6.6 UAPI ------------ */
/* The kernel ABI is LP64 on Linux x86_64 (unsigned long = 8 bytes). The
 * header only compiles against a kernel on POSIX, but it must stay
 * compile-clean under the Windows LLP64 model too (unsigned long = 4),
 * so the asserts are keyed to the data model, not just the ISA.         */
#if defined(__LP64__)
_Static_assert(sizeof(struct snd_pcm_hw_params) == 608,
               "snd_pcm_hw_params LP64 size mismatch (v6.6 asound.h)");
_Static_assert(sizeof(struct snd_pcm_sw_params) == 136,
               "snd_pcm_sw_params LP64 size mismatch (v6.6 asound.h)");
#elif defined(__x86_64__)
_Static_assert(sizeof(struct snd_pcm_hw_params) == 604,
               "snd_pcm_hw_params LLP64 mirror size mismatch");
_Static_assert(sizeof(struct snd_pcm_sw_params) == 104,
               "snd_pcm_sw_params LLP64 mirror size mismatch");
#endif

#endif /* SCENE_ALSA_UAPI_H */