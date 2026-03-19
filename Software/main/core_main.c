/*
 * core_main.c  —  CORE MAIN firmware
 * Target: RP2350B
 *
 * Hardware (from schematic):
 *   UART  GPIO38=TX(MTX)  GPIO39=RX(MRX)  → CORE VIDEO  (921600 baud)
 *   USB   GPIO24=USB_DM   GPIO25=USB_DP   → USB Host via USB2-USB-226-BRY
 *   ETH   GPIO2=RXD GPIO3=TXD GPIO6=LINK  → USR-K6 Ethernet module (UART-ETH bridge)
 *   SWD   GPIO22=SWCLK    GPIO23=SWDIO    → debug header
 *   UARTSEN → ATmega CONNECT header
 *
 * This file contains:
 *   1. Graphics API  (gfx_*)   — sends commands to CORE VIDEO over UART
 *   2. Ethernet API  (eth_*)   — communicates with USR-K6 UART-ETH module
 *   3. USB Host API  (usb_*)   — TinyUSB host stack
 *   4. user_main()             — YOUR PROGRAM GOES HERE
 *      Four example modes are demonstrated.
 */

#include "pico/stdlib.h"
#include "hardware/uart.h"
#include "hardware/gpio.h"
#include "hardware/clocks.h"
#include "pico/time.h"
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include "shared_protocol.h"

/* TinyUSB host */
#include "tusb.h"

/* ================================================================== */
/* Pin definitions (from schematic)                                    */
/* ================================================================== */
#define VID_UART_ID          uart1          /* GPIO38/39 = UART1 */
#define VID_UART_TX          38
#define VID_UART_RX          39

#define ETH_UART_ID          uart0          /* GPIO2/3 = UART0   */
#define ETH_UART_TX          3
#define ETH_UART_RX          2

#define USB_DM_PIN           24
#define USB_DP_PIN           25

/* ================================================================== */
/* ------------------------------------------------------------------ */
/* SECTION 1 — GRAPHICS API                                            */
/* ------------------------------------------------------------------ */
/* ================================================================== */

static void vid_tx(uint8_t b)
{
    uart_putc_raw(VID_UART_ID, b);
}

static uint8_t vid_rx(void)
{
    return uart_getc(VID_UART_ID);
}

static void vid_tx16(uint16_t v)
{
    vid_tx((uint8_t)(v & 0xFF));
    vid_tx((uint8_t)(v >> 8));
}

/* Wait for ACK. Returns true on ACK, false on NAK or timeout. */
static bool vid_wait_ack(void)
{
    uint8_t r = vid_rx();
    return r == PROTO_ACK;
}

/*
 * gfx_set_mode(mode)
 * Switch the video core to a display mode.
 * mode: one of the MODE_* constants from shared_protocol.h
 *
 * Example:
 *   gfx_set_mode(MODE_320x240_8BIT);
 */
bool gfx_set_mode(uint8_t mode)
{
    vid_tx(CMD_SET_MODE);
    vid_tx(mode);
    return vid_wait_ack();
}

/*
 * gfx_set_color(r, g, b)
 * Set the foreground drawing colour (0–255 per channel).
 * In 8-bit mode colours are quantised to RGB332.
 * In 16-bit mode colours are quantised to RGB565.
 *
 * Example:
 *   gfx_set_color(255, 0, 0);   // red
 */
bool gfx_set_color(uint8_t r, uint8_t g, uint8_t b)
{
    vid_tx(CMD_SET_COLOR);
    vid_tx(r); vid_tx(g); vid_tx(b);
    return vid_wait_ack();
}

/*
 * gfx_pixel(x, y)
 * Draw one pixel at (x,y) in the current foreground colour.
 *
 * Example:
 *   gfx_set_color(0, 255, 0);
 *   gfx_pixel(160, 120);
 */
bool gfx_pixel(int x, int y)
{
    vid_tx(CMD_WRITE_PIXEL);
    vid_tx16((uint16_t)x);
    vid_tx16((uint16_t)y);
    return vid_wait_ack();
}

