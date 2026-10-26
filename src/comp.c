/*
 * Master compressor models. Original 1010music Blackbox, firmware 3.1.9.
 *
 * Stock: Tools "Compressor: On / Off" (param 0xb4, global, saved in settings.tml) compresses Out 1. The engine runs it
 * once per 256-frame block (FUN_0804eddc): the gain is decided once per block from the block level, ramped, then a
 * 0 dB block limiter. This turns the switch into a list:
 *   Off, Stock, Glue, Punch, Opto, Squash, Limit
 * Off and Stock keep their stock meaning (0 and 1, as already saved). The five models are fixed presets of one
 * per-sample, stereo-linked feed-forward compressor (soft-knee gain computer, attack / release smoothing of the gain
 * reduction in dB, optional two-stage "auto" release, optional parallel blend, optional 1.5 ms look-ahead), modelled
 * loosely on: SSL bus (Glue), slow-attack drum comp (Punch), LA-2A (Opto), 1176 all-buttons in parallel (Squash),
 * a mastering limiter (Limit). State lives in the backup SRAM at 0x38800400.
 *
 * Plus one setting, "Comp Thresh:" (new global parameter 0x1d0, -24..+12 dB, default 0, xml "compthresh"): moves the
 * models' threshold by that much (for Limit, it raises or lowers the input push instead; the ceiling stays).
 * The Tools Main page is reordered so Compressor and Comp Thresh sit right under Headphone.
 */
#include <stdint.h>

#pragma GCC optimize("O2", "no-tree-loop-distribute-patterns")

#define FN(addr) ((addr) | 1u)

typedef void (*register_fn)(void *table, int param, const char *label, const char *const *names, int count,
                            const char *xml);
typedef int (*setting_fn)(const void *store, unsigned param);
typedef int (*stock_fn)(int *obj, void *bufs);
typedef void *(*buf_of_fn)(void *bufs, unsigned idx);
typedef int (*frames_fn)(void *buf);
typedef void (*chans_fn)(void *buf, float **l, float **r);
typedef unsigned (*index_fn)(int *obj);
typedef int (*chain_fn)(int *next, void *bufs);
typedef void (*register_num_fn)(void *table, int param, int type, const char *label, int min, int max, const char *xml);
typedef void (*store_add_fn)(void *store, int param, int value);

#define fw_register_list ((register_fn)FN(0x0808c0d8))
#define fw_register_num  ((register_num_fn)FN(0x0808c12c))
#define fw_store_add     ((store_add_fn)FN(0x08093ef6))
#define fw_setting       ((setting_fn)FN(0x08093e9c))
#define fw_stock_comp    ((stock_fn)FN(0x0804eddc))
#define fw_buf_of        ((buf_of_fn)FN(0x0805f37c))
#define fw_frames        ((frames_fn)FN(0x0804d8e8))
#define fw_stereo        ((chans_fn)FN(0x0804d9c0))      /* forces stereo: returns left and right */
#define DEFAULT_INDEX_FN 0x08046a15u                     /* stock "output index" method: reads obj +0x1e */

#define SETTINGS ((const void *)(0x24020088u + 0x8c90u)) /* app: global settings store */
#define PARAM_COMP 0xb4
#define PARAM_THRESH 0x1d0       /* unregistered in stock; ids < 500 are table slots */
#define TYPE_DB    7             /* value in milli-dB, shown as dB (the Metro Gain style) */

void bkp_enable(void);
void live_register(void *table);
void live_defaults(void *store);

#define MODELS 5
const char *const comp_names[2 + MODELS] = {"Off", "Stock", "Glue", "Punch", "Opto", "Squash", "Limit"};

/* Replaces the Compressor registration (FUN_0808c12c, an On/Off): same parameter, now a 7-entry list. */
void comp_register(void *table, int param, int type, const char *label, int min, int max, const char *xml)
{
    (void)type;
    (void)min;
    (void)max;
    fw_register_list(table, param, label, comp_names, 2 + MODELS, xml);
    fw_register_num(table, PARAM_THRESH, TYPE_DB, "Comp Thresh:", -24000, 12000, "compthresh");
    live_register(table);
}

