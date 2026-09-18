/*
 * Copyright (c) 2026 Spencer Deven (@Aleblazer)
 * SPDX-License-Identifier: MIT
 *
 * Volumetric SH1107 driver. Panels are grouped into banks, one bank per SPI
 * bus. Every bank has its own transfer thread, so banks refresh in parallel;
 * a frame thread keeps all banks in the same phase (commands, then data) so
 * D/C and RESET may be shared between banks.
 *
 * Each panel runs in SH1107 vertical addressing mode: after the address
 * pointers are homed, one contiguous 2048-byte write covers the whole panel.
 */

#define DT_DRV_COMPAT aleblazer_voxel_sh1107

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/init.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>
#include <string.h>

#include <zmk_voxel/voxel.h>

#if IS_ENABLED(CONFIG_ZMK_VOXEL_BLANK_ON_IDLE)
#include <zmk/event_manager.h>
#include <zmk/events/activity_state_changed.h>
#endif

LOG_MODULE_REGISTER(zmk_voxel, CONFIG_ZMK_VOXEL_LOG_LEVEL);

BUILD_ASSERT(DT_NUM_INST_STATUS_OKAY(aleblazer_voxel_sh1107) == 1,
             "exactly one aleblazer,voxel-sh1107 node is supported");
#define VOXEL_NODE DT_INST(0, aleblazer_voxel_sh1107)

/* ------------------------------------------------------------------------
 * SH1107 command set
 * ---------------------------------------------------------------------- */
#define SH1107_COL_LO(x) (0x00 | ((x) & 0x0F))
#define SH1107_COL_HI(x) (0x10 | (((x) >> 4) & 0x07))
#define SH1107_ADDR_MODE_VERT 0x21
#define SH1107_CONTRAST 0x81
#define SH1107_SEG_REMAP_OFF 0xA0
#define SH1107_SEG_REMAP_ON 0xA1
#define SH1107_ALL_ON_OFF 0xA4
#define SH1107_NORMAL 0xA6
#define SH1107_INVERT 0xA7
#define SH1107_MUX_RATIO 0xA8
#define SH1107_DCDC_MODE 0xAD
#define SH1107_DCDC_OFF 0x8A /* external VPP: internal converter off */
#define SH1107_DISPLAY_OFF 0xAE
#define SH1107_DISPLAY_ON 0xAF
#define SH1107_PAGE_ADDR(p) (0xB0 | ((p) & 0x0F))
#define SH1107_COM_SCAN_INC 0xC0
#define SH1107_COM_SCAN_DEC 0xC8
#define SH1107_DISPLAY_OFFSET 0xD3
#define SH1107_CLOCK_DIV 0xD5
#define SH1107_PRECHARGE 0xD9
#define SH1107_VCOM_DESELECT 0xDB
#define SH1107_START_LINE 0xDC

/* ------------------------------------------------------------------------
 * Banks from the devicetree
 * ---------------------------------------------------------------------- */
#define SPI_OP (SPI_OP_MODE_MASTER | SPI_WORD_SET(8) | SPI_TRANSFER_MSB)

enum bank_job { JOB_NONE = 0, JOB_CMD, JOB_CLEAR, JOB_DATA };

struct voxel_bank {
    const struct spi_dt_spec spi;
    const struct gpio_dt_spec *cs;
    const uint8_t cs_count;
    const struct gpio_dt_spec dc;
    const struct gpio_dt_spec reset;
    k_thread_stack_t *stack;
    size_t stack_size;
    uint8_t z_offset;
    struct k_thread thread;
    struct k_sem go;
    volatile enum bank_job job;
};

#define VOXEL_CS_SPEC(node_id, prop, idx) GPIO_DT_SPEC_GET_BY_IDX(node_id, prop, idx),
#define VOXEL_BANK_SYM(node_id) _CONCAT(voxel_bank_, DT_DEP_ORD(node_id))