/*
 * gfx_line(x0, y0, x1, y1)
 * Draw a line.
 *
 * Example:
 *   gfx_line(0, 0, 319, 239);
 */
bool gfx_line(int x0, int y0, int x1, int y1)
{
    vid_tx(CMD_WRITE_LINE);
    vid_tx16((uint16_t)x0); vid_tx16((uint16_t)y0);
    vid_tx16((uint16_t)x1); vid_tx16((uint16_t)y1);
    return vid_wait_ack();
}

/*
 * gfx_fill_rect(x, y, w, h)
 * Fill a rectangle.
 *
 * Example:
 *   gfx_set_color(0, 0, 255);
 *   gfx_fill_rect(10, 10, 100, 80);
 */
bool gfx_fill_rect(int x, int y, int w, int h)
{
    vid_tx(CMD_FILL_RECT);
    vid_tx16((uint16_t)x); vid_tx16((uint16_t)y);
    vid_tx16((uint16_t)w); vid_tx16((uint16_t)h);
    return vid_wait_ack();
}

/*
 * gfx_clear(r, g, b)
 * Clear the entire screen to a colour.
 *
 * Example:
 *   gfx_clear(0, 0, 0);   // black
 */
bool gfx_clear(uint8_t r, uint8_t g, uint8_t b)
{
    gfx_set_color(r, g, b);
    vid_tx(CMD_CLEAR_SCREEN);
    return vid_wait_ack();
}

/*
 * gfx_swap()
 * In double-buffer modes, flip the display and draw buffers.
 * Call once per frame after all drawing is done.
 */
bool gfx_swap(void)
{
    vid_tx(CMD_SWAP_BUFFER);
    return vid_wait_ack();
}

/*
 * gfx_char(x, y, ch)
 * Draw one ASCII character using the built-in 8x8 font.
 *
 * Example:
 *   gfx_set_color(255,255,255);
 *   gfx_char(10, 10, 'A');
 */
bool gfx_char(int x, int y, char ch)
{
    vid_tx(CMD_DRAW_CHAR);
    vid_tx16((uint16_t)x); vid_tx16((uint16_t)y);
    vid_tx((uint8_t)ch);
    return vid_wait_ack();
}

/*
 * gfx_text(x, y, str)
 * Draw a null-terminated string.
 *
 * Example:
 *   gfx_set_color(255,255,0);
 *   gfx_text(10, 10, "Hello World");
 */
bool gfx_text(int x, int y, const char *str)
{
    uint8_t len = (uint8_t)strlen(str);
    if (len > 255) len = 255;
    vid_tx(CMD_DRAW_TEXT);
    vid_tx16((uint16_t)x); vid_tx16((uint16_t)y);
    vid_tx(len);
    for (uint8_t i = 0; i < len; i++) vid_tx((uint8_t)str[i]);
    return vid_wait_ack();
}

/*
 * gfx_blit(x, y, w, h, pixels)
 * Copy a raw pixel array to the screen.
 * pixels: uint8_t* for 8-bit modes, uint16_t* for 16-bit modes
 *
 * Example (8-bit):
 *   uint8_t sprite[16*16];
 *   // fill sprite data ...
 *   gfx_blit8(64, 64, 16, 16, sprite);
 */
bool gfx_blit8(int x, int y, int w, int h, const uint8_t *pixels)
{
    vid_tx(CMD_BLIT);
    vid_tx16((uint16_t)x); vid_tx16((uint16_t)y);
    vid_tx16((uint16_t)w); vid_tx16((uint16_t)h);
    int total = w * h;
    for (int i = 0; i < total; i++) vid_tx(pixels[i]);
    return vid_wait_ack();
}

bool gfx_blit16(int x, int y, int w, int h, const uint16_t *pixels)
{
    vid_tx(CMD_BLIT);
    vid_tx16((uint16_t)x); vid_tx16((uint16_t)y);
    vid_tx16((uint16_t)w); vid_tx16((uint16_t)h);
    int total = w * h;
    for (int i = 0; i < total; i++) {
        vid_tx((uint8_t)(pixels[i] & 0xFF));
        vid_tx((uint8_t)(pixels[i] >> 8));
    }
    return vid_wait_ack();
}