/* Replaces the "Compressor defaults to 1" entry of the global settings defaults (bl @0x08094886): add ours after it. */
void comp_defaults(void *store, int param, int value)
{
    fw_store_add(store, param, value);
    fw_store_add(store, PARAM_THRESH, 0);
    live_defaults(store);
}

/* Pad Overdrive is now a different circuit (od.c): its label on the pad page. */
const char od_label[] = "Saturation:";

#define FS 48000.0f
#define COEF(ms) __builtin_expf(-1.0f / ((ms) * 0.001f * FS))   /* folded at compile time */

struct model {
    float thresh, ratio, knee;   /* dB, x:1 (0 = infinite), dB */
    float att, rel_fast, rel_slow, auto_db;   /* smoothing coefficients; slow release once within auto_db of target */
    float rms;                   /* RMS detector coefficient, 0 = peak */
    float pre, makeup, mix;      /* linear input gain, linear output gain, wet share */
    int lookahead;               /* frames */
};

static const struct model models[MODELS] = {
    /* Glue: 2:1, 6 dB knee, 10 ms attack, auto release 100 -> 600 ms, RMS, +3 dB */
    {-18.0f, 2.0f, 6.0f, COEF(10.0f), COEF(100.0f), COEF(600.0f), 2.0f, COEF(10.0f), 1.0f, 1.4125f, 1.0f, 0},
    /* Punch: 4:1, 3 dB knee, 30 ms attack lets the hit through, 80 ms release, peak, +4 dB */
    {-20.0f, 4.0f, 3.0f, COEF(30.0f), COEF(80.0f), COEF(80.0f), 0.0f, 0.0f, 1.0f, 1.5849f, 1.0f, 0},
    /* Opto: 3:1, 10 dB knee, 10 ms attack, 60 ms then 1.5 s release, RMS, +4 dB */
    {-22.0f, 3.0f, 10.0f, COEF(10.0f), COEF(60.0f), COEF(1500.0f), 3.0f, COEF(20.0f), 1.0f, 1.5849f, 1.0f, 0},
    /* Squash: 20:1, hard, 0.5 ms attack, 60 ms release, peak, +12 dB wet, blended 50/50 with dry */
    {-35.0f, 20.0f, 0.0f, COEF(0.5f), COEF(60.0f), COEF(60.0f), 0.0f, 0.0f, 1.0f, 3.9811f, 0.5f, 0},
    /* Limit: +6 dB in, ceiling -0.3 dBFS, 1.5 ms look-ahead, 60 ms release */
    {-0.3f, 0.0f, 0.0f, COEF(0.5f), COEF(60.0f), COEF(60.0f), 0.0f, 0.0f, 1.9953f, 1.0f, 1.0f, 72},
};
#define CEILING 0.9661f          /* -0.3 dBFS */

#define LA_MAX 72
struct comp_state {
    uint32_t magic;
    uint32_t model;
    float gr;                    /* current gain reduction, dB (<= 0) */
    float ms;                    /* RMS detector: mean square */
    uint32_t la_pos;
    float la[LA_MAX][2];
};
#define S ((volatile struct comp_state *)0x38800400u)
#define MAGIC 0x434f4d31u        /* "COM1" */

/* log2 / exp2, bit tricks with quadratic correction (error ~0.005 / ~0.3%) */
static inline __attribute__((always_inline)) float log2f_fast(float x)
{
    union { float f; uint32_t u; } v = {x};
    float e = (float)((int)((v.u >> 23) & 0xff) - 128);
    v.u = (v.u & 0x007fffffu) | 0x3f800000u;
    float m = v.f;
    return e + (-0.34484843f * m + 2.02466578f) * m - 0.67487759f;
}

static inline __attribute__((always_inline)) float exp2f_fast(float x)
{
    if (x < -30.0f)
        return 0.0f;
    if (x > 30.0f)
        x = 30.0f;
    int i = (int)x - (x < 0.0f);
    float f = x - (float)i;
    float p = 1.0f + f * (0.6565f + f * 0.3435f);           /* 2^f on [0, 1) */
    union { float f; uint32_t u; } v = {p};
    v.u += (uint32_t)i << 23;
    return v.f;
}

