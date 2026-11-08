# Local patch to the vendored `espressif/es8311` component

**Upstream**: `espressif/es8311` v1.0.0~1, Apache-2.0
(`https://components-file.espressif.com/components/espressif/es8311/1.0.0~1/espressif__es8311-v1.0.0_1.zip`)

## Why the component is vendored instead of pulled by the component manager

Two reasons:

1. The ES8311 datasheet does **not** publish a register map (the public PDF covers
   block diagram, pinout, typical application, clock modes, I2C timing and
   electricals only). A hand-written init table would be guesswork, so the
   official driver is the right source.
2. Vendoring keeps the build reproducible and offline-capable — no dependency on
   `components.espressif.com` at build time.

## The one functional change

Upstream binds to the **legacy** I2C driver through an `i2c_port_t`:

```c
typedef struct { i2c_port_t port; uint16_t dev_addr; } es8311_dev_t;
...
i2c_master_write_to_device(es->port, es->dev_addr, ...);
i2c_master_write_read_device(es->port, es->dev_addr, ...);
es8311_handle_t es8311_create(const i2c_port_t port, const uint16_t dev_addr);
```

`board_hw.c` owns I2C0 with the **new** `i2c_master` driver (needed for
`i2c_master_probe`-style scanning and runtime speed/wiring autodetect), and
ESP-IDF does not allow the legacy and new drivers to share a port. So the device
is identified by an `i2c_master` device handle instead:

```c
typedef struct { i2c_master_dev_handle_t i2c_dev; } es8311_dev_t;
...
i2c_master_transmit(es->i2c_dev, write_buf, 2, 1000);
i2c_master_transmit_receive(es->i2c_dev, &reg_addr, 1, reg_value, 1, 1000);
es8311_handle_t es8311_create(i2c_master_dev_handle_t i2c_dev);
```

Files touched: `es8311.c` (include block, `es8311_dev_t`, the two register
helpers, `es8311_create`) and `include/es8311.h` (include + the `es8311_create`
declaration and its doc comment). **All register tables and logic are untouched
upstream code.**

Caller side:

```c
i2c_master_dev_handle_t dev;
board_i2c_add_device(BOARD_ES8311_I2C_ADDR /* 0x18 */, 0, &dev);
es8311_handle_t codec = es8311_create(dev);
```

## Updating to a newer upstream release

Re-extract the zip over this directory and re-apply the four changes above
(the patch is ~20 lines and confined to the I2C binding), or switch to the
component manager and drop `board_hw`'s new-driver I2C in favour of the legacy
driver.