bool gfx_ping(void)
{
    vid_tx(CMD_PING);
    return vid_wait_ack();
}

/* ================================================================== */
/* ------------------------------------------------------------------ */
/* SECTION 2 — ETHERNET API (USR-K6 UART-ETH bridge)                  */
/* ------------------------------------------------------------------ */
/* ================================================================== */
/*
 * The USR-K6 is a UART-to-Ethernet module. Once configured it acts as
 * a transparent serial tunnel. TCP data sent to the module's IP:port
 * arrives on ETH_UART_ID, and bytes written to ETH_UART_ID are sent
 * over TCP.
 *
 * Configuration of the module (IP, port, mode) is done via AT commands
 * over the same UART. Here we provide a minimal TCP client interface.
 */

static void eth_tx(uint8_t b)    { uart_putc_raw(ETH_UART_ID, b); }
static bool eth_rx_ready(void)   { return uart_is_readable(ETH_UART_ID); }
static uint8_t eth_rx(void)      { return uart_getc(ETH_UART_ID); }

static void eth_tx_str(const char *s)
{
    while (*s) eth_tx((uint8_t)*s++);
}

/*
 * eth_send(data, len)
 * Send len bytes over the Ethernet TCP connection.
 *
 * Example:
 *   const char *msg = "GET / HTTP/1.0\r\n\r\n";
 *   eth_send((const uint8_t *)msg, strlen(msg));
 */
void eth_send(const uint8_t *data, uint16_t len)
{
    for (uint16_t i = 0; i < len; i++) eth_tx(data[i]);
}

/*
 * eth_recv(buf, maxlen, timeout_ms)
 * Receive up to maxlen bytes within timeout_ms milliseconds.
 * Returns number of bytes received.
 *
 * Example:
 *   uint8_t buf[256];
 *   int n = eth_recv(buf, 256, 1000);
 */
int eth_recv(uint8_t *buf, int maxlen, uint32_t timeout_ms)
{
    int n = 0;
    absolute_time_t deadline = make_timeout_time_ms(timeout_ms);
    while (n < maxlen && !time_reached(deadline)) {
        if (eth_rx_ready()) buf[n++] = eth_rx();
        else tight_loop_contents();
    }
    return n;
}

/*
 * eth_connect(ip, port)
 * Send AT commands to USR-K6 to open a TCP connection.
 * ip: string like "192.168.1.100"
 * port: port number
 * Returns true if the module acknowledges.
 *
 * Example:
 *   eth_connect("192.168.1.100", 80);
 */
bool eth_connect(const char *ip, uint16_t port)
{
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "AT+SOCKAEN=on\r\n");
    eth_tx_str(cmd);
    sleep_ms(100);
    snprintf(cmd, sizeof(cmd), "AT+SOCKTP=TCP\r\n");
    eth_tx_str(cmd);
    sleep_ms(100);
    snprintf(cmd, sizeof(cmd), "AT+SOCKRA=%s\r\n", ip);
    eth_tx_str(cmd);
    sleep_ms(100);
    snprintf(cmd, sizeof(cmd), "AT+SOCKRP=%u\r\n", port);
    eth_tx_str(cmd);
    sleep_ms(100);
    snprintf(cmd, sizeof(cmd), "AT+SOCKSEND=1\r\n");
    eth_tx_str(cmd);
    sleep_ms(200);
    /* Check for OK */
    uint8_t resp[32];
    int n = eth_recv(resp, 31, 500);
    resp[n] = '\0';
    return (strstr((char *)resp, "OK") != NULL);
}

/* ================================================================== */
/* ------------------------------------------------------------------ */
/* SECTION 3 — USB HOST API (TinyUSB)                                  */
/* ------------------------------------------------------------------ */
/* ================================================================== */
/*
 * USB host uses TinyUSB. GPIO24/25 are the RP2350's USB DP/DM pins.
 * tusb_init() must be called once at startup.
 * tuh_task() must be called repeatedly in the main loop.
 *
 * The callbacks below are called by TinyUSB when devices connect/disconnect.
 * HID keyboard input is captured here as an example.
 */