#define VOXEL_BANK_DEFINE(node_id)                                                                 \
    static const struct gpio_dt_spec _CONCAT(VOXEL_BANK_SYM(node_id), _cs)[] = {                   \
        DT_FOREACH_PROP_ELEM(node_id, panel_cs_gpios, VOXEL_CS_SPEC)};                              \
    static K_THREAD_STACK_DEFINE(_CONCAT(VOXEL_BANK_SYM(node_id), _stack),                         \
                                 CONFIG_ZMK_VOXEL_BANK_THREAD_STACK_SIZE);                         \
    static struct voxel_bank VOXEL_BANK_SYM(node_id) = {                                           \
        .spi = SPI_DT_SPEC_GET(node_id, SPI_OP, 0),                                                \
        .cs = _CONCAT(VOXEL_BANK_SYM(node_id), _cs),                                               \
        .cs_count = DT_PROP_LEN(node_id, panel_cs_gpios),                                          \
        .dc = GPIO_DT_SPEC_GET(node_id, dc_gpios),                                                 \
        .reset = GPIO_DT_SPEC_GET_OR(node_id, reset_gpios, {0}),                                   \
        .stack = _CONCAT(VOXEL_BANK_SYM(node_id), _stack),                                         \
        .stack_size = K_THREAD_STACK_SIZEOF(_CONCAT(VOXEL_BANK_SYM(node_id), _stack)),             \
    };

DT_FOREACH_STATUS_OKAY(aleblazer_voxel_sh1107_bank, VOXEL_BANK_DEFINE)

#define VOXEL_BANK_PTR(node_id, prop, idx) &VOXEL_BANK_SYM(DT_PHANDLE_BY_IDX(node_id, prop, idx)),
static struct voxel_bank *const banks[] = {DT_FOREACH_PROP_ELEM(VOXEL_NODE, banks, VOXEL_BANK_PTR)};
#define BANK_COUNT ARRAY_SIZE(banks)

#define VOXEL_BANK_LEN(node_id, prop, idx) DT_PROP_LEN(DT_PHANDLE_BY_IDX(node_id, prop, idx)) +
#define VOXEL_DEPTH (DT_FOREACH_PROP_ELEM(VOXEL_NODE, banks, VOXEL_BANK_LEN) 0)
BUILD_ASSERT(VOXEL_DEPTH >= 1 && VOXEL_DEPTH <= 32, "unsupported panel count");

#define VOXEL_HAS_VPP DT_NODE_HAS_PROP(VOXEL_NODE, vpp_en_gpios)
#if VOXEL_HAS_VPP
static const struct gpio_dt_spec vpp_en = GPIO_DT_SPEC_GET(VOXEL_NODE, vpp_en_gpios);
#endif

#define VOXEL_FLIP_X DT_PROP(VOXEL_NODE, flip_x)
#define VOXEL_FLIP_Y DT_PROP(VOXEL_NODE, flip_y)
#define VOXEL_FLIP_Z DT_PROP(VOXEL_NODE, flip_z)
#define VOXEL_SWAP_XY DT_PROP(VOXEL_NODE, swap_xy)

/* ------------------------------------------------------------------------
 * State
 * ---------------------------------------------------------------------- */
/* back: drawn into by the application. front: read by EasyDMA / the SPI
 * driver. Both are plain static RAM, which is what the nRF SPIM needs. */
static uint8_t back_buffer[VOXEL_DEPTH][ZMK_VOXEL_LAYER_BYTES];
static uint8_t front_buffer[VOXEL_DEPTH][ZMK_VOXEL_LAYER_BYTES];

static uint8_t cmd_buf[32]; /* shared command bytes, RAM for DMA */
static uint8_t cmd_len;

static K_THREAD_STACK_DEFINE(frame_stack, CONFIG_ZMK_VOXEL_FRAME_THREAD_STACK_SIZE);
static struct k_thread frame_thread;
static K_SEM_DEFINE(frame_sem, 0, 1);
static K_SEM_DEFINE(done_sem, 0, 64);
static K_MUTEX_DEFINE(back_lock);

enum { PEND_POWER = BIT(0), PEND_CONTRAST = BIT(1), PEND_INVERT = BIT(2) };

static atomic_t pending = ATOMIC_INIT(0);
static atomic_t dirty = ATOMIC_INIT(0);
static volatile bool ready = false;
static volatile bool want_on = true;
static volatile bool auto_blanked = false;
static volatile bool is_on = false;
static volatile uint8_t contrast = DT_PROP(VOXEL_NODE, contrast);
static volatile bool inverted = false;
static volatile uint32_t frame_counter = 0;

