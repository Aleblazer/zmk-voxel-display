# zmk-voxel-display

ZMK module that drives a stack of transparent SH1107 128x128 OLED panels as a
128 x 128 x N voxel volume. Built for a **dongle**: the nRF52840 that acts as
the split central hosts the volume and reacts to keys from any connected
keyboard.

* Panels are grouped into **banks**, one bank per SPI bus. Banks transfer in
  parallel, so two buses halve the refresh time. On an nRF52840 each SPIM
  instance can run the panels' datasheet-maximum 4 MHz; five panels per bus
  refresh the whole volume in about 20 ms.
* Each panel runs in SH1107 **vertical addressing mode**: after a 3-byte
  address reset, one contiguous 2048-byte EasyDMA write covers the whole panel.
* Everything runs in dedicated low-priority threads. BLE and USB HID are not
  delayed by display traffic.
* Built-in animations (keypress glyphs flying rear to front, a retro parallax
  side-scroller, a bouncing block) and a `&voxel` behavior to control them.

Targets ZMK `main` (Zephyr 4.1).

## Adding the module

`config/west.yml`:

```yaml
manifest:
  remotes:
    - name: zmkfirmware
      url-base: https://github.com/zmkfirmware
    - name: aleblazer
      url-base: https://github.com/Aleblazer
  projects:
    - name: zmk
      remote: zmkfirmware
      revision: main
      import: app/west.yml
    - name: zmk-voxel-display
      remote: aleblazer
      revision: main
  self:
    path: config
```

## Wiring

Every panel gets the same hookup except its chip select:

| Panel pin | Name | Connect to |
|---|---|---|
| 1, 9, 12 | VSS | GND |
| 2 | SI | MOSI of its bank's SPI bus |
| 3 | SCL | SCK of its bank's SPI bus |
| 4 | A0 | D/C (may be shared by all panels) |
| 5 | RES | RESET (may be shared by all panels) |
| 6 | CS | its own GPIO |
| 7 | IREF | 1 MΩ to GND |
| 8 | VDD | 3.3 V |
| 10 | VCOMH | 4.7 µF/25 V to GND |
| 11 | VPP | external 13.5 V (7-16.5 V), 4.7 µF local |

The SH1107's internal DC-DC is switched off at init (`0xAD 0x8A`); VPP must
come from an external supply. Budget about 1.3 mA per panel on VPP. An
optional `vpp-en-gpios` line lets the driver follow the datasheet power
sequence (VPP on with RESET held low, display off before VPP off).

D/C and RESET can be shared across banks: the driver keeps every bank in the
same phase (all banks send commands, then all banks stream data).

## Devicetree

Two bank nodes, each a child of an SPI controller, plus one top-level node
that lists them in z order (front-most panel first):

```dts
&spi1 {
    status = "okay";
    pinctrl-0 = <&voxel_spi1_default>;
    pinctrl-1 = <&voxel_spi1_sleep>;
    pinctrl-names = "default", "sleep";

    voxel_bank_a: voxel_bank@0 {
        compatible = "aleblazer,voxel-sh1107-bank";
        reg = <0>;
        spi-max-frequency = <4000000>;
        panel-cs-gpios = <&pro_micro 0 GPIO_ACTIVE_LOW>, <&pro_micro 1 GPIO_ACTIVE_LOW>,
                         <&pro_micro 2 GPIO_ACTIVE_LOW>, <&pro_micro 3 GPIO_ACTIVE_LOW>,
                         <&pro_micro 4 GPIO_ACTIVE_LOW>;
        dc-gpios = <&pro_micro 15 GPIO_ACTIVE_HIGH>;
        reset-gpios = <&pro_micro 20 GPIO_ACTIVE_LOW>;
    };
};

&spi0 { /* second bank, same shape, panels z5..z9 */ };

/ {
    voxel_volume {
        compatible = "aleblazer,voxel-sh1107";
        banks = <&voxel_bank_a &voxel_bank_b>;
        vpp-en-gpios = <&pro_micro 21 GPIO_ACTIVE_HIGH>;
        contrast = <0xA2>;
        /* flip-x; flip-y; flip-z; swap-xy; */
    };
};
```

The `voxel_volume` shield in this module wires exactly this on a Pro Micro
footprint nRF52840 (nice!nano and friends): `shield: my_dongle voxel_volume`.

### Kconfig

| Option | Default | |
|---|---|---|
| `ZMK_VOXEL_DISPLAY` | y when the node exists | driver |
| `ZMK_VOXEL_ANIM` | y | built-in animations |
| `ZMK_VOXEL_ANIM_DEFAULT` | 1 | 0 off, 1 keyfall, 2 parallax, 3 bounce |
| `ZMK_VOXEL_ANIM_FRAME_MS` | 33 | drawing cadence |
| `ZMK_VOXEL_BLANK_ON_IDLE` | y | blank on ZMK idle, wake on activity |
| `ZMK_VOXEL_THREAD_PRIORITY` | 10 | transfer thread priority |

## Behavior

```dts
#include <behaviors/voxel.dtsi>
/* ... */
&voxel VX_TOG   &voxel VX_NEXT   &voxel VX_BRIU   &voxel VX_BRID   &voxel VX_CLR
```

## API

```c
#include <zmk_voxel/voxel.h>

zmk_voxel_lock();
zmk_voxel_clear();
zmk_voxel_set(x, y, z, true);      // x, y 0-127; z 0 = front
zmk_voxel_unlock();
zmk_voxel_flush();                 // queue the back buffer for transfer
zmk_voxel_set_power(false);
zmk_voxel_set_contrast(0xA2);
zmk_voxel_anim_set(ZMK_VOXEL_ANIM_OFF);   // take over drawing yourself
```

The back buffer is a plain 1-bpp row-major bitmap per layer, 16 bytes per row,
bit 0 = leftmost pixel of each byte.

## License

MIT. The 5x7 font is hand-drawn for this module.
