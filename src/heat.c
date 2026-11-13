/* PotentialHeat (PotentialBreaks): a soft clipper / bus saturator for MPC OS standalone devices.
 * DRIVE pushes the signal into a saturation curve that rounds the peaks off at the CEILING instead of clipping:
 *   CHARACTER  SOFT (smooth, warm), TAPE (asymmetric, a little darker, even harmonics), HARD (firm knee, loudest)
 *   TONE       a dark <-> bright tilt after the saturation
 *   LEVEL MATCH  measures the loudness in and out and turns the processed sound down by the difference (1.3.0; it was a
 *                fixed estimate from DRIVE before), so you hear the colour, not just "louder"
 * then dry/wet MIX and OUTPUT. The curves run at 2x the sample rate, so pushing it hard stays clean (no aliasing fizz).
 * A PRESET list (also MPC's PRESET menu) sets everything in one tap. A meter shows how much is being shaved off.
 * 44.1 kHz, interleaved int16 stereo, 128-frame blocks (wrapper/engine.h). */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "engine.h"
#include "../shared/looks/potential_theme.h"   /* the LOOK switch (parameter "theme") */

#define FS 44100.0f
#define PI_F 3.14159265f

enum { P_OLD_PRESET, P_DRIVE, P_CHAR, P_TONE, P_CEIL, P_MATCH, P_MIX, P_OUT, P_PRESET, P_DICE, P_WILD, NP };
static const char *KEYS[NP] = {"preset", "drive", "character", "tone", "ceiling", "match", "mix", "output", "preset_pick", "dice", "wild"};

/* presets: drive dB, character, tone %, ceiling dB, level match, mix %, output dB */
#define NPRESET 16
static const float PRESETS[NPRESET][7] = {
    { 6, 0,   0, -0.3f, 1, 100,  0},   /* Warm Drums */
    { 4, 1, -10, -0.3f, 1, 100,  0},   /* Bus Glue */
    { 9, 1,  10, -0.3f, 1, 100,  0},   /* Break Heat */
    { 6, 2,   0, -0.3f, 0, 100,  0},   /* Loud & Clean */
    {18, 2,  15, -0.3f, 1, 100,  1},   /* Smash */
    { 7, 1, -20, -0.3f, 1, 100,  0},   /* Tape Glow */
    { 8, 0, -15, -0.3f, 1, 100,  0},   /* Fat Kick */
    {10, 2,  20, -0.3f, 1, 100,  0},   /* Snare Bite */
    { 7, 0, -25, -0.3f, 1, 100,  0},   /* Bass Warmth */
    { 3, 0,   0, -0.5f, 0, 100,  0},   /* Beat Master */
    {20, 2,  10, -0.3f, 1,  40,  0},   /* Parallel Crush */
    { 0, 0,   0, -0.3f, 1, 100,  0},   /* Peak Catcher */
    /* 1.3.0: drum-bus pairs. Pick the same name in PotentialPunch, put Punch first and Heat after it on the bus */
    { 7, 1, -10, -0.3f, 1, 100,  0},   /* Boom Bap Bus */
    { 9, 1, -35, -1.0f, 1, 100,  0},   /* Lo-Fi Bus */
    { 6, 2,  15, -0.3f, 1, 100,  0},   /* Modern Bus */
    { 4, 0,   0, -0.5f, 1,  70,  0},   /* Glue Bus */
};
static const char *PRESET_NAMES[NPRESET] = {"Warm Drums", "Bus Glue", "Break Heat", "Loud & Clean", "Smash", "Tape Glow",
                                            "Fat Kick", "Snare Bite", "Bass Warmth", "Beat Master", "Parallel Crush", "Peak Catcher",
                                            "Boom Bap Bus", "Lo-Fi Bus", "Modern Bus", "Glue Bus"};