static inline uint8_t layer_index(uint8_t z) {
    return VOXEL_FLIP_Z ? (uint8_t)(VOXEL_DEPTH - 1 - z) : z;
}

/* ------------------------------------------------------------------------
 * Bank threads
 * ---------------------------------------------------------------------- */
static int bank_write(struct voxel_bank *b, const uint8_t *data, size_t len) {
    struct spi_buf buf = {.buf = (void *)data, .len = len};
    struct spi_buf_set set = {.buffers = &buf, .count = 1};
    int err = spi_write_dt(&b->spi, &set);
    if (err) {
        LOG_ERR("spi write failed (%d)", err);
    }
    return err;
}

static void bank_cs_all(struct voxel_bank *b, bool select) {
    for (uint8_t i = 0; i < b->cs_count; i++) {
        gpio_pin_set_dt(&b->cs[i], select ? 1 : 0);
    }
}

static void bank_thread_fn(void *p1, void *p2, void *p3) {
    struct voxel_bank *b = p1;
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);

    for (;;) {
        k_sem_take(&b->go, K_FOREVER);
        switch (b->job) {
        case JOB_CMD:
            gpio_pin_set_dt(&b->dc, 0);
            bank_cs_all(b, true);
            bank_write(b, cmd_buf, cmd_len);
            bank_cs_all(b, false);
            break;
        case JOB_CLEAR:
            /* one broadcast burst zeroes every panel of the bank */
            gpio_pin_set_dt(&b->dc, 1);
            bank_cs_all(b, true);
            bank_write(b, front_buffer[0], ZMK_VOXEL_LAYER_BYTES);
            bank_cs_all(b, false);
            break;
        case JOB_DATA:
            gpio_pin_set_dt(&b->dc, 1);
            for (uint8_t i = 0; i < b->cs_count; i++) {
                gpio_pin_set_dt(&b->cs[i], 1);
                bank_write(b, front_buffer[layer_index(b->z_offset + i)], ZMK_VOXEL_LAYER_BYTES);
                gpio_pin_set_dt(&b->cs[i], 0);
            }
            break;
        default:
            break;
        }
        k_sem_give(&done_sem);
    }
}

/* Run one job on every bank concurrently and wait for all of them. */
static void run_job(enum bank_job job) {
    for (size_t i = 0; i < BANK_COUNT; i++) {
        banks[i]->job = job;
        k_sem_give(&banks[i]->go);
    }
    for (size_t i = 0; i < BANK_COUNT; i++) {
        k_sem_take(&done_sem, K_FOREVER);
    }
}

static void broadcast(const uint8_t *cmds, size_t len) {
    if (len > sizeof(cmd_buf)) {
        len = sizeof(cmd_buf);
    }
    memcpy(cmd_buf, cmds, len);
    cmd_len = (uint8_t)len;
    run_job(JOB_CMD);
}

/* ------------------------------------------------------------------------
 * Frame thread
 * ---------------------------------------------------------------------- */
static void panel_power(bool on) {
    if (on == is_on) {
        return;
    }
    if (on) {
#if VOXEL_HAS_VPP
        gpio_pin_set_dt(&vpp_en, 1);
        k_sleep(K_MSEC(20));
#endif
        const uint8_t cmd = SH1107_DISPLAY_ON;
        broadcast(&cmd, 1);
        k_sleep(K_MSEC(100)); /* datasheet: wait 100 ms after AFh */
        is_on = true;
        atomic_set(&dirty, 1); /* re-send the current frame */
    } else {
        const uint8_t cmd = SH1107_DISPLAY_OFF;
        broadcast(&cmd, 1);
#if VOXEL_HAS_VPP
        gpio_pin_set_dt(&vpp_en, 0);
#endif
        is_on = false;
    }
}

static void send_frame(void) {
    static const uint8_t home[] = {SH1107_COL_LO(0), SH1107_COL_HI(0), SH1107_PAGE_ADDR(0)};
    k_mutex_lock(&back_lock, K_FOREVER);
    memcpy(front_buffer, back_buffer, sizeof(front_buffer));
    k_mutex_unlock(&back_lock);
    broadcast(home, sizeof(home)); /* every bank in the command phase */
    run_job(JOB_DATA);             /* then every bank streams its layers */
    frame_counter++;
}

