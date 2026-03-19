/*
 * core_video.c  —  CORE VIDEO firmware
 * Target: RP2350B
 *
 * Hardware (from schematic):
 *   HSTX  GPIO12–GPIO19  → TMDS pairs via 270Ω + 0.1µF AC caps → HDMI
 *   UART  GPIO0=TX  GPIO1=RX  → CORE MAIN (921600 baud)
 *   QSPI  GPIO26–GPIO31  → W25Q128 flash
 *   SRAM  23LC1024 on SPI1 (GPIO8=SCK, GPIO9=MOSI, GPIO10=MISO, GPIO11=CS)
 *
 * Supported display modes (CMD_SET_MODE):
 *   0x01  320×240  @ 60fps  8bit RGB332   single buffer
 *   0x02  640×480  @ 60fps  8bit RGB332   single buffer
 *   0x03  320×240  @ 30fps  8bit  double buffer (swap on CMD_SWAP_BUFFER)
 *   0x04  320×240  @ 30fps  16bit double buffer (swap on CMD_SWAP_BUFFER)
 */

#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/uart.h"
#include "hardware/spi.h"
#include "hardware/dma.h"
#include "hardware/clocks.h"
#include "hardware/structs/hstx_ctrl.h"
#include "hardware/structs/hstx_fifo.h"
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include "shared_protocol.h"

/* ================================================================== */
/* Pin definitions                                                      */
/* ================================================================== */
#define UART_TX_PIN         0
#define UART_RX_PIN         1
#define UART_ID             uart0

/* HSTX base pin — GPIO12 is HSTX D0+ in the SDK */
#define HSTX_BASE_PIN       12

/* ================================================================== */
/* Display geometry                                                     */
/* ================================================================== */
#define MAX_W   640
#define MAX_H   480

/* ================================================================== */
/* Framebuffer                                                          */
/* ================================================================== */
/*
 * Layout in 520KB SRAM:
 *   Mode 0x01  320×240 × 1 byte  =  75KB   (single)
 *   Mode 0x02  640×480 × 1 byte  = 300KB   (single)
 *   Mode 0x03  320×240 × 1 byte  =  75KB × 2 = 150KB (double)
 *   Mode 0x04  320×240 × 2 bytes = 150KB × 2 = 300KB (double)
 *
 * We statically allocate the largest case (300KB double 16bit) and
 * reinterpret the buffer for other modes.
 */
static uint8_t  fb8[2][320 * 240];      /* 8-bit double buffer  150KB */
static uint16_t fb16[2][320 * 240];     /* 16-bit double buffer 150KB */
/* 640×480 8-bit single — overlaid on fb8[0] memory, separate pointer */
static uint8_t  fb_large[640 * 480];    /* 300KB single buffer  */

/* active mode */
static uint8_t  current_mode    = MODE_320x240_8BIT;
static uint8_t  draw_buf        = 0;    /* which buffer we draw into */
static uint8_t  disp_buf        = 0;    /* which buffer HSTX reads  */
static volatile bool swap_pending = false;

/* foreground colour */
static uint8_t  fg_r = 0xFF, fg_g = 0xFF, fg_b = 0xFF;

/* ================================================================== */
/* Helpers: get pointer / stride for current draw buffer               */
/* ================================================================== */
static inline void *draw_ptr(void)
{
    switch (current_mode) {
        case MODE_320x240_8BIT:  return fb8[0];
        case MODE_640x480_8BIT:  return fb_large;
        case MODE_320x240_DB8:   return fb8[draw_buf];
        case MODE_320x240_DB16:  return fb16[draw_buf];
        default:                 return fb8[0];
    }
}

static inline int draw_w(void)
{
    return (current_mode == MODE_640x480_8BIT) ? 640 : 320;
}

static inline int draw_h(void)
{
    return (current_mode == MODE_640x480_8BIT) ? 480 : 240;
}

static inline bool is_16bit(void)
{
    return current_mode == MODE_320x240_DB16;
}