/* ---- 2x oversampling: a 31-tap half-band low-pass (windowed sinc, Blackman), used for up- and down-sampling ---- */
#define NT 31
#define HD 15                     /* half length: the filter's delay in 2x samples */
static float H[NT];
static void make_filter(void) {
    double sum = 0;
    for (int i = 0; i < NT; i++) {
        double n = i - HD, x = n * 0.5;                     /* cut-off at a quarter of the 2x rate = 22.05 kHz */
        double s = n == 0 ? 1.0 : sin(M_PI * x) / (M_PI * x);
        double w = 0.42 - 0.5 * cos(2 * M_PI * i / (NT - 1)) + 0.08 * cos(4 * M_PI * i / (NT - 1));
        H[i] = (float)(s * w); sum += H[i];
    }
    for (int i = 0; i < NT; i++) H[i] = (float)(H[i] / sum);
}


/* ---- AUTO LEVEL: measures how loud the dry and processed sound are (about the last second) and turns the processed
 * sound by the difference, so louder never just sounds better. Holds still in near-silence. ---- */
typedef struct { float e_in, e_out, g; } lm_t;
static inline float lm_step(lm_t *m, float p_in, float p_out, float k_e, float k_g, float g_lo, float g_hi) {
    m->e_in += (p_in - m->e_in) * k_e; m->e_out += (p_out - m->e_out) * k_e;
    if (m->e_in > 1e-5f && m->e_out > 1e-9f) {   /* above about -50 dBFS */
        float want = sqrtf(m->e_in / m->e_out);
        want = want < g_lo ? g_lo : want > g_hi ? g_hi : want;
        m->g += (want - m->g) * k_g;
    }
    return m->g;
}

typedef struct {
    float p[NP];
    int pending;
    float up[2][NT], dn[2][NT];          /* filter histories (2x rate) */
    int up_i, dn_i;
    float dry[2][32]; int dry_i;          /* dry delay to line up with the oversampled path */
    float dc_x[2], dc_y[2];               /* DC blocker (TAPE's asymmetry) */
    float tilt_lp[2], tape_lp[2];
    float s_drive, s_ceil, s_mix, s_out, s_tone, s_match;
    lm_t lm;                              /* LEVEL MATCH, measured */
    /* display */
    float win_red, red_hold, in_hold; int win_n, meter, slow; unsigned rev;
    pt_theme_t theme;   /* which look MPC loads next time: not part of the saved state */
} heat_t;

static void apply_preset(heat_t *s, int n) {
    if (n < 0 || n >= NPRESET) return;
    s->p[P_PRESET] = (float)n;
    for (int i = 0; i < 7; i++) s->p[P_DRIVE + i] = PRESETS[n][i];
    s->pending |= 0xfe; s->rev++;
}

static float clampf(float x, float lo, float hi) { return x < lo ? lo : x > hi ? hi : x; }
/* ---- DICE: ROLL makes a new setting; WILD (MILD / MEDIUM / WILD) says how far it goes ---- */
static uint32_t dice_rng = 0x9e3779b9u;
static float dice_u(void) { dice_rng ^= dice_rng << 13; dice_rng ^= dice_rng >> 17; dice_rng ^= dice_rng << 5; return (dice_rng >> 8) / 16777216.0f; }
static float dice_in(float lo, float hi) { return lo + (hi - lo) * dice_u(); }
static int dice_pick(const int *w, int n) { int t = 0; for (int i = 0; i < n; i++) t += w[i]; float r = dice_u() * (float)t; for (int i = 0; i < n; i++) { if (r < (float)w[i]) return i; r -= (float)w[i]; } return n - 1; }
static float dice_step(float x, float step) { return roundf(x / step) * step; }