static float computer(const struct model *m, float level_db)
{
    float over = level_db - m->thresh, slope = m->ratio > 0.0f ? 1.0f / m->ratio - 1.0f : -1.0f;
    if (m->knee > 0.0f && 2.0f * over > -m->knee && 2.0f * over < m->knee) {
        float k = over + 0.5f * m->knee;
        return slope * k * k / (2.0f * m->knee);
    }
    return over > 0.0f ? slope * over : 0.0f;
}

static void process(const struct model *m, float *l, float *r, int n)
{
    float gr = S->gr, ms = S->ms;
    uint32_t pos = S->la_pos;
    for (int i = 0; i < n; i++) {
        float xl = l[i] * m->pre, xr = r[i] * m->pre;
        float level;
        if (m->rms > 0.0f) {
            float p = 0.5f * (xl * xl + xr * xr);
            ms = p + m->rms * (ms - p);
            level = 3.0103f * log2f_fast(ms + 1e-12f);     /* 10 log10 */
        } else {
            float a = xl < 0.0f ? -xl : xl, b = xr < 0.0f ? -xr : xr;
            level = 6.0206f * log2f_fast((a > b ? a : b) + 1e-9f);   /* 20 log10 */
        }
        float target = computer(m, level);
        if (target < gr)
            gr = target + m->att * (gr - target);
        else
            gr = target + (target - gr < m->auto_db ? m->rel_slow : m->rel_fast) * (gr - target);
        float g = exp2f_fast(gr * 0.16610f) * m->makeup;   /* dB -> linear */
        float dl = xl, dr = xr;
        if (m->lookahead) {
            dl = S->la[pos][0];
            dr = S->la[pos][1];
            S->la[pos][0] = xl;
            S->la[pos][1] = xr;
            if (++pos >= (uint32_t)m->lookahead)
                pos = 0;
        }
        float yl = dl * g, yr = dr * g;
        if (m->mix < 1.0f) {
            yl = m->mix * yl + (1.0f - m->mix) * dl / m->pre;
            yr = m->mix * yr + (1.0f - m->mix) * dr / m->pre;
        }
        yl = yl > CEILING ? CEILING : yl < -CEILING ? -CEILING : yl;
        yr = yr > CEILING ? CEILING : yr < -CEILING ? -CEILING : yr;
        l[i] = yl;
        r[i] = yr;
    }
    S->gr = gr;
    S->ms = ms;
    S->la_pos = pos;
}

/* Replaces the engine's call to the stock compressor (bl @0x08053ca0), reached only when the setting is not Off. */
int comp_process(int *obj, void *bufs)
{
    unsigned model = (unsigned)fw_setting(SETTINGS, PARAM_COMP);
    if (model < 2 || model >= 2 + MODELS)
        return fw_stock_comp(obj, bufs);
    bkp_enable();
    if (S->magic != MAGIC || S->model != model) {
        S->gr = 0.0f;
        S->ms = 0.0f;
        S->la_pos = 0;
        for (int i = 0; i < LA_MAX; i++)
            S->la[i][0] = S->la[i][1] = 0.0f;
        S->model = model;
        S->magic = MAGIC;
    }
    index_fn index = *(index_fn *)(*obj + 0x54);
    unsigned out = (uint32_t)index == DEFAULT_INDEX_FN ? *(uint16_t *)((uint8_t *)obj + 0x1e) : index(obj);
    void *buf = fw_buf_of(bufs, out);
    int n = fw_frames(buf);
    float *l = 0, *r = 0;
    fw_stereo(buf, &l, &r);
    if (l && r && n > 0) {
        struct model m = models[model - 2];
        float off = (float)fw_setting(SETTINGS, PARAM_THRESH) * 0.001f;
        off = off < -24.0f ? -24.0f : off > 12.0f ? 12.0f : off;
        if (m.ratio > 0.0f)
            m.thresh += off;
        else
            m.pre *= exp2f_fast(-off * 0.16610f);            /* Limit: lower threshold = more push into the ceiling */
        process(&m, l, r, n);
    }
    int *next = (int *)obj[2];                               /* stock passes the buffers on down a chain */
    if (!next)
        return 1;
    return (*(chain_fn *)(*next + 0xc))(next, bufs);
}
