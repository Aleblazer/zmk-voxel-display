/*
 * Copyright (c) 2026 Spencer Deven (@Aleblazer)
 * SPDX-License-Identifier: MIT
 *
 * Built-in test animations for the voxel volume. Runs in its own low
 * priority thread; keypresses arrive through ZMK keycode events, which on a
 * dongle (split central) cover every key of every peripheral.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <string.h>

#include <zmk_voxel/voxel.h>
#include <zmk/event_manager.h>
#include <zmk/events/keycode_state_changed.h>
#include <zmk/keymap.h>
#include <dt-bindings/zmk/hid_usage_pages.h>

LOG_MODULE_DECLARE(zmk_voxel, CONFIG_ZMK_VOXEL_LOG_LEVEL);

extern const uint8_t voxel_font5x7[][5];
extern const uint8_t voxel_font5x7_count;
#define FONT_W 5
#define FONT_H 7
#define GLYPH_UP 0x40
#define GLYPH_DOWN 0x41
#define GLYPH_RIGHT 0x42
#define GLYPH_LEFT 0x43
#define GLYPH_RETURN 0x44
#define GLYPH_SHIFT 0x45

#define W ZMK_VOXEL_WIDTH
#define H ZMK_VOXEL_HEIGHT
#define Z_REAR (zmk_voxel_depth() - 1)

/* ------------------------------------------------------------------------
 * Helpers: PRNG, fixed point, sine, drawing
 * ---------------------------------------------------------------------- */
#define FP 8
#define F(x) ((int32_t)(x) * (1 << FP))

static uint32_t rng_state = 0x2545F491u;

static uint32_t rnd(void) {
    uint32_t x = rng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    rng_state = x;
    return x;
}

static int32_t rnd_range(int32_t lo, int32_t hi) {
    return lo + (int32_t)(rnd() % (uint32_t)(hi - lo + 1));
}

static const int16_t sin_tab[64] = {
    0,    25,   50,   74,   98,   121,  142,  162,  181,  198,  213,  226,  237,
    245,  251,  255,  256,  255,  251,  245,  237,  226,  213,  198,  181,  162,
    142,  121,  98,   74,   50,   25,   0,    -25,  -50,  -74,  -98,  -121, -142,
    -162, -181, -198, -213, -226, -237, -245, -251, -255, -256, -255, -251, -245,
    -237, -226, -213, -198, -181, -162, -142, -121, -98,  -74,  -50,  -25};

static inline int16_t isin(uint8_t a) {
    return sin_tab[a & 63];
}
static inline int16_t icos(uint8_t a) {
    return sin_tab[(a + 16) & 63];
}

static inline void plot(int16_t x, int16_t y, uint8_t z) {
    if (x < 0 || y < 0 || x >= W || y >= H) {
        return;
    }
    zmk_voxel_set((uint8_t)x, (uint8_t)y, z, true);
}

static void hline(int16_t x0, int16_t x1, int16_t y, uint8_t z) {
    for (int16_t x = x0; x <= x1; x++) {
        plot(x, y, z);
    }
}

static void vline(int16_t x, int16_t y0, int16_t y1, uint8_t z) {
    for (int16_t y = y0; y <= y1; y++) {
        plot(x, y, z);
    }
}

/* Draw a font glyph (index into voxel_font5x7) centred at (cx, cy). */
static void draw_glyph(uint8_t g, int16_t cx, int16_t cy, uint8_t z, uint8_t scale) {
    if (g >= voxel_font5x7_count) {
        g = '*' - 0x20;
    }
    const uint8_t *cols = voxel_font5x7[g];
    int16_t x0 = cx - (int16_t)(FONT_W * scale) / 2;
    int16_t y0 = cy - (int16_t)(FONT_H * scale) / 2;
    for (uint8_t col = 0; col < FONT_W; col++) {
        uint8_t bits = cols[col];
        if (!bits) {
            continue;
        }
        for (uint8_t row = 0; row < FONT_H; row++) {
            if (!(bits & (1u << row))) {
                continue;
            }
            for (uint8_t sy = 0; sy < scale; sy++) {
                for (uint8_t sx = 0; sx < scale; sx++) {
                    plot(x0 + col * scale + sx, y0 + row * scale + sy, z);
                }
            }
        }
    }
}

