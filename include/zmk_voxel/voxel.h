/*
 * Copyright (c) 2026 Spencer Deven (@Aleblazer)
 * SPDX-License-Identifier: MIT
 *
 * Volumetric display: a stack of SH1107 128x128 OLED panels driven as a
 * 128 x 128 x N voxel volume over one or more SPI buses.
 *
 * Framebuffer layout per layer z: 128 rows (y) of 16 bytes, each byte holding
 * 8 horizontally adjacent pixels (x), bit 0 = lowest x. A plain 1-bpp
 * row-major bitmap, 16 bytes per row.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

#define ZMK_VOXEL_WIDTH 128
#define ZMK_VOXEL_HEIGHT 128
#define ZMK_VOXEL_ROW_BYTES (ZMK_VOXEL_WIDTH / 8)
#define ZMK_VOXEL_LAYER_BYTES (ZMK_VOXEL_HEIGHT * ZMK_VOXEL_ROW_BYTES) /* 2048 */

/* Number of layers, from the devicetree (sum of every bank's panels). */
uint8_t zmk_voxel_depth(void);

/* --- drawing (call between zmk_voxel_lock / zmk_voxel_unlock) ------------ */
void zmk_voxel_lock(void);
void zmk_voxel_unlock(void);
void zmk_voxel_set(uint8_t x, uint8_t y, uint8_t z, bool on);
bool zmk_voxel_get(uint8_t x, uint8_t y, uint8_t z);
void zmk_voxel_clear(void);
void zmk_voxel_clear_layer(uint8_t z);
void zmk_voxel_fill_layer(uint8_t z, uint8_t pattern);
uint8_t *zmk_voxel_layer_buffer(uint8_t z); /* raw 2048-byte back buffer */
void zmk_voxel_flush(void);                 /* queue the back buffer for transfer */
uint32_t zmk_voxel_frame_count(void);

/* --- panel control ------------------------------------------------------ */
bool zmk_voxel_is_ready(void); /* panels initialised */
bool zmk_voxel_is_on(void);
void zmk_voxel_set_power(bool on);
void zmk_voxel_toggle_power(void);
void zmk_voxel_set_contrast(uint8_t contrast);
uint8_t zmk_voxel_get_contrast(void);
void zmk_voxel_set_invert(bool invert);

/* --- built-in animations (CONFIG_ZMK_VOXEL_ANIM) ------------------------ */
enum zmk_voxel_anim {
    ZMK_VOXEL_ANIM_OFF = 0,
    ZMK_VOXEL_ANIM_KEYFALL,
    ZMK_VOXEL_ANIM_PARALLAX,
    ZMK_VOXEL_ANIM_BOUNCE,
    ZMK_VOXEL_ANIM_COUNT
};

void zmk_voxel_anim_set(enum zmk_voxel_anim mode);
enum zmk_voxel_anim zmk_voxel_anim_get(void);
void zmk_voxel_anim_next(void);
/* Inject a key event (HID keyboard-page usage id). Fed automatically from
 * ZMK keycode events; exposed for custom sources. */
void zmk_voxel_anim_key_event(uint16_t usage_id, bool pressed);
