# Copyright (c) 2026 Spencer Deven (@Aleblazer)
# SPDX-License-Identifier: MIT

if(CONFIG_ZMK_VOXEL_DISPLAY)
  zephyr_library()

  zephyr_library_sources(src/voxel.c src/font5x7.c)
  zephyr_library_sources_ifdef(CONFIG_ZMK_VOXEL_ANIM src/voxel_anim.c)
  zephyr_library_sources_ifdef(CONFIG_ZMK_VOXEL_BEHAVIOR src/behavior_voxel.c)

  zephyr_include_directories(include)
  # ZMK's own headers (events, behaviors, keymap)
  zephyr_include_directories(${APPLICATION_SOURCE_DIR}/include)
endif()