/* Row-major 1-bpp sprite, 8 pixels wide, bit 7 = leftmost. */
static void draw_sprite8(const uint8_t *rows, uint8_t h, int16_t x, int16_t y, uint8_t z,
                         uint8_t scale) {
    for (uint8_t r = 0; r < h; r++) {
        uint8_t bits = rows[r];
        for (uint8_t c = 0; c < 8; c++) {
            if (!(bits & (1u << (7 - c)))) {
                continue;
            }
            for (uint8_t sy = 0; sy < scale; sy++) {
                for (uint8_t sx = 0; sx < scale; sx++) {
                    plot(x + c * scale + sx, y + r * scale + sy, z);
                }
            }
        }
    }
}

/* ------------------------------------------------------------------------
 * HID keyboard usage -> glyph index
 * ---------------------------------------------------------------------- */
static uint8_t usage_to_glyph(uint16_t id) {
    if (id >= 0x04 && id <= 0x1D) {
        return (uint8_t)('A' - 0x20 + (id - 0x04));
    }
    if (id >= 0x1E && id <= 0x26) {
        return (uint8_t)('1' - 0x20 + (id - 0x1E));
    }
    switch (id) {
    case 0x27: return '0' - 0x20;
    case 0x28: return GLYPH_RETURN;
    case 0x29: return '!' - 0x20; /* escape */
    case 0x2A: return GLYPH_LEFT; /* backspace */
    case 0x2B: return GLYPH_RIGHT; /* tab */
    case 0x2C: return '_' - 0x20; /* space */
    case 0x2D: return '-' - 0x20;
    case 0x2E: return '=' - 0x20;
    case 0x2F: return '[' - 0x20;
    case 0x30: return ']' - 0x20;
    case 0x31: return '\\' - 0x20;
    case 0x33: return ';' - 0x20;
    case 0x34: return '\'' - 0x20;
    case 0x35: return 0x07; /* grave: no glyph, use the apostrophe */
    case 0x36: return ',' - 0x20;
    case 0x37: return '.' - 0x20;
    case 0x38: return '/' - 0x20;
    case 0x39: return 'A' - 0x20; /* caps lock */
    case 0x4C: return 'X' - 0x20; /* delete */
    case 0x4F: return GLYPH_RIGHT;
    case 0x50: return GLYPH_LEFT;
    case 0x51: return GLYPH_DOWN;
    case 0x52: return GLYPH_UP;
    case 0xE1:
    case 0xE5: return GLYPH_SHIFT;
    case 0xE0:
    case 0xE4: return '^' - 0x20; /* ctrl */
    case 0xE2:
    case 0xE6: return '+' - 0x20; /* alt */
    case 0xE3:
    case 0xE7: return '#' - 0x20; /* gui */
    default: break;
    }
    if (id >= 0x3A && id <= 0x45) {
        return 'F' - 0x20;
    }
    return '*' - 0x20;
}

/* ------------------------------------------------------------------------
 * Key event queue (listener context -> animation thread)
 * ---------------------------------------------------------------------- */
#define KEYQ_LEN 16
static uint16_t keyq[KEYQ_LEN];
static atomic_t keyq_head = ATOMIC_INIT(0);
static atomic_t keyq_tail = ATOMIC_INIT(0);

static void keyq_push(uint16_t usage) {
    atomic_val_t head = atomic_get(&keyq_head);
    atomic_val_t tail = atomic_get(&keyq_tail);
    if ((head - tail) >= KEYQ_LEN) {
        return; /* drop when flooded */
    }
    keyq[head % KEYQ_LEN] = usage;
    atomic_set(&keyq_head, head + 1);
}

