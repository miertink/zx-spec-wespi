#!/usr/bin/env python3
"""Builds small ZX Spectrum .tap test files, and checks existing ones.

  python tools/make_tap.py hello  out.tap   -> BASIC "Hello World" with autostart
  python tools/make_tap.py check  file.tap  -> lists blocks and validates checksums

The BASIC program is written already tokenised, so no emulator is needed:
    10 PRINT "HELLO WORLD"
    20 PRINT "ZX-WESPI SPECTRUM"
It uses no numeric literals, so the 5-byte hidden number format is not needed.
"""
import struct
import sys

TOKEN_PRINT = 0xF5


def basic_line(number: int, body: bytes) -> bytes:
    body = body + b"\x0d"                       # ENTER ends every line
    return struct.pack(">H", number) + struct.pack("<H", len(body)) + body


def print_line(number: int, text: str) -> bytes:
    return basic_line(number, bytes([TOKEN_PRINT]) + b'"' + text.encode("ascii") + b'"')


def tap_block(flag: int, payload: bytes) -> bytes:
    data = bytes([flag]) + payload
    checksum = 0
    for b in data:
        checksum ^= b
    data += bytes([checksum])
    return struct.pack("<H", len(data)) + data


def program_tap(name: str, program: bytes, autostart: int) -> bytes:
    header = struct.pack(
        "<B10sHHH",
        0,                                      # type 0 = Program
        name.encode("ascii").ljust(10)[:10],
        len(program),                           # data length
        autostart,                              # param1: autostart line (>= 32768 = none)
        len(program),                           # param2: start of variables = program length
    )
    return tap_block(0x00, header) + tap_block(0xFF, program)


def check(path: str) -> bool:
    data = open(path, "rb").read()
    pos, index, ok = 0, 0, True
    while pos < len(data):
        if pos + 2 > len(data):
            print(f"  truncated length field at offset {pos}")
            return False
        (length,) = struct.unpack_from("<H", data, pos)
        block = data[pos + 2:pos + 2 + length]
        if len(block) != length:
            print(f"  block {index}: truncated ({len(block)} of {length} bytes)")
            return False
        x = 0
        for b in block:
            x ^= b
        status = "ok" if x == 0 else "CHECKSUM ERROR"
        ok &= x == 0
        if block[0] == 0 and length == 19:
            typ, name, dlen, p1, p2 = struct.unpack_from("<B10sHHH", block, 1)
            print(f"  block {index}: header type {typ} \"{name.decode('ascii', 'replace')}\" "
                  f"len {dlen} param1 {p1} param2 {p2} - {status}")
        else:
            print(f"  block {index}: flag 0x{block[0]:02X}, {length} bytes - {status}")
        pos += 2 + length
        index += 1
    return ok


def main() -> int:
    if len(sys.argv) != 3 or sys.argv[1] not in ("hello", "check"):
        print(__doc__)
        return 2
    cmd, path = sys.argv[1], sys.argv[2]
    if cmd == "hello":
        program = print_line(10, "HELLO WORLD") + print_line(20, "ZX-WESPI SPECTRUM")
        with open(path, "wb") as f:
            f.write(program_tap("hello", program, autostart=10))
        print(f"wrote {path}")
    return 0 if check(path) else 1


if __name__ == "__main__":
    sys.exit(main())