/* Last key received from USB HID keyboard */
static volatile uint8_t usb_last_key = 0;
static volatile bool    usb_key_ready = false;
static volatile bool    usb_device_connected = false;

/* TinyUSB mount/unmount callbacks */
void tuh_mount_cb(uint8_t dev_addr)
{
    (void)dev_addr;
    usb_device_connected = true;
}

void tuh_umount_cb(uint8_t dev_addr)
{
    (void)dev_addr;
    usb_device_connected = false;
}

/* HID report callback */
void tuh_hid_report_received_cb(uint8_t dev_addr, uint8_t instance,
                                  const uint8_t *report, uint16_t len)
{
    (void)dev_addr; (void)instance;
    if (len > 2 && report[2] != 0) {
        usb_last_key  = report[2];   /* keycode */
        usb_key_ready = true;
    }
}

/*
 * usb_key_available()
 * Returns true if a new key has been pressed on a connected USB keyboard.
 */
bool usb_key_available(void)    { return usb_key_ready; }

/*
 * usb_get_key()
 * Returns the HID keycode of the last pressed key and clears the flag.
 * HID keycodes: 0x04=A, 0x05=B ... 0x27=0, etc.
 *
 * Example:
 *   if (usb_key_available()) {
 *       uint8_t k = usb_get_key();
 *   }
 */
uint8_t usb_get_key(void)
{
    usb_key_ready = false;
    return usb_last_key;
}

/*
 * usb_device_present()
 * Returns true if a USB device is currently connected.
 */
bool usb_device_present(void)   { return usb_device_connected; }

/* ================================================================== */
/* ------------------------------------------------------------------ */
/* SECTION 4 — EXAMPLE PROGRAMS                                        */
/* ------------------------------------------------------------------ */
/* ================================================================== */

/* ---- Example A: MODE_320x240_8BIT @ 60fps ----------------------- */
/*
 * Draws a moving colour gradient and bouncing ball at 60fps.
 * Uses a single framebuffer — no tearing protection.
 */
static void example_320x240_8bit(void)
{
    gfx_set_mode(MODE_320x240_8BIT);
    sleep_ms(50);

    int bx = 80, by = 60;       /* ball position */
    int vx = 2,  vy = 1;        /* ball velocity */
    int frame = 0;

    while (true) {
        /* background gradient — hue shifts each frame */
        for (int y = 0; y < 240; y++) {
            uint8_t r = (uint8_t)((y + frame) & 0xFF);
            uint8_t g = (uint8_t)((y * 2 + frame * 3) & 0xFF);
            uint8_t b = (uint8_t)(frame & 0xFF);
            gfx_set_color(r, g, b);
            gfx_line(0, y, 319, y);
        }

        /* bounce ball */
        bx += vx; by += vy;
        if (bx <= 5  || bx >= 314) vx = -vx;
        if (by <= 5  || by >= 234) vy = -vy;

        gfx_set_color(255, 255, 255);
        gfx_fill_rect(bx - 5, by - 5, 10, 10);

        /* frame counter text */
        gfx_set_color(255, 255, 0);
        char buf[24];
        snprintf(buf, sizeof(buf), "Frame %d", frame);
        gfx_text(4, 4, buf);

        frame++;
        /* ~60fps pacing — UART latency from drawing provides natural rate */
    }
}

/* ---- Example B: MODE_640x480_8BIT @ 60fps ----------------------- */
/*
 * Draws a grid and diagonal lines across the full 640×480 canvas.
 */