static bool keyq_pop(uint16_t *usage) {
    atomic_val_t head = atomic_get(&keyq_head);
    atomic_val_t tail = atomic_get(&keyq_tail);
    if (head == tail) {
        return false;
    }
    *usage = keyq[tail % KEYQ_LEN];
    atomic_set(&keyq_tail, tail + 1);
    return true;
}

/* ------------------------------------------------------------------------
 * Animation 1: KEYFALL
 * ---------------------------------------------------------------------- */
typedef struct {
    bool alive;
    uint8_t glyph;
    int32_t x, y, z;
    int32_t vx, vy, vz;
    int16_t curl;
} glyph_particle_t;

static glyph_particle_t particles[CONFIG_ZMK_VOXEL_KEYFALL_MAX];
static uint8_t particle_next;

static void keyfall_spawn(uint8_t glyph) {
    glyph_particle_t *p = NULL;
    for (uint8_t i = 0; i < CONFIG_ZMK_VOXEL_KEYFALL_MAX; i++) {
        if (!particles[i].alive) {
            p = &particles[i];
            break;
        }
    }
    if (!p) {
        p = &particles[particle_next];
        particle_next = (uint8_t)((particle_next + 1) % CONFIG_ZMK_VOXEL_KEYFALL_MAX);
    }
    uint8_t angle = (uint8_t)(rnd() & 63);
    int32_t speed = rnd_range(40, 320);

    p->alive = true;
    p->glyph = glyph;
    p->x = F(rnd_range(32, W - 32));
    p->y = F(rnd_range(32, H - 32));
    p->z = F(Z_REAR) + F(1) - 1;
    p->vx = (speed * icos(angle)) >> 8;
    p->vy = (speed * isin(angle)) >> 8;
    p->vz = -rnd_range(12, 48);
    p->curl = (int16_t)rnd_range(-40, 40);
}

static void keyfall_frame(void) {
    zmk_voxel_clear();
    for (uint8_t i = 0; i < CONFIG_ZMK_VOXEL_KEYFALL_MAX; i++) {
        glyph_particle_t *p = &particles[i];
        if (!p->alive) {
            continue;
        }
        int32_t ax = (-p->vy * p->curl) / 1024;
        int32_t ay = (p->vx * p->curl) / 1024;
        p->vx += ax;
        p->vy += ay;
        p->x += p->vx;
        p->y += p->vy;
        p->z += p->vz;
        if (p->z < 0 || p->x < F(-16) || p->x > F(W + 16) || p->y < F(-16) || p->y > F(H + 16)) {
            p->alive = false;
            continue;
        }
        uint8_t zi = (uint8_t)(p->z >> FP);
        uint8_t scale = (uint8_t)(1 + (Z_REAR - zi) / 4);
        if (scale > 4) {
            scale = 4;
        }
        draw_glyph(p->glyph, (int16_t)(p->x >> FP), (int16_t)(p->y >> FP), zi, scale);
    }
}

/* ------------------------------------------------------------------------
 * Animation 2: PARALLAX side-scroller
 * ---------------------------------------------------------------------- */
#define HORIZON 78
#define GROUND_Y 106
#define WALKER_X 28

static const uint8_t walker_a[12] = {0x3C, 0x7E, 0x5A, 0x7E, 0x3C, 0x18,
                                     0x7E, 0x99, 0x18, 0x24, 0x42, 0xC3};
static const uint8_t walker_b[12] = {0x3C, 0x7E, 0x5A, 0x7E, 0x3C, 0x18,
                                     0x3C, 0x5A, 0x5A, 0x18, 0x18, 0x3C};
static const uint8_t walker_c[12] = {0x3C, 0x7E, 0x5A, 0x7E, 0x3C, 0x18,
                                     0x7E, 0x99, 0x18, 0x28, 0x24, 0x63};
static const uint8_t *const walk_cycle[4] = {walker_a, walker_b, walker_c, walker_b};

static uint8_t hill_prof[32];
static uint8_t mount_prof[32];
static uint32_t scroll;
static int16_t jump_y, jump_v;

