/*
 * IO Manager / UART-SPI Bridge
 * Target: ATmega328PB-AU @ 12MHz, 3.3V
 *
 * Pin mapping (from schematic):
 *   PD0 / RXD0    - UART RX  (from CH340N TXD)
 *   PD1 / TXD0    - UART TX  (to CH340N RXD)
 *   PB2 / CS0     - SPI CS MAIN   (K2 header pin 1)
 *   PB3 / MOSI    - SPI MOSI      (K2 header pin 2)
 *   PB4 / MISO    - SPI MISO      (K2 header pin 3)
 *   PB5 / CLK     - SPI SCK       (K2 header pin 4)
 *   PD5 / VIDEOSS - SPI CS VIDEO
 *   PE2 / TSENSE  - Mode select   (LOW=passthrough, HIGH=bridge)
 *   PC0-PC5       - UARTSENSE header H1
 *
 * Protocol (PE2 HIGH - bridge mode):
 *   Send '?' 0x3F  -> identity:  "IOBRIDGE v1\n"
 *   Send 'M' 0x4D  -> select MAIN flash:  "MAIN OK\n"
 *   Send 'V' 0x56  -> select VIDEO flash: "VIDEO OK\n"
 *   Send 'X' 0x58  -> deselect all:       "IDLE\n"
 *   Send 'R' 0x52  -> read:   next 3 bytes = addr, next 2 bytes = len
 *                             returns len bytes of flash data
 *   Send 'W' 0x57  -> write:  next 3 bytes = addr, next 2 bytes = len
 *                             then send len bytes of data
 *                             returns "DONE\n"
 *   Send 'E' 0x45  -> erase 4KB sector: next 3 bytes = addr
 *                             returns "ERASED\n"
 *
 * PE2 LOW - passthrough mode:
 *   All UART bytes forwarded transparently. Both CS lines held HIGH.
 */

#define F_CPU 12000000UL

#include <avr/io.h>
#include <avr/interrupt.h>
#include <util/delay.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Pin definitions                                                      */
/* ------------------------------------------------------------------ */
#define CS_MAIN_DDR   DDRB
#define CS_MAIN_PORT  PORTB
#define CS_MAIN_PIN   PB2

#define MOSI_DDR      DDRB
#define MOSI_PORT     PORTB
#define MOSI_PIN      PB3

#define MISO_DDR      DDRB
#define MISO_PORT     PORTB
#define MISO_PIN      PB4

#define SCK_DDR       DDRB
#define SCK_PORT      PORTB
#define SCK_PIN       PB5

#define CS_VIDEO_DDR  DDRD
#define CS_VIDEO_PORT PORTD
#define CS_VIDEO_PIN  PD5

#define MODE_DDR      DDRE
#define MODE_PORT     PORTE
#define MODE_PINREG   PINE
#define MODE_PIN      PE2          /* PE2 / TSENSE - mode select */

/* ------------------------------------------------------------------ */
/* UART baud rate: 115200 @ 12MHz                                       */
/* UBRR = (F_CPU / (16 * BAUD)) - 1                                    */
/* ------------------------------------------------------------------ */
#define BAUD          115200UL
#define UBRR_VAL      ((F_CPU / (16UL * BAUD)) - 1)  /* = 5 */

/* ------------------------------------------------------------------ */
/* W25Q128 flash commands                                               */
/* ------------------------------------------------------------------ */
#define FLASH_CMD_WRITE_ENABLE   0x06
#define FLASH_CMD_PAGE_PROGRAM   0x02
#define FLASH_CMD_READ_DATA      0x03
#define FLASH_CMD_SECTOR_ERASE   0x20   /* 4KB erase */
#define FLASH_CMD_READ_STATUS    0x05
#define FLASH_CMD_JEDEC_ID       0x9F

#define FLASH_STATUS_BUSY        0x01

/* SPI page size */
#define FLASH_PAGE_SIZE          256

/* ------------------------------------------------------------------ */
/* State                                                                */
/* ------------------------------------------------------------------ */
typedef enum {
    TARGET_NONE  = 0,
    TARGET_MAIN  = 1,
    TARGET_VIDEO = 2
} target_t;

static target_t current_target = TARGET_NONE;

/* ------------------------------------------------------------------ */
/* UART                                                                  */
/* ------------------------------------------------------------------ */
static void uart_init(void)
{
    UBRR0H = (uint8_t)(UBRR_VAL >> 8);
    UBRR0L = (uint8_t)(UBRR_VAL);
    UCSR0B = (1 << RXEN0) | (1 << TXEN0);
    UCSR0C = (1 << UCSZ01) | (1 << UCSZ00);   /* 8N1 */
}