static void hw_init(void) {
    /* Park every control line before any clock appears on the buses. */
    for (size_t i = 0; i < BANK_COUNT; i++) {
        struct voxel_bank *b = banks[i];
        for (uint8_t j = 0; j < b->cs_count; j++) {
            gpio_pin_configure_dt(&b->cs[j], GPIO_OUTPUT_INACTIVE);
        }
        gpio_pin_configure_dt(&b->dc, GPIO_OUTPUT_INACTIVE);
        if (b->reset.port) {
            gpio_pin_configure_dt(&b->reset, GPIO_OUTPUT_ACTIVE); /* hold in reset */
        }
    }
#if VOXEL_HAS_VPP
    gpio_pin_configure_dt(&vpp_en, GPIO_OUTPUT_INACTIVE);
    gpio_pin_set_dt(&vpp_en, 1);
#endif
    /* Datasheet: VDD and VPP up with RES low for >10 us, then release. */
    k_sleep(K_MSEC(10));
    for (size_t i = 0; i < BANK_COUNT; i++) {
        if (banks[i]->reset.port) {
            gpio_pin_set_dt(&banks[i]->reset, 0);
        }
    }
    k_sleep(K_MSEC(1));

    const uint8_t init[] = {
        SH1107_DISPLAY_OFF,
        SH1107_ADDR_MODE_VERT,
        SH1107_START_LINE, 0x00,
        SH1107_DISPLAY_OFFSET, 0x00,
        SH1107_MUX_RATIO, 0x7F,
        VOXEL_FLIP_X ? SH1107_SEG_REMAP_ON : SH1107_SEG_REMAP_OFF,
        VOXEL_FLIP_Y ? SH1107_COM_SCAN_DEC : SH1107_COM_SCAN_INC,
        SH1107_DCDC_MODE, SH1107_DCDC_OFF,
        SH1107_CLOCK_DIV, 0x50,
        SH1107_PRECHARGE, 0x22,
        SH1107_VCOM_DESELECT, 0x35,
        SH1107_CONTRAST, contrast,
        SH1107_ALL_ON_OFF,
        SH1107_NORMAL,
        SH1107_COL_LO(0), SH1107_COL_HI(0), SH1107_PAGE_ADDR(0),
    };
    broadcast(init, sizeof(init));
    run_job(JOB_CLEAR); /* front_buffer is still all zero here */

    if (want_on) {
        panel_power(true);
    }
    ready = true;
    LOG_INF("voxel display ready: %d layers in %d banks", VOXEL_DEPTH, (int)BANK_COUNT);
}

static void frame_thread_fn(void *p1, void *p2, void *p3) {
    ARG_UNUSED(p1);
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);

    hw_init();

    for (;;) {
        k_sem_take(&frame_sem, K_FOREVER);

        uint32_t work = (uint32_t)atomic_clear(&pending);
        if (work & PEND_CONTRAST) {
            const uint8_t cmd[] = {SH1107_CONTRAST, contrast};
            broadcast(cmd, sizeof(cmd));
        }
        if (work & PEND_INVERT) {
            const uint8_t cmd = inverted ? SH1107_INVERT : SH1107_NORMAL;
            broadcast(&cmd, 1);
        }
        if (work & PEND_POWER) {
            panel_power(want_on && !auto_blanked);
        }
        if (is_on && atomic_clear(&dirty)) {
            send_frame();
        }
    }
}

static inline void request(uint32_t flags) {
    atomic_or(&pending, flags);
    k_sem_give(&frame_sem);
}

/* ------------------------------------------------------------------------
 * Init
 * ---------------------------------------------------------------------- */
static int zmk_voxel_init(void) {
    uint8_t z = 0;
    for (size_t i = 0; i < BANK_COUNT; i++) {
        struct voxel_bank *b = banks[i];
        if (!spi_is_ready_dt(&b->spi)) {
            LOG_ERR("SPI bus for bank %d not ready", (int)i);
            return -ENODEV;
        }
        b->z_offset = z;
        z += b->cs_count;
        k_sem_init(&b->go, 0, 1);
        k_thread_create(&b->thread, b->stack, b->stack_size, bank_thread_fn, b, NULL, NULL,
                        K_PRIO_PREEMPT(CONFIG_ZMK_VOXEL_THREAD_PRIORITY), 0, K_NO_WAIT);
        k_thread_name_set(&b->thread, "voxel_bank");
    }
    k_thread_create(&frame_thread, frame_stack, K_THREAD_STACK_SIZEOF(frame_stack),
                    frame_thread_fn, NULL, NULL, NULL,
                    K_PRIO_PREEMPT(CONFIG_ZMK_VOXEL_THREAD_PRIORITY), 0, K_NO_WAIT);
    k_thread_name_set(&frame_thread, "voxel_frame");
    return 0;
}