static void example_640x480_8bit(void)
{
    gfx_set_mode(MODE_640x480_8BIT);
    sleep_ms(50);

    gfx_clear(0, 0, 0);

    /* grid */
    for (int x = 0; x < 640; x += 32) {
        gfx_set_color(0, 60, 0);
        gfx_line(x, 0, x, 479);
    }
    for (int y = 0; y < 480; y += 32) {
        gfx_set_color(0, 60, 0);
        gfx_line(0, y, 639, y);
    }

    /* diagonal cross */
    gfx_set_color(255, 0, 0);
    gfx_line(0, 0, 639, 479);
    gfx_set_color(0, 0, 255);
    gfx_line(639, 0, 0, 479);

    /* label */
    gfx_set_color(255, 255, 255);
    gfx_text(240, 230, "640x480 8bit RGB332");

    int frame = 0;
    while (true) {
        /* animated corner boxes */
        int off = frame & 0x1F;
        gfx_set_color(255, 128, 0);
        gfx_fill_rect(off,       off,       20, 20);
        gfx_fill_rect(619 - off, off,       20, 20);
        gfx_fill_rect(off,       459 - off, 20, 20);
        gfx_fill_rect(619 - off, 459 - off, 20, 20);
        frame++;
        sleep_ms(16);   /* ~60fps */
    }
}

/* ---- Example C: MODE_320x240_DB8 double-buf 8bit @30fps ---------- */
/*
 * Draws a spinning colour wheel into the back buffer.
 * Calls gfx_swap() to flip — no screen tearing.
 */
static void example_db_8bit(void)
{
    gfx_set_mode(MODE_320x240_DB8);
    sleep_ms(50);

    int angle = 0;

    while (true) {
        /* draw into back buffer */
        gfx_clear(0, 0, 20);

        /* draw 8 spokes */
        for (int i = 0; i < 8; i++) {
            int a = angle + i * 45;
            /* approximate sin/cos with lookup */
            static const int8_t cos8[36] = {
                10,9,8,6,4,2,0,-2,-4,-6,-8,-9,
                -10,-9,-8,-6,-4,-2,0,2,4,6,8,9,
                10,9,8,6,4,2,0,-2,-4,-6,-8,-9};
            static const int8_t sin8[36] = {
                0,2,4,6,8,9,10,9,8,6,4,2,
                0,-2,-4,-6,-8,-9,-10,-9,-8,-6,-4,-2,
                0,2,4,6,8,9,10,9,8,6,4,2};
            int ai = (a / 10) % 36;
            int x1 = 160 + (int)cos8[ai] * 10;
            int y1 = 120 + (int)sin8[ai] * 10;
            uint8_t r = (uint8_t)(i * 32);
            uint8_t g = (uint8_t)(255 - i * 32);
            gfx_set_color(r, g, 128);
            gfx_line(160, 120, x1, y1);
        }

        gfx_set_color(255, 255, 255);
        gfx_text(60, 110, "320x240 DB 8bit @30fps");

        /* swap — atomically flips at vblank */
        gfx_swap();
        angle = (angle + 5) % 360;
        sleep_ms(33);   /* ~30fps */
    }
}

/* ---- Example D: MODE_320x240_DB16 double-buf 16bit @30fps -------- */
/*
 * Draws a smooth RGB gradient that cycles colours.
 * 16-bit RGB565 gives noticeably smoother colour transitions than 8-bit.
 */
static void example_db_16bit(void)
{
    gfx_set_mode(MODE_320x240_DB16);
    sleep_ms(50);

    int phase = 0;

    while (true) {
        /* draw smooth gradient into back buffer */
        for (int y = 0; y < 240; y++) {
            for (int x = 0; x < 320; x += 8) {
                /* RGB565 smooth gradient */
                uint8_t r = (uint8_t)((x + phase) & 0xFF);
                uint8_t g = (uint8_t)((y + phase * 2) & 0xFF);
                uint8_t b = (uint8_t)((x + y + phase * 3) & 0xFF);
                gfx_set_color(r, g, b);
                /* draw 8-pixel wide block for speed */
                gfx_fill_rect(x, y, 8, 1);
            }
        }

        gfx_set_color(255, 255, 255);
        gfx_text(64, 112, "320x240 DB 16bit @30fps");

        gfx_swap();
        phase = (phase + 3) & 0xFF;
        sleep_ms(33);
    }
}