static void uart_tx_byte(uint8_t b)
{
    while (!(UCSR0A & (1 << UDRE0)));
    UDR0 = b;
}

static uint8_t uart_rx_byte(void)
{
    while (!(UCSR0A & (1 << RXC0)));
    return UDR0;
}

static void uart_tx_str(const char *s)
{
    while (*s) uart_tx_byte((uint8_t)*s++);
}

static void uart_tx_bytes(const uint8_t *buf, uint16_t len)
{
    for (uint16_t i = 0; i < len; i++)
        uart_tx_byte(buf[i]);
}

/* ------------------------------------------------------------------ */
/* SPI                                                                   */
/* ------------------------------------------------------------------ */
static void spi_init(void)
{
    /* SCK, MOSI, CS_MAIN as outputs */
    SCK_DDR   |= (1 << SCK_PIN);
    MOSI_DDR  |= (1 << MOSI_PIN);
    CS_MAIN_DDR |= (1 << CS_MAIN_PIN);
    CS_VIDEO_DDR |= (1 << CS_VIDEO_PIN);

    /* MISO as input with pull-up */
    MISO_DDR  &= ~(1 << MISO_PIN);
    MISO_PORT |=  (1 << MISO_PIN);

    /* Deassert both CS lines */
    CS_MAIN_PORT  |= (1 << CS_MAIN_PIN);
    CS_VIDEO_PORT |= (1 << CS_VIDEO_PIN);

    /*
     * SPI master, mode 0 (CPOL=0 CPHA=0), FOSC/4 = 3MHz
     * W25Q128 supports up to 104MHz so 3MHz is well within spec.
     * Use FOSC/2 (6MHz) for faster programming:
     *   set SPI2X bit in SPSR0
     */
    SPCR0 = (1 << SPE0) | (1 << MSTR0);   /* enable, master, mode0, FOSC/4 */
    SPSR0 |= (1 << SPI2X0);               /* double speed -> FOSC/2 = 6MHz */
}

static uint8_t spi_transfer(uint8_t data)
{
    SPDR0 = data;
    while (!(SPSR0 & (1 << SPIF0)));
    return SPDR0;
}

static void cs_assert(target_t t)
{
    /* always deassert both first */
    CS_MAIN_PORT  |= (1 << CS_MAIN_PIN);
    CS_VIDEO_PORT |= (1 << CS_VIDEO_PIN);

    if (t == TARGET_MAIN)
        CS_MAIN_PORT  &= ~(1 << CS_MAIN_PIN);
    else if (t == TARGET_VIDEO)
        CS_VIDEO_PORT &= ~(1 << CS_VIDEO_PIN);
}

static void cs_deassert_all(void)
{
    CS_MAIN_PORT  |= (1 << CS_MAIN_PIN);
    CS_VIDEO_PORT |= (1 << CS_VIDEO_PIN);
}

/* ------------------------------------------------------------------ */
/* Flash operations                                                      */
/* ------------------------------------------------------------------ */
static void flash_wait_ready(void)
{
    uint8_t status;
    cs_assert(current_target);
    spi_transfer(FLASH_CMD_READ_STATUS);
    do {
        status = spi_transfer(0x00);
    } while (status & FLASH_STATUS_BUSY);
    cs_deassert_all();
}

static void flash_write_enable(void)
{
    cs_assert(current_target);
    spi_transfer(FLASH_CMD_WRITE_ENABLE);
    cs_deassert_all();
}

static void flash_sector_erase(uint32_t addr)
{
    flash_write_enable();
    cs_assert(current_target);
    spi_transfer(FLASH_CMD_SECTOR_ERASE);
    spi_transfer((uint8_t)(addr >> 16));
    spi_transfer((uint8_t)(addr >> 8));
    spi_transfer((uint8_t)(addr));
    cs_deassert_all();
    flash_wait_ready();
}

static void flash_read(uint32_t addr, uint16_t len)
{
    cs_assert(current_target);
    spi_transfer(FLASH_CMD_READ_DATA);
    spi_transfer((uint8_t)(addr >> 16));
    spi_transfer((uint8_t)(addr >> 8));
    spi_transfer((uint8_t)(addr));
    for (uint16_t i = 0; i < len; i++)
        uart_tx_byte(spi_transfer(0x00));
    cs_deassert_all();
}

static void flash_write_page(uint32_t addr, uint8_t *buf, uint16_t len)
{
    /* len must be <= 256 and not cross a page boundary */
    flash_write_enable();
    cs_assert(current_target);
    spi_transfer(FLASH_CMD_PAGE_PROGRAM);
    spi_transfer((uint8_t)(addr >> 16));
    spi_transfer((uint8_t)(addr >> 8));
    spi_transfer((uint8_t)(addr));
    for (uint16_t i = 0; i < len; i++)
        spi_transfer(buf[i]);
    cs_deassert_all();
    flash_wait_ready();
}

