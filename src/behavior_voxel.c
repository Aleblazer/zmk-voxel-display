/*
 * Copyright (c) 2026 Spencer Deven (@Aleblazer)
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT aleblazer_behavior_voxel

#include <zephyr/device.h>
#include <zephyr/logging/log.h>
#include <drivers/behavior.h>
#include <zmk/behavior.h>

#include <dt-bindings/zmk_voxel/voxel.h>
#include <zmk_voxel/voxel.h>

LOG_MODULE_DECLARE(zmk_voxel, CONFIG_ZMK_VOXEL_LOG_LEVEL);

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
static const struct behavior_parameter_value_metadata param_values[] = {
    {.display_name = "Toggle On/Off", .type = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE, .value = VX_TOG},
    {.display_name = "Turn On", .type = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE, .value = VX_ON},
    {.display_name = "Turn Off", .type = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE, .value = VX_OFF},
    {.display_name = "Next Animation", .type = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE, .value = VX_NEXT},
    {.display_name = "Contrast Up", .type = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE, .value = VX_BRIU},
    {.display_name = "Contrast Down", .type = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE, .value = VX_BRID},
    {.display_name = "Clear", .type = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE, .value = VX_CLR},
};

static const struct behavior_parameter_metadata_set param_set = {
    .param1_values = param_values,
    .param1_values_len = ARRAY_SIZE(param_values),
};

static const struct behavior_parameter_metadata metadata = {
    .sets_len = 1,
    .sets = &param_set,
};
#endif

static int on_keymap_binding_pressed(struct zmk_behavior_binding *binding,
                                     struct zmk_behavior_binding_event event) {
    switch (binding->param1) {
    case VX_TOG:
        zmk_voxel_toggle_power();
        break;
    case VX_ON:
        zmk_voxel_set_power(true);
        break;
    case VX_OFF:
        zmk_voxel_set_power(false);
        break;
    case VX_NEXT:
#if IS_ENABLED(CONFIG_ZMK_VOXEL_ANIM)
        zmk_voxel_anim_next();
#endif
        break;
    case VX_BRIU: {
        uint16_t c = zmk_voxel_get_contrast() + 16;
        zmk_voxel_set_contrast(c > 255 ? 255 : (uint8_t)c);
        break;
    }
    case VX_BRID: {
        uint8_t c = zmk_voxel_get_contrast();
        zmk_voxel_set_contrast(c < 16 ? 0 : (uint8_t)(c - 16));
        break;
    }
    case VX_CLR:
        zmk_voxel_lock();
        zmk_voxel_clear();
        zmk_voxel_unlock();
        zmk_voxel_flush();
        break;
    default:
        LOG_ERR("Unknown voxel command: %d", binding->param1);
        return -ENOTSUP;
    }
    return ZMK_BEHAVIOR_OPAQUE;
}

static int on_keymap_binding_released(struct zmk_behavior_binding *binding,
                                      struct zmk_behavior_binding_event event) {
    return ZMK_BEHAVIOR_OPAQUE;
}

static const struct behavior_driver_api behavior_voxel_driver_api = {
    .binding_pressed = on_keymap_binding_pressed,
    .binding_released = on_keymap_binding_released,
    .locality = BEHAVIOR_LOCALITY_CENTRAL,
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
    .parameter_metadata = &metadata,
#endif
};

#define VOXEL_BEHAVIOR_INST(n)                                                                     \
    BEHAVIOR_DT_INST_DEFINE(n, NULL, NULL, NULL, NULL, POST_KERNEL,                                \
                            CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &behavior_voxel_driver_api);

DT_INST_FOREACH_STATUS_OKAY(VOXEL_BEHAVIOR_INST)

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
