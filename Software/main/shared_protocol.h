#pragma once
/*
 * shared_protocol.h
 * Command protocol between CORE MAIN (sender) and CORE VIDEO (receiver)
 * Communication via UART: MAIN GPIO38(TX) -> VIDEO UART RX
 *                         VIDEO UART TX   -> MAIN GPIO39(RX)
 * Baud rate: 921600
 * All multi-byte values are little-endian.
 *
 * Packet format:
 *   [CMD 1 byte] [payload bytes per command]
 *
 * Responses from VIDEO to MAIN:
 *   ACK  0xAA  - command accepted
 *   NAK  0xFF  - command rejected / busy
 *   DATA 0xDD  [len 2 bytes] [data bytes] - response data follows
 */

/* ------------------------------------------------------------------ */
/* Baud rate                                                            */
/* ------------------------------------------------------------------ */
#define PROTO_BAUD          921600

/* ------------------------------------------------------------------ */
/* Response bytes                                                       */
/* ------------------------------------------------------------------ */
#define PROTO_ACK           0xAA
#define PROTO_NAK           0xFF
#define PROTO_DATA          0xDD

/* ------------------------------------------------------------------ */
/* Display modes                                                        */
/* ------------------------------------------------------------------ */
#define MODE_320x240_8BIT   0x01   /* 320x240 @ 60fps RGB332        */
#define MODE_640x480_8BIT   0x02   /* 640x480 @ 60fps RGB332        */
#define MODE_320x240_DB8    0x03   /* 320x240 double-buf 8bit @30fps */
#define MODE_320x240_DB16   0x04   /* 320x240 double-buf 16bit @30fps*/

/* ------------------------------------------------------------------ */
/* Commands                                                             */
/* ------------------------------------------------------------------ */

/* CMD_SET_MODE  [mode 1 byte]
 * Switch display mode. VIDEO replies ACK when mode is active.      */
#define CMD_SET_MODE        0x01

/* CMD_SET_COLOR  [r 1][g 1][b 1]
 * Set current foreground colour for subsequent draw commands.      */
#define CMD_SET_COLOR       0x02

/* CMD_WRITE_PIXEL  [x 2][y 2]
 * Draw one pixel at (x,y) in current foreground colour.            */
#define CMD_WRITE_PIXEL     0x03

/* CMD_WRITE_LINE  [x0 2][y0 2][x1 2][y1 2]
 * Draw a line from (x0,y0) to (x1,y1) using Bresenham.            */
#define CMD_WRITE_LINE      0x04

/* CMD_FILL_RECT  [x 2][y 2][w 2][h 2]
 * Fill rectangle with current colour.                              */
#define CMD_FILL_RECT       0x05

/* CMD_CLEAR_SCREEN
 * Fill entire framebuffer with current colour.                     */
#define CMD_CLEAR_SCREEN    0x06

/* CMD_BLIT  [x 2][y 2][w 2][h 2] [pixel data: w*h bytes (8bit) or w*h*2 (16bit)]
 * Copy raw pixel block to framebuffer at (x,y).                    */
#define CMD_BLIT            0x07

/* CMD_SWAP_BUFFER
 * (Double-buffer modes only) flip front/back buffer.              */
#define CMD_SWAP_BUFFER     0x08

/* CMD_DRAW_CHAR  [x 2][y 2][ch 1]
 * Draw one ASCII character at (x,y) using built-in 8x8 font.      */
#define CMD_DRAW_CHAR       0x09

/* CMD_DRAW_TEXT  [x 2][y 2][len 1][chars len bytes]
 * Draw a string.                                                   */
#define CMD_DRAW_TEXT       0x0A

/* CMD_GET_MODE
 * Ask VIDEO which mode is active. VIDEO replies DATA [1 byte mode].*/
#define CMD_GET_MODE        0x0B

/* CMD_PING
 * Liveness check. VIDEO replies ACK.                               */
#define CMD_PING            0x0C