static void roll(heat_t *s) {
    int w = (int)s->p[P_WILD];
    static const float DRV[3][2] = {{2, 8}, {4, 14}, {8, 24}}, TONE[3] = {20, 40, 80}, CEIL[3] = {-1, -3, -6}, MIX[3] = {80, 60, 30};
    static const int CH[3][3] = {{5, 5, 0}, {4, 4, 2}, {1, 1, 1}};
    s->p[P_DRIVE] = dice_step(dice_in(DRV[w][0], DRV[w][1]), 0.5f);
    s->p[P_CHAR] = (float)dice_pick(CH[w], 3);
    s->p[P_TONE] = dice_step(dice_in(-TONE[w], TONE[w]), 1);
    s->p[P_CEIL] = dice_step(dice_in(CEIL[w], -0.1f), 0.1f);
    s->p[P_MATCH] = 1;                       /* level matched, so a roll is judged by its sound, not its loudness */
    s->p[P_MIX] = dice_step(dice_in(MIX[w], 100), 1);
    s->p[P_OUT] = 0;
    s->pending |= 0xfe; s->rev++;
}
static void clamp_all(heat_t *s) {
    s->p[P_PRESET] = clampf(roundf(s->p[P_PRESET]), 0, NPRESET - 1);
    s->p[P_DRIVE] = clampf(s->p[P_DRIVE], 0, 24);
    s->p[P_CHAR] = clampf(roundf(s->p[P_CHAR]), 0, 2);
    s->p[P_TONE] = clampf(s->p[P_TONE], -100, 100);
    s->p[P_CEIL] = clampf(s->p[P_CEIL], -6, 0);
    s->p[P_MATCH] = clampf(roundf(s->p[P_MATCH]), 0, 1);
    s->p[P_MIX] = clampf(s->p[P_MIX], 0, 100);
    s->p[P_OUT] = clampf(s->p[P_OUT], -12, 12);
    s->p[P_WILD] = clampf(roundf(s->p[P_WILD]), 0, 2);
    s->p[P_DICE] = 0;
}

static float match_gain(const heat_t *s) {   /* LEVEL MATCH: the measured gain (only ever down), else none */
    return s->p[P_MATCH] > 0.5f ? s->lm.g : 1.0f;
}

static void *create(const char *data_dir) {
    static int made = 0;
    if (!made) { make_filter(); made = 1; }
    heat_t *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->lm.g = 1.0f;
    pt_theme_init(&s->theme, data_dir);
    apply_preset(s, 0); s->pending = 0;
    s->p[P_WILD] = 1;
    dice_rng ^= (uint32_t)(uintptr_t)s;
    s->s_drive = powf(10.0f, s->p[P_DRIVE] / 20.0f); s->s_ceil = powf(10.0f, s->p[P_CEIL] / 20.0f);
    s->s_mix = s->p[P_MIX] / 100.0f; s->s_out = powf(10.0f, s->p[P_OUT] / 20.0f); s->s_match = match_gain(s); s->s_tone = s->p[P_TONE] / 100.0f;
    s->in_hold = -120.0f;
    return s;
}
static void destroy(void *inst) { free(inst); }
static void midi(void *inst, const uint8_t *msg, int len) { (void)inst; (void)msg; (void)len; }

/* "preset" (parameter 0) is an unused slot since this version: MPC's host flicks parameter 0 on STOP and on Overdub while
 * playing, which reloaded a preset and threw away knob tweaks. The PRESET picker is "preset_pick" (last); a saved
 * project's "preset=" still restores into it. */
static int find(const char *key) {
    for (int i = 0; i < NP; i++) if (!strcmp(key, KEYS[i])) return i;
    return -1;
}

static void set_param(void *inst, const char *key, const char *val) {
    heat_t *s = inst;
    if (pt_theme_set(&s->theme, key, val)) return;
    if (!strcmp(key, "state")) {
        char tmp[512]; snprintf(tmp, sizeof tmp, "%s", val);
        for (char *tok = strtok(tmp, ";"); tok; tok = strtok(NULL, ";")) {
            char *eq = strchr(tok, '=');
            if (!eq) continue;
            *eq = 0;
            int i = find(tok);
            if (i == P_OLD_PRESET) i = P_PRESET;
            if (i >= 0) s->p[i] = (float)atof(eq + 1);
        }
        clamp_all(s); s->rev++;
        return;
    }
    int i = find(key);
    if (i < 0 || i == P_OLD_PRESET) return;
    if (i == P_PRESET) {   /* only on a change: a project reload re-sending the same preset keeps its tweaks */
        int n = (int)lroundf((float)atof(val));
        if (n != (int)s->p[P_PRESET]) apply_preset(s, n);
        return;
    }
    if (i == P_DICE) { if (atof(val) > 0.5) roll(s); return; }   /* a trigger: each press rolls */
    s->p[i] = (float)atof(val);
    clamp_all(s);
}