/* Layer for each scene plane, spread through whatever depth is available. */
static uint8_t px_ground, px_water, px_hills, px_mount, px_cloud, px_stars;

static int16_t profile_at(const uint8_t *prof, uint32_t wx) {
    uint8_t i = (uint8_t)((wx >> 3) & 31);
    uint8_t f = (uint8_t)(wx & 7);
    int16_t a = prof[i];
    int16_t b = prof[(i + 1) & 31];
    return a + ((b - a) * f) / 8;
}

static void parallax_init(void) {
    for (uint8_t i = 0; i < 32; i++) {
        hill_prof[i] = (uint8_t)rnd_range(HORIZON - 14, HORIZON - 2);
        mount_prof[i] = (uint8_t)rnd_range(HORIZON - 52, HORIZON - 12);
    }
    mount_prof[5] = HORIZON - 60;
    mount_prof[19] = HORIZON - 56;
    mount_prof[27] = HORIZON - 48;

    uint8_t d = zmk_voxel_depth();
    px_ground = 0;
    px_water = (uint8_t)((d - 1) * 2 / 9);
    px_hills = (uint8_t)((d - 1) * 4 / 9);
    px_mount = (uint8_t)((d - 1) * 6 / 9);
    px_cloud = (uint8_t)((d - 1) * 8 / 9);
    px_stars = (uint8_t)(d - 1);
}

