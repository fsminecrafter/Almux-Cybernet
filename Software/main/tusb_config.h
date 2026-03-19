/*
 * tusb_config.h  —  TinyUSB host configuration for CORE MAIN
 * RP2350B, USB on GPIO24/25
 */
#pragma once

/* host mode only */
#define CFG_TUSB_RHPORT0_MODE   OPT_MODE_HOST

/* controller */
#define CFG_TUSB_MCU            OPT_MCU_RP2040   /* RP2350 uses same driver */

/* enable HID host class for keyboard/mouse */
#define CFG_TUH_HID             4
#define CFG_TUH_HUB             1
#define CFG_TUH_CDC             0
#define CFG_TUH_MSC             0
#define CFG_TUH_VENDOR          0

/* FIFO size */
#define CFG_TUH_MEM_SECTION
#define CFG_TUH_MEM_ALIGN       __attribute__((aligned(4)))