/* ------------------------------------------------------------------ */
/* Command handlers                                                      */
/* ------------------------------------------------------------------ */

/* Read 3-byte address from UART, returns as uint32_t */
static uint32_t recv_addr(void)
{
    uint32_t addr = 0;
    addr  = (uint32_t)uart_rx_byte() << 16;
    addr |= (uint32_t)uart_rx_byte() << 8;
    addr |= (uint32_t)uart_rx_byte();
    return addr;
}

/* Read 2-byte length from UART */
static uint16_t recv_len(void)
{
    uint16_t len = 0;
    len  = (uint16_t)uart_rx_byte() << 8;
    len |= (uint16_t)uart_rx_byte();
    return len;
}

static void cmd_read(void)
{
    if (current_target == TARGET_NONE) {
        uart_tx_str("ERR NO TARGET\n");
        return;
    }
    uint32_t addr = recv_addr();
    uint16_t len  = recv_len();
    flash_read(addr, len);           /* bytes streamed directly to UART */
}

static void cmd_write(void)
{
    if (current_target == TARGET_NONE) {
        uart_tx_str("ERR NO TARGET\n");
        return;
    }

    uint32_t addr = recv_addr();
    uint16_t len  = recv_len();

    uint8_t  page_buf[FLASH_PAGE_SIZE];
    uint32_t cur_addr = addr;
    uint16_t remaining = len;

    while (remaining > 0) {
        /* how many bytes fit in this page without crossing boundary */
        uint16_t page_offset = (uint16_t)(cur_addr & (FLASH_PAGE_SIZE - 1));
        uint16_t chunk = FLASH_PAGE_SIZE - page_offset;
        if (chunk > remaining) chunk = remaining;

        /* receive chunk bytes from UART */
        for (uint16_t i = 0; i < chunk; i++)
            page_buf[i] = uart_rx_byte();

        flash_write_page(cur_addr, page_buf, chunk);

        cur_addr  += chunk;
        remaining -= chunk;
    }

    uart_tx_str("DONE\n");
}

static void cmd_erase(void)
{
    if (current_target == TARGET_NONE) {
        uart_tx_str("ERR NO TARGET\n");
        return;
    }
    uint32_t addr = recv_addr();
    flash_sector_erase(addr);
    uart_tx_str("ERASED\n");
}

/* ------------------------------------------------------------------ */
/* Mode check                                                            */
/* ------------------------------------------------------------------ */
static inline bool bridge_mode(void)
{
    return (MODE_PINREG & (1 << MODE_PIN)) != 0;
}

/* ------------------------------------------------------------------ */
/* Main                                                                  */
/* ------------------------------------------------------------------ */
int main(void)
{
    /* PE2 as input, no internal pull-up (10k pull-down on schematic) */
    MODE_DDR  &= ~(1 << MODE_PIN);
    MODE_PORT &= ~(1 << MODE_PIN);

    uart_init();
    spi_init();
    sei();

    uart_tx_str("IOBRIDGE v1\n");

    while (1) {

        if (!bridge_mode()) {
            /*
             * Passthrough mode — PE2 LOW
             * Forward any received byte straight back out.
             * CS lines are deasserted.
             * current_target is reset to NONE.
             */
            cs_deassert_all();
            current_target = TARGET_NONE;

            if (UCSR0A & (1 << RXC0)) {
                uint8_t b = UDR0;
                uart_tx_byte(b);
            }
            continue;
        }

        /*
         * Bridge mode — PE2 HIGH
         * Wait for a command byte from the PC.
         */
        uint8_t cmd = uart_rx_byte();

        switch (cmd) {

            case '?':   /* identity */
                uart_tx_str("IOBRIDGE v1\n");
                break;

            case 'M':   /* select MAIN flash */
                current_target = TARGET_MAIN;
                uart_tx_str("MAIN OK\n");
                break;

            case 'V':   /* select VIDEO flash */
                current_target = TARGET_VIDEO;
                uart_tx_str("VIDEO OK\n");
                break;

            case 'X':   /* deselect / idle */
                cs_deassert_all();
                current_target = TARGET_NONE;
                uart_tx_str("IDLE\n");
                break;

            case 'R':   /* read from selected flash */
                cmd_read();
                break;

            case 'W':   /* write to selected flash */
                cmd_write();
                break;

            case 'E':   /* erase 4KB sector */
                cmd_erase();
                break;

            default:
                uart_tx_str("ERR CMD\n");
                break;
        }
    }

    return 0;   /* never reached */
}