static void parallax_frame(void) {
    zmk_voxel_clear();
    scroll++;

    /* stars */
    {
        uint32_t off = scroll >> 4;
        for (uint8_t i = 0; i < 40; i++) {
            uint32_t h = (uint32_t)i * 2654435761u;
            int16_t x = (int16_t)(((h >> 8) - off) & 255) - 64;
            int16_t y = (int16_t)((h >> 20) % (HORIZON - 20));
            if (x >= 0) {
                plot(x, y, px_stars);
            }
        }
    }
    /* clouds + moon */
    {
        const int16_t mx = 100, my = 22, r = 9;
        for (uint8_t a = 0; a < 64; a++) {
            plot(mx + ((r * icos(a)) >> 8), my + ((r * isin(a)) >> 8), px_cloud);
        }
        uint32_t off = scroll >> 3;
        for (uint8_t i = 0; i < 4; i++) {
            int16_t cx = (int16_t)(((i * 64 + 20) - off) & 255) - 32;
            int16_t cy = 30 + (i * 7) % 15;
            for (int8_t k = -1; k <= 1; k++) {
                int16_t ex = cx + k * 9;
                int16_t er = 8 - (k ? 2 : 0);
                for (uint8_t a = 32; a < 64; a++) {
                    plot(ex + ((er * 2 * icos(a)) >> 8), cy + ((er * isin(a)) >> 8), px_cloud);
                }
            }
            hline(cx - 20, cx + 20, cy, px_cloud);
        }
    }
    /* mountains */
    {
        uint32_t off = scroll >> 2;
        int16_t prev_h = profile_at(mount_prof, off);
        for (int16_t x = 0; x < W; x++) {
            int16_t h = profile_at(mount_prof, off + (uint32_t)x);
            int16_t lo = h < prev_h ? h : prev_h, hi = h < prev_h ? prev_h : h;
            vline(x, lo, hi, px_mount);
            prev_h = h;
            for (int16_t y = h + 1; y < HORIZON; y++) {
                bool snow = (y - h) < 6 && h < HORIZON - 40;
                if (snow ? (((x + y) & 1) == 0) : (((x & 3) == 0) && ((y & 3) == 0))) {
                    plot(x, y, px_mount);
                }
            }
        }
        hline(0, W - 1, HORIZON, px_mount);
    }
    /* hills + trees */
    {
        uint32_t off = scroll >> 1;
        int16_t prev_h = profile_at(hill_prof, off);
        for (int16_t x = 0; x < W; x++) {
            int16_t h = profile_at(hill_prof, off + (uint32_t)x);
            int16_t lo = h < prev_h ? h : prev_h, hi = h < prev_h ? prev_h : h;
            vline(x, lo, hi, px_hills);
            prev_h = h;
            for (int16_t y = h + 2; y <= HORIZON + 2; y += 3) {
                if (((x + y) % 3) == 0) {
                    plot(x, y, px_hills);
                }
            }
            uint32_t wx = off + (uint32_t)x;
            if ((wx % 40) == 0) {
                vline(x, h - 6, h, px_hills);
                for (int8_t k = -3; k <= 3; k++) {
                    plot(x + k, h - 6 + (k < 0 ? -k : k) - 4, px_hills);
                }
                for (int8_t k = -2; k <= 2; k++) {
                    plot(x + k, h - 3 + (k < 0 ? -k : k) - 3, px_hills);
                }
            }
        }
    }
    /* water */
    {
        uint32_t off = scroll;
        for (uint8_t band = 0; band < 4; band++) {
            int16_t base = HORIZON + 5 + band * 5;
            for (int16_t x = 0; x < W; x++) {
                uint8_t a = (uint8_t)(((x + off) >> 1) + band * 8 + (scroll >> 1));
                int16_t y = base + ((2 * isin(a)) >> 8);
                if (((x + off + band * 3) & 7) < 5) {
                    plot(x, y, px_water);
                }
            }
        }
        for (uint8_t i = 0; i < 12; i++) {
            uint32_t h = ((uint32_t)i * 40503u + (scroll >> 3) * 2654435761u);
            plot((int16_t)(h % W), HORIZON + 4 + (int16_t)((h >> 8) % 22), px_water);
        }
    }
    /* ground + walker */
    {
        uint32_t off = scroll * 2;
        hline(0, W - 1, GROUND_Y, px_ground);
        for (int16_t x = 0; x < W; x++) {
            uint32_t wx = off + (uint32_t)x;
            uint32_t h = wx * 2654435761u;
            if ((h >> 28) == 0) {
                plot(x, GROUND_Y + 2 + (int16_t)((h >> 20) & 3), px_ground);
            }
            if ((wx % 23) == 0) {
                vline(x, GROUND_Y - 3, GROUND_Y - 1, px_ground);
                plot(x - 1, GROUND_Y - 2, px_ground);
                plot(x + 1, GROUND_Y - 2, px_ground);
            }
        }
        for (int16_t x = (int16_t)(off & 3); x < W; x += 4) {
            plot(x, GROUND_Y + 6, px_ground);
        }
        if (jump_y || jump_v) {
            jump_v -= 12;
            jump_y += jump_v;
            if (jump_y <= 0) {
                jump_y = jump_v = 0;
            }
        }
        uint8_t pose = jump_y ? 0 : (uint8_t)((scroll >> 2) & 3);
        int16_t wy = GROUND_Y - 24 - (jump_y >> FP);
        draw_sprite8(walk_cycle[pose], 12, WALKER_X, wy, px_ground, 2);
    }
}

/* ------------------------------------------------------------------------
 * Animation 3: BOUNCE
 * ---------------------------------------------------------------------- */
static void bounce_frame(void) {
    static int16_t px = 20, py = 40, pz = 0;
    static int8_t vx = 2, vy = 3, vz = 1;
    static uint8_t zdiv;
    const int16_t size = 14;
    int16_t depth = zmk_voxel_depth() >= 3 ? 3 : 1;

    px += vx;
    py += vy;
    if (px <= 0 || px >= W - size) {
        vx = -vx;
    }
    if (py <= 0 || py >= H - size) {
        vy = -vy;
    }
    if (++zdiv >= 8) {
        zdiv = 0;
        pz += vz;
        if (pz <= 0 || pz >= zmk_voxel_depth() - depth) {
            vz = -vz;
        }
    }
    zmk_voxel_clear();
    for (int16_t z = pz; z < pz + depth; z++) {
        for (int16_t y = py; y < py + size; y++) {
            for (int16_t x = px; x < px + size; x++) {
                zmk_voxel_set((uint8_t)x, (uint8_t)y, (uint8_t)z, true);
            }
        }
    }
}