int mpc_engine_changed(void *inst, char *key, int len) {
    heat_t *s = inst;
    for (int i = 0; i < NP; i++)
        if (s->pending & (1 << i)) { s->pending &= ~(1 << i); snprintf(key, len, "%s", KEYS[i]); return 1; }
    return 0;
}

static int get_param(void *inst, const char *key, char *buf, int buf_len) {
    heat_t *s = inst;
    { int pt_n = pt_theme_get(&s->theme, key, buf, buf_len); if (pt_n) return pt_n; }
    if (!strcmp(key, "state")) {
        int n = 0;
        for (int i = 1; i < NP && n < buf_len; i++) n += snprintf(buf + n, buf_len - n, "%s=%g;", KEYS[i], (double)s->p[i]);
        return n;
    }
    if (!strcmp(key, "display_rev")) return snprintf(buf, buf_len, "%u", s->rev);
    if (!strcmp(key, "meter")) return snprintf(buf, buf_len, "%d", s->meter);
    if (!strcmp(key, "reading")) return snprintf(buf, buf_len, "TAP A PRESET, THEN SET DRIVE  -  THE METER SHOWS WHAT'S SHAVED OFF THE PEAKS");   /* fixed: live text would need UpdateDisplays, which close MPC's PRESET menu */
    if (!strncmp(key, "preset:", 7) || !strncmp(key, "preset_pick:", 12)) {
        int n = atoi(strchr(key, ':') + 1);
        return n >= 0 && n < NPRESET ? snprintf(buf, buf_len, "%s", PRESET_NAMES[n]) : 0;
    }
    int i = find(key);
    if (i < 0) return 0;
    if (i == P_OLD_PRESET) i = P_PRESET;
    if (i == P_DICE) return snprintf(buf, buf_len, "0");
    if (i == P_PRESET || i == P_CHAR || i == P_MATCH || i == P_WILD) return snprintf(buf, buf_len, "%d", (int)lroundf(s->p[i]));
    return snprintf(buf, buf_len, "%g", (double)s->p[i]);
}

static void render(void *inst, int16_t *out_lr, int frames) { (void)inst; memset(out_lr, 0, (size_t)frames * 2 * sizeof(int16_t)); }

static inline float ftanh(float x) {    /* rational approximation, error < 1e-4; exact +-1 beyond +-4.97 */
    if (x > 4.97f) return 1.0f;
    if (x < -4.97f) return -1.0f;
    float x2 = x * x;
    return x * (135135.0f + x2 * (17325.0f + x2 * (378.0f + x2))) / (135135.0f + x2 * (62370.0f + x2 * (3150.0f + x2 * 28.0f)));
}

/* the curves: input already scaled so 1.0 = the ceiling; output stays within -1..1 */
static inline float curve(float x, int ch) {
    if (ch == 0) {                                                  /* SOFT: clean up to half the ceiling, then a smooth knee */
        float a = fabsf(x);
        if (a <= 0.5f) return x;
        float y = 0.5f + 0.5f * ftanh((a - 0.5f) * 2.0f);
        return x < 0 ? -y : y;
    }
    if (ch == 1) {                                                  /* TAPE: asymmetric, softer on one side */
        float y = ftanh(x + 0.18f * x * x * (x > 0 ? 1.0f : -0.4f) * (fabsf(x) < 3 ? 1.0f : 0.0f));
        return y > 1 ? 1 : y < -1 ? -1 : y;
    }
    float a = fabsf(x);                                             /* HARD: flat to ~0.8, then a short knee */
    float a2 = a * a, a4 = a2 * a2;
    float y = a / sqrtf(sqrtf(sqrtf(1.0f + a4 * a4)));
    return x < 0 ? -y : y;
}