SYS_INIT(zmk_voxel_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

#if IS_ENABLED(CONFIG_ZMK_VOXEL_BLANK_ON_IDLE)
static int voxel_activity_listener(const zmk_event_t *eh) {
    struct zmk_activity_state_changed *ev = as_zmk_activity_state_changed(eh);
    if (ev == NULL) {
        return ZMK_EV_EVENT_BUBBLE;
    }
    auto_blanked = ev->state != ZMK_ACTIVITY_ACTIVE;
    request(PEND_POWER);
    return ZMK_EV_EVENT_BUBBLE;
}
ZMK_LISTENER(zmk_voxel, voxel_activity_listener);
ZMK_SUBSCRIPTION(zmk_voxel, zmk_activity_state_changed);
#endif

/* ------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */
uint8_t zmk_voxel_depth(void) {
    return VOXEL_DEPTH;
}

void zmk_voxel_lock(void) {
    k_mutex_lock(&back_lock, K_FOREVER);
}

void zmk_voxel_unlock(void) {
    k_mutex_unlock(&back_lock);
}

void zmk_voxel_set(uint8_t x, uint8_t y, uint8_t z, bool on) {
    if (x >= ZMK_VOXEL_WIDTH || y >= ZMK_VOXEL_HEIGHT || z >= VOXEL_DEPTH) {
        return;
    }
#if VOXEL_SWAP_XY
    uint8_t t = x;
    x = y;
    y = t;
#endif
    uint8_t *p = &back_buffer[z][(uint16_t)y * ZMK_VOXEL_ROW_BYTES + (x >> 3)];
    uint8_t m = (uint8_t)(1u << (x & 7));
    if (on) {
        *p |= m;
    } else {
        *p &= (uint8_t)~m;
    }
}

bool zmk_voxel_get(uint8_t x, uint8_t y, uint8_t z) {
    if (x >= ZMK_VOXEL_WIDTH || y >= ZMK_VOXEL_HEIGHT || z >= VOXEL_DEPTH) {
        return false;
    }
#if VOXEL_SWAP_XY
    uint8_t t = x;
    x = y;
    y = t;
#endif
    return (back_buffer[z][(uint16_t)y * ZMK_VOXEL_ROW_BYTES + (x >> 3)] >> (x & 7)) & 1;
}

void zmk_voxel_clear(void) {
    memset(back_buffer, 0, sizeof(back_buffer));
}

void zmk_voxel_clear_layer(uint8_t z) {
    if (z < VOXEL_DEPTH) {
        memset(back_buffer[z], 0, ZMK_VOXEL_LAYER_BYTES);
    }
}

void zmk_voxel_fill_layer(uint8_t z, uint8_t pattern) {
    if (z < VOXEL_DEPTH) {
        memset(back_buffer[z], pattern, ZMK_VOXEL_LAYER_BYTES);
    }
}

uint8_t *zmk_voxel_layer_buffer(uint8_t z) {
    return z < VOXEL_DEPTH ? back_buffer[z] : NULL;
}

void zmk_voxel_flush(void) {
    atomic_set(&dirty, 1);
    k_sem_give(&frame_sem);
}

uint32_t zmk_voxel_frame_count(void) {
    return frame_counter;
}

bool zmk_voxel_is_ready(void) {
    return ready;
}

bool zmk_voxel_is_on(void) {
    return is_on;
}

void zmk_voxel_set_power(bool on) {
    want_on = on;
    request(PEND_POWER);
}

void zmk_voxel_toggle_power(void) {
    zmk_voxel_set_power(!want_on);
}

void zmk_voxel_set_contrast(uint8_t c) {
    contrast = c;
    request(PEND_CONTRAST);
}

uint8_t zmk_voxel_get_contrast(void) {
    return contrast;
}

void zmk_voxel_set_invert(bool invert) {
    inverted = invert;
    request(PEND_INVERT);
}
