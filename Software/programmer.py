#!/usr/bin/env python3
"""
IO Bridge programmer tool
Communicates with the ATmega328PB IO manager firmware.

Usage:
    python3 programmer.py --port /dev/ttyUSB0 --target MAIN --write firmware.bin
    python3 programmer.py --port /dev/ttyUSB0 --target VIDEO --write firmware.bin
    python3 programmer.py --port /dev/ttyUSB0 --target MAIN --read output.bin --addr 0x000000 --len 1048576
    python3 programmer.py --port /dev/ttyUSB0 --target MAIN --erase --addr 0x000000

Install dependency:
    pip3 install pyserial
"""

import serial
import argparse
import sys
import time
import struct
import os

BAUD = 115200
SECTOR_SIZE = 4096          # W25Q128 4KB sector
PAGE_SIZE   = 256           # W25Q128 page size
FLASH_SIZE  = 16 * 1024 * 1024   # W25Q128 = 16MB


def open_port(port: str) -> serial.Serial:
    s = serial.Serial(port, BAUD, timeout=3)
    time.sleep(0.1)
    s.reset_input_buffer()
    return s


def expect(s: serial.Serial, msg: str):
    resp = s.readline().decode("ascii", errors="replace").strip()
    if msg not in resp:
        print(f"ERROR: expected '{msg}', got '{resp}'")
        sys.exit(1)


def ping(s: serial.Serial):
    s.write(b'?')
    resp = s.readline().decode("ascii", errors="replace").strip()
    if "IOBRIDGE" not in resp:
        print(f"ERROR: device not recognised. Got: '{resp}'")
        print("Check PE2 is HIGH (bridge mode) and the correct COM port.")
        sys.exit(1)
    print(f"Connected: {resp}")


def select_target(s: serial.Serial, target: str):
    if target.upper() == "MAIN":
        s.write(b'M')
        expect(s, "MAIN OK")
        print("Target: CORE MAIN flash selected")
    elif target.upper() == "VIDEO":
        s.write(b'V')
        expect(s, "VIDEO OK")
        print("Target: CORE VIDEO flash selected")
    else:
        print(f"ERROR: unknown target '{target}'. Use MAIN or VIDEO.")
        sys.exit(1)


def idle(s: serial.Serial):
    s.write(b'X')
    expect(s, "IDLE")


def send_addr(s: serial.Serial, addr: int):
    s.write(struct.pack(">I", addr)[1:])   # 3 bytes big-endian


def send_len(s: serial.Serial, length: int):
    s.write(struct.pack(">H", length))     # 2 bytes big-endian


def read_flash(s: serial.Serial, addr: int, length: int, outfile: str):
    """Read length bytes from flash starting at addr, save to outfile."""
    print(f"Reading {length} bytes from 0x{addr:06X}...")

    # Read in 4KB chunks to show progress
    chunk_size = SECTOR_SIZE
    data = bytearray()
    remaining = length
    cur_addr  = addr

    while remaining > 0:
        chunk = min(chunk_size, remaining)
        s.write(b'R')
        send_addr(s, cur_addr)
        send_len(s, chunk)
        received = s.read(chunk)
        if len(received) != chunk:
            print(f"ERROR: expected {chunk} bytes, got {len(received)}")
            sys.exit(1)
        data.extend(received)
        cur_addr  += chunk
        remaining -= chunk
        pct = 100 * len(data) // length
        print(f"  {pct:3d}%  {len(data)}/{length} bytes", end="\r")

    print()
    with open(outfile, "wb") as f:
        f.write(data)
    print(f"Saved {len(data)} bytes to {outfile}")


