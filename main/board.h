#pragma once

/*
 * The one place that knows which board this is. One board per IDF target:
 *
 *   esp32s3  Waveshare ESP32-S3-Touch-AMOLED-1.75C   466x466 AMOLED, CO5300 over QSPI
 *   esp32p4  Waveshare ESP32-P4-WIFI6-Touch-LCD-3.4C 800x800 IPS, JD9365 over MIPI-DSI
 *
 * Include this, not a BSP's board header by name. The generic BSP headers
 * (bsp/esp-bsp.h, bsp/display.h, bsp/touch.h) are fine to include directly:
 * both BSPs ship them with the same names.
 *
 * What differs between the boards is said here as a capability, so code asks
 * "does this board have X" rather than "is this the S3". Things that already
 * have a Kconfig switch (the battery: CONFIG_BATTERY) stay there instead.
 */

#include "sdkconfig.h"

#if CONFIG_IDF_TARGET_ESP32S3
#include "bsp/esp32_s3_touch_amoled_1_75c.h"

/* The panel is a CO5300 on a QSPI bus: raw commands go through the panel IO,
 * and the bus has a chip-select pin that must be held high in sleep. */
#define BOARD_PANEL_CO5300 1

/* Dynamic frequency scaling floor. 80 MHz keeps the APB clock, and with it the
 * console UART and peripheral timings, where they were built; see main.c. */
#define BOARD_PM_MIN_FREQ_MHZ 80

/* The BOOT button: the chip's download-mode strapping pin, so every board with
 * this chip has it on the same GPIO. */
#define BOARD_BOOT_GPIO 0

/* The orb clock's frame period on external power (on battery it is ui.c's
 * 5 fps). A clock frame costs 25-60 ms here, so 10 fps leaves the LVGL core
 * free for a session starting under it; 30 would keep it busy. */
#define BOARD_CLOCK_EXT_FRAME_MS 100

#elif CONFIG_IDF_TARGET_ESP32P4
#include "bsp/esp32_p4_wifi6_touch_lcd_xc.h"

/* JD9365 over MIPI-DSI: no QSPI bus, no CO5300 command set. */
#define BOARD_PANEL_CO5300 0

/* No frequency scaling yet: the floor is the ceiling. Rev 1.x silicon only
 * runs at 360, 180 or 90 MHz, and what dropping the clock does under a DSI
 * panel refreshing from PSRAM has not been measured. */
#define BOARD_PM_MIN_FREQ_MHZ CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ

/* BOOT is the P4's strapping pin, GPIO 35 -- not 0, which is a plain GPIO on
 * this chip. The P4 BSP declares no buttons (BSP_CAPS_BUTTONS 0). */
#define BOARD_BOOT_GPIO 35

/* The orb clock on external power, which on this board, having no battery, is
 * always. 30 fps was tried (2026-11-17): a clock frame at 800x800 draws in
 * 36-37 ms, and with the flush the panel managed 12.7 fps with the LVGL task
 * saturated. 10 fps, as on the S3, leaves it headroom. */
#define BOARD_CLOCK_EXT_FRAME_MS 100

#else
#error "No board for this IDF target. See main/board.h."
#endif

/* Square panels, both of them: the panel's side in pixels. */
#define BOARD_PANEL_SIZE BSP_LCD_H_RES

/* The drawn screens are laid out for PANEL_SIZE, which main/CMakeLists.txt sets
 * per target (see ui_scale.h). The BSP knows the real panel; they must agree. */
_Static_assert(BSP_LCD_H_RES == PANEL_SIZE && BSP_LCD_V_RES == PANEL_SIZE,
               "PANEL_SIZE in main/CMakeLists.txt does not match this board's panel");