/* ------------------------------------------------------------------------
 * Mode management, key events, thread
 * ---------------------------------------------------------------------- */
static volatile enum zmk_voxel_anim mode = (enum zmk_voxel_anim)CONFIG_ZMK_VOXEL_ANIM_DEFAULT;

void zmk_voxel_anim_set(enum zmk_voxel_anim m) {
    if (m >= ZMK_VOXEL_ANIM_COUNT) {
        m = ZMK_VOXEL_ANIM_OFF;
    }
    mode = m;
    zmk_voxel_lock();
    zmk_voxel_clear();
    zmk_voxel_unlock();
    zmk_voxel_flush();
}

enum zmk_voxel_anim zmk_voxel_anim_get(void) {
    return mode;
}

void zmk_voxel_anim_next(void) {
    zmk_voxel_anim_set((enum zmk_voxel_anim)((mode + 1) % ZMK_VOXEL_ANIM_COUNT));
}

void zmk_voxel_anim_key_event(uint16_t usage_id, bool pressed) {
    if (pressed) {
        keyq_push(usage_id);
    }
}

static int voxel_anim_keycode_listener(const zmk_event_t *eh) {
    struct zmk_keycode_state_changed *ev = as_zmk_keycode_state_changed(eh);
    if (ev == NULL || ev->usage_page != HID_USAGE_KEY) {
        return ZMK_EV_EVENT_BUBBLE;
    }
    zmk_voxel_anim_key_event((uint16_t)ev->keycode, ev->state);
    return ZMK_EV_EVENT_BUBBLE;
}
ZMK_LISTENER(zmk_voxel_anim, voxel_anim_keycode_listener);
ZMK_SUBSCRIPTION(zmk_voxel_anim, zmk_keycode_state_changed);

static void drain_keys(void) {
    uint16_t usage;
    while (keyq_pop(&usage)) {
        switch (mode) {
        case ZMK_VOXEL_ANIM_KEYFALL:
            keyfall_spawn(usage_to_glyph(usage));
            break;
        case ZMK_VOXEL_ANIM_PARALLAX:
            if (usage == 0x2C && jump_y == 0) { /* space */
                jump_v = 120;
            }
            break;
        default:
            break;
        }
    }
}

static void anim_thread_fn(void *p1, void *p2, void *p3) {
    ARG_UNUSED(p1);
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);

    while (!zmk_voxel_is_ready()) {
        k_sleep(K_MSEC(100));
    }
    rng_state ^= k_cycle_get_32() | 1u;
    parallax_init();

    for (;;) {
        k_sleep(K_MSEC(CONFIG_ZMK_VOXEL_ANIM_FRAME_MS));
        drain_keys();
        if (mode == ZMK_VOXEL_ANIM_OFF || !zmk_voxel_is_on()) {
            continue;
        }
        zmk_voxel_lock();
        switch (mode) {
        case ZMK_VOXEL_ANIM_KEYFALL:
            keyfall_frame();
            break;
        case ZMK_VOXEL_ANIM_PARALLAX:
            parallax_frame();
            break;
        case ZMK_VOXEL_ANIM_BOUNCE:
            bounce_frame();
            break;
        default:
            break;
        }
        if (zmk_keymap_highest_layer_active() > 0) {
            for (uint8_t i = 0; i < W; i++) {
                plot(i, 0, 0);
                plot(i, H - 1, 0);
                plot(0, i, 0);
                plot(W - 1, i, 0);
            }
        }
        zmk_voxel_unlock();
        zmk_voxel_flush();
    }
}

K_THREAD_DEFINE(zmk_voxel_anim_thread, CONFIG_ZMK_VOXEL_ANIM_THREAD_STACK_SIZE, anim_thread_fn,
                NULL, NULL, NULL, K_PRIO_PREEMPT(CONFIG_ZMK_VOXEL_THREAD_PRIORITY + 1), 0, 0);