/* ================================================================== */
/* ------------------------------------------------------------------ */
/* SECTION 5 — YOUR PROGRAM                                            */
/* ------------------------------------------------------------------ */
/* ================================================================== */
/*
 * user_main() is called after all hardware is initialised.
 *
 * You have access to:
 *   gfx_*()      — graphics (see Section 1 above)
 *   eth_*()      — ethernet TCP (see Section 2)
 *   usb_*()      — USB host keyboard (see Section 3)
 *   sleep_ms()   — delay in milliseconds
 *   tuh_task()   — call this regularly in your loop for USB events
 *
 * Uncomment the example you want to run, or write your own below.
 */
static void user_main(void)
{
    /* --- choose one example to run, or write your own --- */

    /* example_320x240_8bit();    */  /* Mode 0x01: 320x240 60fps 8bit  */
    /* example_640x480_8bit();    */  /* Mode 0x02: 640x480 60fps 8bit  */
    /* example_db_8bit();         */  /* Mode 0x03: 320x240 DB 8bit     */
    example_db_16bit();               /* Mode 0x04: 320x240 DB 16bit    */

    /* ---- example of ethernet + graphics together ---- */
    /*
    gfx_set_mode(MODE_320x240_8BIT);
    gfx_clear(0, 0, 0);
    gfx_set_color(255, 255, 255);
    gfx_text(8, 8, "Connecting...");

    if (eth_connect("192.168.1.100", 80)) {
        gfx_clear(0, 40, 0);
        gfx_text(8, 8, "Connected!");
        const char *req = "GET / HTTP/1.0\r\nHost: 192.168.1.100\r\n\r\n";
        eth_send((const uint8_t *)req, strlen(req));
        uint8_t resp[256];
        int n = eth_recv(resp, 255, 2000);
        resp[n] = '\0';
        gfx_text(8, 24, (char *)resp);
    } else {
        gfx_clear(80, 0, 0);
        gfx_text(8, 8, "ETH FAILED");
    }
    while (true) sleep_ms(1000);
    */

    /* ---- example of USB keyboard input ---- */
    /*
    gfx_set_mode(MODE_320x240_8BIT);
    gfx_clear(0, 0, 0);
    gfx_set_color(255,255,255);
    gfx_text(8, 8, "USB keyboard demo");
    int cy = 30;
    while (true) {
        tuh_task();
        if (usb_key_available()) {
            uint8_t k = usb_get_key();
            char buf[16];
            snprintf(buf, sizeof(buf), "Key: 0x%02X", k);
            gfx_set_color(0,0,0);
            gfx_fill_rect(8, cy, 200, 10);
            gfx_set_color(0,255,0);
            gfx_text(8, cy, buf);
            cy += 10;
            if (cy > 230) cy = 30;
        }
    }
    */
}

/* ================================================================== */
/* Main                                                                 */
/* ================================================================== */
int main(void)
{
    /* System clock — 252MHz for exact 640x480x60 pixel clock */
    set_sys_clock_khz(252000, true);

    stdio_init_all();

    /* UART to VIDEO core: GPIO38=TX, GPIO39=RX */
    uart_init(VID_UART_ID, PROTO_BAUD);
    gpio_set_function(VID_UART_TX, GPIO_FUNC_UART);
    gpio_set_function(VID_UART_RX, GPIO_FUNC_UART);
    uart_set_hw_flow(VID_UART_ID, false, false);
    uart_set_format(VID_UART_ID, 8, 1, UART_PARITY_NONE);

    /* UART to Ethernet module: GPIO3=TX, GPIO2=RX */
    uart_init(ETH_UART_ID, 115200);
    gpio_set_function(ETH_UART_TX, GPIO_FUNC_UART);
    gpio_set_function(ETH_UART_RX, GPIO_FUNC_UART);
    uart_set_hw_flow(ETH_UART_ID, false, false);

    /* USB host init — TinyUSB */
    tusb_init();

    /* Wait for VIDEO core to send initial ACK */
    {
        uint8_t r = vid_rx();
        (void)r;   /* consume startup ACK */
    }

    /* Ping VIDEO core */
    while (!gfx_ping()) sleep_ms(100);

    /* Run user program */
    user_main();

    /* Should never reach here */
    while (true) {
        tuh_task();
        sleep_ms(1);
    }
}