/* Convert r,g,b (0–255 each) to the pixel value for current mode */
static inline uint16_t make_pixel(uint8_t r, uint8_t g, uint8_t b)
{
    if (is_16bit()) {
        /* RGB565 */
        return ((uint16_t)(r & 0xF8) << 8) |
               ((uint16_t)(g & 0xFC) << 3) |
               (b >> 3);
    } else {
        /* RGB332: RRR GGG BB */
        return ((r & 0xE0)) | ((g & 0xE0) >> 3) | (b >> 6);
    }
}

/* ================================================================== */
/* Built-in 8×8 font (ASCII 32–127)                                    */
/* Minimal subset — full table stored in flash via const               */
/* ================================================================== */
#include "font8x8.h"   /* generated separately — see font8x8.h below  */

/* ================================================================== */
/* Draw primitives                                                      */
/* ================================================================== */
static void put_pixel(int x, int y, uint16_t colour)
{
    int w = draw_w(), h = draw_h();
    if (x < 0 || y < 0 || x >= w || y >= h) return;
    if (is_16bit()) {
        ((uint16_t *)draw_ptr())[y * w + x] = colour;
    } else {
        ((uint8_t *)draw_ptr())[y * w + x]  = (uint8_t)colour;
    }
}

static void draw_line(int x0, int y0, int x1, int y1, uint16_t col)
{
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    while (1) {
        put_pixel(x0, y0, col);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

static void fill_rect(int x, int y, int w, int h, uint16_t col)
{
    int dw = draw_w(), dh = draw_h();
    for (int row = y; row < y + h && row < dh; row++)
        for (int col2 = x; col2 < x + w && col2 < dw; col2++)
            put_pixel(col2, row, col);
}

static void clear_screen(uint16_t col)
{
    int total = draw_w() * draw_h();
    if (is_16bit()) {
        uint16_t *p = (uint16_t *)draw_ptr();
        for (int i = 0; i < total; i++) p[i] = col;
    } else {
        memset(draw_ptr(), (uint8_t)col, total);
    }
}

static void draw_char(int x, int y, char ch, uint16_t col)
{
    if (ch < 32 || ch > 127) ch = '?';
    const uint8_t *glyph = font8x8_basic[(uint8_t)ch - 32];
    for (int row = 0; row < 8; row++) {
        uint8_t bits = glyph[row];
        for (int col2 = 0; col2 < 8; col2++) {
            if (bits & (1 << col2))
                put_pixel(x + col2, y + row, col);
        }
    }
}

static void draw_text(int x, int y, const char *str, uint8_t len, uint16_t col)
{
    int cx = x;
    for (uint8_t i = 0; i < len; i++) {
        draw_char(cx, y, str[i], col);
        cx += 8;
        if (cx + 8 > draw_w()) break;
    }
}

/* ================================================================== */
/* HSTX / DVI output                                                   */
/* ================================================================== */
/*
 * We use the pico-sdk picodvi-compatible HSTX driver.
 * For full DVI at 640x480x60 the TMDS clock is 25.175MHz * 10 = 251.75MHz.
 * RP2350 system clock is set to 252MHz for exact pixel clock.
 * For 320x240 we output at 640x480 by pixel-doubling in the scanline ISR.
 *
 * The actual HSTX setup uses the Raspberry Pi pico_scanvideo or
 * hardware_hstx APIs. Here we integrate with the pico-dvhstx library
 * (https://github.com/Wren6991/PicoDVI) which handles TMDS encoding.
 */
#include "dvi.h"           /* from PicoDVI library */
#include "dvi_serialiser.h"

#define TMDS_CLOCK_KHZ      252000   /* 252MHz system clock for 640x480x60 */

static struct dvi_inst dvi0;

/* DVI timing config for 640×480 60Hz */
static const struct dvi_timing dvi_timing_640x480_60hz = {
    .h_sync_polarity   = false,
    .h_front_porch     = 16,
    .h_sync_width      = 96,
    .h_back_porch      = 48,
    .h_active_pixels   = 640,
    .v_sync_polarity   = false,
    .v_front_porch     = 10,
    .v_sync_width      = 2,
    .v_back_porch      = 33,
    .v_active_lines    = 480,
    .bit_clk_khz       = TMDS_CLOCK_KHZ
};

/* Scanline callback — called by DVI ISR for each line */
static void __not_in_flash_func(render_scanline)(uint y, uint32_t *tmds_buf)
{
    const uint8_t  *fb8_ptr  = NULL;
    const uint16_t *fb16_ptr = NULL;
    int src_y = y;

    switch (current_mode) {
        case MODE_320x240_8BIT:
            /* pixel-double vertically: two output lines per source line */
            src_y = y / 2;
            fb8_ptr = fb8[0] + src_y * 320;
            tmds_encode_palette_pixel_doubled(tmds_buf, fb8_ptr, 640);
            break;

        case MODE_640x480_8BIT:
            fb8_ptr = fb_large + y * 640;
            tmds_encode_palette(tmds_buf, fb8_ptr, 640);
            break;

        case MODE_320x240_DB8:
            src_y   = y / 2;
            fb8_ptr = fb8[disp_buf] + src_y * 320;
            tmds_encode_palette_pixel_doubled(tmds_buf, fb8_ptr, 640);
            /* check for pending swap at end of frame */
            if (y == 479 && swap_pending) {
                disp_buf    = draw_buf;
                swap_pending = false;
            }
            break;

        case MODE_320x240_DB16:
            src_y    = y / 2;
            fb16_ptr = fb16[disp_buf] + src_y * 320;
            tmds_encode_rgb565_pixel_doubled(tmds_buf, fb16_ptr, 640);
            if (y == 479 && swap_pending) {
                disp_buf    = draw_buf;
                swap_pending = false;
            }
            break;

        default:
            /* blank line */
            memset(tmds_buf, 0, 640 * 4);
            break;
    }
}

static void init_display(uint8_t mode)
{
    current_mode = mode;
    draw_buf     = 0;
    disp_buf     = 0;
    swap_pending = false;

    /* init DVI if not already running */
    static bool dvi_started = false;
    if (!dvi_started) {
        set_sys_clock_khz(TMDS_CLOCK_KHZ, true);

        dvi0.timing         = &dvi_timing_640x480_60hz;
        dvi0.ser_cfg        = DVI_DEFAULT_SERIAL_CONFIG;
        dvi0.scanline_callback = render_scanline;
        dvi_init(&dvi0, next_striped_spin_lock_num(), next_striped_spin_lock_num());

        /* DVI runs on core 1 */
        multicore_launch_core1(dvi_scanbuf_main_8bpp);
        dvi_start(&dvi0);
        dvi_started = true;
    }
}

/* ================================================================== */
/* UART command receiver                                               */
/* ================================================================== */
static uint8_t uart_rx(void)
{
    return uart_getc(UART_ID);
}

static uint16_t uart_rx16(void)
{
    uint16_t lo = uart_rx();
    uint16_t hi = uart_rx();
    return lo | (hi << 8);
}

static void uart_tx(uint8_t b)
{
    uart_putc_raw(UART_ID, b);
}

static void send_ack(void)  { uart_tx(PROTO_ACK); }
static void send_nak(void)  { uart_tx(PROTO_NAK); }

static void process_command(uint8_t cmd)
{
    uint16_t col;

    switch (cmd) {

        case CMD_PING:
            send_ack();
            break;

        case CMD_GET_MODE:
            uart_tx(PROTO_DATA);
            uart_tx(1); uart_tx(0);      /* length = 1 */
            uart_tx(current_mode);
            break;

        case CMD_SET_MODE: {
            uint8_t mode = uart_rx();
            if (mode < MODE_320x240_8BIT || mode > MODE_320x240_DB16) {
                send_nak();
                break;
            }
            init_display(mode);
            send_ack();
            break;
        }

        case CMD_SET_COLOR:
            fg_r = uart_rx();
            fg_g = uart_rx();
            fg_b = uart_rx();
            send_ack();
            break;

        case CMD_WRITE_PIXEL: {
            int x = (int16_t)uart_rx16();
            int y = (int16_t)uart_rx16();
            put_pixel(x, y, make_pixel(fg_r, fg_g, fg_b));
            send_ack();
            break;
        }

        case CMD_WRITE_LINE: {
            int x0 = (int16_t)uart_rx16();
            int y0 = (int16_t)uart_rx16();
            int x1 = (int16_t)uart_rx16();
            int y1 = (int16_t)uart_rx16();
            draw_line(x0, y0, x1, y1, make_pixel(fg_r, fg_g, fg_b));
            send_ack();
            break;
        }

        case CMD_FILL_RECT: {
            int x = (int16_t)uart_rx16();
            int y = (int16_t)uart_rx16();
            int w = (int16_t)uart_rx16();
            int h = (int16_t)uart_rx16();
            fill_rect(x, y, w, h, make_pixel(fg_r, fg_g, fg_b));
            send_ack();
            break;
        }

        case CMD_CLEAR_SCREEN:
            clear_screen(make_pixel(fg_r, fg_g, fg_b));
            send_ack();
            break;

        case CMD_BLIT: {
            int x = (int16_t)uart_rx16();
            int y = (int16_t)uart_rx16();
            int w = (int16_t)uart_rx16();
            int h = (int16_t)uart_rx16();
            int bpp = is_16bit() ? 2 : 1;
            int dw  = draw_w(), dh = draw_h();
            for (int row = 0; row < h; row++) {
                for (int c = 0; c < w; c++) {
                    uint16_t pix;
                    if (bpp == 2) {
                        uint16_t lo = uart_rx();
                        uint16_t hi = uart_rx();
                        pix = lo | (hi << 8);
                    } else {
                        pix = uart_rx();
                    }
                    if (x + c < dw && y + row < dh)
                        put_pixel(x + c, y + row, pix);
                }
            }
            send_ack();
            break;
        }

        case CMD_SWAP_BUFFER:
            if (current_mode == MODE_320x240_DB8 ||
                current_mode == MODE_320x240_DB16) {
                /* flip draw buffer, flag display swap for next vblank */
                draw_buf     = 1 - draw_buf;
                swap_pending = true;
                send_ack();
            } else {
                send_nak();
            }
            break;

        case CMD_DRAW_CHAR: {
            int x   = (int16_t)uart_rx16();
            int y   = (int16_t)uart_rx16();
            char ch = (char)uart_rx();
            draw_char(x, y, ch, make_pixel(fg_r, fg_g, fg_b));
            send_ack();
            break;
        }

        case CMD_DRAW_TEXT: {
            int x      = (int16_t)uart_rx16();
            int y      = (int16_t)uart_rx16();
            uint8_t ln = uart_rx();
            char buf[256];
            for (uint8_t i = 0; i < ln; i++) buf[i] = (char)uart_rx();
            buf[ln] = '\0';
            draw_text(x, y, buf, ln, make_pixel(fg_r, fg_g, fg_b));
            send_ack();
            break;
        }

        default:
            send_nak();
            break;
    }
}

/* ================================================================== */
/* Main                                                                 */
/* ================================================================== */
int main(void)
{
    /* UART0: TX=GPIO0, RX=GPIO1, 921600 baud */
    uart_init(UART_ID, PROTO_BAUD);
    gpio_set_function(UART_TX_PIN, GPIO_FUNC_UART);
    gpio_set_function(UART_RX_PIN, GPIO_FUNC_UART);
    uart_set_hw_flow(UART_ID, false, false);
    uart_set_format(UART_ID, 8, 1, UART_PARITY_NONE);

    /* default mode */
    init_display(MODE_320x240_8BIT);

    /* signal ready */
    send_ack();

    /* command loop — runs on core 0, DVI runs on core 1 */
    while (true) {
        uint8_t cmd = uart_rx();
        process_command(cmd);
    }
}
