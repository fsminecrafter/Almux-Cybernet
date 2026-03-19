# Makefile for ATmega328PB IO Manager
# Build: make
# Flash via AVRDUDE: make flash PORT=/dev/ttyUSB0

MCU     = atmega328pb
F_CPU   = 12000000UL
TARGET  = io_manager
CC      = avr-gcc
OBJCOPY = avr-objcopy
AVRDUDE = avrdude

CFLAGS  = -mmcu=$(MCU) -DF_CPU=$(F_CPU) -Os -Wall -Wextra \
          -std=c11 -ffunction-sections -fdata-sections

LDFLAGS = -mmcu=$(MCU) -Wl,--gc-sections

PORT   ?= /dev/ttyUSB0
BAUD   ?= 115200

all: $(TARGET).hex

$(TARGET).elf: $(TARGET).c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

$(TARGET).hex: $(TARGET).elf
	$(OBJCOPY) -O ihex -R .eeprom $< $@
	avr-size --format=avr --mcu=$(MCU) $<

flash: $(TARGET).hex
	$(AVRDUDE) -p $(MCU) -c arduino -P $(PORT) -b $(BAUD) -U flash:w:$<:i

fuses:
	# 12MHz external crystal, no divide, BOD 2.7V
	$(AVRDUDE) -p $(MCU) -c arduino -P $(PORT) -b $(BAUD) \
	  -U lfuse:w:0xFF:m \
	  -U hfuse:w:0xD9:m \
	  -U efuse:w:0xFD:m

clean:
	rm -f $(TARGET).elf $(TARGET).hex

.PHONY: all flash fuses clean