def erase_range(s: serial.Serial, addr: int, length: int):
    """Erase all 4KB sectors covering addr..addr+length."""
    # align start down to sector boundary
    start_sector = (addr // SECTOR_SIZE) * SECTOR_SIZE
    end_addr     = addr + length
    cur          = start_sector
    sectors      = (end_addr - start_sector + SECTOR_SIZE - 1) // SECTOR_SIZE

    print(f"Erasing {sectors} sector(s) from 0x{start_sector:06X}...")
    done = 0
    while cur < end_addr:
        s.write(b'E')
        send_addr(s, cur)
        expect(s, "ERASED")
        cur  += SECTOR_SIZE
        done += 1
        pct = 100 * done // sectors
        print(f"  {pct:3d}%  sector 0x{cur - SECTOR_SIZE:06X} erased", end="\r")

    print()
    print("Erase complete.")


def write_flash(s: serial.Serial, addr: int, data: bytes):
    """Erase and write data to flash starting at addr."""
    length = len(data)

    # Erase required sectors first
    erase_range(s, addr, length)

    # Write in page-aligned chunks
    print(f"Writing {length} bytes to 0x{addr:06X}...")
    remaining = length
    cur_addr  = addr
    offset    = 0

    while remaining > 0:
        # how many bytes until end of current page
        page_offset = cur_addr & (PAGE_SIZE - 1)
        chunk = PAGE_SIZE - page_offset
        if chunk > remaining:
            chunk = remaining

        s.write(b'W')
        send_addr(s, cur_addr)
        send_len(s, chunk)
        s.write(data[offset:offset + chunk])
        expect(s, "DONE")

        cur_addr  += chunk
        offset    += chunk
        remaining -= chunk
        pct = 100 * offset // length
        print(f"  {pct:3d}%  {offset}/{length} bytes", end="\r")

    print()
    print("Write complete.")


def verify_flash(s: serial.Serial, addr: int, data: bytes) -> bool:
    """Read back and compare written data."""
    print("Verifying...")
    length = len(data)
    readback = bytearray()
    chunk_size = SECTOR_SIZE
    remaining = length
    cur_addr  = addr

    while remaining > 0:
        chunk = min(chunk_size, remaining)
        s.write(b'R')
        send_addr(s, cur_addr)
        send_len(s, chunk)
        received = s.read(chunk)
        if len(received) != chunk:
            print(f"ERROR: short read during verify")
            return False
        readback.extend(received)
        cur_addr  += chunk
        remaining -= chunk

    if bytes(readback) == data:
        print("Verify OK — flash matches input file.")
        return True
    else:
        # find first mismatch
        for i, (a, b) in enumerate(zip(readback, data)):
            if a != b:
                print(f"VERIFY FAILED at offset 0x{i:06X}: "
                      f"expected 0x{b:02X}, got 0x{a:02X}")
                break
        return False


def main():
    parser = argparse.ArgumentParser(
        description="IO Bridge flash programmer for CORE MAIN / CORE VIDEO")

    parser.add_argument("--port",   required=True,
                        help="Serial port e.g. /dev/ttyUSB0 or COM3")
    parser.add_argument("--target", required=True,
                        choices=["MAIN", "VIDEO"],
                        help="Which processor flash to program")
    parser.add_argument("--write",  metavar="FILE",
                        help="Binary file to write to flash")
    parser.add_argument("--read",   metavar="FILE",
                        help="Output file for flash readback")
    parser.add_argument("--erase",  action="store_true",
                        help="Erase sectors before write (always done with --write)")
    parser.add_argument("--addr",   type=lambda x: int(x, 0), default=0,
                        help="Flash start address (default 0x000000)")
    parser.add_argument("--len",    type=lambda x: int(x, 0),
                        help="Number of bytes to read (required with --read)")
    parser.add_argument("--no-verify", action="store_true",
                        help="Skip readback verify after write")

    args = parser.parse_args()

    if not args.write and not args.read and not args.erase:
        parser.error("Specify at least one of --write, --read, or --erase")

    if args.read and not args.len:
        parser.error("--read requires --len")

    print(f"Opening {args.port} at {BAUD} baud...")
    s = open_port(args.port)

    try:
        ping(s)
        select_target(s, args.target)

        if args.erase and not args.write:
            length = args.len if args.len else SECTOR_SIZE
            erase_range(s, args.addr, length)

        if args.write:
            with open(args.write, "rb") as f:
                data = f.read()
            print(f"Input file: {args.write}  ({len(data)} bytes)")
            write_flash(s, args.addr, data)
            if not args.no_verify:
                ok = verify_flash(s, args.addr, data)
                if not ok:
                    sys.exit(1)

        if args.read:
            read_flash(s, args.addr, args.len, args.read)

        idle(s)
        print("Done — bridge returned to IDLE.")

    finally:
        s.close()


if __name__ == "__main__":
    main()