static void process(void *inst, const int16_t *in_lr, int16_t *out_lr, int frames) {
    heat_t *s = inst;
    const int ch = (int)s->p[P_CHAR];
    const float k = 1.0f - expf(-1.0f / (0.02f * FS));
    const float t_drive = powf(10.0f, s->p[P_DRIVE] / 20.0f), t_ceil = powf(10.0f, s->p[P_CEIL] / 20.0f);
    const float t_mix = s->p[P_MIX] / 100.0f, t_out = powf(10.0f, s->p[P_OUT] / 20.0f), t_tone = s->p[P_TONE] / 100.0f;
    const float k_e = 1.0f - expf(-1.0f / (0.8f * FS)), k_lg = 1.0f - expf(-1.0f / (0.25f * FS));
    const int matching = s->p[P_MATCH] > 0.5f;
    const float k_tilt = 1.0f - expf(-2.0f * PI_F * 900.0f / FS);
    const float k_tape = 1.0f - expf(-2.0f * PI_F * 12000.0f / (2 * FS));  /* tape's gentle top-end roll-off, at the 2x rate */
    const float k_dc = 1.0f - 2.0f * PI_F * 8.0f / FS;

    s->s_tone += (t_tone - s->s_tone) * 0.35f;   /* per block */
    const float g_lo = powf(10.0f, -s->s_tone * 6.0f / 20.0f), g_hi = powf(10.0f, s->s_tone * 6.0f / 20.0f);
    for (int n = 0; n < frames; n++) {
        s->s_drive += (t_drive - s->s_drive) * k; s->s_ceil += (t_ceil - s->s_ceil) * k;
        s->s_mix += (t_mix - s->s_mix) * k; s->s_out += (t_out - s->s_out) * k;
        float in[2] = {in_lr[2 * n] * (1.0f / 32768.0f), in_lr[2 * n + 1] * (1.0f / 32768.0f)};
        float pre = s->s_drive / s->s_ceil, post = s->s_ceil;
        float out[2], red = 1.0f;

        for (int c = 0; c < 2; c++) {
            /* dry, delayed by the oversampling filters' delay (2 x HD samples at 2x = HD samples here) */
            s->dry[c][s->dry_i] = in[c];
        }
        /* 2x: two input samples per input (x and 0), filtered (x2 gain to keep the level) */
        float y2[2][2], dec[2] = {0, 0};
        /* up-sampling, polyphase: the half-band filter's odd phase is just the centre tap (a plain delay) */
        int ui = s->up_i;
        for (int c = 0; c < 2; c++) s->up[c][ui] = in[c];
        float u[2][2];
        for (int c = 0; c < 2; c++) {
            float acc = 0;
            for (int m = 0; m < 16; m++) { int j = ui - m; if (j < 0) j += NT; acc += H[2 * m] * s->up[c][j]; }
            int j = ui - 7; if (j < 0) j += NT;
            u[c][0] = 2.0f * acc; u[c][1] = 2.0f * H[HD] * s->up[c][j];
        }
        s->up_i = (ui + 1) % NT;
        for (int ph = 0; ph < 2; ph++) {
            for (int c = 0; c < 2; c++) {
                float x = u[c][ph] * pre;
                float y = curve(x, ch);
                float ax = fabsf(x);
                if (ax > 0.05f) { float r = ax / (fabsf(y) + 1e-9f); if (r > red) red = r; }   /* ratio now, dB once per window */
                if (ch == 1) { s->tape_lp[c] += (y - s->tape_lp[c]) * k_tape; y = s->tape_lp[c]; }
                y2[c][ph] = y * post;
            }
            /* down-sampling filter history */
            int di = s->dn_i;
            for (int c = 0; c < 2; c++) s->dn[c][di] = y2[c][ph];
            if (ph == 0)                     /* keep the even samples: the delay is then exactly HD input samples */
                for (int c = 0; c < 2; c++) {
                    float acc = 0;
                    for (int t = 0; t < NT; t += 2) { int j = di - t; if (j < 0) j += NT; acc += H[t] * s->dn[c][j]; }
                    { int j = di - HD; if (j < 0) j += NT; acc += H[HD] * s->dn[c][j]; }
                    dec[c] = acc;
                }
            s->dn_i = (di + 1) % NT;
        }
        float wet_raw[2], dry_d[2];
        for (int c = 0; c < 2; c++) {
            float acc = dec[c];
            /* DC blocker (TAPE's asymmetry makes a little DC) */
            float x = acc, y = x - s->dc_x[c] + k_dc * s->dc_y[c]; s->dc_x[c] = x; s->dc_y[c] = y;
            /* tone tilt: +-6 dB around 900 Hz */
            s->tilt_lp[c] += (y - s->tilt_lp[c]) * k_tilt;
            float lo = s->tilt_lp[c], hi = y - lo;
            wet_raw[c] = lo * g_lo + hi * g_hi;
            int dj = s->dry_i - HD; if (dj < 0) dj += 32;
            dry_d[c] = s->dry[c][dj];
        }
        {   /* LEVEL MATCH: the processed sound measured against the dry, turned down (never up) to match */
            float g = lm_step(&s->lm, 0.5f * (dry_d[0] * dry_d[0] + dry_d[1] * dry_d[1]),
                              0.5f * (wet_raw[0] * wet_raw[0] + wet_raw[1] * wet_raw[1]), k_e, k_lg, 0.063f, 1.0f);
            s->s_match += ((matching ? g : 1.0f) - s->s_match) * k;
        }
        for (int c = 0; c < 2; c++) {
            float wet = wet_raw[c] * s->s_match, dry = dry_d[c];
            out[c] = (dry + (wet - dry) * s->s_mix) * s->s_out;
        }
        s->dry_i = (s->dry_i + 1) & 31;

        for (int c = 0; c < 2; c++) {
            float o = out[c];
            if (!(o == o)) o = 0;
            o = o > 0.89f ? 0.89f + 0.11f * tanhf((o - 0.89f) / 0.11f) : o < -0.89f ? -0.89f - 0.11f * tanhf((-o - 0.89f) / 0.11f) : o;
            o = clampf(o, -1.0f, 1.0f);
            out_lr[2 * n + c] = (int16_t)lrintf(o * 32767.0f);
        }
        if (red > s->win_red) s->win_red = red;
        float a = fabsf(in[0]) > fabsf(in[1]) ? fabsf(in[0]) : fabsf(in[1]);
        float adb = a > 1e-6f ? 20.0f * log10f(a) : -120.0f;
        if (adb > s->in_hold) s->in_hold = adb;
    }
    s->win_n += frames;
    if (s->win_n >= 1764) {   /* every 40 ms */
        s->win_red = s->win_red > 1.0f ? 20.0f * log10f(s->win_red) : 0.0f;
        int m = (int)lroundf(s->win_red * 2.0f);   /* 0.5 dB a segment, 0..15 dB */
        m = m < 0 ? 0 : m > 30 ? 30 : m;
        if (m != s->meter) { s->meter = m; s->rev++; }
        if (s->win_red > s->red_hold) s->red_hold = s->win_red;
        s->win_red = 0; s->win_n = 0;
        if (++s->slow >= 8) {   /* ~320 ms: let the readout fall slowly */
            s->slow = 0;
            s->red_hold *= 0.5f; if (s->red_hold < 0.05f) s->red_hold = 0;
            s->in_hold = s->in_hold > -120.0f ? s->in_hold - 12.0f : -120.0f;
        }
    }
}

static const mpc_engine_t ENGINE = { create, destroy, midi, set_param, get_param, render, process, NULL };
const mpc_engine_t *mpc_engine(void) { return &ENGINE; }
